# 更新日志

遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与语义化版本。

## [未发布]

## [0.1.0] - 未发布
### 新增
- 一个仓库两个产物（Kit v0.4.0 一仓多包）：
  - **ALXR (OpenXR Remote)**：TCP 客户端连串流端 `IP:49192`，收 342 字节
    `ALXRFacialEyePacket`，断线 1s→8s 指数退避自动重连。
  - **ALXR (OpenXR Local)**：运行时加载 `alxr_engine.dll`（libalxr），
    headless OpenXR 会话直读 FB/FB_V2/HTC/EXT/Android 面捕扩展，
    会话失败 2s×10 次退避重试、退出后自动重建。
- 70 槽权重 → Unified Expressions 映射（逐项对照上游 `ModuleUtils.cs`，保守化差异见协议文档）。
- 睁眼度四种模式（linear|nonlinear|smooth|multi，默认 linear）；
  注视由 gaze pose 四元数旋转前向向量，仅该眼 valid 且四元数非退化时置位。
- 线格式清洗：NaN/Inf 权重归零、越界裁剪、退化四元数不产出注视。
- `config.ini`：Remote `ip`/`port`，Local `graphics_api`/`headless`/`simulate_headless`/
  `verbose`，共同 `eye_openness`。
- libalxr C ABI 最小手写声明（依据 libalxr-sharp v1.7.1 镜像，static_assert 锁死布局），
  不引入厂商头文件、不链接、不打包厂商 DLL。
- 单测：包解析（字节序/长度/截断/多余尾巴/NaN）、映射（FB/FB_V2/HTC/Android/无效帧）、
  退避与 TCP 重连状态机、Local ABI 冒烟（动态加载 alxr-local.dll 验导出与缺依赖优雅失败）；
  Remote 端到端合成 TCP 服务端冒烟（`smoke-test` 目标）。

### 已知限制
- 未在真机验证（串流端 / PC OpenXR 运行时），状态为未验证；验收清单见 docs/verification.md。
- Local 的 `AlxrProcessFrameResult` 尾部偏移按 MSVC 自然对齐推导，未经 libalxr 原生头复核
  （见 docs/protocol.md 第 6 节）。
