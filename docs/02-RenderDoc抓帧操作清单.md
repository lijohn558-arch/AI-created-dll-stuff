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
- **抓帧启动方式（2026-10-01 定）**：通过 **MO2** 启动（快速开局模组建 `BENCH_Sx` 存档，避免每次过长开场）。内容类模组（材质/NPC/任务）允许；**着色器修改类（Community Shaders / ENB / ReShade）禁止出现在模组列表**。`capture-helper.dll`/`renderdoc.dll` 位于物理 `Data\`，MO2 VFS 正常加载；记录表须注明模组环境
- ENB/ReShade **仅有残留配置**（`enbseries/`、`reshade-shaders/`、`enb*.ini`），根目录**无代理 DLL**（`dxgi.dll`/`d3d11.dll` 均不存在）→ 实际未生效，抓帧前清理残留配置即可
- RenderDoc UI 外部注入启动崩溃（c0000005，模块 unknown）→ 根因未定（NvAPI / 注入链冲突），已用 **capture-helper 插件方案**绕开（见 §2.3，已实测通过）

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

**✅ 实测记录（2026-10-01）**：方案 B 在本机验证通过——SE 1.5.97 + SKSE 2.0.20 + RenderDoc 1.46：
- GA 构建成功（`actions/checkout@v5` / `upload-artifact@v5`，编译参数含 `/utf-8`）
- 游戏正常启动不崩溃，RenderDoc 角标可见，`capture-helper.log` 记录 NvAPI 白名单设置成功
- **F12 抓帧闭环验证通过**：`skyrimse_frame_frame1441.rdc` 生成，RenderDoc UI 可手动打开（插件抓帧为静默存盘，UI 不自动弹出属正常设计）
- 首次构建修复记录：`pRENDERDOC_GetAPI` 参数需 `RENDERDOC_Version` 枚举（不可传 int）
- 版本解码修正：SKSE `MAKE_EXE_VERSION = (major<<24)|(minor<<16)|(build<<4)|sub`（源码 `skse64_common/skse_version.h`），`0x01050610 = 1.5.97.0`、`0x02000140 = 2.0.20.0`

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
- [ ] **Photo Mode / 截图类 mod 已关闭或改键**（2026-10-01 实测教训：Photo Mode 截图键 = PrintScreen = RenderDoc 抓帧键，两者同时触发会把截图管线抓进帧里——首抓 S1 因此多了 8100 个 Dispatch（BC 块压缩 2×4050）+ 4 个 CS + 3 个 Copy，被迫重抓。**抓帧前改掉冲突键或关掉截图 mod**）
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
| `S6-stutter-spot` | ~~固定掉帧位置~~ **（挂起）** | **观察结论（2026-10-01）：仅首次走到时掉帧，后续不复现，初判远处材质/LOD 流送的一次性加载开销** → 暂不纳入基线 | 挂起，待复现时再定性 |
| `S7-heavy-mods` | ~~高负载场景~~ **（跳过，2026-10-02 定案）** | **跳过理由：① S1~S5 五次提取证明 pass 结构逐位同构（9 CS/98 dispatch 恒等），结构集合已收敛，高负载只增 Draw/光源规模、不产生结构新知（光源规模效应 S5 的 483 批数已覆盖）；② 4K 材质只改纹理尺寸/mip、不改管线结构（16 种格式 S5 已全覆盖），且属 mod 侧不进原版纯净管线；③ 性能压测属移植后验证阶段（DX11 性能数字对 Vulkan 侧参考有限）** | 不抓帧 |

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
- **UI 口径（2026-10-02 S5 确立；⑤ 号遗留同日结清）**：场景截图均在 photo mod 内拍（UI 自动隐藏），抓帧时 photo mod 已关（正常 HUD：顶部罗盘、战斗时血条）。**两者 UI 状态天然不同**——分析 .rdc 时 UI 段 Draw 数不以截图为基准（S5 抓帧帧内含罗盘+3 敌血条 = UI 段 58 Draw，S4/S2 无战斗血条同位 14/15 Draw，均属正常）。**用户确认（2026-10-02）：Photo Mode 拍照时即隐藏原版 UI → S1 首抓 vs 干净帧的 Draw +387 = 被隐藏的原版 UI，无需再做开关对照抓帧**（docs/98 §⑤）。

### 4.3 每场景抓帧数量
- 基线：**每场景 1 帧**（静止机位，内容确定）
- `S6-stutter-spot`（掉帧点）：额外抓 **多帧序列（3-5 帧）**，观察编译/加载事件 —— 可用 RenderDoc 的 multi-frame capture（热键连按或 CS 内置的帧数滑条）

---

## 5. 抓帧步骤（每个场景）

1. 启动：RenderDoc Launch（按 §2 配置）→ 等游戏完全加载
2. 读档到 `BENCH_Sx` 存档 → 按 §4.2 摆好机位 → 等 5 秒（纹理流送稳定）
3. 按 **F12**（capture-helper 热键）→ Overlay 提示抓取成功。**不要用 PrintScreen**：它与 Photo Mode 等截图 mod 冲突（会同时截图，污染帧，见 §3）
4. 若抓取多帧：**快速连按 F12 多次**（每按一次存一个 .rdc，按时间顺序），或用 `Queue Capture of Frame` 填帧号精确抓取
5. 退出游戏 → RenderDoc 自动打开 .rdc → **立即另存到归档目录**（§7）
6. 填写 §6 记录表

### 掉帧点（S6）特别流程

> **状态：挂起（2026-10-01）** — 你观察：掉帧仅首次走到时发生、后续不明显，初判为远处材质加载（流送类一次性开销）。与"着色器首次编译卡顿"要区分开（后者每次新着色器必现，是编译策略章节的主目标）。若后续复现固定掉帧，再执行以下流程：

1. 先**不带 RenderDoc** 走一遍该位置，用 Performance Overlay / FRAPS 确认掉帧复现
2. 带 RenderDoc 走同一路径抓帧（行走路线、速度尽量一致）
3. 注意：RenderDoc 本身有开销，改变时序，**掉帧原因可能在抓帧环境下不复现**——此时改用"逐点抓帧"（停在掉帧点各抓一帧）对比前后帧内容差异

---

## 6. 抓帧记录表（每个 .rdc 一份，Markdown）

```markdown
- 场景 ID: S1-city-day
- 日期: 2026-10-01
- 游戏版本: 1.5.97.0 (0x01050610) | SKSE: 2.0.20 (0x02000140)   ← 已确认，直接抄
- GPU/驱动: NVIDIA xxx / xxx.xx（驱动版本对编译行为影响大，必须记）
- 分辨率/画质设置: 1920x1080 / [附截图链接]
- mod 环境: MO2 启动 / 无着色器修改类模组（CS/ENB/ReShade 必须为无）+ [材质包等大类列举]
- 相机坐标: X= Y= Z= | 朝向: [截图]
- 抓帧方式: capture-helper 插件 F12（RenderDoc 1.46，NvAPI 白名单已开）
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

> **状态：达成（2026-10-02）** — S1~S5 五场景基线 .rdc + pass1~pass5 五轮提取（pass6 消费链/PS 反汇编 2026-10-02 补齐）+ 分析笔记全部入档；S6 挂起、S7 跳过（理由见 §4.1）。

- [x] §4 全部场景各 1 帧基线 .rdc + 记录表 → **S1/S2/S3/S4/S5 共 5 份**（S6 挂起、S7 跳过，见 §4.1）
- [ ] S6 掉帧点已定性（**挂起**：初判一次性流送加载，复现后再做 着色器编译/脚本/流送/GPU内容 四选一）
- [x] §8 提取清单至少完成 3 个代表性场景 → **实际完成 5 个**（S1、S2、S3、S4、S5，每场景 pass1~pass5 五轮；pass6 消费链/PS 反汇编 2026-10-02 补齐，见 §10）
- [x] 产出物：`docs/analysis/` 下的分析笔记 → 作为 Vulkan 管线设计（总体规划 §4）的直接输入

---

## 10. 无头批跑（qrenderdoc --py，不开 UI；2026-10-02 打通）

> 本机无 Python（exit 9009），**qrenderdoc 内嵌 Python 是唯一运行环境**。两种跑法：
> - **UI 手跑**：qrenderdoc 打开 .rdc → 菜单 Python → Run Script，print 在 Output 面板（**UI 里不能同时开抓帧**）。
> - **无头批跑（推荐）**：不开 UI，命令行走 `qrenderdoc.exe --py <脚本>`，由 runner 自动判定成败。

### 10.1 命令模板

```powershell
# 基本形（pass1~pass6 通用）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_pass6.py -Scene S5

# 指定查消费者的资源 + 导出 PS 反汇编（事件号白名单 / PS 资源 ID 白名单二选一或并用）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_pass6.py -Scene S1 `
    -Targets "501,520,324,466,585" -PsWL "15478,15607,17352,17586"
```

### 10.2 参数（`tools/rdc_run.ps1`）

| 参数 | 说明 |
|---|---|
| `-Script` | 脚本名或绝对路径（默认在 `tools\` 下找） |
| `-Scene` | S1~S5，拼出 `captures\<Scene>.rdc` 与 `docs\analysis\<Scene>-extract-*.json` |
| `-Targets` | 传 pass6：查消费者的资源 ID（逗号分隔，抓帧局部 ID） |
| `-PsEvents` | 传 pass6：直接导出反汇编的事件号 |
| `-PsWL` | 传 pass6：**PS 资源 ID 白名单**——自动在 `draw_recs` 找该 PS 首个绑定的 Draw 事件，与 PsEvents 合并去重后导出反射+反汇编 |
| `-Stride` | 逐 Draw 步长（默认 1 = 全量） |
| `-TimeoutSec` | 默认 900s；超时强杀并打印 LOG 尾 |

runner 内部：设 `RDC_SCENE/RDC_TARGETS/RDC_PSEVENTS/RDC_PSWL/RDC_STRIDE/RDC_HEADLESS` 环境变量（脚本据此读参数；`RDC_HEADLESS=1` 让脚本结尾 `sys.exit(0)`，UI 手跑时不设此变量、不会退出进程）→ `qrenderdoc.exe --py` → 扫 `docs/analysis/<场景前缀>-*` 新产出 → 逐 JSON 读 `errors`。

### 10.3 判定口径（与项目脚本一致）

- **`errors` 数组为空 = 成功**（`RESULT: SUCCESS (errors 0)`）；有错则逐条打印并 `RESULT: FAILED`。
- **runner 会因「未找到本轮产出文件」抛错**——探针/一次性脚本若不写 `docs/analysis` 属预期，此时只看 `.log` 即可。
- 实测（pass6，五场景全 errors=0）：S2 19.6s / S4 32.7s / S3 43.1s / S5 45.2s / S1 68.2s。
- 日志行会打印 `scene / targets / psEvents / psWL`，用于回溯本轮口径。

### 10.4 与 UI 手查的关系（Clear 真实目标）

**已不需要 UI API Inspector**：pass6 第 3 节 `ctrl.GetUsage()` 按资源反查 Clear 归属（S2/S4/S5 全命中），slice 级再看 `Descriptor.firstSlice`，终判用像素 md5（详见 `docs/98-遗留事项结清-2026-10-02.md` §③）。

---

## 11. 配对判定 harness（rdc_compare，2026-10-02 打通）

> `docs/03` §7.1 第③条「锚点比对走脚本而非人工目视」的落地：把 `docs/03` §3 锚点表变成 **10 条可执行判定**，读两侧提取产出（基线 DX11 vs 复跑 / 未来 Vulkan 候选），逐锚点给 PASS / DIFF / SKIP。
> 纯 stdlib Python、不 import renderdoc；本机经 `qrenderdoc --py` 运行（已验证无 renderdoc 依赖也能跑、exit 0）。

### 11.1 命令模板

```powershell
# 自比（回归自检，应全绿）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_compare.ps1 -Base S4 -Cand S4

# 跨场景（只报告差异，exit 0）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_compare.ps1 -Base S1 -Cand S3

# 配对闸门（任一锚点 DIFF → exit 1；Vulkan 候选接入后用这条）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_compare.ps1 -Base S4 -Cand <候选> -Strict
```

### 11.2 参数（`tools/rdc_compare.ps1`）

| 参数 | 说明 |
|---|---|
| `-Base` / `-Cand` | 基线侧 / 候选侧：场景 ID（`S4`）、前缀（`S4-water`）或 pass1 JSON 绝对路径 |
| `-Out` | 报告输出前缀（默认 `docs/analysis\<base>~<cand>-compare`，同时出 `.log`） |
| `-Strict` | 有 DIFF 时 exit 1（配对闸门用；默认只报告、exit 0） |
| `-TimeoutSec` | 默认 300s（超时强杀并打印 LOG） |
| `-QRenderDoc` | qrenderdoc.exe 路径（默认 `C:\Program Files\RenderDoc\`） |

### 11.3 十条锚点（全部 id-free——ResourceId 是抓帧局部量，跨帧不比句柄，见 docs/03 §5 R2）

| # | 锚点 ID | 数据源 | 比什么 |
|---|---|---|---|
| 1 | `counts.pass1` | pass1 | Draw / Dispatch / Clear / Copy / Present / PSO / 纹理总数 |
| 2 | `textures.formats` | pass1 | 纹理格式集合 |
| 3 | `cs.dispatch-profile` | pass4 | dispatch 数、总线程、线程组多重集 |
| 4 | `cs.bytecode-hash` | `S*-post-cs-disasm.txt` | 反汇编正文 sha1（丢事件号头部行）→ 逐字节等价 |
| 5 | `pass5.structure` | pass5 | 分段数 + (viewport, rt 格式串, dsv) 签名多重集 + Draw 覆盖自检 |
| 6 | `clear.bound-profile` | pass5 | 绑定态视角 Clear 画像（aspects + 绑定格式）——**辅助口径** |
| 7 | `clear.target-profile` | pass6 | **GetUsage 反查的真实清屏目标**（权威口径，docs/98 §③） |
| 8 | `copy.sequence` | pass5 | 有序 (src 格式 → dst 格式) 拷贝链 |
| 9 | `conditional-nodes` | pass5(+p6) | 512² cubemap 段 / G-Buffer MRT / 透明 MRT / 暗场景 rgba8 小段 / copy 总数 / 2048² 槽清次数 |
| 10 | `transparent.draws` | pass5 | 透明段 Draw 数 |

判定口径（沿用项目惯例）：

- 输出 JSON `summary.match = true`（无 DIFF 且 ≥1 项通过）→ `RESULT: MATCH`；
- 两侧缺哪个文件，对应锚点 `SKIP`；全 SKIP → `RESULT: NO DATA`（`-Strict` 时 exit 1）；
- `errors[]` 非空 = **脚本执行失败**（不是 DIFF，runner 直接抛错）——例如拿 errors 非空的提取 JSON 来比。

### 11.4 自测（2026-10-02）

| 用例 | 结果 |
|---|---|
| S4 vs S4 | **10/10 PASS → MATCH**（exit 0） |
| S2 vs S2（pass1~3 无头重生成后复测） | **10/10 PASS → MATCH** |
| S4 vs S2 | **10 DIFF**（室内：512² 段缺失、CS 9→5、拷贝 3→2、透明 242→159、格式 14→11…全中已知差异） |
| S1 vs S3（S1 pass4 重生成后） | 4 PASS / 6 DIFF：`cs.dispatch-profile`、`cs.bytecode-hash`（9 块逐位同）、`copy.sequence` 等跨场景共性 PASS |
| S7 vs S7（无数据） | 全 SKIP → NO DATA（`-Strict` → exit 1） |

比对本身 0.1s；单次端到端（含 qrenderdoc 启动）数秒。

### 11.5 本轮顺带修复与踩坑（复跑前必读）

- **S1 `post-cs-disasm.txt` 曾是 v1 残留（7 块）**，与 pass4 JSON（v2、9 CS）不同步 → 已 `rdc_run.ps1 -Script rdc_pass4.py -Scene S1` 重生成（9 块、errors=0），此后 S1 vs S3 `cs.bytecode-hash` PASS。**「JSON 与 txt 不同步」这类数据缺口就是 harness 抓出来的。**
- **pass1~pass6 现全部吃 `RDC_SCENE` + `RDC_HEADLESS`**（本轮把 `rdc_extract/pass2/pass3/pass4` 补齐到 pass5/pass6 同范式）→ 整链无头复跑可直接 `rdc_run.ps1 -Script rdc_extract.py -Scene Sx` 依次跑到 pass6。S2 全链实测 errors=0，重生成与归档**除 elapsed_sec 外逐字节一致**。旧口径「路径预切 S7、复跑须手改头部」**已失效**（不设 env 时仍回落 S7 占位，手跑无害）。
- **PS5.1 编码两连坑**：`.ps1` 必须 **UTF-8 带 BOM**（无 BOM 时 ANSI 解码会让中文吞掉字符串引号 → 解析失败；`rdc_compare.ps1` / `rdc_run.ps1` 均已补 BOM）；读 UTF-8 无 BOM 的 JSON/LOG 必须 `-Encoding UTF8`（默认 ANSI 会破坏 JSON 结构）。

## 12. PoC-B 注入帧的像素探针（rdc_pass7_pixels，2026-10-03 打通）

> **为什么必须另开一条**：§11 的十条锚点全是**结构**锚点（计数 / 格式集合 / 字节码哈希 / 分段 /
> Clear 方位…），一条都不看像素。PoC-B 要验的是「Vulkan 渲出的像素有没有真的落进抓帧的最终帧」，
> 结构锚点对此天然盲——只能直接读 backbuffer 像素做机器判定。

### 12.1 命令模板

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_pass7_pixels.py -Scene S4b
```

- `-Scene` 未知 ID 时按原样拼 `captures\<Scene>.rdc`（与 pass1 同范式，`_SCENES.get(scene, scene)`）
  → **PoC-B 候选帧就叫 `captures\S4b.rdc`**，pass1~pass6 与 `rdc_compare` 全部零改动可吃。
- 输出 `docs\analysis\S4b-pass7-pixels.json`；判定沿用项目口径：`errors=[]` 才算脚本执行成功，
  `verdict` 才是像素结论。

### 12.2 探针点（坐标与 `src\poc-presenter\main.cpp` 的 `POCB_X/Y/W/H` 常量对齐）

| 探针 | 坐标 | 期望 | 含义 |
|---|---|---|---|
| `rect-corner-1..4` | (22,22) (521,22) (22,521) (521,521) | 洋红 255,0,255 | 注入矩形四角 = `vkCmdClearColor` 的哨兵色 |
| `triangle-centroid` | (272,313) | 绿 ≈ 85,217,85 | 三角形覆盖到重心 → **几何**（不只清屏）也进帧 |
| `control-outside-1/2` | (1056,540) (1900,1060) | 非洋红 | 证明确实只注入了一个矩形，没污染整帧 |

