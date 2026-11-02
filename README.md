# FGB ALXRModule

[![build](https://github.com/Aqamoe/FGB-ALXRModule/actions/workflows/ci.yml/badge.svg)](https://github.com/Aqamoe/FGB-ALXRModule/actions/workflows/ci.yml)

**状态：0.1.0，未验证（untested）**。协议解析、映射与 ABI 生命周期有单元测试和合成 TCP
端到端冒烟；真机（头显 + 串流端 / PC OpenXR 运行时）验收待补，见
[docs/verification.md](docs/verification.md)。

[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge) 的 ALXR 原生模块。
**一个仓库出两个产物**，对应上游 [VRCFT-ALXR-Modules](https://github.com/korejan/VRCFT-ALXR-Modules)
的两个模块：

| 产物 | 数据通路 | 依赖 |
|---|---|---|
| **ALXR (OpenXR Local)** `alxr-local.dll` | 运行时加载 `alxr_engine.dll`（libalxr），在 PC 建 headless OpenXR 会话，直读 FB / FB_V2 / HTC / EXT / Android 面捕扩展 | 活动 OpenXR 运行时 + 厂商 libalxr |
| **ALXR (OpenXR Remote)** `alxr-remote.dll` | 主动连串流端（头显上的 ALXR 客户端）`IP:49192` 的 TCP，收 342 字节的 `ALXRFacialEyePacket` | 头显端 ALXR 客户端开启面捕推送 |

两个产物都不需要 VRCFT，不注入任何进程，也不需要管理员权限。
协议逐项核对与「已确认/未确认」清单见 [docs/protocol.md](docs/protocol.md)。

## 功能 / Features

- **表情**：按上游索引表把 70 槽权重映射到 Unified Expressions（FB_V2 全量；FB 无 63..69 舌头；
  HTC 眼 0..13 + 唇 14 起；Android 舌 63..67）。
- **眼睛**：睁眼度沿用上游公式（FB 系 `1 - clamp(closed + closed·lidTightener)` 等四种模式，
  默认 linear；HTC 用 blink）；注视由 `XrPosef.orientation` 旋转前向向量得 HMD 局部单位向量，
  仅在该眼 `isEyeGazePoseValid` 且四元数非退化时置位——无效帧绝不写「看正前方」的假数据。
- **断流可恢复**：Remote 断线按 1s→2s→4s→8s 退避自动重连；10 秒没有数据包在卡片提示原因。
  Local 会话退出（头显休眠/运行时切换）自动重建。
- **可诊断**：缺厂商 DLL、地址非法、畸形包、连接被拒都有明确日志，不静默丢数据。

## 不提供 / Not provided

| 数据 | 原因 |
|---|---|
| 头姿 | 报文里的 `XrPosef.position` 是眼坐标系的本地姿态，未确认与 VRCFT 头姿口径一致，先不接 |
| 瞳孔 | 上游一律硬编码 5mm，非真实测量 |
| 2D 注视点 | 报文没有该字段 |
| 上游灵敏度乘子 / OneEuro 平滑 | 属上游配置生态，本模块不引入对应配置文件 |

## 环境要求

| 项 | Local | Remote |
|---|---|---|
| 系统 | Windows 10 / 11 x64 | Windows 10 / 11 x64 |
| 宿主 | FaceGaze Bridge（原生模块 ABI v2） | 同左 |
| 来源 | 活动 OpenXR 运行时（Link/AirLink/VDXR 等）+ `alxr_engine.dll` | 头显端 ALXR 客户端（PC 主动连其 49192） |
| 其它 | 关闭 VRCFaceTracking，避免两个 headless 会话抢同一运行时 | 关闭 VRCFT 的 ALXR Remote 模块（会连同一端口） |

## 安装

1. 从 Releases 下载 `alxr-local-0.1.0.zip` 和/或 `alxr-remote-0.1.0.zip`（一个仓库两个包）。
2. FaceGaze Bridge →「模块库 → 原生模块 → 安装原生模块 (.zip)」，或「模块」页「从 .zip 安装」。
3. 「模块」页把对应模块设为启动，角色通常给 `expression` 与 `eyes`。
4. 重启引擎；来源在线后模块卡片应显示活跃。

## 配置

模块目录下可选 `config.ini`（改完重启引擎；未知键会记警告不会静默失效）：

```ini
# Remote：串流端（头显）地址与端口，默认 127.0.0.1:49192
ip = 192.168.1.23
port = 49192

# 两个产物都支持：睁眼度模式（linear|nonlinear|smooth|multi），默认 linear
eye_openness = linear

# 仅 Local：图形 API（auto|vulkan2|vulkan|d3d12|d3d11）、headless 行为、libalxr 详细日志
graphics_api = auto
headless = true
simulate_headless = true
verbose = false
```

## Local 与厂商运行时

本仓库**不包含也不链接** `alxr_engine.dll`：运行时按「模块目录 → 系统目录 → PATH」查找，
找不到时模块卡片直接给出提示。对 libalxr 的手写声明依据
`korejan/libalxr-sharp`（VRCFT-ALXR-Modules v1.7.1 的子模块）的官方布局定义逐项镜像，
理由与逐项状态写在 [docs/protocol.md](docs/protocol.md)。**真实调用链在 CI 上无法验证**
（CI 没有厂商 DLL 也没有 OpenXR 运行时）：CI 验证的是 ABI 布局镜像（`static_assert` 锁死
尺寸/偏移）、包解析、映射与生命周期（缺依赖时的优雅失败）；会话行为收进 `src/alxr_engine.*`
适配层，待真机验收。

## 从源码构建

需要 Visual Studio 2022（x64）、CMake ≥ 3.24 与 Node 20（audit 脚本用）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

SDK 与 Kit 由 `sdk.lock.json` / `kit.lock.json`（**Kit v0.4.0**，支持一仓多产物打包）
在配置阶段拉取并校验 SHA256，仓库里不含 SDK 头文件副本。本地想指向已解压的 Kit：

```powershell
cmake -S . -B build -A x64 -DFGB_KIT_DIR="D:/code/FGB-Module/FGB-Module-Kit"
```

打包与测试：

```powershell
cmake --build build --config Release --target package-modules
#   -> build/Release/alxr-local-0.1.0.zip 与 alxr-remote-0.1.0.zip（两个包）
ctest --test-dir build -C Release --output-on-failure   # 协议/映射/重连单测 + local ABI 冒烟
cmake --build build --config Release --target smoke-test # Remote：真实 ABI + 合成 TCP 服务端
```

> **为什么 smoke-test 只给 Remote**：Kit 的 `SMOKE_SOURCE` 会创建固定名 `smoke-test`
> 目标，同仓第二次 `fgb_add_module` 再传就会撞名。Local 的 ABI 冒烟写成普通测试
> `tests/test_abi_local.cpp`（动态加载本仓库构建的 `alxr-local.dll`，验证导出符号、
> ABI 协商与缺 `alxr_engine.dll` 时的优雅失败）。Local 的**完整**端到端需要厂商 DLL +
> 真实 OpenXR 运行时，CI 上必然缺依赖，因此不进 `smoke-test`，见
> [docs/verification.md](docs/verification.md)。

## 目录结构

```
src/main_local.cpp            Local：libalxr 生命周期、会话重建、状态上报
src/main_remote.cpp           Remote：TCP 连接、退避重连、状态上报
src/alxr_packet.*             342 字节线格式解析（清洗 NaN/越界）
src/alxr_mapper.*             FB/FB_V2/HTC/Android → Unified Expressions 映射
src/alxr_transport.*          TCP 客户端 + 退避（Remote 专用）
src/alxr_abi.hpp              libalxr C ABI 最小手写声明（static_assert 锁布局）
src/alxr_engine.*             厂商运行时适配层（不链接、运行时加载）
module/alxr-local/            清单与图标（另一个产物同）
tests/                        协议/映射/重连单测、local ABI 冒烟、remote 端到端冒烟
docs/protocol.md              线格式与映射依据（逐项已确认/未确认）
docs/verification.md          真机验收清单
cmake/                        依赖锁解析（SDK / Kit 都是拉取，不复制）
```

## 排错

| 现象 | 处理 |
|---|---|
| 卡片提示「找不到 alxr_engine.dll」 | 安装对应串流/运行时软件，或把该 DLL 放到模块目录旁，再重启引擎 |
| Local 反复「libalxr 初始化失败」 | 确认活动 OpenXR 运行时已切换为目标运行时、头显处于活动状态；重试 10 次后会放弃，需重启模块 |
| Remote 一直「等待连接串流端」 | `config.ini` 的 `ip` 填头显局域网地址；确认串流端在线、PC 与头显同网段 |
| 日志出现「丢弃畸形包」 | 一般是 TCP 错位（上一版模块残留）；重启模块即可，持续出现请带版本提 issue |
| 表情不动、眼睛正常 | 角色没分给 `expression` |
| VRCFT 同时开着 | 关掉：Remote 抢同一 TCP 来源，Local 可能与 VRCFT 的 headless 会话互斥 |

## 致谢 / Attribution

- 协议与映射依据 [VRCFT-ALXR-Modules](https://github.com/korejan/VRCFT-ALXR-Modules)
  及其子模块 [libalxr-sharp](https://github.com/korejan/libalxr-sharp)；
  本仓库是该线格式的独立 C++ 实现，未包含其源码，也不含厂商头文件。
- ABI 头与宿主：[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge)；
  共享工具链：[FGB-Module-Kit](https://github.com/Aqamoe/FGB-Module-Kit)。

## 许可

Apache License 2.0，见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。
