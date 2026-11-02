#pragma once
// ALXRFacialEyePacket 的线格式解析（342 字节，TCP 流里一包到底）。
//
// 布局依据 libalxr-sharp ALXRFacialEyePacket.cs 的 [StructLayout(Pack=1)]：
//   0: expressionType(u8)  1: eyeTrackerType(u8)  2: isEyeFollowingBlendshapesValid(u8)
//   3: isEyeGazePoseValid[2]  5: expressionDataSource(u8)
//   6: expressionWeights[70] (float 小端) 286: 左眼 XrPosef 314: 右眼 XrPosef
// C# 侧是 MemoryMarshal 直接按字节重解释——没有任何字节序转换，
// 即线格式就是小端原生布局（Windows/Android 端均为小端）。

#include "alxr_abi.hpp"

#include <cstdint>
#include <string>

namespace alxr {

// 从至少 kPacketBytes 的缓冲区解析一包。
// 返回 false 表示长度不足（error 带原因）；字段级异常（NaN/Inf）不报错，
// 而是在清洗时归零或把对应 valid 位视作无效。
bool ParsePacket(const std::uint8_t* data, std::size_t size, DecodedPacket& out,
                 std::string& error);

// 供日志用的可读名。
const char* ExpressionTypeName(std::uint8_t type);
const char* EyeTrackerTypeName(std::uint8_t type);

} // namespace alxr