坐标推导：三角形 NDC 顶点 (−0.66,0.60)/(0.66,0.60)/(0,−0.72)，Vulkan 的 y 轴向下 → 屏幕
(87,410)/(425,410)/(256,72)，重心 = (256,297) + 矩形偏移 (16,16) = **(272,313)**。

### 12.3 判定

| verdict | 含义 |
|---|---|
| `SENTINEL_FOUND` | 四角 ≥3 洋红 且 对照点干净 → **注入进了抓帧（PoC-B 的期望值）** |
| `SENTINEL_ABSENT` | 四角 0 洋红 → 未注入（**基线帧的期望值**） |
| `PARTIAL` | 角点部分命中 / 对照点也脏 → 看 `probes[].rgb` 明细再判 |
| `NO_BACKBUFFER` | 抓帧里找不到 `ResourceType.SwapchainImage` |

backbuffer 身份一律走 `ResourceType.SwapchainImage`（`ctrl.GetResources()`，RenderDoc 对交换链
backbuffer 的正式类型），找不到才退回「同尺寸 RGBA8 纹理」这种弱口径——json 的
`backbuffer.identified_by` 会写明是哪种，**弱口径的结论要打折看**。

### 12.4 自测（2026-10-03）

| 用例 | verdict | 细节 | errors |
|---|---|---|---|
| S4 基线（无注入，对照组） | `SENTINEL_ABSENT` | 0/4 洋红、2 个对照点干净、2.0s | **0** |
| `PoC-B.rdc`（首局候选帧，插件 0.9.0 初始化失败 → 注入未启用） | `SENTINEL_ABSENT` | 0/4 洋红、对照点干净、2.2s | **0** |
| `S5b.rdc`（0.9.1 注入局，S5 夜战） | **`SENTINEL_FOUND`** | 四角全 `(255,0,255)`、重心 `(86,217,85)`、对照点 `(47,63,58)`/`(13,31,29)`、2.2s | **0** |
| `S6-pool.rdc`（0.9.1 注入局，白漫水池） | **`SENTINEL_FOUND`** | 四角全 `(255,0,255)`、重心 `(86,217,85)`、对照点 `(65,87,85)`/`(45,69,69)`、2.0s | **0** |
| `S5c.rdc`（0.9.2 对照局，`vulkan=0`+`vtable=0`，S5 夜战） | **`SENTINEL_ABSENT`** | 0/4 洋红（四角 `(49,96,114)`/`(9,35,47)`/`(14,39,49)`/`(21,49,58)`）、重心 `(49,92,112)`、对照点 `(0,10,16)`/`(12,30,28)`、2.2s | **0** |

第 2 行是**候选文件上的首次实跑**：探针能正常打开该抓帧、按 `SwapchainImage` 找到
backbuffer，并如实报「没注入」——与当局日志一致（PoC-B 初始化在实例级函数表就失败了，注入
从未开启），所以**探针在候选文件上的阴性判定可信**。第 3、4 行是注入成功后的同一命令翻成
`SENTINEL_FOUND`，**这一翻转就是「写进去了」的机器判据**（前后共四次运行、errors 全 0）。

backbuffer 识别结果：基线 `ResourceId::35`、注入局 `ResourceId::78`（**id 是抓帧局部的，属正常**），
均为 `1920×1080 / R8G8B8A8_UNORM / row_pitch 7680`——与 poc-presenter 日志里的 `format=28`
（= `DXGI_FORMAT_R8G8B8A8_UNORM`）**互证**，说明探针读的正是 DLL 断言的那张交换链图。

**踩坑**：本版 `ResourceFormat` 既不能直接 `.name` 取到、也没有 `__str__`（`str()` 只给
`<Swig Object ... at 0x...>` 地址，跨进程不稳定）→ `_fmt_name` 改为逐属性尝试 + 组件数兜底，
现已返回 `R8G8B8A8_UNORM`。

### 12.5 整帧导出（`rdc_dump_backbuffer.py`，2026-10-03 新增）

7 点探针回答「那几个点对不对」，整帧导出回答「**这一帧长什么样、两帧可不可比**」：

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_dump_backbuffer.py -Scene S5b
```

| 项 | 口径 |
|---|---|
| 身份 | 与探针同一权威口径 `ResourceType.SwapchainImage`，找不到才退「1920×1080 RGBA8」弱口径（json 的 `identified_by` 标明） |
| 数据 | `ctrl.SetFrameEvent(末事件)` → `ctrl.GetTextureData(resId, rd.Subresource(0,0,0))`，行距 = `len/height`（紧凑），`row_pitch < w*4` 时**拒绝导出**（避免花屏假图） |
| 编码 | **无 PIL**，`zlib + struct` 手写 8-bit RGB PNG（`\x89PNG…` + IHDR/IDAT/IEND + CRC32） |
| 输出 | `docs\analysis\<场景前缀>-backbuffer.png` + 同名 `.json`（含 `spotcheck` 三个抽检点，可与 pass7 互证） |
| 耗时 | **约 7 s/张**（1920×1080，Python 逐像素转 RGB） |

**已在库的七张**：`S5-backbuffer.png`（10/2 基线：火焰战斗中）、`S5b-backbuffer.png`（注入局：
同机位对话状态 + **左上角洋红块与渐变三角形肉眼可见**）、`S6-backbuffer.png`（白漫水池注入局）、
`S5c-backbuffer.png`（0.9.2 对照局：同机位、**无**洋红块）、`S4b-backbuffer.png` /
`S4c-backbuffer.png` / `S4-backbuffer.png`（10-03 水体标定三联，见 §13——S4b 有洋红块、
S4c 干净、S4 基线为同机位**不同存档状态**，三图互证机位与注入态）。

用法要点：把基线与候选两图**并排打开**，能一眼判断两帧是否处于同一游戏状态——这直接决定
`rdc_compare` 的差异该归因注入还是归因场景（见 §12.6）。

> ⚠️ **图证要落到字节**：2026-10-03 曾出现「看图工具把 S5b 的图当成 S5c 显示」的串档，
> 与 pass7 探针、dump 自己的 `spotcheck` 当场矛盾。判图别只凭肉眼——用图里 `spotcheck`
> 抽检点、或直接解 PNG 字节核对（`.json` 的 `spotcheck` 与 pass7 探针点同坐标可互证），
> 四路（探针 json / dump json / PNG 字节 / pass5 Copy 数）一致才算数。

### 12.6 结构配对的判读口径（`rdc_compare` 的 DIFF 怎么归因）

`rdc_compare -Base <基线> -Cand <候选>` 十条锚点里，DIFF 不等于「注入造成的」。判读顺序：

1. **先找注入指纹（预期且必须出现，三口径互证）**：
   `counts.pass1.copies` **+1**、`copy.sequence` 尾部多一条
   `512x512 R8G8B8A8_UNORM -> 1920x1080 R8G8B8A8_UNORM`、`conditional-nodes.copies_total` **+1**；
   同时 pass5 日志里应有 `Copy ev…: ResourceId::N(512x512) -> ResourceId::M(1920x1080)`，
   其目标 **M 必须等于探针读的 SwapchainImage id**。
2. **注入不可能产生的差异 → 归因场景**：draws / clears / pso / 分段数 / 透明段 Draw。注入侧
   **一个 D3D11 Draw/Dispatch/Clear 都不发**（只有 `UpdateSubresource` + `CopySubresourceRegion`
   + 调原 Present），故这些数的变动来自游戏状态本身（人物、粒子、字幕、光照档）。
3. **归因前必须看图**：跑 §12.5 把两帧导出并排看，确认「同机位不同时刻」还是「同一帧」。
   2026-10-03 实测（`S5 ~ S5b`）：pass3 / diff7——纹理格式 16=16、CS dispatch 98 次
   18786750 线程全等、CS 字节码 9 块全等 **PASS**；`copies 3→4` 指纹 ✓；draws 3809→4218
   (+409) 与分段 42→41、PSO 157→151 经图证确认为**同机位不同时刻**（基线=火焰战斗、候选=对话）。
4. **「唯一差别=注入」的严格数字（2026-10-03 已实测，遗留清零）**：同日同机位的无注入对照帧
   `S5c.rdc`（`poc-presenter.ini` 写 `vulkan=0`+`vtable=0`，0.9.2 局）→
   `rdc_compare -Base S5c -Cand S5b` = **pass6 / diff4**——`clear.bound-profile`、
   `clear.target-profile`、`transparent.draws (161=161)` 三项**由跨日配对的 DIFF 转 PASS**，
   余下 4 条 DIFF 里 `counts.pass1`（copies **3→4**）、`copy.sequence`、
   `conditional-nodes`（copies_total **3→4**）正是注入指纹，`pass5.structure` 的
   draws_sum 差只是 draws 4273→4218（−55）的连带。
   **噪声底参考**：同为无注入的 `S5 → S5c` 是 pass5 / diff5，draws +464、clear 两档 DIFF、
   透明段 122→161——即跨日那批"场景动态"DIFF **在对照对里原样出现**，copy 两锚点则是 **PASS**。
   （详见 docs/01 §7.9；注意本局 `vulkan=0` 分支其实没被执行到，见 §7.9.5 的缺口。）

---

## 13. 水体场景注入归因标定（S4b/S4c 双帧，2026-10-03）

> 阶段1 配对门的**预演**：在水体场景把「注入指纹」与「会话/帧态漂移」分开钉死，给真 VK
> 候选帧的 `rdc_compare` 判读提供分级口径。协议沿用 S5b/S5c（§12.6 第 4 条）。

### 13.1 抓帧协议与执行记录

| 帧 | 配置 | 时刻 | 大小 | pass7 哨兵 |
|---|---|---|---|---|
| `captures\S4b.rdc` | 默认态（`poc-presenter.ini` 不存在 = 注入开） | 10-03 19:02:49 | 704.1 MB | **SENTINEL_FOUND**：四角 4/4 `(255,0,255)`、三角重心 `(86,217,85)`、对照点 `(15,22,24)`/`(16,36,39)` 干净 |
| `captures\S4c.rdc` | ini 写 `vulkan=0` 后**重启游戏** | 10-03 19:05:47 | 802.4 MB | **SENTINEL_ABSENT**：0/4 洋红、重心 `(57,77,77)`、对照点 `(17,26,26)`/`(16,40,48)` 干净 |

- 同存档同机位（S4 河谷水面：角色居中站水中、罗盘同向），三张 backbuffer PNG 互证：
  `S4b-backbuffer.png`（左上洋红块+渐变三角肉眼可见）/ `S4c-backbuffer.png`（干净）/
  `S4-backbuffer.png`（10-02 基线：**同机位但不同存档状态——角色皮草装**，跨日+跨存档）。
- 开关语义两条铁律：`pocbEnabled()` 是 `static cached` → **进程内只读一次，改 ini 必须重启游戏**；
  写入必须 **ASCII 无 BOM**（``[IO.File]::WriteAllText($p,"vulkan=0`r`n",[Text.Encoding]::ASCII)``——
  BOM/UTF-16 会给首行键加前导字节 → `key != "vulkan"` → 开关静默失效、对照帧被污染）。
  抓完即删 ini（实验态回收 ✓，已核 `Test-Path` = False）。
- 执行：提取链 12/12 步（extract→pass6 ×2）全 `errors=0`；pass7 ×2、backbuffer dump ×3 全 `errors=0`。
- **四路互证（§12.5 口径）全齐**：pass7 探针 json = dump json `spotcheck` 同坐标逐值相等
  （S4b 角1 `(255,0,255)` / 三角 `(86,217,85)` / 对照 `(15,22,24)`）= PNG 目视 = pass5 `copies`=4。

### 13.2 两道闸门结果

| 比对 | 含义 | 结果 |
|---|---|---|
| `-Base S4c -Cand S4b` | 同日相邻时刻、唯一配置差=注入开关 → **指纹标定** | **pass5 / diff5**（skip0） |
| `-Base S4 -Cand S4c` | 跨日+跨存档、两帧均无注入 → **噪声底** | **pass7 / diff3**（skip0） |

**指纹三口径全中 + 尾验（§12.6 规则 1）**：

1. `counts.pass1.copies` **3→4**；
2. `copy.sequence` 候选侧独有 `512x512 R8G8B8A8_UNORM -> 1920x1080 R8G8B8A8_UNORM`
   （基线侧 0 处、`S4~S4c` 对照 0 处——全文 grep 核对）；
3. `conditional-nodes.copies_total` **3→4**（`S4~S4c` 为 3=3）；
4. 尾验：pass5 日志 `Copy ev33412: ResourceId::17759(512x512) -> ResourceId::78(1920x1080)`，
   目标 **78 = pass7/dump 读的 SwapchainImage id `ResourceId::78`**（S4c 侧 backbuffer 为 35——
   两次启动 id 不同，证 id-free 锚点口径必要，`docs/03` §5 R2）。

**diff5 归因（§12.6 三步走完 + 看图）**：

- **指纹类（预期且必须出现）**：`counts.pass1`（copies 分量）、`copy.sequence`、`conditional-nodes` —— 3 条；
- **会话态漂移（注入 0 Draw/Dispatch/Clear，不可能产生）**：`counts.pass1` 的 draws/pso/textures
  分量（3862→2846 / 138→126 / **423→316**）、`pass5.structure`（36 段=36 段、draws_sum 同幅）、
  `transparent.draws`（276→232）—— 2 条整锚点 + 1 条混合锚点；
- **段级定位（−1016 draws 精确闭合）**：SEG0（512² 探针段）25→70 **+45**、SEG7（无 RT，深度/阴影类）
  680→1173 **+493**、SEG9（主 MRT RGBA16F|R16G16|RGBA8）713→1227 **+514**、SEG20（透明 MRT）
  232→276 **+44**、SEG5 698→632 **−66**、SEG2/3/4/10/18 合计 **−14**，**其余 26 段 draws 全等**
  （逐段和 = +1016 与 pass1 差值闭合）；
- **看图结论**：两帧同机位、相邻时刻（两次启动间隔约 3 分钟），构图一致（右崖受光差异=云影/时刻）→
  漂移归因**会话间场景状态**（候选机制：加载后流送/生成物/可见集未稳定即抓帧——S4b `textures` 316
  显著低于 S4/S4c 的 418/423，指向资产/对象未全量驻留；**机制未定死 = 诚实边界**，
  操作口径：以后同机位待画面稳定再 F12、连抓两张核对）。

**噪声底（S4→S4c，pass7/diff3）**：draws 3667→3862（+195）、pso 131→138、textures 418→423、
透明 242→276（+34）；**其余 7 条锚点全绿**（formats 14=14、CS 98 次 18,786,750 线程三帧全等、
CS 字节码 9=9、clear 两档、copy.sequence、conditional-nodes 逐值相等）——比 S5 噪声底
（`S5→S5c` pass5/diff5、draws +464、clear 两档 DIFF）更稳。

### 13.3 锚点稳定性分级（阶段1 判读权威口径）

| 级 | 锚点 | 三帧实测（S4 / S4b / S4c） | 阶段1 用法 |
|---|---|---|---|
| **A · 逐帧确定** | `textures.formats`、`cs.dispatch-profile`、`cs.bytecode-hash`、`clear.bound-profile`、`clear.target-profile`、`copy.sequence` 基础三拷贝、`conditional-nodes` 结构（除 copies_total） | 全绿：14=14=14；98 次 / 18,786,750 线程全等；9=9；clear 两档全等 | **硬门槛**：候选帧红一条 = 移植错误 |
| **F · 注入指纹** | copies +1、copy.sequence 尾条 512²→1920×1080、copies_total +1（目标=SwapchainImage id） | S4c→S4b 三条全中；S4→S4c 零出现 | 注入帧**必须出现**；共享纹理通路若改变拷贝结构，须在候选帧提案里**先声明预期差异**再跑闸门 |
| **B · 会话/帧态敏感** | `counts.pass1` 的 draws/pso/textures、`pass5.structure` 的 draws_sum 与段签名（`_seg_signature` = viewports/rt_infos/dsv_info，与 draws 无关——2048² R16_TYPELESS 段两两必 DIFF 即此字段漂移）、`transparent.draws` | 同日两抓：draws **−1016**、pso −12、textures −107、透明 −44；跨日：draws +195、pso +7、textures +5、透明 +34 | **不做单对硬门槛**：DIFF 先按 §12.6 看图归因；量级超出本次实测（约 ±1000 draws / ±50 透明 / ±100 textures）才升级为可疑 |

> **「10 锚点全绿」的准确含义**（`docs/03` §3、`docs/00` §1.1 同步按此执行）：
> **A 类全绿 + F 类必现 + B 类归因后无注入不可解释项**。字面 10/10 只在同帧自比时成立
> （`S4 vs S4` 10/10，2026-10-03 复跑仍绿）；跨抓帧对必然携带 B 类漂移——这正是 §12.6
> 判读顺序存在的原因。

### 13.4 复现命令

```powershell
# 全链（两帧各 6 步：extract → pass2..pass6，-Scene 换 S4c 同跑）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_extract.py -Scene S4b
# 哨兵 / 闸门 / 图证
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_pass7_pixels.py -Scene S4b
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_compare.ps1 -Base S4c -Cand S4b
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_compare.ps1 -Base S4 -Cand S4c
powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_dump_backbuffer.py -Scene S4b
```

在档产出：`S4{b,c}-extract*.json`、`S4{b,c}-pass7-pixels.json`、`S4{,b,c}-backbuffer.{json,png}`、
`S4c~S4b-compare.json`（指纹标定）、`S4~S4c-compare.json`（噪声底）。

---

## 14. 阶段1 候选帧判读 —— S4d/S4d2（v0.10.0 共享通路 + 探针升质，2026-10-03）

> 阶段1 的**真·配对门**：同存档同机位连抓两张候选帧，对照 §13 三帧标定按 §13.3 分级收口。
> 本节新增三工具：`rdc_api_scan.py`（帧内结构化 API 调用扫描——state 类调用非 action、pass5 永远看不见）、
> `rdc_state_probe.py`（最小 pipe state 对拍）、`rdc_dump_cubefaces.py`（探针面 PNG + md5/统计）、
> `rdc_chunk_scan.py`（全文件 chunk 普查——视图创建在不在 rdc 里）。

### 14.1 抓帧协议与执行记录

| 帧 | 配置 | 时刻 | 大小 | 备注 |
|---|---|---|---|---|
| `captures\S4d.rdc` | v0.10.0 默认态（probe+shared 全开、无 ini） | 10-03 20:59:38 | 792.6 MB（831,106,833 B） | 稳定后 F12 首张 |
| `captures\S4d2.rdc` | 同上 | 10-03 20:59:55 | 792.0 MB（830,438,313 B） | 隔 17 s 连抓第二张（稳定性核对） |

- **同一游戏进程内连抓**（run2：20:58:20 启动 → 20:59:38/55 抓帧，抓帧间隙注入持续稳态）。
- 运行日志（run2）关键行全绿：`槽5 已挂 … orig5 … 来自 …\renderdoc.dll` → 升质改写的 desc
  **经 RenderDoc 序列化**；`512² RGBA16F cube(6面) → 1024² 第 1 次, hr=0`（两 run 各恰 1 次、无第 2 次）；
  `共享图导入 OK`、`init 完成: 离屏 512x512 (洋红清屏+三角形) → 共享纹理 (NT handle+event闸, 无读回)`；
  稳态 0.54–0.66 ms/帧（run1 0.40–0.83）；**无降级记录**（grep 0 处）。

### 14.2 提取链（12/12 全绿）

- S4d / S4d2 各 6 步（extract→pass6）全 `errors=0`；pass6 3.6 / 3.7 MB。
- pass5 尾部注入指纹：`Copy ev46151 / ev47375: 17760(512x512 RGBA8) -> 78(1920x1080)` —— 目标
  **78 = SwapchainImage**（§13.2 尾验同口径；源 17760 vs S4b 17759 = 跨启动 id 漂移，id-free 必要性再证）。

### 14.3 三道闸门（§13.3 分级判读）

| 比对 | 含义 | 结果 |
|---|---|---|
| `-Base S4c -Cand S4d` | 关注入基线 → 候选（唯一配置差 = v0.10.0 两特性） | **pass3 / diff7**（skip0） |
| `-Base S4b -Cand S4d` | 注入帧 → 注入+升质候选 | **pass4 / diff6**（skip0） |
| `-Base S4d2 -Cand S4d` | 同配置连抓两张 → 稳定性核对 | **pass9 / diff1**（skip0） |

**闸门1（S4c→S4d）逐条**：

- **A 类硬门槛 3 绿**：`textures.formats` 14=14；`cs.dispatch-profile` 98 次 / 18,786,750 线程、9 组
  groups 逐值相等；`cs.bytecode-hash` 9=9。
- **F 类必现 3/3**：`counts.copies` 3→4、`copy.sequence` 尾条 `512²→1920×1080`（基础 3 条逐值同）、
  `conditional-nodes.copies_total` 3→4。
- **声明项 ✓**（探针升质提案先声明再跑闸）：`clear.target-profile` base-only
  `ClearColor 512² R16G16B16A16_FLOAT ×2` ↔ cand-only `ClearColor 1024² R16G16B16A16_FLOAT ×2`。
- **B 类带内**：draws 3862→3822（−40）、pso 138→137、textures 423→438（+15）、
  `transparent.draws` 276→267（−9）。
- **遗留疑点 2 条（同源 → §14.4）**：`clear.bound-profile`（probe 清屏绑定态 `512² D24S8`→`-`）、
  `pass5.structure`（probe 段签名 rt/dsv `512²+512² D24S8`→全空；`cube512_seg` 2→0 同源）。
  结构锚里另有 base/cand 各一条 = 2048² R16_TYPELESS 段（§13.3 已知 B 类噪声，两两必 DIFF）。

**闸门2（S4b→S4d）**：PASS 4 = formats / CS×2 / `copy.sequence`（4=4——两注入帧同指纹，F 在同注入对里
消隐为相等）；diff6 = counts（draws **2846→3822**、pso 126→137、textures **316→438**）、pass5、bound、
target（声明）、conditional（仅 `cube512_seg`）、transparent（232→267，+35）。判读注意：S4b 是 §13.2
判过的**会话不稳定帧**（textures 316 显著低于 S4/S4c/S4d 的 418/423/438），差异归 S4b 侧漂移；
以 S4c 为基线的 textures 差 +15 才是带内口径。

**稳定性闸（S4d2→S4d，pass9/diff1）**：唯一 DIFF = `counts.pass1`（draws 3943→3822 = −121、
textures 428→438；pso 137=137、clears/copies 全等），B 类 ±1000 带内；**其余 9 锚全绿——含 pipe0 相关的
`clear.bound-profile`/`pass5.structure` 两侧逐值一致、`clear.target-profile` 两侧同为 1024²×2、
`copy.sequence` 两侧同指纹 → 异常系统性，非单帧抖动**。

### 14.4 探针段 pipe state 全 0（已归因：升质未同步 depth → OM 非法）

事实链（全部已入档）：

1. **capture 侧调用真实有效**（`S4d-api-scan.txt`，range 0-2700 共 21 条）：ev14、ev35 绑
   `{view550}+DSV553`，ev399、ev426 绑 `{view548}+DSV553` —— 序列化参数是**非零视图 id**（若视图创建
   失败，游戏只会持 NULL、绑出 `res:0`）→ 视图对象 capture 侧存在；ev12/ev397
   `ClearRenderTargetView` 真清 **544（1024² RGBA16F，GetUsage 权威）**、ev13/ev398 清 DSV553→552
   （512² D24S8——描述符只匹配 RGBA16F cube，depth 未升质 = 预期）。
2. **回放态解析为 0**（`S4d-state-probe.txt`）：ev118/400/440/900 `RT=[8×res=0] DS=res=0`
   （seek=True/False 同值）；同事件段 S4b = `RT=544 slice=5/0, DS=552` 正常；两侧 viewport 均 512²。
3. **无解绑可能**：ev35→ev118 之间 api-scan 无任何 `OMSetRenderTargets*`/`ClearState`（过滤含
   `OMSetRenderTargetsAndUnorderedAccessViews` 全变体）→ RT 槽不可能被后续调用清掉；且 ev400 的
   DS=0 发生在 ev399 **重新绑定 DSV553 之后** → 是绑定动作本身没解析出来，不是解绑。
4. S4d2 同现象（§14.3 稳定性闸逐值一致）= 系统性。
5. 视图均帧外创建（帧内 0 条 Create*View；`main.cpp` 注入侧无 ClearState/OMSetRenderTargets，grep 实证）。
6. **归因已落定（2026-10-03 22:46–22:51，`GetDebugMessages` 回放诊断 + 双场景面导出对照）**：

   > **根因：升质 hook 只升了 cube、没升配对 depth → `RT 1024² + DSV 512²` 尺寸不匹配 →
   > OM 绑定判非法（Invalid output merger）→ 绑定作废（pipe state=0）→ probe draw 零写入。**

   - **回放诊断消息**（`GetDebugMessages()` 无参，8 条，见 `S4d-cubefaces.json` api_probe）：
     `MessageSeverity.High / State_Setting / IncorrectAPIUse` = `"Invalid output merger -
     Depth target is different size or MS count to render target(s)"`，命中事件恰为
     **ev14/ev35/ev118/ev399/ev426/ev440** = probe 段全部绑定点 + 首 draw，与 state-probe 全 0
     的事件集逐一对应；**S4b 同口径 0 条**（512²=512² 匹配）。
   - **面内容对照**（`S4{b,d}-cubefaces.json` + 12 张面 PNG，`rdc_dump_cubefaces.py`）：

     | | S4b（基线 512²） | S4d（升质 1024²） |
     |---|---|---|
     | 面尺寸 | 512×512 | **1024×1024**（分辨率图证） |
     | 6 面 md5 | 互不相同（slice 参数有效 ✓） | 全同 `2c14cb93dc77…` |
     | uniq_rgb8 | 80–118（实渲内容） | **3**（纯清屏色，半浮点 min==max 精确统一） |
     | head16 | 每面各异 | 全面 `(0.196,0.441,0.598,0)` = 清屏色 |
     | PNG 大小 | 158–366 KB | 5.3 KB（近纯色） |

   - **机制链自洽**：清屏走 `ClearRenderTargetView` 独立通路（不经 OM）→ 6 面 = 清屏色存活；
     draw 走 OM → 绑定作废 → 写入全灭。「清屏在、内容无」与 pipe=0、pass5 签名全空三处互证。
   - **捕获侧实证（初态内容链，闭合诚实边界⑤）**：S4b 本帧 RTV 是 `firstSlice=5/0` 的**单面视图**
     （state-probe 实测），faces1–4 在回放内**不被本帧任何调用触碰**，却呈连贯实渲内容
     （uniq8 80–118、逐面互异、head16 各不同）→ 实证回放保留**帧初态内容**（RenderDoc 初态
     序列化；否则未触碰面只能是驱动初始化值，不可能是连贯场景内容）。S4d 同口径：faces1–4
     回放内同样无触碰（帧内清屏仅 ev12→view550/face5、ev397→view548/face0，copies 不涉及 544），
     内容却是**精确清屏色**（全 6 面 md5 全同）→ 只能来自帧初态 = 捕获前该面最后一轮
     「清屏+draw」只有清屏落地 → **真机 draw 同样未执行，反射 cube 自启动起即为平铺清屏色 =
     游戏内视觉回归实锤**（非仅回放读数异常）。
   - **backbuffer 目视互证**：`S4b/S4d-backbuffer.png` 同机位——水面整体相近，S4d 略偏均匀青蓝、
     方向与「反射=平铺清屏色」一致，但被浅水底质+法线扰动稀释，**不能单独定案**（与用户
     「1080p 差异细微」观感一致）。
   - **归因判定 = 假设 B（升质缺陷）为主**；取证口径部分成立：非法绑定是**游戏在捕获时真实发出的
     调用序列**（api-scan 非零视图 id 已证视图创建成功）——升质后 D3D11 语义已非法，回放只是
     把它显形。
   - **诚实边界（第 5 条，已被初态内容链大幅收窄）**：真机 release 运行时对该非法 OM 绑定是
     「绑定时拒绝」还是「draw 时跳过」的机制细节未知；但「真机 draw 未落地」已由初态链实证
     （faces1–4 初态 = 清屏色）→ 边界收窄为机制细节，不再影响视觉回归结论。残余验证：v0.11
     修复后重抓，面内容应恢复逐面互异。

