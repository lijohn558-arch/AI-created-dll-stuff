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

### 7.6 v1.7 实测判读：PoC-A 验收通过（2026-10-03 13:44，带 renderdoc 局）

**环境**：游戏 1.5.97.0 / SKSE 2.0.20，`renderdoc.dll` 已加载（`D:\...\Data\Renderdoc\`），
1920x1080 `Skyrim Special Edition`；**13:44:32 触发（安装后 6s），2 分钟 7201 次 Present，
近600帧恒 60.0 FPS，画面完全正常**；全程无 `告警:` 行、无槽位「被改写」记录。

**验收链逐行对上（`poc-presenter.log` 13:44:25~13:44:33）**：

| # | 日志行 | 判读 |
|---|---|---|
| 1 | `工厂口径0..4: OK` | 五口径工厂对象取到 |
| 2 | `dummy1: slot8 原值=…BE5BD670 来自 renderdoc.dll` / `slot22 原值=…BE5BD700` | v1.7 读的**真槽**（8=Present、22=Present1），原值落在 renderdoc → 挂 |
| 3 | `登记完成: 交换链 vtable 1 个, 工厂 vtable 1 个 (含创建方法挂钩)` | 槽 10/15/16/24 原值全读到并挂上（renderdoc 包装类工厂） |
| 4 | **`阳性确认 (dummy1/dummy2 安装完成): vtbl[8]=…poc-presenter A610 ==我们的Present钩子 ✓`**、`vtbl[22]=…A270 ==我们的Present1钩子 ✓` | **槽位修对的铁证**（v1.0~v1.6 只验证过 vtbl[4]=SetPrivateDataInterface） |
| 5 | `方案A(安装期解包): 真对象=… (经 +0x28) vtbl=dxgi+0xCD688 \| 真vtbl[8]=dxgi+0x19000 \| 真vtbl[22]=dxgi+0x194A0` | **方案A 完全成立**：与 `dxbytes.ps1` 新 RVA、本地 `dxdump3.cs` 实测（BLT/FLIP 同 vtable = `dxgi+0xCD688`）**三方逐位吻合** |
| 6 | `方案B Present 候选 1/2: 活交换链vtbl[8] (renderdoc)` → `入口字节与预期不符…保守跳过`（前 16B 留档 `48 89 5C 24 08…`）→ `候选 2/2: 方案A真vtbl[8]` → **`RAW 已装 target=dxgi+0x19000 窃取=14B (入口 5B E9)`** | 字节守卫按设计工作：renderdoc 存根是 32B 标准序言 ≠ 14B 守卫 → 跳过留档，改用方案A 给的真靶；Present1 同理装于 `dxgi+0x194A0` |
| 7 | **`第 1 次 Present (经 Present): vtable=0x7FFDBF0B7018 来自 renderdoc.dll \| 1920x1080 … title="Skyrim Special Edition"`** | **PoC-A 验收行**——游戏交换链的 vtable 与 dummy1 同类 ⇒ 槽 8 直接命中 |

**三个「没出现」的行同样有价值**：

- **无 `★ 工厂拦截`** ⇒ 游戏交换链没走我方五口径工厂创建（仍由 renderdoc 内部路径创建），
  但其 vtable 与 dummy1 **同类** → vtable 层直接命中 ⇒ 印证 §7.5 结论「**★ 中否不影响计数**」；
  连带 `探针A` / `g_gameSc` / `阳性确认(★游戏…)` 未触发（不阻断验收）。
- **无 `告警:` 行** ⇒ 6s 即触发，120s 兜底未走到。
- **第 1 次 Present 的来源是 `经 Present`（vtable 层）而非 `经 函数detour`** ⇒ 本局 vtable 层
  先命中、TLS 去重把方案B 层的计数吃掉 ⇒ **方案B 层是否独立有效本局无法证明**（它是
  renderdoc 未在场 / GFE 绕行时的兜底），属已知而非缺陷。

**待复跑（无 renderdoc / GFE 在场局）的预期**：游戏交换链为真 dxgi 类 ⇒ `★` 应中、
`方案A` 打 `vtable 不在 renderdoc 包装类 — 跳过`、方案B 候选 1 直接是 `dxgi+0x19000`
（14B 守卫匹配即 RAW 装上）；两局合起来构成双环境闭环。

**结论**：最高风险项 #1「渲染管线可否 Hook」第一环——**Present 可拦**——在带 renderdoc
环境实证成立（此前 v1.0~v1.6 的 0 计数确系槽位错挂，非机制不可行）。下一步进 **PoC-B**：
在 Present 内创建 Vulkan instance/swapchain → 游戏窗口出清屏/三角形 → F12 抓帧 →
`rdc_run` 提取链 → `rdc_compare -Base S4 -Cand <候选>`。

### 7.7 v1.7 第二局判读：无 renderdoc 局 + 双环境闭环（2026-10-03 13:59）

**环境切换**：`capture-helper.dll` 暂时改名停用（跑完已恢复到 `Data\SKSE\Plugins\` 与
`poc-presenter.dll` 并列）⇒ 日志 `等待 renderdoc.dll 2s 未出现 (无 capture-helper 场景)` +
`renderdoc.dll 未加载`，交换链回到**真 dxgi 类**；GFE 覆盖层在场。**13:59:30 ★ 命中（安装后
4s）、13:59:32 第 1 次 Present（6s），6001 次 / 近600帧恒 60.0 FPS，无告警、无槽位被改写**。

**本局逐行判读（与 §7.6 上局一一互补）**：

| # | 日志行 | 判读 |
|---|---|---|
| 1 | `dummy1(ForHwnd): vtable=dxgi+0xCD688 来自 dxgi.dll`、设备链/工厂 vtable 全 dxgi | **类分叉验证成立**：无 renderdoc 时我方与游戏同持真类（上局全在 renderdoc 包装类） |
| 2 | `slot8 原值=dxgi+0x19000`、`slot22 原值=dxgi+0x194A0`；`slot10/15/16/24` 原值全 dxgi | 交换链**与工厂**新槽位在真类上全部读对、挂上 |
| 3 | **`探针: Present(slot8) 原实现前32字节 @ dxgi+0x19000 = 48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 90 …`** | **三方互证闭合**：前 14B 与 `dxbytes.ps1` / §7.5 表逐字节一致，第 15 字节起恰是判据 `48 8D 6C 24 90`（边界 14）；Present1 前 14B 末字节 `54` vs Present `56` 亦吻合 ⇒ 上局探针 B 打到的是 renderdoc 32B 序言存根，**"读到什么取决于类"，这正是字节守卫必须逐候选的原因** |
| 4 | `方案A(安装期解包): 对象 … vtable 不在 renderdoc 包装类 — 跳过`（安装期两处 + ★处各一次） | 按设计跳过，非包装类不适用偏移 |
| 5 | **`方案B Present 候选 1/1: dxgi+0x19000 来自 活交换链vtbl[8]` → `RAW 已装 窃取=14B`**（Present1 同理 `候选 1/1` → `RAW`） | **靶子候选①直接命中**（本局候选只有 1 个：方案A 无贡献）——与上局「候选①被守卫跳过、靠候选②（方案A真vtbl）」**正好互补，两条候选路径都被实测走通** |
| 6 | **`★ 工厂拦截 CreateSwapChain → … title="Skyrim Special Edition"`** + `阳性确认 (★游戏 CreateSwapChain): vtbl[8]/vtbl[22] ==我们的钩子 ✓` | **上局缺的那环补齐**——游戏真实创建路径经我方挂的槽 10 被拦、就地确认槽 8/22 已是我们的钩子 |
| 7 | `探针: 游戏交换链 vptr=dxgi+0xCD688 (基线)` → `探针: 游戏交换链 slot8=…poc-presenter (我们的钩子 ✓)` | 探针 A 全链生效：基线 + 槽位自证 |
| 8 | **`第 1 次 Present (经 Present): vtable=0x7FFE994FD688 来自 dxgi.dll \| 1920x1080 … title="Skyrim Special Edition"`** | **验收行**——真 dxgi 类上直接命中 |

**两局通道覆盖矩阵（合起来 = 双环境闭环）**：

| 通道 | 上局（带 renderdoc） | 本局（无 renderdoc / GFE） |
|---|---|---|
| 同步安装 / 五口径工厂 / 设备链 GetParent | ✓ | ✓ |
| 槽 8/22 + 阳性确认 | ✓（renderdoc 类） | ✓（dxgi 类）+ **★ 处再证一次** |
| 方案A `[obj+0x28]` 解包 | ✓ **成功**（真vtbl=dxgi+0xCD688） | ✓ **按设计跳过**（非包装类） |
| 方案B 函数级 detour | 候选②方案A真vtbl → `RAW 14B` | **候选①活交换链 → `RAW 14B`** |
| ★ 工厂拦截 + 探针A | 未触发（游戏交换链由 renderdoc 内部创建，vtable 同类直击） | ✓ 中 + 基线/钩子自证 |
| 计数 | 7201 次 / 60.0 FPS | 6001 次 / 60.0 FPS |
| 告警 / 槽位被改写 | 无 / 无 | 无 / 无 |

**一处诚实的未证项**：两局的第 1 次 Present 都是 `经 Present`（vtable 层），**方案B 层的
"独立计数"始终未被观察到**——只要 vtable 层挂上，它恒先命中、TLS 去重吃掉函数层的计数。
方案B 的验证止于「**靶 = 真 Present 入口、14B 字节守卫通过、RAW 已装 trampoline**」，
它作为"某环境 vtable 类挂不上时的兜底"这一角色无法在当前两局内单独计数证明；
若要实测，需临时停用 vtable 挂钩（或加一条只让函数层计数的开关）——**优先级低，留作 PoC-B
之后的可选项**。

> **→ 已清（2026-10-03 15:56 局，插件 0.9.2，见 §7.9）**：`poc-presenter.ini` 写
> `vtable=0` 实测——第 1 次 Present 变成 **`经 函数detour`**、3001 次计数全在函数层、
> ~60 FPS 无告警。上面这段按原貌保留，作为"当时确实没证"的记录。

**总判**：风险项 #1 第一环「Present 可拦」在**双环境（renderdoc 包装类 / 真 dxgi 类）实证闭合**，
七条通道全部在真实游戏里走通过，v1.0~v1.6 的 0 计数确系槽位错挂。**→ 进 PoC-B**：
在 Present 内创建 Vulkan instance/swapchain → 游戏窗口出清屏/三角形 → F12 抓帧 →
`rdc_run` 提取链 → `rdc_compare -Base S4 -Cand <候选>`。

### 7.8 PoC-B 判读：Vulkan 离屏渲染的像素写进游戏呈现帧（2026-10-03 15:13–15:33）

**架构回顾**（详见 README「PoC-B 三步验收」）：Present 钩子内**不开 Vulkan swapchain**
（不与游戏 / RenderDoc / GFE 争 HWND）→ 离屏 512×512 洋红清屏 + 渐变三角形 →
`vkCmdCopyImageToBuffer` 读回 host buffer → `UpdateSubresource` + `CopySubresourceRegion`
到 backbuffer `(16,16)` → 调原 Present。命令缓冲**录一次逐帧复提交**，任一步失败即关注入、
游戏照常呈现。

#### 7.8.1 首局（v0.9.0）失败 → 根因与 0.9.1 修复

| 项 | 内容 |
|---|---|
| 现象 | `PoC-B 失败: 实例级函数表不完整 (vkGetInstanceProcAddr 返回空) code=0 — 已关闭注入, 游戏照常呈现`（游戏不崩、60 FPS、PoC-A 计数正常） |
| 根因 | v0.9.0 把**设备级** `vkGetDeviceQueue` 混进了 `vkGetInstanceProcAddr` 之后的必查项——规范允许对设备级命令返回 NULL（renderdoc in-app 包装层更倾向如此）；设备级本该等 `vkCreateDevice` 后走 `vkGetDeviceProcAddr` 重装 |
| 修复（0.9.1，commit `30e0067`） | ① 拆出 `POCB_INST_REQ_FNS` 必查 7 项（全实例级），`vkGetPhysicalDeviceProperties2` 降为可选；② 实例级/设备级两级都改 X-macro 全表校验，**空指针按函数名落日志**、各加一行 OK 正查；③ 版本号 0.9.0→0.9.1 以便日志段首区分新旧 DLL |
| 首局副产物 | 该局注入未启用 → `captures\PoC-B.rdc` 被像素探针判 `SENTINEL_ABSENT`（阴性），**证明候选文件上的阴性判定可信**——下一局同一命令翻成 `SENTINEL_FOUND` 即为阳性 |

#### 7.8.2 次局（v0.9.1）日志：PoC-B 全绿

```
==== poc-presenter v0.9.1 (PoC-A v1.7 验收通过 + PoC-B v0.1: Vulkan 离屏渲染 → 读回 → 注入 backbuffer) ====
[15:13:04] PoC-B init: 开始 (首次 Present 触发, 一次性)
[15:13:04] PoC-B init: backbuffer = 1920x1080 format=28 (期望28=R8G8B8A8_UNORM)
[15:13:05] PoC-B init: 实例级函数表 OK (必查 7/7)
[15:13:05] PoC-B init: GPU = "NVIDIA GeForce GTX 1660 Ti" LUID匹配=是 apiVer=1.4
[15:13:07] PoC-B init: 设备级函数表 OK (POCB_DEV_FNS 全查)
[15:13:07] PoC-B init 完成: 离屏 512x512 (洋红清屏+三角形) → 读回 → CopySubresourceRegion 到 backbuffer (16,16)
[15:13:07] PoC-B 第 1 帧注入: 渲染+读回+拷贝 7.14 ms → CopySubresourceRegion(16,16 512x512) 已提交
[15:14:48] PoC-B 注入 3600 帧, 累计均值 6.45 ms/帧 (近帧 6.94 ms) | Present 已触发 3601  近600帧 60.0 FPS
[15:15:23] PoC-B 注入 4200 帧, 累计均值 6.47 ms/帧 (近帧 6.16 ms) | Present 已触发 4201  近600帧 17.3 FPS
```

**判读**：LUID 匹配成功（选中同一块 GTX 1660 Ti）→ `apiVer=1.4`（1.1 实例成功，无需回退 1.0）；
**4200 帧注入零失败、零告警**；均值 **6.47 ms/帧**（含 Vulkan 渲染 + 读回 + 拷贝），近 600 帧恒
59.5–60.0 FPS ⇒ 60 FPS 预算内（末条 17.3 FPS 是 F12 抓帧瞬间的 RenderDoc 开销，非注入所致）。

#### 7.8.3 三步验收结果

| 步 | 判据 | 实测 |
|---|---|---|
| ① 游戏内目视 | 左上角 512×512 洋红块 + 三角形 | ✓ **两个场景都出现**（S5 夜战精准位、白漫水池） |
| ② 像素探针 | `verdict=SENTINEL_FOUND` | ✓ **两次全中**（见下表） |
| ③ 结构配对 | `copy +1` 指纹 + 其余同构 | ✓ **Copy 指纹精确 +1**；其余锚点差异全部由图证归因场景动态（见 7.8.5） |

**② 探针明细**（`tools/rdc_pass7_pixels.py`，errors=0）：

| 帧 | 四角 (22,22)(521,22)(22,521)(521,521) | 三角形重心 (272,313) | 对照点 (1056,540)/(1900,1060) |
|---|---|---|---|
| `S5b.rdc` | 全 `(255,0,255)` | `(86,217,85)` vs 期望 `(85,217,85)` | `(47,63,58)` / `(13,31,29)` 非洋红 |
| `S6-pool.rdc` | 全 `(255,0,255)` | `(86,217,85)` | `(65,87,85)` / `(45,69,69)` 非洋红 |

两帧 backbuffer 均按 `ResourceType.SwapchainImage` 权威身份识别（1920×1080 R8G8B8A8_UNORM）。
三角形重心差 1/255 来自三顶点色（绿 `0,1,0` / 黄 `1,1,0` / 蓝 `0,0.55,1`）光栅化插值的舍入。

**抓帧与命名**：`S5.rdc`→`S5b.rdc`、`水边.rdc`→`S6-pool.rdc`（改名以符合 `rdc_run`/`_SCENES`
的场景 ID 规范：`-Scene S5b` 会直取 `captures\S5b.rdc`，而 `-Scene S5` 恰好映射到 10/2 的
基线 `S5-night-combat.rdc`，**不改名就会误抓基线**）。

#### 7.8.4 提取链看到的注入痕迹（pass5）

```
Copy ev32355: ResourceId::504(1920x1080 D24S8_TYPELESS)   -> ResourceId::563(...)      ← 游戏自己的
Copy ev45903: ResourceId::364(1920x1080 R16G16B16A16_FLOAT) -> ResourceId::367(...)     ← 游戏自己的
Copy ev49979: ResourceId::504(1920x1080 D24S8_TYPELESS)   -> ResourceId::509(...)      ← 游戏自己的
Copy ev50446: ResourceId::17759(512x512 R8G8B8A8_UNORM)    -> ResourceId::78(1920x1080 R8G8B8A8_UNORM)  ← 我们的
```

末条的源正是我方 512×512 离屏图，目标 `ResourceId::78` **就是像素探针读的那张 SwapchainImage**
——渲染端（Vulkan）与呈现端（D3D11）在同一个资源 id 上对上了。

#### 7.8.5 结构配对 `rdc_compare -Base S5 -Cand S5b`（pass=3 / diff=7）

| 锚点 | 结果 | 判读 |
|---|---|---|
| `textures.formats` | **PASS** 16 = 16 | 纹理格式集合不变 |
| `cs.dispatch-profile` | **PASS** 98 次 / 18786750 线程 / 9 种组尺寸逐项相等 | 计算着色器档案不变 |
| `cs.bytecode-hash` | **PASS** 9 个 CS 块全等 | CS 字节码逐字节不变 |
| `counts.pass1` | DIFF：copies **3→4**、draws 3809→4218、clears 23→22、pso 157→151 | **copies +1 即注入指纹**；其余见 7.8.6 |
| `copy.sequence` | DIFF：候选两段序列尾部**各追加一条 `512x512 R8G8B8A8_UNORM -> 1920x1080 R8G8B8A8_UNORM`** | 与 §7.8.4 互证，指纹在序列层面可见 |
| `conditional-nodes` | DIFF：`copies_total 3→4`（其余 5 项全等） | 同一指纹的第二个计数口径 |
| `pass5.structure` / `clear.*` / `transparent.draws` | DIFF（42→41 段、透明段 122→161） | 场景动态，见 7.8.6 |

#### 7.8.6 差异归因：整帧图证（新工具 `tools/rdc_dump_backbuffer.py`）

为了不靠嘴说"那是场景动态"，写了 `rdc_dump_backbuffer.py`：把抓帧末态 backbuffer 整张导出
PNG（无 PIL，手写 PNG；`SwapchainImage` 权威身份 + `GetTextureData` 末态整图；约 **7 s/张**）：

| 图 | 内容 |
|---|---|
| `docs/analysis/S5-backbuffer.png` | 10/2 基线：**火焰战斗中**（Sigurd 燃烧、`Sigurd: Agggghh!` 字幕、罗盘带敌对标记） |
| `docs/analysis/S5b-backbuffer.png` | 今日候选：**同机位但对话状态**（Anoriath 对话字幕、无战斗、罗盘无标记）+ **左上角洋红块与渐变三角形** |
| `docs/analysis/S6-backbuffer.png` | 白漫水池候选：同样带洋红块 + 三角形 |

两图并排即得结论：**同机位、不同时刻**——draws +409 / 透明段 +39 / 分段 42→41 / PSO 157→151 /
clears 23→22 全部可归因于**游戏状态本身不同**（人物、粒子、字幕、光照档），而非注入：
注入侧**一个 D3D11 Draw/Dispatch/Clear 都不发**（代码里只有 `UpdateSubresource` +
`CopySubresourceRegion` + 调原 Present），故 draw 类差异不可能来自注入；**唯一可归因给注入的
差异就是那 +1 条 Copy**（三处口径互证：`counts.pass1.copies`、`copy.sequence`、
`conditional-nodes.copies_total`）。

#### 7.8.7 结论与遗留

**结论**：最高风险项 #1 的第二环——「**写**」——**实证闭合**：Vulkan 离屏渲染的像素经
D3D11 `CopySubresourceRegion` 落进游戏 backbuffer，被游戏 Present 呈现（目视 ✓），
被 RenderDoc F12 抓帧记录（探针 `SENTINEL_FOUND` ×2 ✓），在结构比对里留下唯一且可归因的
`+1 Copy` 指纹（✓）。叠加 §7.6/§7.7 的第一环「Present 可拦」双环境闭环，**风险项 #1 的两环
都已闭合**。

**遗留（低优先级，均不阻塞）**：
1. **同日同场景噪声底**未测——现有基线是 10/2 的另一场战斗，若要「唯一差别=注入」的严格数字，
   可跑一次对照局：`<pluginDir>\poc-presenter.ini` 写 `vulkan=0`（逃生门，**至今未实测**）→
   同机位 F12 → `rdc_compare -Base S5c -Cand S5b`。预期只剩 Copy +1，顺带首测逃生门。
2. **方案B 函数层独立计数**仍未被观察（沿用 §7.7 末的说明）。

> **→ 两条均已于 2026-10-03 15:56 对照局清零，实测见 §7.9**：
> ① `S5c.rdc` 探针 `SENTINEL_ABSENT` + 整帧 PNG 无洋红 + pass5 只有 3 条 Copy ⇒ 干净对照帧
> 落袋，`rdc_compare -Base S5c -Cand S5b`（同日同机位、唯一差别=注入）pass6/diff4，
> copy 两锚点是**唯一**可归因注入的差异；
> ② `vtable=0` 下第 1 次 Present = **`经 函数detour`**、3001 次计数、~60 FPS 无告警。
> 唯一没证到的是逃生门那一行日志（`vulkan=0` 分支没被执行到，原因见 §7.9.5 的缺口），
> 已在 0.9.3 修掉并留作可选项——这两条遗留本身已不复存在。

### 7.9 对照局 + 方案B 独立计数实测（2026-10-03 15:56–15:57，插件 0.9.2）

#### 7.9.1 实验设置：一条跑局清两条遗留

`poc-presenter.dll` 换 0.9.2（CI run `37107784970`），`<pluginDir>\poc-presenter.ini` 同时写两键：

```
vtable=0    ← 停用 vtable 层（不挂交换链槽 8/22、看门狗不"打回"），Present 只能从方案B 的
              函数级 detour 进来 → 观察方案B 独立计数
