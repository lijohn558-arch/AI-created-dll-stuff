# Community Shaders 项目调研 — 对 RenderDoc 阶段的价值

> 调研日期：2026-09-30
> 主仓库：https://github.com/community-shaders/skyrim-community-shaders（默认分支 dev，GPL-3.0 + Modding Exception）
> 组织：https://github.com/community-shaders

---

## ⚠️ 许可证警示（重要）

Community Shaders 采用 **GPL-3.0 WITH Modding Exception AND GPL-3.0 Linking Exception**。
- 研究、学习、借鉴**思路与接口信息**：没问题
- **直接复制代码到本项目**：必须遵守 GPL-3.0 传染性要求（本项目若发布需开源）
- 建议：只把它当作**参考资料与知识来源**，代码重写/独立实现

---

## 1. 直接可用的 RenderDoc 资料（第一步核心）

### 1.1 README 官方抓帧配置
README 中 "Capture with RenderDoc" 一节给出标准配置：
- Executable Path: `PATH/TO/ModOrganizer.exe`
- Working Directory: `C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition`
- Command-line Arguments: `--log run ".../skse64_loader.exe"`
- ☑ **Capture Child Process**（必须勾选，RenderDoc 挂在 MO2 上，实际 D3D 设备在子进程）

### 1.2 内置 RenderDoc 集成（`src/Features/RenderDoc.cpp`）
Community Shaders 内置了完整的 RenderDoc 程序化集成，其中**踩坑经验极具价值**：

1. **NvAPI 崩溃问题（关键！）**
   - RenderDoc 默认禁用厂商扩展（NvAPI），`nvapi_QueryInterface` 返回 NULL
   - Skyrim 依赖 NvAPI (D3D11)，混合显卡机器上驱动会在第一次 Present 时移除设备 → **游戏启动即崩溃**
   - 解决方案：`SetCaptureOptionU32(eRENDERDOC_Option_AllowUnsupportedVendorExtensions, 0x10DE)`（0x10DE = NVIDIA vendor id），RenderDoc 1.3.0+ 支持
2. **捕获热键**：F12 / PrtSc（无修饰键时触发），支持多帧捕获 `TriggerMultiFrameCapture`
3. **捕获文件命名**：`Skyrim_{runtime}_{gameVersion}` 模板，自动嵌入游戏版本、CS 版本、启用特性清单（`SetCaptureFileComments`）
4. **Overlay 禁用**：`MaskOverlayBits(eRENDERDOC_Overlay_None, ...)` 避免叠加层干扰
5. **磁盘空间预检**：按帧数估算所需空间
6. **DLL 加载位置**：`Data/Renderdoc/renderdoc.dll`（不走系统 PATH）

> **2026-10-02 源码复核**（GitHub API 直读 dev 分支 `src/Features/RenderDoc.cpp`，34.6KB）：以上 6 点全部属实。
> 补充：① `SetCaptureKeys(nullptr, 0)` 显式清空 RenderDoc 自带热键，触发收归 CS 自家菜单
> （`HandleCaptureHotkey` / `TriggerMultiFrameCapture`，多帧捕获 #2355）；② renderdoc.dll 随 feature 包
> 分发于 `features/RenderDoc/Renderdoc/renderdoc.dll`；③ NvAPI 透传为 PR #2621（2026-08），
> 其源码注释明确写的是 **"the driver remove the device at the first Present"**（混合显卡、启动即崩）。
> **本项目对齐情况**：`src/capture-helper/main.cpp:292` 已实现同款
> `SetCaptureOptionU32(eRENDERDOC_Option_AllowUnsupportedVendorExtensions, 0x10DE)`；API 版本阶梯
> 1_7_0→1_6_0→1_5_0 比 CS 只取 1_7_0 更宽容；触发走同一官方 `TriggerCapture` 接口。

