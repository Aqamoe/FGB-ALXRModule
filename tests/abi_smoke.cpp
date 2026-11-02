// 走真实 ABI 的端到端冒烟（Remote 产物）：
// 加载 DLL → query → initialize → 合成 TCP 服务端喂 342 字节包 → update → 检查提交帧。
//
// 与上游 VRCFT 模块同构：模块是 TCP 客户端，这里在 127.0.0.1 上开合成服务端。
// 端口写死 49192 会撞真实串流端和并行任务，改用系统分配的临时端口，
// 通过模块目录的 config.ini 告知模块。
// 因此不挂进 ctest，只由 `cmake --build ... --target smoke-test` 触发。

#include "alxr_abi.hpp"

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"
#include "fgb/kit/udp.hpp"
#include "fgb_module_api.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kSecondMs = 1000;

// 342 字节的合法 FB_V2 包：jawOpen 权重 + 双眼恒等 gaze pose。
std::vector<std::uint8_t> BuildPacket(float jaw_open) {
    std::vector<std::uint8_t> bytes(alxr::kPacketBytes, 0);
    bytes[0] = alxr::kExprFBV2;
    bytes[1] = alxr::kEyeExtGaze;
    bytes[2] = 1;
    bytes[3] = 1;
    bytes[4] = 1;
    std::memcpy(bytes.data() + 6 + 4 * 24, &jaw_open, sizeof(jaw_open));
    const float w = 1.0f;
    std::memcpy(bytes.data() + 286 + 12, &w, sizeof(w));
    std::memcpy(bytes.data() + 314 + 12, &w, sizeof(w));
    return bytes;
}

// 一次性合成服务端：临时端口、accept 若干连接、逐条发送。
class FakeAlxrClient {
public:
    ~FakeAlxrClient() { Close(); }

    bool Open(std::string& error) {
        if (!fgb::kit::WinsockRuntime::Acquire(error)) {
            return false;
        }
        acquired_ = true;
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            error = "bind 临时端口失败";
            return false;
        }
        int len = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &len);
        port_ = ntohs(address.sin_port);
        if (::listen(listener_, 4) != 0) {
            error = "listen 失败";
            return false;
        }
        return true;
    }

    // 等模块连上来；返回 false = 超时。
    bool WaitForConnection(int timeout_ms) {
        fd_set read_set{};
        FD_ZERO(&read_set);
        FD_SET(listener_, &read_set);
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        if (::select(0, &read_set, nullptr, nullptr, &timeout) != 1) {
            return false;
        }
        client_ = ::accept(listener_, nullptr, nullptr);
        return client_ != INVALID_SOCKET;
    }

    bool Send(const std::vector<std::uint8_t>& bytes) {
        if (client_ == INVALID_SOCKET) {
            return false;
        }
        return ::send(client_, reinterpret_cast<const char*>(bytes.data()),
                      static_cast<int>(bytes.size()), 0) != SOCKET_ERROR;
    }

    // 模拟串流端断开：关掉当前连接，模块应退避后重连到同一 listener。
    void DropClient() {
        if (client_ != INVALID_SOCKET) {
            ::closesocket(client_);
            client_ = INVALID_SOCKET;
        }
    }

    bool Connected() const { return client_ != INVALID_SOCKET; }

    std::uint16_t Port() const { return port_; }

    void Close() {
        DropClient();
        if (listener_ != INVALID_SOCKET) {
            ::closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        if (acquired_) {
            fgb::kit::WinsockRuntime::Release();
            acquired_ = false;
        }
    }

private:
    SOCKET listener_ = INVALID_SOCKET;
    SOCKET client_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    bool acquired_ = false;
};

