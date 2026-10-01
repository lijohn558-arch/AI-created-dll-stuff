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
