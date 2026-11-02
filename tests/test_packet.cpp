// ALXRFacialEyePacket 线格式解析单测：字段顺序、字节序、长度/截断、NaN 清洗。
//
// 断言值全部来自一手核对的 libalxr-sharp ALXRFacialEyePacket.cs（Pack=1）与
// FBExpression2.cs 索引，见 docs/protocol.md。

#include "alxr_packet.hpp"

#include "fgb/kit/testing/check.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

// 按线格式手摆一个 342 字节包：头部 6 字节 + 70 个 LE float + 双眼各 7 float。
std::vector<std::uint8_t> BuildPacket(std::uint8_t expression_type, std::uint8_t eye_type,
                                      std::uint8_t pose_valid_l, std::uint8_t pose_valid_r,
                                      const float* weights = nullptr,
                                      const float* left_pose = nullptr,
                                      const float* right_pose = nullptr) {
    std::vector<std::uint8_t> bytes(alxr::kPacketBytes, 0);
    bytes[0] = expression_type;
    bytes[1] = eye_type;
    bytes[2] = 1; // isEyeFollowingBlendshapesValid
    bytes[3] = pose_valid_l;
    bytes[4] = pose_valid_r;
    bytes[5] = 0; // VisualSource
    for (std::size_t i = 0; i < alxr::kMaxExpressionCount; ++i) {
        const float value = weights != nullptr ? weights[i] : 0.0f;
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        bytes[6 + 4 * i + 0] = static_cast<std::uint8_t>(bits & 0xffu);
        bytes[6 + 4 * i + 1] = static_cast<std::uint8_t>((bits >> 8) & 0xffu);
        bytes[6 + 4 * i + 2] = static_cast<std::uint8_t>((bits >> 16) & 0xffu);
        bytes[6 + 4 * i + 3] = static_cast<std::uint8_t>((bits >> 24) & 0xffu);
    }
    const float* pose_sources[2] = {left_pose, right_pose};
    for (int eye = 0; eye < 2; ++eye) {
        if (pose_sources[eye] == nullptr) {
            continue;
        }
        for (int i = 0; i < 7; ++i) {
            const float value = pose_sources[eye][i];
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            const std::size_t base = 286 + static_cast<std::size_t>(eye) * 28 + 4 * i;
            bytes[base + 0] = static_cast<std::uint8_t>(bits & 0xffu);
            bytes[base + 1] = static_cast<std::uint8_t>((bits >> 8) & 0xffu);
            bytes[base + 2] = static_cast<std::uint8_t>((bits >> 16) & 0xffu);
            bytes[base + 3] = static_cast<std::uint8_t>((bits >> 24) & 0xffu);
        }
    }
    return bytes;
}

constexpr float kIdentity[7] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f};

} // namespace

