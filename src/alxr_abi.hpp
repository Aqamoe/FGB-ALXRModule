#pragma once
// libalxr C ABI 的最小手写声明（不引入任何厂商头文件、不链接任何导入库）。
//
// 依据：korejan/libalxr-sharp（VRCFT-ALXR-Modules v1.7.1 的子模块）里
// LibALXR.cs / ALXRTypes.cs / ALXRFacialEyePacket.cs / ALXRHandTracking.cs 的
// P/Invoke 与 [StructLayout] 定义；alxr_engine.dll 的导出符号在运行时用
// fgb::kit::Library 解析（见 src/alxr_engine.cpp）。
// 之所以手写：libalxr 的原生头不在上游仓库里（ModuleLibs/ 只有占位说明），
// 而 C# 绑定就是官方发布的对端布局描述，按同一布局镜像成 C++ 即可。
// 每个布局都在下面标注来源文件；跨编译单元用 static_assert 锁死尺寸与偏移，
// 布局一旦算错，编不过而不是跑错。

#include <cstdint>
#include <cstddef>

namespace alxr {

// ---- 线格式常量（LibALXR.cs: TrackingServerDefaultPortNo；ALXRFacialEyePacket.cs: Max*）----
inline constexpr std::uint16_t kDefaultTrackingPort = 49192;
inline constexpr std::size_t kMaxExpressionCount = 70;
// 头部 6 字节 + 70 个 float + 2 个 XrPosef（7 float）= 342 字节。
inline constexpr std::size_t kPacketBytes = 6 + 4 * kMaxExpressionCount + 2 * 7 * 4;
static_assert(kPacketBytes == 342);

// ---- 枚举（ALXRFacialEyePacket.cs）----
enum FacialExpressionType : std::uint8_t {
    kExprNone = 0,
    kExprFB = 1,
    kExprFBV2 = 2,
    kExprHTC = 3,
    kExprPico = 4, // 上游有枚举但没有任何映射实现，本模块同样不实现
    kExprAndroid = 5,
    kExprAuto = 6,
};

enum EyeTrackerType : std::uint8_t {
    kEyeNone = 0,
    kEyeFBSocial = 1,
    kEyeExtGaze = 2,
    kEyeAndroid = 3,
    kEyeAuto = 4,
};

enum FaceTrackingDataSource : std::uint8_t {
    kSourceVisual = 0,
    kSourceAudio = 1,
    kSourceUnknown = 2,
};

// ---- 解析后的报文（与内存布局解耦，字段语义见 docs/protocol.md）----
struct DecodedPacket {
    std::uint8_t expression_type = kExprNone;
    std::uint8_t eye_tracker_type = kEyeNone;
    std::uint8_t gaze_blendshapes_valid = 0; // isEyeFollowingBlendshapesValid
    std::uint8_t gaze_pose_valid[2] = {0, 0}; // isEyeGazePoseValid[左, 右]
    std::uint8_t expression_data_source = kSourceUnknown;
    float weights[kMaxExpressionCount] = {};
    // XrPosef：orientation(x,y,z,w) + position(x,y,z)，小端、按声明顺序。
    float left_pose[7] = {};
    float right_pose[7] = {};
};

// =====================================================================
// 以下为 Local 侧调用 alxr_engine.dll 需要的原生布局镜像（仅 x64/MSVC）。
// =====================================================================

#pragma pack(push, 8)

// ALXRVersion：3 个 uint32（ALXRTypes.cs ALXRVersion）
struct AlxrVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

// ALXRClientCtx（ALXRTypes.cs）：9 个函数指针 + 版本 + 图形/解码选择 + 数据路径 +
// 面捕/眼捕开关。回调字段全部指向本模块里的静态蹦床（空实现 + FNV-1a 路径哈希）。
struct AlxrClientCtx {
    void* input_send;
    void* views_config_send;
    void* path_string_to_hash;
    void* time_sync_send;
    void* video_error_report_send;
    void* battery_send;
    void* user_presence_send;
    void* set_waiting_next_idr;
    void* request_idr;

    AlxrVersion firmware_version; // (0,0,0)：上游只给 Android 客户端用
    AlxrVersion xr_api_version;   // 上游未显式赋值，保持 0

    std::uint32_t graphics_api;   // ALXRGraphicsApi：0=Auto
    std::uint32_t decoder_type;   // ALXRDecoderType：0=D3D11VA
    std::int32_t display_color_space; // ALXRColorSpace：5=Quest(Default)
    std::uint32_t passthrough_mode;   // 0=None

    void* internal_data_path;         // UTF-8 字符串，模块保活

    std::uint32_t face_tracking_data_sources; // 位标：1=Visual
    std::uint8_t facial_tracking;             // ALXRFacialExpressionType
    std::uint8_t eye_tracking;                // ALXREyeTrackingType
    std::uint16_t tracking_server_port;       // 本地 FT 推送端口（不用，保持默认）