### 1.3 抓帧前注意事项（来自其 UI 提示）
- RenderDoc 激活时性能严重下降（帧注解 Frame Annotations 会被强制开启）
- Upscaling / Frame Generation 可能与抓帧不兼容（抓帧时应关闭 DLSS/FSR/FG）

---

## 2. 渲染管线逆向资料（RenderDoc 对照分析的基础）

### 2.1 `package/Shaders/` — 87 个游戏着色器 HLSL 源码
这是 Skyrim SE 着色器的反编译/增强版本，**完整对应 RenderDoc 抓帧里看到的每一个 Draw Call 用的着色器**：

| 类别 | 文件 | 对应渲染阶段 |
|---|---|---|
| 场景着色 | `Lighting.hlsl`, `Effect.hlsl`, `DistantTree.hlsl`, `BloodSplatter.hlsl` | G-Buffer 几何/光照 |
| 延迟合成 | `DeferredCompositeCS.hlsl` | 延迟渲染合成 (Compute) |
| 图像空间/后处理 | `IS*.hlsl`（约 40 个） | 后处理链 |
| — | `ISVolumetricLighting*.hlsl` (含 Raymarch/Blur CS) | 体积光 |
| — | `ISSAO*.hlsl`, `ISSILComposite.hlsl` | SSAO/SIL |
| — | `ISTemporalAA.hlsl` | TAA |
| — | `ISDepthOfField.hlsl`, `ISBlur.hlsl`, `ISHDR.hlsl` | DOF/模糊/HDR |
| — | `ISWater*.hlsl` | 水面（位移/流动/混合） |
| — | `ISReflectionsRayTracing.hlsl` | 屏幕空间反射 |
| UI | `Menu/BackgroundBlurComposite.hlsl` | 菜单模糊 |
| 着色器公共库 | `Common/GBuffer.hlsli` | **G-Buffer 布局定义** |
| | `Common/FrameBuffer.hlsli` | **常量缓冲区布局（cbuffer 寄存器）** |
| | `Common/ShadowSampling.hlsli` | 阴影采样（CSM/PCF） |
| | `Common/Skinned.hlsli` | 蒙皮骨骼动画 |
| | `Common/Permutation.hlsli` | 着色器 permutation/变体机制 |
| | `Common/Shading.hlsli`, `BRDF.hlsli`, `LightingEval.hlsli` | 光照模型 |
| | `Common/SharedData.hlsli` | 共享数据结构 |

**用途**：
- RenderDoc 抓帧时用它**快速理解每个事件的着色器逻辑**
- 反查 cbuffer/texture **寄存器绑定**（slot ↔ Vulkan descriptor 映射）
- G-Buffer 布局（RT 格式/语义）直接用于 Vulkan RT 设计
- IS* 链即为**游戏后处理顺序的权威清单**

### 2.2 `src/Hooks.cpp`（1121 行）— 引擎渲染函数 Hook 清单
已识别的 Hook 点（即我们 Vulkan 对接需要拦截的相同入口）：
- `BSShader::LoadShaders` — 着色器加载（可截获字节码，内含 `DumpShader`）
- `BSShader::BeginTechnique` — 渲染技术切换（vertexDescriptor/pixelDescriptor）
- `BSLightingShader/BSEffectShader/BSSkyShader/BSGrassShader/BSParticleShader::SetupGeometry` — 各材质通道的 Pass 设置
- `BSImagespaceShader::Render` — 图像空间（后处理）渲染
- `IDXGISwapChain::Present` — **Present 拦截（Vulkan 呈现注入点）**
- `Main_HDRTonemapBlendCinematic_Render` — HDR 色调映射
- `Sky_UpdateColors`, `Sky_SetDirectionalAmbientColors` — 天空颜色
- 依赖 **CommonLibSSE-NG**（`RE::` 命名空间）做地址重定位（REL::Relocation）