int main() {
    // ---- 头部字段顺序：0..5 必须逐个对上 ----
    {
        // 恒等四元数：valid 字节应原样透传（退化四元数会被 Parse 清成 0，见后面用例）。
        auto bytes = BuildPacket(alxr::kExprFBV2, alxr::kEyeExtGaze, 1, 1, nullptr, kIdentity,
                                 kIdentity);
        bytes[2] = 1;
        bytes[5] = 1; // AudioSource
        alxr::DecodedPacket packet;
        std::string error;
        CHECK(alxr::ParsePacket(bytes.data(), bytes.size(), packet, error));
        CHECK_EQ(packet.expression_type, alxr::kExprFBV2);
        CHECK_EQ(packet.eye_tracker_type, alxr::kEyeExtGaze);
        CHECK_EQ(packet.gaze_blendshapes_valid, std::uint8_t(1));
        CHECK_EQ(packet.gaze_pose_valid[0], std::uint8_t(1));
        CHECK_EQ(packet.gaze_pose_valid[1], std::uint8_t(1));
        CHECK_EQ(packet.expression_data_source, std::uint8_t(1));
    }

    // ---- float 小端读取 + FB2 索引对位 ----
    {
        float weights[alxr::kMaxExpressionCount] = {};
        weights[24] = 0.75f; // Jaw_Drop
        weights[50] = 0.31f; // Lips_Toward
        weights[68] = 0.5f;  // Tongue_Out（FB_V2）
        auto bytes = BuildPacket(alxr::kExprFBV2, alxr::kEyeNone, 0, 0, weights);
        alxr::DecodedPacket packet;
        std::string error;
        CHECK(alxr::ParsePacket(bytes.data(), bytes.size(), packet, error));
        CHECK_NEAR(packet.weights[24], 0.75f, 1e-6);
        CHECK_NEAR(packet.weights[50], 0.31f, 1e-6);
        CHECK_NEAR(packet.weights[68], 0.5f, 1e-6);
    }

    // ---- 双眼 pose 偏移：左 286、右 314 ----
    {
        auto bytes = BuildPacket(alxr::kExprFBV2, alxr::kEyeExtGaze, 1, 1);
        // 手写一个「绕 Y 轴 180°」四元数 (0,1,0,0) 到左眼，位置 (1,2,3)。
        const float left[7] = {0.f, 1.f, 0.f, 0.f, 1.f, 2.f, 3.f};
        const float right[7] = {0.f, 0.f, 0.f, 1.f, -4.f, 0.f, 0.f};
        for (int i = 0; i < 7; ++i) {
            std::memcpy(bytes.data() + 286 + 4 * i, &left[i], sizeof(float));
            std::memcpy(bytes.data() + 314 + 4 * i, &right[i], sizeof(float));
        }
        alxr::DecodedPacket packet;
        std::string error;
        CHECK(alxr::ParsePacket(bytes.data(), bytes.size(), packet, error));
        CHECK_NEAR(packet.left_pose[1], 1.0f, 1e-6);
        CHECK_NEAR(packet.left_pose[5], 2.0f, 1e-6);
        CHECK_NEAR(packet.right_pose[3], 1.0f, 1e-6);
        CHECK_NEAR(packet.right_pose[4], -4.0f, 1e-6);
    }

    // ---- 长度/截断 ----
    {
        auto bytes = BuildPacket(alxr::kExprFBV2, alxr::kEyeExtGaze, 1, 1, nullptr, kIdentity,
                                 kIdentity);
        alxr::DecodedPacket packet;
        std::string error;
        bytes.resize(alxr::kPacketBytes - 1); // 341 字节截断
        CHECK(!alxr::ParsePacket(bytes.data(), bytes.size(), packet, error));
        CHECK(error.find("长度不足") != std::string::npos);
        CHECK(!alxr::ParsePacket(nullptr, 0, packet, error));
        CHECK(!alxr::ParsePacket(bytes.data(), 0, packet, error));
    }

    // ---- 多余尾巴：只消费前 342 字节，后面的属于下一包 ----
    {
        auto bytes = BuildPacket(alxr::kExprFB, alxr::kEyeNone, 0, 0);
        bytes.push_back(0xff);
        bytes.push_back(0x7f);
        alxr::DecodedPacket packet;
        std::string error;
        CHECK(alxr::ParsePacket(bytes.data(), bytes.size(), packet, error));
        CHECK_EQ(packet.expression_type, alxr::kExprFB);
    }

    // ---- NaN/Inf/越界清洗 + valid 标志透传 ----
    {
        float weights[alxr::kMaxExpressionCount] = {};
        const float nan_value = std::numeric_limits<float>::quiet_NaN();
        const float inf_value = std::numeric_limits<float>::infinity();
        weights[12] = nan_value; // Eyes_Closed_L → 0
        weights[13] = inf_value; // Eyes_Closed_R → 0
        weights[24] = 2.5f;      // 超上界 → 1
        weights[25] = -3.0f;     // 超下界 → 0
        const float nan_pose[7] = {nan_value, 0.f, 0.f, inf_value, nan_value, nan_value, nan_value};
        auto bytes = BuildPacket(alxr::kExprFBV2, alxr::kEyeExtGaze, 1, 1, weights, nan_pose,
                                 kIdentity);
        alxr::DecodedPacket packet;
        std::string error;
        CHECK(alxr::ParsePacket(bytes.data(), bytes.size(), packet, error));
        CHECK_NEAR(packet.weights[12], 0.0f, 0.0f);
        CHECK_NEAR(packet.weights[13], 0.0f, 0.0f);
        CHECK_NEAR(packet.weights[24], 1.0f, 0.0f);
        CHECK_NEAR(packet.weights[25], 0.0f, 0.0f);
        // 全 NaN/Inf 四元数被清洗成零范数 → Parse 把 valid 清掉（不伪造注视）。
        CHECK_EQ(packet.gaze_pose_valid[0], std::uint8_t(0));
        // 有效四元数与 valid 字节原样保留。
        CHECK_EQ(packet.gaze_pose_valid[1], std::uint8_t(1));
        CHECK_NEAR(packet.right_pose[3], 1.0f, 1e-6);
    }

    // ---- 上游不读 isEyeFollowingBlendshapesValid：这里只透传，不做任何映射 ----
    {
        auto bytes = BuildPacket(alxr::kExprFBV2, alxr::kEyeNone, 0, 0);
        bytes[2] = 7; // 任意值
        alxr::DecodedPacket packet;
        std::string error;
        CHECK(alxr::ParsePacket(bytes.data(), bytes.size(), packet, error));
        CHECK_EQ(packet.gaze_blendshapes_valid, std::uint8_t(7));
    }

    // ---- 名字表给日志用，越界值不许崩 ----
    CHECK(std::string(alxr::ExpressionTypeName(alxr::kExprFBV2)) == "FB_V2");
    CHECK(std::string(alxr::ExpressionTypeName(200)) == "Unknown");
    CHECK(std::string(alxr::EyeTrackerTypeName(alxr::kEyeExtGaze)) == "ExtEyeGazeInteraction");

    return CheckSummary("alxr-packet");
}
