# ALXR 协议要点

来源一手核对：[VRCFT-ALXR-Modules](https://github.com/korejan/VRCFT-ALXR-Modules)
（v1.7.1，2026-09-28）与它的子模块
[libalxr-sharp](https://github.com/korejan/libalxr-sharp)。两个模块共享同一份
`ALXRFacialEyePacket`，本仓库是该线格式的独立 C++ 实现，未复制上游源码。
每项标注「已确认」（在源码里直接看到依据）或「未确认」。

## 1. Remote：TCP 数据通路

| 事实 | 状态 | 依据 |
|---|---|---|
| PC 侧是 **TCP 客户端**，主动连头显（串流端）的地址:端口 | 已确认 | `ALXRRemoteModule.cs` `ConnectToServer()`：`TcpClient.BeginConnect(remoteConfig.ClientIpAddress, remoteConfig.PortNo)` |
| 默认端口 **49192**，默认地址 127.0.0.1 | 已确认 | `LibALXR.cs:9` `TrackingServerDefaultPortNo = 49192`；`ModuleConfig.cs` `ALXRClientConfig.Default` |
| 连接超时 2000ms，`stream.ReadTimeout = 2000`；读失败即关连接、下轮重连 | 已确认 | `ALXRRemoteModule.cs` `ConnectionTimeoutMs = 2000`、`Update()` catch 块 |
| 每 tick 读满 `Marshal.SizeOf<ALXRFacialEyePacket>()` 字节后解析 | 已确认 | `ReadALXRFacialEyePacket()`：循环 `stream.Read` 到 offset==size |
| **无帧头/无分隔符**：TCP 流按定长 342 字节切分 | 已确认 | 上游直接读固定长度；结构体 `[StructLayout(Pack=1)]` 无 magic |
| 本模块的重连退避 1s→2s→4s→8s（上游是每 tick 立即重试） | 已确认（我们的有意差异） | `alxr_transport.hpp Backoff`；避免网络抖动卡住引擎线程 |

## 2. ALXRFacialEyePacket 线格式（Pack=1，342 字节，小端）

依据 `libalxr-sharp/ALXRFacialEyePacket.cs` 的 `[StructLayout(Sequential, Pack=1)]`
与 `Marshal.MaxEyeCount=2 / MaxExpressionCount=70`；C# 用 `MemoryMarshal.Cast`
按字节重解释，**没有任何字节序转换** → 线格式 = 小端原生布局（Windows/ARM 皆小端）。

| 偏移 | 字段 | 类型 | 状态 |
|---|---|---|---|
| 0 | `expressionType` | u8 枚举（None=0,FB=1,FB_V2=2,HTC=3,Pico=4,Android=5,Auto=6） | 已确认 |
| 1 | `eyeTrackerType` | u8 枚举（None=0,FBEyeSocial=1,ExtEyeGaze=2,AndroidEye=3,Auto=4） | 已确认 |
| 2 | `isEyeFollowingBlendshapesValid` | u8 | 已确认存在；**上游从未读取**（`ModuleUtils.cs` 无引用），语义未确认 |
| 3..4 | `isEyeGazePoseValid[2]` | u8×2，[0]=左眼 | 已确认（C# `fixed byte[MaxEyeCount]`，喂 `eyeGazePose0/1` 的顺序即左/右） |
| 5 | `expressionDataSource` | u8（Visual=0/Audio=1/Unknown=2） | 已确认；上游亦不消费，仅透传 |
| 6..285 | `expressionWeights[70]` | float LE ×70 | 已确认 |
| 286..313 | `eyeGazePose0`（左眼 XrPosef） | 7×float LE | 已确认 |
| 314..341 | `eyeGazePose1`（右眼 XrPosef） | 7×float LE | 已确认 |
| 总长 | **342** = 6 + 280 + 56 | | 已确认（`static_assert` 锁死） |

XrPosef 布局：`orientation(x,y,z,w)` 在前、`position(x,y,z)` 在后，各 28 字节
（`ALXRTypes.cs ALXRPosef/ALXRQuaternionf/ALXRVector3f`）——已确认。

## 3. 70 槽权重索引（FBExpression2）

`libalxr-sharp/FBExpression2.cs`：0..62 与 `FBExpression`（XR_FB_face_tracking，63 槽）
逐项同序（已 diff 核对）；63..69 为 FB_V2 舌头组
（interdental / alveolar / front / mid / back / **out=68** / retreat=69）——已确认。
Android（`AndroidExpressions.cs`）0..62 同 FB，63..67 是舌头五向
（Out/Left/Right/Up/Down）——已确认。HTC：0..13 眼（blink/wide/squeeze/look），
14 起唇（`XrLipExpressionHTC/XrLipExpressionHTC`，上游 `Slice(14)`）——已确认。

映射逐项依据 `ModuleUtils.cs`（adjerry 对照表注释行 592/845/1072）：
`alxr_mapper.cpp` 表内每条注释的索引都出自上述枚举；差异（保守化）：

- 不引入 TrackingSensitivity 乘子（上游默认 1.0，已确认 `TrackingSensitivity.cs:56-132` 默认值）。
- `MouthUpperUp/Deepen` 不做「nose 取 max」workaround；`LipSuckUpper` 不做
  `min(1-uL^(1/6), suck)` 修正——直接取自身权重（上游对应行为已确认存在，简化属我们的选择）。
- HTC JawOpen 上游是 `JawOpen + ApeShape` **加法**；本表用两条规则取 **max**（避免超 1 后失真）。
- FB_V2 舌头：64→`TongueCurlUp`、68→`TongueOut`（上游 693-696 行，已确认）；
  65/66/67/69 上游注释明确「no current mappings」→ 不映射。
- FB（v1）没有舌头数据：上游 939 行写 `TongueOut=0f`——我们不写该形状（属性位更干净）。
- HTC 眼 6..13（look 组）：上游 `UpdateEyeExpressionsHTC` 完全没用它们（注视来自 gaze pose），已确认。

## 4. 注视方向换算

上游 `NormalizedGaze(quaternion)`（`ModuleUtils.cs:273`）把四元数折成 pitch/yaw
（VRCFT 的 Vector2，弧度，已确认公式）。FGB 帧槽要 HMD 局部单位向量（-Z 前），
本模块改为「四元数旋转 (0,0,-1)」（`alxr_mapper.cpp GazeDirection`）——数学上同为
该姿态的前向向量，但与上游 VRCFT 消费端的欧拉角口径是否等价：**未确认**（真机对比项）。
仅当 `eyeTrackerType != None` 且该眼 valid 字节非 0、四元数范数 > 1e-6 才置
`FGB_ATTR_*_GAZE`；置信度统一 0.8（我们的选择，上游无此概念）。

## 5. 睁眼度

`ModuleUtils.cs` 四模式（Linear/NonLinear/Smooth/Multi，上游默认 Linear=0，
已确认 `ModuleConfig.cs:153`），FB/FB_V2/Android 同一套索引（closed 12/13、
lid 28/29、down 14/15、widen 59/60——已确认）；HTC 用 blink(0)/blink(2)。
Multi 模式里 `1.1f - eyeClosed*EyeLid` 等乘子取 1.0（上游默认值，已确认）。

## 6. Local：libalxr C ABI

| 事实 | 状态 | 依据 |
|---|---|---|
| 导出符号 `alxr_init / alxr_destroy / alxr_process_frame2 / alxr_is_session_running`（另有 request_exit_session 等） | 已确认 | `libalxr-sharp/LibALXR.cs:11-29` DllImport `alxr_engine`，CallingConvention.Cdecl |
| DLL 文件名 `alxr_engine.dll` | 已确认 | 同上；`ModuleLibs/NativeLibsHere.txt` 说明厂商 DLL 随包放置、不在仓库 |
| `ALXRClientCtx` 144 字节布局（9 指针 + 版本 + 图形/解码枚举 + 路径 + u32 + u8/u8/u16 + 13 bool） | 已确认（C# Marshal 布局推导；`static_assert` 锁 112/120/128 偏移） | `ALXRTypes.cs ALXRClientCtx`，字段声明序 + 自然对齐 |
| `ALXRSystemProperties` 296 字节（refresh_rates 偏移 264） | 已确认（同上推导） | `ALXRTypes.cs:239-256` |
| `ALXRProcessFrameResult = ALXRHandTracking(3760) + 面部包(自然对齐版) + exitRenderLoop + requestRestart` | 部分确认：结构组成与声明序已确认；**尾部偏移（weights 3768 / eye_gaze_pose 4048 / exit_render_loop 4104）按 MSVC 规则推得**，libalxr 若用 `#pragma pack` 会不同——**未确认**，需拿真实 `alxr.h` 复核 | `ALXRTypes.cs:446-455` + `ALXRHandTracking.cs` 尺寸 |
| ctx 字段语义（headless=1、no_ft_server=1、facial_tracking/eye_tracking=Auto 等） | 已确认 | `ALXRLocalModule.cs CreateALXRClientCtx()` 168-211 行 |
| init 失败重试节奏：2000ms × 10 次后放弃 | 已确认 | `ALXRLocalModule.cs` `RetryTime=2000 / MaxRetryCount=10` |
| 上游对 alxr_init 失败即 `Thread.Sleep(2000)` 阻塞重试 | 已确认（我们改为宿主 tick 内退避，不阻塞） | 同上 |
| libalxr 版本口径：上游仓库不锁子模块 commit，`.gitmodules` 的 libalxr 分支为 `v0.10.x`（libalxr-sharp 侧） | **未确认**：具体到 `alxr_engine.dll` 的发布版本号需要在装了厂商 DLL 的机器上核对导出表 | `.gitmodules` |

`path_string_to_hash` 上游用 `path.GetHashCode()`（C# 随机化运行时不可复制）；
本模块用 FNV-1a 64（已确认可替换：该哈希只在进程内做 interaction profile 路径键，
不上线，`ALXRLocalModule.cs:210` 的注释语义）。

## 7. 其它未确认 / 不实现项

- **Pico**：枚举存在（4），上游无任何映射实现——本模块同样返回 0 属性。未确认是否将来有数据。
- 头姿：包里没有头部姿态字段（只有两个**眼**pose）；`ALXRTrackingInfo.headPose`
  属 Remote 视频通路，不在本包内——不接头姿。
- 瞳孔：上游硬编码 5mm 且 `_minDilation=0/_maxDilation=10`（已确认），非真实数据，不声明 pupil 能力。
- `isEyeFollowingBlendshapesValid`、`expressionDataSource` 的语义与消费场景：**未确认**
  （上游透传不消费）。