### 2.3 其他相关源码
- `src/Deferred.cpp` — 延迟渲染管线实现（游戏当前的 deferred 流程）
- `src/ShaderCache.cpp` + `src/ShaderTools/ShaderCompiler.cpp` — 着色器缓存/编译/变体管理
- `src/ShaderTools/BSShaderHooks.cpp` — BSShader 深度 Hook
- `src/Utils/D3D.cpp` — D3D11 辅助工具
- `src/FrameAnnotations.cpp` — 帧注解（RenderDoc 内分段标注）
- `docs/development/shader-workflow.md` — 着色器工作流文档

---

## 3. 同组织其他相关仓库

| 仓库 | 价值 |
|---|---|
| **community-shaders/dxvk** | 他们自己的 **DXVK fork**（D3D11→Vulkan 翻译层），是本项目"混合方案"的直接参考实现 |
| **CommonLibSSE-NG** | Skyrim SE/VR 逆向库（RE:: 类型、REL:: 地址重定位）——Hook 系统的基础设施，C++，GPL-3.0 |
| **CommonLibSSE** | 同上的经典版本（MIT） |
| **Streamline** | NVIDIA Streamline 集成（DLSS/FSR 框架）参考 |

---

## 4. 对本项目第一阶段的直接影响

1. **抓帧配置照抄即可**（MO2 + Capture Child Process），加上 NvAPI workaround 即可避免混合显卡崩溃
2. **抓帧后对照清单**：用 `package/Shaders/` 的 HLSL 逐一解释 RenderDoc 事件
3. **Hook 点清单已验证可行**：CommonLibSSE 的 `RE::` 接口 + Hook.cpp 模式证明 Skyrim SE 渲染管线可被完整拦截 → 直接回应了风险项 #1
4. **G-Buffer/常量缓冲区布局**不用从零逆向，`GBuffer.hlsli` / `FrameBuffer.hlsli` 直接给出
5. **后处理链顺序**：以 IS* 着色器为权威参考（约 40 个，含体积光/SSAO/TAA/DOF/水）
6. **dxvk fork** 是混合方案的活教材（他们已经在维护一个 D3D11→Vulkan 层）

---

## 5. 关于"能否直接获取现成抓帧数据"的查证结论（2026-09-30）

### 5.1 Community Shaders 渠道
- GitHub Releases：只有 AIO mod 安装包，**无 .rdc**
- GitHub Issues：搜索 `rdc`/`renderdoc` 无任何上传的抓帧文件（GitHub Issue 也不支持大文件附件）
- 其 RenderDoc 功能定位是"用户抓帧 → Discord 私发团队"，**无公开存储**
- 且其抓帧是 **CS 魔改后管线**，不是原版 DX11 基线

### 5.2 DXVK 渠道（doitsujin/dxvk）
- 仓库树 627 个条目：**无 .rdc、无 tests 目录、无任何抓帧文件**
- Issues 中 99 个 renderdoc 相关条目均为问题讨论，无帧数据附件
- DXVK 定位是通用 D3D11→Vulkan 翻译层，**不提供任何游戏（含 Skyrim）的帧基线数据**
- ✅ DXVK 真正的价值：**源码即完整的 D3D11→Vulkan 概念映射参考**（资源语义、状态对象、Swapchain、映射策略等）

### 5.3 最终结论
**"原版管线一帧基线数据"和"DX11↔Vulkan 对照帧"均无公开来源，必须自己抓取。**
准备工作（管线结构、着色器逻辑、抓帧方法、Hook 点）可全部借力 CS/DXVK，只有帧数据本身需本地生成。

### 5.4 附带发现（后续可用的工具）
- `wenlek/renderdoc-mcp` / `JiaboLi-GitHub/renderdoc-mcp`：RenderDoc 的 MCP 服务器 + HTML 报告生成，可让 AI 直接分析 .rdc（59 个结构化工具，支持抓帧对比 compare captures）→ 后续 DX11↔Vulkan 自动化对比可评估引入
- `renderdoc-tool-set`：.rdc 命令行解析工具集

