// Local 侧适配层的可离线验证部分：ABI 布局镜像 + 帧搬运。
//
// alxr_engine.dll 本体在 CI 与开发机上都不存在（厂商运行时随 Link/AirLink 等安装），
// 所以这里不 LoadLibrary 真货，只验证「没有 DLL 时的失败路径」与
// 「原生 result → DecodedPacket」的搬运逻辑；真实调用链在真机验收清单里补。

#include "alxr_abi.hpp"
#include "alxr_engine.hpp"
#include "alxr_packet.hpp"

#include "fgb/kit/testing/check.hpp"

#include <cstring>

int main() {
    // ---- ABI 镜像的尺寸/偏移由 static_assert 在编译期把关，这里跑一遍可见值 ----
    CHECK_EQ(alxr::kPacketBytes, std::size_t(342));
    CHECK_EQ(sizeof(alxr::AlxrClientCtx), std::size_t(144));
    CHECK_EQ(sizeof(alxr::AlxrSystemProperties), std::size_t(296));
    CHECK_EQ(sizeof(alxr::AlxrPosef), std::size_t(28));
    CHECK_EQ(sizeof(alxr::AlxrProcessFrameResult) % 8, std::size_t(0));

    // ---- FillFromResult：搬运 + 权重清洗 + 退化四元数不给出注视 ----
    {
        alxr::AlxrProcessFrameResult result{};
        result.expression_type = alxr::kExprFBV2;
        result.eye_tracker_type = alxr::kEyeExtGaze;
        result.gaze_pose_valid[0] = 1;
        result.gaze_pose_valid[1] = 1;
        result.weights[24] = 0.6f;
        result.weights[50] = 1.4f; // 越界 → 清洗到 1
        result.eye_gaze_pose[0].orientation[3] = 1.0f;
        result.eye_gaze_pose[1].orientation[1] = 1.0f;

        alxr::DecodedPacket packet;
        alxr::FillFromResult(result, packet);
        CHECK_EQ(packet.expression_type, alxr::kExprFBV2);
        CHECK_NEAR(packet.weights[24], 0.6f, 1e-6);
        CHECK_NEAR(packet.weights[50], 1.0f, 1e-6);
        CHECK_NEAR(packet.left_pose[3], 1.0f, 1e-6);
        CHECK_EQ(packet.gaze_pose_valid[0], std::uint8_t(1));
        // 右眼是 (0,1,0,0)，范数合法，valid 保留。
        CHECK_EQ(packet.gaze_pose_valid[1], std::uint8_t(1));

        // 全零四元数（会话刚建、姿态未同步时 libalxr 会给 0）：valid 必须被清掉。
        alxr::FillFromResult(result, packet);
        packet = alxr::DecodedPacket{};
        std::memset(&result.eye_gaze_pose[0], 0, sizeof(result.eye_gaze_pose[0]));
        alxr::FillFromResult(result, packet);
        CHECK_EQ(packet.gaze_pose_valid[0], std::uint8_t(0));
        CHECK_EQ(packet.gaze_pose_valid[1], std::uint8_t(1));
    }

    // ---- 没有厂商 DLL：StartSession 立刻失败并给可操作原因，不抛异常 ----
    {
        alxr::AlxrEngine engine;
        std::string error;
        const bool loaded = engine.Load(".", error);
        if (!loaded) {
            CHECK(error.find(alxr::kEngineDllName) != std::string::npos);
            alxr::EngineConfig config;
            std::string session_error;
            CHECK(!engine.StartSession(config, session_error));
            CHECK(!engine.SessionRunning());
            alxr::DecodedPacket packet;
            bool exit_render_loop = false;
            CHECK(!engine.ReadFrame(packet, exit_render_loop));
            CHECK(!exit_render_loop);
        } else {
            // 真装了 libalxr 的开发机：允许加载成功，但会话起不起得来都不该崩。
            CHECK(engine.Loaded());
        }
    }

    return CheckSummary("alxr-engine-stub");
}