### 14.5 probe 段 viewport 未随升质适配（capture 侧事实，独立于 pipe0）

- `S4d-api-scan.txt`：probe 段全部 `RSSetViewports … 512.0×512.0` 字面常量
  （ev17/38/313/402/429/2504/2522；ev2648/2667 的 256² 是阴影段），与 S4b/S4c 同值；pass5 逐 draw
  vp=512² → 游戏用**自身缓存值**，`main.cpp` 注释「viewport/RTV 从 GetDesc 自动适配」**实测不成立**。
- capture 侧后果：512² viewport（原点 0,0）渲进 1024² face = **每面仅左上 1/4 有探针内容**、其余 =
  清屏色；SRV 按全 1024² 归一化采样 → UV 错配（0.5 内双倍缩放、0.5 外清屏色）。
- 与 §14.4 的关系：**缺陷②**（本条）叠加在**缺陷①**（OM 非法）之上——即使 depth 同步升质修复了
  绑定，viewport 不适配仍只渲 1/4 面；两条都修，升质才算成立。
- 图证：`S4d-cube-*-face*.png`（1024²）对照 `S4b-cube-*-face*.png`（512²）——分辨率差与内容差
  双图证已入档。

### 14.6 过闸判定（2026-10-03 定稿）

**证据闭合链**：提取 12/12 → 三闸判读 → api-scan/state-probe 定位 → 双场景面导出 +
`GetDebugMessages` 归因 → pass7/backbuffer 四路互证（`S4d/S4d2-pass7` 均 **SENTINEL_FOUND**：
洋红 4/4、三角 `(86,217,85)`、对照点干净、swapchain `ResourceId::78`）。

按 §13.3 分级收口：

- **A 类硬门槛 3/3 绿**：formats 14=14、CS dispatch 全等、CS 字节码 9=9；
- **F 类必现 3/3**：copies 3→4、tail `512²→1920×1080`、copies_total 3→4；
- **声明项 ✓**：`clear.target-profile` probe `512²→1024² RGBA16F ×2`；
- **B 类带内**：三闸的 draws/pso/textures/transparent 全部在带内；
- **遗留项全部归因、无不可解释项**：`pass5.structure` / `clear.bound-profile` /
  `cube512_seg 2→0` 三处 DIFF = 同一根因（缺陷①）；2048² R16 段 = §13.3 已知 B 类噪声。

**特性分级结论**：

| 特性 | 判定 | 依据 |
|---|---|---|
| ① 共享纹理通路（NT handle+fence） | **过闸** | A 全绿 + F 3/3 + 双帧哨兵 SENTINEL_FOUND + 稳态 0.40–0.83 ms/帧 + 无降级 |
| ② 探针升质 512²→1024² | **过闸（2026-10-04，v0.12.0 S4f/S4f2 三判据全中）** | 缺陷①（配对 depth 未升质 → OM 非法）v0.11 已修；缺陷②（viewport 恒 512²）plan-B 拦 RSSetViewports 后实测：① 双帧 0 诊断 + 1024² 配对；② 双帧 8×1024²、0×512²；③ 六面 md5 互异、四象限全实内容（对照 S4e 仅 TL 1/4）→ **§14.8** |

- **阶段1 判定：全绿收口（2026-10-04）**——共享通路（阶段1 核心交付）+ 探针升质特性②均
  实证成立（§14.8），`probe=0` 逃生门保留为兜底。
- **v0.11 修复清单（2026-10-03 已回码 `v0.10.0→v0.11.0`；2026-10-04 S4e 重抓验证
  = 判据①③达成、②未达成 → 见 §14.7，plan-B 触发；**v0.12.0 plan-B 回码 → S4f/S4f2
  三判据全中过闸，见 §14.8**）**：
  1. ✅ hook 同步升质配对 depth：`probeDepthDesc`（512² D24 家族 4 个合法 DXGI 枚举
     = R24G8_TYPELESS/D24_UNORM_S8_UINT/R24_UNORM_X8_TYPELESS/X24_TYPELESS_G8_UINT，
     RenderDoc 显示名 "D24S8_TYPELESS"；+ mips=1 + array=1 + DSV bind）+ **cube 命中后
     10s 一次性开窗**（依据 `rdc_tex_desc` 实测：
     552 全帧唯一 512² D24；ResourceId 时序 544cube<552depth 同突发 → 前向窗即可配对；
     收窗后其余 512² depth 一概不碰——无条件升会把别的 512² pass 也弄成尺寸不匹配）；
  2. ✅ viewport 适配：**原地回写游戏那份 desc**（`pokeDescSize`，写前 `VirtualQuery`
     查页保护、只读页放弃+告警不崩）——实测坐实游戏用创建时缓存值（不重查 GetDesc），
     改它手里那份即改 viewport 派生值；若 S4e 抓帧 viewport 仍 512² → 降级 plan-B
     （拦 RSSetViewports 或重新评估升质方案）；
  3. ✅ 修完重抓 S4e/S4e2 双帧（§14.1 同协议：同存档同机位、稳定后 F12 连抓两张）→
     同三闸 + 面导出重过——**2026-10-04 实测见 §14.7：判据①③达成、②未达成 →
     特性② 仍未过闸，plan-B（拦 RSSetViewports）触发 → v0.12.0 回码后 S4f/S4f2
     三判据全中过闸，见 §14.8**。

     **启动日志 sanity（进游戏 1 分钟内看 `poc-presenter.log`）**：

     ```
     ==== poc-presenter v0.11.0 ...
     探针升质: 槽5 已挂 (512² cube+配对depth→1024² + 回写desc, 逃生门 probe=0 / vulkan=0)
     探针升质: 512² RGBA16F cube(6面) → 1024² 第 1 次, hr=0, 回写desc=ok
     探针配对depth: 512² D24 → 1024² 第 1 次, hr=0, 回写desc=ok
     ```

     异常分支：无「探针配对depth」行 = 配对失败（±10s 窗没等到，查「开窗 10s 已过」告警）；
     `回写desc=no` +「游戏描述符页只读」= 缺陷② plan-B（viewport 仍 512² → 另拦
     RSSetViewports）；「带初始数据」告警 = 该纹理建时带 init，升质被主动跳过。

     **通过判据**（三项全中才算特性②过闸）：

     | 判据 | 口径 | 工具 |
     |---|---|---|
     | ① OM 绑定修复 | probe 段 `GetDebugMessages` **0 条**（S4d 是 8 条）+ state-probe ev118/400 `RT=544(1024²) DS=552(1024²)` 非零 | `rdc_dump_cubefaces` / `rdc_state_probe` |
     | ② viewport 适配 | probe 段 `RSSetViewports` = **1024²**（S4d/S4b 均 512² 字面量） | `rdc_api_scan` |
     | ③ 面内容恢复 | 六面 **md5 互异、uniq_rgb8 拉高到两位数**（对照 S4d 全同=3、S4b=80–118）、PNG 从 5.3KB 涨到百 KB 级 | `rdc_dump_cubefaces` |

     **S4d→S4e 预期 DIFF = 修复生效项（declared，非回归）**：`clear.depth` 552 档
     512²→1024²；pass5/pass6 bound-profile 由全 0 恢复为 `RT=544, DS=552`（对齐 S4b 形态）；
     `cube512_seg` conditional 2→0 应回到基线 2（**2026-10-04 修正：该指标在
     `rdc_compare.py` 硬编码 `512²+512²` 签名，升质生效后按设计恒 0——「回 2」不可达且
     无意义，恢复证据改看 `pass5.structure` 的 1024² 配对段，见 §14.7**）；
     viewport 512²→1024²（api-scan 口径）。
     稳定性闸 S4e2↔S4e 预期同 S4d2↔S4d（唯一 DIFF = 计数带内漂移）。
     F 类锚点（copies 3→4、tail `512²→1920×1080`、copies_total）与 A 类锚点**不应变化**——
     共享通路与升质正交，动了就是新回归。
- **下一步不被阻塞**：SSR 第二步（或 2048² 探针对照 demo）可先行决策（docs/03 §6.1）。

在档产出：`S4d*-extract*.json`（全套）、`S4c~S4d-compare.json`、`S4b~S4d-compare.json`、
`S4d2~S4d-compare.json`、`S4d-api-scan.txt`、`S4{b,d}-state-probe.txt`、`S4{b,d}-cubefaces.{json,log}`、
`S4d-tex-desc.txt`（depth 配对取证：552 = 全帧唯一 512² D24）、
`S4d-cube-*-face*.png`（6 张 1024²）、`S4b-cube-*-face*.png`（6 张 512² 对照）、
`S4{d,d2}-pass7-pixels.json`、`S4{d,d2}-backbuffer.{json,png}`。

### 14.7 S4e 验证记录（2026-10-04，v0.11 三判据实测）

**证据链**：日志 sanity 四行齐（v0.11.0 banner / 槽5 已挂 / `cube→1024² 回写desc=ok` /
`探针配对depth→1024² 回写desc=ok`，与抓帧取证互洽）→ S4e/S4e2 双帧 api-scan → cubefaces
双帧（面内容 + `GetDebugMessages`）→ state-probe（ev14/41/882/909 绑定实底）→ tex_desc
（升质尺寸硬取证）→ 提取 12/12 → 三组 compare（S4d→S4e / S4e2→S4e / S4b→S4e）→
pass7/backbuffer 四路互证（双帧均 `SENTINEL_FOUND`、洋红 4/4、swapchain `ResourceId::78`）。

**三判据结果（§14.6 表逐项）**：