vulkan=0    ← 逃生门：关掉 PoC-B 注入 → 拿同日同机位的无注入对照帧
```

进 S5 夜战精准机位 → F12 → `captures\S5c.rdc`（846.9 MB，15:57:09）。

#### 7.9.2 日志判读（0.9.2 段，log 行 215–331）

| 行 | 日志原文（节选） | 判读 |
|---|---|---|
| 216 | `PoC-A: ini vtable=0 → vtable 层停用 (不挂槽 8/22, 看门狗不打回), 只留方案B 函数级 detour —— 方案B 独立计数实验` | 开关被读到并生效 |
| 232 起 ×N | `vtable=0 (ini) → 不挂槽 8/22, 本 vtable 交由方案B 覆盖` | 每个登记点（dummy1/2、QI、设备链、工厂入口）都不挂 |
| 293 | `登记完成: 交换链 vtable 1 个 (vtable=0 未挂 Present), 工厂 vtable 1 个 (含创建方法挂钩); Present 钩子=00007FFE716DA920` | 工厂钩子（喂方案B 候选）按设计保留 |
| 294 | `阳性确认 … vtbl[8]=00007FFDBE5BD670 来自 …renderdoc.dll ≠我们的Present钩子 (vtable=0 预期内: 本局不挂槽, 由方案B detour 覆盖)` | 槽位确实**没**被挂上——不是开关没生效 |
| 305/306 | `方案B Present 候选 1/2: … 活交换链vtbl[8] (renderdoc.dll)` → `入口字节与预期不符且非可识别跳板, 保守跳过; 前16字节=48 89 5C 24 08 …` | **14B 字节守卫照常工作**：第一候选是 renderdoc 的 32B 存根序言，与我方 14B 预期不符 → 正确放弃 |
| 307/308 | `候选 2/2: 方案A真vtbl[8] (dxgi.dll)` → **`方案B Present: RAW 已装 target=00007FFE99449000 … 窃取=14B (入口 5B E9)`**（Present1 同理） | 靶 = `dxgi+0x19000` / `+0x194A0`，与 §7.5 表一致 |
| **318** | **`第 1 次 Present (经 函数detour): vtable=00007FFE994FD688 来自 C:\WINDOWS\SYSTEM32\dxgi.dll \| 1920x1080 … title="Skyrim Special Edition"`** | **验收行**——口径从 `经 Present` 变成 `经 函数detour`，方案B 函数层成为唯一 Present 入口 |
| 327–331 | `Present 计数 601/1201/1801/2401/3001`，近600帧 `49.8/59.9/60.0/58.8/48.0 FPS` | 3001 次全由函数层计数，~60 FPS，**无 `120s 仍 0 次 Present` 告警**、无槽位被改写 |
| 全段 | **无任何 `PoC-B` 行** | 见 §7.9.5——这正是发现的缺口 |

#### 7.9.3 对照帧 `S5c.rdc` 四路互证：确实无注入

| 证据 | 结果 |
|---|---|
| 像素探针 `rdc_pass7_pixels.py -Scene S5c` | **`SENTINEL_ABSENT`**、errors=0：四角 `(49,96,114)`/`(9,35,47)`/`(14,39,49)`/`(21,49,58)`、重心 `(49,92,112)`、对照点 `(0,10,16)`/`(12,30,28)` |
| 整帧图 `docs/analysis/S5c-backbuffer.png` | 直接解 PNG 字节逐点核对：与探针**逐点一致**、无洋红（S5b 同法核对 = `(255,0,255)`×2 + 重心 `(86,217,85)`，两文件 SHA1 不同——**图证以字节为准**，曾出现展示图串档） |
| pass5 提取 | 只有 3 条 Copy（`ev33083`/`ev46607`/`ev50673`，全是游戏自有的 1920x1080 内部拷贝），**无 `512x512 → backbuffer`** |
| 日志 | 无 `PoC-B init` / `PoC-B 注入` 行 |

#### 7.9.4 三对结构配对：噪声底 vs 唯一差别=注入

| 配对 | 口径 | copy 两锚点 | 其余 DIFF（归因） |
|---|---|---|---|
| `S5 → S5b`（跨日，含注入） | pass3 / diff7 | **DIFF**：copies 3→4、sequence 追加、copies_total 3→4 | draws 3809→4218 (+409)、clears 23→22、段 42→41、透明 122→161 |
| `S5 → S5c`（跨日，**双无注入**） | pass5 / diff5 | **PASS**（copies 3=3、sequence 全等） | draws 3809→4273 (**+464**)、clears 23→22、pso 157→149、textures 538→567、段 42→41、透明 122→161、clear 两档 DIFF |
| **`S5c → S5b`（同日同机位，唯一差别=注入）** | **pass6 / diff4** | **DIFF**：copies **3→4**、sequence 尾部追加 `512x512 R8G8B8A8_UNORM -> 1920x1080 R8G8B8A8_UNORM`、copies_total **3→4** | draws 4273→4218 (**−55**)、pso 149→151、textures 567→538、段 41=41；**clear.bound / clear.target / transparent.draws (161=161) 三项由跨日的 DIFF 转为 PASS** |

**结论**（把三行并排读）：

1. 注入的净效应 = **恰好 +1 条 Copy**，且只在这一个口径上 DIFF——无注入对照对里它 PASS，
   两条锚点 + pass5 日志三口径互证；
2. 跨日噪声（双无注入）里 draws **+464**、clear/透明段全 DIFF → 这些在 S5~S5b 里出现过的
   DIFF **全部在对照对里同样出现**，因此不可能归因注入；同日配对后 draws 差收到 **−55**、
   clear/透明转 PASS，噪声随"同时刻"收敛；
3. 注入侧零 Draw/Dispatch/Clear（代码只有 `UpdateSubresource`+`CopySubresourceRegion`+调原
   Present），与上述归因一致。

#### 7.9.5 顺带发现的缺口：函数层 detour 没驱动 PoC-B（0.9.3 已修）

- **现象**：0.9.2 段没有一行 `PoC-B`，连预期的 `PoC-B: poc-presenter.ini 关闭了 vulkan 注入
  (vulkan=0)` 都没打——说明 `pocbEnabled()` 根本没被调用过。
- **根因**：`pocbFrame()` 只在 `hookedPresent` / `hookedPresent1`（vtable 层）里调用；
  `vtable=0` 时 Present 走函数层 detour，它只 `notePresent` 就转发 → **兜底模式下 PoC-B 永远
  不会注入**（哪怕 `vulkan=1`），逃生门也永远走不到。
- **对本局结论的影响**：无，反而更稳——S5c 的"无注入"由两重保险共同达成（ini 关 + 该层压根
  不调 `pocbFrame`）；但**逃生门 `vulkan=0` 至此仍未被运行时执行过**，须诚实标注。
- **修复（0.9.3，commit `2fba20b`）**：`detouredPresent` / `detouredPresent1` 在
  `!g_inVtableHook` 分支里补 `pocbFrame(sc)`；经 vtable 转来的调用 TLS 置位，不会二次注入。
  CI run `37109051718` 编译中，**未跑局验证**——下次跑局可顺带看
  `关闭了 vulkan 注入` 那行（可选项，不阻塞任何结论）。

#### 7.9.6 本节状态

§7.7 的「方案B 独立计数未证」✓ 清零；§7.8.7 遗留 ① 噪声底 ② 方案B 计数 ✓ 双清零；
仅剩「逃生门运行时验证」一项可选观察（0.9.3 起才有机会走到，见 §7.9.5）。
