#include "alxr_packet.hpp"

#include "fgb/kit/arkit.hpp"

#include <cmath>
#include <cstring>

namespace alxr {
namespace {

// 小端 float 读取：显式按字节组装，不依赖 memcpy 后强转的对齐假设。
float ReadFloatLe(const std::uint8_t* p) {
    std::uint32_t bits = static_cast<std::uint32_t>(p[0]) |
                         (static_cast<std::uint32_t>(p[1]) << 8) |
                         (static_cast<std::uint32_t>(p[2]) << 16) |
                         (static_cast<std::uint32_t>(p[3]) << 24);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// pose 的 7 个分量：NaN/Inf 的分量清零；四元数范数过小的视作无效并返回 false。
bool ReadPoseLe(const std::uint8_t* p, float (&pose)[7]) {
    for (int i = 0; i < 7; ++i) {
        const float value = ReadFloatLe(p + 4 * i);
        pose[i] = (std::isfinite(value) ? value : 0.0f);
    }
    const float norm = std::sqrt(pose[0] * pose[0] + pose[1] * pose[1] + pose[2] * pose[2] +
                                 pose[3] * pose[3]);
    return norm > 1e-6f;
}

} // namespace

bool ParsePacket(const std::uint8_t* data, std::size_t size, DecodedPacket& out,
                 std::string& error) {
    if (data == nullptr) {
        error = "空缓冲区";
        return false;
    }
    if (size < kPacketBytes) {
        error = "包长度不足：收到 " + std::to_string(size) + " 字节，需要 " +
                std::to_string(kPacketBytes) + " 字节";
        return false;
    }

    out = DecodedPacket{};
    out.expression_type = data[0];
    out.eye_tracker_type = data[1];
    out.gaze_blendshapes_valid = data[2];
    out.gaze_pose_valid[0] = data[3];
    out.gaze_pose_valid[1] = data[4];
    out.expression_data_source = data[5];

    // 70 个权重：NaN/Inf 归零并夹到 [0,1]（上游值域本来就是 0..1）。
    for (std::size_t i = 0; i < kMaxExpressionCount; ++i) {
        out.weights[i] = fgb::kit::CleanUnit(ReadFloatLe(data + 6 + 4 * i));
    }

    // 双眼 pose：valid 字节为真且四元数非退化，才认为这一眼的注视可用。
    for (int eye = 0; eye < 2; ++eye) {
        float pose[7] = {};
        const bool sane = ReadPoseLe(data + 286 + eye * 28, pose);
        if (!sane) {
            out.gaze_pose_valid[eye] = 0;
            continue;
        }
        float (&target)[7] = eye == 0 ? out.left_pose : out.right_pose;
        std::memcpy(target, pose, sizeof(pose));
    }
    return true;
}

const char* ExpressionTypeName(std::uint8_t type) {
    switch (type) {
        case kExprNone: return "None";
        case kExprFB: return "FB";
        case kExprFBV2: return "FB_V2";
        case kExprHTC: return "HTC";
        case kExprPico: return "Pico";
        case kExprAndroid: return "Android";
        case kExprAuto: return "Auto";
        default: return "Unknown";
    }
}

const char* EyeTrackerTypeName(std::uint8_t type) {
    switch (type) {
        case kEyeNone: return "None";
        case kEyeFBSocial: return "FBEyeTrackingSocial";
        case kEyeExtGaze: return "ExtEyeGazeInteraction";
        case kEyeAndroid: return "AndroidEyeTracking";
        case kEyeAuto: return "Auto";
        default: return "Unknown";
    }
}

void FillFromResult(const AlxrProcessFrameResult& result, DecodedPacket& out) {
    out = DecodedPacket{};
    out.expression_type = result.expression_type;
    out.eye_tracker_type = result.eye_tracker_type;
    out.gaze_blendshapes_valid = result.gaze_blendshapes_valid;
    out.gaze_pose_valid[0] = result.gaze_pose_valid[0];
    out.gaze_pose_valid[1] = result.gaze_pose_valid[1];
    out.expression_data_source = result.expression_data_source;
    std::memcpy(out.weights, result.weights, sizeof(out.weights));
    for (int eye = 0; eye < 2; ++eye) {
        const AlxrPosef& pose = result.eye_gaze_pose[eye];
        float (&target)[7] = eye == 0 ? out.left_pose : out.right_pose;
        for (int i = 0; i < 4; ++i) {
            target[i] = std::isfinite(pose.orientation[i]) ? pose.orientation[i] : 0.0f;
        }
        for (int i = 0; i < 3; ++i) {
            target[4 + i] = std::isfinite(pose.position[i]) ? pose.position[i] : 0.0f;
        }
        const float norm = std::sqrt(target[0] * target[0] + target[1] * target[1] +
                                     target[2] * target[2] + target[3] * target[3]);
        if (!(norm > 1e-6f)) {
            out.gaze_pose_valid[eye] = 0;
        }
        // 权重清洗与线格式保持一致。
        for (std::size_t i = 0; i < kMaxExpressionCount; ++i) {
            out.weights[i] = fgb::kit::CleanUnit(out.weights[i]);
        }
    }
}

} // namespace alxr