---

## 6. 建议的后续动作

- [ ] 将 `package/Shaders/` 的着色器清单整理为"渲染阶段对照表"（抓帧时对照用）
- [ ] 研读 `Common/GBuffer.hlsli` + `FrameBuffer.hlsli`，产出寄存器映射草案
- [ ] 抓取 CS 官方示例 .rdc（如 issues/社区有分享）先行熟悉结构，再抓自己游戏的帧
- [ ] 评估 CommonLibSSE-NG 作为 Hook 基础库的可行性（GPL 传染性 vs MIT 的 CommonLibSSE）

---

## 7. Present 拦截路线对比 — PoC-A 的参考系（2026-10-02 补充）

**起因**：PoC-A v1.4 双跑谜团——游戏交换链对象 vptr = 我们挂好的类 vtable、槽 4 被看门狗
连续 120s 复查稳定指向钩子、游戏画面正常动画（Present 必然在发生），**Present 计数却恒 0**；
用户确认本机有 **GeForce 覆盖层（GFE，游戏启动时注入）** → 据此调研成熟工具的 Present
拦截姿势（当日 GitHub API 直读源码实证）。

### 7.1 Community Shaders：根本不拦 Present
- 全树检索（dev 分支 1064 节点）：**无任何 DXGI/Present hook 文件**——仅有
  `src/Features/Upscaling/DX12SwapChain.*`（自家超分用的私有 DX12 交换链，不拦截游戏）。
- 注入全部在引擎层（`src/Hooks.cpp` 1121 行，Address Library 定位，见 §2.2）；对 RenderDoc
  走官方程序化 API（§1.2），**从不与 RenderDoc/GFE 在 DXGI 层抢对象**。
- 结论：CS 对本谜团无现成答案，但其「不碰 DXGI、走官方接口」的做法解释了他们为何从未
  遇到此类问题——**没拦截 Present，就不会被 Present 层的多方混战争吵波及**。

### 7.2 ReShade / RenderDoc：包装对象（proxy COM class）
- ReShade `source/dxgi/dxgi_swapchain.hpp`：
  `class DXGISwapChain final : public IDXGISwapChain4`，构造收 `IDXGISwapChain *original`；
  `Present()` 内 `on_present(Flags)` → `_orig->Present(...)` → `on_finish_present(hr)`——
  **自有 vtable 的包装类转发到原对象**；配套 `deps/minhook` + `source/hook_manager.cpp`
  detour dxgi/d3d11 **导出函数**（CreateDXGIFactory 等）以替换创建结果（连 Windows 内部的
  `IDXGISwapChainTest` 接口都做了仿真转发）。
- RenderDoc 同为包装对象——**本项目实测证据**：v1.2/v1.3/v1.4 带 capture-helper 的日志里，
  五口径工厂 / 设备链 / 辅助 dummy 的 vtable 全落在 `renderdoc.dll`（README 实测链）。
- **包装桩字节实证（v1.5 探针 B，2026-10-02）**：renderdoc 交换链类 slot4 原实现 = 11 字节
  三指令 thunk（`48 8B 49 10 | 48 8B 01 | 48 FF 60 20` = `mov rcx,[rcx+10]; mov rax,[rcx];
  jmp [rax+20]`，后接 INT3 填充；slot18 同款跳 `[rax+90]`）——即**动态读取真对象当前
  vtable 并转发**，不缓存函数指针 ⇒ 只要真类 slot4 是我们的钩子，包装层 Present 最终必落
  入我方钩子（这决定了加载序方案下 ★ 中否不再影响计数）。CreateSwapChain 原实现为真函数
  序言，到 `sub rsp,0xA0` 共 15 字节全位置无关，可安全窃取做 trampoline。
  〔v1.7 勘误：slot4/slot18 实为 `SetPrivateDataInterface`/`GetDesc1`，上面"槽 4 是我们的
  钩子 ⇒ Present 必落钩"的推论不成立——真条件是**槽 8**；同款 thunk 图案在槽 8 上的字节
  由 v1.7 探针 B 重新打出（见 §7.5）。〕
