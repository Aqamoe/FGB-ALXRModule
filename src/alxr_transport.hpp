#pragma once
// Remote 侧的 TCP 客户端：主动连「串流端」（头显上的 ALXR 客户端）IP:49192，
// 按 342 字节定长切分 ALXRFacialEyePacket，断线用指数退避自动重连。
//
// 线程模型与 Kit 其它来源一致：全部在宿主 update() 线程里驱动，非阻塞 socket，
// 每个 tick 只搬一次数据，不起后台线程。
//
// 上游行为（ALXRRemoteModule.cs ConnectToServer/ReadALXRFacialEyePacket）是
// 「阻塞读满 342 字节 + 异常即断开重连」；这里保持同一语义但改为非阻塞 +
// 退避（1s→2s→4s→8s 封顶），避免网络抖动卡住引擎线程。

#include "alxr_packet.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace alxr {

// 重连退避：纯逻辑，单测直接驱动假时钟。
class Backoff {
public:
    Backoff(std::uint64_t base_ns, std::uint64_t max_ns) : base_ns_(base_ns), max_ns_(max_ns) {}

    void Reset() {
        current_ns_ = 0;
        next_try_ns_ = 0;
    }

    // 记一次失败：立刻把下次尝试时间推到 now + 当前间隔（并翻倍，封顶 max）。
    void RecordFailure(std::uint64_t now_ns) {
        const std::uint64_t delay = current_ns_ == 0 ? base_ns_ : std::min(current_ns_ * 2, max_ns_);
        current_ns_ = delay;
        next_try_ns_ = now_ns + delay;
    }

    // 到点可以试了吗；从没失败过（next=0）随时可以。
    bool Due(std::uint64_t now_ns) const {
        return next_try_ns_ == 0 || now_ns >= next_try_ns_;
    }
    std::uint64_t NextTryNs() const { return next_try_ns_; }

    std::uint64_t CurrentDelayNs() const { return current_ns_; }

private:
    std::uint64_t base_ns_ = 0;
    std::uint64_t max_ns_ = 0;
    std::uint64_t current_ns_ = 0;
    std::uint64_t next_try_ns_ = 0;
};

struct StreamPollResult {
    bool connected_now = false;       // 当前是否连着
    bool just_connected = false;      // 本次 Poll 完成了握手
    bool just_disconnected = false;   // 本次 Poll 检测到断开
    std::size_t packets = 0;          // 本次取出的完整包数
    std::size_t parse_errors = 0;     // 其中解析失败的包数（长度外的字段级异常在 Parse 内清洗）
};

// 主动连接型 TCP 客户端。地址/端口来自 config.ini（默认 127.0.0.1:49192）。
class TcpStreamClient {
public:
    TcpStreamClient() = default;
    TcpStreamClient(const TcpStreamClient&) = delete;
    TcpStreamClient& operator=(const TcpStreamClient&) = delete;
    ~TcpStreamClient() { Close(); }

    void SetTarget(std::string address, std::uint16_t port) {
        address_ = std::move(address);
        port_ = port;
    }

    // 尝试建连（若退避未到点则直接跳过）+ 收数据 + 切包。
    // out_packets 里按到达顺序返回本轮的完整包（解析失败的不返回，只计数）。
    StreamPollResult Poll(std::uint64_t now_ns, std::vector<DecodedPacket>& out_packets);

    void Close();

    bool Connected() const { return socket_ok_; }
    std::uint64_t LastPacketNs() const { return last_packet_ns_; }
    std::uint64_t Packets() const { return packets_; }
    std::uint64_t ParseErrors() const { return parse_errors_; }
    std::uint64_t Drops() const { return drops_; }
    std::uint64_t NextRetryNs() const { return backoff_.CurrentDelayNs(); }
    const std::string& Target() const { return address_; }
    std::uint16_t Port() const { return port_; }
    std::string LastError() const { return last_error_; }

private:
    bool TryConnect();
    void OnDisconnected(std::uint64_t now_ns);

    std::string address_ = "127.0.0.1";
    std::uint16_t port_ = kDefaultTrackingPort;
    long long socket_ = -1; // SOCKET（避免把头文件拖进 winsock2）
    bool socket_ok_ = false;
    bool acquired_ = false; // WinsockRuntime 引用计数的持有标记
    Backoff backoff_{1000000000ull, 8000000000ull}; // 1s 起步，8s 封顶
    std::vector<std::uint8_t> pending_;             // 未满一包的残留
    std::uint64_t last_packet_ns_ = 0;
    std::uint64_t packets_ = 0;
    std::uint64_t parse_errors_ = 0;
    std::uint64_t drops_ = 0;
    std::string last_error_;
};

} // namespace alxr