    // 13 个 bool，顺序照抄 C# 声明（ALXRTypes.cs:358-386）。
    std::uint8_t verbose;
    std::uint8_t disable_linearize_srgb;
    std::uint8_t no_suggested_bindings;
    std::uint8_t no_server_framerate_lock;
    std::uint8_t no_frame_skip;
    std::uint8_t disable_local_dimming;
    std::uint8_t headless_session;
    std::uint8_t no_ft_server;
    std::uint8_t no_passthrough;
    std::uint8_t no_hand_tracking;
    std::uint8_t simulate_headless;
    std::uint8_t no_visibility_masks;
    std::uint8_t no_multiview_rendering;
};
// 9*8(指针) + 24(版本) + 16(4 个枚举) + 8(路径) + 4+1+1+2 + 13(bool) = 144
static_assert(sizeof(AlxrClientCtx) == 144);
static_assert(offsetof(AlxrClientCtx, internal_data_path) == 112);
static_assert(offsetof(AlxrClientCtx, face_tracking_data_sources) == 120);
static_assert(offsetof(AlxrClientCtx, verbose) == 128);

// ALXRSystemProperties（ALXRTypes.cs:236-256）。
struct AlxrSystemProperties {
    char system_name[256];
    float current_refresh_rate;
    void* refresh_rates;
    std::uint32_t refresh_rates_count;
    std::uint32_t recommended_eye_width;
    std::uint32_t recommended_eye_height;
    std::uint64_t enabled_tracking_flags;
};
static_assert(offsetof(AlxrSystemProperties, refresh_rates) == 264);
static_assert(offsetof(AlxrSystemProperties, enabled_tracking_flags) == 288);
static_assert(sizeof(AlxrSystemProperties) == 296);

// XrPosef：orientation 在前、position 在后（ALXRTypes.cs ALXRPosef）。
struct AlxrPosef {
    float orientation[4]; // x,y,z,w
    float position[3];
};
static_assert(sizeof(AlxrPosef) == 28);

// 手部跟踪块（ALXRHandTracking.cs）。本模块 no_hand_tracking=1，
// 但 process_frame2 的出参布局里它占首位，尺寸必须原样镜像。
struct AlxrHandJointLocation {
    std::uint64_t location_flags;
    AlxrPosef pose;
    float radius;
};
static_assert(sizeof(AlxrHandJointLocation) == 40);

struct AlxrHandJointVelocity {
    std::uint64_t velocity_flags;
    float linear[3];
    float angular[3];
};
static_assert(sizeof(AlxrHandJointVelocity) == 32);

struct AlxrHandJointLocations {
    AlxrHandJointLocation joints[26];
    AlxrHandJointVelocity velocities[26];
    std::uint8_t is_active; // C# U1 bool；尾字节不参与 4 字节对齐，26 的倍数会撑到 8
};
static_assert(sizeof(AlxrHandJointLocations) == 1880);

struct AlxrHandTracking {
    AlxrHandJointLocations hands[2];
};
static_assert(sizeof(AlxrHandTracking) == 3760);

// ALXRProcessFrameResult（ALXRTypes.cs:446-455）：
//   ALXRHandTracking + ALXRFacialEyePacket（自然对齐版）+ exitRenderLoop + requestRestart
struct AlxrProcessFrameResult {
    AlxrHandTracking hand_tracking;

    // —— ALXRFacialEyePacket 的展开（与线格式同序，但按自然对齐摆放）——
    std::uint8_t expression_type;
    std::uint8_t eye_tracker_type;
    std::uint8_t gaze_blendshapes_valid;
    std::uint8_t gaze_pose_valid[2];
    std::uint8_t expression_data_source;
    float weights[kMaxExpressionCount];
    AlxrPosef eye_gaze_pose[2];

    std::uint8_t exit_render_loop;
    std::uint8_t request_restart;
};
// 3760 → weights 是 4 字节对齐点（3760 % 4 == 0），头部 6 字节后正好落在 3766+2？
// 实测由 static_assert 把关，编译器决定 padding：
static_assert(offsetof(AlxrProcessFrameResult, weights) % 4 == 0);
static_assert(offsetof(AlxrProcessFrameResult, eye_gaze_pose) ==
              offsetof(AlxrProcessFrameResult, weights) + 4 * kMaxExpressionCount);
static_assert(offsetof(AlxrProcessFrameResult, exit_render_loop) >
              offsetof(AlxrProcessFrameResult, eye_gaze_pose));

#pragma pack(pop)

// ---- 函数原型（LibALXR.cs 的 DllImport；x64 上 __cdecl 即默认约定，写出以自证）----
using AlxrInitFn = bool(__cdecl*)(AlxrClientCtx* ctx, AlxrSystemProperties* properties);
using AlxrDestroyFn = void(__cdecl*)();
using AlxrProcessFrame2Fn = void(__cdecl*)(AlxrProcessFrameResult* result);
using AlxrIsSessionRunningFn = bool(__cdecl*)();
using AlxrRequestExitSessionFn = void(__cdecl*)();
// 日志回调：(level, utf8, len)。可选符号，缺失不影响跟踪。
using AlxrLogOutputFn = void(__cdecl*)(std::uint32_t level, const char* output, std::uint32_t len);
using AlxrSetLogOutputFn = void(__cdecl*)(std::uint32_t options, AlxrLogOutputFn fn);

inline constexpr const char* kEngineDllName = "alxr_engine.dll";

// 把原生 result 里的面捕/眼捕块搬进 DecodedPacket（不做清洗，清洗在 Parse 层做）。
void FillFromResult(const AlxrProcessFrameResult& result, DecodedPacket& out);

} // namespace alxr
