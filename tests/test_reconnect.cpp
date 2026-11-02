// 断流与重连退避单测：Backoff 纯逻辑 + TcpStreamClient 对真实回环端口。
//
// 不绑固定端口：监听端用 port=0 让系统分配，避免与串流端或 CI 上其它进程冲突。

#include "alxr_packet.hpp"
#include "alxr_transport.hpp"

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/udp.hpp" // WinsockRuntime + NowNs

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kSecondNs = 1000000000ull;

// 最小监听端：bind 到临时端口，accept 后按需收发。
class FakeStreamServer {
public:
    ~FakeStreamServer() { Close(); }

    bool Open(std::string& error) {
        if (!fgb::kit::WinsockRuntime::Acquire(error)) {
            return false;
        }
        acquired_ = true;
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) {
            error = "socket 失败";
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0; // 系统分配空闲端口
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            error = "bind 失败";
            return false;
        }
        int name_len = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &name_len);
        port_ = ntohs(address.sin_port);
        if (::listen(listener_, 2) != 0) {
            error = "listen 失败";
            return false;
        }
        return true;
    }

    // 非阻塞轮询 accept；成功返回 true。
    bool AcceptOnce() {
        fd_set read_set{};
        FD_ZERO(&read_set);
        FD_SET(listener_, &read_set);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 50 * 1000;
        if (::select(0, &read_set, nullptr, nullptr, &timeout) != 1) {
            return false;
        }
        client_ = ::accept(listener_, nullptr, nullptr);
        return client_ != INVALID_SOCKET;
    }

    bool AcceptBlocking(int attempts) {
        for (int i = 0; i < attempts; ++i) {
            if (AcceptOnce()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    void Send(const std::vector<std::uint8_t>& bytes) {
        if (client_ != INVALID_SOCKET) {
            ::send(client_, reinterpret_cast<const char*>(bytes.data()),
                   static_cast<int>(bytes.size()), 0);
        }
    }

    void ShutdownClient() {
        if (client_ != INVALID_SOCKET) {
            ::closesocket(client_);
            client_ = INVALID_SOCKET;
        }
    }

    void Close() {
        ShutdownClient();
        if (listener_ != INVALID_SOCKET) {
            ::closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        if (acquired_) {
            fgb::kit::WinsockRuntime::Release();
            acquired_ = false;
        }
    }

    std::uint16_t Port() const { return port_; }

private:
    SOCKET listener_ = INVALID_SOCKET;
    SOCKET client_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    bool acquired_ = false;
};

std::vector<std::uint8_t> BuildValidPacket() {
    std::vector<std::uint8_t> bytes(alxr::kPacketBytes, 0);
    bytes[0] = alxr::kExprFBV2;
    bytes[1] = alxr::kEyeExtGaze;
    bytes[3] = 1;
    bytes[4] = 1;
    const float jaw = 0.5f;
    std::memcpy(bytes.data() + 6 + 4 * 24, &jaw, sizeof(jaw));
    const float identity_w = 1.0f;
    std::memcpy(bytes.data() + 286 + 12, &identity_w, sizeof(identity_w));
    std::memcpy(bytes.data() + 314 + 12, &identity_w, sizeof(identity_w));
    return bytes;
}

} // namespace

int main() {
    // ---- Backoff：1s 起步、每次翻倍、8s 封顶、成功清零 ----
    {
        alxr::Backoff backoff{kSecondNs, 8 * kSecondNs};
        CHECK(backoff.Due(0));
        backoff.RecordFailure(0);
        CHECK_EQ(backoff.CurrentDelayNs(), kSecondNs);
        CHECK(!backoff.Due(kSecondNs - 1));
        CHECK(backoff.Due(kSecondNs));
        backoff.RecordFailure(kSecondNs);
        CHECK_EQ(backoff.CurrentDelayNs(), 2 * kSecondNs);
        backoff.RecordFailure(3 * kSecondNs);
        CHECK_EQ(backoff.CurrentDelayNs(), 4 * kSecondNs);
        backoff.RecordFailure(8 * kSecondNs);
        CHECK_EQ(backoff.CurrentDelayNs(), 8 * kSecondNs);
        backoff.RecordFailure(20 * kSecondNs);
        CHECK_EQ(backoff.CurrentDelayNs(), 8 * kSecondNs); // 封顶不再翻倍
        backoff.Reset();
        CHECK_EQ(backoff.CurrentDelayNs(), std::uint64_t(0));
        CHECK(backoff.Due(21 * kSecondNs));
    }

    std::string error;
    CHECK(fgb::kit::WinsockRuntime::Acquire(error));

    // ---- 对端不存在：连接失败被记录，退避逐次拉开 ----
    {
        FakeStreamServer probe;
        std::string open_error;
        CHECK(probe.Open(open_error));
        const std::uint16_t port = probe.Port();
        probe.Close(); // 关掉监听，connect 必然被拒

        alxr::TcpStreamClient client;
        client.SetTarget("127.0.0.1", port);
        std::vector<alxr::DecodedPacket> packets;
        auto first = client.Poll(fgb::kit::NowNs(), packets);
        CHECK(!first.connected_now);
        CHECK_EQ(first.packets, std::size_t(0));

        // 连不上时不该疯狂重连：第二次尝试前必须等退避到期。
        auto soon = client.Poll(fgb::kit::NowNs(), packets);
        CHECK(!soon.connected_now); // 退避未到，连都没试
    }

    // ---- 正常路径：连接 → 切包（含半包滞留）→ 断流检出 → 重连 ----
    {
        FakeStreamServer server;
        std::string open_error;
        CHECK(server.Open(open_error));

        alxr::TcpStreamClient client;
        client.SetTarget("127.0.0.1", server.Port());
        std::vector<alxr::DecodedPacket> packets;

        alxr::StreamPollResult connected;
        for (int i = 0; i < 50 && !connected.connected_now; ++i) {
            connected = client.Poll(fgb::kit::NowNs(), packets);
            // 必须显式 accept：非阻塞 connect 在三次握手进 backlog 后就报成功，
            // 不 accept 的话服务端 send 找不到已连接套接字。
            server.AcceptOnce();
        }
        CHECK(connected.connected_now);
        CHECK(connected.just_connected);

        const auto packet_bytes = BuildValidPacket();
        // 发一包 + 半包：只吐出完整的一包，半包滞留等下一轮。
        std::vector<std::uint8_t> stream;
        stream.insert(stream.end(), packet_bytes.begin(), packet_bytes.end());
        stream.insert(stream.end(), packet_bytes.begin(),
                      packet_bytes.begin() + static_cast<std::ptrdiff_t>(alxr::kPacketBytes / 2));
        server.Send(stream);

        alxr::StreamPollResult got;
        for (int i = 0; i < 50 && got.packets == 0; ++i) {
            got = client.Poll(fgb::kit::NowNs(), packets);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK_EQ(got.packets, std::size_t(1));
        CHECK(!packets.empty());
        if (!packets.empty()) {
            CHECK_NEAR(packets.back().weights[24], 0.5f, 1e-6);
            CHECK_EQ(packets.back().gaze_pose_valid[0], std::uint8_t(1));
        }
        CHECK(client.LastPacketNs() != 0);

        // 补齐后半包 → 第二包到手。
        packets.clear();
        server.Send(std::vector<std::uint8_t>(
            packet_bytes.begin() + static_cast<std::ptrdiff_t>(alxr::kPacketBytes / 2),
            packet_bytes.end()));
        alxr::StreamPollResult got2;
        for (int i = 0; i < 50 && got2.packets == 0; ++i) {
            got2 = client.Poll(fgb::kit::NowNs(), packets);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK_EQ(got2.packets, std::size_t(1));

        // 服务端断开：检出断流；LastPacketNs 保留供 Staleness 判断。
        server.ShutdownClient();
        alxr::StreamPollResult dropped;
        for (int i = 0; i < 50 && !dropped.just_disconnected; ++i) {
            dropped = client.Poll(fgb::kit::NowNs(), packets);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(dropped.just_disconnected);
        CHECK(!dropped.connected_now);
        CHECK(client.LastPacketNs() != 0);

        // 退避后重连成功：Reset 清掉计数、再收到新包。
        server.Close();
        FakeStreamServer server2;
        std::string open_error2;
        CHECK(server2.Open(open_error2));
        client.SetTarget("127.0.0.1", server2.Port());
        // 新目标的退避窗口：手动推进假时钟到 10 秒后（Backoff 用外部时钟，可注入）。
        alxr::StreamPollResult reconnected;
        for (int i = 0; i < 100 && !reconnected.connected_now; ++i) {
            reconnected = client.Poll(fgb::kit::NowNs() + 10ull * kSecondNs, packets);
            server2.AcceptOnce(); // 同前：accept 必须在连接成功那一轮也执行
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(reconnected.connected_now);
        CHECK(reconnected.just_connected);
        packets.clear();
        server2.Send(packet_bytes);
        alxr::StreamPollResult got3;
        for (int i = 0; i < 50 && got3.packets == 0; ++i) {
            got3 = client.Poll(fgb::kit::NowNs() + 10ull * kSecondNs, packets);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK_EQ(got3.packets, std::size_t(1));
        server2.Close();
    }

    fgb::kit::WinsockRuntime::Release();
    return CheckSummary("alxr-reconnect");
}
