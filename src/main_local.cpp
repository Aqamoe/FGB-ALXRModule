// ALXR Local → FaceGaze Bridge 原生模块。
//
// 直接坐在 PC 的 OpenXR 运行时（Link/AirLink/VDXR 等）上：运行时加载
// alxr_engine.dll（libalxr），起一个 headless 会话读 FB / FB_V2 / HTC / EXT /
// Android 面捕扩展，输出 Unified Expressions 表情与双眼睁眼度/注视方向。
// 不需要 VRCFT、不注入任何进程、也不需要管理员权限。
//
// 上游参考：VRCFT-ALXR-Modules 的 ALXRLocalModule.cs（同样的 libalxr 生命周期：
// init 失败退避重试、process_frame2 拉帧、exit/restart 时重建会话）。

#include "alxr_engine.hpp"
#include "alxr_mapper.hpp"
#include "alxr_packet.hpp"

#include "fgb/kit/config.hpp"
#include "fgb/kit/module.hpp"

#include <cstdint>
#include <string>

namespace {

constexpr std::uint64_t kWarnIntervalNs = 5ull * 1000ull * 1000ull * 1000ull;
// 上游节奏：RetryTime=2000ms、MaxRetryCount=10，放弃后等用户重启模块。
constexpr std::uint64_t kInitRetryNs = 2ull * 1000ull * 1000ull * 1000ull;
constexpr int kInitMaxRetry = 10;

// graphics_api 的可读名 → ALXRGraphicsApi 枚举（ALXRTypes.cs）。
bool ParseGraphicsApi(const std::string& text, std::uint32_t& out) {
    if (text == "auto") {
        out = 0;
    } else if (text == "vulkan2") {
        out = 1;
    } else if (text == "vulkan") {
        out = 2;
    } else if (text == "d3d12") {
        out = 3;
    } else if (text == "d3d11") {
        out = 4;
    } else {
        return false;
    }
    return true;
}

class AlxrLocalSource : public fgb::kit::IModuleSource {
public:
    bool Start(fgb::kit::Host& host, const FgbInitParams& params, std::string& error) override {
        fgb::kit::ConfigIni config;
        std::string config_error;
        const std::string directory = params.module_dir != nullptr ? params.module_dir : "";
        config.AllowKeys({"graphics_api", "headless", "simulate_headless", "verbose",
                          "eye_openness"});
        if (config.Load(directory, "config.ini", config_error)) {
            const std::string api = config.Get("graphics_api", "");
            if (!api.empty() && !ParseGraphicsApi(api, engine_config_.graphics_api)) {
                host.Warn("config.ini 的 graphics_api 无法识别（auto|vulkan2|vulkan|d3d12|d3d11），"
                          "沿用 auto");
            }
            engine_config_.headless_session = config.GetBool("headless", true);
            engine_config_.simulate_headless = config.GetBool("simulate_headless", true);
            engine_config_.verbose = config.GetBool("verbose", false);
            const std::string mode = config.Get("eye_openness", "");
            if (!mode.empty() && !alxr::ParseOpennessMode(mode, options_.openness_mode)) {
                host.Warn("config.ini 的 eye_openness 无法识别（linear|nonlinear|smooth|multi），"
                          "沿用 linear");
            }
            if (!config_error.empty()) {
                host.Warn("config.ini：" + config_error);
            }
            for (const auto& key : config.UnknownKeys()) {
                host.Warn("config.ini 有未识别的键：" + key);
            }
        }

        // 尽早加载厂商 DLL：CI / 没装依赖的机器在这里就得到可操作提示，
        // 而不是等到 OpenXR 会话失败才知道缺了什么。
        if (!engine_.Load(directory, error)) {
            return false;
        }

        fgb::kit::ShapeTable table;
        alxr::DeclareTargets(table);
        if (!table.Build(host)) {
            error = "宿主没有提供 shape_index，无法映射 Unified Expressions";
            return false;
        }
        if (table.ResolvedCount() < table.Size()) {
            host.Warn("部分 Unified Expressions 形状未被宿主识别，已跳过 " +
                      std::to_string(table.Size() - table.ResolvedCount()) + " 个");
        }
        table_ = std::move(table);

        host.Info("ALXR Local 已加载 alxr_engine.dll，等待 OpenXR 会话（headless 优先）");
        host.StatusStarting("等待 OpenXR 运行时会话");
        return true;
    }

    bool Read(fgb::kit::Host& host, FgbFrameSlot& slot, std::uint64_t now_ns) override {
        if (!engine_.SessionStarted()) {
            if (give_up_) {
                return false;
            }
            if (next_try_ns_ != 0 && now_ns < next_try_ns_) {
                return false;
            }
            std::string error;
            if (!engine_.StartSession(engine_config_, error)) {
                ++retry_count_;
                next_try_ns_ = now_ns + kInitRetryNs;
                if (retry_count_ >= kInitMaxRetry) {
                    give_up_ = true;
                    host.StatusError("libalxr 初始化失败（已重试 " +
                                     std::to_string(retry_count_) +
                                     " 次）。确认活动 OpenXR 运行时已设为目标运行时、"
                                     "头显处于活动状态，然后重启模块。原因：" + error);
                } else {
                    host.WarnThrottled(warn_ns_, now_ns, kWarnIntervalNs,
                                       "libalxr 初始化失败，2 秒后重试（" +
                                           std::to_string(retry_count_) + "/" +
                                           std::to_string(kInitMaxRetry) + "）：" + error);
                }
                return false;
            }
            host.Info("OpenXR 会话已建立，等待面捕数据");
            retry_count_ = 0;
            next_try_ns_ = 0;
        }

        alxr::DecodedPacket packet;
        bool exit_render_loop = false;
        if (!engine_.ReadFrame(packet, exit_render_loop)) {
            if (exit_render_loop) {
                // 运行时要求重建（头显断开/切换）：回到启动期退避重试。
                host.Warn("OpenXR 会话退出，重建中（常见原因：头显休眠或运行时切换）");
                engine_.Shutdown();
                give_up_ = false;
                retry_count_ = 0;
                next_try_ns_ = 0;
            }
            return false;
        }
        last_frame_ns_ = now_ns;

        slot.attributes |= alxr::WriteFrame(packet, table_, &slot, options_);
        if (slot.attributes == 0) {
            return false; // None/Pico 等未知来源类型，不提交空帧
        }
        if (!session_announced_) {
            session_announced_ = true;
            host.StatusActive(std::string("面捕来源 ") + alxr::ExpressionTypeName(packet.expression_type) +
                              "，眼追踪 " + alxr::EyeTrackerTypeName(packet.eye_tracker_type));
        }
        return true;
    }

    void Stop(fgb::kit::Host& host) override {
        // 上游经验：先关掉 libalxr 日志再 destroy，避免退出路径上的输出崩溃。
        engine_.Shutdown();
        host.StatusStopped("ALXR Local 已停止");
    }

private:
    alxr::AlxrEngine engine_;
    alxr::EngineConfig engine_config_{};
    fgb::kit::ShapeTable table_;
    alxr::MappingOptions options_{};
    int retry_count_ = 0;
    std::uint64_t next_try_ns_ = 0;
    std::uint64_t warn_ns_ = 0;
    std::uint64_t last_frame_ns_ = 0;
    bool give_up_ = false;
    bool session_announced_ = false;
};

} // namespace

FGB_KIT_EXPORT(AlxrLocalSource, "com.fgbbridge.alxr-local", "ALXR (OpenXR Local)",
               FGB_CAP_EXPRESSION | FGB_CAP_EYES)