- 共同点：**两者都不修补多方共用的共享类 vtable**——要么拥有对象（包装），要么拥有函数
  入口（MinHook 导出级/函数级 detour）。

### 7.3 对 PoC-A 的推论
- 我们的「共享类 vtable 补丁」是三者中唯一改动**公共结构**的方案 → 对 renderdoc 包装、
  GFE 覆盖层这类第三方对象操作天然脆弱。
- v1.5 三探针先取证：探针 A（对象 vptr 盯梢）若显示 vptr 被换 → GFE 包装实锤；若 vptr
  稳如钩子而计数仍 0 → 游戏 Present 落在**另一个对象**上 → 两种结局都指向 **v1.6：
  函数级 detour**（对 dxgi 真 Present 实现打 trampoline，按 ReShade/MinHook 家族姿势，
  任何对象、任何包装最终都汇入该函数入口；探针 B 的 32 字节就是选安全窃取长度的原料）。
- 链式共存注意：函数级 detour 若与 GFE/renderdoc 的 detour 并存，后装者先触发，
  须把前一家的入口转发出去（MinHook 的引用计数链式即为此设计）。
- 若函数级 detour 也不通（GFE 在其自有模块内完成全部转发、不回 dxgi），兜底 =
  **7.1 的 CS 路线**：Present 层不拦，改在引擎层（BSGraphics 帧边界）注入 PoC-B。

### 7.4 外部建议评估与 v1.6 定型（2026-10-03）

用户征询的外部意见给出四方案 + 一项 bug 修，对照实测证据逐条裁决：

| 方案 | 外部建议 | 裁决 | 依据 |
|---|---|---|---|
| A | 从 renderdoc 包装对象 `+0x10` 掏真对象挂真 vtable（偏移由存根 `48 8B 49 XX` 自动提取） | **正确，v1.7 落地为方案A** | 存根字节实测吻合（当时读到的"Present/Present1 存根"实为槽 4/槽 18 桩，见 §7.5 勘误）；v1.7 按同好复核改用 **v0 系方法桩偏移 `+0x28`**（idx4 桩 `+0x10` 作备选），解包得真对象后取其 `vtbl[8]` 当方案B 候选靶；A 单独用仍只覆盖"走类 vtable"的路径，故定位为候选/诊断，主漏斗仍是 B |
| B | 对真 Present 打函数级 detour | **采纳，落地为 v1.6 主体** | 唯一能覆盖"一切调用路径"的漏斗；窃取长度与前导字节本机已实测取齐（见下），无需猜 |
| C | 不必追真工厂 vtable，★ 仅诊断 | **采纳** | PoC-A 的目标是 Present 拦截而非创建路径 |
| D | 加载序改名（与我方原计划相同） | **半采纳**：外部称"SKSE 加载序不可控"过强——NTFS 目录枚举近似字母序、实测 c<p 与之一致，改名 `z-capture-helper` 是零成本实证；但 B 落地后 D 降级为可选验证项 | 与 B 正交，可同跑，日志可分辨谁起的作用 |
| bug | `设备链工厂 IID` 行打对象地址恒 `?`，应打 vtable 所在模块 | **采纳（v1.6 已修）** | 对象在堆上，`modulePathOf(对象)` 必然 `?`；vtable 才落在模块里 |