| 判据 | 结果 | 证据 |
|---|---|---|
| ① OM 绑定修复 | ✅ **PASS** | `GetDebugMessages` **0 条**（S4d=8 条，双帧同）；state-probe ev14/41 `RT=ResourceId::544 slice=4`、ev882/909 `slice=3`、`DS=ResourceId::552` 全非零；`S4e-tex-desc`：544=1024² RGBA16F cube、**552=1024² D24S8**（S4d 时 512²）→ `RT/DSV` 尺寸配对成立 |
| ② viewport 适配 | ❌ **FAIL** | 双帧全帧 `RSSetViewports` **无任何 1024² 字面量**；probe 段（ev17/44/742/760/885/912/2979/2997）恒 512²——与 S4b/S4d 同值 → `pokeDescSize` 改不到游戏 viewport 来源（创建时缓存于 desc 之外 / 硬编码常量）→ **§14.6 预declared 降级分支触发** |
| ③ 面内容恢复 | ⚠️ **字面 PASS、覆盖仅 1/4** | 六面 1024² md5 互异（S4e：`cb67f2…/6a333b…/103fd5…/b78229…/10395f…/db63bc…`）、uniq8=80–120（S4d=3、S4b=80–118）、PNG 177–394KB（S4d=5.3KB 级）——三项字面判据全中；**但象限取样：内容仅 TL（x<512 且 y<512，face0 TL distinct=769，边界 x511 实内容 / x512 实心），其余三象限单色 = 清屏色**（PNG 呈 `(42,78,95,255)`，与 S4d 纯清屏色帧 PNG 同值；导出链 x/(1+x) 实测：0.196/1.196→42、0.441/1.441→78、0.598/1.598→95 精确吻合）= 判据②失败的直接后果 |

**判定（S4e 时点）：特性②仍未过闸 → 后经 v0.12.0 plan-B 于 S4f 三判据全中过闸，见
§14.8**——缺陷①（升质只翻 cube 不翻 depth → OM 非法 → probe 零写入）
**已修复实锤**（0 诊断 + 1024² 配对 + TL 象限实渲内容 + 日志配对行）；缺陷②（viewport
恒 512² → 只渲 1/4 面）**未修**，按 §14.6 预declared 分支走 **plan-B：拦 `RSSetViewports`
（+ `RSSetScissorRects`）**。实现口径：不搞「无条件 512² 全拦」（会误伤其他 512² pass），
改用**绑定感知**——hook 内 `OMGetRenderTargets` → `GetResource` 解析当前 RT 是否 ==
建帧时记住的 probe cube 指针，是且视口 512² 才改写 1024²。

**三组 compare 判读（§13.3 分级口径）**：

- **S4e2↔S4e 稳定闸**：9/10 PASS，唯一 DIFF = `counts.pass1`（draws 2837↔2838、textures
  微漂）= 计数带内漂移，与 §14.6 预期一致 ✓；
- **S4d→S4e declared 差分逐项核对**：
  - `clear.target-profile`：`ClearDepthStencil 512²×2 → 1024²×2` ✓（= 预declared
    `clear.depth` 552 档升级）；
  - `clear.bound-profile`：S4d 的 probe clear 绑定全 0（`ClearColor@-`）→ S4e 恢复
    `ClearColor@1024² D24×1 + ClearDepthStencil@1024² D24×2`；S4b→S4e 对照呈**逐项同构、
    纯尺寸差分（512²↔1024²）** =「bound-profile 恢复对齐 S4b 形态」✓；
  - `pass5.structure`：cand-only 段 `1024² RGBA16F || … || 1024² D24S8` = probe 段恢复
    绑定 ✓；2048² R16 段差分 = §13.3 已知 B 类噪声；
  - **预期条目修正（已回改 §14.6）**：`cube512_seg 回基线 2` 不可达且无意义——该指标在
    `rdc_compare.py` 硬编码 `512x512 RGBA16F + 512x512 D24S8` 签名，**升质生效后按设计
    恒 0**（S4d→S4e conditional 0=0 PASS 无信息量；S4b→S4e `2 vs 0` 的 DIFF = 升质尺寸
    升级的 declared 差分）。恢复证据以 `pass5.structure` 1024² 配对段为准；
  - **未 declared 差分归因（⚠️ 2026-10-04 S4f 实测后作废，见 §14.8）**：`counts.pass1`
    （draws 3822→2838、textures 438→335、
    pso 137→126）、`transparent.draws` 267→235——当时按 probe 功能态**聚类**：S4b(2846/316/126、
    trans 232) ≈ S4e(2838/335/126、trans 235) 为健康聚类，S4c(3862/423/138) / S4d(3822/438/
    137、trans 267) 为缺陷聚类 → 当时归因「缺陷态多出 ~980 draws 随修复消失」——**S4f（三
    判据全中的健康态）draws=3964 反落缺陷聚类区间 → 该假说证伪**，真正驱动为场景内容态
    A/B（主段 PS 分布逐项一致，与 probe 功能态无因果），见 §14.8「counts 聚类归因修正」；
- **A 类 3/3 PASS + F 类 `copy.sequence` PASS**（copies 序列、`512²→1920×1080` tail 全等）：
  共享通路与升质正交面稳定，**无新回归**。

**在档产出（本轮）**：`S4{e,e2}-extract*.json`（全套 12）、`S4{e,e2}-api-scan.txt`、
`S4{e,e2}-state-probe.txt`、`S4{e,e2}-cubefaces.{json,log}`、`S4{e,e2}-cube-*-face*.png`
（6×2 张 1024²）、`S4e-tex-desc.txt`（544/552 = 1024² 配对取证）、`S4{e,e2}-pass7-pixels.json`、
`S4{e,e2}-backbuffer.{json,png}`、`S4d~S4e-compare.json`、`S4e2~S4e-compare.json`、
`S4b~S4e-compare.json`。

**v0.12.0 回码（2026-10-04，plan-B 已入码）**：`src/poc-presenter/main.cpp` —— context
vtable **槽44（RSSetViewports）/ 槽45（RSSetScissorRects）绑定感知拦截**：仅当 ① 视口/裁剪
恰 512² 且 ② `OMGetRenderTargets→GetResource` 解析回升质后的 probe cube（对象身份）时改写
1024²，其余原样下传——不做无条件 512² 全拦（防误伤其他 512² pass）。槽号双证 = 官方
`d3d11.h` MIDL 声明序（IUnknown 0-2 + DeviceChild 3-6 + 接口偏移 7+37 / 7+38）+ xosh
vtable 表（44/45）；原值模块安全阀（d3d11.dll/renderdoc.dll）与槽5 同款；`probe=0 /
vulkan=0` 同门不挂。新增 sanity 日志三行：

- `探针升质: ctx vtable=… slot44(RSSetViewports) 原值=… 来自 …; slot45(RSSetScissorRects) …`
- `探针升质: ctx槽44/45 已挂 — probe cube 绑定中 512²→1024² (v0.12 plan-B)`
- `探针升质: RSSetViewports 512²→1024² (probe cube 绑定中) 第 N 次`

**S4f 重抓验证口径（同 §14.1 协议：同存档同机位、稳定后 F12 连抓两张）**：判据① 保持
`GetDebugMessages` 0 条；判据② = probe 段 `RSSetViewports` 实测 **1024²**（对照 S4e/S4e2
恒 512²）；判据③ = 六面**全幅**内容（四象限互异、uniq8 拉高，对照 S4e 仅 TL 1/4 象限）。
三判据全中 → 特性②过闸，阶段1 全绿收口；任一不中按新增日志分支归因（ctx 槽未挂 / 改写
计数为 0 / 绑定解析未命中）。**→ 2026-10-04 S4f/S4f2 实测三判据全中，见 §14.8。**

### 14.8 S4f 验证记录（2026-10-04，v0.12.0 plan-B 三判据实测 → 特性②过闸）

**证据链**：日志 sanity 全中（`==== poc-presenter v0.12.0` banner、`ctx vtable=… slot44/45
原值=… 来自 renderdoc.dll`、`ctx槽44/45 已挂`、`RSSetViewports 512²→1024² (probe cube 绑定中)
第 N 次` 改写计数日志 117 行、末次 **第 29696 次**）→ S4f/S4f2 双帧 api-scan → cubefaces 双帧
（六面内容 + `GetDebugMessages`）→ state-probe（RT/DS 绑定 + vp 实测）→ tex_desc（配对尺寸
硬取证）→ 提取 12/12 `errors=0` → 三组 compare（S4f2→S4f / S4e→S4f / S4b→S4f）→ pass7/
backbuffer 四路互证（双帧 `SENTINEL_FOUND`、洋红 4/4、三角 `(86,217,85)`）。

**三判据结果（§14.6 表逐项）**：

| 判据 | 结果 | 证据 |
|---|---|---|
| ① OM 绑定修复 | ✅ **PASS** | 双帧 `GetDebugMessages -> 0 msgs`（S4d=8 条）；state-probe `RT=ResourceId::544` 实底 + `DS=ResourceId::552` 全非零（S4f2 slice=4）；`S4f-tex-desc`：**544=1024² RGBA16F TextureCubeArray、552=1024² D24S8** → `RT/DSV` 尺寸配对保持 |
| ② viewport 适配 | ✅ **PASS** | 双帧 api-scan 各 **8 处 `Width=1024.0,Height=1024.0`**（S4f=ev17/44/2277/2295/2402/2429/4496/4514；S4f2=ev17/44/742/760/885/912/2979/2997——与 S4e 的 8 处 512² 事件一一对应）、**0 处 `Width=512`**；state-probe probe 绑定处 `vp=1024x1024` → plan-B 改写落地 |
| ③ 面内容恢复 | ✅ **PASS** | 六面 1024² md5 互异（S4f：`97a298afc04f…/5b6a4d95d7af…/7ba10dee3bbd…/a5548a8b2155…/e342ad8bec0a…/614ef28bd74e…`；S4f2 六面亦互异且与 S4f 不同）、uniq8=80–132（S4f2=79–135；S4e 对照 80–120、S4d=3）、PNG 434KB–1.23MB（S4e=177–394KB、S4d=5.3KB 级）；**四象限取样（`quadcheck`，每 4px）双帧全部实内容**：distinct 278–4565（如 S4f face0 `TL=3061 TR=4490 BL=3733 BR=2883`、S4f2 face0 `TL=3090 TR=4565 BL=3685 BR=2846`），x511/x512 边界两侧连续实内容（如 `(52,56,39)/(52,56,39)`），cornerBR 均非清屏色（对照 **S4e 六面 `TR/BL/BR=1` 单色 `(42,78,95)`**、x512 起即清屏色——完美闭合判据②失败态） |

**三组 compare 判读（§13.3 分级口径）**：

- **S4f2↔S4f 稳定闸：7/10 PASS、3 DIFF**——超出 §14.6「唯一 DIFF = 计数带内」预期，诚实
  记录并逐项归因，**均为帧间动态、非回归**：
  - `counts.pass1`（draws 3964↔3774、pso 136↔137、textures 448↔438）：主段 draws 仅
    1152/1205 ↔ 1145/1198（±7 带内）；差额大头在 probe 段 228+218=446 ↔ 70+218=288
    ——源于帧内 cube clear 时机漂移（S4f 首个 cube-bound clear ev2397 vs S4f2 ev880）+
    slice 轮换（S4f ev14 slice=0 / S4f2 ev14 slice=4），probe 分段边界不同所致；
  - `pass5.structure`（36 vs 37 段）：S4f2 多 ev40634–40671 `draws=2` 的 1920×1080 瞬态段
    = 帧间动态；
  - `clear.bound-profile`：同多重集、`base-only/cand-only` 均空——仅 x3/x1 分组顺序漂移；
  - 非回归依据：**A 类 3/3 PASS、`copy.sequence` PASS、conditional 全等（4 档）、
    `transparent` 257=257、CS dispatch/bytecode 全等（98 dispatch、grand 18786750）、
    `textures.formats` 14=14、`clear.target-profile` PASS**。
- **S4e→S4f（缺陷态→修复态）**：A 类 3/3 + `copy.sequence` PASS + `clear.target-profile`
  PASS（1024² ×2 两侧一致）；`counts/pass5.structure/transparent(+22)` DIFF = 场景内容态
  切换（见下「归因修正」），非 probe 回归；`pass5.structure` 的 512²→1024² 段尺寸差分 =
  判据②生效的 declared 证据。
- **S4b→S4f（基线→修复态）**：4/10 PASS、6 DIFF——declared 尺寸差分（`clear.bound/
  clear.target` 512²→1024²、`pass5.structure` 512²→1024² 段、`cube512_seg 2→0` 按设计
  恒 0，同 §14.7 修正）+ `counts/transparent` 场景态差分。

**counts 聚类归因修正（重要发现，推翻 §14.7 旧假说）**：S4f=3964/136/448、S4f2=3774/137/438
落在原「缺陷聚类」区间（S4c 3862、S4d 3822），但三判据全中 → **原「probe 功能态聚类」假说
证伪**。真正驱动是两个**场景内容态**：
- **态A**（S4b 主段 680+713、S4e 661+690、S4e2 660+689；PS 分布逐项一致）：draws ~2840、
  transparent 232–235；
- **态B**（S4d 1178+1234、S4f 1152+1205、S4f2 1145+1198；PS 分布逐项一致）：draws ~3800–3960、
  transparent 257–267。

两态与 probe 功能态无因果（S4d 与 S4f 同属态B、但一缺陷一健康），与抓帧时的场景内容（同存档
同机位下仍存在的动态内容差）相关——**§14.7「缺陷态多出 ~980 draws 随修复消失」观察项撤回，
改记为场景内容态差分（非回归项）**；§14.7 三组 compare 中该差分的旧归因同步作废。

**判据③口径工具化**：象限取样脚本 `quadcheck`（System.Drawing 内联、每 4px 采样、四象限
distinct + x511/x512 边界 + cornerBR，输出 ASCII 至 `Temp\opencode\<scene>-quads.txt`）——
已固化入 `tools/quadcheck.ps1`，参数 = scene 名。

**判定：特性②过闸（三判据全中）→ 阶段1 全绿收口**——特性①共享纹理通路 + 特性②探针升质
512²→1024² 均实证成立，`probe=0` 逃生门保留为兜底；下一步 = SSR 第二步，实施设计已落
**`docs/05`**（节点边界、D1–D5 决策、Step 1–5 计划；2048² 探针对照 demo 仍为可选小项）。

**在档产出（本轮）**：`S4{f,f2}-extract*.json`（全套 12）、`S4{f,f2}-api-scan.txt`、
`S4{f,f2}-state-probe.txt`、`S4{f,f2}-cubefaces.{json,log}`、`S4{f,f2}-cube-*-face*.png`
（6×2 张 1024²）、`S4f-tex-desc.txt`（544/552 = 1024² 配对取证）、`S4{f,f2}-pass7-pixels.json`、
`S4{f,f2}-backbuffer.{json,png}`、`S4f~S4f2-compare.json`、`S4e~S4f-compare.json`、
`S4b~S4f-compare.json`、`S4{e,f,f2}-quads.txt`（象限取样对照）。

### 14.9 SSR Step 1 侦察准备（2026-10-04，v0.13.0 挂钩前地面真值 + 槽号双证）

阶段1 全绿收口后进入水体第二步（`docs/05`）。Step 1 侦察钩挂 ctx 槽33/50 只记日志，挂钩前
先用 capture 侧数据把「判据会不会在真机上自相矛盾」核一遍——**结果推翻两条既有结论**。

**槽号双证**：`d3d11-ctx-slots.txt`（官方 d3d11.h MIDL 声明序，与 xosh 44/45 互证）=
33 `OMSetRenderTargets` / 34 `OMSetRenderTargetsAndUnorderedAccessViews` /
50 `ClearRenderTargetView` / 53 `ClearDepthStencilView` / 47 `CopyResource` /
89 `OMGetRenderTargets`。游戏 api-scan 全帧零命中槽34 → **不挂槽34**；
`main.cpp` 旧注释把 `OMGetRenderTargets` 写成 33 → 已改 89。

**新证据 `S4-api-scan.txt`**（`rdc_api_scan.py -Scene S4`，range 0-999999，
filter `OMSetRenderTargets,ClearRenderTargetView` → 142 + 9 条，4s）。state 类调用是
action，pass5/pass6 永远看不见——这是第一次拿到反射段边界的**原生调用**真值。

**发现① ev39511 的清屏目标不是 585**：
`ev39511 ClearRenderTargetView pRenderTargetView=res:ResourceId::592`，而
`ev39512 OMSetRenderTargets NumViews=2 {322,592} DS=463` = 段17 的 `321+591`（pass5 段表）
→ **view592 = 纹理591 = 段17 的 RT1（R10G10B10A2_UNORM）**，**585 整帧从不被清**。
`S4-water.md:114` 的「585 用后清」来自 pass5 `clears.rts` = `bound_targets()`（当时绑着谁）
**不是清谁**——该文件 §Clear 方法论行本已警告此坑，此行是漏改。
同表 ev39546/47 的「疑 582/336 预清」被 api-scan **确证**（view583/337 = 纹理582/336）。
→ `docs/05` D3 特征A 判据改为「**清在强特征通道进行期间**」（清谁不参与判定），否则永不触发。
→ `S4-water.md` 段表/Clear 表、`S5-night-combat.md:146` 同步修正（S5 未跑 api-scan，按 S4 口径）。

**发现② D1「同帧唯一」不成立**：全帧 36 段里有 **2 个签名完全相同的强特征段**
（判据 = 非空 RT0 恰 1 + 1920x1080 RGBA16F + 1920x1080 D24S8 DSV461）：

| 段 | 事件 | RT0 | Draw | 备注 |
|---|---|---|---|---|
| 段16 反射段 | 39270–39502 | 585 | 16 | 每 Draw 前重绑 = 16 次 OMSet |
| 段19 后期前处理 | 44021 | 321 | 1 | PS 1628（`S4-water.md:93`；S5 43684 同款） |

**判别子**（按 `ssrReconOm` 同口径回放全帧 api-scan 得出，非估算）：

| 对象 | 作 RT0 绑定 | 连续绑定段 |
|---|---|---|
| 585 反射目标 | **16** | **1** |
| 321 主 HDR | **50** | **4** |
| 339（非强特征段） | **17** | 1 |

→ **次数不唯一**（339 比 585 还多），**判别取「1 段 vs 4 段」**，且只在强特征对象集合内
有效，不能全帧海选。备选 = 段内 Draw 数 16 vs 1，但要 Draw Hook（`docs/05` 方案 C，v1 规避）。
特征B 也带判别力但靠次序：后期前处理必在场景渲染之后 ⇒ 段16 恒为帧内首个强特征段 ⇒
B 每帧只触发一次 ⇒ 恒归因段16；若真机出现更早的强特征段，「1 段 vs 4 段」是第二道保险。