bool WriteConfig(const std::string& directory, std::uint16_t port) {
    std::ofstream stream{directory + "\\config.ini", std::ios::binary | std::ios::trunc};
    if (!stream) {
        return false;
    }
    stream << "ip = 127.0.0.1\n";
    stream << "port = " << port << "\n";
    return static_cast<bool>(stream.flush());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法：%s <alxr-remote.dll 路径>\n", argv[0]);
        return 2;
    }

    // 模块目录用 DLL 所在目录：config.ini 落在那里，宿主语义一致。
    std::string module_dir = ".";
    char dir_buffer[MAX_PATH] = {0};
    if (::GetTempPathA(MAX_PATH, dir_buffer) > 0) {
        module_dir = std::string(dir_buffer) + "fgb-alxr-smoke-" +
                     std::to_string(::GetCurrentProcessId());
        ::CreateDirectoryA(module_dir.c_str(), nullptr);
    }

    HMODULE library = ::LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(library != nullptr);
    if (library == nullptr) {
        std::printf("LoadLibrary 失败，错误码 %lu（DLL 是否已构建？依赖是否在同目录？）\n",
                    ::GetLastError());
        return CheckSummary("alxr-remote-abi-smoke");
    }

    const auto query = reinterpret_cast<decltype(&fgb_module_query_v2)>(
        ::GetProcAddress(library, "fgb_module_query_v2"));
    CHECK(query != nullptr);

    fgb::kit::testing::HostMock host;
    FgbInitParams params = host.MakeParams(module_dir);

    const FgbModule* module = query(FGB_MODULE_ABI_V2);
    CHECK(module != nullptr);
    if (module == nullptr) {
        ::FreeLibrary(library);
        return CheckSummary("alxr-remote-abi-smoke");
    }
    CHECK(module->id == std::string{"com.fgbbridge.alxr-remote"});
    CHECK_EQ(module->abi_version, FGB_MODULE_ABI_V2);
    CHECK_EQ(module->capabilities,
             static_cast<std::uint32_t>(FGB_CAP_EXPRESSION | FGB_CAP_EYES));
    CHECK(query(FGB_MODULE_ABI_V2 + 1) == nullptr); // ABI 不匹配必须拒绝

    std::string error;
    FakeAlxrClient server;
    CHECK(server.Open(error));
    CHECK(WriteConfig(module_dir, server.Port()));

    void* self = nullptr;
    CHECK_EQ(module->initialize(&self, host.services(), &params), 1);
    if (self == nullptr) {
        std::printf("initialize 失败：%s\n", host.LastStatusDetail().c_str());
        ::FreeLibrary(library);
        return CheckSummary("alxr-remote-abi-smoke");
    }

    // 没有数据时不应该提交帧。
    module->update(self);
    CHECK(host.committed().empty());

    CHECK(server.WaitForConnection(5 * kSecondMs));
    CHECK(server.Send(BuildPacket(0.62f)));
    for (int attempt = 0; attempt < 400 && host.committed().empty(); ++attempt) {
        module->update(self);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(!host.committed().empty());

    if (!host.committed().empty()) {
        const FgbFrameSlot& slot = host.committed().back();
        CHECK((slot.attributes & FGB_ATTR_EXPRESSION) != 0);
        CHECK((slot.attributes & FGB_ATTR_LEFT_OPENNESS) != 0);
        CHECK((slot.attributes & FGB_ATTR_RIGHT_OPENNESS) != 0);
        CHECK((slot.attributes & FGB_ATTR_LEFT_GAZE) != 0);
        CHECK((slot.attributes & FGB_ATTR_RIGHT_GAZE) != 0);
        const int jaw_index = host.IndexOf("JawOpen");
        CHECK(jaw_index >= 0);
        if (jaw_index >= 0) {
            CHECK_NEAR(slot.shapes[jaw_index], 0.62, 1e-5);
        }
        // 恒等四元数 → 正前 (0,0,-1)。
        CHECK_NEAR(slot.left_eye.gaze_dir[2], -1.0, 1e-4);
        CHECK_NEAR(slot.right_eye.gaze_dir[2], -1.0, 1e-4);
        CHECK(host.HasStatus(FGB_STATUS_STARTING) || host.HasStatus(FGB_STATUS_ACTIVE));
    }

    // 半包 + 补包：只应吐出一个完整帧，不丢不错位。
    std::size_t expected_frames = host.committed().size() + 1;
    {
        const auto packet = BuildPacket(0.31f);
        CHECK(server.Send(std::vector<std::uint8_t>(packet.begin(),
                                                   packet.begin() +
                                                       static_cast<std::ptrdiff_t>(
                                                           alxr::kPacketBytes / 2))));
        module->update(self);
        CHECK(server.Send(std::vector<std::uint8_t>(
            packet.begin() + static_cast<std::ptrdiff_t>(alxr::kPacketBytes / 2), packet.end())));
        for (int attempt = 0; attempt < 400; ++attempt) {
            module->update(self);
            if (host.committed().size() >= expected_frames) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(host.committed().size() >= expected_frames);
        if (host.committed().size() >= expected_frames) {
            const int jaw_index = host.IndexOf("JawOpen");
            CHECK_NEAR(host.committed().back().shapes[jaw_index], 0.31, 1e-5);
        }
    }

    // 断流 → 退避重连：服务端关连接再等模块自己连回来，继续能出帧。
    expected_frames = host.committed().size() + 1;
    server.DropClient();
    bool reconnected = false;
    for (int attempt = 0; attempt < 400 && !reconnected; ++attempt) {
        module->update(self);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        reconnected = server.WaitForConnection(20);
    }
    CHECK(reconnected);
    CHECK(server.Connected());
    CHECK(server.Send(BuildPacket(0.44f)));
    for (int attempt = 0; attempt < 400 && host.committed().size() < expected_frames; ++attempt) {
        module->update(self);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(host.committed().size() >= expected_frames);
    if (host.committed().size() >= expected_frames) {
        const int jaw_index = host.IndexOf("JawOpen");
        CHECK_NEAR(host.committed().back().shapes[jaw_index], 0.44, 1e-5);
    }

    module->teardown(self);
    server.Close();
    ::FreeLibrary(library);
    ::RemoveDirectoryA(module_dir.c_str());
    return CheckSummary("alxr-remote-abi-smoke");
}
