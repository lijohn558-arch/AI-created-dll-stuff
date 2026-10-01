# RenderDoc 抓帧操作清单（第一阶段执行手册）

> 用途：Skyrim SE 原版 DX11 管线基线抓帧 + 后续 DX11↔Vulkan 对照帧
> 原则：**基线抓帧必须纯净环境（不装 CS/ENB/ReShade）**，探索性抓帧可用 CS 内置
> 分工提醒：本步骤全部在本地执行（测试），不涉及 GA 编译

---

## 1. 工具准备

| 工具 | 要求 | 获取 |
|---|---|---|
| RenderDoc | **64-bit** 最新 stable（Skyrim SE 是 64 位进程，必须用 64 位版），已确认本机装于 `C:\Program Files\RenderDoc\`（1.46） | https://renderdoc.org/builds |
| MO2 (Mod Organizer 2) | 若游戏本体通过 MO2 启动（大多数 SE 环境） | nexusmods |
| SKSE64 | 与游戏版本匹配 — **已确认：SKSE64 2.0.20** | skse.silverlock.org |
| 游戏本体 | 记录精确版本号 — **已确认：SE 1.5.97.0**（`D:\The Elder Scrolls V Skyrim\`） | 启动器 / `skyrimse.exe` 属性 |

> 若游戏不使用 MO2（Steam 直启），RenderDoc 可直接 Launch `skse64_loader.exe`，跳过第 2 节部分配置。

### 本机环境备注（2026-10-01 勘查）
- ENB/ReShade **仅有残留配置**（`enbseries/`、`reshade-shaders/`、`enb*.ini`），根目录**无代理 DLL**（`dxgi.dll`/`d3d11.dll` 均不存在）→ 实际未生效，抓帧前清理残留配置即可
- RenderDoc UI 外部注入启动崩溃（c0000005，模块 unknown）→ 根因未定（NvAPI / 注入链冲突），已用 **capture-helper 插件方案**绕开（见 §2.3）

---

## 2. RenderDoc 启动配置（外部 Launch，原版基线用）

RenderDoc → **File → Launch Application**，填入：

| 字段 | 值 |
|---|---|
| Executable Path | `PATH/TO/ModOrganizer.exe` |
| Working Directory | `<Steam>/steamapps/common/Skyrim Special Edition` |
| Command-line Arguments | `--log run "<Steam>/steamapps/common/Skyrim Special Edition/skse64_loader.exe"` |
| ☑ **Capture Child Process** | **必须勾选**（RenderDoc 挂在 MO2 上，真正的 D3D11 设备在子进程） |
| Capture file path template | 设为 `C:/captures/SkyrimSE/<场景ID>/frame`（便于管理） |

### Launch 选项（Capture Options，对应 RenderDoc 1.46 官方文档）
- ☑ **Capture Child Processes：开**（MO2 子进程注入的关键）
- ☑ Allow Fullscreen：开
- ⬜ **Enable API validation：关**（旧版叫 "Debug Device"，新版已移除合并至此项，我们不需要）
- Ref All Resources：关（开启会使 .rdc 暴涨）
- Capture All Cmd Lists (D3D11)：默认关；**若抓帧失败提示 deferred command list 缺失再开**
- 其余选项（VSync / Seconds Delay / Callstacks / Verify Buffer Access / Auto start / Queue Capture of Frame）保持默认
- 配置完成后点 **Save Settings** 保存为 `.cap` 文件，以后一键加载

> **多帧抓取**：新版无 "Frame Cap" 数量框，替代方式 = **快速连按 F12 多次**（每次存一个 .rdc，按时间顺序），或用 `Queue Capture of Frame` 填帧号精确抓取

### 2.3 方案 B：capture-helper 插件抓帧（本机实测外部注入崩溃后的替代路径）

本机外部注入启动崩溃（c0000005）后启用的方案，**绕开整个注入链**：

1. GitHub Actions 构建 `capture-helper.dll`（仓库 README 有完整步骤）
2. dll 放入 `Data/SKSE/Plugins/`，`renderdoc.dll` 放入 `Data/Renderdoc/` 或用 ini 指定
3. 正常启动游戏（SKSE 路径）→ 插件进程内加载 RenderDoc 并设置 NvAPI 白名单
4. **F12 / PrintScreen** 直接抓帧 → `Data/SKSE/Plugins/captures/*.rdc`
5. RenderDoc UI 打开分析（只用 UI 的分析功能，不用其 Launch）

优点：无注入链、NvAPI 选项可达（UI 无此选项）、环境信息自动写入 `capture-helper.log`（版本号可直接抄进记录表）。

> 两条路线抓到的 .rdc 格式完全相同，分析流程（§8）不变。

### ⚠️ 混合显卡（笔记本双显卡）崩溃预案
若游戏在**第一次 Present 时崩溃**（NvAPI 被 RenderDoc 禁用导致，CS 源码记载的已知坑）：
- 该问题的修复选项 `Allow unsupported vendor extensions` **无官方 UI 复选框**（CS 通过 `SetCaptureOptionU32(0x10DE)` API 调用）
- 处理顺序：① 升级 RenderDoc 检查有无此选项 → ② 用 CS 内置 RenderDoc 抓帧兜底（自动处理）→ ③ 请求提供 API 小启动器

---

## 3. 抓帧前的纯净环境检查（关键！）

基线抓帧要求"原版管线"，抓帧前逐项确认：

- [ ] **Community Shaders 未安装**（或至少完全移出 MO2 模组列表）
- [ ] **ENB / ReShade / Reshade 类注入器未安装**（会破坏 RenderDoc 注入，也会改写渲染）
- [ ] **D3D11 Mod 未安装**（任何 Hook D3D 的 mod：SweetFX、各种 FPS 美化类）
- [ ] 关闭帧生成 / 超分辨率（若安装了 DLSS/FSR mod，卸载或设为关闭——CS 文档明确警告抓帧不兼容）
- [ ] 关闭游戏内**垂直同步**（不影响内容，但便于测帧时间）
- [ ] 分辨率固定为一个**基准分辨率**（建议 1920×1080，全局画质设置截图存档）
- [ ] 图形设置固定（阴影质量、SSAO、反射、水面等——记录到 §6 记录表）

> 纯材质/模型/美工类 mod（不影响管线结构）可以保留，但必须在记录表中列明。

---

## 4. 固定测试场景（基准场景集）

目的：可重复、覆盖各渲染阶段、包含已知问题点。**每个场景做成固定存档 + 固定机位**。

### 4.1 场景清单

| 场景 ID | 内容 | 覆盖的渲染阶段 | 制作方法 |
|---|---|---|---|
| `S1-city-day` | 白天白漫城中心 | 几何+光照+阴影+NPC+UI | `coc whiterun`，正午，`tcai` 关 AI |
| `S2-interior` | 地牢内部（火把/点光源） | 点光源阴影+暗部光照 | 固定地牢入口 `coc` |
| `S3-forest-godrays` | 树林+阳光穿射 | 体积光+植被+LOD | 白天树林，太阳角度固定 |
| `S4-water` | 湖面/河边 | 水面反射/折射/流动 | 对准水面 |
| `S5-night-combat` | 夜晚战斗（法术粒子） | 粒子+动态光+透明混合 | 夜间，触发战斗 |
| `S6-stutter-spot` | **你观察到固定掉帧的位置** | 已知问题点（重点） | 定位后存档 |
| `S7-heavy-mods`（可选） | 高负载场景 | 压力测试 | 4K 材质+密集光 |

### 4.2 场景固定化操作（游戏内控制台）
```
tfc                # 自由相机，摆好固定机位后记录坐标
tcai               # 关闭 AI（场景稳定，帧内容可复现）
sgtm 0.2           # 慢速时间（可选，稳定动态内容）
set timescale to 1 # 时间流逝固定（光照角度可复现）
```
- 用 **截图 + 控制台 `player.getpos X/Y/Z` 记录坐标**
- 在该点**保存一个专用存档**（命名 `BENCH_S1` 等），以后每次读档复现场景
- 相机朝向：记录或用截图比对（RenderDoc 只抓一帧内容，朝向必须一致才有对照意义）

### 4.3 每场景抓帧数量
- 基线：**每场景 1 帧**（静止机位，内容确定）
- `S6-stutter-spot`（掉帧点）：额外抓 **多帧序列（3-5 帧）**，观察编译/加载事件 —— 可用 RenderDoc 的 multi-frame capture（热键连按或 CS 内置的帧数滑条）

---

## 5. 抓帧步骤（每个场景）

1. 启动：RenderDoc Launch（按 §2 配置）→ 等游戏完全加载
2. 读档到 `BENCH_Sx` 存档 → 按 §4.2 摆好机位 → 等 5 秒（纹理流送稳定）
3. 按 **F12 / PrintScreen**（RenderDoc 热键）→ Overlay 提示抓取成功
4. 若抓取多帧：**快速连按 F12 多次**（每按一次存一个 .rdc，按时间顺序），或用 `Queue Capture of Frame` 填帧号精确抓取
5. 退出游戏 → RenderDoc 自动打开 .rdc → **立即另存到归档目录**（§7）
6. 填写 §6 记录表

### 掉帧点（S6）特别流程
1. 先**不带 RenderDoc** 走一遍该位置，用 Performance Overlay / FRAPS 确认掉帧复现
2. 带 RenderDoc 走同一路径抓帧（行走路线、速度尽量一致）
3. 注意：RenderDoc 本身有开销，改变时序，**掉帧原因可能在抓帧环境下不复现**——此时改用"逐点抓帧"（停在掉帧点各抓一帧）对比前后帧内容差异

---

## 6. 抓帧记录表（每个 .rdc 一份，Markdown）

```markdown
- 场景 ID: S6-stutter-spot
- 日期: 2026-10-01
- 游戏版本: 1.6.xxxxx | SKSE: x.xx.x
- GPU/驱动: NVIDIA xxx / xxx.xx（驱动版本对编译行为影响大，必须记）
- 分辨率/画质设置: 1920x1080 / [附截图链接]
- mod 列表（若非纯净）: [列表或"纯净"]
- 相机坐标: X= Y= Z= | 朝向: [截图]
- 抓帧方式: RenderDoc 外部Launch / CS内置
- 帧数: 1 / multi(3)
- 备注: （掉帧点：本次抓帧环境是否复现掉帧？）
```

---

## 7. 数据管理约定

```
skyrim-vulkan/
├── docs/
│   ├── 00-总体规划-实施步骤.md
│   ├── 01-CommunityShaders项目调研.md
│   ├── 02-RenderDoc抓帧操作清单.md      ← 本文件
│   └── analysis/                        ← 每个抓帧的分析笔记（提交到 git）
│       ├── S1-city-day.md
│       └── S6-stutter-spot.md
├── captures/                            ← .rdc 原始文件（**加入 .gitignore，不提交**）
│   └── S1-city-day/frame_000.rdc
```
- .rdc 单文件数百 MB ~ 数 GB：**本地保存，不进 git**
- 分析结论（事件列表、RT 格式、Pass 顺序）提炼成 markdown 放 `docs/analysis/`，进 git

---

## 8. 每个 .rdc 的提取清单（分析时逐项填写）

抓完之后要从 RenderDoc 里挖出的信息（对应总体规划 §1.3 和 §4）：

- [ ] **Event Browser 全景**：一帧内的 Pass 分组（shadow → geometry → lighting → post → UI），各 Pass 的 draw call 数量
- [ ] **渲染目标清单**：每个 RT 的格式/尺寸/用途（对照 `Common/GBuffer.hlsli` 验证）
- [ ] **着色器对照**：每个 Pass 的 VS/PS 与 `package/Shaders/` 哪个 .hlsl 对应
- [ ] **常量缓冲区布局**：cbuffer slot → 内容（对照 `FrameBuffer.hlsli`）
- [ ] **后处理链顺序**：IS* shader 的实际执行顺序（权威数据）
- [ ] **资源创建清单**：纹理/缓冲区的数量、格式、Usage flag（→ Vulkan 资源系统设计输入）
- [ ] **状态切换统计**：blend/depth/rasterizer 状态组合数（→ Vulkan Pipeline 数量估算）
- [ ] **掉帧点专项**：有无 `Create*Shader` 长调用（编译实锤）/ IO 阻塞 / 异常长的 Pass

---

## 9. 完成标准（第一阶段出口条件）

- [ ] §4 全部场景各 1 帧基线 .rdc + 记录表
- [ ] S6 掉帧点已定性（着色器编译 / 脚本 / 流送 / GPU 内容，四选一）
- [ ] §8 提取清单至少完成 3 个代表性场景（S1、S2、S6）
- [ ] 产出物：`docs/analysis/` 下的分析笔记 → 作为 Vulkan 管线设计（总体规划 §4）的直接输入
