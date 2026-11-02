// ALXR Remote → FaceGaze Bridge 原生模块。
//
// 主动连「串流端」（头显/盒子上的 ALXR 客户端）的 TCP 端口（默认 客户端IP:49192），
// 收定长 342 字节的 ALXRFacialEyePacket，输出 Unified Expressions 表情与
// 双眼睁眼度/注视方向。不需要 VRCFT，不注入任何进程，也不需要管理员权限。
//
// 上游参考：VRCFT-ALXR-Modules 的 ALXRRemoteModule.cs（TCP 客户端 + 直读结构体）。

#include "alxr_mapper.hpp"
#include "alxr_packet.hpp"
#include "alxr_transport.hpp"

#include "fgb/kit/config.hpp"
#include "fgb/kit/module.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kNoDataTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;
constexpr std::uint64_t kWarnIntervalNs = 5ull * 1000ull * 1000ull * 1000ull;

class AlxrRemoteSource : public fgb::kit::IModuleSource {
public:
    bool Start(fgb::kit::Host& host, const FgbInitParams& params, std::string& error) override {
        fgb::kit::ConfigIni config;
        std::string config_error;
        const std::string directory = params.module_dir != nullptr ? params.module_dir : "";
        config.AllowKeys({"ip", "port", "eye_openness"});
        if (config.Load(directory, "config.ini", config_error)) {
            ip_ = config.Get("ip", ip_);
            port_ = static_cast<std::uint16_t>(
                config.GetInt("port", static_cast<int>(port_)));
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

        transport_.SetTarget(ip_, port_);

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

        host.Info("ALXR Remote 将连接串流端 " + ip_ + ":" + std::to_string(port_) +
                  "（断线自动退避重连）");
        host.StatusStarting("等待连接串流端 " + ip_ + ":" + std::to_string(port_));
        return true;
    }

    bool Read(fgb::kit::Host& host, FgbFrameSlot& slot, std::uint64_t now_ns) override {
        std::vector<alxr::DecodedPacket> packets;
        const auto poll = transport_.Poll(now_ns, packets);
        if (poll.just_connected) {
            host.Info("已连接串流端 " + transport_.Target() + ":" +
                      std::to_string(transport_.Port()));
            host.StatusActive("串流端已连接");
        }
        if (poll.just_disconnected) {
            host.Warn("串流端断开：" + transport_.LastError() +
                      "，退避重连中（下次间隔 " +
                      std::to_string(transport_.NextRetryNs() / 1000000ull) + "ms）");
        }
        if (poll.parse_errors != 0 && poll.parse_errors != reported_errors_) {
            reported_errors_ = poll.parse_errors;
            host.WarnThrottled(warn_ns_, now_ns, kWarnIntervalNs,
                               "丢弃畸形包共 " + std::to_string(poll.parse_errors) + " 个：" +
                                   transport_.LastError());
        }

        if (packets.empty()) {
            // 没包也推进断流检查：超过 10 秒在卡片上提示原因。
            staleness_.Check(host, transport_.LastPacketNs(), now_ns);
            return false;
        }
        // TCP 队列里可能积压多包；只取最新一个有效包（旧帧不如新帧准）。
        const alxr::DecodedPacket* latest = nullptr;
        for (auto& packet : packets) {
            latest = &packet;
        }
        if (latest == nullptr) {
            return false;
        }

        if (!staleness_.Check(host, transport_.LastPacketNs(), now_ns)) {
            return false;
        }

        slot.attributes |= alxr::WriteFrame(*latest, table_, &slot, options_);
        if (slot.attributes == 0) {
            return false; // 未知来源类型：不提交空帧
        }
        return true;
    }

    void Stop(fgb::kit::Host& host) override {
        transport_.Close();
        host.StatusStopped("ALXR Remote 已停止");
    }

private:
    alxr::TcpStreamClient transport_;
    fgb::kit::ShapeTable table_;
    fgb::kit::Staleness staleness_{kNoDataTimeoutNs, "ALXR Remote"};
    alxr::MappingOptions options_{};
    std::string ip_ = "127.0.0.1";
    std::uint16_t port_ = alxr::kDefaultTrackingPort;
    std::uint64_t warn_ns_ = 0;
    std::size_t reported_errors_ = 0;
    bool first_type_logged_ = false;
};

} // namespace

FGB_KIT_EXPORT(AlxrRemoteSource, "com.fgbbridge.alxr-remote", "ALXR (OpenXR Remote)",
               FGB_CAP_EXPRESSION | FGB_CAP_EYES)