**代码侧同步修正（`main.cpp` v0.13.0）**：① 强特征按「进入」计数不按次绑（否则段16 的
16 次重绑把 fs 顶到 16，「同帧恰1」判据必挂）；② A/B fired 只在 Present 复位（否则段19
进入时复位 → ev44352 换绑到 35 再触发一次 B → fb=2 误判）；③ 特征A 触发前提 =
强特征通道进行中（`g_ssrInStrong`，任一次非强 OMSet/解绑即中断）；④ 候选详情按
(对象, 强征与否) 去重再节流（防 16 次重绑刷屏）；⑤ 新增 RT0 身份表（32 项/帧，
记绑定次数 + 连续段数，Present 取数后清）。

**预期日志基线（判读用）**：`强特征=2 distinct=2 特征A=1 特征B=1`，两对象分别报
`16次/1段` 与 `50次/4段`；异常 = 有候选却强特征0 / 有强特征而 A+B==0 / A 或 B 单项>1 /
distinct>2；加载过场无候选无强特征不算异常。判读回填 `docs/05` §5 Step 1。
（原列的 `强特征>2` 已按真机判读删除 → 见 §14.10。）

**在档产出（本轮）**：`S4-api-scan.txt`（全帧原生调用真值）、`d3d11-ctx-slots.txt`
（槽号双证表，原在 `Temp\opencode\` 已归档入库）、`docs/05` D1 前置实测 + D3 特征A 修正 +
R1 更新、`S4-water.md` 段表/Clear 表修正、`S5-night-combat.md:146` 同步、
`main.cpp` v0.13.0 Step 1 侦察钩（待 CI）。

### 14.10 SSR Step 1 真机判读（2026-10-04，v0.13.0 `64a290e` 实跑）

CI `64a290e` artifact 换 DLL（`Data\SKSE\Plugins\poc-presenter.dll` 222,208 B @ 09:41:22）+
新建 `poc-presenter.ini` 写 `ssr=1`（此前该文件不存在，`iniFlag` 走默认 0 ⇒ 一个槽都不挂）。
会话 18:40:03–18:41:35，**4472 帧**，日志
`…\overwrite\SKSE\Plugins\poc-presenter.log` = **1,074,761 B / 16,660 行**（SSR 行 8,118）。

**挂载 ok**：`ini ssr=1 → 挂 ctx 槽33/50 只记日志` → slot33/50 原值来自
`Data\Renderdoc\renderdoc.dll`（常驻注入，`isD3D11Family` 放行）→ `ctx槽33/50 已挂`；
7 秒后 `上下文指针与安装时不同 → 转为按 vtbl 观察全部上下文` —— **设计好的 fallback 生效**。

**判别子成立（本轮核心结论）**：

| 对象 | RT0 绑定 | 连续段 | S4 api-scan 真值 | 判定 |
|---|---|---|---|---|
| `…1D3C90` | 10~19 次 | **1 段** | 585 = 16次/1段 | **反射段** |
| `…1D47D0` | 10~370 次 | **4~6 段** | 321 = 50次/4段 | **主 HDR** |

零重叠；2607 个打印帧中 **2496 帧反射段即 obj1**。`distinct` 恒 ≤2（2481 帧=2 / 126 帧=1）。

**A/B 判据**：特征A 游戏帧 **100% =1/帧**（A=0 仅 25 帧，全在菜单期 1–1220、无清 → 正常），
归因 64/66 = 反射段，被清对象 64/66 = `1920x1080 R10G10B10A2` = 段17 RT1 格式；
特征B **100% =1/帧**，游戏帧样本 48/54 = `非空=2 (1920x1080 RGBA16F)` = 段17 MRT×2。
→ **二选一定 B**（见 `docs/05` D3）。

**异常判据修正（本轮唯一缺陷）**：2571 条 `[异常]` **逐条回放验证，全部只因旧规则 `fs>2`**，
其余五条判据（`fs==0&&fc>0` / `fs>0&&A+B==0` / `A>1` / `B>1` / `distinct>2`）**一次都没触发**。
根因 = `fs` 是「进入次数」不是「唯一性」，S4 只有单帧真值 fs=2，真机主 HDR 有 2~3 次强特征
进入 ⇒ fs=3~4 恒成立。代价：8,118 行 SSR 里 7,713 行是这 2,571 帧 ×3 行的重复汇总、日志冲到
1 MB，**真异常被淹没** → 已删 `fs>2`（`main.cpp` `ssrReconPresent`），下次跑图预期 0 条
`[异常]`、日志 ≈30 KB。

**附带发现**：段16 缺席帧 = **连续单窗 `f2552–f2652`**（101 帧 ≈1.7 s，该窗 `distinct=1`
且唯一对象 `runs=5` = 只剩主 HDR）—— 非随机掉帧 ⇒ 取景无反射的场景状态，钩子稳定；
由每 60 帧定期打印可见，不设专门判据。

**判读方法备忘**：日志是 `/utf-8` + `ofstream` 原样写的 **UTF-8 无 BOM**，PS5.1 `Get-Content`
默认按 ANSI 读会把中文判成乱码（本轮首次 `anomaly_lines=0` 即此假象）——
统计一律 `[System.IO.File]::ReadAllLines($p, [System.Text.Encoding]::UTF8)`；
含中文的 `.ps1` 须先补 BOM（`0xEF,0xBB,0xBF`）否则 PS5.1 按 ANSI 读脚本直接解析失败。

**判读结果**：`docs/05` §4 D1 末「Step 1 真机判读」+ §5 Step 1 行 + R1 关闭 + D3 二选一。

**复跑确认（`45edc18`，19:19:54–19:21:08，3480 帧，日志 97,765 B / 1,424 行）**：

| 项 | `64a290e` | `45edc18` |
|---|---|---|
| 日志体积 | 1,074,761 B | **97,765 B**（↓91%） |
| SSR 行 | 8,118 | **526** |
| `[异常]` | 2,571 | **0** |
| 汇总行 | 2,607 | **63 = 5 + 3480/60**（精确吻合） |

**`fs>2` 已消失的硬证据**：本轮 `fs=3` 出现 15 次，旧规则下这 15 帧必带 `[异常]`，实测 0 条。
其余判据复绿：`distinct` 恒 ≤2（40 帧=2 / 23 帧=1）、反射段 `runs=1 ×53` vs 主 HDR
`runs=4 ×35 / runs=6 ×5`、特征A 游戏帧 41 帧 A=1（A=0 的 22 帧全在菜单期）、特征B 100%=1。
→ **Step 1 收口。**

**会话中 3 条 `跳过` 与 SSR 无关**（`git log -S` 定年份）：`dummy3 (FLIP_DISCARD) 创建失败
0x80070005` 来自 `e31b9a0`（v0.6.0）、`方案B Present 入口字节与预期不符` 来自 `380edbf`
（v0.7.0）—— 两者都是 PoC-B 老诊断；方案B 函数级 detour 装不上只丢「独立计数」口径，
vtable 层 Present 钩仍在（`vtbl[8]==我们的Present钩子 ✓`），PoC-B 通路照常
（3600 帧 / 0.87 ms/帧 / 59.7 FPS）。

### 14.11 SSR Step 2a/2b 跑图判读模板（2026-10-04，`v0.14.0`，**待实跑**）

**挂载前**：确认 DLL 已换成新 artifact；`poc-presenter.ini` 需要**两行**——
```
ssr=1
ssr.sentinel=1        ← 2b 回写开关, 只测 2a 就删掉或写 0
```
（ini 只在启动时读一次 → **改完必须重启游戏**。`ssr.sentinel=1` 时反射区会显示成
**可辨识的"旧场景色"错误画面，这是预期内的验收现象，不是 bug**。）

**判读 checklist**（日志仍用 §14.10 的 UTF-8 读法）：

| # | 判据 | 预期 | 不符时怀疑 |
|---|---|---|---|
| 1 | `ini ssr=1 → 挂 ctx 槽33/47/50` | 出现 1 行 | DLL/ini 没换 |
| 2 | `slot47(CopyResource) 原值=… 来自 d3d11.dll/renderdoc.dll` | 与 33/50 同款 | 槽号错 → 查 `d3d11-ctx-slots.txt` |
| 3 | `ctx槽33/50/47 已挂` | 出现 | 若只 `33/50` = slot47 校验没过（有降级日志） |
| 4 | `[2a] 场景色快照 324 = 0x…` | **恰好 1 行**（换代才多） | **0 行** → 游戏没走 `CopyResource`，见 D2a-2 排除法复核 |
| 5 | `[2a] 深度快照 520(假设) = 0x…` | 恰好 1 行 | 0 行同样问题；**多行** → "本帧首条"判据失效，看 `槽47Copy#` 详情核对 `461→466` 位置 |
| 6 | 汇总 `COPY=3` | 游戏每帧 3 条（520/324/466；S2 室内 2 条） | `COPY=0` 见 #4；偏大 → 看 `槽47Copy#` 谁在多调 |
| 7 | 强特征对象行的 `[585]`/`[321]` 标注 | `[585]` 落 `runs=1`、`[321]` 落 `runs>1`，每帧 | 标注互换/缺失 → runs 判据或帧错位 |
| 8 | `特征B(换绑)…[2b哨兵已排队]` | ssr.sentinel=1 时**实机场景每帧 1 次**；**菜单/加载期不出现是预期**（`distinct=1` 只设一个身份变量 → `g_ssrMainHdr != g_ssrReflRt` 守卫不过，见 `docs/05` D2a-3a） | 实机恒不出现 → `g_ssrReflRt` 没学到（查 #7）或段16 缺席窗 |
| 9 | `[2b] 哨兵#K 585=0x… <- 324=0x… 帧=F [desc一致\|desc不一致!]` | 与 `哨兵=` 计数同步增长；`585=` 应等于 `[585]` 的对象；**必须是 `[desc一致]`** | **`[desc不一致!]`** = `CopyResource` 被 D3D11 静默丢弃（返回 void 无 HRESULT）→ 行尾两个 desc 直接比对找差异；**计数在涨但无 `[desc一致]` 后缀** = 同因；**计数恒 0** = 武装条件没过 → 看 #7 与 `g_ssrMainHdr != g_ssrReflRt` 守卫（菜单期 `distinct=1` 恒不 arm 是**预期**，须进到实机场景）|
| 10 | 目视 + 抓帧 | **反射区变"旧场景色"错误画面**；候选帧段17 后 321 内容 = 324。**实测形态 = 水几乎消失**（见下实跑表） | 画面无变化 → 哨兵没真跑（回看 #8/#9），或段17 不读 585 |
| 11 | `[异常]` / 帧时 | 0 条；`0.87 ms/帧` 量级不劣化 | — |

**过关 ⇒ 2a/2b 收口，硬约束 5（必须 in-frame 拦截）验通，进 2c（共享入向 + descriptor/sampler）。**

#### 14.11.1 实跑确认表（2026-10-04，`407c8b3`，5520 帧 / 190,494 B / 2,688 行）

**11/11 全绿。** 会话 `20:37:46–20:39:32`（约 106 s），日志由旧的 97,765 B 追加位重置后新写。

| # | 实测值 | 判定 |
|---|---|---|
| 0 | banner `v0.14.0 … Step2b 通路哨兵 ssr.sentinel` | ✅ 新 DLL |
| 1/2/3 | `ini ssr=1` + `ini ssr.sentinel=1` + `ctx槽33/50/47 已挂`；`slot47 原值=00007FFCE57A3530 来自 renderdoc.dll`（与 33/50 同款）；`原值不在` **0 行** | ✅ 零降级 |
| 4 | `[2a] 场景色快照 324 = 000002289AE75CA0 (src=000002289AE75CD0 1920x1080 RGBA16F)` **恰好 1 行**，**帧=1** 即认出 | ✅ |
| 5 | `[2a] 深度快照 520(假设) = 000002289AE75520 (src=… D24家族)` 1 行，**帧=951** —— 正是 `COPY` 由 1→3 的那帧 | ✅ "本帧首条"判据生效 |
| 6 | 实机帧 **`COPY=3` 恒定**；菜单/加载期 `COPY=1`（只有 324 那条，无深度拷贝） | ✅ 游戏自己的拷贝**未被钩子影响**（我们自己的被 `g_ssrSelfCopy` 排除）|
| 7 | `[585]` 行 **恒 `1个连续段`**（`runs=1` ×86、`2` ×9）；`[321]` 行 **恒 ≥2 段**（`2`×9 / `3`×2 / `4`×75）；两对象不同：**585 = `000002289AE75190`**、**321 = `000002289AE75CD0`** | ✅ 身份判别子成立 |
| 8 | `[2b哨兵已排队]` 70 条（节流），**首条 `帧=963`，哨兵#1 = `帧=952`** | ✅ 见下"守卫生效" |
| 9 | `[2b] 哨兵` 42 条日志：**`[desc一致]=42` / `[desc不一致!]=0`**；恒 `dst=000002289AE75190 <- src=000002289AE75CA0` | ✅ **拷贝真生效**，非静默丢弃；**写目标 = `[585]` 那个 runs==1 对象**，身份链三端对上 |
| 10 | **水几乎消失（"基本看不出有水"）** | ✅ 见下"机制" |
| 11 | `[异常]` **0 条**；`distinct` 分布 `1×22 / 2×75`（恒 ≤2）；PoC-B **0.88–0.98 ms/帧**（Step1 为 0.87） | ✅ 无劣化 |

**累计**：`哨兵=4415` 次写入 / 5520 帧（自 952 帧起，减去段16 缺席窗）；`特征B` 日志 102、`特征A` 87。

**D2a-3a 双身份守卫按设计生效（本轮最干净的一条证据）**：
帧 1–951 是菜单/加载期，`distinct=1` 且那个唯一强特征段 `runs==1` 是**菜单对象**（恰 = `...CD0`），
于是 `g_ssrReflRt` 被污染成 `...CD0`；帧 420 起 `g_ssrMainHdr` 也学到 `...CD0`
→ 两值**相等** → `g_ssrMainHdr != g_ssrReflRt` 守卫不过 → **951 帧内 0 次误写**。
进实机场景后段16 出现（`runs==1`）→ `g_ssrReflRt` 纠正为 `...75190`
→ 两值不同 → 守卫过 → **下一帧（952）立刻 arm**。若没有这条守卫，菜单期就会把 324 写进菜单纹理、
进场景后还可能写进主 HDR（段19=321 也 `!= 污染值` 时）。

**水消失的机制（§14.10 预判"反射区旧场景色"的修正）**：
`324` 是段16 **之前**的 321 快照 → **那一刻水还没画**；段16 的 16 Draw 才把水面/反射结果写进 585；
哨兵在段17 的 `OMSet` 之后、段17 的 Draw **之前**把 585 覆盖成 324
→ 段17 的 PS 17586 读 585 写回 321，读到的是"**没画水的场景**" → 水的可见贡献被换成背景 → **水基本看不见**。
预判的形态（"旧场景色"）不准，但**结论不变：段17 读到了我们写的内容 ⇒ 硬约束 5「必须 in-frame 拦截」验通**。

**收口动作**：`poc-presenter.ini` 已把 `ssr.sentinel` 改回 **0**（正常玩水即恢复；`ssr=1` 保留，
2a 身份行照常打）。开关只在启动时读 → 改完须重启游戏。

**判读脚本**：`Temp\opencode\run3.ps1`（11 条 checklist 自动化，含 banner 版本自检与 desc 判据结论）→ `run3.out`；
2c 用 `run2c.ps1`（§14.12 的 12 条自动结论）→ `run2c.out`。**判读脚本已入库 `tools/run2c.ps1`
（2026-10-06，P2-1；含 BOM，PS5.1 可直跑）—— 以仓库副本为准，改判据先改它再提交。**
**两个 PowerShell 坑**（都导致"统计恒 0 / 只出 1 行"，极易误判成"日志里没这些行"）：
1. `$matches` 在 `Where-Object` 块内**不外泄**到父作用域 → 统计 `captures` 必须在 `foreach`
   里自己再 `-match` 一次。
2. **PowerShell 变量大小写不敏感** → `foreach ($l in $L)` 里的循环变量 `$l` 与数组 `$L`
   是**同一个变量**，循环一跑就把 `$L` 就地覆盖成最后一行，之后所有统计恒 0、
   "末 N 行"只出 1 行。循环变量一律改用与数组不同的名字（脚本里统一 `$x` + `$lines`）。
3. **含中文的 `.ps1` 必须补 UTF-8 BOM**（`0xEF,0xBB,0xBF`）——PowerShell 5.1 对无 BOM 的
   UTF-8 文件按 ANSI 读，注释里的中文会变乱码、进而**破坏脚本解析**（`check1.ps1` 加了
   中文注释检查后，无 BOM 时把 `/*=1` 打成空并误报"注释提前关闭"；补 BOM 后同一份脚本 `OK`）。
   纯 ASCII 脚本不用。判断脚本是否漏 BOM：`[System.IO.File]::ReadAllBytes($f)[0..2]` 不是
   `239,187,191` 就是漏了。

**坑：C++ 块注释里千万别出现 `*/` —— `**/` 就是 `*/`**（v0.16.0 实踩）。
日志样例写成 `**一致✓**/ **不一致✗**`，其中 `**/` 把 `/** … */` **提前关掉**，之后整段
文件头文档被当代码解析，报出一串 `error C2146/C4430/C3872/C3873`，且报错行号全在**注释区**
（`main.cpp(365)` 附近）——看到 `C3872: 'U+2717' not allowed in an identifier`、
`missing ';' before identifier '本帧VK读回'` 这种"中文/符号被当标识符"的错，
**第一时间查 `*/` 提前闭合**，而不是去改那几行代码。
自校验已固化：`check1.ps1` 检查 `/*` 与 `*/` 个数相等 **且** 全文无字面 `**/`。

**读 CI 编译错误的正确姿势（不用抓日志）**：`actions/*` 的日志端点
（`/actions/runs/{id}/logs`）**必须认证**，未认证返 403；但工作流里 `::error::` 生成的
**注解 API 是无认证可读的**：
```
GET https://api.github.com/repos/{owner}/{repo}/commits/{sha}/check-runs
GET https://api.github.com/repos/{owner}/{repo}/check-runs/{id}/annotations
```
（`User-Agent` 必带；`annotations_url` 字段是空的，要自己按 `/check-runs/{id}/annotations`
拼。）`build.yml` 的编译步已有 `| Select-Object { if ($_ -like '*error*') { '::error::' + $_ } }`
这层转换，所以编译错误会完整出现在注解里。脚本：`Temp\opencode\cianno3.ps1` → `ci1ddanno2.txt`。

### 14.12 SSR Step 2c-α（共享入向·D3D11 侧）跑图判读（`v0.15.0` 已实跑，**2026-10-04**）

> **实跑结论：色全绿、深度 R2 未过**（详见本节末"实跑结果"）。归因重试 + 2c-β 版 = `v0.16.0`。

