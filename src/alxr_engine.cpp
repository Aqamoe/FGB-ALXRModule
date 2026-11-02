#include "alxr_engine.hpp"

#include "fgb/kit/udp.hpp" // NowNs，与 Kit 其它来源同一时钟

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace alxr {
namespace {

// ---- libalxr 要求的回调蹦床：全是空实现，路径哈希用 FNV-1a ----
// 上游 C# 里这些回调也都是 no-op（inputSend/viewsConfigSend/... 皆空 lambda），
// 只有 pathStringToHash 返回一个稳定值。签名按 C 导出约定（x64 上即默认调用约定）。
// 参数类型是 libalxr 的不透明指针：本模块不读它们，用 void* 收即可。
void CdeclInputSend(void* /*data*/) {}
void CdeclViewsConfigSend(void* /*view_config*/) {}
std::uint64_t CdeclPathStringToHash(const char* path) {
    std::uint64_t hash = 1469598103934665603ull; // FNV-1a 64 位 offset basis
    if (path != nullptr) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p) {
            hash = (hash ^ static_cast<std::uint64_t>(*p)) * 1099511628211ull;
        }
    }
    return hash;
}
void CdeclTimeSyncSend(void* /*data*/) {}
void CdeclVideoErrorReportSend() {}
void CdeclBatterySend(std::uint64_t /*path*/, float /*gauge*/, bool /*plugged*/) {}
void CdeclUserPresenceSend(bool /*present*/) {}
void CdeclSetWaitingNextIdr(bool /*waiting*/) {}
void CdeclRequestIdr() {}

} // namespace

bool AlxrEngine::Load(const std::string& module_dir, std::string& error) {
    if (library_.Loaded()) {
        return true;
    }
    if (!library_.Load(kEngineDllName, module_dir, error)) {
        return false;
    }

    init_ = library_.Function<AlxrInitFn>("alxr_init");
    destroy_ = library_.Function<AlxrDestroyFn>("alxr_destroy");
    process_frame2_ = library_.Function<AlxrProcessFrame2Fn>("alxr_process_frame2");
    is_session_running_ = library_.Function<AlxrIsSessionRunningFn>("alxr_is_session_running");
    request_exit_session_ = library_.Function<AlxrRequestExitSessionFn>("alxr_request_exit_session");

    if (init_ == nullptr || destroy_ == nullptr || process_frame2_ == nullptr ||
        is_session_running_ == nullptr) {
        std::string missing = library_.LastMissing();
        error = std::string("alxr_engine.dll 缺少必需导出（最近缺失：") +
                (missing.empty() ? "alxr_init/alxr_destroy/alxr_process_frame2" : missing) +
                "）。请安装与 libalxr ABI 匹配的运行时后重启引擎。";
        library_.Close();
        return false;
    }
    return true;
}

bool AlxrEngine::StartSession(const EngineConfig& config, std::string& error) {
    if (!library_.Loaded()) {
        error = "alxr_engine.dll 尚未加载";
        return false;
    }
    if (session_started_) {
        return true;
    }

    // internalDataPath：上游给的是 ModuleLibs 目录；这里 libalxr 只用来放缓存文件，
    // 给当前目录即可（字符串必须在会话期间保活，所以存成员而不是临时量）。
    data_path_ = ".";
    std::memset(&ctx_, 0, sizeof(ctx_));
    ctx_.input_send = reinterpret_cast<void*>(&CdeclInputSend);
    ctx_.views_config_send = reinterpret_cast<void*>(&CdeclViewsConfigSend);
    ctx_.path_string_to_hash = reinterpret_cast<void*>(&CdeclPathStringToHash);
    ctx_.time_sync_send = reinterpret_cast<void*>(&CdeclTimeSyncSend);
    ctx_.video_error_report_send = reinterpret_cast<void*>(&CdeclVideoErrorReportSend);
    ctx_.battery_send = reinterpret_cast<void*>(&CdeclBatterySend);
    ctx_.user_presence_send = reinterpret_cast<void*>(&CdeclUserPresenceSend);
    ctx_.set_waiting_next_idr = reinterpret_cast<void*>(&CdeclSetWaitingNextIdr);
    ctx_.request_idr = reinterpret_cast<void*>(&CdeclRequestIdr);

    // firmware/xrApi 版本保持 0：上游 C# 也只给 Android 客户端填 0.0.0。
    ctx_.graphics_api = config.graphics_api;
    ctx_.decoder_type = 0;             // D3D11VA（上游固定值）
    ctx_.display_color_space = 5;      // Quest = ALXRColorSpace.Default
    ctx_.passthrough_mode = 0;         // None
    ctx_.internal_data_path = data_path_.empty() ? nullptr : data_path_.data();
    ctx_.face_tracking_data_sources = 1u; // VisualSource 位标
    ctx_.facial_tracking = kExprAuto;     // 由运行时挑 FB / FB_V2 / HTC / Android
    ctx_.eye_tracking = kEyeAuto;
    ctx_.tracking_server_port = kDefaultTrackingPort;
    ctx_.verbose = config.verbose ? 1 : 0;
    ctx_.disable_linearize_srgb = 0;
    ctx_.no_suggested_bindings = 1;
    ctx_.no_server_framerate_lock = 0;
    ctx_.no_frame_skip = 0;
    ctx_.disable_local_dimming = 1;
    ctx_.headless_session = config.headless_session ? 1 : 0;
    ctx_.no_ft_server = 1;    // 本地不需要把数据再推给别的客户端
    ctx_.no_passthrough = 1;
    ctx_.no_hand_tracking = 1;
    ctx_.simulate_headless = config.simulate_headless ? 1 : 0;
    ctx_.no_visibility_masks = 1;
    ctx_.no_multiview_rendering = 0;

    std::memset(&system_properties_, 0, sizeof(system_properties_));
    const bool ok = init_(&ctx_, &system_properties_);
    if (!ok) {
        error = "alxr_init 失败：确认已把活动 OpenXR 运行时设为目标运行时、头显已连接并处于活动状态";
        return false;
    }
    session_started_ = true;
    std::memset(&frame_result_, 0, sizeof(frame_result_));
    return true;
}

bool AlxrEngine::ReadFrame(DecodedPacket& out, bool& exit_render_loop) {
    exit_render_loop = false;
    if (!session_started_ || process_frame2_ == nullptr) {
        return false;
    }
    std::memset(&frame_result_, 0, sizeof(frame_result_));
    process_frame2_(&frame_result_);
    if (frame_result_.exit_render_loop != 0) {
        exit_render_loop = true;
        return false;
    }
    if (frame_result_.request_restart != 0) {
        exit_render_loop = true; // 复用退出路径：会话需要重建
        return false;
    }
    if (frame_result_.expression_type == kExprNone &&
        frame_result_.eye_tracker_type == kEyeNone) {
        return false; // 没有面捕/眼捕数据的一帧
    }
    FillFromResult(frame_result_, out);
    return true;
}

bool AlxrEngine::SessionRunning() {
    if (!session_started_ || is_session_running_ == nullptr) {
        return false;
    }
    return is_session_running_();
}

void AlxrEngine::Shutdown() {
    if (session_started_ && destroy_ != nullptr) {
        destroy_();
    }
    session_started_ = false;
    if (library_.Loaded()) {
        library_.Close();
    }
    init_ = nullptr;
    destroy_ = nullptr;
    process_frame2_ = nullptr;
    is_session_running_ = nullptr;
    request_exit_session_ = nullptr;
}

} // namespace alxr
