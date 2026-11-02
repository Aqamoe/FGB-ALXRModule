#pragma once
// ALXRFacialEyePacket → FaceGaze Bridge Unified Expressions 的映射层。
//
// 依据上游 VRCFT-ALXR-Modules ModuleUtils.cs 的 FBExpression2 / FBExpression /
// XrEyeExpressionHTC / XrLipExpressionHTC / XrFaceParameterIndicesANDROID 索引表
// （逐项核对见 docs/protocol.md）。有意简化掉的部分（灵敏度乘子、OneEuro 平滑、
// 若干上游近似项）都在协议文档里标注，不做未经确认的臆造映射。

#include "alxr_abi.hpp"

#include "fgb/kit/host.hpp"
#include "fgb_module_api.h"

#include <cstdint>
#include <string>

namespace alxr {

// FB 系睁眼度模式（ModuleUtils.cs FBEyeOpennessMode，上游默认 Linear）。
enum class OpennessMode : std::uint8_t { Linear, NonLinear, Smooth, Multi };

struct MappingOptions {
    OpennessMode openness_mode = OpennessMode::Linear;
    // 注视来自 gaze pose（真眼动源），置信度按「近似真值」给。
    float gaze_confidence = 0.8f;
    float openness_confidence = 1.0f;
};

// 由配置字符串解析模式；无法识别时返回 false 并保留原值。
bool ParseOpennessMode(const std::string& text, OpennessMode& out);

// 登记本映射会写的全部目标形状名（两个产物共用一张表）。
void DeclareTargets(fgb::kit::ShapeTable& table);

// 写一帧：表情形状 + 双眼睁眼度/注视方向，返回置起的 attributes 位。
// 调用方负责把返回值或进 slot.attributes。
std::uint64_t WriteFrame(const DecodedPacket& packet, const fgb::kit::ShapeTable& table,
                         FgbFrameSlot* slot, const MappingOptions& options = {});

} // namespace alxr