**挂载前**：换新 artifact；`poc-presenter.ini` 要**三行**——
```
ssr=1
ssr.sentinel=0        ← 2b 验收后已关; 保持 0, 否则水会再消失, 干扰 2c 判读
ssr.shared=1          ← 2c 入向开关 (v0.15.0 新增)
```
（ini 启动时只读一次 → **改完必须重启游戏**；**必须进实机场景**，菜单期 `g_ssrMainHdr ==
g_ssrReflRt` ⇒ 不 arm，是预期。**2c 只往我方镜像写，不改画面** —— 与 2b 不同，**看不到任何
视觉变化才是对的**。）

**判读 checklist**（12 条；日志读法同 §14.10）：

| # | 判据 | 预期 | 不符时怀疑 |
|---|---|---|---|
| 0 | banner `v0.15.0 … Step2c 共享入向 ssr.shared` | 版本对得上 | DLL 没换 |
| 1 | `ini ssr.shared=1 → 2c 共享入向 …` | 恰好 1 行 | ini 没写 / `ssr=0`（会打"总门"行） |
| 2 | `[2c] 色324 SHARED 镜像 OK = 0x… (1920x1080 RGBA16F) handle=0x… 源=0x…` | **恰好 1 行** | 0 行 → 身份没学齐（见 §14.11 #4/#5）；失败行见 #4 |
| 3 | `[2c] 深度520 SHARED 镜像 OK = 0x… (1920x1080 D24家族) handle=0x…` | **恰好 1 行** ← **R2 主判据** | 0 行 → 走 #4 归因 |
| 4 | 若失败：`[2c] … CreateTexture2D 失败 0x… (… D24家族 …) ← R2 风险点` 或 `CreateSharedHandle 失败 0x… ← R2 风险点: D3D11 建得出但 DXGI 不让共享` | 无此行 = **R2 全过**；有 = **R2 半过**（色过、深度不过，日志指明是哪一步） | 见左列两种措辞的区别 |
| 5 | `[2c] 共享入向就绪 — 色324=OK 深度520=OK; 基线校验和(建好未拷) 色=0x… 深=0x…; 自此每帧…` | 恰好 1 行，紧接 #2/#3 之后 | 缺 → `ssrInBuild` 中途 return |
| 6 | `[2c] 入向拷贝#K 色324=0x…→0x… 深520=0x…→0x… 帧=F` | 前 8 条 + 每 128 条 | 0 行 → 见 #9；`深=跳过(R2 未过)` → 见 #4 |
| 7 | **`[2c读回 色=0x…≠基线✓ 非零N 深=0x…≠基线✓ 非零M]`** | **`≠基线✓` + `非零` 大于 0**（前 3 次 + 每 600 次） | `=基线✗` → **`CopyResource` 静默失败**（desc 不匹配），回看 #2/#3 的 desc；`非零=0` → 读回内容全空 |
| 8 | 汇总 `… COPY=3 哨兵=0 入向=N \| 累计候选=…` | `入向=N` **持续增长**；`COPY=3` 仍是 3；`哨兵=0` | `入向=0` 见 #9；`COPY>3` → 我们自己的拷贝漏进计数（`g_ssrSelfCopy` 失效） |
| 9 | `特征B(换绑)#K … [2c入向已排队]` | 实机场景每帧 1 条 | 无 `[2c入向已排队]` → `ssr.shared=0`，或双强特征格局没确立（`g_ssrMainHdr == g_ssrReflRt`，还在菜单/加载期） |
| 10 | 目视 | **画面无任何变化**（2c 不改渲染）；PoC-B 帧时仍在 `0.9 ms/帧` 量级 | 有视觉变化 → 不该有，回查是否 `ssr.sentinel` 误开 |
| 11 | `[异常]` | 0 条 | 有 → 同 §14.11 #11 归因 |

**过关 ⇒ 2c-α 收口，R2 解除**（D24 深度与 FP16 都能建 `SHARED` + 被 DXGI 允许共享 + 拷进去读得回），
进 **2c-β**（VK `OPAQUE_WIN32` 导入两图 + `vkCmdCopyImageToBuffer`/`vkMapMemory` 交叉校验，
两函数已在 `POCB_DEV_FNS`，零新增 VK API）。

**若深度那条红了**：`R2` 只过一半 → 先确认失败的是 `CreateTexture2D`（D3D11 不让这种格式
做 SHARED）还是 `CreateSharedHandle`（建得出但 DXGI 拒绝共享），再定 2c-β 是否改用
**`VK_FORMAT_D24_SFLOAT` 自建深度图 + 从 520 拷过去**的降级路（此时深度不经共享内存，
需要一次 GPU 拷进 VK 可见的自有图，走 PoC-B 已有的 `vkCmdCopyImage` 通道）。

#### 14.12.1 实跑结果（2026-10-04，`3be86c3`，3455 帧，2 会话）

**判读脚本先踩了一个坑**：`poc-presenter.log` 是**跨会话追加**的（备份≠清空，上一次 v0.14.0
的会话仍在文件前段，v0.15.0 从第 2689 行起）。首版脚本取"第一个 banner"⇒ 把旧会话的
`ini ssr.sentinel=1`、`[2a]=2`、`ctx槽挂=2` 全算进新会话，误报 9 项 FAIL。**修法**：脚本先扫
最后一个 `==== poc-presenter ` 行，**只切片分析该行往后的部分**（输出里多打
`sessions=N analyzing=LAST`）。⇒ 以后任何判读脚本都要先做会话切片。

| # | 实测 | 结论 |
|---|---|---|
| 0 | banner `v0.15.0 … Step2c 共享入向 ssr.shared` | ✅ |
| 1 | `ini ssr.shared=1`=1、`ssr.sentinel=1`=**0**、`ctx槽33/50/47 已挂`=1 | ✅（哨兵确认关着） |
| 2 | `[2c] 色324 SHARED 镜像 OK` = 1 | ✅ **FP16/1920×1080 SHARED 建得出** |
| 3 | `[2c] 深度520 SHARED 镜像 OK` = **0** | ❌ **R2 主判据未过** |
| 4 | `[2c] … CreateTexture2D 失败 0x80070057 (1920x1080 D24家族 mips1 msaa1) ← R2 风险点` | ❌ `0x80070057 = E_INVALIDARG`，**卡在 `CreateTexture2D`（不是 `CreateSharedHandle`）** |
| 5 | `[2c] 共享入向就绪 — 色324=OK 深度520=FAIL(R2 未过); 基线 色=0xB9D103FD6854A325 深=0x0` | ✅ 1 行 |
| 6 | `[2c] 入向拷贝#1…#2560`，`深=跳过(R2 未过)` | ✅ 32 行 |
| 7 | `[2c读回]` **7 行全 `≠基线✓`、`=基线` 0 条**；非零 7143–7157 / 8192；7 个校验和互不相同 | ✅✅ **拷贝真落地、内容随场景真实变化**（非零率 87% = 真实图像） |
| 8 | 末值 `入向=2576`、`COPY=3`、`哨兵=0` | ✅ |
| 9 | `[2c入向已排队]`=40（= arm 后 (3400−845)/64 的节流口径 ✓）、`[2b哨兵已排队]`=0、`[2b]哨兵执行`=0 | ✅ |
| 10 | 画面无变化；PoC-B **0.91 ms/帧**（2b 验收时 0.88，+0.03 含每帧 16.6MB 镜像拷贝） | ✅ |
| 11 | `[异常]` = 0 | ✅ |

**结论**：
- **色通路（R2 的 FP16 半边）完全验通** —— 11/12 条绿，仅 #3/#4 因深度格式红。
- **R2 的深度半边未过，病因锁定在 `CreateTexture2D` 报 `E_INVALIDARG`**，尚存两种解释：
  ① `D24 家族格式`不在 D3D11 `SHARED|NTHANDLE` 白名单（**大概率**）；
  ② 源带 `D3D11_BIND_DEPTH_STENCIL` 而 SHARED 不许带。
  ⇒ **`v0.16.0` 加了归因重试**：首次失败后按 `BindFlags` 递减（`SRV`→`0`）再试两次、逐次记
  `[2c]   重试 … → OK/仍失败`，**Format 一律不动**（`CopyResource` 要求 src/dst 格式完全一致）。
  三种全失败 ⇒ 病因=格式 ⇒ 深度不能走 D3D11 SHARED，须改道
  （**`v0.16.0` 实测：两次重试全 `0x80070057` ⇒ 病因确为格式，见 §14.13.1**）。
- **2c-β 不被阻塞**：VK 侧代码按"镜像存在才导入"写，色必有、深度看重试结果，两种结局都兼容。

### 14.13 SSR Step 2c-β（VK 导入 + 交叉校验）跑图判读模板（`v0.16.0` 首跑见 §14.13.1；`v0.16.2` 复跑见 §14.13.2；归因三件套 `v0.16.3` 见 §14.13.3；变体B `v0.16.4` 见 §14.13.4；轮换单图 `v0.16.5` 见 §14.13.5；**定案 `v0.16.6` 见 §14.13.6 = 2c 已收口**）

**前置**：同 §14.12 的 ini 三行（`ssr=1` / `ssr.sentinel=0` / `ssr.shared=1`），另外
**PoC-B 必须开着**（`vulkan`/`pocb` 不为 0）—— 2c-β 借用它的 device/queue/command pool/fence，
PoC-B 一关这步自然不跑。仍**不需要**改画面，目视上应与 2c-α 一样"什么都没变"。

**判据**（`run2c.ps1` 已覆盖 #0–#11，2c-β 另看下面 5 条；日志读法同 §14.10）：

| # | 判据 | 预期 | 不符时怀疑 |
|---|---|---|---|
| β1 | banner `v0.16.6 … β3归因三件套 + 轮换单图槽(handle类型/LINEAR/全程GENERAL) 定案D3D11_TEXTURE_BIT单图常驻` | 版本对得上 | DLL 没换 |
| β2 | `[2c-β] VK 导入 OK: 色镜像 NT handle → VkImage (首次, …) 1920x1080 RGBA16F, 读回buffer=16588800B host-coherent, 命令已录 …` | **恰好 1 行**（首槽建成后那一行；此后各槽改建图行 `槽K(名字) 建图+导入OK`）← **2c-β 主判据** | 0 行 → 看下面"导入失败归因"四类日志 + `[各槽结果]` 的换槽行 |
| β2a | `v0.16.1` 新增：`[2c-β]   导入矩阵…` + `归因探测A/B` + `[2c-β]   归因结论: …` | 导入一次成功时应有 `导入矩阵成功: 第1/N 组合`；失败时应有矩阵全失败行 + 两张探针 + 一句归因结论 | 只有矩阵行没探针 → 代码没跑归因；探针也缺 → 见 §14.13.1 |
| β3 | `[2c-β] 交叉校验#K 槽N(D3D11句柄) D3D11=0x… VK=0x… **一致✓** VK非零=… 本帧VK读回=…ms 帧=F` | **全部 `一致✓`，样本 ≥4**（`v0.16.6` 单参数已定案，`NSlot=1` 图常驻）← **2c 收口的最终证据** | 出现 `不一致✗` = 抖动 → 看 `[v0.16.3 归因(只统计不一致的行)]`；一条都没有 → 见 β5 节奏/PoC-B 是否先挂 |
| β4 | `[2c] 重试 深度520 … → …` 归因行 | 有行则读出"病因是 BindFlags"还是"病因是格式" | 无重试行 = 深度首次就建成功（R2 全过，意外之喜） |
| β5 | 交叉校验行数 | `v0.16.6` 单图常驻：**只有首次建图那 1 次机会不比**，之后每次机会都出一行 ⇒ 约为 §14.12 #7 的 `[2c读回]` 行数 −1 | 行数远少于该值 → PoC-B 先挂了（看 `PoC-B 失败:` 行，2c-β 会静默不跑） |
| β6 | PoC-B 累计均值 | 仍在 **0.9 ms/帧** 量级（交叉校验刻意排在 PoC-B 统计**之后**，不污染该指标） | 明显变大 → 回查是否每帧都在读回（节流失效） |
| β7 | `[异常]` **＋ `PoC-B 失败:` 行（其中 `code=-4`）** | 0 条 / 0 / 0 ← **三角必须全程在画上** | `code=-4` = 设备丢，**2c-β 自己的提交可能还是成功的**，`-4` 由 PoC-B 的提交报出（§14.13.5 的第二次事故）→ 换掉刚测的那个参数；其余 `[异常]` 同 §14.11 #11 |

**导入失败归因**（`[2c-β] … → VK 交叉校验停用`，一次只会出现其中一条）：

| 措辞 | 含义 |
|---|---|
| `vkCreateImage(外部内存) = …` | VK 不接受这张图的格式/用法 |
| `vkAllocateMemory(NT handle 导入) = …` | **OPAQUE_WIN32 导入被拒** —— 最有信息量的一条，说明 D3D11 `SHARED_NTHANDLE` 与 VK 的内存类型/句柄语义对不上 |
| `vkBindImageMemory(色镜像)` | 导入内存与 image 的 memoryTypeBits 不匹配 |
| `色镜像格式非 RGBA16F (…)` | 身份学错了（应不可能，2c-α 已验过色镜像是 RGBA16F） |

**`**不一致✗**` 归因（三种，按可能性排序）** —— **`v0.16.3` 起判读不再靠猜**，交叉校验行自带三个归因字段，
`run2c.ps1` 会打一行 `[v0.16.3 归因] 同刻=VK(时序) N | 前帧=VK(差一帧) N | 按D3D11行距=D3D11(行距) N | 镜像帧内被改 N`：

| 字段判据 | 命中含义 | 处置 |
|---|---|---|
| `同刻D3D11=0x…=VK✓时序差` | Present 时**当场**再读一次 D3D11 侧得到的值与 VK 相等 ⇒ 布局/行距都没问题，**是两次读的时刻不同**（帧内镜像被再次写过） | 查镜像在特征B 之后还有谁在写 |
| `前帧D3D11=0x…=VK✓差一帧` | VK 读到的是**上一帧** D3D11 的值 ⇒ VK 提交时机比 D3D11 写早 | 调读回提交时机（挪到特征B 之后） |
| `VK按D3D11行距=0x…=D3D11✓行距归因` | 用 D3D11 STAGING 的 `RowPitch` 重算 VK 才相等 ⇒ **行距口径不同**（两边抽样点位不同，内容其实一样） | 抽样统一按同一 pitch（D3D11 侧改紧密 pitch 或 VK 侧带 pitch），2d 前必须钉死 |
| `≠帧内(镜像帧内被改)` | 同刻再读就与帧内那次不同 ⇒ 镜像在帧内被改过 | 时间窗有洞，先修窗 |
| 三项全 `0` | 以上都排除 ⇒ **归因1：布局/字节序**（VK 图布局与 D3D11 物理布局不一致） | **已定案（§14.13.5）**：病因 = **handle 类型** —— `OPAQUE_WIN32` 不解析句柄布局，须用 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`，v0.16.5 实测 `VK==D3D11` 逐字节一致。`v0.16.4` usage 已排除（§14.13.4）；槽1 `LINEAR` 不一致；**槽2「全程GENERAL」把设备搞丢（`code=-4`），该保守姿势作废**（§14.13.5） |

（原始归因，`v0.16.3` 之前的版本只能靠这些推）：
1. **布局/字节序视图不一致** —— D3D11 写的物理布局与 VK 在 `GENERAL⇄TRANSFER_SRC` 下读到的不一致。
   这正是 2c-β 要测的核心未知数；若恒不一致，下一步改试 `VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL`
   之外的更保守姿势（如全程 `GENERAL` 不转 layout、只用 access mask 做内存栅栏）。
2. **跨 API 时序** —— D3D11 的 `Map` 已保证其队列跑完，理论上不该发生；若只有前 1–2 次不一致、
   之后稳定一致，就是首帧布局初转的残留（代码已跳过建成当帧的比对）。
3. **抽样口径不同** —— 不可能：两边用的是**同一个 `ssrFnvSample`**，D3D11 侧行距是 `RowPitch`、
   VK 侧是 `width*8`（`bufferRowLength=0` = 紧密排列），若镜像 STAGING 的 RowPitch 恰好 ≠ `width*8`
   则抽到的像素点位不同 —— 这种情况会**恒不一致且非零数接近**，看 β3 的 `VK非零` 与 §14.12 #7 的
   `非零` 是否接近即可分辨（接近 → 是行距问题，不是内容问题）。

**过关 ⇒ 2c 收口，入向通路（D3D11 写 → VK 读）验通**，进 **2d**（出向回写 + descriptor/sampler，
把 SSR 采样真正接进 `POCB_DEV_FNS`）。

#### 14.13.1 首跑结果（2026-10-04，`c5357c1` = `v0.16.0`，2040 帧，1 会话，`run2c.ps1` 判读）

**#0–#11 复跑与 §14.12.1 同样全绿**（banner `v0.16.0 … Step2c-β` ✅、`ini` 三行 ✅、色镜像 OK=1、
`[2c读回]` 5 行全 `≠基线✓` 且 `=基线` 0 条（非零 7121–7157/8192）、`入向=1288`、`COPY=3`、
`哨兵=0`、`[2c入向已排队]`=20、`[异常]`=0、PoC-B 0.93–1.02 ms/帧、画面无变化）——
**2c-α 色通路复验通过，本次新信息全在 β4/β2 两条**：

| # | 实测 | 结论 |
|---|---|---|
| β4 | `[2c]   重试 深度520 BindFlags=0x00000008 (原 0x00000048) → 仍失败 0x80070057 = 病因是格式`<br>`[2c]   重试 深度520 BindFlags=0x00000000 (原 0x00000048) → 仍失败 0x80070057 = 病因是格式`<br>`[2c] 深度520 三种 BindFlags 全失败 → 结论: D24家族 (Format=44) 不在 D3D11 SHARED 白名单 —— R2 病因=格式, 深度不能走 D3D11 SHARED, 需改道` | ❌→**归因收口**：三种 `BindFlags`（`0x48`→`0x08`→`0x00`）全报 `E_INVALIDARG` ⇒ **病因=格式**，排除"DEPTH_STENCIL 不许带"。**深度确定不能走 D3D11 SHARED**，按 §14.12 末尾改道：VK 自建 `VK_FORMAT_D24_SFLOAT` 图 + `vkCmdCopyImage`（520→VK 图） |
| β2 | `[2c-β] vkAllocateMemory(NT handle 导入) = -2 (0xFFFFFFFE) → VK 交叉校验停用 (D3D11 侧入向照常)` | ❌ `-2 = VK_ERROR_OUT_OF_DEVICE_MEMORY`；**卡在导入分配，不是 `vkCreateImage`、不是句柄类型**。D3D11 侧入向照常，只有交叉校验没跑 |
| β3 | 交叉校验 0 行 | ⏸ 由 β2 直接导致 |

**β2 归因（为什么 `v0.16.0` 一定失败）**：同一套导入代码里 **PoC-B 的 512×512 `RGBA8` 导入一直是好的**
（`PoC-B init: 共享图导入 OK (D3D11 NT handle → VkImage, OPAQUE_WIN32)`），而色镜像是
**1920×1080 `RGBA16F`** —— 两者只差三样：

1. **格式**（`RGBA8` vs `RGBA16F`）；
2. **尺寸/对齐**：`512×512×8 = 2,097,152` 恰好页对齐，`1920×1080×8 = 16,588,800` **不对齐**；
   D3D11 共享分配八成向上对齐，而 `mai.allocationSize` 填的是 **VK 自己算的 `req.size`** —— 与句柄
   真实分配大小对不上时 NV 驱动就返 `-2`；
3. **内存类型**（`pocbMemType` 选的 `DEVICE_LOCAL` 可能不是导入要的那个）。

⇒ **`v0.16.1` 加了两级归因**（`ssrInVkBuild` 内）：
- **导入矩阵**：`[尺寸候选 × 允许内存类型]` 逐个试，尺寸候选 = `req.size` / 其 64KB 对齐 / 其 2MB 对齐 /
  朴素 `W×H×8` / 朴素 64KB 对齐 / 朴素 2MB 对齐；成功打
  `[2c-β]   导入矩阵成功: 第K/N 组合 size=… (VK报=… 朴素=…) type=… → 病因=…`，
  全失败打 `[2c-β]   导入矩阵 N 次全失败 码=… (size候选x类型) VK报size=… 朴素size=…`；
- **2×2 探针**（矩阵全失败才跑，各建一张 D3D11 SHARED 纹理、导入一次即销毁）：
  `归因探测A RGBA8@1920x1080(只换格式)` 与 `归因探测B RGBA16F@512x512(只换尺寸)`，
  四种组合直接落成一句话 `[2c-β]   归因结论: 病因是格式/尺寸/…`。

| 归因结论措辞 | 含义 | 改道方向 |
|---|---|---|
| `病因是格式`（A 成 B 败） | `RGBA16F` 跨不了 API | 色通路换格式，或退回 D3D11 STAGING 读回（2c-α 已验通） |
| `病因是尺寸`（A 败 B 成） | 大图 `allocationSize` 与句柄真实分配对不上 | 按对齐口径修 `allocationSize`（矩阵里赢的那个 size 就是口径） |
| 两个探针都成 | 单独格式/尺寸都没问题 | 看矩阵哪一组合赢，是组合差异 |
| 两个探针都败 | 只有"既小又页对齐"能导 | 格式与尺寸同时卡 → 换小块分片拷，或走 2d 的 VK 自建图 |

#### 14.13.2 复跑结果（2026-10-05，`54ab042` = `v0.16.2`，本会话约 16200 帧，`run2c.ps1` 判读）

**#0–#11 与 §14.13.1 同样全绿**（banner `v0.16.2` ✅、`ini` 三行 ✅、色镜像 OK=1、`[2c读回]` 5 行全 `≠基线✓`
且 `=基线` 0 条（非零 7149–7153/8192）、`入向=1622`、`[2c入向已排队]`=26、`COPY=3`、`哨兵=0`、`特征B`=215、
`[异常]`=0、PoC-B **0.54–0.59 ms/帧**（低于 Step1 基线 0.87）；`#3/#4` 按设计落入 `[已知/告警]` 不挡 PASS）。
**本轮新增信息 = β2 归因收口 + β3 首次拿到样本 + 帧时基线首跑**：

