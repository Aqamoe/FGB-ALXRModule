#include "alxr_mapper.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace alxr {
namespace {

// 一条「权重下标 → Unified 形状名」规则；family 位决定它在哪些来源下生效。
constexpr std::uint8_t kFbFamily = 1u << 0; // FB / FB_V2（0..62 语义相同）
constexpr std::uint8_t kAndroidFamily = 1u << 1;
constexpr std::uint8_t kFbV2Only = 1u << 2;
constexpr std::uint8_t kHtcFamily = 1u << 3;

struct WeightRule {
    std::uint8_t index;
    std::string_view shape;
    std::uint8_t families;
};

// FB（XR_FB_face_tracking / _2）0..62：逐项照抄 ModuleUtils.cs 的
// UpdateEyeExpressionsFBV2 / UpdateMouthExpressionsFBV2（adjerry 对照表，见上游注释链接）。
constexpr WeightRule kRules[] = {
    {0, "BrowLowererLeft", kFbFamily | kAndroidFamily},
    {0, "BrowPinchLeft", kFbFamily | kAndroidFamily},
    {1, "BrowLowererRight", kFbFamily | kAndroidFamily},
    {1, "BrowPinchRight", kFbFamily | kAndroidFamily},
    {2, "CheekPuffLeft", kFbFamily | kAndroidFamily},
    {3, "CheekPuffRight", kFbFamily | kAndroidFamily},
    {4, "CheekSquintLeft", kFbFamily | kAndroidFamily},
    {5, "CheekSquintRight", kFbFamily | kAndroidFamily},
    {6, "CheekSuckLeft", kFbFamily | kAndroidFamily},
    {7, "CheekSuckRight", kFbFamily | kAndroidFamily},
    {8, "MouthRaiserLower", kFbFamily | kAndroidFamily},   // Chin_Raiser_B
    {9, "MouthRaiserUpper", kFbFamily | kAndroidFamily},   // Chin_Raiser_T
    {10, "MouthDimpleLeft", kFbFamily | kAndroidFamily},
    {11, "MouthDimpleRight", kFbFamily | kAndroidFamily},
    // 12/13 Eyes_Closed、28/29 Lid_Tightener、59/60 Upper_Lid_Raiser 走眼部属性，
    // 22..27 眉毛/下颌、见下。
    {22, "BrowInnerUpLeft", kFbFamily | kAndroidFamily},
    {23, "BrowInnerUpRight", kFbFamily | kAndroidFamily},
    {24, "JawOpen", kFbFamily | kAndroidFamily},           // Jaw_Drop
    {25, "JawLeft", kFbFamily | kAndroidFamily},           // Jaw_Sideways_Left
    {26, "JawRight", kFbFamily | kAndroidFamily},          // Jaw_Sideways_Right
    {27, "JawForward", kFbFamily | kAndroidFamily},        // Jaw_Thrust
    {28, "EyeSquintLeft", kFbFamily | kAndroidFamily},     // Lid_Tightener_L
    {29, "EyeSquintRight", kFbFamily | kAndroidFamily},
    {30, "MouthFrownLeft", kFbFamily | kAndroidFamily},    // Lip_Corner_Depressor_L
    {31, "MouthFrownRight", kFbFamily | kAndroidFamily},
    {32, "MouthCornerPullLeft", kFbFamily | kAndroidFamily},
    {32, "MouthCornerSlantLeft", kFbFamily | kAndroidFamily},
    {33, "MouthCornerPullRight", kFbFamily | kAndroidFamily},
    {33, "MouthCornerSlantRight", kFbFamily | kAndroidFamily},
    {34, "LipFunnelLowerLeft", kFbFamily | kAndroidFamily},  // Lip_Funneler_LB
    {35, "LipFunnelUpperLeft", kFbFamily | kAndroidFamily},  // Lip_Funneler_LT
    {36, "LipFunnelLowerRight", kFbFamily | kAndroidFamily}, // Lip_Funneler_RB
    {37, "LipFunnelUpperRight", kFbFamily | kAndroidFamily}, // Lip_Funneler_RT
    {38, "MouthPressLeft", kFbFamily | kAndroidFamily},
    {39, "MouthPressRight", kFbFamily | kAndroidFamily},
    {40, "LipPuckerUpperLeft", kFbFamily | kAndroidFamily},
    {40, "LipPuckerLowerLeft", kFbFamily | kAndroidFamily},
    {41, "LipPuckerUpperRight", kFbFamily | kAndroidFamily},
    {41, "LipPuckerLowerRight", kFbFamily | kAndroidFamily},
    {42, "MouthStretchLeft", kFbFamily | kAndroidFamily},
    {43, "MouthStretchRight", kFbFamily | kAndroidFamily},
    {44, "LipSuckLowerLeft", kFbFamily | kAndroidFamily},  // Lip_Suck_LB
    {45, "LipSuckUpperLeft", kFbFamily | kAndroidFamily},  // Lip_Suck_LT
    {46, "LipSuckLowerRight", kFbFamily | kAndroidFamily}, // Lip_Suck_RB
    {47, "LipSuckUpperRight", kFbFamily | kAndroidFamily}, // Lip_Suck_RT
    {48, "MouthTightenerLeft", kFbFamily | kAndroidFamily},
    {49, "MouthTightenerRight", kFbFamily | kAndroidFamily},
    {50, "MouthClosed", kFbFamily | kAndroidFamily},       // Lips_Toward
    {51, "MouthLowerDownLeft", kFbFamily | kAndroidFamily},
    {52, "MouthLowerDownRight", kFbFamily | kAndroidFamily},
    {53, "MouthUpperLeft", kFbFamily | kAndroidFamily},    // Mouth_Left
    {53, "MouthLowerLeft", kFbFamily | kAndroidFamily},
    {54, "MouthUpperRight", kFbFamily | kAndroidFamily},   // Mouth_Right
    {54, "MouthLowerRight", kFbFamily | kAndroidFamily},
    {55, "NoseSneerLeft", kFbFamily | kAndroidFamily},     // Nose_Wrinkler_L
    {56, "NoseSneerRight", kFbFamily | kAndroidFamily},
    {57, "BrowOuterUpLeft", kFbFamily | kAndroidFamily},   // Outer_Brow_Raiser_L
    {58, "BrowOuterUpRight", kFbFamily | kAndroidFamily},
    {59, "EyeWideLeft", kFbFamily | kAndroidFamily},       // Upper_Lid_Raiser_L
    {60, "EyeWideRight", kFbFamily | kAndroidFamily},
    // 上游把 MouthUpperUp 与 NoseSneer 取 max 后同时喂 UpperUp/UpperDeepen
    // （「tracking quirk」workaround）；这里保留同一形状对，但不掺 nose 权重——
    // Nose_Wrinkler 已经单独映射到 NoseSneer，简化掉该 workaround 并在文档标注。
    {61, "MouthUpperUpLeft", kFbFamily | kAndroidFamily},
    {61, "MouthUpperDeepenLeft", kFbFamily | kAndroidFamily},
    {62, "MouthUpperUpRight", kFbFamily | kAndroidFamily},
    {62, "MouthUpperDeepenRight", kFbFamily | kAndroidFamily},

    // —— FB_V2 独有（63..69）——
    {64, "TongueCurlUp", kFbV2Only}, // Tongue_Tip_alveolar
    {68, "TongueOut", kFbV2Only},

    // —— Android（XR_ANDROID_face_tracking，63..67 是舌头五向）——
    {63, "TongueOut", kAndroidFamily},
    {64, "TongueLeft", kAndroidFamily},
    {65, "TongueRight", kAndroidFamily},
    {66, "TongueUp", kAndroidFamily},
    {67, "TongueDown", kAndroidFamily},

    // —— HTC（XrEyeExpression / XrLipExpression，唇部分从 14 开始）——
    // 索引写成「绝对下标」，与包里的权重数组对齐：0..13 是眼，14 起是唇。
    {1, "EyeWideLeft", kHtcFamily},
    {3, "EyeWideRight", kHtcFamily},
    {4, "EyeSquintLeft", kHtcFamily},   // LeftSqueeze
    {5, "EyeSquintRight", kHtcFamily},
    // 上游用睁眼/挤眼近似眉毛四组（SRanipal 无眉毛通道），保留该近似：
    {1, "BrowInnerUpLeft", kHtcFamily},
    {1, "BrowOuterUpLeft", kHtcFamily},
    {3, "BrowInnerUpRight", kHtcFamily},
    {3, "BrowOuterUpRight", kHtcFamily},
    {4, "BrowPinchLeft", kHtcFamily},
    {4, "BrowLowererLeft", kHtcFamily},
    {5, "BrowPinchRight", kHtcFamily},
    {5, "BrowLowererRight", kHtcFamily},

    {14 + 0, "JawRight", kHtcFamily},                    // JawRight
    {14 + 1, "JawLeft", kHtcFamily},
    {14 + 2, "JawForward", kHtcFamily},
    {14 + 3, "JawOpen", kHtcFamily},                     // JawOpen
    {14 + 4, "MouthClosed", kHtcFamily},                 // MouthApeShape
    {14 + 4, "JawOpen", kHtcFamily},                     // 上游：JawOpen = JawOpen + ApeShape
    {14 + 5, "MouthUpperRight", kHtcFamily},
    {14 + 6, "MouthUpperLeft", kHtcFamily},
    {14 + 7, "MouthLowerRight", kHtcFamily},
    {14 + 8, "MouthLowerLeft", kHtcFamily},
    {14 + 9, "LipFunnelUpperLeft", kHtcFamily},          // MouthUpperOverturn
    {14 + 9, "LipFunnelUpperRight", kHtcFamily},
    {14 + 10, "LipFunnelLowerLeft", kHtcFamily},         // MouthLowerOverturn
    {14 + 10, "LipFunnelLowerRight", kHtcFamily},
    {14 + 11, "LipPuckerUpperLeft", kHtcFamily},         // MouthPout
    {14 + 11, "LipPuckerLowerLeft", kHtcFamily},
    {14 + 11, "LipPuckerUpperRight", kHtcFamily},
    {14 + 11, "LipPuckerLowerRight", kHtcFamily},
    {14 + 12, "MouthCornerPullRight", kHtcFamily},       // MouthSmileRight
    {14 + 12, "MouthCornerSlantRight", kHtcFamily},
    {14 + 12, "CheekSquintRight", kHtcFamily},           // 上游近似项
    {14 + 12, "MouthDimpleRight", kHtcFamily},
    {14 + 13, "MouthCornerPullLeft", kHtcFamily},        // MouthSmileLeft
    {14 + 13, "MouthCornerSlantLeft", kHtcFamily},
    {14 + 13, "CheekSquintLeft", kHtcFamily},
    {14 + 13, "MouthDimpleLeft", kHtcFamily},
    {14 + 14, "MouthFrownRight", kHtcFamily},            // MouthSadRight
    {14 + 14, "MouthStretchRight", kHtcFamily},          // 上游近似项（也用于左侧，取同值）
    {14 + 15, "MouthFrownLeft", kHtcFamily},             // MouthSadLeft
    {14 + 15, "MouthStretchLeft", kHtcFamily},
    {14 + 16, "CheekPuffRight", kHtcFamily},
    {14 + 17, "CheekPuffLeft", kHtcFamily},
    {14 + 18, "CheekSuckLeft", kHtcFamily},              // CheekSuck（双侧同值）
    {14 + 18, "CheekSuckRight", kHtcFamily},
    {14 + 19, "MouthUpperUpRight", kHtcFamily},          // MouthUpperUpRight
    {14 + 19, "MouthUpperDeepenRight", kHtcFamily},
    {14 + 20, "MouthUpperUpLeft", kHtcFamily},
    {14 + 20, "MouthUpperDeepenLeft", kHtcFamily},
    {14 + 21, "MouthLowerDownRight", kHtcFamily},
    {14 + 22, "MouthLowerDownLeft", kHtcFamily},
    {14 + 23, "LipSuckUpperLeft", kHtcFamily},           // MouthUpperInside
    {14 + 23, "LipSuckUpperRight", kHtcFamily},
    {14 + 24, "LipSuckLowerLeft", kHtcFamily},           // MouthLowerInside
    {14 + 24, "LipSuckLowerRight", kHtcFamily},
    {14 + 25, "MouthRaiserLower", kHtcFamily},           // MouthLowerOverlay
    {14 + 26, "TongueOut", kHtcFamily},                  // TongueLongStep1
    {14 + 32, "TongueOut", kHtcFamily},                  // TongueLongStep2（上游取两者均值）
    {14 + 27, "TongueLeft", kHtcFamily},
    {14 + 28, "TongueRight", kHtcFamily},
    {14 + 29, "TongueUp", kHtcFamily},
    {14 + 30, "TongueDown", kHtcFamily},
    {14 + 31, "TongueRoll", kHtcFamily},
};

std::uint8_t FamilyMask(std::uint8_t expression_type) {
    switch (expression_type) {
        case kExprFB: return kFbFamily;
        case kExprFBV2: return kFbFamily | kFbV2Only;
        case kExprHTC: return kHtcFamily;
        case kExprAndroid: return kAndroidFamily;
        default: return 0;
    }
}

float Clamp01(float value) {
    return fgb::kit::Clamp01(value);
}

// FB 系睁眼度（ModuleUtils.cs UpdateEyeOpenessFBV2 的四种模式，默认 Linear）。
// closed=12/13、lid=28/29、down=14/15、widen=59/60。
float FbOpenness(const DecodedPacket& p, OpennessMode mode, int closed, int lid, int down,
                 int widen) {
    const float c = p.weights[closed];
    const float t = p.weights[lid];
    switch (mode) {
        case OpennessMode::NonLinear:
            return Clamp01(1.0f - Clamp01(c + c * (2.0f * t / std::pow(2.0f, 2.0f * t))));
        case OpennessMode::Smooth: {
            const float eye_closed = std::min(1.0f, c + t * 0.5f);
            const float s = 1.0f / (1.0f + std::exp(-5.0f * ((1.0f - eye_closed) - 0.5f)));
            return std::min(1.0f, s);
        }
        case OpennessMode::Multi: {
            float eye_closed = std::min(1.0f, c + p.weights[down] * 0.5f);
            eye_closed = std::min(1.0f, eye_closed + t * 0.5f);
            const float openness = Clamp01(1.1f - eye_closed);
            const float eye_widen = std::max(0.0f, p.weights[widen] - 3.0f * (1.0f - openness));
            return std::min(1.0f, openness + eye_widen);
        }
        case OpennessMode::Linear:
        default:
            return Clamp01(1.0f - Clamp01(c + c * t));
    }
}

// 四元数把 (0,0,-1) 旋过去 = HMD 局部注视单位向量（-Z 前，与 ABI 注释一致）。
// 上游把姿态折成 pitch/yaw 两个角（NormalizedGaze）；FGB 的帧槽要求单位向量，
// 这里直接旋转前向向量，等价且免去二次近似。
void GazeDirection(const float (&pose)[7], float (&dir)[3]) {
    const float x = pose[0];
    const float y = pose[1];
    const float z = pose[2];
    const float w = pose[3];
    dir[0] = -2.0f * (w * y + x * z);
    dir[1] = 2.0f * (w * x - y * z);
    dir[2] = -1.0f + 2.0f * (x * x + y * y);
    const float norm = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (norm > 1e-6f) {
        dir[0] /= norm;
        dir[1] /= norm;
        dir[2] /= norm;
    } else {
        dir[0] = 0.0f;
        dir[1] = 0.0f;
        dir[2] = -1.0f;
    }
}

} // namespace

