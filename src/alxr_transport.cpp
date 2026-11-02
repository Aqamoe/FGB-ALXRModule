#include "alxr_transport.hpp"

#include "fgb/kit/udp.hpp" // 复用 WinsockRuntime 的引用计数与 NowNs

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>

namespace alxr {
namespace {

constexpr std::size_t kReadChunkBytes = 8192;
// TCP 是字节流：残留缓冲最多囤 40 包，超了丢最老并计数，避免内存被刷屏打爆。
constexpr std::size_t kMaxPendingBytes = 40 * kPacketBytes;

} // namespace

void TcpStreamClient::Close() {
    if (socket_ != -1) {
        const SOCKET handle = static_cast<SOCKET>(socket_);
        ::closesocket(handle);
        socket_ = -1;
    }
    socket_ok_ = false;
    pending_.clear();
    if (acquired_) {
        fgb::kit::WinsockRuntime::Release();
        acquired_ = false;
    }
}

bool TcpStreamClient::TryConnect() {
    if (!fgb::kit::WinsockRuntime::Acquire(last_error_)) {
        return false;
    }
    acquired_ = true;

    const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
        last_error_ = "socket() 失败，WSA " + std::to_string(::WSAGetLastError());
        fgb::kit::WinsockRuntime::Release();
        acquired_ = false;
        return false;
    }

    const int no_delay = 1;
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay),
                 sizeof(no_delay));
    u_long non_blocking = 1;
    ::ioctlsocket(handle, FIONBIO, &non_blocking);

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port_);
    if (::inet_pton(AF_INET, address_.c_str(), &target.sin_addr) != 1) {
        last_error_ = "串流端地址非法：" + address_;
        ::closesocket(handle);
        fgb::kit::WinsockRuntime::Release();
        acquired_ = false;
        return false;
    }

    const int result =
        ::connect(handle, reinterpret_cast<sockaddr*>(&target), sizeof(target));
    if (result == 0) {
        socket_ = static_cast<long long>(handle);
        socket_ok_ = true;
        return true;
    }
    const int code = ::WSAGetLastError();
    if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS) {
        last_error_ = "connect 失败，WSA " + std::to_string(code);
        ::closesocket(handle);
        fgb::kit::WinsockRuntime::Release();
        acquired_ = false;
        return false;
    }

    // 非阻塞 connect 在途：用 50ms 的可写探针完成握手，卡住就本轮放弃、下轮再试。
    fd_set write_set{};
    FD_ZERO(&write_set);
    FD_SET(handle, &write_set);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 50 * 1000;
    const int ready = ::select(0, nullptr, &write_set, nullptr, &timeout);
    if (ready == 1) {
        int so_error = 0;
        int so_error_len = sizeof(so_error);
        ::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error),
                     &so_error_len);
        if (so_error == 0) {
            socket_ = static_cast<long long>(handle);
            socket_ok_ = true;
            return true;
        }
        last_error_ = "connect 完成但 SO_ERROR=" + std::to_string(so_error);
    } else if (ready == 0) {
        last_error_ = "connect 超时（50ms 探针），稍后重连";
    } else {
        last_error_ = "select 失败，WSA " + std::to_string(::WSAGetLastError());
    }
    ::closesocket(handle);
    fgb::kit::WinsockRuntime::Release();
    acquired_ = false;
    return false;
}

void TcpStreamClient::OnDisconnected(std::uint64_t now_ns) {
    Close();
    backoff_.RecordFailure(now_ns);
}

StreamPollResult TcpStreamClient::Poll(std::uint64_t now_ns,
                                       std::vector<DecodedPacket>& out_packets) {
    StreamPollResult result;

    if (!socket_ok_) {
        if (backoff_.Due(now_ns)) {
            if (TryConnect()) {
                backoff_.Reset();
                result.just_connected = true;
                result.connected_now = true;
            } else {
                backoff_.RecordFailure(now_ns);
            }
        }
        result.connected_now = socket_ok_;
        return result;
    }

    // 读取：一次 tick 最多搬 8 块，块间不睡眠；来源刷屏时靠积压上限丢旧包。
    std::uint8_t chunk[kReadChunkBytes];
    bool peer_closed = false;
    for (int round = 0; round < 8 && !peer_closed; ++round) {
        const int received = ::recv(static_cast<SOCKET>(socket_), reinterpret_cast<char*>(chunk),
                                    static_cast<int>(sizeof(chunk)), 0);
        if (received > 0) {
            pending_.insert(pending_.end(), chunk, chunk + received);
            if (pending_.size() > kMaxPendingBytes) {
                const std::size_t excess = pending_.size() - kMaxPendingBytes;
                pending_.erase(pending_.begin(),
                               pending_.begin() + static_cast<std::ptrdiff_t>(excess));
                ++drops_;
            }
            continue;
        }
        if (received == 0) {
            peer_closed = true;
            break;
        }
        const int code = ::WSAGetLastError();
        if (code == WSAEWOULDBLOCK) {
            break;
        }
        last_error_ = "recv 失败，WSA " + std::to_string(code);
        peer_closed = true;
        break;
    }

    // 切包：定长 342。上游 C# 是「读满一整包再解析」，等价于这里的按长取模切分。
    std::size_t offset = 0;
    std::vector<DecodedPacket> complete;
    while (pending_.size() - offset >= kPacketBytes) {
        DecodedPacket packet;
        std::string error;
        const bool parsed =
            ParsePacket(pending_.data() + offset, kPacketBytes, packet, error);
        offset += kPacketBytes;
        ++packets_;
        ++result.packets;
        if (!parsed) {
            ++parse_errors_;
            ++result.parse_errors;
            last_error_ = error;
            continue;
        }
        last_packet_ns_ = now_ns;
        complete.push_back(packet);
    }
    if (offset > 0) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    out_packets.insert(out_packets.end(), complete.begin(), complete.end());

    if (peer_closed) {
        OnDisconnected(now_ns);
        result.just_disconnected = true;
        result.connected_now = false;
    } else {
        result.connected_now = true;
    }
    return result;
}

} // namespace alxr
