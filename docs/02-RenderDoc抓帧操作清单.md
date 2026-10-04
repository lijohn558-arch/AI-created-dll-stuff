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
| ② 探针升质 512²→1024² | **未过闸（机制缺陷 + 捕获侧视觉回归实锤）** | 缺陷①：配对 depth 未升质 → OM 非法（8 条 High 诊断、6 面内容全灭；**初态内容链证真机 draw 亦未落地** = 游戏内反射自启动即平铺清屏色）；缺陷②：viewport 恒 512² → 只渲 1/4 面 |

- **阶段1 判定：主体过闸**——共享通路（阶段1 核心交付）实证成立；探针升质单列缺陷清单，
  可按 `probe=0` 逃生门降级，不阻塞主线。
- **v0.11 修复清单（2026-10-03 已回码 `v0.10.0→v0.11.0`，待 S4e 重抓验证）**：
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
  3. ⬜ 修完重抓 S4e/S4e2 双帧（§14.1 同协议：同存档同机位、稳定后 F12 连抓两张）→
     同三闸 + 面导出重过。

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
     `cube512_seg` conditional 2→0 应回到基线 2；viewport 512²→1024²（api-scan 口径）。
     稳定性闸 S4e2↔S4e 预期同 S4d2↔S4d（唯一 DIFF = 计数带内漂移）。
     F 类锚点（copies 3→4、tail `512²→1920×1080`、copies_total）与 A 类锚点**不应变化**——
     共享通路与升质正交，动了就是新回归。
- **下一步不被阻塞**：SSR 第二步（或 2048² 探针对照 demo）可先行决策（docs/03 §6.1）。

在档产出：`S4d*-extract*.json`（全套）、`S4c~S4d-compare.json`、`S4b~S4d-compare.json`、
`S4d2~S4d-compare.json`、`S4d-api-scan.txt`、`S4{b,d}-state-probe.txt`、`S4{b,d}-cubefaces.{json,log}`、
`S4d-tex-desc.txt`（depth 配对取证：552 = 全帧唯一 512² D24）、
`S4d-cube-*-face*.png`（6 张 1024²）、`S4b-cube-*-face*.png`（6 张 512² 对照）、
`S4{d,d2}-pass7-pixels.json`、`S4{d,d2}-backbuffer.{json,png}`。