bool ParseOpennessMode(const std::string& text, OpennessMode& out) {
    const std::string_view key{text};
    if (key == "linear") {
        out = OpennessMode::Linear;
    } else if (key == "nonlinear") {
        out = OpennessMode::NonLinear;
    } else if (key == "smooth") {
        out = OpennessMode::Smooth;
    } else if (key == "multi") {
        out = OpennessMode::Multi;
    } else {
        return false;
    }
    return true;
}

void DeclareTargets(fgb::kit::ShapeTable& table) {
    for (const auto& rule : kRules) {
        table.Declare(rule.shape);
    }
}

std::uint64_t WriteFrame(const DecodedPacket& packet, const fgb::kit::ShapeTable& table,
                         FgbFrameSlot* slot, const MappingOptions& options) {
    if (slot == nullptr) {
        return 0;
    }
    const std::uint8_t families = FamilyMask(packet.expression_type);
    if (families == 0) {
        // None / Pico / Auto 落不进任何表：这一帧不产出任何属性。
        return 0;
    }

    std::uint64_t attributes = 0;
    bool wrote_shape = false;
    for (const auto& rule : kRules) {
        if ((rule.families & families) == 0) {
            continue;
        }
        if (rule.index >= kMaxExpressionCount) {
            continue;
        }
        table.Write(slot, rule.shape, packet.weights[rule.index]);
        wrote_shape = true;
    }
    if (wrote_shape) {
        slot->attributes |= FGB_ATTR_EXPRESSION;
        attributes |= FGB_ATTR_EXPRESSION;
    }

    // 双眼睁眼度：HTC 用 blink，FB 系用闭眼+睑紧公式，Android 与 FB 同布局。
    if (packet.expression_type == kExprHTC) {
        slot->left_eye.openness = Clamp01(1.0f - packet.weights[0]);  // LeftBlink
        slot->right_eye.openness = Clamp01(1.0f - packet.weights[2]); // RightBlink
        slot->left_eye.openness_confidence = options.openness_confidence;
        slot->right_eye.openness_confidence = options.openness_confidence;
        attributes |= FGB_ATTR_LEFT_OPENNESS | FGB_ATTR_RIGHT_OPENNESS;
    } else {
        slot->left_eye.openness =
            FbOpenness(packet, options.openness_mode, 12, 28, 14, 59);
        slot->right_eye.openness =
            FbOpenness(packet, options.openness_mode, 13, 29, 15, 60);
        slot->left_eye.openness_confidence = options.openness_confidence;
        slot->right_eye.openness_confidence = options.openness_confidence;
        attributes |= FGB_ATTR_LEFT_OPENNESS | FGB_ATTR_RIGHT_OPENNESS;
    }

    // 逐眼注视：只有 tracker 类型不是 None、valid 字节为真（Parse 已把退化四元数
    // 清成 0）才置位；无效帧绝不写「闭眼/看正前方」的假数据。
    if (packet.eye_tracker_type != kEyeNone) {
        if (packet.gaze_pose_valid[0] != 0) {
            GazeDirection(packet.left_pose, slot->left_eye.gaze_dir);
            slot->left_eye.gaze_confidence = options.gaze_confidence;
            attributes |= FGB_ATTR_LEFT_GAZE;
        }
        if (packet.gaze_pose_valid[1] != 0) {
            GazeDirection(packet.right_pose, slot->right_eye.gaze_dir);
            slot->right_eye.gaze_confidence = options.gaze_confidence;
            attributes |= FGB_ATTR_RIGHT_GAZE;
        }
    }
    return attributes;
}

} // namespace alxr
