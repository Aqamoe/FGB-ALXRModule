#pragma once
// libalxr（alxr_engine.dll）运行时适配层。
//
// 厂商 DLL 一律不链接、不打包：用 fgb::kit::Library 在「模块目录 → 系统目录 → PATH」
// 里运行时找。找不到就给出可操作的错误（要么装 OpenXR 运行时配套软件，要么把
// alxr_engine.dll 放到模块目录旁）。布局与原型见 alxr_abi.hpp。
//
// 真实调用路径在无厂商 DLL / 无 OpenXR 运行时的 CI 上不会被执行——
// 这正好让 CI 能编能测（ABI 镜像由 static_assert 把关，映射层由单测覆盖），
// 差异说明写进 README 与 docs/verification.md。

#include "alxr_abi.hpp"
#include "alxr_packet.hpp"

#include "fgb/kit/host.hpp"
#include "fgb/kit/library.hpp"

#include <cstdint>
#include <string>

namespace alxr {

// 会话参数对应上游 ALXRLocalModule.CreateALXRClientCtx 的取值。
struct EngineConfig {
    std::uint32_t graphics_api = 0;  // ALXRGraphicsApi：0=Auto
    bool headless_session = true;    // headless 优先
    bool simulate_headless = true;   // 运行时不支持时模拟 headless
    bool verbose = false;
};

class AlxrEngine {
public:
    AlxrEngine() = default;
    AlxrEngine(const AlxrEngine&) = delete;
    AlxrEngine& operator=(const AlxrEngine&) = delete;
    ~AlxrEngine() { Shutdown(); }

    // 只解析符号，不碰 OpenXR——CI / 缺依赖的机器能立刻拿到可操作错误。
    bool Load(const std::string& module_dir, std::string& error);
    bool Loaded() const { return library_.Loaded(); }
    const std::string& LibraryName() const { return library_.Name(); }

    // alxr_init：起 headless OpenXR 会话。可能较慢（要连运行时），调用方负责重试节奏。
    bool StartSession(const EngineConfig& config, std::string& error);
    bool SessionStarted() const { return session_started_; }

    // 拉一帧。返回 false = 本 tick 没有有效面捕（会话没在跑或类型是 None）。
    // exit_render_loop 为 true 时会话已被 libalxr 判定要重建。
    bool ReadFrame(DecodedPacket& out, bool& exit_render_loop);

    bool SessionRunning();
    void Shutdown();

private:
    fgb::kit::Library library_;
    AlxrInitFn init_ = nullptr;
    AlxrDestroyFn destroy_ = nullptr;
    AlxrProcessFrame2Fn process_frame2_ = nullptr;
    AlxrIsSessionRunningFn is_session_running_ = nullptr;
    AlxrRequestExitSessionFn request_exit_session_ = nullptr;

    AlxrClientCtx ctx_{};
    AlxrSystemProperties system_properties_{};
    AlxrProcessFrameResult frame_result_{};
    std::string data_path_; // internalDataPath 指向的存储要活过 alxr_init
    bool session_started_ = false;
};

} // namespace alxr
