// 70 个权重 → Unified Expressions 的映射单测。
//
// 期望值逐项对照上游 ModuleUtils.cs 的 FBExpression2/HTC/Android 表
// （docs/protocol.md「表情映射」），并验证：无效 gaze pose 不置属性位、
// NaN 权重被清洗、FB_V2 与 FB 的舌头差异。

#include "alxr_mapper.hpp"
#include "alxr_packet.hpp"

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace fgb::kit;
using namespace fgb::kit::testing;

FgbFrameSlot FreshSlot() {
    FgbFrameSlot slot{};
    slot.struct_size = sizeof(FgbFrameSlot);
    slot.abi_version = FGB_MODULE_ABI_V2;
    slot.shape_count = FGB_MAX_SHAPES;
    return slot;
}

// 造一个「解析后」的包，跳过字节层，直接写索引值（线格式在 test_packet 里测）。
alxr::DecodedPacket MakePacket(std::uint8_t expression_type, std::uint8_t eye_type) {
    alxr::DecodedPacket packet;
    packet.expression_type = expression_type;
    packet.eye_tracker_type = eye_type;
    return packet;
}

} // namespace

int main() {
    HostMock host;
    Host logger{host.services()};
    ShapeTable table;
    alxr::DeclareTargets(table);
    CHECK(table.Build(logger));
    // 表里所有目标名都必须是宿主认识的真实形状，拼错会在这里现形。
    CHECK_EQ(table.ResolvedCount(), table.Size());

    // ---- FB_V2：70 槽里抽样覆盖嘴/下颌/鼻/眼周/舌头 ----
    {
        alxr::DecodedPacket p = MakePacket(alxr::kExprFBV2, alxr::kEyeNone);
        p.weights[24] = 0.9f;  // Jaw_Drop → JawOpen
        p.weights[50] = 0.8f;  // Lips_Toward → MouthClosed
        p.weights[55] = 0.7f;  // Nose_Wrinkler_L → NoseSneerLeft（上游 Nose 组）
        p.weights[59] = 0.55f; // Upper_Lid_Raiser_L → EyeWideLeft
        p.weights[28] = 0.5f;  // Lid_Tightener_L → EyeSquintLeft
        p.weights[22] = 0.4f;  // Inner_Brow_Raiser_L → BrowInnerUpLeft
        p.weights[61] = 0.3f;  // Upper_Lip_Raiser_L → MouthUpperUpLeft(+Deepen)
        p.weights[68] = 0.2f;  // Tongue_Out（FB_V2 专有）
        p.weights[64] = 0.4f;  // Tongue_Tip_alveolar → TongueCurlUp

        FgbFrameSlot slot = FreshSlot();
        const std::uint64_t attrs = alxr::WriteFrame(p, table, &slot);
        slot.attributes |= attrs;
        CHECK((attrs & FGB_ATTR_EXPRESSION) != 0);
        CHECK_NEAR(slot.shapes[host.IndexOf("JawOpen")], 0.9f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("MouthClosed")], 0.8f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("NoseSneerLeft")], 0.7f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("EyeWideLeft")], 0.55f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("EyeSquintLeft")], 0.5f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("BrowInnerUpLeft")], 0.4f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("MouthUpperUpLeft")], 0.3f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("MouthUpperDeepenLeft")], 0.3f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("TongueOut")], 0.2f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("TongueCurlUp")], 0.4f, 1e-6);

        // 睁眼度：Linear 模式，closed=12、lid=28 → 1 - (0.2 + 0.2*0.5)
        p.weights[12] = 0.2f;
        alxr::DecodedPacket eye = p;
        FgbFrameSlot slot2 = FreshSlot();
        const std::uint64_t attrs2 = alxr::WriteFrame(eye, table, &slot2);
        CHECK((attrs2 & FGB_ATTR_LEFT_OPENNESS) != 0);
        CHECK((attrs2 & FGB_ATTR_RIGHT_OPENNESS) != 0);
        CHECK_NEAR(slot2.left_eye.openness, 1.0f - 0.3f, 1e-6);
        CHECK_NEAR(slot2.right_eye.openness, 1.0f, 1e-6);
    }

    // ---- FB（v1）：69/68 的 FB_V2 专有舌头不生效 ----
    {
        alxr::DecodedPacket p = MakePacket(alxr::kExprFB, alxr::kEyeNone);
        p.weights[68] = 0.2f;
        p.weights[64] = 0.4f;
        FgbFrameSlot slot = FreshSlot();
        alxr::WriteFrame(p, table, &slot);
        CHECK(slot.shape_valid[host.IndexOf("TongueOut")] == 0);
        CHECK(slot.shape_valid[host.IndexOf("TongueCurlUp")] == 0);
    }

    // ---- Android：63..67 是舌头五向 ----
    {
        alxr::DecodedPacket p = MakePacket(alxr::kExprAndroid, alxr::kEyeNone);
        p.weights[63] = 0.9f; // Tongue_Out
        p.weights[66] = 0.5f; // Tongue_Up
        p.weights[24] = 0.4f; // Jaw_Drop → JawOpen（索引语义与 FB 相同，已核对）
        FgbFrameSlot slot = FreshSlot();
        alxr::WriteFrame(p, table, &slot);
        CHECK_NEAR(slot.shapes[host.IndexOf("TongueOut")], 0.9f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("TongueUp")], 0.5f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("JawOpen")], 0.4f, 1e-6);
    }

    // ---- HTC：blink 睁眼 + 唇部切片偏移 14 ----
    {
        alxr::DecodedPacket p = MakePacket(alxr::kExprHTC, alxr::kEyeNone);
        p.weights[0] = 0.75f;  // LeftBlink
        p.weights[2] = 0.4f;   // RightBlink
        p.weights[1] = 0.6f;   // LeftWide → EyeWideLeft
        p.weights[17] = 0.9f;  // 14+3 JawOpen
        p.weights[18] = 0.3f;  // 14+4 MouthApeShape → MouthClosed，并参与 JawOpen（上游是加法，
                               // 这里取 max，差值在 protocol.md 标注）
        FgbFrameSlot slot = FreshSlot();
        const std::uint64_t attrs = alxr::WriteFrame(p, table, &slot);
        CHECK((attrs & FGB_ATTR_LEFT_OPENNESS) != 0);
        CHECK_NEAR(slot.left_eye.openness, 0.25f, 1e-6);
        CHECK_NEAR(slot.right_eye.openness, 0.6f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("EyeWideLeft")], 0.6f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("JawOpen")], 0.9f, 1e-6);
        CHECK_NEAR(slot.shapes[host.IndexOf("MouthClosed")], 0.3f, 1e-6);
    }

    // ---- gaze pose：valid 才置位；恒等四元数 = 正前 (0,0,-1) ----
    {
        alxr::DecodedPacket p = MakePacket(alxr::kExprFBV2, alxr::kEyeExtGaze);
        p.gaze_pose_valid[0] = 1;
        p.gaze_pose_valid[1] = 1;
        p.left_pose[3] = 1.0f; // w=1 恒等
        p.right_pose[1] = 1.0f; // (0,1,0,0)：绕 Y 轴 180° → 正后 (0,0,1)
        p.right_pose[3] = 0.0f;
        FgbFrameSlot slot = FreshSlot();
        const std::uint64_t attrs = alxr::WriteFrame(p, table, &slot);
        CHECK((attrs & FGB_ATTR_LEFT_GAZE) != 0);
        CHECK((attrs & FGB_ATTR_RIGHT_GAZE) != 0);
        CHECK_NEAR(slot.left_eye.gaze_dir[2], -1.0f, 1e-5);
        CHECK_NEAR(slot.right_eye.gaze_dir[2], 1.0f, 1e-5);
        const float norm = std::sqrt(slot.left_eye.gaze_dir[0] * slot.left_eye.gaze_dir[0] +
                                     slot.left_eye.gaze_dir[1] * slot.left_eye.gaze_dir[1] +
                                     slot.left_eye.gaze_dir[2] * slot.left_eye.gaze_dir[2]);
        CHECK_NEAR(norm, 1.0f, 1e-5);
        CHECK_NEAR(slot.left_eye.gaze_confidence, 0.8f, 1e-6);
    }

    // ---- 无效帧不置属性位：gaze valid=0 / tracker=None 都不写注视 ----
    {
        alxr::DecodedPacket p = MakePacket(alxr::kExprFBV2, alxr::kEyeExtGaze);
        p.gaze_pose_valid[0] = 0;
        p.gaze_pose_valid[1] = 0;
        FgbFrameSlot slot = FreshSlot();
        const std::uint64_t attrs = alxr::WriteFrame(p, table, &slot);
        CHECK((attrs & FGB_ATTR_LEFT_GAZE) == 0);
        CHECK((attrs & FGB_ATTR_RIGHT_GAZE) == 0);

        alxr::DecodedPacket none = MakePacket(alxr::kExprFBV2, alxr::kEyeNone);
        none.gaze_pose_valid[0] = 1;
        none.left_pose[3] = 1.0f;
        FgbFrameSlot slot2 = FreshSlot();
        const std::uint64_t attrs2 = alxr::WriteFrame(none, table, &slot2);
        CHECK((attrs2 & FGB_ATTR_LEFT_GAZE) == 0);
    }

    // ---- None / Pico / Auto：整帧不产出 ----
    {
        alxr::DecodedPacket p = MakePacket(alxr::kExprNone, alxr::kEyeNone);
        FgbFrameSlot slot = FreshSlot();
        CHECK_EQ(alxr::WriteFrame(p, table, &slot), std::uint64_t(0));
        p.expression_type = alxr::kExprPico;
        CHECK_EQ(alxr::WriteFrame(p, table, &slot), std::uint64_t(0));
    }

    // ---- NaN 权重经完整线格式路径（ParsePacket→WriteFrame）清洗为 0 ----
    {
        std::vector<std::uint8_t> bytes(alxr::kPacketBytes, 0);
        bytes[0] = alxr::kExprFBV2;
        const float nan_value = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(bytes.data() + 6 + 4 * 24, &nan_value, sizeof(nan_value));
        alxr::DecodedPacket p;
        std::string error;
        CHECK(alxr::ParsePacket(bytes.data(), bytes.size(), p, error));
        CHECK(std::isfinite(p.weights[24]));
        FgbFrameSlot slot = FreshSlot();
        alxr::WriteFrame(p, table, &slot);
        CHECK_NEAR(slot.shapes[host.IndexOf("JawOpen")], 0.0f, 1e-6);
    }

    // ---- 睁眼度模式：Smooth 的闭眼 0.5 → sigmoid(0) = 0.5 ----
    {
        alxr::DecodedPacket p = MakePacket(alxr::kExprFBV2, alxr::kEyeNone);
        p.weights[12] = 0.5f;
        p.weights[28] = 0.0f;
        alxr::MappingOptions options;
        options.openness_mode = alxr::OpennessMode::Smooth;
        FgbFrameSlot slot = FreshSlot();
        alxr::WriteFrame(p, table, &slot, options);
        CHECK_NEAR(slot.left_eye.openness, 0.5f, 1e-3);
    }

    alxr::OpennessMode mode = alxr::OpennessMode::Smooth;
    CHECK(alxr::ParseOpennessMode("linear", mode));
    CHECK(mode == alxr::OpennessMode::Linear);
    CHECK(!alxr::ParseOpennessMode("nope", mode));

    return CheckSummary("alxr-mapper");
}