**本机字节取齐（v1.6 的定心丸）**：本机无编译器，但 Windows PowerShell 5.1 的
`Add-Type`（内置 .NET 编译器）可用 → 写只读转储器（D3D11 设备 + 隐藏窗口 + 直读
vtable）。设备/工厂均创建成功，仅交换链 desc 被拒（`DXGI_ERROR_INVALID_CALL`，未继续
追）——改走更直接的路：**同 boot 内 dxgi 基址进程间稳定**（游戏 run2 与本 shell 同为
`0x7FFBBACF0000`，以 run2 日志的三个已知地址对照本进程 dxgi 模块范围全部验证命中），
按址直读取齐三份前导字节：

| 函数 | 地址 | 前导字节（实测转储） | 指令边界 | 结论 |
|---|---|---|---|---|
| 「Present」存根〔v1.7 勘误：实为槽 4 `SetPrivateDataInterface` 存根，非 Present〕 | `dxgi+0x2E460` | `48 83 EC 38 4C 89 44 24 50 4C 8D 4C 24 50` | **14B 恰为边界**、零地址依赖 | 历史条目；真 Present = 槽8 `dxgi+0x19000`，见 §7.5 |
| 「Present1」存根〔v1.7 勘误：实为槽 18 `GetDesc1` 存根，非 Present1〕 | `dxgi+0x4EE60` | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20` | **15B 边界** | 历史条目；真 Present1 = 槽22 `dxgi+0x194A0`，见 §7.5 |
| CreateSwapChain | `dxgi+0x2C4D0` | 序言 25B 起含 `mov rax,[rip+X]`（RIP 相对） | 18B（25 前） | 按方案 C 不挂工厂，仅留档 |

**v1.6 实现要点**（`src/poc-presenter/main.cpp`）：

1. 入口只写 **5B `E9 rel32`** → 近端跳板（±2GB 内 `VirtualAlloc` 存 `FF 25` 绝对跳到
   钩子）——只碰前 5 字节，第三方（GFE/renderdoc）trampoline 的续接区（t+5 起）保持原样；
   钩子模块与 dxgi 常超 ±2GB，故不能裸写 `FF 25`（14B 会踩续接区）也不能直写远 `E9`；
2. 入口若已被第三方改成跳板（`E9` / `FF 25` / `48 B8..FF E0`）→ **CHAIN**：不建
   trampoline，钩子计数后直接转发其目标，与既有 detour 链式共存；
3. 前导字节与上表不符且非可识别跳板 → 保守跳过并打前 16 字节留档（dxgi 版本漂移防线）；
4. vtable 钩子转调期间置 TLS 标记，函数钩子据此跳过计数——**双层不双计**：类 vtable
   虚调用 / GFE 包装绕行 / renderdoc 动态转发，任何单一路径必被其一计到。
   有 renderdoc 时即便游戏对象在包装层，`renderdoc 存根 → 真对象 slot4 → dxgi 存根`
   的动态转发也必落同一入口。
   〔v1.7 勘误：此链按槽 4（SetPrivateDataInterface）叙述是错的——真链是
   `renderdoc 存根(slot8) → 真对象 slot8 → dxgi+0x19000`，见 §7.5。〕

### 7.5 槽位勘误与 v1.7 定型（2026-10-03，根因定案）

**根因（v1.0~v1.6 全链 0 计数的真凶）**：我们挂的"槽 4 / 槽 18"按 MS dxgi.h 实为
`SetPrivateDataInterface`（IDXGIObject 第 4 项）与 `GetDesc1`（IDXGISwapChain1 第 18 项）
——**从未挂到 Present**。连带两处假证据同时作废：

- v1.6 的字节守卫 `dxgi+0x2E460 / 0x4EE60` 就是槽 4 / 槽 18 存根的本体，探针 B 打的
  "Present 原实现" 与守卫永远一致 → **自证循环**（守卫与被验对象是同一段字节）；
- §7.4 表中 "Present 存根 disp=0x10 / Present1 存根 disp=0x30" 实为槽 4 / 槽 18 桩
  自曝的取对象偏移，不是 Present/Present1 的。

**正确 ABI**（MS SDK 头镜像 `tpn/winsdk-10` + mingw WIDL 两源一致，且与本地
`dxdump3.cs` 实测互证）：

| 方法 | 槽位 | 本机地址 / 前导 14B | 备注 |
|---|---|---|---|
| `IDXGISwapChain::Present` | **8** | `dxgi+0x19000` = `48 89 5C 24 10 48 89 74 24 18 55 57 41 56` | 恰为指令边界（其后 `48 8D 6C 24 90`），窃取 14B |
| `IDXGISwapChain1::Present1` | **22** | `dxgi+0x194A0` = `... 55 57 41 54` | 第 14 字节 `54` vs Present `56` 可区分；边界 14/16，窃取 14B |
| `IDXGIFactory::CreateSwapChain` | 10 | 运行时读（日志 `探针: CreateSwapChain 原实现` 行） | run2 ★ 实证不变 |
| `IDXGIFactory2::CreateSwapChainForHwnd` | **15** | 旧代码 14/15/17 → 仅 15 名字对、但被挂了 CoreWindow 钩子（签名错位） | 14 实为 `IsWindowedStereoEnabled` |
| `IDXGIFactory2::CreateSwapChainForCoreWindow` | **16** | 旧代码未挂 | |
| `IDXGIFactory2::CreateSwapChainForComposition` | **24** | 旧代码 17 实为 `GetSharedResourceAdapterLuid` | 潜在崩溃点，v1.7 改正 |

- 推导链：`IUnknown(0-2) → IDXGIObject(3-6) → IDXGIFactory(7-11) → Factory1(12-13)
  → Factory2(14-24)` ⇒ ForHwnd=15、ForCoreWindow=16、GetSharedResourceAdapterLuid=17、
  ForComposition=24（Factory2 末项，也是"QI Factory2 成功才挂 15/16/24"的越界防线）；
- 外部建议的工厂槽 `13/14/22` 与 ABI 不符（13=`IsCurrent`、14=`IsWindowedStereoEnabled`、
  22=`RegisterOcclusionStatusEvent`，均非创建路径）→ 方向（保留 10、删改 15/17）采纳，
  具体槽按 dxgi.h 落为 **10/15/16/24**；
- `GetDesc=槽 12`（未挂钩 → `swapDesc()` 直调安全）。

**v1.7 四项落地**（`src/poc-presenter/main.cpp`，插件版本 0.8.0）：

1. 交换链全量改号 4→8、18→22（登记 / 查原值 / 挂钩 / 看门狗 / 探针 A / 探针 B）；
2. **方案B 靶子运行时取**：① 活交换链 `vtbl[8]/[22]` 直读 → ② 方案A 解包真对象的
   `vtbl[8]/[22]` → ③ dxgi RVA `0x19000/0x194A0` 兜底；逐候选过 14B 字节守卫，
   旧"交叉验证行"删除，硬编码 RVA 降为兜底；
3. **方案A 落地**：renderdoc 转发桩自曝偏移（v0 系方法 `[obj+0x28]`，idx4 桩 `[obj+0x10]`）
   → 解包得真 `IDXGISwapChain*` → 其 `vtbl[8]` 即 dxgi 真 Present（版本无关、覆盖真类）；
   仅包装类对象尝试、全程 `memReadable` 兜底、结果只作候选（可执行页 + 字节守卫把关）；
4. **阳性确认**：安装完成与 ★ 处打印对象 vptr + `vtbl[8]` 是否等于我们的钩子；120s 告警行
   附 `g_gameSc` 是否取得与方案B detour 状态——杜绝 v1.0~v1.6 "只证明挂了错槽"的假阳性。

v1.6 的 5B `E9` 入口改写、CHAIN 共存、TLS 双层不双计机制不变；
`tools/dxbytes.ps1` 同步新 RVA/字节（旧 0x2E460/0x4EE60 保留为历史条目供解读老日志）。