| # | 实测 | 结论 |
|---|---|---|
| β2 | `[2c-β] VK 导入 OK: 色镜像 NT handle → VkImage (OPAQUE_WIN32) 1920x1080 RGBA16F, 读回buffer=16588800B` | ✅ **首次成功**（v0.16.0 卡在 `vkAllocateMemory=-2`）。`ssr.shared=1` 时 β2 恰 1 行 |
| 矩阵 | `[2c-β]   导入矩阵成功: 第5/5 组合 size=16588800 (VK报=17694720 朴素=16588800) type=1 → 病因=尺寸口径 (VK 报值不成, 换对齐值才成)` | ✅ **归因收口 = 尺寸口径**：`allocationSize` 必须用**被导入对象**（D3D11）的朴素 `W×H×8=16588800`；VK 自己算的 `req.size=17694720` 是"新建图"的口径，导入时不成立（`req` 与 `req+2MB` 等 4 个候选全败）。内存类型 `type=1` 一次就对，排除类型因素 |
| β4 | `三种 BindFlags 全失败 → 结论: D24家族 … R2 病因=格式` | 与 §14.13.1 一致（已知项） |
| β3 | 交叉校验 **4 行 / 一致 0 / 不一致 4**，`VK非零=7148–7158` vs D3D11 `7149–7153`（**量级接近**） | ❌ **唯一 FAIL**。按 §14.13 归因表，"非零接近"正是**行距/时序/布局**三者的共同签名（内容没坏，是抽样点位或时刻不同）——`v0.16.2` 没带归因字段，只能猜 ⇒ **已修**，见下 |
| 帧时基线 | 27 行 `帧时基线 …`，`帧时基线: GPU 时间戳查询就绪 (TIMESTAMP+DISJOINT)` 出现 1 次（查询建成、非阻塞读生效） | ✅ **功能首跑通过**：CPU 均值 0.64–1.46 ms、`>20ms=0/600`、GPU 样本≈帧数一半（300/600，符合"两帧一样本"预期）、`丢弃>=1s=1`。**但本局 FPS 在 99–804 之间跳（vsync 没锁）⇒ 这局的数不是可比基线**，只证明通路能出数 |

**下一步（`v0.16.3`，已实现在 54ab042 之后的提交）**：交叉校验行加**归因三件套**——
① Present 时当场再读一次 D3D11（`同刻D3D11`）② 上一次 D3D11 校验和（`前帧D3D11`）
③ 按 D3D11 `RowPitch` 重算 VK（`VK按D3D11行距`）——三个字段把归因1/2/3 一次跑图分清，
`run2c.ps1` 自动打 `[v0.16.3 归因]` 一行。**β3 归因出来前 2c 不算收口。**

**判读脚本**：`run2c.ps1` 已加 `[矩阵]` / `[探针A]` / `[探针B]` / `[结论]` 四类输出，
并把 `#3/#4`（R2 深度红）挪进 `[已知/告警]` —— R2 归因为"格式"后属**已知项**，不再挡 PASS；
banner 期望改为 `v0.16.2`。

#### 14.13.3 归因三件套实测（2026-10-05，`4f19a01` = `v0.16.3`，约 4000 帧，`run2c.ps1` 判读）

**#0–#11 全绿、β2 矩阵仍 `size=16588800 type=1`、β4 已知项照旧；唯一新信息是 β3 归因字段——
四项计数全是 `0`：**

```
[v0.16.3 归因] 同刻=VK(时序) 0 | 前帧=VK(差一帧) 0 | 按D3D11行距=D3D11(行距) 0 | 镜像帧内被改 0
```

原始样本（4 次交叉校验，全部 `**不一致✗**`）：

```
交叉校验#1 D3D11=0x5611EF4B690267D9 VK=0x32D120DA3FFEEF3F **不一致✗** VK非零=7152
  行距=15360/15360 同刻D3D11=0x5611EF4B690267D9≠VK 前帧D3D11=0x435E17F1296EB62F 本帧VK读回=11.92ms 帧=2561
```

| 字段 | 实测 | 排除依据 |
|---|---|---|
| 行距 | `15360/15360` **两边相等** ⇒ `VK按D3D11行距` 字段**没出现** | **归因3（行距）排除**：D3D11 STAGING `RowPitch` 与 VK 紧密 `width*8` 一样，抽样点位相同 |
| 同刻 D3D11 | `0x5611…` **等于** 帧内那次 `D3D11=0x5611…` ⇒ 走 `≠VK` 分支 | **"镜像帧内被改"（时间窗有洞）排除**：镜像从特征B 到 Present 之间没被人再写 |
| 同刻 D3D11 | 与**同一时刻**的 `VK=0x32D1…` **不等** | **归因2（时序）排除**：两边同一物理时刻、同一块内存读出的字节不同 |
| 前帧 D3D11 | `0x435E…` **≠ VK** ⇒ `=VK✓差一帧` 没命中 | **"差一帧"排除**：VK 读到的不是上一帧的值 |
| `VK非零` | 7148–7158 vs D3D11 7149–7153（量级接近） | 内容没坏（读到的确实是这张图的像素分布），差在**哪些字节** |

⇒ **结论：落在归因1 —— VK 图对这块内存的字节视图与 D3D11 的物理布局不一致。**

**病因最可能项（仓内对照，`v0.16.4` 据此设计）**：PoC-B 的 **512×512 RGBA8** 走同一套
`OPAQUE_WIN32` 导入、`VK_IMAGE_TILING_OPTIMAL`、读回拷贝，**是能与 D3D11 双向对上的**
（画面上屏验过）；两者的差异只剩 **VK image 的 `usage`**——

| | PoC-B（对得上） | 2c-β 主图（对不上） |
|---|---|---|
| D3D11 侧 BindFlags | `BIND_RENDER_TARGET` 系 | `BIND_RENDER_TARGET \| BIND_SHADER_RESOURCE` |
| VK `usage` | `COLOR_ATTACHMENT \| TRANSFER_SRC` | **只有 `TRANSFER_SRC`** |
| tiling / handleType / 格式对齐 | OPTIMAL / OPAQUE_WIN32 / 逐字段 | 同左（一致） |

⇒ 驱动给"**只读传输图**"挑的物理布局，很可能与 D3D11 给"RT+SRV 图"用的不是同一套
（压缩/分块形态不同）⇒ 这正是归因1。

**`v0.16.4` 变体B（一次跑图定死，不必一参数一跑）**：同一块导入内存再建**第二张** VkImage
（逐字段复制主图，只把 `usage` 换成 `COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC|TRANSFER_DST`），
有自己的导入分配 + 初转/读回命令；交叉校验连着提交两次、**打两个 hash**：

| 字段判据 | 含义 | 下一步 |
|---|---|---|
| `变体B=0x…=D3D11✓usage病因` | 变体读出的值与同帧 D3D11 相等 ⇒ **usage 就是病因** | 主图改同款 usage ⇒ β3 应转 `一致✓`，2c 收口 |
| `变体B=0x…=主图(usage非病因)` | 两张图读法一致但都与 D3D11 不等 | usage 排除，下轮试 tiling（`LINEAR`）/ handle 类型（`D3D11_TEXTURE_BIT`） |
| `变体B=0x…≠两边` | 变体也读不出 D3D11 内容 | 布局归因仍在，按备选姿势改（全程 `GENERAL` 不转 layout） |
| 无 `变体B=` 字段 | 变体没建成（看 `变体B …` 失败行）或 DLL 不是 `v0.16.4` | 看 banner 与变体日志 |

变体建不成 / 提交失败都**只关自己**，不拖累主图；`run2c.ps1` 新增 `[v0.16.4 变体B]` 段
（建成行数、`命中共/非病因/≠两边` 三计数、一句结论），并把"主图不一致但变体命中"改判为
`[已知/告警]` 而非 FAIL。**banner 期望 = `v0.16.4`。**

> **该设计已真机跑过，结论 + 事故 + 替代设计见 §14.13.4（`v0.16.4` 实测 → `v0.16.5` 轮换单图）。**

#### 14.13.4 变体B 实测（2026-10-05，`5ab3491` = `v0.16.4`，2520 帧）：usage 排除 + DEVICE_LOST 事故

**结果一（实验成立，结论明确）**：变体B 建成 —— `变体B 建成: usage=COLOR_ATTACHMENT|SAMPLED|TRANSFER 尺寸=16588800 类型=1`，交叉校验#1 打出两个 hash：

```
[2c-β] 交叉校验#1 D3D11=0xA8EEA37F79112EB9 VK=0xC498A22FC017739F **不一致✗** VK非零=7150
  行距=15360/15360 同刻D3D11=0xA8EEA37F79112EB9≠VK 前帧D3D11=0xB6CEB938BB7DC11E
  变体B=0xC498A22FC017739F=主图(usage非病因) 本帧VK读回=18.36ms 帧=1051
```

⇒ **`变体B` 与主图的 hash 完全相同、两者都 ≠ D3D11 ⇒ `usage` 不是病因**（两张图对同一块内存的
解读一致，VK 侧没有分歧）。归因1 仍然成立，剩下候选：**handle 类型 / tiling / layout 转换姿势**。
（附带：`[v0.16.3 归因]` 本轮仍是四项全 `0`；`本帧VK读回` 从 11.9ms 涨到 18.36ms = 多读一次变体。）

**结果二（事故，已入档）**：**同一块导入内存被两张 VkImage 同时绑定 ⇒ `VK_ERROR_DEVICE_LOST`**

```
[2026-10-05 14:05:29] PoC-B 失败: vkQueueSubmit / vkWaitForFences code=-4 (0xFFFFFFFC) → 已关闭注入, 游戏照常渲染
```

- 时间点 = 交叉校验#1 之后一帧（帧1051）⇒ 变体B 的提交把这块 `VkDevice` 搞丢；
- **连坐后果**：PoC-B 注入被关 ⇒ **画面上 VK 渲染的三角消失**（用户能看到的唯一表征）、
  `PoC-B 注入 N 帧` 统计行停在 600 帧、本会话只剩 **1 次交叉校验**（样本不够）；
- 判读识别：`PoC-B 失败: … code=-4` + `[异常]` 行 + PoC-B 统计只有一条 + 三角不见了。

**⇒ `v0.16.5` 改成「轮换单图」：任何时刻只有一张 VkImage 持有这块导入内存。**

| 槽（按序） | 只改什么 | 检验什么 |
|---|---|---|
| `0 D3D11句柄` | `handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT` | **头号嫌疑**：`OPAQUE_WIN32` 的语义是"我不解析句柄的布局信息"，而被导入对象就是 D3D11 纹理 ⇒ 只有该类型才让驱动按 D3D11 的布局去解读 |
| `1 LINEAR` | `tiling = VK_IMAGE_TILING_LINEAR` | D3D11 共享纹理若按 pitch-linear 存，VK 用 `OPTIMAL`（tiled/swizzled）解读必然错位 |
| `2 全程GENERAL` | 命令不转 `TRANSFER_SRC`，直接在 `GENERAL` 下 copy（栅栏只用 access mask） | §14.13 归因表"处置"列一直挂着的保守姿势 |
| `3 对照原样` | 什么都不改（`OPAQUE_WIN32` + `OPTIMAL` + 转 layout） | 自证轮换机制没引入新变量：它仍应 `不一致✗`（= v0.16.2/3/4 的已知结果） |

**节奏（决定要跑多久）**：每个槽要**两次**交叉校验机会 —— 第一次建图+初转（**本帧不比**，
`UNDEFINED→GENERAL` 按规范可能丢内容），第二次才读回比对（这帧特征B 已拷过，内容是新的），
读完 `vkDeviceWaitIdle` + `vkDestroyImage`/`vkFreeMemory` 再换槽。交叉校验节奏是"前3次 + 每600帧"
⇒ 4 槽全覆盖 ≈ 8 次 ≈ **帧 4100**。**只跑到 2500 帧是不够的。**

| `run2c.ps1 [各槽结果]` 判据 | 含义 | 处置 |
|---|---|---|
| `槽[D3D11句柄]: 一致 N` | handle 类型就是病因 | 主图改成该参数 ⇒ 2c 收口 |
| `槽[LINEAR]: 一致 N` | tiling 是病因 | 同上 |
| `槽[全程GENERAL]: 一致 N` | 布局转换姿势是病因 | 同上 |
| `槽[对照原样]: 一致 N` | **意外**：与已知结果不符 ⇒ 轮换机制引入了新变量 | 先查轮换本身（初转时机/重录命令），再信其它槽 |
| 四槽全 `不一致` | 归因1 仍在 | 换下一轮候选（`D3D11_TEXTURE_KMT`、或改用"小探针图 + CPU 写已知 pattern"的实验室法） |
| `[v0.16.5 轮换单图] DEVICE_LOST 行 > 0` | 这个槽又把 device 搞丢了 | 看 `[异常]` 行定位到槽 ⇒ **该槽参数本身危险**，下版把它换成更保守的做法；判 FAIL |

`run2c.ps1` banner 期望 = **`v0.16.5`**；`[v0.16.5 轮换单图]` 段给
`建图成功行 / 建图失败换槽行 / 导入失败换槽行 / DEVICE_LOST 行` 四个计数 + 四槽明细。

#### 14.13.5 v0.16.5 实测（2026-10-05，`4cce61e`，约 6300 帧）：**槽0 命中 ⇒ 病因定死 = handle 类型**

| 机会 | 槽 | 结果 | 关键行 |
|---|---|---|---|
| #2（帧873） | **槽0 `D3D11句柄`** | **`一致✓`** | `D3D11=0x7581B71D2B801E3D VK=0x7581B71D2B801E3D **一致✓** VK非零=7143 槽0(D3D11句柄) 行距=15360/15360 本帧VK读回=11.75ms` |
| #4（帧1471） | 槽1 `LINEAR` | `不一致✗` | `VK=0x0BF952C8CE7EFB9F`，读回 11.41ms（没丢设备，也没解决问题） |
| #6（帧2671） | 槽2 `全程GENERAL` | `不一致✗` | `VK=0x6C38D52BC35D5CBF`，**读回 19.43ms**（正常 11.4ms） |
| — | 槽3 `对照原样` | **没跑到** | 帧2671 后 PoC-B 已挂，2c-β 随之不跑 |

**结论一（2c 的病因定死）**：`handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`
与 D3D11 **逐字节一致** ⇒ 归因1（布局/字节序）的病因 = **handle 类型用错**。
`OPAQUE_WIN32` 的语义是"我不解析这个句柄的布局信息"，被导入对象却是 D3D11 纹理 ⇒
只有 `D3D11_TEXTURE_BIT` 才让驱动按 D3D11 的布局去解读。

**旁证（同一轮日志里的矩阵行）**：
- 槽0 `D3D11_TEXTURE_BIT`：`第1/1 组合 size=17694720 (VK报=17694720 朴素=16588800) → 首个组合即成`
- 槽2 `OPAQUE_WIN32`（原样）：`第5/5 组合 size=16588800 (VK报=17694720) → 病因=尺寸口径`

⇒ `D3D11_TEXTURE_BIT` 下驱动报的 `req.size=17694720` **直接可分配**；`OPAQUE_WIN32` 下同一个
17694720 会失败、必须换到对齐值 16588800 才成 —— **这正是 v0.16.1 那个 `VK_ERROR_OUT_OF_DEVICE_MEMORY(-2)`
的来由**。两个独立观察互相印证：`OPAQUE` 从头到尾就是错的语义。

