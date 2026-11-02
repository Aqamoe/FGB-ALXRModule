# 真机验收清单

> 当前状态：**未验证**。下列任一项完成后，请在 issue 里附设备与版本信息。

CI 能证明的：包解析（字节序/长度/截断/NaN）、映射表、退避与重连状态机、
两个产物的导出 ABI、Remote 的合成 TCP 端到端。CI **不能**证明的（都依赖厂商 DLL +
真实 OpenXR 运行时/头显）：libalxr ABI 镜像的实际行为、真机坐标系极化、串流端设置。
因此 `smoke-test` 只挂 Remote；Local 的 ABI 冒烟是普通测试 `test_abi_local`
（动态加载 `alxr-local.dll`，验证缺 `alxr_engine.dll` 时优雅失败），
Local 端到端必须在真机补，就是下面这一页。

## Remote

### 环境

| 项 | 值 |
|---|---|
| 头显端 | ALXR 客户端（记录版本），开启面捕/眼捕推送（开关名称依版本，未确认） |
| 系统 | Windows 10 / 11 x64 |
| 宿主 | FaceGaze Bridge（记录版本号） |
| 网络 | PC 能访问头显 IP:49192（PC 是 TCP 客户端） |

### 步骤

- [ ] `config.ini` 填 `ip = 头显局域网IP`
- [ ] 关闭 VRCFT（避免连同一来源）
- [ ] 安装 `alxr-remote-0.1.0.zip`，设为启动，重启引擎
- [ ] 模块卡片显示「串流端已连接」

### 观察项

- [ ] 张嘴 → `JawOpen`；微笑 → `MouthCornerPull*`
- [ ] 眨眼 → 睁眼度下降，左右眼独立（wink 分别掉）
- [ ] 眼球转动 → 注视方向跟随；极化正确（看左→向量 x 同号于 VRCFT 口径，需与 VRCFT 并排对比）
- [ ] 拔网线/退出串流 → 卡片 10 秒内提示断流；恢复后自动重连回到活跃
- [ ] 头显休眠再唤醒 → 连接重建且数据恢复

## Local

### 环境

| 项 | 值 |
|---|---|
| 运行时 | 目标 OpenXR 运行时设为活动（Quest Link/AirLink、VDXR 等，记录版本） |
| 厂商 DLL | `alxr_engine.dll`（记录它来自哪个软件/版本；本仓库不打包） |
| 头显 | 已连接、处于活动状态（Link 需进入 VR） |
| 宿主 | FaceGaze Bridge（记录版本号） |

### 步骤

- [ ] 关闭 VRCFT（两个 headless 会话可能互斥）
- [ ] 安装 `alxr-local-0.1.0.zip`，设为启动，重启引擎
- [ ] 模块卡片状态从「等待 OpenXR 运行时会话」→「面捕来源 FB_V2 …」

### 观察项

- [ ] 首次会话建立耗时与重试节奏（2s × ≤10 次，超限后卡片给出原因）
- [ ] 表情极化/映射与 VRCFT ALXR Local 模块并排对比（同一组动作）
- [ ] 头显休眠 → 会话退出后自动重建，卡片有「会话退出，重建中」日志
- [ ] 退出引擎无崩溃、无残留 OpenXR 会话（任务管理器确认）
- [ ] `config.ini` 的 `graphics_api`/`headless`/`eye_openness` 各值生效

### ABI 复核（首要风险项）

- [ ] 取该 `alxr_engine.dll` 对应 libalxr 版本的 `alxr.h`（或 dump 导出表），
  逐项复核 `docs/protocol.md` 第 6 节标「未确认」的偏移推导
  （`AlxrProcessFrameResult` 尾部偏移按 MSVC 自然对齐推得，libalxr 若用
  `#pragma pack` 会不同）；不一致就把实际布局写进 protocol.md 并改 `alxr_abi.hpp`。
- [ ] 验证 FNV-1a `path_string_to_hash` 替换 C# `GetHashCode()` 后，运行时 interaction
  profile 路径解析无回归（日志出现 hash 相关告警即为不一致）。