**结论二（事故，第二次 DEVICE_LOST，判读口径已修）**：
```
[2026-10-05 14:47:07] SSR侦察: [2c-β] 交叉校验#6 … 槽2(全程GENERAL) … 本帧VK读回=19.43ms 帧=2671
[2026-10-05 14:47:07] PoC-B 失败: vkQueueSubmit / vkWaitForFences code=-4 (0xFFFFFFFC) → 已关闭注入, 游戏照常渲染
```
- **同一秒**：槽2 的读回（不转 layout、直接在 `GENERAL` 下跨 API 并发读，只用 access mask 做
  栅栏）之后，PoC-B 的下一次提交报 `-4` ⇒ **该姿势有害，槽2 已从表里删除**。
- 与 §14.13.4 的区别：**这次 2c-β 自己的提交是成功的**，`-4` 是 PoC-B 的提交报出来的 ⇒
  旧 `run2c.ps1` 只数 `[异常]`/`DEVICE_LOST` 行会**漏抓**（本轮就漏了，把三角消失判成"无异常"）。
  已改：新增 `PoC-B 失败:` 行计数 + `code=-4` 子计数，命中即 FAIL。
- 现象链（供下次比对）：`PoC-B 注入 N 帧` 统计行停在 2400 → `[2c-β]` 行停在帧2671 →
  三角消失 → 2c-β 静默不跑（`ssrInVkBuild` 开头的 `c.state != 2` 直接 return，无日志）。

**⇒ `v0.16.6`**：槽表只剩 `D3D11句柄`（`NSlot=1` ⇒ `g_ssrVkRotate=false` ⇒ **图常驻**，
读完不销毁），每次交叉校验机会都出一行对比；用 **≥4 次 `一致✓`** 确认后 2c 收口。

| `run2c.ps1` 判据（v0.16.6） | 预期 | 不符 |
|---|---|---|
| banner | `v0.16.6 … 定案D3D11_TEXTURE_BIT单图常驻` | DLL 没换 |
| `[2c-β 单图] DEVICE_LOST(2c-β 自己)` | 0 | 看 `[异常]` |
| `PoC-B 失败行` / `其中 code=-4 设备丢` | **0 / 0** ← 三角必须全程在画上 | 换参数把设备搞丢了 |
| `[各槽结果] 槽[D3D11句柄]` | **一致 ≥4 / 不一致 0** | 见下 |
| β3 `一致/不一致` | 全部一致；<4 次一致 → `[已知/告警]` 建议多跑 | 有不一致 = 抖动，查归因字段 |
| `[v0.16.3 归因(只统计不一致的行)]` | 无不一致行 ⇒ 打"不需要归因" | 有不一致才统计（一致时"同刻==VK"必然成立，旧口径会误报 1 次时序差） |

#### 14.13.6 定案实测（2026-10-05，`7442b5f` = `v0.16.6`，约 3900 帧）：**2c 收口**

| 判据 | 实测 | 要求 |
|---|---|---|
| banner | `v0.16.6 … β3归因三件套 + 轮换单图槽(handle类型/LINEAR/全程GENERAL) 定案D3D11_TEXTURE_BIT单图常驻` | 对上 ✓ |
| β3 交叉校验 | **6 行，一致 6 / 不一致 0**（帧981/982/1579/2179/2779/3379） | 全部一致且 ≥4 ✓ |
| `[各槽结果] 槽[D3D11句柄]` | **一致 6 / 不一致 0** | ✓ |
| 每行关键字段 | `D3D11=0x…` 与 `VK=0x…` **完全相同**；`VK非零=7081~7150`；`行距=15360/15360`；`本帧VK读回=11.25~11.83ms` | ✓ |
| `DEVICE_LOST(2c-β 自己)` / `PoC-B 失败行` / `code=-4` | **0 / 0 / 0** ← 三角全程在画上 | ✓ |
| `[异常]` | 0 | ✓ |
| PoC-B 累计均值 | 0.88 / 0.89 / 0.92 / 0.94 / 0.98 ms/帧（基线 0.87） | ≈1ms 不劣化 ✓ |
| `[2c读回]` | 7 行全 `≠基线`，`含 =基线 = 0` | 拷贝真落地 ✓ |
| `[v0.16.3 归因(只统计不一致的行)]` | 无不一致行 ⇒ 打 `不需要归因 (全部一致)` | ✓ |
| R2 深度 | 仍 `病因是格式`（D24 不在 D3D11 SHARED 白名单） | **已知，2d 改道** ✓ |

**单图常驻的节奏验证**：首次建图那 1 次机会不比（`交叉校验#1` 只建不读），之后**每次机会都出一行**——
`[2c-β] 交叉校验 6 行` vs `[2c读回] 7 行` = `7−1` ✓ 与 β5 的预期完全一致。

> **Step 2c-β 收口结论**：`handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`
> 让 D3D11 `SHARED|NTHANDLE` 色镜像（1920×1080 RGBA16F）在 VK 侧读出的字节与 D3D11 侧
> **逐帧逐字节一致** —— **入向通路（D3D11 写 → VK 读）正式验通**。
>
> 排障链条（三轮、每次只改一个变量）：
> ① `usage` 排除（§14.13.4 变体B `hash=主图`）→
> ② `双图并存` 弃用（§14.13.4 第一次 DEVICE_LOST）→
> ③ `LINEAR` 否决 / `全程GENERAL` 否决 + 第二次 DEVICE_LOST（§14.13.5）→
> ④ **handle 类型定案**（§14.13.5 槽0 一致 + §14.13.6 六次复验全绿）。
>
> 余项（全部进 2d，见 `docs/05` D2a-4）：出向回写（`ssr.sentinel=1` 那条 2b 通路 + 共享出向拷贝）、
> descriptor/sampler 接 `POCB_DEV_FNS`、**深度改道**（D24 不能 SHARED ⇒ VK 自建
> `VK_FORMAT_D24_SFLOAT` 图 + `vkCmdCopyImage`）、帧时基线按 §14.14 固定场景重采进 R3。

> 备注：本会话日志里 `VK 导入 OK … 之后每槽现建现毁 (v0.16.5)` 那行是上一版的**措辞残留**
> ——v0.16.6 起单图常驻、读完不销毁，不再"现建现毁"。只是文案过时，不影响判读。

### 14.14 帧时基线（`v0.16.2`，`frametime`）跑图判读

**动机**（审查提出、`docs/00` 历史欠账）：阶段2 与 SSR Step4 的过闸判据一直是"帧时不劣于基线 5%"
（`docs/00:96`、`docs/05:386` R3），**但基线数从来没实测过**——闸没有数可比；后续 compute culling +
indirect draw 要按帧预算调剔除力度/间接 draw 上限，`docs/00` §6.5 动态帧预算也都要这份数据。

**日志行**（每 600 帧一次，紧跟既有的 `Present 计数 … 近600帧 … FPS` 那行，两行分开免得破旧匹配）：

```
帧时基线 CPU: 均值 16.62 中位 16.61 p95 17.05 最大 42.10 ms >20ms=3/600
      | GPU: 均值 16.70 中位 16.65 p95 17.90 最大 45.20 ms (样本 301)
      | 会话 CPU n=3600 均值 16.70 最大 60.10 / GPU n=1806 均值 16.80 最大 95.20
      | 丢弃>=1s 2
```

| 字段 | 口径 | 判读 |
|---|---|---|
| `CPU` | **Present-to-Present 帧间隔**（`steady_clock`，每帧 1 样本） | `均值/中位/p95/最大` 四档分布；只给均值会被尖峰骗 |
| `>20ms=N/M` | 本窗内超过 20ms 的帧数 | 60 FPS 目标下 >16.7ms 即慢一帧；>20ms 是"明显可感"的口径 |
| `GPU` | `D3D11_QUERY_TIMESTAMP` + `TIMESTAMP_DISJOINT`：Present 处 `End(ts0)` 开窗、下次 Present `End(ts1)` 收窗 ⇒ 一帧的 **GPU 提交跨度** | 与 CPU **语义不同，别混用**：CPU=帧提交间隔，GPU=GPU 忙时长 |
| `(样本 N)` | 通常 ≈ 帧数的一半 | D3D11 规定同一时刻只允许一个 disjoint 活跃，三条（Disj/Ts0/Ts1）全拿到才开下一窗 ⇒ 约 2 帧 1 样本，**属预期不是缺陷** |
| `无样本` | 拿不到 immediate context 或查询没建成 | PoC-B / 探针 / `ssr` 全关时没有 ctx 来源 ⇒ 只记 CPU 侧（正常降级，日志另有 `GPU 时间戳查询就绪/创建失败` 行说明） |
| `会话 …` | 自本局第 1 帧起的累计 | 只有均值/最大/计数（不分位）——要分位就看窗口行 |
| `丢弃>=1s` | 单帧间隔 ≥1s（读盘、切场景、加载）**不进分布**只计数 | 这类不是帧时，混进 p95 会把基线毁掉 |
| `>20ms` 计数在**排序前**统计 | 代码里 `ftStats` 会就地排序 | 判读不用管，但改代码时别在排序后数 |

**采基线的标准姿势（给 R3 / 阶段2 闸用）**：

1. **基线局**：`ssr=0`（三槽全不挂，零注入）—— 进目标场景（S4 水面 / S5 火焰战斗）固定机位，
   静置 ≥600 帧（至少 2 行窗口统计），抄下 `CPU 均值/p95` 与 `GPU 均值`。
2. **注入局**：同一机位、同一配置只改 `ssr=1`（+ 要测的特性开关），同样 ≥600 帧。
3. **判定**：`(注入 − 基线) / 基线 ≤ 5%` ⇒ R3 过闸；两个场景各测一次（`docs/00:96` 的 S4、S5）。
4. **注意**：`frametime=0` 关掉本统计（默认 1）；`>20ms` 计数和 `丢弃>=1s` 一起看——注入局
   `丢弃>=1s` 明显变多说明是加载差异不是帧时回归，先归因再下结论。

**采集局日志约束（必须遵守，否则基线失真 —— 2026-10-06 补，审查 P0-6）**：

- **基线局与注入局必须使用完全相同的 ini**（含所有 `ssr.*` 开关），**日志节流档位也必须一致**；
  两局日志行数差一个量级 ⇒ CPU 帧时不可比。
- `logLine` 在**游戏线程**上做文件 open/write/close（`main.cpp` logLine，见修复 C-1：每行一次
  `std::ofstream` 析构），行数密集时是**主线程 I/O 尖峰**，会直接顶高 `帧时基线 CPU` 分布。
- 采基线时建议把日志调到最低档，或**对照一次「日志全关」局**验证 CPU 帧时差异；
- 判读脚本需输出 `日志行数/秒`，**超阈值时该局作废重采**。

**诚实边界（本步没证明的事）**：① GPU 时间戳取的是"Present→Present 的 GPU 提交跨度"，
不是每个 pass 的分段耗时（要分段得再加打点）；② 分辨率切换/设备重建后三只查询是否仍有效
**未实测**——若出现 GPU 行恒为"无样本"或数值异常，先怀疑这里；③ 查询打点本身有极小开销
（每帧 2 次 `End`），未单独标定，但与 PoC-B 的 0.9ms 量级相比应不可见。

### 14.15 SSR Step 2d-1（出向回写）跑图判读（`v0.18.0` `ec94958` CI SUCCESS，**待实跑**）

> **先决**：`poc-presenter.dll` 换成 `ec94958` 轮 artifact；`poc-presenter.ini` 四行：
> `ssr=1` / `ssr.shared=1` / `ssr.sentinel=0` / **`ssr.vkout=1`**（本版新增，默认 0；
> 写 0 = 整轮回退成纯 2c 形态，其余行为与 `v0.17.0` 一致）。

**跑法**：2000+ 帧，跑完执行 `run2c.ps1`（已扩出 §#12）。

| # | 判据 | 期望 | 失败含义 |
|---|---|---|---|
| 0 | banner `v0.18.0` 且含 `ssr.vkout` | 是 | DLL 没换 |
| 12a | `ini ssr.vkout=1 → 2d 出向回写` | 1 行 | ini 没写，或 `ssr` / `ssr.shared` 总门没开 |
| 12b | `[2d] 出向镜像 OK` | 1 | 第 3 张 SHARED 镜像没建成 → 看 `[2d]` 关闸行 |
| 12c | `[2d] 出向图就绪` | 1 | VK 导入 / 录命令没成 |
| 12d | `[2d] 回写#K … [desc一致]` | 有行，**`[desc不一致!]` 必须 = 0** | 不一致 = `CopyResource` 静默丢弃（2b 同坑） |
| 12e | `[2d] 出向读回#K … **一致✓**` | **≥1 次** | 0 次一致 = passthrough 没逐字节还原 → 出向通路不通 |
| 12f | 帧汇总 `出向=` | 持续增长 | 恒 0 = `g_ssrOutReady` 没置上（VK 填充那步没成） |
| 12g | `哨兵=` | 仍为 0 | >0 = 2b 让位没生效 → 585 的内容归因不纯 |
| 12h | 2c 全套（§14.13.6） | 6/6 一致、事故 0、PoC-B 失败行 0、`code=-4` 0 | 出向**不许连坐**入向 2c 与 PoC-B |

**画面预期（不属回归）**：反射区显示「上一帧的场景色错误画面」——v0 passthrough 尚未做任何
屏幕空间反射，验的是通路不是画质；`F12` 抓帧看段17 的输入即为出向镜像内容。

**性能**：`[2d] VK出向#` 行的 `fence=` 是出向自己的耗时，**刻意不计入** PoC-B 的 `accMs`
（否则会污染那条 ≈0.9ms/帧的验收基线）；PoC-B 数值应与 `v0.17.0` 持平。
出向固定成本 = 每帧一次 16.6MB `vkCmdCopyImage`（GPU）+ 每帧一次 D3D11 `CopyResource`（GPU）
+ 节流的 STAGING 读回（前3次+每600次）。

**时序为什么是 1 帧延迟**（判读时若被问到）：见 `docs/05` D3「D3a-v0 落地」——
在 D3D11 钩子里同步跑 VK 要等 D3D11 GPU（全管线 stall），在 VK 里等 D3D11 更不可能
（D3D11 给不出 VK 能等的 fence）⇒ 取「上一帧填、本帧读」，代价只有 1 帧（16ms）延迟。

---

### 14.16 段16 逐 Draw 状态与 PS 反汇编取数（D-1/D-2/D-3，2026-10-06 补齐）

**为什么**：`docs/05` D3b（抑制式）的准入前置缺三件数据 —— 三个 PS 的反汇编、段16 的
blend state、段16 对 DS=461 有无实质深度写入。**没有这三件就无法判断「抑制 16 个 draw
会发生什么」**（`docs/待修复事项总结.md` D-1/D-2/D-3）。

**两条实跑命令**（均 errors 0，qrenderdoc 无头，约 30~35s）：

```powershell
# D-1: 8 个 PS 的反汇编（原 5 个 + 段16 的 15478/15990/17352）
tools\rdc_run.ps1 -Script rdc_pass6.py -Scene S4 `
  -Targets "501,509,520,324,466,366" `
  -PsWL "1567,12496,1632,12140,2612,15478,15990,17352"

# D-2/D-3: 段16 逐 Draw 的 blend/depth/stencil + 几何线索（+ 段15/17/18 对照）
tools\rdc_run.ps1 -Script rdc_seg16_state.py -Scene S4
```

**踩坑（固化）**：
- **`-PsEvents 39270-39502` 这种区间写法无效** —— `rdc_pass6.py` 的 `_ints()` 只吃
  分号/逗号分隔的**整数**，非整数静默丢弃 ⇒ 区间会被整段丢掉。要么写成 `39270,39272,...`
  的逗号枚举，要么用 **`-PsWL`（按 PS 资源 ID）**——后者正合本用例，自动在 `draw_recs`
  里找该 PS 首个绑定事件。
- `pass6` 输出文件是 `"w"` 覆盖式 ⇒ 重跑必须把**旧 PS 一并**放进 `-PsWL`，否则会抹掉
  已有的 5 个 PS 反汇编（本轮就是这么保住的）。
- 新工具 `tools/rdc_seg16_state.py` 的 API 踩坑（qrenderdoc 内嵌 Python）：
  `PipeState` **没有** `GetBlendState/GetDepthStencilState`，真名是
  **`GetColorBlends()` / `GetDepthTestState()` / `GetPrimitiveTopology()`**；
  `ColorBlend` 的字段是 **`enabled / colorBlend / alphaBlend / logicOperation / writeMask`**
  （不是 D3D11 的 `SrcBlend/DestBlend` 命名）；SWIG 结构体 `str()` 只给指针地址
  ⇒ 必须逐属性取值，否则「不同取值」会被地址刷成一堆假差异。
  同理 `Drawcall` 事件号与 `numIndices` 在 **action 对象**上（`a.eventId` / `a.numIndices`）。

**实测结果（`docs/analysis/S4-seg-state.txt` + `S4-pass6-ps-disasm.txt`）**：

| 项 | 段16 实测 | 含义 |
|---|---|---|
| 几何 | 16 个 draw 的 `numIndices` **各不相同**：6144×5、606、252、54、78、30、54、18、6、36、150、1848；全 `TriangleList` / `NoCull` / viewport 1920×1080 / scissor 未启用 | **不是 16 层全屏**，是 16 份不同几何（不同对象/表面） |
| blend | **16/16 全关**（`enabled=False`），RT0 `writeMask=7`（只写 RGB），logicOp NoOp，factor=1,1,1,1 | **不是分层合成**，是后画覆盖前画 |
| depth | `depthEnable=True` / `LessEqual` / **`depthWrites=False`** | **只测不写** ⇒ 抑制段16 不影响 461（**D-3 准入通过**） |
| stencil | **开**：`NotEqual`、ref=1、compareMask=1、writeMask=255、fail 全 `Keep` | 像素门：只在 `(stencil & 1) != 1` 的像素上画 |
| PS | 三个 PS（15478 / 15990 / 17352）**都声明 `dcl_resource_texturecube t3`** | **16 个全是反射绘制**，按材质变体分 3 组 |
| 对照 | 段15/段17：blend 关、`writeMask=15`、depth 测关写开；段18：**blend 开（SrcAlpha/InvSrcAlpha）**、writeMask=7 | 段18 才是真混合段，段16 不是 |

**据此更正的三条推论**（原文都是按「blend 分层合成」推的，实测证伪）：

1. `docs/05:439` D3b 行 —— 「只替换反射那一份（PS 17352 组）」**不成立**：16 个全是反射。
   改为：抑制式 = 吞掉 16 个 cubemap 反射 draw，**因 585 整帧从不被清 ⇒ 实际效果 = 沿用
   上一帧反射**（静止近乎无差，移动时旧帧拖影），depth 不写故不影响 461。
2. `docs/05:565` declared-diff —— 「16→N 保留层」**不成立**：改为 **16→K（K = SSR 输出
   draw 数，v1 全屏 1 个 ⇒ 16→1）**。
3. `docs/analysis/S4-water.md` 段16 行 —— 「必为 blend 分层合成、非可整体替换节点」
   改为实测口径（见上表）。
