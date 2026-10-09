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

### 14.11 SSR Step 2a/2b 跑图判读模板（2026-10-04，`v0.14.0`，**已实跑 → §14.11.1**）

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
这层转换，所以编译错误会出现在注解里（**但注解只拿到 10 条、按行号倒序**，
拿不准就别据此断言「没有更早的错误」——见下「坑3」）。脚本：`Temp\opencode\cianno3.ps1` → `ci1ddanno2.txt`。

**坑：本机系统 DNS 解析器故障（2026-10-06）—— 与「GitHub 被墙」是两回事**：
`Get-DnsClientServerAddress` 显示以太网 DNS = `192.168.199.1`（路由器）**不应答**，
连 `Resolve-DnsName www.baidu.com -QuickTimeout` 都超时 ⇒ `Invoke-RestMethod` 全挂。
**只读绕法（不改系统配置、无需管理员）**：

1. 指定公共 DNS 查一次 A 记录：`Resolve-DnsName api.github.com -Server 223.5.5.5 -Type A -QuickTimeout`
   → `20.205.243.168`（备选 `119.29.29.29`、`1.1.1.1`；`api.github.com` 的 IP 会变，**每次现查**）；
2. **直连 IP 发请求 + 手工带 Host + 跳过证书校验**（`Invoke-RestMethod` 做不到，必须用 `WebRequest`）：

   ```powershell
   [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12
   $old = [System.Net.ServicePointManager]::ServerCertificateValidationCallback
   [System.Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }   # 用完复原 $old
   $req = [System.Net.WebRequest]::Create("https://<ip>/repos/<r>/commits/<40位sha>/check-runs")
   $req.Method = "GET"; $req.Host = "api.github.com"; $req.Timeout = 20000
   ([System.Net.HttpWebRequest]$req).UserAgent = "ps-5.1"    # ← 见下「坑2」
   ```

> **坑2**：`$req.Headers.Add("User-Agent", "ps-5.1")` 在 PS 5.1 抛
> 「必须使用适当的属性或方法修改 User-Agent 标头。参数名: name」⇒ 改用 `.UserAgent`
> 属性（变量要先转成 `HttpWebRequest`，否则属性不存在）。

脚本：`Temp\opencode\ci_once.ps1`（一次性判 RESULT）与 `Temp\opencode\ciwait_ip.ps1`（轮询后台），
均已实测拿到 `RESULT SUCCESS`（`86aa133`）。

**坑3（2026-10-06，v0.18.3 首轮 CI FAILURE）：Vulkan 结构体名写错 ⇒ 10 条错误全挤在 4 行**：

错误全在 `vkrenderer.cpp` 1927–1930（每行 2~3 条 C2065/C2146，合计恰好 10 条，因此**看不出来**是否还有更早的错误）。
真相是我凭记忆写了两个**根本不存在**的类型：

| 我写的（错） | 真名（对照 `Vulkan-Headers@main`） |
| --- | --- |
| `VkPhysicalDeviceImageFormatProperties2` | `VkImageFormatProperties2` + `VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2` |
| `VkPhysicalDeviceExternalImageFormatProperties` | `VkExternalImageFormatProperties` + `VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES` |

同批里 `VkPhysicalDeviceImageFormatInfo2` / `VkPhysicalDeviceExternalImageFormatInfo` /
`PFN_vkGetPhysicalDeviceImageFormatProperties2` **是存在的**（所以 1917–1926 一行不报）——
`vkGetPhysicalDeviceImageFormatProperties2` 的**出参是 `VkImageFormatProperties2`**，
外部内存属性靠 `VkExternalImageFormatProperties` 挂在其 `pNext` 上。

**被墙时怎么拉文件核符号**（`raw.githubusercontent.com` 直连超时、`github.com` 浏览器
`ERR_CONNECTION_CLOSED`、job 日志端点 403 要鉴权）⇒ 改用 **jsDelivr 的 GitHub 镜像**，
`webfetch` 与 Code Mode `fetch`、PowerShell `Invoke-WebRequest` **三者都通**：

```
https://cdn.jsdelivr.net/gh/<owner>/<repo>@<ref>/<path>
https://cdn.jsdelivr.net/gh/KhronosGroup/Vulkan-Headers@main/include/vulkan/vulkan_core.h
```

做法：把改动函数体里所有 `Vk*` / `VK_*` token 抽出来，逐个 `header.Contains()` 比对；
`vulkan_core.h` 查不到的再查 `vulkan_win32.h`（`VkImportMemoryWin32HandleInfoKHR` 这类
Win32 句柄结构体都在那）。本轮 38 个符号全过才 push，修完 `RESULT SUCCESS`（`624c4d0`）。

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

### 14.15 SSR Step 2d-1（出向回写）跑图判读（`v0.18.0` `ec94958` CI SUCCESS，**2026-10-06 实跑 PASS**）

> **先决**：`poc-presenter.dll` 换成 `ec94958` 轮 artifact；`poc-presenter.ini` **五行**：
> `ssr=1` / `ssr.shared=1` / `ssr.sentinel=0` / **`ssr.vkout=1`**（本版新增，默认 0；
> 写 0 = 整轮回退成纯 2c 形态，其余行为与 `v0.17.0` 一致）
> / **`probe=1`**（**不能漏、不能写 0** —— 见下方「挂载门」）。
>
> **⚠ 挂载门（2026-10-06 实跑踩坑，`installSsrRecon` main.cpp:2089）**：挂槽33/47/50 的
> 门是 `pocbEnabled() && g_probeOn && g_ssrOn`，而 `g_probeOn = iniFlag("probe", true)`
> （main.cpp:2255）。**`probe=0` 会让 `installSsrRecon` 第一行就静默 return，一条日志都不打**
> ⇒ 槽33/50/47 全没挂 ⇒ `OM=0 → 候选=0 → 入向=0 → 出向=0` ⇒ run2c 一口气 **10 条 FAIL，
> 但每一条都是同一个根因**，与 2d-1 代码、与跑图都无关。
> **判别法**：日志里搜 `ctx槽33/50` —— 出现 `已挂` 才算装上；只出现
> `ini ssr=1 → 挂 ctx 槽33/47/50` 那行**只是意图打印**（main.cpp:2261），不代表挂上。
> 该静默 return 待 v0.18.1 补一条一次性日志（挂载门未满足时打明是哪个开关关的）。

**跑法**：2000+ 帧，跑完执行 `run2c.ps1`（已扩出 §#12）。

| # | 判据 | 期望 | 失败含义 |
|---|---|---|---|
| 0 | banner `v0.18.1`（v0.18.0 那轮实跑用 `v0.18.0`，见 §14.15.1；`run2c` 的 expect 同步升）且含 `ssr.vkout` | 是 | DLL 没换 |
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

#### 14.15.1 实跑记录（2026-10-06，`v0.18.0` = `01b8911` artifact，`run2c.ps1` → **[PASS] 全绿**）

**环境**：DLL 用 `01b8911` 轮 artifact（该提交**纯 docs**，源码 = `ec94958`，故 banner 仍 `v0.18.0`）；
ini **五行** `ssr=1 / ssr.sentinel=0 / ssr.shared=1 / probe=1 / ssr.vkout=1`；12:36:12→12:38:22，
**6001 帧**（进实机跑图）。

| 判据 | 实测 | 判 |
|---|---|---|
| #0 banner | `v0.18.0` 且含 `ssr.vkout` | ✅ |
| #1 挂载 | `ctx槽33/50/47 已挂`（**唯一**装上凭证） | ✅ |
| #2/#5 镜像+就绪 | 色324 SHARED OK = 1、共享入向就绪 = 1 | ✅ |
| #6/#7 入向拷贝 | 日志 44 行、`[2c读回` **9 行 9/9 ≠基线、`=基线` = 0** | ✅ |
| #8 入向累计 | **3855**（`COPY=3`） | ✅ |
| #9 排队 | `[2c入向已排队] 61`、`[2b哨兵已排队] 0` | ✅ |
| #11 副作用 | `[异常] = 0`、PoC-B 失败行 0、`code=-4` 0 | ✅ |
| β2/β3 | VK 导入 OK 1 行、交叉校验 **11 一致 / 0 不一致**（槽[D3D11句柄] 8/0） | ✅ |
| 12a | `ini ssr.vkout=1 → 2d 出向回写` 1 行 | ✅ |
| 12b/12c | `[2d] 出向镜像 OK` = 1、`[2d] 出向图就绪` = 1 | ✅ |
| 12d | `[2d] 回写#` **1357 行，`desc一致` 1357 / `desc不一致` 0** | ✅ |
| 12e | `[2d] 出向读回` **3 行 3/3 一致✓**（v0 passthrough 逐字节还原） | ✅ |
| 12f | `出向=` 末值 **172597**（持续增长） | ✅ |
| 12g | `哨兵= 0`、`[2b] 哨兵 执行 0`（2b 全程让位） | ✅ |
| 12h | 2c 6/6 未回退（见 β3 11/11、读回 9/9） | ✅ |
| 12i | `[2d] 关闸行 = 0` | ✅ |

**R2 告警 2 项**（已知，不拦）：深度520 SHARED 仍 FAIL、重试 2 次归因 = `不是该格式`（Format=44
D24S8 不支持 D3D11 SHARED）⇒ 深度进 VK 走 2d-3 改道。

**画面（实测 = 文档预期，不属回归）**：**水体消失 + 严重拖影**。
原因：`vkout=1` 时 585 的内容被换成「上一帧的场景色」，水体 shader 拿它当反射就整片消失
（**与 2b 哨兵同观感**，故 ini 注释写 `Set 1 only to reproduce the broken-water test`）；
拖影 = 一帧延迟的场景色，属 §14.15 开头的 `v0 passthrough` 预期。**验通路不是画质**，
`ssr.vkout=0` 即恢复。

**⚠ 本轮新发现 C-6：出向回写 28.7 次/帧（应为 1 次/帧）**
- 实测 `出向=172597 / 6019 帧 ≈ 28.7`；`[2d] 回写#` 1357 行（前8+每128 ⇒ 真实 ≈172k 次）。
- 对照：2b 哨兵当年验收 = 5520 帧 **4415 次 ≈ 0.8/帧**；本文档上面「性能」段也写明
  **每帧一次** 16.6MB `CopyResource`。
- 根因：2d 回写块（`hookedOMSetRenderTargets` 内，main.cpp:2012-2060）**没有 2b 那样的
  一次性门** —— 2b 走 `g_ssrSentinelPending`（特征B 触发时置位、消费即清，main.cpp:1372），
  2d 只判 `g_ssrStrResObj == g_ssrLastStr == g_ssrReflRt` 这个**身份条件**，于是段16 的
  16 次 OMSet 重绑（每个 draw 前一次）每次都会触发 → ~29 次/帧。
- 影响：① 带宽 ~29 × 16.6MB ≈ **477MB/帧** 多余 DMA；② 日志行数膨胀 29×；
  ③ **画面不变**（最后一次写仍在段17 Draw 之前，与补上锁存后等价）——所以判读 PASS。
  实测末 600 帧 GPU 16.77→**17.81ms**（+6.3%）、`>20ms` 由 5/600 涨到 52/600，会话
  GPU 均值 17.16（基线 16.66~16.77，+2.6%~+3%），与该冗余拷贝量级吻合。
- 修法（归 **v0.18.1**，见 `docs/待修复事项总结.md` C-6）：给 2d 补与 2b 同款的一次性门
  （特征B 置位 → 首次回写即消费），并同步把 `run2c.ps1` 的 12f 判据从「>0」收紧为
  「≈ 帧数（±20%）」以免下次再漏。

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

---

### 14.17 v0.18.1 代码批真机回归（2026-10-06 13:07–13:10，`3fc6366`，`run2c.ps1` → **[PASS] 全绿**）

**这一轮验什么**：§14.15.1 通过后按计划解除代码禁改，`v0.18.1` 一次改了 6 项
（C-1/C-2/C-3/P1-1/C-5/C-6，见 `docs/待修复事项总结.md` 第 3 批）。其中 **C-2/C-3 正是 §#12 在判的那两块代码**、
**C-6 直接改变 2d 回写次数** ⇒ 必须重跑真机。

**环境**：DLL = `3fc6366` artifact（banner `expect v0.18.1: OK`）；ini 五行不变；
13:07:01 启动 → 13:10:39 结束，**Present 计数 6001、汇总帧 5940**（进实机跑图）。

| 判据 | 实测 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.1: OK`（`ssr.shared`/`Step2c-β`/`ssr.sentinel`/`ssr.vkout` 四特征全 OK） | ✅ |
| #1 挂载 | `ctx槽33/50/47 已挂 = 1`（**无** `挂载门未满足` 行 ⇒ C-5 的门是绿的） | ✅ |
| #2/#5 镜像+就绪 | 色324 SHARED OK=1、共享入向就绪=1 | ✅ |
| #6/#7 入向拷贝 | 45 行、`[2c读回` **9 行 9/9 ≠基线、`=基线` = 0** | ✅ |
| #8 入向累计 | **4026**（`COPY=3`） | ✅ |
| #9 排队 | `[2c入向已排队] 64`、`[2b哨兵已排队] 0`、`[2b] 哨兵 执行 0` | ✅ |
| #11 副作用 | `[异常] = 0`、PoC-B 失败行 0、`code=-4` 0；PoC-B **0.92~0.95 ms/帧**（基线 0.87） | ✅ |
| β2/β3 | VK 导入 OK=1、交叉校验 **11 一致 / 0 不一致**（槽[D3D11句柄] 8/0）、`不需要归因` | ✅ |
| 12a–12c | `ini ssr.vkout=1` 1 行、`[2d] 出向镜像 OK` = 1、`[2d] 出向图就绪` = 1、**关闸行 0** | ✅ |
| 12d | `[2d] 回写#` **39 行，desc一致 39 / desc不一致 0** | ✅ |
| 12e | `[2d] 出向读回` **3 行 3/3 一致✓**（v0 passthrough 字节还原） | ✅ |
| 12f | `出向= 4024`、`帧= 5940` ⇒ **出向/帧 = 0.68**（新上界判据，>1.5 FAIL / <0.5 WARN） | ✅ |
| R2 告警 | #3 深度镜像 0、#4 R2 风险点 1（归因 = 格式 ×2）= 已知，不拦 | ~ 已知 |

**C-6 修复验证（本轮重点）**：

| | `v0.18.0`（无一次性门） | `v0.18.1`（`g_ssrOutPending` 有门） |
|---|---|---|
| `出向=` / 帧 | 172597 / 6019 = **28.77 次/帧** | 4024 / 5940 = **0.68 次/帧** |
| `[2d] 回写#` 行数 | 1357 行 | **39 行**（前8 + 每128 ⇒ 与 4024 精确吻合） |
| 585 冗余拷贝 | ~477 MB/帧 | 单次 16.6MB × 0.68 |

`出向/帧 = 0.68` **不是没修好**：同局 `入向/帧 = 4026/5940 = 0.678` —— 2c 与 2d 共用**同一处**
特征B 触发（`ssrReconOm` 里那组前置：双强特征格局 + 刚离开的是 585），两者只差 2 次。
0.68 的分母含菜单/加载期根本不触发特征B 的帧（2b 当年验收 4415/5520 = 0.80 同源）。
**判据口径因此定为「出向/帧 ≈ 入向/帧，且落在 0.5~1.5」**，而不是硬盯 1.0。

**帧时基线（C-6 的性能兑现，§14.14 口径）**：

| 局 | 会话 CPU / GPU 均值 | 末 600 帧 CPU / GPU | 末窗 `>20ms` |
|---|---|---|---|
| `v0.18.0`（28.77 写/帧） | 17.14 / **17.16** ms | 18.09 / **17.81** ms | **52/600** |
| `v0.18.1`（0.68 写/帧） | **16.80 / 16.82** ms | **16.77 / 16.84** ms | **3/600** |
| Step1 基线 | — | 中位 16.65~16.66 | — |

⇒ 会话 GPU 17.16 → **16.82**（−0.34 ms），末窗 CPU 回到 16.77、`>20ms` 由 52 降到 3。
**上一轮末 600 帧抬到 17.81ms 的元凶就是那 28.77 次/帧的冗余拷贝**（当时只是推断，本轮对上了）。

**画面**：与 §14.15.1 相同 —— 水体消失 + 拖影（`vkout=1` 换掉 585 内容 = 2b 同观感，**预期，非回归**）。

**⚠ 本轮顺带查出 C-7（诊断类，归 v0.18.2）**：`[2d] 出向读回` **永远只有 3 行** ——
`g_ssrOutChkN` 在**打日志分支里**才回填（main.cpp:2090 `g_ssrOutChkN = cc`），`cc` 停在 4 ⇒
`cc % 600 == 0` 永不命中，「每 600 次采一次」从未发生（对比 2c：`++g_ssrInLogN` **进门就推进**，
所以有 9 行）。不影响 §#12 判据（≥1 一致即可），但**会话后段的帧从未被采过**。修法见 `docs/待修复事项总结.md` C-7（**v0.18.2 已修**）。

### 14.18 v0.18.2 判读模板（C-7 读回节流 + 2d-3 深度格式探测，**2026-10-06 实跑 PASS → §14.18.1**）

**这一轮验什么**：`v0.18.2` 只加两个**诊断类**功能，除下面两项外**其余行为必须与 §14.17 逐格一致**
（等价于一次回归对照）：

1. **C-7** —— `[2d] 出向读回` 节流计数改为「进门先 `++`」⇒ 后段帧也开始被采，行数从恒 3 行变成
   ≈ 3 + `0.68×帧数/600`（6000 帧 ⇒ **9~10 行**）。
2. **2d-3** —— 深度 SHARED 失败的那条结论分支里一次性跑 `ssrDepthFormatProbe()`，在 2c-α 的
   `BindFlags` 矩阵旁**再加一列格式**，为路线 1（D3D11 侧把深度转进可共享的浮点颜色镜像）
   的**前置条件**取数。**纯发现**：建出来的立刻 `Release`，不写 `g_ssrIn*`、不改本轮任何行为
   ⇒ 深度那格照旧是 FAIL，§14.17 的既有判据**不该有任何变化**。

**跑法（与 §14.15.1 / §14.17 相同）**：ini 五行全开（`ssr=1` / `ssr.sentinel=0` / `ssr.shared=1` /
`probe=1` / `ssr.vkout=1`）→ 换 artifact DLL → 进存档实机跑图 **≥2000 帧** → 先自查日志含
`SSR侦察: ctx槽33/50/47 已挂`，再跑 `tools\run2c.ps1` 判读。

**判读表（`run2c.ps1` §#0~§#13 全绿 + 下列两项新增判据）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.2: OK`（否则 DLL 没换） | — |
| **§#13 探测行数** | 深度 SHARED 不成时 = **7**（源格式×单独 `SHARED` 1 格 + 3 格式 × `BindFlags{0x00,0x28}` 6 格）；深度若哪天建成则 0，不算 FAIL | — |
| **§#13 结论行** | 有探测行时必须 = 1（0 ⇒ FAIL「结论打印被跳过，路线 1/2 无法定案」） | — |
| **§#13 `handle=OK(NT)` 格子数** | **> 0 ⇒ 路线 1 前置成立**；= 0 ⇒ 路线 1 不成立，退回路线 2（仅颜色 SSR + 屏幕边缘 fallback） | 按日志定 |
| **§#12 C-7 行数** | `[2d] 出向读回` 行数 ≥ `max(3, 帧数/1000)`（6000 帧 ⇒ ≥6，预期 9~10）；不足 ⇒ WARN「C-7 节流计数没生效」 | — |
| 其余 §#1~§#12、12f、帧时基线 | **与 §14.17 同判据同结论**（2c 读回 6/6、`出向/帧` 0.5~1.5、`desc不一致 = 0`、`哨兵 = 0`、`[异常] = 0`、PoC-B ms 未劣化） | 回归对照 |

**结论怎么读（本轮真正要拿的东西）**：

- `NT handle 可用的格子` 里**含 `R32_FLOAT` 且 `BindFlags=0x28`** ⇒ **路线 1 首选定 `R32_FLOAT`**
  （全屏 PS 可直接当 RTV 写入，再走 2c-β 已验通的 `D3D11_TEXTURE_BIT` 导入，VK 按 float 采样，
  不建 D24 图）。
- 只有 `R32_FLOAT` 的 `0x00` 档过、`0x28` 档挂 ⇒ 说明「可共享」但「不能同时挂 SRV+RTV」，
  路线 1 需要拆成「RTV 写入镜像 → 另一次拷成可采样资源」，代价与可行性**看日志再定**。
- `R16_FLOAT` / `R32_TYPELESS` 单独可用 ⇒ 作为内存/精度备选（1080p 全屏 R32F = 8.3 MB，R16F = 4.2 MB）。
- **源格式那格（`SHARED` 不带 `NTHANDLE`）若也建不出** ⇒ 定案「病因就是格式、与 MiscFlags 无关」，
  归因彻底闭环；若**建得出**（老式 handle OK）⇒ 说明 `NTHANDLE` 才是拦路的那一半，但
  VK 的 `D3D11_TEXTURE_BIT` 只认 NT handle，**这条对路线 1 无用**，仅补全归因。
- 全部 7 格 NT handle 都 FAIL ⇒ 路线 1 前置不成立，按 `docs/05` D2a-4 降级到路线 2。

**已知不拦判读**：R2 固定 2 项告警（深度镜像 FAIL + 归因格式 ×2，`0x80070057`、Format=44
= `R24G8_TYPELESS`）保持不变；`Format=44` 正是 §#13 探测的源格式那一格。

### 14.18.1 实跑记录（2026-10-06 14:02:51–14:04:27，`86aa133` = `v0.18.2` artifact，`run2c.ps1` → **[PASS] 全绿**）

**环境**：CI `86aa133` **RESULT SUCCESS**（`poc-presenter` + `capture-helper` 双 job 均 success；
前一个提交 `6ed52c7` 曾被 CI 抓出 `C2039 'GetSharedHandle' 不是 ID3D11Device 成员` —— 老式共享句柄是
`IDXGIResource::GetSharedHandle`，已改）；ini 五行全开；本局 1323 行 / 96 秒，
末条汇总 **帧=4980**（关停前最后一帧 5247，**≥2000 达标**）。

| 判据 | 实测 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.2: OK`（`ssr.shared`/`Step2c-β`/`ssr.sentinel`/`ssr.vkout` 四特征全 OK） | ✅ |
| #1 挂载 | `ctx槽33/50/47 已挂 = 1`（无 `挂载门未满足`） | ✅ |
| #2/#5 镜像+就绪 | 色324 SHARED OK=1、共享入向就绪=1 | ✅ |
| #6/#7 入向拷贝 | 46 行、`[2c读回` **9 行 9/9 ≠基线、`=基线` = 0** | ✅ |
| #8/#9 | `入向累计 = 4086`、`COPY= 末值 = 3`、`[2c入向已排队] = 65`、哨兵执行 0 | ✅ |
| #11 副作用 | `[异常] = 0`、PoC-B 失败行 0、`code=-4` 0；PoC-B **0.86~0.87 ms/帧**（Step1 基线 0.87） | ✅ |
| β2/β3 | VK 导入 OK=1、导入矩阵 1 组合成、交叉校验 **12 一致 / 0 不一致**、`不需要归因` | ✅ |
| β4 R2 归因 | 重试 2 行、`病因是格式 x2`、结论行 1 —— **本轮对这条做了归因更正，见下** | ⚠ 更正 |
| 12a–12c | `ini ssr.vkout=1` 1 行、`[2d] 出向镜像 OK` = 1、`[2d] 出向图就绪` = 1、**关闸行 0** | ✅ |
| 12d | `[2d] 回写#` **40 行，desc一致 40 / desc不一致 0** | ✅ |
| **12e（C-7 验收）** | `[2d] 出向读回` **9 行**（修复前恒 3 行，§14.18 预期 9~10）；**一致 4 / 不一致 5** | ✅ + 观察项 |
| 12f | `出向= 4084`、`帧= 4980` ⇒ **出向/帧 = 0.82**（0.5~1.5 内）；入向 4086 ⇒ 出向/入向 = **0.999** | ✅ |
| **#13 2d-3 探测** | **行数 = 7、结论行 = 1、NT-handle 可用格子 = 6** | ✅ |
| R2 告警 | 仍为固定 2 项（深度镜像 FAIL + 归因格式 ×2） | ~ 已知 |
| **总判** | **`[PASS] 全绿`**（`2c-alpha + 2c-beta 收口`） | ✅ |

**C-7 修复验证（本轮重点之二）**：`[2d] 出向读回` 由**恒 3 行 → 9 行**，且「每 600 次采一次」
（#600/#1200/#1800）**确实发生了** ⇒ 计数进门先 `++` 生效，C-7 关闭。

> **O-1 观察项（新暴露，不改判）**：9 行里 **一致 4 / 不一致 5**（#600、#1800 不一致，#1200 一致）。
> 修复前中后段帧**从未被采**，所以这个问题本轮才看得见。样本特征：`#600 出向=0x5E20817998A32B28`
> 正是同刻 `[2c] 入向拷贝#600` 的色校验和，而该行「入向」是另一个值 ⇒ 两份**来自不同时刻的采样**
> （帧错位/时序），与 `不一致✗ (帧错位/行距/时序)` 的自诊断一致。判据只要求 **≥1 一致** ⇒ 不拦 PASS；
> 是否为真问题（入向镜像在我 Map 读之前已被下一帧覆盖）待后续复核，不进 FAIL。

**帧时基线（§14.14 口径）**：

| 局 | 会话 CPU / GPU 均值 | 末 600 帧 CPU / GPU | 末窗 `>20ms` |
|---|---|---|---|
| `v0.18.0`（28.77 写/帧） | 17.14 / 17.16 ms | 18.09 / 17.81 ms | 52/600 |
| `v0.18.1`（0.68 写/帧） | 16.80 / 16.82 ms | 16.77 / 16.84 ms | 3/600 |
| **`v0.18.2`（+2d-3 探测 + C-7）** | **16.84 / 16.89** ms | **16.70 / 16.69** ms | **4/600** |
| Step1 基线 | — | 中位 16.65~16.66 | — |

⇒ 与 `v0.18.1` 持平（+0.02 / +0.07 ms），**末窗 GPU 还略好**，`>20ms` 4 vs 3 ⇒
**2d-3 探测（一次性建 7 张纹理）无性能回退**；PoC-B 回到 **0.86~0.87**（上一轮 0.92~0.95）。

**★ 2d-3 实测结论（本轮真正要拿的东西）** —— 七格明细（日志原文）：

```
#1 源格式(Format=44)      BindFlags=0x00000000 misc=SHARED          → 建=OK handle=老式OK(NT=FAIL 0x80070057)
#2 R32_FLOAT(41)          BindFlags=0x00000000 misc=SHARED|NTHANDLE → 建=OK handle=OK(NT)
#3 R32_FLOAT(41)          BindFlags=0x00000028 misc=SHARED|NTHANDLE → 建=OK handle=OK(NT)
#4 R16_FLOAT(54)          BindFlags=0x00000000 misc=SHARED|NTHANDLE → 建=OK handle=OK(NT)
#5 R16_FLOAT(54)          BindFlags=0x00000028 misc=SHARED|NTHANDLE → 建=OK handle=OK(NT)
#6 R32_TYPELESS(39)       BindFlags=0x00000000 misc=SHARED|NTHANDLE → 建=OK handle=OK(NT)
#7 R32_TYPELESS(39)       BindFlags=0x00000028 misc=SHARED|NTHANDLE → 建=OK handle=OK(NT)
```

1. **路线 1 前置完全成立**：3 个候选格式 × 2 档 `BindFlags` **6/6 拿到 NT handle** ⇒
   **首选 `R32_FLOAT × 0x28(SRV|RTV)`**（#3 格，正是路线 1 全屏 PS 要写的那档；`0x28` 这档通了
   意味着不必拆成「先写 RTV 再拷成可采样」）。`R16_FLOAT`（4.2 MB/全屏）与 `R32_TYPELESS` 作备选。
2. **★ 归因更正（修正 §14.12 / §14.17 沿用的措辞）**：第 1 格证明
   **`Format=44` × 单独 `SHARED`（不带 `NTHANDLE`）建得出来、老式 handle 也拿得到** ⇒
   2c-α 三次 `BindFlags` 全失败的真凶是 **`SHARED_NTHANDLE`**，不是「格式不在 D3D11 SHARED 白名单」。
   正确表述 = **病因是 `D24 家族 × SHARED_NTHANDLE` 这个组合**。
   当初矩阵只动 `BindFlags`、`MiscFlags` 恒为 `SHARED|NTHANDLE`，**少排除了一个轴** ——
   这正是 §14.18 特意加「源格式 × 单独 SHARED」那一格的用意，本轮闭环。
3. **对路线选择的影响：首选不变，但多出一条候选**：
   - 老式（KMT）handle 对**当前**导入路径无用（2c-β 已定案 `D3D11_TEXTURE_BIT` 只认 NT handle）⇒
     **路线 1 仍是首选**，第 2/3 条的结论不受影响；
   - **新增候选路线 1′（待探测）**：D24 走 legacy `SHARED` → `IDXGIResource::GetSharedHandle`
     （KMT handle）→ VK `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT` 直接导入，
     **可省掉每帧 1 次全屏 PS**。前置未知：① 驱动是否给该 handle type 配出 `VkImage`（D24/S8）；
     ② 跨 API 同步原语（NT 路径用的 fence 在 KMT 资源上是否等效）。**结论出来前不写死实现**。
4. **C-8（诊断输出，归 v0.18.3）**：结论行后半句「源格式 D24家族 **不可共享**」与第 1 格实测
   （`建=OK 老式OK`）**自相矛盾**，措辞应为「源格式不可 **NT** 共享」。本轮按七格明细读结论，
   **不影响判读**，但下次会误导。

**画面**：与 §14.15.1 / §14.17 相同 —— 水体消失 + 拖影（`vkout=1` 换掉 585 内容 = **预期，非回归**）。

### 14.19 v0.18.3 判读模板（2d-4 路线 1′ KMT 探测 + C-8 结论行 + O-1 差一帧 Flush，**2026-10-06 实跑 PASS → §14.19.1**）

**这一轮验什么**：`v0.18.3` 三处改动 —— 前两处**纯诊断、不改行为**，第三处是**本轮唯一的行为改动**，
除它以外其余判据必须与 §14.18.1 逐格一致（等价于一次回归对照）：

1. **2d-4 路线 1′ 探测** —— 深度 SHARED 失败的那条结论分支里，在 2d-3 后面再跑一次
   `ssrKmtProbe()`（`main.cpp`）+ `ssrKmtProbeVk()`（`vkrenderer.cpp`），回答 §14.18.1 第 3 条留下的两个未知：
   ① D24 源格式 × **单独 `SHARED`（不带 `NTHANDLE`）** 在 `BindFlags {0x00, 0x48}` 下建得出吗、拿得到老式（KMT）handle 吗；
   ② 拿到的老式 handle 能不能用 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT` 导成 `VkImage`
   （查支持度 + 实测「建图→导入→绑定」，建完立刻销毁）。
   **纯发现**：不写任何 `g_ssrIn*` / `g_ssrOut*` 状态 ⇒ 深度那格照旧 FAIL、2c/2d 全套判据**不该有任何变化**。
2. **C-8** —— `[2d-3] 格式探测 结论` 行后半句改为按**第 1 格实测**分支输出（第 1 格
   `建=OK 老式OK` 时不再写「源格式不可共享」，改写「可单独 SHARED、只是拿不到 NT handle ⇒ 病因是
   D24 家族 × SHARED_NTHANDLE 组合」）；只动措辞，`§#13` 行数/结论行判据不变。
3. **O-1（唯一行为改动）** —— `ssrInQueue` 的入向 `CopyResource` 之后加一次 `ctx->Flush()`，
   详见下方「O-1 复核」。

**跑法（与 §14.15.1 / §14.17 / §14.18 相同）**：ini 五行全开（`ssr=1` / `ssr.sentinel=0` /
`ssr.shared=1` / `probe=1` / `ssr.vkout=1`）→ 换 artifact DLL → 进存档实机跑图 **≥2000 帧** →
先自查日志含 `SSR侦察: ctx槽33/50/47 已挂`，再跑 `tools\run2c.ps1` 判读。

**判读表（`run2c.ps1` §#0~§#14 全绿 + 下列新增/改动判据）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.3: OK`（否则 DLL 没换） | — |
| §#13 2d-3 | 与 §14.18.1 同（行数 7、结论行 1、NT 可用 6），**只是结论行后半句措辞变了** | 回归对照 |
| **§#14 探测行数** | **= 4**（`#1/#2` D3D11 两档 `BindFlags` + `#3/#4` VK 支持度/实测；0 = 探测没跑 ⇒ WARN） | — |
| **§#14 老式 handle OK 格子数** | ≥1 ⇒ D24 单独 `SHARED` 可用（与 2d-3 第 1 格互证） | 按日志定 |
| **§#14 `bind=0 (0x00000000)`** | **≥1 ⇒ 路线 1′ 前置成立**（深度可原样直入 VK，省掉每帧全屏 PS）；=0 ⇒ 回到路线 1（`R32_FLOAT × 0x28`） | 按日志定 |
| §#14 结论行 | 有探测行时 = 1（0 ⇒ FAIL） | — |
| **§#12 12e（O-1 验收）** | `[2d] 出向读回` **不一致 = 0**（§14.18.1 是 5/9）；>0 ⇒ WARN「Flush 没把差一帧消干净」 | **本轮重点** |
| 12f / §#11 / 帧时基线 | 与 §14.18.1 同判据：`出向/帧` 0.5~1.5、`desc不一致 = 0`、`哨兵 = 0`、`[异常] = 0`；**帧时看 Flush 有没有把 5% 闸碰了**（v0.18.2 = 16.84/16.89、末窗 16.70/16.69） | 回归对照 |

**结论怎么读（本轮真正要拿的东西）**：

- **2d-4 `bind` 成功 ⇒ 走路线 1′**：深度镜像改用老式 `SHARED` 拿 KMT handle、VK 侧按
  `D3D11_TEXTURE_KMT_BIT` 直入，省掉路线 1 的每帧 1 次全屏 PS；落地前按 `docs/05` D2a-4 另开导入分支评估
  （含同步原语：KMT 资源上跨 API 等待是否等效于现在这条 fence）。
- **2d-4 `bind` 失败 ⇒ 回到路线 1**：`R32_FLOAT × 0x28` 全屏 PS 转深度（§14.18.1 已 6/6 验通），
  看 `#3/#4` 卡在哪一步再决定要不要补格式候选。
- **O-1 不一致归零 ⇒ 差一帧定案并关闭**；仍 >0 ⇒ Flush 不够，备选修法 = 用 `copyQ` 那套 EVENT 查询做跨 API 闸
  （PoC-B 已有范式，`pocbInject` 尾部）。
- **C-8**：结论行不再与第 1 格自相矛盾即为修好，无独立判据（随 §#13 一起看）。

**O-1 复核（本轮先给结论，再用实跑证伪/证实）**：

- **证据链（§14.18.1 的 9 行）**：① 头 3 行**全一致**，且这 3 帧恰好**每帧都有阻塞读回**
  （`2c#1~#3` + `2d#1~#3` 连着跑，`Map` 会把 D3D11 队列等完）；② 5 次不一致**全部**是
  「出向 = 2 帧前的入向」，恒差一帧，不是随机乱值（如 `#600 出向=0x5E20817998A32B28` =
  同刻 `[2c] 入向拷贝#600` 的色校验和）。
- **成因**：入向 `CopyResource` 是**异步排队**的，原来只有阻塞读回（前 3 + 每 600）才把队列等完；
  非读回帧走到 `pocbInject` 提交 VK 时，D3D11 那条拷贝可能还没进 GPU 队列 —— 而 2d-1 的设计口径正是
  「1 帧延迟**零跨 API 栅栏**」，两边没有顺序保证 ⇒ VK 读到**上一帧**那份入向，出向就差一帧。
- **影响面**：v0 passthrough 下 585 拿到的是「早一帧」的场景色，视觉上无感；真正受损的是
  **自校验判据**（本来该逐字节还原，却因竞态恒差一帧）。SSR v1 落地后同理（反射晚 1~2 帧，可接受，
  但要写进 D2a-4 的同步方案里）。
- **修法**：入向拷贝后 `ctx->Flush()`（**只提交不等待**，不引入 CPU 等 GPU），把「Map 才保证可见」
  这个隐含假设补成**每帧**成立；验收 = 12e 的不一致 5 → 0，代价看帧时基线是否劣化 5%。

### 14.19.1 实跑记录（2026-10-06 19:50:50–19:52:32，`79f21a1`（代码 sha `624c4d0`）= `v0.18.3` artifact，`run2c.ps1` → **[PASS] 全绿**）

**会话**：1 段（`sessions=1`）、`#0 banner expect v0.18.3: OK`、`入向拷贝#3456 帧=4653`、
`Present 计数 4801`（末行 `帧=5038`）⇒ **≥2000 帧要求过**。

| §14.19 判据 | 期望 | 实测 | 判 |
|---|---|---|---|
| §#14 探测行数 | = 4 | **4**（#1 `BindFlags=0x00`、#2 `0x48`、#3 支持度查询、#4 导入实测） | ✅ |
| §#14 老式 handle OK 格子数 | ≥1 | **2**（两档都 `建=OK 老式handle=OK`） | ✅ |
| **§#14 `bind=0`** | ≥1 ⇒ 路线 1′ 成立 | **1**：`D24/S8 alloc=0 (0x00000000) VK报size=8847360 bind=0 (0x00000000)` | ✅ **路线 1′ 定案** |
| §#14 支持度查询 | 有值即可 | `D24/S8 features=0x0005 可导入; D32S8 features=0x0005 可导入` | ✅ |
| §#14 结论行 | 有探测行时 = 1 | 1（`路线1' 前置成立 (老式 handle + VK 导入 + 绑 全过)`） | ✅ |
| §#13 回归对照 | 行数 7 / NT 6 / 结论行 1 | **7 / 6 / 1**，七格逐格同 §14.18.1 | ✅ |
| **§#12 12e（O-1 验收）** | 不一致 = 0（§14.18.1 = 5/9） | **一致 5 / 不一致 3（共 8 行）**，示例 `#600`、`#1200` 不一致、`#1800` 一致 | ⚠️ **未归零 → WARN** |
| 12f | `出向/帧` 0.5~1.5 且 ≈ 入向/帧 | 入向 3543 / 出向 3541 ⇒ 各 **0.747** | ✅ |
| §#11 / desc / 哨兵 | `[异常]=0`、`desc不一致=0`、`哨兵=0` | 0 / 0 / 0（回写 35 行 desc 全一致） | ✅ |
| **帧时基线** | 不得碰 5% 闸（v0.18.2 = 16.84/16.89、>20ms=4） | 末窗 CPU 均值 **16.75** 中位 **16.68** p95 17.19、GPU 16.65/16.64、**>20ms = 2**/600；会话 CPU n=4799 均值 16.85、GPU n=2399 均值 16.81 | ✅ **反而略好** |
| PoC-B 性能 | Step1 基线 0.87，5% 闸 | 累计均值 0.89~0.90 ms/帧（+2.3~3.4%） | ✅ |

`run2c.ps1` 自动结论：`[PASS] 全绿`，告警 3 项 —— ① #3 深度镜像 OK 行 ≠ 1（已知 = 格式）；
② #4 R2 风险点命中 1 次（已知 = 格式）；③ **12e 不一致 3 次**（O-1 的 `Flush` 没把差一帧消干净）。

**这一轮真正拿到的三件事**：

1. **2d-4 路线 1′ 前置成立 ⇒ 深度改道定案走路线 1′**：D24 源格式在两档 `BindFlags`
   （`0x00000000` / `0x00000048`，**都不带 `NTHANDLE`**）下都 `建=OK 老式handle=OK`；拿到的老式
   handle 经 `D3D11_TEXTURE_KMT_BIT` 走「建图 → 导入 → 绑定」`alloc=0 / bind=0` 全过（VK 报
   `size=8847360` = **8192B 行距 × 1080**（朴素 7680B×1080 = 8294400，VK 只是把行距对齐到 8192，1.067 倍，合理），支持度查询两格 `features=0x0005`
   （`DEDICATED_ONLY|IMPORTABLE`）⇒ 深度可**原样**进 VK，省掉路线 1 每帧 1 次全屏 PS。
   与 2d-3 第 1 格（`BindFlags=0x00000000 misc=SHARED → 建=OK 老式OK(NT=FAIL)`）互证：
   **病因钉死在 `SHARED_NTHANDLE` 这一轴，`SHARED` 单独用是通的**。
2. **C-8 修好**（无独立判据，随 §#13 一起看）：结论行后半句现在按第 1 格实测分支 ——
   「源格式 D24家族 可单独 SHARED 共享(老式 handle 拿得到)、只是拿不到 NT handle ⇒ 对当前走
   `D3D11_TEXTURE_BIT` 的导入路径无用, 病因是 D24 家族 × `SHARED_NTHANDLE` 组合」，
   与第 1 格不再自相矛盾。
3. **O-1 未归零**：12e 不一致 5/9 → **3/8**（55% → 37.5%），仍属「出向 = 上一帧入向」这类帧错位
   ⇒ `ctx->Flush()` **只提交不等 GPU**，在「零跨 API 栅栏」设计下仍不构成两边的顺序保证。
   备选修法（§14.19「结论怎么读」第 3 条）继续有效：用 `copyQ` 那套 **EVENT 查询**做跨 API 闸，
   `pocbInject` 尾部已有范式。v0 passthrough 下 585 拿到的是「早一帧」的场景色，视觉无感 ⇒
   **不阻塞** SSR v1，但必须写进 `docs/05` D2a-4 的同步方案。

**下一步**：按第 1 条走**路线 1′** —— `docs/05` D2a-4 另开导入分支评估（含 KMT 资源跨 API 等待
是否等效现有 fence），随后 SSR v1 shader 采样；O-1 的 EVENT 闸、§14.14 日志约束下的 S4/S5
帧时基线采样与 R3 5% 闸一并排入该批。

### 14.20 v0.18.4 判读模板（O-1 正式修法：2c 入向 EVENT 闸，**实跑 PASS** → §14.20.1）

**这一轮只动一处行为**（选 B：先把同步原语钉死，再动 shader），其余全部当回归对照。
背景与证据见 §14.19 末「O-1 复核」+ §14.19.1 第 3 条：`ctx->Flush()` 只提交不等待，
12e 不一致 5/9 → 3/8 **仍未归零** ⇒ 备选的 `copyQ` 式 EVENT 闸转正。

**代码在哪**（与 PoC-B `copyQ` 同款范式，只是管的对象不同）：

1. `main.cpp` `ssrInQueue`：两条 `CopyResource` 之后 —— 惰性建 `D3D11_QUERY_EVENT`（`g_ssrInQ`，
   建不出只打一次日志并降级为无闸）→ `End(g_ssrInQ)` → `Flush`（查询连同拷贝一起进驱动队列）→
   置 `g_ssrInQLive`。上一条结果还没被消费则**不再 End**（免得 debug layer 报错）。
2. `vkrenderer.cpp` `ssrInGateWait(PocbCtx&)`，在 `pocbInject` 里 **`ssrInVkFrame` / `ssrOutVkFrame`
   之前**调用 —— 一次等待同时覆盖 2c-β 与 2d 出向两条读路径：`GetData(..., D3D11_ASYNC_GETDATA_DONOTFLUSH)`
   自旋 + `Sleep(0)` + **2000ms 超时放行**（绝不卡死帧），超时/等到都消费 `g_ssrInQLive`。
   VK 没起来（`c.state != 2`）时只消费不等待。
3. 日志：`SSR侦察: [2c] 入向EVENT闸#K 等待=… 累计均值=… 帧=…`（前 8 条 + 每 128 条），
   超时另打 `入向EVENT闸 等待超 2000ms`。

**跑法**：与 §14.19 完全相同 —— ini 五行全开、换 artifact DLL、实机 ≥2000 帧、
先自查 `SSR侦察: ctx槽33/50/47 已挂`，再跑 `tools\run2c.ps1`。

**判读表（`run2c.ps1` §#0~§#14 + 下列判据）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.4: OK`（否则 DLL 没换） | — |
| **§#12 入向EVENT闸 行数** | **≥1**（前 8 条 + 每 128 条 ⇒ 实机几十行）；**0 ⇒ WARN 闸没跑** | 本轮新增 |
| **§#12 超时放行** | **= 0**（>0 ⇒ 拷贝 2s 没跑完，属异常，看帧时） | — |
| **§#12 12e 不一致** | **= 0**（v0.18.3 = 3/8；v0.18.2 = 5/9）>0 ⇒ WARN 并看闸行等待值 | **本轮核心** |
| §#12 12e 一致 | = 行数 8（前 3 + 每 600） | — |
| §#12 12f / desc / 哨兵 / §#11 | 同 §14.19.1：`出向/帧` ≈1、`desc不一致 = 0`、`哨兵 = 0`、`[异常] = 0` | 回归对照 |
| §#13 2d-3 / §#14 2d-4 | 与 §14.19.1 逐格同（7/6/1、行数 4、老式 OK 2、`bind=0` ×1、结论 1） | 回归对照 |
| 帧时基线 | v0.18.3 = 末窗 CPU/GPU 16.75/16.68、`>20ms=2`、会话 16.85/16.81 ⇒ **5% 闸按 16.85 算** | 回归对照 |
| PoC-B 性能 | 基线 0.87（v0.18.3 实测 0.89~0.90）⇒ 5% 闸 | 回归对照 |

**结论怎么读（这一轮要拿的东西）**：

- **12e 归零 ⇒ 差一帧定案关闭、EVENT 闸可用** → 同步原语就绪，进 **SSR v1 shader 采样**
  （路线 1′ KMT 直入，`docs/05` D2a-4）。
- **12e 仍 >0** ⇒ 先看闸行：`等待=` 恒 0.000ms 说明查询没真正跑到（`End` 没执行 / 消费错位）；
  若有超时行说明拷贝被饿着；都排除就要找**还有哪个读点没过闸**（2c-β 的读回、2d 出向、
  或游戏自己在同一镜像上的读写）。
- **闸等待均值应 ≈0**（拷贝在特征B、读在 Present，中间隔了半帧 GPU 时间）。
  若常 >1ms ⇒ 拷贝经常拖到 Present 才跑完，闸的等待要**单列**进 R3 5% 闸（不能只看总帧时）。

#### 14.20.1 实跑记录（2026-10-06，第五轮真机回归 [PASS] 全绿）

`02c10b8`（CI 双 job SUCCESS），实机 **5220 帧 / Present 5401**、`sessions=1`、
`expect v0.18.4: OK`、`ctx槽33/50/47 已挂 = 1`。自动结论 **[PASS] 全绿**，
告警仍只有已知的 2 项 R2 深度格式（色过/深度不过、病因=格式）。

| 判据 | 期望 | 实测 | 判 |
|---|---|---|---|
| **§#12 12e 不一致** | **= 0** | **行数 9 / 一致 9 / 不一致 0** | ✅ **归零** |
| §#12 入向EVENT闸 行数 | ≥1 | **41**（前 8 + 每 128） | ✅ |
| §#12 超时放行 | = 0 | **0**（最大单次 3.264ms，远未到 2000ms） | ✅ |
| 闸等待累计均值 | ≈0；**>1ms ⇒ 单列 R3** | **1.783ms**（尾段稳定 1.7~1.9ms） | ⚠ **单列登记，见下** |
| §#13 / §#14 | 与 §14.19.1 逐格同 | 7/6/1；4 行 / 老式 OK 2 / `bind=0` / `features=0x0005`×2 / 结论 1 | ✅ |
| 12f 出向/帧 | ≈1（±20%） | 0.77（同 v0.18.3 的 0.747；分母含 arm 前帧） | ✅ |
| desc不一致 / `[异常]` / `哨兵` | 全 0 | 0 / 0 / 0 | ✅ |
| 2c-β 交叉校验 | 全一致 | 9 行：一致 9 / 不一致 0 | ✅ |
| 帧时末窗 CPU/GPU 中位 | 5% 闸 | 16.78 / 16.67（v0.18.3 = 16.75 / 16.68） | ✅ |
| 会话 CPU/GPU 均值 | 5% 闸 | **16.85 / 16.84**（v0.18.3 = 16.85 / 16.81，**+0.18%**） | ✅ |
| `>20ms` | 量级不变 | 3/600（v0.18.3 = 2） | ✅ |
| PoC-B 累计均值 | 基线 0.87 × 1.05 | 0.88~0.90（**+3.4%**） | ✅ |

**这一轮真正拿到的三件事**：

1. **O-1 关闭**：12e 不一致 **v0.18.2 = 5/9 → v0.18.3（只 Flush）= 3/8 → v0.18.4（EVENT 闸）= 0/9**，
   「零跨 API 栅栏下 D3D11 拷贝没跑完 VK 就读」的**归因与修法双向坐实**；同轮 2c-β 9/9 全一致
   ⇒ 入向、出向两条通路的同步原语**就绪**。
2. **闸的代价摸清**：等待累计均值 **1.78ms**、单次 1.5~3.3ms —— 说明特征B 的拷贝**经常拖到
   Present 才跑完**（本节预设的分支命中）。帧时**无回退**（会话 CPU 持平 16.85、GPU +0.18%）
   ⇒ 代价真实存在但被帧预算吸收；按本节约定 **把「EVENT 闸等待均值 1.78ms」单列登记为
   R3 基线观测项**，SSR v1 shader 落地后须与本轮同口径对比（**不能只看总帧时**）。
3. **回归面全绿**：§#13/§#14 与 §14.19.1 逐格同（探测类零行为变化）、PoC-B +3.4%、
   `[异常]=0`、`哨兵=0`、`desc不一致=0` ⇒ 闸**只改「读镜像的时机」，不碰任何数据通路**。

**下一步**：同步原语已钉死 → 按 `docs/05` D2a-4 落地**路线 1′ KMT 导入分支**，
随后 **SSR v1 shader 采样**（descriptor/sampler 函数表已备、`POCB_DEV_FNS` 含
`vkFreeDescriptorSets`）。

### 14.21 v0.18.5 判读模板（路线 1′ 深度 KMT 导入分支落地，**实跑 PASS** → §14.21.1）

**这一轮把 2d-4 的一次性探测转成持久导入分支**（代码落地与同步原语评估结论见
`docs/05` D2a-4 末「路线 1′ 落地」块）；色通路 / 2c-β / 2d 出向 / 两个探测**全部当回归对照**。

**代码在哪**：

1. `main.cpp` `ssrInMakeShared`：NTHANDLE × 三档全败后，**深度源**追加
   `D3D11_RESOURCE_MISC_SHARED` 回退（两档 BindFlags = `0x48` / `0x00`，即 2d-4 实测过的两格），
   handle 走 `IDXGIResource::GetSharedHandle`（**不 `CloseHandle`**）；成功打
   `[2c] 深度520 SHARED 镜像 OK … 老式SHARED/KMT`，就绪行 = `深度520=OK(路线1′ 老式SHARED→KMT)`。
2. `vkrenderer.cpp` `ssrKmtVkBuild` / `ssrKmtVkFrame` / `ssrKmtReset`：KMT handleType 持久导入
   （**dedicated 分配**，因 `features=0x0005` = `DEDICATED_ONLY|IMPORTABLE`）+ `DEPTH|STENCIL` 视图
   + 持久映射读回 + 布局初转；`pocbInject` 在 `ssrOutVkFrame(c)` **之后**调用，**EVENT 闸在前面已等完**。
3. 节奏：`ssrKmtVkFrame` **完全跟随 D3D11 侧深度读回节奏**（一次性 `g_ssrInChkDValid` 消费，
   前 3 次 + 每 600 次）；**建图帧只建不比**（初转按规范可能丢内容）⇒ 首行对比出现在第 2 次机会。
4. 三档候选：`ssrFnvSampleAdv` (stride,nB) 参数化 —— VK 侧 `4B 全量` / `4B 跳 stencil` /
   `3B 紧密排布`，D3D11 侧 `ssrInFnv` 的 `alt3` 出参给低 24 位口径。

**跑法**：与 §14.20 完全相同 —— ini 五行全开、换 artifact DLL、实机 ≥2000 帧、
先自查 `SSR侦察: ctx槽33/50/47 已挂`，再跑 `tools\run2c.ps1`。

**判读表（`run2c.ps1` §#0~§#15 + 下列判据）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.5: OK`（否则 DLL 没换） | — |
| **§#15 `[2c]` 深度镜像 OK 且标路线1′** | **= 1**（NTHANDLE 走不通 ⇒ 必须改道成功） | **本轮核心** |
| **§#15 `[2d-5] 深度KMT 导入OK`** | **= 1**、`导入失败 = 0` | **本轮核心** |
| **§#15 深度KMT读回 行数** | **≥1**（节奏 = `[2c读回 深=]` 前 3 + 每 600）；**0 ⇒ WARN**（读回失能/没出深读回） | **本轮核心** |
| §#15 三档判定 | **一致 / 低24位一致 / 紧密3字节一致 合计 ≥1 且 不一致 = 0**；三档全不一致 ⇒ **WARN 归因待定（不 FAIL）** | **同步证据** |
| §#15 老式SHARED 重试行 | 1..2（`0x48` / `0x00` 两档）；0 ⇒ 没进改道 | 对照 |
| §#13 2d-3 / §#14 2d-4 | 与 §14.19.1 逐格同（**探测仍跑 ⇒ 7/6/1、4/2/1 不掉**） | 回归对照 |
| §#12 12e 不一致 / 闸行 / 超时 | 仍 = 0 / ≥1 / = 0（闸与本轮改动正交） | 回归对照 |
| 2c-β / desc / 哨兵 / `[异常]` | 全绿同 §14.20.1 | 回归对照 |
| 帧时基线 | 5% 闸按 **16.85** 算（CPU 会话均值）；GPU 同理 | 回归对照 |
| PoC-B 性能 | 基线 0.87 × 1.05（v0.18.4 实测 0.88~0.90） | 回归对照 |
| 闸等待均值 | 仍单列 R3 观测（v0.18.4 = 1.78ms） | R3 观测 |

**结论怎么读（这一轮要拿的东西）**：

- **导入OK + 至少一次三档命中** ⇒ 路线 1′ 通路成立、**EVENT 闸对 KMT 同样成立**
  （D2a-4 问题②的答案）⇒ 深度原样直入 VK，**省掉路线 1 那每帧一次全屏 PS**，
  进 **SSR v1 shader 采样**（`DEPTH|STENCIL` 视图已备）。
- **三档全不一致** ⇒ 只是归因未明：看该行 `D3D全量/低3/VK 三值` 判断是
  「VK depth aspect 排布又一种」还是「读到别的帧」（若 `不一致` 且 12e 也一致，
  基本排除时序，剩下就是排布 ⇒ 加第四档候选）。
- **`导入失败` 或没走改道** ⇒ 路线 1′ 降级，**色通路/2c/2d 一行不受影响**（按设计只关自己），
  兜底走路线 1（`R32_FLOAT × 0x28` 全屏 PS，前置已由 2d-3 6/6 验通）。

#### 14.21.1 实跑记录（2026-10-06，第六轮真机回归 [PASS] 全绿）

`882903b`（CI 双 job SUCCESS），实机 **帧=4440（摘要行）/ 入向 3799**、`sessions=1`、
`expect v0.18.5: OK`、`ctx槽33/50/47 已挂 = 1`。自动结论 **[PASS] 全绿**，
告警只剩已知的 1 项 R2（文案已按「深度已改道」分支改写，属预期）。

| 判据 | 期望 | 实测 | 判 |
|---|---|---|---|
| **§#15 深度镜像标路线1′** | = 1 | **1**（`… BindFlags=0x00000048 … [路线1′ 老式SHARED/KMT]`） | ✅ **核心** |
| §#15 老式SHARED 重试行 | 1..2 | **1**（`BindFlags=0x48` **首档即成**，`0x00` 兜底档没用上） | ✅ |
| **§#15 KMT 导入OK / 失败** | 1 / 0 | **1 / 0**：`usage=TRANS_SRC\|SAMPLED alloc=dedicated 内存类型#1 view=OK 读回=OK` | ✅ **核心** |
| **§#15 深度KMT读回 行数** | ≥1 | **8**（节奏 = `[2c读回 深=]` 前3+每600；首行对比出在帧 644，即建图帧之后那一次机会） | ✅ **核心** |
| **§#15 三档判定** | 合计 ≥1 且 不一致 0 | **全量4B一致 = 8**、低24位 0、紧密3B 0、**不一致 0** —— tier-1 命中，连 stencil 字节都逐字节相同 | ✅ **同步证据** |
| 深度读回 `提交+等fence` | （无既定闸） | **30.23~41.72ms（8 次，均值 ≈37ms）** | ⚠ **新观测项，见下** |
| §#13 2d-3 | 7/6/1 | **7 / 6 / 1**（探测照跑，判据不掉） | ✅ 回归 |
| §#14 2d-4 | 4 行 / 老式 2 / `bind=0` | **4 / 2 / 1**（`features=0x0005`×2、结论 1） | ✅ 回归 |
| §#12 12e 出向读回 | 一致 ≥1、不一致 0 | **7 行：一致 7 / 不一致 0** | ✅ 回归 |
| §#12 EVENT 闸 | ≥1、超时 0 | **37 行 / 超时 0**；**累计均值 1.420ms**（v0.18.4 = 1.783ms，**-20%**，仍 >1ms ⇒ 继续单列 R3） | ✅ 回归 |
| 2c-β 交叉校验 | 全一致 | **8 行：一致 8 / 不一致 0**（`一致=15` 是 `CntBoth` 重复计数口径） | ✅ 回归 |
| `desc不一致` / `[异常]` / `哨兵` / `COPY` | 0 / 0 / 0 / 3 | 0 / 0 / 0 / 3 | ✅ 回归 |
| `[2d] 回写` / 出向帧率 | desc 不一致 0、出向/帧 ≈1 | 28 行 desc一致 28；出向/帧 **0.6**（v0.18.4 = 0.77，C-6 修前 28.7；分母含 arm 前帧） | ✅ 回归 |
| 帧时**末窗中位** CPU/GPU | 5% 闸 | **16.66 / 16.70**（v0.18.4 = 16.78 / 16.67 ⇒ **-0.7% / +0.2%**）；末窗均值 16.76 / 16.88，`>20ms=3/600`（v0.18.4 = 3） | ✅ 无回退 |
| 会话均值 CPU/GPU | 5% 闸 | **16.95 / 16.93**（v0.18.4 = 16.85 / 16.84 ⇒ **+0.6% / +0.5%**） | ✅ |
| PoC-B 累计均值 | 基线 0.87 × 1.05 | **0.93（1800帧）→ 0.88（4200帧末值）** | ✅ |
| 首窗 `>20ms` | 含加载尖峰 | 10/599（`最大 318.40ms` = 进场景加载，非本轮改动；稳态各窗 3/3/5/3/3） | ⚠ 已知 |

**这一轮真正拿到的三件事**：

1. **路线 1′ 通路成立、问题② 定案**：D24 深度 → 单独 `SHARED` + `GetSharedHandle` 老式句柄 →
   VK `D3D11_TEXTURE_KMT_BIT` 直入（**dedicated 分配命中**，`view=OK`、首选 usage 首档即成），
   跨 API 读回 **8/8 逐字节一致**（tier-1 全量 4B，stencil 字节也一致）⇒
   **EVENT 闸与 handleType 无关、KMT 分支继承同款闸即够用**；老式 handle 只在建图时导入一次，
   之后每帧「D3D11 写同一块物理页 → 闸等到写完 → VK 读」。**深度原样直入 VK，
   路线 1 那每帧一次全屏 PS 不需要了**（R32_FLOAT × 0x28 只剩兜底）。
2. **代价摸清**：深度读回 `提交+等fence` **37ms 量级**（2c-β 同帧色读回 11ms、2d 出向 0.4ms），
   归因是跨 API 共享硬件队列上我的 copy 排在游戏本帧命令之后（2c-β wait 期间 D3D11 又提交了新批）；
   但它只发生在 **8 个节流帧**（前3 + 每600）⇒ 折合 **0.065ms/帧**，稳态 `>20ms` 窗计数
   **3/600 与 v0.18.4 持平**、末窗中位反而略降 ⇒ **无回退**。按本节约定把
   **「深度读回 fence 30~42ms（8 次/局）」新增登记为 R3 观测项**（与 EVENT 闸 1.420ms 并列跟踪）。
3. **回归面全绿**：§#13/§#14 探测判据逐格不掉（改道不吞探测）、2c-β 8/8、12e 7/7、
   `desc/哨兵/异常` 全 0、PoC-B 0.88、帧时中位 16.66/16.70 ≈ v0.18.4 的 16.78/16.67
   ⇒ 本轮**唯一行为改动只增加了 8 次节流读回**，其余全是回归对照。

**画面（用户目视回报，与 §14.15.1 / §14.17 / §14.18.1 相同）**：**水体几乎完全透明 + 严重拖影**
—— `ssr.vkout=1` 的 v0 passthrough「换掉 585 内容 = **预期，非回归**」（`324` 快照在段16 之前不含水
⇒ 段17 读到没画水的场景 ⇒ 水消失；回写的是**上一帧**场景色 ⇒ 移动时拖影）；`ssr.sentinel=0` 故 2b
回写未开。**该观感由 SSR v1 shader 出真反射后自然消失**，在那之前不作判据。

**下一步**：进 **SSR v1 shader 采样**（`DEPTH|STENCIL` 视图已备、`POCB_DEV_FNS` 的
descriptor/sampler 函数表已备、`ssrKmtVkFrame` 的节流读回在 shader 上线后转为回归对照）。

---

### 14.22 v0.18.6 判读模板（SSR v1 shader 采样落地，**已实跑 → §14.22.1**）

**这一轮把 2d 出向的「入向整幅原样拷」换成一次全屏三角 render pass**（设计定案见
`docs/05` D4 末「✅ 落地」块 + R4 行）；入向 2c / 2c-β / 2d 通路 / 深度 KMT §#15
**全部当回归对照**（v1 关着时行为与 v0.18.5 逐字节相同）。

**代码在哪**：

1. `shaders/ssr.vert` + `ssr.frag`（新）：无顶点缓冲全屏三角；`viewZ()` 由深度反推视图 Z
   （D3D 正投影 近→0 / 远→1，`ssr.rev=1` 先翻回标准口径）→ `viewPos()` / `projectUV()`，
   逐像素差分还原法线、线性 ray march + 末段 3 次二分收紧、未命中用射线最后一个在屏点做
   屏幕边缘延展；Schlick fresnel(F0=0.02) × `ssr.strength` × 4 合成、未命中再 ×0.5；
   `ssr.mode=0` 或深度越界 = 纯透传。`tools/make_shaders.ps1` 扩成 4 对
   （`pocb.vert/frag` + `ssr.vert/frag` → `kPocbVertSpv`/`kPocbFragSpv`/`kSsrVertSpv`/`kSsrFragSpv`）。
2. `vkrenderer.cpp`：`ssrV1Build`（render pass / framebuffer / 色 view / **只取 depth aspect**
   的深度 view / 色 LINEAR + 深 NEAREST 两个 sampler / 2 binding 描述符布局+pool+set /
   push constant 48B 的图形管线；幂等、换设备只丢不毁）+ `ssrV1RecordRender`（色深
   `GENERAL ↔ SHADER_READ_ONLY`、出向 `UNDEFINED → GENERAL`、**深度布局迁移带 `DEPTH|STENCIL`
   两位**）+ `ssrV1RecordCopy`（= v0.18.5 的原样拷）+ `ssrV1RecordOut`（唯一录制入口）+
   `ssrV1Dirty`/`ssrV1Sig`（依赖签名变才重录，不重建资源）。
3. `main.cpp`：`iniNum()` 解析器 + `ssr.v1` / `ssr.mode` / `ssr.fov` / `ssr.near` / `ssr.far` /
   `ssr.steps` / `ssr.dist` / `ssr.strength` / `ssr.rev` 读取与合法性回退；banner = `v0.18.6`。
4. 入向图 usage += `SAMPLED`、出向图 usage += `COLOR_ATTACHMENT`（**两者建不出都回退原 usage
   并只关 v1**）；深度 KMT 图由 v1 提前触发 `ssrKmtVkBuild`，不再等 D3D11 节流读回。

**跑法（与 §14.20 相同 + ini 追加）**：五行全开之外**追加 `ssr.v1=1`**（可选 `ssr.mode=1`、
`ssr.fov=65`、`ssr.near=10`、`ssr.far=100000`、`ssr.steps=32`、`ssr.dist=500`、
`ssr.strength=1`、`ssr.rev=0`），换 artifact DLL，实机 ≥2000 帧，先自查
`SSR侦察: ctx槽33/50/47 已挂`，再跑 `tools\run2c.ps1`。**ini 启动只读一次 ⇒ 改完必须重启游戏。**

**判读表（`run2c.ps1` §#0~§#16 + 下列判据）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.6: OK`（否则 DLL 没换） | — |
| **§#16 `[v1] SSR v1 就绪`** | **= 1**（>1 = 资源被反复重建 ⇒ WARN 看有无换设备/换分辨率行） | **本轮核心** |
| **§#16 `[v1] 渲染#` 行数** | **> 0**（前8 + 每128 节流）；有渲染行却没就绪行 = **FAIL** | **本轮核心** |
| §#16 降级行（`→ v1 关自己` / `深度KMT 图还没就绪` / 出向无 `COLOR_ATTACHMENT` / 入向无 `SAMPLED`） | 全 0 最好；任一 >0 ⇒ **WARN + 按行文归因**，出向已退回 2d 纯拷贝、**只关自己** | 降级面 |
| §#12 `[2d] VK出向#` | 仍 >0（**两种模式都照打**，12e/12f 判据不断档），文案 = `全屏三角→出向` | 回归对照 |
| §#15 深度 KMT（§14.21.1 逐格） | 导入OK=1、失败=0、读回 ≥1、不一致 0 | 回归对照（= v1 的深度输入） |
| §#13 / §#14 / 12e / 2c-β / desc / 哨兵 / `[异常]` | 与 §14.21.1 同（全 0 / 全绿） | 回归对照 |
| 帧时基线 | 5% 闸按 **16.85** 算（CPU 会话均值）；`[v1]` 的 fence 与 PoC-B **分列**看 | 回归对照 |
| PoC-B 性能 | 基线 0.87 × 1.05（v0.18.5 实测 0.88~0.93） | 回归对照 |

**画面（这一轮的真正判据 = 人眼，无自动判据）**：

- **预期**：水体不再「几乎透明 + 拖影」，取而代之是**屏幕空间倒影**——命中时倒影内容来自本帧
  屏内其它位置的场景色，未命中时是屏幕边缘延展且强度再 ×0.5（一眼能看出「没打中」）。
  **1 帧延迟仍在**（回写的是上一帧场景色）⇒ 移动时倒影轻微滞后属预期，不算缺陷。
- **观感对不上怎么调（R4 = 反推 inv(投影)，全 ini 可校，不必改代码）**：
  - **倒影位置/比例整体偏** ⇒ 调 `ssr.fov`（默认 65 = Skyrim 默认垂直 FOV；装了改 FOV 的 mod
    就填那个值）。
  - **倒影远近/延伸长度不对** ⇒ 调 `ssr.dist`（**以 near 为单位**，默认 = 500 × near）与
    `ssr.steps`（默认 32；步太粗会跳过薄几何）。
  - **倒影明显错乱/远近反了** ⇒ 试 `ssr.rev=1`（游戏用反向深度时的开关）。
  - **倒影太淡** ⇒ 调 `ssr.strength`（0..4，乘在 fresnel 上）。
  - `ssr.near` / `ssr.far` 一般**不用动**：`p1.w = dist × near` 的写法已把 near 的尺度自由度消掉，
    far 只在 near/far 比值 < ~100 时才有可见影响；真要动就看日志里回退提示（非法自动回 10 / 100000）。
- **完全没变化** ⇒ 看 §#16 降级行（深度没就绪 / usage 退回 / `ssr.v1=1 但 ssr/ssr.shared/ssr.vkout
  有没开的`），或 **ini 没重启**（只读一次）。
- **画面比 v0.18.5 还差（花屏/整屏错乱/DEVICE_LOST）** ⇒ **不是本轮该有的行为**：先把
  `ssr.v1=0` 退回 2d 纯拷贝确认通路，再按 `[v1]` 日志归因（只关自己，`ssr=0` / `ssr.vkout=0`
  是总逃生门）。**同一块导入内存绑两张 VkImage = DEVICE_LOST（v0.16.4 教训）**，v1 只给现有
  图换 usage、没有新建并存图。

---

#### 14.22.1 实跑记录（2026-10-07，`v0.18.6` artifact，**通路全绿、画面两个问题 → v0.18.7 A/B 双修**）

**通路**：`run2c.ps1` 按 §14.22 判读表逐条对照全绿 —— `[v1] SSR v1 就绪 = 1`、`[v1] 渲染# > 0`、
§#12 `[2d] VK出向#` >0、§#15 深度 KMT 全绿、§#13/§#14/12e/2c-β/desc/哨兵/`[异常]` 全 0、帧时在
5% 闸（16.85）内。**⇒ 通路没坏，坏的是画面**（SSR 真的写进 585 并被段17 消费了，这是 Step 2d/v1
一路要验的硬约束，本次再次验通）。

**目视回报（两条，都不是通路问题）**：

| # | 现象 | 归因（本轮查清） | v0.18.7 对策 |
|---|---|---|---|
| ① | **倒影完全破碎**（一屏碎渣，看不出连续倒影） | 法线是**逐像素**中心差分还原的，而**水面像素的深度其实是水底/河床**（水体不写深度）⇒ 差分得到的是**碎石法线** ⇒ 每像素反射方向各不相同 ⇒ 破碎；次要 = 深度跳变处的命中抖动 | **A**：`ssr.smooth`（差分邻域半径，默认 4px）把邻域压成整体坡度 + `ssr.blur`（5-tap 十字平滑）兜住命中抖动 |
| ② | **水面除去倒影依然几乎透明无色** | 合成底 base = **324 快照 = 段16 之前**（那一刻水还没画），fresnel 正对相机时 `wgt≈0.08` ⇒ **92% 的合成结果来自「没水的画面」**；585 被换成这张底色后，段17 水体 PS 拿它当反射做自己的合成 ⇒ 水的可见贡献被换成背景 ⇒ 水无色。§14.21.1 当初「出真反射后自然消失」是**误判** | **B**：特征B 处在 2d 出向回写**之前**抢一份 **585（段16 刚画完）** 当合成底色 ⇒ 底色里有段16 写进去的反射本该有的内容；建不出 → 退回 324（= v0.18.6 行为，只关自己） |

**时序**：底色镜像 = **本帧段16 之后的 585**，与 324 同帧 ⇒ base/refl 协调；显示仍是 1 帧延迟
（出向镜像下帧才拷进 585）⇒ 移动时倒影/水色轻微滞后属预期。

**下一轮 = `v0.18.7`（A 平滑批 + B 底色镜像，一次跑图同时验收）→ §14.23。**

---

### 14.23 v0.18.7 判读模板（A 法线平滑批 + B 底色镜像，**代码批待 CI / 待实跑**）

**一句话**：A 修「倒影破碎」（`ssr.smooth` / `ssr.blur` / `ssr.debug`），B 修「水无色」
（`ssr.base585` 第 4 张 SHARED 镜像当合成底色）。入向 2c / 2c-β / 2d / 深度 KMT §#15 /
12e / desc / 哨兵 **全当回归对照**（`ssr.smooth=1 / ssr.blur=0 / ssr.base585=0` 时行为应与
v0.18.6 逐字节相同）。

**代码在哪**：

1. `shaders/ssr.frag`（重写）：
   - **A**：`PAt()` 改**中心差分 + 邻域半径 `ssr.smooth`**（clamp 1..16，叉积退化时兜
     `vec3(0,0,1)`）；命中色 `ssr.blur=1` 时做 5-tap 十字平滑（半径同 `ssr.smooth`）；
     `ssr.debug` 三分支绕过合成（1 法线 / 2 命中红绿 / 3 深度灰度）。
   - **B**：描述符第 3 个 binding `uBase`（585 底色），`baseRGB = p3.w>0 ? uBase : uColor`；
     `ssr.base585` 关着 / 建不出 ⇒ `p3.w=0` ⇒ 退回 v0.18.6 的 `uColor` 底色。
   - push constant **48B → 64B**（4×vec4，`p3 = (smooth, blur, debug, useBase)`）。
2. `main.cpp`：`ssrMakeSharedTo()`（第 4 张 `SHARED|NTHANDLE` 镜像，判读串
   `[base] 底色#` 与 `[2d] 出向镜像 OK` 同款口径）；`ssrInBuild` 内建 `g_ssrBaseTex`
   （源优先 `g_ssrStrRes` 否则 `g_ssrSceneRes`）；特征B 抢拷块插在 **2d 出向回写之前**，
   门 = `g_ssrInPending` + 身份三重判据 + `copyResDescChecked`；ini 读
   `ssr.base585` / `ssr.smooth` / `ssr.blur` / `ssr.debug` + 合法性回退；banner = **`v0.18.7`**。
3. `vkrenderer.cpp`：`ssrBaseVkBuild()`（底色 VkImage + 外部内存导入 + UNDEFINED→GENERAL
   初转，独立 `g_ssrVkCmdB` 提交 + fence 等待，**换设备整套只丢不毁**）+ `ssrOutVkFrame`
   顶部幂等调用；`ssrV1Sig` 吸收底色图 handle 与 3 个调参 ⇒ 变了自动重录；`ssrV1Build`
   建 `g_ssrV1ViewB` + 描述符 3 binding（**没底色时 binding2 填 viewC**，靠 `p3.w` 告诉
   shader 采不采，避免空 view）+ `pcr.size=64`；`ssrV1RecordRender` 加底色进/出 barrier
   （成对 `GENERAL ↔ SHADER_READ`）与 `p3` 压栈；`ssrV1DropViewB` 清理。
4. `tools/make_shaders.ps1` 已跑通：`ssr.frag.spv = 14732 B`（原 11044），`pocb_shaders.h`
   588 行入库；`tools/check1.ps1` → **RESULT OK**。

**跑法（与 §14.22 相同 + ini 追加 4 键）**：

| 键 | 默认 | 含义 |
|---|---|---|
| `ssr.smooth` | `4` | 法线差分邻域半径 px（1..16）；**破碎的第一开关** |
| `ssr.blur` | `1` | 反射色 5-tap 空间平滑 0/1；只兜命中抖动，对破碎主因无效 |
| `ssr.debug` | `0` | 0 正常 / 1 法线 / 2 命中（红未命中·绿命中）/ 3 深度灰度 |
| `ssr.base585` | `1` | 用段16 后的 585 当合成底色（B）；0 = v0.18.6 行为 |

其余沿用 §14.22：五行全开 + `ssr.v1=1` + `ssr.mode=1 / ssr.fov=65 / ssr.near=10 /
ssr.far=100000 / ssr.steps=32 / ssr.dist=500 / ssr.strength=1 / ssr.rev=0`。换 artifact DLL →
实机 **≥2000 帧** → 先自查 `SSR侦察: ctx槽33/50/47 已挂` → `tools\run2c.ps1`。
**ini 启动只读一次 ⇒ 改完必须重启游戏。**

**判读表（`run2c.ps1` §#0~§#16 + 下列判据）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.7: OK`（否则 DLL 没换） | — |
| **§#16 `[base] 底色镜像 OK`** | **= 1**（>1 ⇒ WARN：`g_ssrInBuilt` 被重跑？） | **B 核心** |
| **§#16 `[base] 底色图就绪`（VK 导入）** | **= 1**；建成却没就绪 ⇒ WARN 看 `[base]` 失败行（**退回 324 底色 ⇒ 水色仍偏透明，属预期**） | **B 核心** |
| **§#16 `[base] 底色#` 抢拷行** | **> 0**；就绪却无拷贝行 ⇒ WARN（一次性门 / 身份判据没过 ⇒ 底色内容不更新，画面是旧的） | **B 核心** |
| §#16 `[base]` desc 不一致 | **= 0**（>0 ⇒ **FAIL**：`CopyResource` 被静默丢弃，**水色不会回来**） | **B 核心** |
| 一行 `[base]` 都没有 | WARN：`ssr.base585=0` 或 `ssr.v1`/`ssr.vkout` 没开 ⇒ 水色仍偏透明，属预期 | 降级面 |
| §#16 `[v1] SSR v1 就绪` | = 1（含底色来源与 `smooth/blur/debug` 调参回显） | 回归对照 |
| §#16 `[v1] 渲染#` 行数 | > 0；有渲染行却没就绪行 ⇒ FAIL | 回归对照 |
| §#16 降级行（`→ v1 关自己` / `深度KMT 图还没就绪` / usage 退回 / `→ v1 关底色`） | 全 0 最好；任一 >0 ⇒ WARN + 按行文归因，**只关自己** | 降级面 |
| §#12 `[2d] VK出向#` / §#15 深度 KMT / §#13 / §#14 / 12e / 2c-β / desc / 哨兵 / `[异常]` | 与 §14.21.1 同（全 0 / 全绿） | 回归对照 |
| 帧时基线 | 5% 闸按 **16.85** 算（CPU 会话均值）；`[v1]` 的 fence 与 PoC-B **分列**看 | 回归对照 |

**画面（这一轮的真正判据 = 人眼，无自动判据）——按顺序走，每步只动一个旋钮**：

1. **`ssr.debug=1` 看法线**：水面应是**平滑斜面**（随水面坡度缓变）；仍像「碎石/噪点」⇒
   `ssr.smooth` 提到 8 / 16 再看。
2. **`ssr.debug=2` 看命中**：**红 = 未命中、绿 = 命中**。红区过大 ⇒ 调 `ssr.dist` /
   `ssr.steps`；岸线附近红绿乱跳属正常（深度跳变）。
3. **`ssr.debug=3` 看深度**：灰度 = 线性视距，水面应平滑、河床细节属正常（水体不写深度）。
4. **`ssr.debug=0` 正常模式看两条主判据**：
   - ① **倒影连续不碎**（默认 `ssr.smooth=4`；碎 ⇒ 8/16，仍碎再看第 1 步的法线图）。
   - ② **水色回来了**（不再近乎透明）。**没回来** ⇒ 先查 `[base]` 三行（建成/就绪/拷贝）；
     三行都对却仍透明 ⇒ 是 `ssr.strength` 太低（正对相机 `wgt≈0.08`，底色权重占绝对多数）
     ⇒ 试 `ssr.strength=2~4`。
   - ③ **`ssr.base585=0` 对照**：应退回 v0.18.6 的「水透明」行为 ⇒ 用来确认 B 真的在起作用。
5. **倒影位置/比例/远近/反向的调法仍按 §14.22 的 R4 表**（`ssr.fov` / `ssr.dist` /
   `ssr.steps` / `ssr.rev` / `ssr.near` / `ssr.far`）；`ssr.near`/`ssr.far` 一般不用动。
6. **完全没变化** ⇒ 看 `ssr.debug` 是不是忘改回 0（debug 模式会绕过合成），或 ini 没重启。

**这一轮的降级线**：任何一步失败只关自己 —— 底色建不出 ⇒ 退回 `uColor` 底色（= v0.18.6 水透明）；
v1 失败 ⇒ 退回 2d 纯拷贝（= v0.18.5）；`ssr=0` / `ssr.vkout=0` 是总逃生门。**底色镜像只读不
消费，不改任何既有通路的时序**；判读入档补 §14.23.1。

---

#### 14.23.1 实跑记录（2026-10-07，`v0.18.7` = `5b3fcfb` 代码批 + `d8b6e80` docs 批，CI SUCCESS）

**通路**：`run2c.ps1` 按 §14.23 判读表逐条对照 —— `expect v0.18.7: OK`、`[v1] SSR v1 就绪 = 1`、
`[v1] 渲染# > 0`、`[base] 底色镜像 OK = 1` / `[base] 底色图就绪 = 1` / `[base] 底色# > 0`、
`[base] desc 不一致 = 0`；§#12 `[2d] VK出向#` >0、§#15 深度 KMT 全绿、§#13/§#14/12e/2c-β/
desc/哨兵/`[异常]` 全 0；帧时在 5% 闸（16.85）内。**⇒ B（底色镜像）验收通过。**

**目视回报**：

| # | 现象 | 判 |
|---|---|---|
| ① **B 水色** | 水面不再近乎透明，水色正常 | **PASS（v0.18.7 B 达成）** |
| ② **A 倒影** | 碎渣被压掉了，但倒影仍不正常 —— 从「一屏碎渣」变成**不规则多边形拼图**（一块块边界清晰的多边形，各自一个反射方向） | **半修：`ssr.smooth=4` 只压住了面内噪声，压不住面间跳变** |

**归因（`v0.18.7` 预判得到确认，且拿到了 RenderDoc 关键证据）**：

1. **多边形 = 河床三角面法线**。水面像素**不写深度**，而 `520` 快照取在**段17 之前**（`ev21505`
   对 461 的段前拷贝）⇒ 那一刻水还没画 ⇒ 水面像素在 520 里读到的是**河床**。逐像素差分在
   「三角形内部」得到的是该三角形的**面法线**（常数），差分邻域再大也只是把面内噪声抹平，
   **相邻三角形之间的跳变永远在** ⇒ 反射方向按三角形跳变 ⇒ 不规则多边形拼图。`ssr.smooth`
   加到 8/16 也只是把「碎石」糊成「大石」，治不了面间跳变 —— **换数据源才治本**。
2. **关键证据（抓帧 + 文档双向确认）**：
   - `docs\待修复事项总结.md:128/137-138` —— **段16 / 段17 / 段18 的 `dsv` 全 = ResourceId 461**，
     且 `520 = ev21505` 是对 461 的**段前**拷贝 ⇒ `dW vs dPre` 是**同一缓冲的段后 vs 段前**，
     「同一缓冲、只差一个段」的比较成立。
   - `docs/02:1723` —— 段17 = `ev39530` 单 draw（rt0=321 + RT1=591），**blend 关、writeMask=15、
     depth 测关写开** ⇒ 段17 draw 之后 461 里就是**真·水面深度**（水体并回写进去的）。
   - 段17 前后分别是 `ev39512`（段17 的 OMSet）与 `ev39585`（段18 的 OMSet）；中间的
     `ev39546/39547` 是 Color 清（非 OMSet）⇒ **特征B 之后第一次换绑 = `ev39585`**，此时段17 的
     draw 已排完、段18/242 的透明还没画、**也还没走到 UI 中途的 ClearDS**（放到 Present 拷会被
     清成全 1.0 ⇒ 特征静默失效）。

**结论**：A 平滑只治了「面内噪声」，主因是「**法线源拿错了**」。⇒ `v0.18.8` 走**正解 B**：段16→段17
换绑之后**第一次换绑**时把 461 拷一份（第 5 张 SHARED 镜像），水面像素的法线/反射原点改用它，
520 只当行进层级 —— **设计与判读模板见 §14.24**。

---

### 14.24 v0.18.8 判读模板（正解 B：**段后水深做算法线**，代码批待 CI / 待实跑）

**一句话**：`v0.18.7` 的 A 只压住面内噪声、B 保住了水色；`v0.18.8` 换数据源 —— 水面像素的**法线与
反射原点**改用**段17 之后**的深度（真·水面深度），520 段前快照（= 河床）只继续当 ray march 的层级。
入向 2c / 2c-β / 2d / 深度 KMT §#15 / 12e / desc / 哨兵 / 底色通路 **全当回归对照**（`ssr.wdep=0`
时行为应与 `v0.18.7` 逐字节相同 —— 这也是这版的逃生门）。

**根因复述（§14.23.1 ①）**：水面像素在 520 里读到河床三角面 ⇒ 一整块三角形一个法线 ⇒ 倒影按三角形
跳变 = 不规则多边形拼图。

**关键设计约束（改代码前必须记住的四条）**：

1. **拷贝时机是硬约束**：只在**特征B（段16→段17 换绑）之后的第一次换绑**（= 段18 的 `ev39585`）
   拷 —— 此刻段17 draw 已排完（461 含真·水面深度）、段18/242 的透明还没画、**尚未走到 UI 中途
   ClearDS**。放到 Present 拷 ⇒ 461 已被清成全 1.0 ⇒ 特征静默失效。
2. **不用解绑/恢复**：`CopyResource` 源可 bound、目标（我方镜像）不 bound 即可 —— 游戏自身
   `ev21505` 就是同类先例。这样不污染 hook 的 OMSet 计数与身份学习。
3. **着色器判据必须用 viewZ 比较**：`linD` 在 `rev=0/1` 下单调性不同，直接比线性深度无效 ⇒
   `useW = p2.w>0.5 && dWr>0 && dWr<1 && viewZ(dWr) > viewZ(dPreR)`（水深**更近**才算水面）。
4. **行进层级仍用 uDepth(520)**：射线从水面出发，若拿水面深度当层级，一出门就打在自己脚下的
   水面上 ⇒ 一个也命中不了。只有 **origin + 法线** 换源。

**代码在哪**：

1. `main.cpp`：
   - 全局 `g_ssrWDepOn/Tex/H/Kmt/Src/Q/QLive/N/Arm/Fire/Off/Warn`（插在 `g_ssrInGateWarn` 旁）。
   - `ssrReconOm` **顶部**消费 `arm → fire`（置于一切 early return 之前，连 `NumViews=0` 的解绑
     也算数 —— 段17 的 draw 早已排进队列）；**feature-B 块内** `pDSV->GetResource(&r)` 存活引用
     `g_ssrWDepSrc` + `g_ssrWDepArm=true`，并先清 `fire` 防跨帧陈旧拷贝。
   - `hookedOMSetRenderTargets` 在 base 抢拷块之后消费 `fire → ssrWDepQueue()`（受 v1/vkout/shared
     三门 + `ssr.wdep`）。
   - `ssrWDepQueue()`（插在 `copyResDescChecked` 之后）：desc 预检（`descEqual`）→
     `ssrInMakeShared(..., "段后深度461", &kmt)`（**`nm` 必须含「深度」**才触发老式 SHARED/KMT
     兜底分支）→ 拷前自检不一致则 `g_ssrWDepOff` → `g_ssrSelfCopy=true` 期间 `CopyResource` →
     节流日志 `[wdep] 水深拷贝#` → 建 EVENT + `End` + `Flush`。
   - `notePresent` **帧末清 `arm/fire`**（段17 之后没等到换绑的特殊帧，不拿上一帧的水深去拷）。
   - ini：`ssr.wdep`（默认 1）、`ssr.debug` 合法范围 **0..5**、v1 参数日志加 `wdep=`、banner =
     **`v0.18.8`**。
2. `vkrenderer.cpp`：`ssrWDepVkBuild()`（**KMT/NT 双分支**导入 + `VK_FORMAT_D24_UNORM_S8_UINT`、
   `usage = SAMPLED`、desc 门 = D24 家族 / 1 mip / 1 array / 无 MSAA、换设备与 handle 变更**只丢
   不毁**、UNDEFINED→GENERAL 初转复用 `g_ssrVkCmdB`）+ `ssrOutVkFrame` 顶部 `ssrBaseVkBuild(c)`
   旁幂等调用；`ssrV1Sig` 吸收 `g_ssrWDepImg` ⇒ 变了自动重录；`ssrV1Build` 建 `g_ssrV1ViewW`
   （**depth aspect**）+ 描述符 **4 binding**（**没水深时 binding3 填 `g_ssrV1ViewC`**，靠 `p2.w`
   告诉 shader 采不采，避免空 view）；`ssrV1RecordRender` 加水深进/出 barrier（成对
   `GENERAL ↔ SHADER_READ`，`DEPTH|STENCIL` 两位同带，没水深整段跳过）与 `p2[3]` 压栈；
   `ssrV1DropViewW` / `ssrV1Free` / 「拆干净」块清理；`ssrInGateWait` **第二道闸**
   `[wdep] 段后水深EVENT闸#`（2c 的 `g_ssrInQ` 在拷贝前就 End 了，等不到它 ⇒ 必须单列；
   同款 DONOTFLUSH 自旋 2000ms / 一次性消费 / **独立计数**不与 12e 闸串味）。
3. `shaders/ssr.frag`：`uWdep`（binding 3）；`useW` 三条件判据；`dAt/PAt(uv, bool)` 按源取数
   （法线邻域 4 个点用与中心**同一个源**，免得水面/河床两种几何混进一个叉积）；origin + 法线
   `useW ? dWr : dPreR`；`ssr.debug` **4 = 段后水深灰度**、**5 = 水面像素 mask（绿=用水深，
   灰=退回 520）**。
4. `tools/`：`check1.ps1` `$need` 补 `g_ssrWDep*` / `ssrWDep*` / `g_ssrV1ViewW` / `ssr.wdep`；
   `run2c.ps1` §#16 增 `[wdep]` 计数与判据 + banner `expect v0.18.8`。
5. `make_shaders.ps1` 已跑通：`ssr.frag.spv = 17536 B`（原 14732）；`check1.ps1` → **RESULT OK**。

**跑法（与 §14.23 相同 + ini 追加 1 键）**：

| 键 | 默认 | 含义 |
|---|---|---|
| `ssr.wdep` | `1` | **正解 B 开关**：水面像素法线/原点改用段后水深；`0` = `v0.18.7` 行为（倒影仍多边形拼图） |

`ssr.smooth` / `ssr.blur` / `ssr.debug` / `ssr.base585` 沿用 §14.23（`smooth=4`、`blur=1`、
`debug=0`、`base585=1`）；其余仍是五行全开 + `ssr.v1=1` + `ssr.mode=1 / ssr.fov=65 /
ssr.near=10 / ssr.far=100000 / ssr.steps=32 / ssr.dist=500 / ssr.strength=1 / ssr.rev=0`。
换 artifact DLL → 实机 **≥2000 帧** → 先自查 `SSR侦察: ctx槽33/50/47 已挂` → `tools\run2c.ps1`。
**ini 启动只读一次 ⇒ 改完必须重启游戏。**

**判读表（`run2c.ps1` §#0~§#16 + 下列判据）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.8: OK`（否则 DLL 没换） | — |
| **§#16 `[wdep] 段后水深镜像就绪`** | **= 1**（>1 ⇒ WARN：源 desc 变过 / `g_ssrInBuilt` 被重跑？） | **正解 B 核心** |
| **§#16 `[wdep] 段后水深图就绪`（VK 导入）** | **= 1**；建成却没就绪 ⇒ WARN 看 `[wdep]` 失败行（**法线退回 520 河床 ⇒ 退回 `v0.18.7` 多边形拼图，属预期**） | **正解 B 核心** |
| **§#16 `[wdep] 水深拷贝#` 行** | **> 0**；图就绪却无拷贝行 ⇒ WARN（特征B 之后没等到换绑 ⇒ 水深内容不更新） | **正解 B 核心** |
| §#16 `[wdep] 源/镜像 desc 不一致` | **= 0**（>0 ⇒ **FAIL**：`CopyResource` 被静默丢弃 + 停用） | **正解 B 核心** |
| **§#16 `[wdep] 段后水深EVENT闸#`** | **> 0**（超时放行行 = 0 最好）；有拷贝却无闸行 ⇒ WARN（第二道闸没消费 ⇒ 水深可见性只靠 `Flush` 兜） | 正解 B 核心 |
| 一行 `[wdep]` 都没有 | WARN：`ssr.wdep=0` 或 `ssr.v1`/`ssr.vkout`/`ssr.shared` 有没开的 ⇒ **法线仍取 520 河床 = `v0.18.7` 行为** | 降级面 |
| §#16 `[v1] SSR v1 就绪`（回显含 `4 binding` + `水深=`） | = 1；`水深=461段17后(真水面深度)` 才是正解 B 生效的标志（`=520河床` ⇒ 没生效） | 回归对照 |
| §#16 `[base]` 三行 / `[v1] 渲染#` | 与 §14.23 同（水色那半边不能被这版弄坏） | 回归对照 |
| §#12 / §#15 / §#13 / §#14 / 12e / 2c-β / desc / 哨兵 / `[异常]` | 与 §14.23.1 同（全 0 / 全绿） | 回归对照 |
| 帧时基线 | 5% 闸按 **16.85** 算（多了第 5 张镜像的拷贝 + 第二道闸自旋，若超闸看 §#16 闸等待行） | 回归对照 |

**画面（真正判据 = 人眼，无自动判据）——按顺序走，每步只动一个旋钮**：

1. **`ssr.debug=4` 看第 5 张镜像内容**：应是**平滑的水面灰度**（近白远暗、无碎石纹理）。若与
   `ssr.debug=3`（520 河床）**几乎一样** ⇒ 拷的时机或源不对 ⇒ 先查 `[wdep]` 三行与关键证据
   （段16/17/18 的 `dsv` 是否仍是 461）。
2. **`ssr.debug=5` 看水面像素 mask**：**绿 = 判为水面并用水深，灰 = 退回 520**。绿区应**正好盖住
   水面**且边界贴岸线；绿区一片空白 ⇒ 判据没过（看 `ssr.wdep` 与 `[wdep]` 就绪行）；绿区漫到
   地面上 ⇒ 源内容不对（461 不是段后水深）。
3. **`ssr.debug=0` 正常模式看两条主判据**：
   - ① **倒影连续不碎**（不再有清晰多边形边界）。仍有多边形边界 ⇒ 回第 1/2 步归因，**别先动
     `ssr.smooth`**（它对面间跳变无效，这是 §14.23.1 的教训）。
   - ② **水色仍正常**（`v0.18.7` 的 B 不能被弄坏）⇒ 不正常先查 `[base]` 三行。
4. **`ssr.debug=1` 看法线**：水面应是**一整片平滑斜面**（随水面坡度缓变），不再是碎石/多边形。
5. **`ssr.wdep=0` 对照**：应退回 `v0.18.7` 的多边形拼图 ⇒ 用来确认正解 B 真的在起作用。
6. **倒影位置/比例/远近/反向仍按 §14.22 的 R4 表**（`ssr.fov` / `ssr.dist` / `ssr.steps` /
   `ssr.rev` / `ssr.near` / `ssr.far`）。
7. **完全没变化** ⇒ 看 `ssr.debug` 是不是忘改回 0，或 ini 没重启。

**这一轮的降级线（任一步只关自己）**：水深镜像建不出 / VK 导入失败 / view 建不出 ⇒ `p2.w=0` ⇒
**退回 `v0.18.7`**（法线仍取 520 河床，倒影仍多边形拼图，但不产生更坏的画面）；461 被 UI 中途
ClearDS 清成全 1.0 ⇒ `viewZ` 判据不成立 ⇒ 整帧退回 520（**天生自愈**）；`ssr.wdep=0` 是这版的
总逃生门，`ssr=0` / `ssr.vkout=0` 仍是总闸。**水深镜像只读不消费，不改任何既有通路的时序**。

#### 14.24.1 实跑记录（2026-10-07，`v0.18.8` = `85725b3` 代码批 + `6b630dc` docs 批，CI SUCCESS）

**通路（`tools\run2c.ps1` §#16 + §#0）—— 全绿，正解 B 确实生效**：

| 判据 | 实测 | 判 |
|---|---|---|
| #0 banner | `v0.18.8` | PASS |
| `[wdep] 段后水深镜像就绪` | **1** | PASS |
| `[wdep] 段后水深图就绪`（VK 导入） | **1**（KMT `handle=0000000080000302`、`D24/S8`、`usage=SAMPLED`、`alloc=dedicated`） | PASS |
| `[wdep] 水深拷贝#` | **51 行**（节流计数，`帧=905` 起；首绑拷 17 次后节流） | PASS（>0） |
| `[wdep] 段后水深EVENT闸#` | **51**，`超时=0` | PASS |
| `[wdep] 源/镜像 desc 不一致` | **0** | PASS |
| §#12/#13/#14/#15/12e/2c-β/`[2d-3]`/哨兵/`[异常]` | 全绿（唯一 FAIL = banner 版本差 ⇒ 属换 DLL 之前的旧日志切片，非新问题） | 回归 PASS |

**画面判读（人眼）**：

| # | 现象 | 判 |
|---|---|---|
| ① | **倒影不再破碎**（`v0.18.7` 的不规则多边形拼图消失） | **正解 B PASS** —— 水面像素法线确实换到了段后水深 |
| ② | **水色正常** | **B PASS**（`v0.18.7` 的底色批没被弄坏） |
| ③ | **倒影像"很多层堆叠在一起"** | **新问题 → `v0.18.9` A** |
| ④ | **有倒影的地方水面变平面，本来流动的波纹消失** | **新问题 → `v0.18.9` B** |

##### ③ 归因：585 里同时躺着两层反射（违反契约）

**证据 = 段17 PS 17586 反汇编 + 逐 draw 绑定表**（`docs/analysis/S1-pass6-ps-disasm.txt:418` /
`S4-extract-pass6.json:131390`）：

- 绑定：`t0=585`（RGBA16F）/ `t1=588`（R10G10B10A2）/ `t2=349`（R16G16 偏移）/ `t3=461`（D24S8）/
  `t4=339`（RGBA8 遮罩），`RT0=321 + RT1=591`，`dsv=461`，`numIndices=6`（全屏三角）。
- `discard_nz` 当 `t4 < 1e-4` ⇒ **段17 只在有水像素上画**。
- 主体：`out = mix(585, 588@涟漪扭曲UV, w)`，`w = (339*-0.85+0.95) * depthW[0.1..0.95] * cb2.w`
  ⇒ 水面像素（`339≈1`）`w ≈ 0.01..0.095` ⇒ **585 占水面像素的 90~99%**，剩下不到 10% 是被 349
  扭曲过的 588 场景项；`r1.z` 无效（扭曲 UV 出界 / `588.a != 1`）时**直接输出 585**。
- ⇒ **585 的契约 = 「一层反射色」，层叠是段17 的活**。而 `v0.18.7/8` 写进 585 的是
  `mix(cubemap(uBase), SSR, fresnel×strength×4)` ⇒ 585 里躺着**探针反射 + 屏幕空间反射**两层，
  段17 再按 90% 叠上去（外加 <10% 场景）⇒ 三层错位叠印 = "很多层堆叠"。
- 附带坑：我方**自算的 fresnel** 与段17 自己的权重相乘 ⇒ 掠射角（河面常态）`wgt→1` ⇒ 585 几乎
  纯 SSR，把 ④ 的波纹一起压没了。

##### ④ 归因：换掉 585 内容时把涟漪调制也换掉了

- 段16 的 16 个 draw = 水面对象，**按水体自身的涟漪法线**采 501 cubemap 写进 585（stencil
  `NotEqual ref=1` 是像素门、`writeMask=7` 只写 RGB、blend off）⇒ **原版 585 自带逐像素涟漪**。
- 我方的法线 = 段后水深重建的**几何平面**（水面网格是平的，波纹在法线贴图里，深度里没有）
  ⇒ 反射区成了纯平面镜；涟漪随 585 内容一起被覆盖 ⇒ 波纹消失。
- 段17 那 <10% 的 `588@扭曲` 只是场景/折射项，补不回**反射**里的涟漪。

⇒ `v0.18.9` = **A 契约修正**（585 只放一种反射、不自算 fresnel）+ **B 涟漪回注**（把段16 快照
的高频亮度结构乘回 SSR）。详见 §14.25。

### 14.25 v0.18.9 判读模板（**585 纯反射层契约** + **涟漪回注**，代码批待 CI / 待实跑）

**一句话**：`v0.18.8` 把倒影治"不碎"了，但 585 这个汇合点的**契约**错了 —— 它只该装**一层**反射
（§14.24.1 ③），而涟漪本来就长在段16 那层反射里（§14.24.1 ④）。这版两件事一起修：**A** 写进 585
的只有 SSR（或未命中时的原版兜底），fresnel/权重全部归段17；**B** 把段16 快照的**高频**亮度结构
按 `ssr.ripple` 乘回 SSR，把流动波纹还给反射区。

**关键设计约束（改代码前必须记住的四条）**：

1. **585 只放一层反射**：不掺 cubemap、不自算 fresnel。层叠（mix 权重、遮罩、深度衰减）是段17
   的活 —— 我方再叠一层就回到"多层堆叠"。
2. **`ssr.strength` 语义变了**：`v0.18.8` 及以前 = fresnel 之上的强度 `0..4`；**`v0.18.9` 起 =
   SSR 替换比 `0..1`**（`0` = 585 原样 = 游戏 cubemap = **原版观感**，一号对照；`1` = 纯 SSR）。
   CPU 侧夹 `0..1` ⇒ 老 ini 里 `>1` 的值静默夹到 1（v1 参数日志回显）。
3. **涟漪回注只吃高频**：`kf = clamp(lum(uBase)/lum(5tap(uBase)), 0.4, 2.5)` —— 低频（探针自己的
   山/天）被 5tap 除掉，**不会把探针内容重新印上来**（也就不会重新变回堆叠）；只在
   `ssr.base585=1`（uBase 在场）时生效，`ssr.ripple=0` 完全关；回注块必须放在
   `!hit && lastUV==uv` 兜底**之前**（兜底要覆盖它，保证 `strength=0` 是逐字节原版）。
4. **push constant 64B → 80B**（多一格 `p4` = 涟漪回注量），`ssrV1Sig` 吸收 `g_ssrV1Ripple`
   ⇒ 改 ini 后自动重录，不用重建 pipeline。

**代码在哪**：

1. `shaders/ssr.frag`：文件头 v0.18.9 两症状归因段；`push_constant` 加 `vec4 p4`；**涟漪回注块**；
   **删掉** Schlick fresnel（`ndv/F/wgt`）与 `!hit → wgt*0.5`；合成改
   `oColor = vec4(mix(baseRGB, refl, clamp(p1.z,0,1)), base.a)`。
2. `vkrenderer.cpp`：`pcr.size = 80`；`pc.p4[0] = g_ssrV1Ripple`（其余 3 格清 0）；`ssrV1Sig()`
   加 `cv.f = g_ssrV1Ripple`；`pc.p1[2]` 注释改语义。
3. `vkrenderer.h` / `main.cpp`：extern + 全局 `g_ssrV1Ripple = 1.0f`；`iniNum("ssr.ripple", 1.0)`
   夹 `0..1`；`g_ssrV1Strength` 上限 `4 → 1`；v1 参数日志加 ` ripple=`；**banner = `v0.18.9`**。
4. `tools/`：`check1.ps1` `$need` 补 `g_ssrV1Ripple` / `ssr.ripple` / `v0.18.9` → **RESULT OK**；
   `run2c.ps1` banner 改 `expect v0.18.9`（判据本身不变，§#16 `[wdep]`/`[base]` 全当回归对照）。
5. `make_shaders.ps1` 已跑通：`ssr.frag.spv = 18868 B`（原 17536）。

**跑法（与 §14.24 相同，ini 只多 1 键、改 1 行）**：

| 键 | 值 | 含义 |
|---|---|---|
| `ssr.ripple` | `1` | **B 波纹回注量** `0..1`；`0` = 关（反射区是干净平面镜） |
| `ssr.strength` | `1` | **语义改**：SSR 替换比 `0..1`；`0` = 原版 cubemap = **一号对照** |

`ssr.smooth=4 / ssr.blur=1 / ssr.debug=0 / ssr.base585=1 / ssr.wdep=1` 沿用 §14.24；其余五行全开
+ `ssr.v1=1` + `ssr.mode=1 / ssr.fov=65 / ssr.near=10 / ssr.far=100000 / ssr.steps=32 /
ssr.dist=500 / ssr.rev=0`。换 artifact DLL → 实机 **≥2000 帧** → 先自查
`SSR侦察: ctx槽33/50/47 已挂` → `tools\run2c.ps1`。**ini 启动只读一次 ⇒ 改完必须重启游戏。**

**判读表（`run2c.ps1`）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.9: OK`（否则 DLL 没换） | — |
| `[v1] SSR v1 就绪` 回显 | 含 `strength=`；`ripple=`/`ripplesz=`/`ripplemode=` 见 §14.26（v0.18.9 的就绪行漏回显 ripple，**v0.18.10 补上**）；`strength 现在是 SSR 替换比` 在 ini 参数行 | push/ini 生效 |
| §#16 `[wdep]` 五组计数 | 与 §14.24 判读表同（`就绪=1`、`拷贝#>0`、`desc不一致=0`、`闸#>0`） | 回归对照（A/B 不碰通路） |
| §#16 `[base]` 三行 | 与 §14.23 同（涟漪回注**依赖** uBase ⇒ 底色通路必须在场） | **回归对照** |
| §#12/#13/#14/#15/12e/2c-β/哨兵/`[异常]` | 全绿 | 回归对照 |
| 帧时基线 | 16.85 的 5% 闸（回注多 4 次 uBase 采样/像素，预期仍在闸内） | 回归对照 |

**画面（真正判据 = 人眼）——按顺序走，每步只动一个旋钮**：

1. **`ssr.strength=0` → 应回到原版观感**（有流动波纹、无堆叠）。这一步同时验证 §14.24.1 的两条
   归因：若原版有波纹、`strength=1` 时没有 ⇒ 问题确在 585 的内容，管线本身没坏。
2. **`ssr.strength=1 / ssr.ripple=0` → 纯 SSR、不回注**：③"多层堆叠"应**消失**（A 生效），
   ④"平面镜/没波纹"**仍在**（预期内）—— 这就是 A、B 各自的对照。
3. **`ssr.ripple=1` → 波纹回注**：反射区应重新出现**流动的波纹**（随水面流动走），且不再有第二层
   错位倒影。波纹太脏/太重 → `ssr.ripple=0.5`；太弱 → 先确认 `[base]` 三行在场。
4. **`ssr.debug=1` 看法线**：水面仍是**平滑斜面**（几何法线没变，波纹是**乘进去的调制**不是法线）
   —— 与 `v0.18.8` 一致即正常，不是回退。
5. **`ssr.debug=2` 看命中**：绿=命中 / 红=未命中；未命中处 `refl` 走边缘延展，`strength=0` 时整层
   变原版兜底。
6. **`ssr.base585=0` 对照**：涟漪回注**自动跳过**（没有 uBase 就没有回注源）+ 底色退回 324
   ⇒ 水色问题会回归（`v0.18.6` 行为），用来确认回注确实吃的是 uBase。
7. **倒影位置/比例/远近/反向仍按 §14.22 的 R4 表**（`ssr.fov` / `ssr.dist` / `ssr.steps` /
   `ssr.rev` / `ssr.near` / `ssr.far`）。
8. **完全没变化** ⇒ 看 `ssr.debug` 是不是忘改回 0，或 ini 没重启。

**这一轮的降级线（任一步只关自己）**：`ssr.ripple=0` 关掉 B（回到纯 SSR 平面镜）；`ssr.strength=0`
回到**原版 cubemap 反射**（最彻底的一号对照）；`ssr.base585=0` ⇒ 无回注源（B 自动跳过）+ 水色回归；
`ssr.wdep=0` 退回 `v0.18.7` 多边形拼图；`ssr.mode=0` 纯透传；总闸 `ssr=0` / `ssr.vkout=0` /
`ssr.v1=0`。**A/B 都只动 585 的内容，不改任何通路时序**（入向 2c / 2c-β / 2d / §#15 / 12e /
desc / 哨兵 / 底色 / 水深 全当回归对照）；判读入档补 §14.25.1。

#### 14.25.1 实跑记录（2026-10-07，`v0.18.9` = `d0426d2` 代码批 + `43c0391` docs 批 + `0cf4cb6` 引号修正，CI SUCCESS）

**用户实跑回报（原话）**：「水面有波纹，但是非常细小，不如原本的明显，观感不原本的好，倒影仍然是
多重堆叠的。」

⇒ **B 起效但量级不对**（波纹真的出现了 ⇒ 回注通路接上了，DLL/ini 也确实换成功了）；**A 没起效**
（堆叠仍在）。两个症状各有一条**本地就能坐实**的归因，和一条**必须实跑**才能分辨的归因：

**① 波纹细小 —— 本地坐实**：v0.18.9 的低频半径写死 `vec2(px * 2.0)`，**2px 带宽只抓得住像素级
亮度差**；河面波纹的空间周期在几 px ～ 几十 px 这一档，落在带外 ⇒ `kf` 几乎处处恒为 1，真正被
印上来的只剩轮廓/噪点 ⇒ 看着就是「细小、不明显」。另外**亮度调制这个机制本身**也不像原版：
原版涟漪是**反射方向被水面法线扰动**（段16 按涟漪法线采 cubemap），亮度做得再准也读不出「波在走」。
⇒ 归 `v0.18.10` 修（带宽可调 + 位移式回注，§14.26）。

**② 堆叠仍在 —— 先把三条路在本地排掉**（本节的证据全部来自已入库的抓帧分析文件）：

| 候选 | 证据 | 结论 |
|---|---|---|
| 段18 又把 585 画了一遍（同一层两遍） | 首笔 draw `ev39585` 绑定 `t0=19520(512×4096 BC3)`、`t1=520`、`t2=18483(256×512 BC3)`（`S4-extract-pass6.json:131459`），**没有 585** | **排除** |
| 跨帧递归（我们写的 585 流回下帧反射源，几何级数叠层） | `321→324` 折射拷贝 = `ev39225`，在段16（`ev39270`）**之前** ⇒ 反射源 324 里没有上一帧的 585 | **排除** |
| 段17 权重比我们以为的大 | `S1-pass6-ps-disasm.txt:460-481` 逐条：`r1.z = 扭曲UV∈[0,1] 且 588.a==1`；合法且 `cb2.z==0` ⇒ `w = (0.95 - 0.85·mask339) · depthW[0.1..0.95] · cb2.w`，水面 `mask≈1` ⇒ **`w ≤ 0.095`**（`cb2.z!=0` 那支给 `w=0.95`，只留 5% 我们这层 ⇒ 反射看得见就说明走的不是它） | 最终水面 = **90% 我们写的 585 + 9.5% 扭曲场景色**，再叠段18 水体混合 |

**⇒ 只剩两条候选，靠「只动一个旋钮」的隔离试验分开**（三个旋钮都在，`v0.18.9` 就能跑）：

1. **`ssr.strength=0`**（585 写回原版 cubemap 层）→ 堆叠应**消失**。消失 ⇒ 堆叠出在**我们这层的
   内容**；不消失 ⇒ 是游戏那 9.5% + 段18 水体，我们换不掉 ⇒ **改判据而不是改代码**（这是本轮最要紧
   的一个数据点）。
2. **`ssr.strength=1 / ssr.ripple=0`**（纯 SSR、关回注）→ 堆叠还在 ⇒ 与回注无关，转 `ssr.blur=0`
   试 5tap 叠影；堆叠**没了** ⇒ **回注把探针轮廓印上来了** —— mode0 的 `kf` 在阶跃边缘会顶到夹紧值，
   带宽越大印得越狠 ⇒ 直接换 `v0.18.10` 的位移式（两支梯度相减，阶跃归 0）。
3. **`ssr.blur=0`** → 5tap 十字平均落在深度跳变处会把轮廓前后各取一份 ⇒ 叠影候选。

**判读未完**：三条试验的结论 + 最终画面判读补写在本节；`v0.18.10` 的结果写 §14.26.1。

### 14.26 v0.18.10 判读模板（**回注带宽 + 位移式回注** + **自诊断 debug 6/7**，代码批待 CI / 待实跑）

**一句话**：`v0.18.9` 实跑证明回注通路接上了，但**带宽写死 2px**（波纹细小）且**亮度调制不像原版
涟漪**（原版是方向扰动）；堆叠归因也还没收敛（§14.25.1 已排掉段18 双读 / 跨帧递归 / 段17 权重三条）。
这版只动 B 和诊断，**A 契约一条不改**：585 仍只放一层反射，fresnel/权重仍归段17。

**关键设计约束（改代码前必须记住的六条）**：

1. **`ssr.ripplesz` = 回注带宽 `1..16` px（默认 4）**，两种方式**共用这一个旋钮**：mode0 是 5tap
   低频半径；mode1 是梯度差里「宽带那支」的半径 —— 位移式对**波长 ≈ 2·Rb** 的纹路响应最大，
   所以 `ripplesz ≈ 波长 / 2`。
2. **`ssr.ripplemode` 默认 `1` = 位移式**：采样**之前**拖 `ruv`（波纹该动的是反射方向）。梯度取
   **「1px 梯度 − Rb 梯度」** —— 阶跃轮廓两支梯度相近 ⇒ 相减归 0，**不把探针的山/天轮廓印上来**
   （这正是 mode0 的堆叠嫌疑）；波纹级往复则两支差最大 ⇒ 拖动跟着波纹走。`0` = `v0.18.9` 老路
   （亮度调制），随时退回去做对照。
3. **回注的场必须在采样之前算**（位移式要改 `ruv`），mode0 的乘法仍在采样**之后**；`!hit &&
   lastUV==uv` 兜底仍排最后 ⇒ `strength=0` 仍是逐字节原版。
4. **debug 判据必须闭区间**：`>4.5` / `>2.5` / `>0.5` 三个开区间会把 6、7 提前吃掉 ⇒ 收成
   `[4.5,5.5)` / `[2.5,3.5)` / `[0.5,2.5)`；深度缺失分支的 `>1.5` **保持不变**（2..7 才走灰、
   1 照旧透传，否则 debug=1 的兜底语义被改掉）。
5. push constant **仍是 80B**，`p4.y = ripplesz`、`p4.z = ripplemode`；`ssrV1Sig` 吸收两者 ⇒ 改 ini
   自动重录，不重建 pipeline。
6. **`ssr.ripplesz=1` 就近似不回注**（mode1 两支梯度相同 ⇒ 差为 0；mode0 中心≈带均值 ⇒ `kf≈1`），
   可当"半关"用。

**代码在哪**：

1. `shaders/ssr.frag`：文件头 **v0.18.10 归因段**（段17 权重逐条算死 + 排掉的三条路 + 两症状归因）；
   回注**场**前移 + mode0/mode1 分支；**debug 6/7 块**；三处 debug 判断改闭区间。
2. `vkrenderer.cpp/h`：`pc.p4[1] = g_ssrV1RippleSz`、`pc.p4[2] = g_ssrV1RippleMode`；`ssrV1Sig()`
   加两行 mix；`g_ssrV1RippleSz/Mode` extern。
3. `main.cpp`：全局 `g_ssrV1RippleSz = 4` / `g_ssrV1RippleMode = 1`；`iniNum("ssr.ripplesz", 4.0)`
   夹 `1..16`（越界回退并打日志）、`iniNum("ssr.ripplemode", 1.0)` 夹 `0|1`；debug 上限 `5 → 7`
   （报错文案也带上 6/7）；v1 参数日志加 ` ripplesz=` / ` ripplemode=`；**banner = `v0.18.10`**。
4. `tools/`：`check1.ps1` `$need` 补 `g_ssrV1RippleSz` / `ssr.ripplesz` / `g_ssrV1RippleMode` /
   `ssr.ripplemode` / `v0.18.10` → **RESULT OK**；`run2c.ps1` banner 改 `expect v0.18.10`
   （§#16 `[wdep]` / `[base]` 判据本身一条没动，全当回归对照）。
5. `make_shaders.ps1` 已跑通：`ssr.frag.spv = 23340 B`（原 18868）。

**跑法（ini 多 2 键、改 1 行）**：

| 键 | 值 | 含义 |
|---|---|---|
| `ssr.ripplesz` | `4` | **回注带宽** px `1..16`；太细/太碎 → 调大，定法见 `debug=7` |
| `ssr.ripplemode` | `1` | `1` = 位移扭曲（默认）/ `0` = 亮度调制（v0.18.9 老路） |
| `ssr.ripple` | `1` | **回注量**（两方式共用）；位移式下 `1` = 位移上限 6px，太晃调 `0.5` |
| `ssr.strength` | `1` | **SSR 替换比** `0..1`；`0` = 原版 cubemap = **一号对照** |
| `ssr.debug` | `0` | `6` = 回注可视化、`7` = uBase 原样（新）；其余同 §14.25 |

`ssr.smooth=4 / ssr.blur=1 / ssr.base585=1 / ssr.wdep=1` + 五行全开 + `ssr.v1=1` +
`ssr.mode=1 / ssr.fov=65 / ssr.near=10 / ssr.far=100000 / ssr.steps=32 / ssr.dist=500 /
ssr.rev=0` 沿用 §14.25。换 artifact DLL → 实机 **≥2000 帧** → 先自查 `SSR侦察: ctx槽33/50/47 已挂`
→ `tools\run2c.ps1`。**ini 启动只读一次 ⇒ 改完必须重启游戏。**

**判读表（`run2c.ps1`）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.10: OK`（否则 DLL 没换） | — |
| `[v1] SSR v1 就绪` 回显 | 含 `ripplesz=` / `ripplemode=` / `ripple=` | push/ini 生效 |
| §#16 `[wdep]` 五组、`[base]` 三行 | 与 §14.24 / §14.23 判读表同 | **回归对照**（这版不碰通路） |
| §#12/#13/#14/#15/12e/2c-β/哨兵/`[异常]` | 全绿 | 回归对照 |
| 帧时基线 | 16.85 的 5% 闸（mode1 多 8 次 uBase 采样/像素，预期仍在闸内） | 回归对照 |

**run2c 自身这轮的两处改动（`v0.18.10` 同批，判读前先看懂，别当成回归）**：

1. **日志路径自动选**：原来把路径**写死**在 `ModOrganizer\Skyrim Special Edition fsr\overwrite\...`，
   `v0.18.9` 那轮脚本里搜不到 —— **原因后由用户澄清：那份 log 当时被手动删了**，那一轮的落点又变成
   游戏 cwd `D:\The Elder Scrolls V Skyrim\poc-presenter.log` ⇒ 现在在「游戏根目录 + 各 MO2 实例
   `overwrite\SKSE\Plugins`」里自动取**最新**的一份，首行多打 `logfile=...`；一份都搜不到则输出候选
   列表并 `exit 1`（不再 `Get-Item` 直接抛异常崩掉）。要强制指定某一份，改脚本顶部 `$forceLog`。
   **`v0.18.10` 实跑已证明这个修正是必需的**：这轮日志又落在 fsr 那条（276097 B / 15:18），而
   `D:\...\poc-presenter.log` 此刻**不存在** —— 写死哪一条都会翻车，取最新才选得对（详见 §14.26.1）。
2. **`2d 出向读回` 在 v1 活着时不判**：`v0.18.6` 起出向由「原样拷」改成「**shader 产物**」，所以
   `[v1] 就绪` 时入向/出向校验和必然不同 —— `v0.18.9` 日志实测 **10/10 不一致且差得彻底**（入向
   `0xFA2A740613FB25F8` / 出向 `0x0D058FB07D7C51F0`）= 内容确实被换过，**不是差一帧**。所以
   `v1 就绪 > 0` 时把这条从 FAIL/WARN 降成信息行（原判据 `≥1 次一致` / `预期 0 次不一致` 只在
   `ssr.v1=0` 或 `ssr.mode=0` 纯透传时才有意义）。

**画面（真正判据 = 人眼）——按顺序走，每步只动一个旋钮**：

1. **`ssr.strength=0` → 应回原版观感**（有流动波纹、无堆叠）。**这是 §14.25.1 试验①**：消失 ⇒
   堆叠在我们这层；不消失 ⇒ 在游戏那 9.5%+水体（那就换不掉，先记录再议）。
2. **`ssr.strength=1 / ssr.ripple=0` → 纯 SSR、关回注**：堆叠仍**在** ⇒ 与回注无关（转第 6 步
   `ssr.blur=0`）；堆叠**没了** ⇒ 是 mode0 印轮廓 ⇒ 用第 4 步的位移式。
3. **`ssr.ripple=1 / ssr.ripplemode=0` → 老路、带宽 4**：波纹应比 `v0.18.9`（带宽 2）**粗一档**，
   这一步只验证「带宽」这一个变量。
4. **`ssr.ripplemode=1` → 位移式**：波纹应**跟着水面流动走**（反射内容整体抖动，而不是变亮/变暗），
   且轮廓堆叠应比 mode0 轻。
5. **`ssr.debug=7` 看原版那层**：量一下涟漪的**波长（px）与流向** ⇒ `ssr.ripplesz ≈ 波长 / 2`；
   再 **`ssr.debug=6` 看回注场**（mode1 画位移灰度、mode0 画红/蓝 `kf`），全黑 ⇒ 没回注源（查 `[base]`）。
6. **`ssr.blur=0`** → 5tap 叠影对照；**`ssr.blur=1`** 回来。
7. **`ssr.debug=1/2`** 法线仍应是平滑斜面 / 命中率（回归）；**`ssr.base585=0`** ⇒ 回注自动跳过 +
   水色回归（对照，同 §14.25 第 6 步）。
8. 波纹太脏/太晃 → `ssr.ripple=0.5`；仍脏 → `ssr.ripplesz` 调小（只认更短的波）；轮廓仍堆叠 →
   `ssr.ripplemode=0` 做 A/B。
9. **倒影位置/比例/远近/反向仍按 §14.22 的 R4 表**（`ssr.fov` / `ssr.dist` / `ssr.steps` /
   `ssr.rev` / `ssr.near` / `ssr.far`）；完全没变化 ⇒ 看 `ssr.debug` 是不是忘改回 0，或 ini 没重启。

**这一轮的降级线（任一步只关自己）**：`ssr.ripplesz=1` 半关回注；`ssr.ripple=0` 完全关 B；
`ssr.ripplemode=0` 退回 v0.18.9 老路；`ssr.strength=0` 回**原版 cubemap**（一号对照）；
`ssr.base585=0` ⇒ 无回注源 + 水色回归；`ssr.wdep=0` 退回 `v0.18.7` 拼图；`ssr.mode=0` 纯透传；
总闸 `ssr=0` / `ssr.vkout=0` / `ssr.v1=0`。**这版只改 585 的内容与诊断画面，不改任何通路时序**
（入向 2c / 2c-β / 2d / §#15 / 12e / desc / 哨兵 / 底色 / 水深 全当回归对照）；判读入档补 §14.26.1。

#### 14.26.1 实跑记录（2026-10-07，`v0.18.10` = `5e44175` 代码批 + `86b2bb7` docs 批 + `6e9ee4a` tools+docs，CI SUCCESS）

**日志**：`C:\Users\joker\AppData\Local\ModOrganizer\Skyrim Special Edition fsr\overwrite\SKSE\Plugins\poc-presenter.log`
（276097 B / 1873 行 / mtime 15:18:05）—— **这就是 run2c 首行 `logfile=` 自动选中的那份**；
`D:\The Elder Scrolls V Skyrim\poc-presenter.log` 此刻**不存在**（用户澄清：fsr 那份先前搜不到是被
手动删的，`v0.18.9` 那轮落在游戏 cwd，这轮又回到 fsr）⇒ §14.26「日志路径自动选」实测必要。

**版本与旋钮回显（自查通过）**：banner `poc-presenter v0.18.10`；参数行 `strength=1.000000 rev=0
smooth=4 blur=1 debug=0 base585=1 wdep=1 ripple=1.000000 ripplesz=4 ripplemode=1`；就绪行
`push constant 80B` ⇒ **DLL 换成功、两个新键都被读到、走的全是默认值**（这轮没做任何隔离试验）。

**用户实跑回报（原话）**：「问题依然存在，**扇形范围内是错误的倒影**，屏幕作用两边一小部分水纹正常。」
配图 `docs/analysis/Screenshot_SSR debug.png`（1918×1071：湖面中央一块上宽下窄的扇形亮区，两侧窄条是
正常涟漪；扇形**上半**是镜像、**下半**是同一棵树被放大重贴了一份，中间有一条水平接缝）。

**图和代码对上（本地归因，抓帧不用重做）** —— 那个扇形是 **hit/miss 的结构**，不是"我们只画了这么大"：

| 位置 | 射线行为 | `v0.18.10` 走的分支 | 实际画面 |
|---|---|---|---|
| 扇形**上半**（远水） | 反射角小、近水平 ⇒ 还没出屏先撞对岸悬崖 | `hit` → 采 `uColor@hitUV` | 看起来是正常镜像（横纹 = 岩层本体 + 32 步量化） |
| 扇形**下半**（近水） | `θ = atan(相机高 / 水距)` 大 ⇒ 一两跳就飞出**屏顶** | `miss` → `ruv = lastUV`（≈**屏顶边**那一处） | **采到岸边/树的原位画面（没经过镜像关系）整块贴进水里 = 错位倒影、同一棵树两份** |
| 屏幕**左右窄条** | 第一跳就在屏外 ⇒ `lastUV == uv` | 兜底 `refl = baseRGB` | **原版层 = 用户说的「两边一小部分水纹正常」** |

⇒ 两侧"正常"恰好证明**回退到原版层是对的方向**；`v0.18.10` 只把「零步进屏」这一种情况回退了，
其余 `miss` 全走「屏幕边缘延展」⇒ 错误内容只出现在扇形中间。**用户报的两个症状是同一条分支的两半。**

⇒ **修法归 `v0.18.11`（§14.27）**：未命中一律回原版层（门 `ssr.edge`）+ `hit` 但 `hitUV` 贴屏边 4% 淡回。

**可证伪的验证（与 §14.25.1 那三条试验合并跑，每步只动一个旋钮 + 重启游戏）**：

1. **`ssr.debug=2`（命中绿 / 未命中红）** → 扇形边界应与**红绿分界**重合：扇形内绿、扇形外
   （左右窄条 + 下半带）红 ⇒ 上表归因坐实；对不上则推翻本节归因，回抓帧重查。
2. **`ssr.edge=1`** → 应**复现**这张截图的错位倒影；`ssr.edge=0`（默认）⇒ 那块贴图消失、下半带换成
   原版观感（有涟漪）。
3. §14.25.1 的 `ssr.strength=0` / `ssr.ripple=0` / `ssr.blur=0` 三条**仍未跑**，接着跑 —— 它们判的是
   「多重堆叠」，本节判的是「错位贴图」，是两件事，别混。

**判读未完**：`v0.18.11` 的实跑结果写 §14.27.1；§14.25.1 的三条试验结论回来后补到 14.25.1。

### 14.27 v0.18.11 判读模板（**未命中回原版层 `ssr.edge`** + **hit 贴边淡出**，代码批待 CI / 待实跑）

**一句话**：`v0.18.10` 实跑把问题定位成**射线出屏后的回退源拿错了**（不是行进、不是法线、不是段17
权重），这版只动"未命中时用哪一层"：**命中路径、段17 契约、push constant 大小（仍 80B，`p4.w` 从
预留转正）一条不改**。

**关键设计约束（改代码前必须记住的六条）**：

1. **`ssr.edge`（`p4.w`）默认 `0` = 未命中一律回 `baseRGB`**；`1` = `v0.18.10` 的屏幕边缘延展（A/B
   对照）。理由：镜像点已经出屏时 `lastUV` 处拿到的是**未镜像**的原图，贴进水里必然错位；而我们手上
   正好有游戏自己那层（`uBase` = 段16 输出、带涟漪）可以**无条件兜底** —— 屏幕里看不到的内容本来就
   该交给 cubemap（这正是"方案C 混合"该有的分工：屏幕里看得见的用 SSR，看不见的用探针）。
2. **`hit` 但 `hitUV` 贴屏边 4% ⇒ 淡回 `baseRGB`**（硬编码 `0.04`，不开新键）：扇形边界上的像素正是
   「再走一跳出屏就变 miss」那批，它们的 `hitUV` 就落在屏幕边附近 ⇒ 淡出后边界**两边都是反射**
   （锐 SSR ↔ 原版层），不再是「镜像图 硬接 未镜像贴图」那条硬缝。
3. **兜底仍排在 mode0 亮度回注之后、debug 早返回之前** ⇒ `strength=0` 仍逐字节原版；`debug=6/7` 的
   语义与画面**不变**（它们不看 `refl`）。
4. push constant **仍是 80B**，只把 `p4.w` 由预留改用；`ssrV1Sig()` 补一行 mix ⇒ 改 ini 自动重录、
   不重建 pipeline。
5. **`ssr.edge=1` 时贴边淡出仍然生效**（淡出只走 `hit` 分支）⇒ A/B 的差异严格限定在「miss 的回退源」，
   一个变量对照。
6. **A 契约一条不改**：585 仍只放一层反射、fresnel/权重仍归段17；这版只是让"这一层"在射线出屏处
   退回游戏那层。

**代码在哪**：

1. `shaders/ssr.frag`：文件头 **v0.18.11 归因段**（扇形 hit/miss 三分区表 + 边缘延展为什么没物理意义）；
   `p4.w` 注释转正；兜底改 `!hit && (p4.w < 0.5 || lastUV == uv)` + `hit` 贴边 4% 淡出；`ruv` 与
   `strength` 两处注释同步。
2. `vkrenderer.cpp/h`：`pc.p4[3] = g_ssrV1Edge`（原 `0.0f`）；`ssrV1Sig()` 加一行 mix；就绪行回显
   ` edge=`；`extern g_ssrV1Edge`。
3. `main.cpp`：全局 `g_ssrV1Edge = 0`；`iniNum("ssr.edge", 0.0)` 夹 `0|1`；v1 参数日志加 ` edge=`；
   **banner = `v0.18.11`**。
4. `tools/`：`check1.ps1` `$need` 补 `g_ssrV1Edge` / `ssr.edge` / `v0.18.11` → **RESULT OK**；
   `run2c.ps1` banner 改 `expect v0.18.11`（`#0` 判据 / `§#16` 标题 / 收尾标签）+ 新增 WARN：
   就绪行缺 ` edge=`（回显没落上去时提醒，不连坐）。
5. `make_shaders.ps1` 已跑通：`ssr.frag.spv = 23972 B`（原 23340）。

**跑法（ini 多 1 键、改 1 行）**：

| 键 | 值 | 含义 |
|---|---|---|
| `ssr.edge` | `0` | **`0` = 未命中回原版层（默认）** / `1` = `v0.18.10` 屏幕边缘延展（A/B 对照） |
| `ssr.debug` | `0` | `2` = 命中绿/未命中红（本版**关键验证**）；其余同 §14.26 |
| `ssr.strength` | `1` | `0` = 原版 cubemap（§14.25.1 试验①，还没跑） |

`ssr.smooth=4 / ssr.blur=1 / ssr.base585=1 / ssr.wdep=1 / ssr.ripple=1 / ssr.ripplesz=4 /
ssr.ripplemode=1` + 五行全开 + `ssr.v1=1` + `ssr.mode=1 / ssr.fov=65 / ssr.near=10 / ssr.far=100000 /
ssr.steps=32 / ssr.dist=500 / ssr.rev=0` 沿用 §14.26。换 artifact DLL → 实机 **≥2000 帧** → 先自查
`SSR侦察: ctx槽33/50/47 已挂` → `tools\run2c.ps1`。**ini 启动只读一次 ⇒ 改完必须重启游戏。**

**判读表（`run2c.ps1`）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.11: OK`（否则 DLL 没换） | — |
| `[v1] SSR v1 就绪` 回显 | 含 ` edge=`（缺 → 新 WARN） | 新键/新回显落地 |
| §#16 `[wdep]` 五组、`[base]` 三行 | 与 §14.24 / §14.23 判读表同 | **回归对照**（不碰通路） |
| §#12/#13/#14/#15/12e/2c-β/哨兵/`[异常]` | 全绿 | 回归对照 |
| 帧时基线 | 16.85 的 5% 闸（这版只多两个 `min`+`smoothstep`，预期仍在闸内） | 回归对照 |

**画面（真正判据 = 人眼）——按顺序走，每步只动一个旋钮**：

1. **默认 `ssr.edge=0`**：扇形**下半**那块「错位倒影 / 同一棵树两份」应**消失**，换成原版观感（有涟漪）；
   扇形**上半**仍应是镜像；**左右窄条不变**（本来就是原版层）。
2. **`ssr.debug=2`**：扇形边界应与**红(未命中)/绿(命中)**分界重合 ⇒ §14.26.1 归因坐实；对不上 ⇒
   记录实际红绿分布再议（别先改代码）。
3. **`ssr.edge=1`**：应**复现** `v0.18.10` 那张截图 ⇒ 确认修的是同一条分支（A/B 对照）。
4. 扇形边界仍显眼 ⇒ 现在边界两边都是反射（锐 SSR ↔ 原版层），应比之前轻；仍突兀 ⇒ 下一手是把 4%
   调大（改常数重编）或再按命中距离混一层，先记录不急着动。
5. **`ssr.strength=0`** ⇒ 回原版（§14.25.1 试验①，**还没跑，接着跑**）。
6. **`ssr.ripple=0`** ⇒ 关回注（试验②）；**`ssr.blur=0`** ⇒ 5tap 对照（试验③）。
7. **`ssr.debug=1`** 法线应仍是平滑斜面；命中率回归看 `debug=2` 绿区有没有比 `edge=1` 时变小
   （不该变 —— 这版不动行进）。
8. 波纹观感仍按 §14.26 第 3/4/5/8 步（`ssr.ripplemode` / `ssr.ripplesz` / `ssr.ripple`）。
9. 倒影位置/比例/远近/反向仍按 §14.22 的 R4 表（`ssr.fov` / `ssr.dist` / `ssr.steps` / `ssr.rev` /
   `ssr.near` / `ssr.far`）；完全没变化 ⇒ 先看 `ssr.debug` 是不是忘改回 0，或 ini 没重启。

**这一轮的降级线（任一步只关自己）**：**`ssr.edge=1` 一步退回 `v0.18.10` 行为**（本版自带的 A/B 开关）；
`ssr.strength=0` 回原版 cubemap；`ssr.ripple=0` 关回注；`ssr.base585=0` ⇒ 无回注源 + 水色回归；
`ssr.wdep=0` 退回 `v0.18.7` 拼图；`ssr.mode=0` 纯透传；总闸 `ssr=0` / `ssr.vkout=0` / `ssr.v1=0`。
**这版只改「未命中用哪一层」与一处贴边淡出，不改任何通路时序**（入向 2c / 2c-β / 2d / §#15 / 12e /
desc / 哨兵 / 底色 / 水深 全当回归对照）；判读入档补 §14.27.1。

#### 14.27.1 实跑记录（2026-10-07，`v0.18.11` = `d0681a2` 代码批 + `77874c3` docs 批，CI SUCCESS）

**三张图（每步只改一个旋钮，其余原封不动）**：`docs/analysis/Screenshot_ssr.debug=2.png`、
`Screenshot_ssr.edge=0.png`、`Screenshot_ssr.edge=1.png`。

**用户实跑回报（原话）**：「三张都跑了，只改了对应项，其他的都保持没变，**只有 `ssr.edge=0`
效果较好**，除了倒影质量差点，以及倒影区域水波较小，还有人物身体一圈区域，其他的问题不大。」

**判读（三条全部命中预期）**：

1. **`ssr.debug=2` ⇒ §14.26.1 的归因坐实**：绿(命中) = 贴岸那条带 + 人物正下方一条竖带；
   红(未命中) = 近处整片水面（含左右窄条）。与「远水撞对岸 = hit / 近水飞出屏 = miss /
   零步进屏 = 原版层兜底」**同构** ⇒ 那个扇形确实就是 hit/miss 结构，不是 bug 面积。
2. **`ssr.edge=1` ⇒ 复现并放大了错位**：整片水面被贴成屏幕顶边那张画面（岸、蓝树、碎石），
   连一条条**直边**都在 —— 屏幕边缘延展这条路彻底废，`edge=0`（回原版层）是唯一正解。
3. **`ssr.edge=0` ⇒ 修复确认**：水面正常、贴岸带是镜像、其余是原版层的涟漪。
   **`v0.18.11` 的回退与默认值就此定死，不再动。**

**剩余三处全部落到具体代码 ⇒ 归 `v0.18.12`（§14.28）**：

| 症状 | 归因（本地坐实） | 修法 |
|---|---|---|
| **人物身体一圈区域** | **前景遮挡假命中**：射线从原点往**深处**走，本来就碰不到站在它**前面**的人/石头；但深度图是高度场，轮廓处**突然变浅** ⇒ 单看 `Q.z < sz` 会把「射线落在它后面」当成命中，紧挨轮廓那一圈像素把身体颜色采进来糊在水上（`debug=2` 里人物外圈那道**绿边**就是它） | 命中多认一道 **`sz <= P.z * 0.98`（反射目标不许比原点浅 2%）**，判定与二分**两处**都加；人/石头**正下方**那条竖带（目标确实更深）是合法反射，**不受影响** |
| **倒影质量差（块状/阶梯）** | 末段二分只有 **3 次** ⇒ 32 步时命中点精度 = 156/8 ≈ 19.5 单位，一跳一跳就是色块 | **二分 3 → 5 次**（精度 ≈ 4.9，只多 2 次深度采样）；ini 侧另有 `ssr.steps=64` / `ssr.blur=0` 可试，不用重编 |
| **倒影区域水波较小** | 位移幅度**写死 `6.0 * px`**，与波长脱钩 | 幅度改随 `ssr.ripplesz` 缩放 `clamp(1.5·ripplesz, 6, 16)` px ⇒ **`ripplesz=4` 仍是 6px，与 v0.18.11 逐位一致**（默认画面不变）；亮度那一半（sparkle 有没有回到贴岸带里）要走 mode0 ⇒ §14.28 第 4 步 |

**判读未完**：`v0.18.12` 的实跑结果写 §14.28.1；§14.25.1 那三条隔离试验（`strength=0` /
`ripple=0` / `blur=0`）**仍未跑**。

### 14.28 v0.18.12 判读模板（**前景遮挡守卫 + 二分 5 次 + 位移幅度随 ripplesz**，代码批待 CI / 待实跑）

**一句话**：`v0.18.11` 已经把「错位倒影」修掉并定死默认值；这一版**只收紧命中的判定与精度、
把回注幅度和波长挂钩** —— `edge=0` 那条回退、A 契约、push constant 80B、通路时序**一条不改**。

**关键设计约束（改代码前必须记住的六条）**：

1. **守卫加在两处**（命中判定 + 二分内部）：只加外面那处，二分会往「轮廓变浅」那一侧收敛，
   等于白加。容差 `0.98`（2%）是留给深度噪声的。
2. **守卫只减少命中**：判不中就走 `ssr.edge=0` 的原版层兜底 ⇒ 最坏情况是「少了一块 SSR」，
   **不会产生更坏的画面**（这是它敢不开键的底气）。**没有逃生键**，要回退就改容差重编。
3. **二分 3 → 5** 只多 2 次深度采样/命中像素，帧时预期不动；命中更细可能带来逐像素抖动 ⇒
   兜底看 `ssr.smooth` / `ssr.blur`。
4. **幅度 `ripAmp = clamp(1.5 * ripplesz, 6, 16)` px**：`ripplesz=4 ⇒ 6px`，与 `v0.18.11`
   **逐位一致**（默认画面不变）；`ssr.ripple` 仍是总闸 `0..1`。`debug=6` 的归一化也改用
   `ripAmp`，否则 `ripplesz > 4` 时灰度会被截顶。
5. **push constant / 描述符 / `ssr.edge` 回退 / A 契约一条没改** ⇒ 改 ini 仍只重录命令。
6. **`strength=0` 仍逐字节原版**（守卫在 `refl` 计算内，原版兜底仍排在它后面）。

**代码在哪**：

1. `shaders/ssr.frag`：文件头 **v0.18.12 归因段**（三张图结论 + 三症状归因）；命中判定改
   `Q.z < sz && sz <= P.z * 0.98`、二分内同样加守卫、**`k < 3` → `k < 5`**；`ripAmp` 随
   `ripplesz` 缩放，`debug=6` 归一化改用 `ripAmp`。
2. `main.cpp`：**banner = `v0.18.12`**（没有新全局、没有新 ini 键）。
3. `tools/`：`check1.ps1` `$need` 的版本串 → `v0.18.12`；`run2c.ps1` banner 三处 →
   `expect v0.18.12`（`#0` / `§#16` 标题 / 收尾标签，`edge=` 回显 WARN 保留）。
4. `make_shaders.ps1` 已跑通：`ssr.frag.spv = 24560 B`（原 23972）。

**跑法（没有新键，只调已有的）**：

| 键 | 值 | 含义 |
|---|---|---|
| `ssr.debug` | `2` | **关键验证**：人物外圈那道绿边应消失，贴岸带/竖带/红绿分界不变 |
| `ssr.ripplesz` | `4` → `8` | 波长与**幅度**一起变大（6px → 12px），太晃回 `4` 或 `ssr.ripple=0.5` |
| `ssr.ripplemode` | `1` → `0` | 试「亮度那一半」：sparkle 有没有回到贴岸带；印轮廓就退回 `1` |
| `ssr.steps` | `32` → `64` | 试命中精度（配合二分 5 次），看帧时 |
| `ssr.blur` | `1` → `0` | 试锐度（5tap 十字平均是按 `ssr.smooth` 的半径糊的） |

基线沿用 §14.27（`ssr.edge=0` / `ssr.smooth=4` / `ssr.blur=1` / `ssr.base585=1` / `ssr.wdep=1` /
`ssr.ripple=1` / `ssr.ripplemode=1` / 五行全开 / `ssr.v1=1` / `ssr.mode=1 / fov=65 / near=10 /
far=100000 / steps=32 / dist=500 / rev=0`）。换 artifact DLL → 实机 **≥2000 帧** → 自查
`SSR侦察: ctx槽33/50/47 已挂` → `tools\run2c.ps1`。**ini 启动只读一次 ⇒ 改完必须重启游戏。**

**判读表（`run2c.ps1`）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.12: OK`（否则 DLL 没换） | — |
| `[v1] SSR v1 就绪` 回显 | 含 ` edge=`（缺 → 新 WARN） | 回归 |
| §#16 `[wdep]` 五组、`[base]` 三行 | 同 §14.24 / §14.23 | **回归对照** |
| §#12/#13/#14/#15/12e/2c-β/哨兵/`[异常]` | 全绿 | 回归对照 |
| 帧时基线 | 16.85 的 5% 闸（多 2 次深度采样，预期仍在闸内） | 回归对照 |

**画面（真正判据 = 人眼）——按顺序走，每步只动一个旋钮**：

1. **默认（`edge=0` / `ripplesz=4`）** ⇒ 应与 `v0.18.11` 的 `edge=0` **基本一致**（幅度没变），
   唯一可见变化是**人物外圈那道边消失或明显变淡**（守卫生效）。
2. **`ssr.debug=2`** ⇒ 人物外圈的**绿边应没了**，只剩人物正下方那条**竖带**（合法反射）；
   贴岸带与红绿分界应与 `v0.18.11` 一模一样（这版不该改变命中率）。
3. **`ssr.ripplesz=8`** ⇒ 贴岸带里的波纹应**变长变大**（12px 上限）；太晃 → 回 `4` 或
   `ssr.ripple=0.5`。
4. **`ssr.ripplemode=0` + `ssr.ripplesz=8`** ⇒ 试「亮度那一半」：原版那些**白色 sparkle**
   有没有回到贴岸带里；如果山/天轮廓被印上来 ⇒ 退回 `mode=1`（记下，别硬改）。
5. **`ssr.steps=64`** ⇒ 块状应明显减轻（记帧时）；**`ssr.blur=0`** ⇒ 更锐、可能更碎，二选一。
6. **`ssr.debug=7`** 量原版波长 ⇒ `ssr.ripplesz ≈ 波长 / 2`（幅度跟着走，不再单独调）。
7. **`ssr.edge=1`** ⇒ 应回到 §14.27.1 那张错位图（A/B，确认回退链没退化）。
8. **`ssr.strength=0` / `ssr.ripple=0` / `ssr.blur=0`** ⇒ §14.25.1 的三条隔离试验，
   **至今还没跑**，这轮一起补（判「多重堆叠」，与本轮三症状不是一回事）。
9. 倒影位置/比例/远近/反向仍按 §14.22 的 R4 表（`ssr.fov` / `ssr.dist` / `ssr.steps` /
   `ssr.rev` / `ssr.near` / `ssr.far`）；完全没变化 ⇒ 先看 `ssr.debug` 是不是忘改回 0。

**这一轮的降级线（任一步只关自己）**：`ssr.ripplesz=4` 幅度回 6px；`ssr.ripple=0` 关回注；
`ssr.ripplemode=1` 回位移式；**`ssr.edge=1` 退回 `v0.18.10`**；`ssr.strength=0` 回原版 cubemap；
`ssr.base585=0` ⇒ 无回注源 + 水色回归；`ssr.wdep=0` 退回 `v0.18.7` 拼图；`ssr.mode=0` 纯透传；
总闸 `ssr=0` / `ssr.vkout=0` / `ssr.v1=0`。**前景遮挡守卫没有 ini 键**（只减命中、不加坏画面），
真要回退改 `P.z * 0.98` 的容差重编。**这版不改任何通路时序**（入向 2c / 2c-β / 2d / §#15 /
12e / desc / 哨兵 / 底色 / 水深 全当回归对照）；判读入档补 §14.28.1。

#### 14.28.1 实跑记录（2026-10-07，`v0.18.12` = `ac8e5d6` 代码批 + `9c1313b` docs 批 + `cdb2184` R4 取证，CI SUCCESS）

**用户实机反馈（四张图，全部入库 `docs/analysis/`）：「人物周围一圈还在」**。

| # | 文件 | 只改了 | 用途 |
|---|---|---|---|
| 1 | `Screenshot_v0.18.12 默认.png` | 无（`debug=0`） | 白圈症状本身 |
| 2 | `Screenshot_v0.18.12 ssr.debug=2.png` | `debug=2` | A 组基线 |
| 3 | `Screenshot_v0.18.12 ssr.debug=2 ssr.fov=58.7155 .png` | `fov` | §14.29 A/B 第 1 条 |
| 4 | `Screenshot_v0.18.12 ssr.debug=2 ssr.steps=128.png` | `steps` | §14.29 A/B 第 2 条 |

（用户原话「3和4都只改了一个参数，234都是debug=2模式截图的」；面积受机位影响的说明见 §14.29.1。）

**判 1（必须如实记的错判）**：**`v0.18.12` 的前景遮挡守卫没治好「人物一圈」** ⇒ 上一轮把白圈归因成
「前景遮挡假命中」是**错的**。守卫保留（它治的是另一类越界），白圈另有成因 ⇒ 见判 2。

**判 2（像素级测量，归因定死）** —— 判据取**与机位无关的局部量**（`Temp\opencode\px_scan.ps1` /
`px_yhist.ps1` / `px_ring2.ps1`，System.Drawing LockBits 24bpp；
**环 = 长度 1..24px 的绿色 run，且紧邻一侧有 ≥25px 的未涂色物体 run**）：

（a）抽行统计（`y = 400..1060 step 10`）：

| 图 | 环命中 | min | **中位数** | max | 主峰 `4px` |
|---|---|---|---|---|---|
| A（fov65/steps32） | 57 | 1 | **4** | 11 | **33** |
| B（fov58.7155） | 65 | 2 | **4** | 9 | **46** |
| C（steps128） | 63 | 1 | **4** | 10 | **36** |

（b）逐行 y 分带（`y = 400..1079`，括号内 = 其中 3..6px 的「环核」）：

| 图 | y400-599 | y600-799 | **y800-999（底部礁石区）** | y1000-1079 | 合计 | **环核占比** |
|---|---|---|---|---|---|---|
| A | 156 (134) | 130 (76) | **246 (212)** | 50 (50) | 582 | **81%** |
| B | 133 (85) | 165 (114) | **257 (251)** | 95 (87) | 650 | **83%** |
| C | 158 (129) | 172 (87) | 219 (173) | 63 (61) | 612 | **74%** |

⇒ **三条结论**：

1. **环宽中位数一律 4px = `ssr.smooth` 的中心差分邻域半径**，且在 fov / steps 三个旋钮下**纹丝不动**
   ⇒ **归因定死：法线差分的邻点跨过了物体剪影** —— 中心是水面（`uWdep` = 461），邻点却踩在人/石头上
   （那里 461 与 520 同源 ⇒ 判不成水），3D 位置突跳 ⇒ 叉出**乱法线** ⇒ 反射方向被甩进屏内 ⇒
   **平白多出一次 hit** ⇒ 采到对岸亮岩 = 默认图那圈**白亮边**（`debug=2` 里同一批像素 = 那圈绿边）。
   这一条同时解释了「只在物体与水交界出现」「宽度恒等于 `ssr.smooth`」「fov/steps 怎么调都不动」。
2. **环不是人物特例**：y 分带里**底部礁石区（y800-999）条数反而最多**，环核 3..6px 在**所有 y 带都占
   74..83%**（y1000+ 高达 97..100%）⇒ 是**所有剪影**的通病。人物那圈被一眼看见，是因为白亮边压在
   深水上；底部那批采到的颜色不亮 ⇒ 「脚贴礁石处看不见圈」是**可见度差异，不是没发生**
   （这条修正了我先前看图时的口头判断）。
3. `wmax` 达 10..20px 的少数长 run（多在 y600-799 = 人物下方那条**合法**竖带、贴岸带的边上）被同一
   判据捞了进来，**不是环** —— 「环核占比 74..83%」就是把这批剥掉之后的比例。

**判 3（面积计数 —— 只当参考）**：三张不是同一存档加载（用户先声明），且 **B 的水面总面积比 A 少
25.5 万 px** ⇒ 面积不可归因，明细见 §14.29.1 表。

**判 4（至今没跑）**：`ssr.debug=1`（直接看法线）、`ssr.fov=58.7155` **不带 debug**（判 §14.29.1 判 4 的
落点预言）、`§14.25.1` 三条隔离试验（`strength=0` / `ripple=0` / `blur=0`）。

> 工具注：`px_scan.ps1` 第 3 段用**中文文件名**读默认图时报错（临时脚本无 BOM ⇒ 中文按 ANSI 读成
> 乱码）—— 踩坑清单「`.ps1` 必须 BOM+CRLF」再次应验；上表全部取自 ASCII 文件名的三张 `debug=2` 图，
> 不受影响。定版度量脚本已入库 `tools/px_ring.ps1`（BOM+CRLF）。

**⇒ 由判 2 落 `v0.18.13`（§14.30）：法线差分轮廓守卫。**

### 14.29 外部评审采纳判断 + SSR v2 路线（`docs/SSR修复建议.md`，2026-10-07）

**来源**：用户投喂 `docs/SSR修复建议.md`（对 v0.18.11 三张实跑图 + 现有代码的外部评审）。它把问题
收敛成三处**结构性**问题：① 命中率低 / 行进太粗（32 步 × 5000 单位 = **156 单位/步**）② 投影矩阵是
「猜」的（`ssr.fov/near/far` 人工填）③ 波纹是从 585 **最终颜色**反推的、不是真水面法线。这三条与本仓
一直挂着的 **R4 相机矩阵来源**、**Hi-Z 范围外**、**第 6 张镜像取真法线**完全同源 ⇒ **采纳为主路线**，
但有四处要按本仓事实校准：

| # | 外部建议 | 采纳 | 判 / 落点 |
|---|---|---|---|
| 1 | 立即 A/B `ssr.steps=128` + `ssr.dist=100`（纯 ini） | ✅ 部分 | **一次只动一个旋钮**：先 `steps=128`（`dist` 不动，隔离「采样密度」）再叠 `dist=100`。`main.cpp:3069` 只拒 `steps<4`、无上限 ⇒ 128 合法。**`dist=100` 有反效果风险**：100×near=1000 单位够不到对岸 ⇒ 远水绿区可能变红，必须配 `ssr.debug=2` 同机位比。**预期要校准**：近水红区是「射线飞出屏顶」的**几何事实**，步数变细不会明显缩小它；步数改的是**绿区的命中精度/块状** —— `v0.18.12` 的二分 3→5 就是同一方向的便宜版。 |
| 2 | 拿真 Projection/View 矩阵，废掉 fov/near/far 猜值 | ✅ **已取证，结论收紧** | 正是 `docs/05` D4 风险表挂着的 **R4**。**已在既有抓帧里量出来（见下「R4 取证结果」）**：`ssr.fov` 猜成 65、真值 **58.7155°**（+13.26% 横向尺度误差）= 真错；`near/far` 猜值只是**同一套几何的缩放单位系**，对 SSR **无害** ⇒ 评审「near/far 也是结构性问题」这条**降级**；aspect 本来就取自 swapchain，对。 |
| 3 | Hi-Z / 深度金字塔 coarse→fine | ✅ P2 | D4 当初明确「不做 Hi-Z」（PoC 范围）；在那之前 `steps`/二分是代偿。 |
| 4 | `thickness` / `bias` / `confidence` 混合 | ◐ 半做 | `v0.18.12` 的 `sz <= P.z * 0.98` 已经是**前景遮挡 + 深度容差**（相对式，免去绝对 `thickness` 的单位问题），治的正是它列的「轮廓 halo」；剩余 distance×edge×depth×normal 淡化留 v2。 |
| 5 | 用真水面法线替代「从 585 颜色猜波纹」 | ✅ 已在 backlog | 就是 `§14.27` 记的「第 6 张镜像（582 R8G8 / 349 R16G16）取真法线」；**这才是「倒影区域水波较小」的根治**，`v0.18.12` 的幅度缩放只是止血。 |
| 6 | 「`edge=0` 是在主动隐藏 miss」 | ◐ 半采纳 | 结构上对，但**这是正确的默认行为**：屏幕外的镜像本来就该交给 cubemap（本仓设计原则「看得见的用 SSR，看不见的交给 cubemap」）。判据不是「把红区清零」，而是**能命中的区域要命中得准、接得自然**。 |
| 7 | 「别再做 `v0.18.12` 式小修」 | ◐ 半采纳 | `v0.18.12` 已落地（三处都是用户**已报症状**的最小修复，且 `ripplesz=4` 默认逐位一致）；但**接受「不再往 edge/smooth/blur 上加小修」** —— 下一个改动要么是 ini 试验，要么进 v2 阶段。 |

**R4 取证现状（2026-10-07，本机对既有抓帧跑，零游戏）**：

- `tools/rdc_pass6.py:491` 每张常量缓冲只解包 `struct.unpack("<16f", …64)` ⇒ `cb_head.head16f`
  **只有一整个 4×4（64 字节）的窗口**。
- 扫 `docs/analysis/S1~S5*-extract-pass6.json` 里所有非空 `head16f`：**18 个矩阵，13 个非单位**，
  全部是 `ResourceId::1463` 的**旋转矩阵**（正交、末行 `0,0,0,1`，如 S4 `ev39270` 首个段16 draw =
  `[0.979 0.204 0 0 | -0.049 0.236 0.971 0 | -0.198 0.950 -0.241 0 | 0 0 0 1]`），
  **没有投影矩阵**（投影矩阵的特征：第 4 行/列有 `0,0,±1,0` 结构 + `m00 ≠ m11` + `m32 = −near·far/(far−near)`）。
- ⇒ **矩阵不在前 64 字节里** ⇒ **新增 `tools/rdc_cbuf_scan.py`**（窗口放到 512 B + 按投影矩阵特征
  自动反解 fov/aspect/near/far），对既有 `captures\S4-water.rdc` 重跑
  （`powershell tools\rdc_run.ps1 -Script rdc_cbuf_scan.py -Scene S4 -Stride 25`，**4.4s / 514 个
  缓冲块 / 0 错**）⇒ **取证成功**，落 `docs/analysis/S4-cbuf-scan.json`。
  （我们**不需要 view 矩阵**：行进全程在视图空间做，只要 `viewZ/viewPos/projectUV` 的投影口径对。）

**R4 取证结果（2026-10-07，主相机 = `ResourceId::1463` 的第 2 个 4×4，135/149 命中完全一致）**：

| 量 | 抓帧实测 | ini 猜值 | 影响判读 |
|---|---|---|---|
| **垂直 FOV** | **58.7155°**（= `2·atan(0.5625)`；水平 = **90.000°**，16:9 自洽） | `ssr.fov=65` | **`tan(fov/2)`: 0.6371 vs 0.5625 ⇒ 横向尺度比深度大 +13.26%** ⇒ 倒影位置/比例偏、倾斜面法线错、**射线投影外扩更快 ⇒ 更早出屏 = 红区（未命中）偏大** |
| aspect | `1.777778` = 1920/1080 | 取自 swapchain | ✓ 本来就对 |
| near | **15.0** | `ssr.near=10` | **无害**：`B = A·n` ⇒ `(10,100000)` 与 `(15,353467)` 是**同一套几何整体缩放 0.6667 的单位系**，`viewPos / projectUV / 命中判定 / 近平面截断` 全部尺度不变（实测非均匀度仅 **0.05%**），只有 `ssr.dist` 的**单位含义**跟着变 |
| far | **≈353,467** | `ssr.far=100000` | 同上无害（且由 `c−1 ≈ 4.2e-5` 反解，本身精度有限，不值得追） |

原始矩阵（`ResourceId::1463`，512B 窗口；offset 0 = view（纯旋转、末行 `0,0,0,1`），offset 64 = proj）：

```
1 0 0 0  |  0 1.777778 0 0  |  0 0 1.000042 -15.000636  |  0 0 1 0
```

⇒ **`ssr.fov` 是唯一真正猜错的旋钮**；评审把 near/far 也列成结构性问题这条，按上表**降级为无害**。
（另一份 14/149 的矩阵 `m00 = m11 = 1.0`、`near = 35.384` 是别的通道，aspect=1，不是主相机。）

**零代码、可证伪的 A/B（优先级最高，一次只动一个旋钮）**：

1. **`ssr.fov=58.7155`**（其余全不动）⇒ 两条预言：**① `ssr.debug=2` 的红区应明显缩小**（射线不再
   过早出屏）；**② 倒影应往它本来的位置收**（对岸/船该在自己正下方）。**任一条没发生 ⇒ 记录，
   先别改默认值。**
2. 对照组 **`ssr.steps=128`**（fov 不动，评审第 1 条）⇒ 同机位 `ssr.debug=2` 看红区缩不缩。
   两次各测一遍即可分清「红区大」到底是**投影口径**还是**步进密度**造成的（评审归因于步进，
   本仓取证指向 fov —— 两个假设互斥可验）。
3. 两条都有效 ⇒ 再叠 `fov=58.7155 + steps=128`，据结果定 `v0.18.13` 的默认 `ssr.fov`，
   并把「读真矩阵上 hook」（P1）从 backlog 提上来。

**当下就能跑的（零代码，全部现有键，按优先级）**：
1. **`ssr.fov=58.7155`**（见上 A/B 第 1 条，两条可证伪预言）—— 这是取证量出来的真值，**先测它**。
2. `ssr.steps=128`（`dist` 不动）→ 同机位 `ssr.debug=2` + 正常画面；看红区是否更稳、帧时是否还在
   16.85 的 5% 闸内，超闸降 64（作为 A/B 第 2 条，与 fov 互斥可验）。
3. 叠 `ssr.dist=100` → 看远水绿区有没有变红（够不到对岸）。
4. `§14.28` 的其余键（`ripplemode` / `ripplesz` / `blur` / `debug=7`）与 `§14.25.1` 三条隔离试验。

**v2 阶段划分**（判读各开 `§14.29.x`）：
1. **P1 真投影矩阵（R4）**：改 dump 窗口 → 抓帧反解 `fov/near/far` → ini A/B → 上 hook 逐帧读。
2. **P2 行进器**：Hi-Z 深度金字塔 coarse → fine → binary（过渡版 = 指数步进 + 段内细分）。
3. **P3 confidence**：distance × edge × depth × normal 淡化，与 585 平滑混合（现有 `edge=0` 是它的 0/1 版）。
4. **P4 真水法线**：第 6 张镜像取 Skyrim water normal → `reflect(V, Nwater)`，根治「水波小」。

#### 14.29.1 R4 A/B 首轮结果（2026-10-07，`v0.18.12` 的三张 `debug=2` 实跑图 + `ssr.fov` 相消推导）

三张图只各动一个旋钮（用户原话「3和4都只改了一个参数，234都是debug=2模式截图的」），像素计数
（`Temp\opencode\px_scan.ps1`，System.Drawing LockBits 24bpp，绿 = `G>170 且 R<130 且 B<130`，
红 = `R>170 且 G<130 且 B<130`，全屏 1920×1080）：

| 组 | 旋钮 | 绿 px | 绿占比 | 红 px | 红占比 | 水面(g+r) | 水面占屏 | **绿/水面** | 目测绿带下沿 |
|---|---|---|---|---|---|---|---|---|---|
| A | 基线 `fov=65` / `steps=32` | 202,543 | 9.77% | 1,022,674 | 49.32% | 1,225,217 | 59.1% | **16.5%** | ≈y470 |
| B | **只动** `fov=58.7155` | 310,055 | 14.95% | 660,358 | 31.85% | 970,413 | 46.8% | **32.0%** | ≈y410 |
| C | **只动** `steps=128` | 244,613 | 11.80% | 936,925 | 45.18% | 1,181,538 | 57.0% | **20.7%** | ≈y500 |

**判 1（否定性结论，必须先记）：三张不可归因。** 用户先声明「不是通一个存档加载的，肯定有误差」；
像素侧也佐证了机位确实变了 —— **B 的水面总面积比 A 少 25.5 万 px（占屏 46.8% vs 59.1%）**，
且**目测（B 绿带更靠上更细）与计数（B 绿更多红更少）互相矛盾**。⇒ §14.29 A/B 的**预言①（红区缩小）
与「steps ⇒ 红区缩小」两条都判不了**，只能记方向。

**判 2（方向一致，无反证，但未定案）**：两组都是「命中变多」（绿/水面 16.5% → 32.0% / 20.7%），
与两个假设**同向** ⇒ 两条 A/B **都保留**，复测方法见下。

**判 3（重要更正：`ssr.fov` 是规范自由度，不改倒影落点）** —— 写上面 R4 表时只看了
`tan(fov/2)` 的比值就写「倒影位置/比例偏、倾斜面法线错」，**这句是错的**，补做相消核对后更正：

- `viewPos`（`ssr.frag:133`）与 `projectUV`（`ssr.frag:158`）用的是**同一套** `pc.p0.x` / `pc.p0.x*pc.p0.y`，
  而 `viewZ` 只吃 `ssr.near/far`（**与 fov 无关**）。设 `k = tan(fov猜)/tan(fov真) = 1.1326`，则
  反推出来的视图点 = `(k·x, k·y, z)` = `S·(真点)`，`S = diag(k,k,1)`。
- 镜像与 `S` **可交换**（`S·M_Π = M_{S(Π)}·S`），投影又恰好是 `project_真 ∘ S⁻¹`
  ⇒ **数值核对**：真点 `(10,2,-300)` 关于水面 `y=-5` 的镜像 `(10,-12,-300)`，
  在 `fov=65` 重建空间里算出的 UV = `0.51667 / 0.53556`，与真 fov 算出的 **完全相同**；
  反射方向 `reflect(S·v, ŷ) = S·reflect(v, ŷ)`（`S` 对 y 只缩放、不换向）同样严格相等。
- ⇒ **倒影位置/比例/法线都不受 `ssr.fov` 影响**（法线由差分重建，随空间一起协变）。fov 真正影响的
  只有**行进的参数化**：一步在我们的重建空间里覆盖 `1/k` 的真实横向距离，`ssr.dist` 的横向有效射程
  也 ÷k ⇒ **`fov=65` 相当于横向射程与横向步进密度各打 88.3% 折** ⇒ **只影响命中覆盖率，不影响落点**。
  （这恰好解释了 A<B 的方向；也解释了为什么「倒影看着是对的」却仍然 65 vs 58.7 差这么多。）

**判 4（可证伪，下次同机位直接验）**：`ssr.fov=58.7155` 的**正常画面里，倒影位置/比例应纹丝不动，
只有绿区覆盖率变**。若倒影真的动了 ⇒ **推翻判 3**，那时才按原 R4 表那句处理。

**`ssr.fov` 默认值决策：先不改。** ① 判 3 说它不改落点 ⇒ 改它的收益只剩「命中覆盖 +13.3% 横向射程」，
而这可以先用 `ssr.dist` / `ssr.steps` 调；② 还没有同机位证据；③ `v0.18.13` 只动一个旋钮
（法线轮廓守卫），不叠加 fov。⇒ **改默认值要等复测，且与 P1「读真矩阵上 hook」分开算收益**
（P1 的价值在一致性/可维护，不在画面位置）。

**复测方法（下次跑图的硬要求）**：同一个存档读档 → 站到同一块礁石 → 同一视角 → **只改一个旋钮** →
每组两张（`ssr.debug=2` + 正常画面）。判据优先级：**正常画面的倒影位置/比例（预言②） >
`debug=2` 的面积比**。`ssr.steps=128` 那组另记帧时（16.85 的 5% 闸）。

### 14.30 v0.18.13 判读模板（**法线差分轮廓守卫：水面法线只由「同样是水面」的邻点差出**，代码批待 CI / 待实跑）

**一句话**：`v0.18.12` 的前景遮挡守卫**没治好「人物一圈」**（早先归因错，见 §14.28.1）⇒ 像素级测量把
成因定死在**法线差分半径跨过物体剪影**（三张图环宽中位数一律 **4px = `ssr.smooth`**）⇒ 这一版
**只加一个轮廓守卫**：水面法线的 4 个差分邻点逐个过 `nbWater`，越界那一侧退成单侧差分；
**命中判定 / 二分 / 回注 / push constant / 通路时序一条不改**，**没有新 ini 键**。

**关键设计约束（改代码前必须记住的六条）**：

1. **为什么不用「深度差阈值」**：前景礁石只比水面高几十厘米、深度几乎一样，任何阈值都抓不住；而 461
   （段后水深）与 520（段16 前）在**非水面**像素上同源同内容 ⇒ `viewZ(461) > viewZ(520)` 在
   人 / 石头 / 岸上一律不成立。这**就是 `useW` 自己那条已经在跑的判据**（`ssr.frag:181`，
   `ssr.debug=5` 的判定图语义同它）⇒ **不引入任何新假设、零参数、零调优**。
2. **单侧越界 → 单侧差分**：水面是平的，单侧差分照样给出正确平面法线 ⇒ 这圈像素会和周围一样 miss
   ⇒ 亮边自然消失；**两侧都越界** → 落到原有退化兜底（叉积为 0 ⇒ `N = 视图+Z` ⇒ 射线回头穿回近平面
   ⇒ miss ⇒ `ssr.edge=0` 回原版层）——兜底链本来就通，不用新写。
3. **邻点全是水面时逐位一致**：`okL..okD` 全真 ⇒ `vx = Pr-Pl`、`vy = Pd-Pu`、`cx` 与 `v0.18.12`
   **完全相同** ⇒ **只有剪影旁那几排像素会变**，其余画面不动（这是它敢不开键的底气）。
4. **只对 `useW` 生效**：`bool okL = !useW || nbWater(...)` —— `!useW` 短路 ⇒ 非水面像素连 `nbWater`
   都不调，**逐位不变**；树/石头/天空的输出本来只在段17 水面网格里可见，不构成症状。
5. **代价**：水面像素 +8 次纹理取样（4 邻点各取 `uWdep` + `uDepth`，`PAt` 已取的那份不省），
   对比 32..128 步行进可忽略 ⇒ 帧时仍按 16.85 的 5% 闸看。
6. **`strength=0` 仍逐字节原版**（守卫在法线重建处，原版兜底仍排在它后面）。

**代码在哪**：

1. `shaders/ssr.frag`：新增 `nbWater(vec2)`（`PAt` 之后，`viewZ` 之后定义 ✓）；法线差分处改
   `okL..okD` + `vx/vy` 单侧回退 + `cx` 重算；文件头归因段补 v0.18.13。
2. `main.cpp`：**banner = `v0.18.13`**，**追加** `+ v0.18.13:` 历史条目（`+ v0.18.12:` 那条**保留**）。
3. `tools/`：`check1.ps1` `$need` 版本串 → `v0.18.13`；`run2c.ps1` **8 处** → `expect v0.18.13`
   （字节级等长替换 ⇒ BOM 与行尾逐项未变：`run2c` BOM+CRLF×547、`check1` BOM+LF×113、`main.cpp` 无 BOM+纯 LF）。
4. 三闸：`make_shaders.ps1` OK（**`ssr.frag.spv = 27708 B`**，原 24560）、`check1.ps1` **RESULT OK**、
   引号扫描 **`oddQuoteLines=0`**（887 行新增）。

**跑法（没有新键，只调已有的）**：

| 键 | 值 | 含义 |
|---|---|---|
| `ssr.steps` / `ssr.debug` | **先回 `32` / `0`** | ini 里残留着上一轮的 `128` / `2`，**每轮只动一个旋钮，先归位** |
| `ssr.debug` | `2` | **关键验证**：人物/礁石外圈那道 **4px 绿边应消失** |
| `ssr.debug` | `1` | 佐证：剪影附近的法线应不再乱偏（水面一片平滑渐变） |
| `ssr.smooth` | `4`（不动） | 守卫落地后就不该再靠调它来躲环；若改 `1` 能压环 ⇒ 说明守卫没生效 |

基线沿用 §14.28（`ssr.edge=0` / `ssr.smooth=4` / `ssr.blur=1` / `ssr.base585=1` / `ssr.wdep=1` /
`ssr.ripple=1` / `ssr.ripplemode=1` / `ssr.ripplesz=4` / 五行全开 / `ssr.v1=1` / `ssr.mode=1 / fov=65 /
near=10 / far=100000 / steps=32 / dist=500 / rev=0`）。换 artifact DLL → 实机 **≥2000 帧** → 自查
`SSR侦察: ctx槽33/50/47 已挂` → `tools\run2c.ps1`。**ini 启动只读一次 ⇒ 改完必须重启游戏。**

**判读表（`run2c.ps1`）**：

| 判据 | 期望 | 判 |
|---|---|---|
| #0 banner | `expect v0.18.13: OK`（否则 DLL 没换） | — |
| `[v1] SSR v1 就绪` 回显 | 含 ` edge=`（同 §14.28） | 回归对照 |
| §#16 `[wdep]` 五组、`[base]` 三行 | 同 §14.24 / §14.23 | **回归对照** |
| §#12/#13/#14/#15/12e/2c-β/哨兵/`[异常]` | 全绿 | 回归对照 |
| 帧时基线 | 16.85 的 5% 闸（+8 次取样，预期仍在闸内） | 回归对照 |

**画面（真正判据 = 人眼）——按顺序走，每步只动一个旋钮**：

1. **`ssr.debug=2`** ⇒ **人物头/肩/上臂外侧那圈 4px 绿边应没了**（本轮唯一新判据）；贴岸带、
   人物正下方竖带、红绿分界、三张 A/B 的覆盖率应与 `v0.18.12` **基本一致**（守卫只砍剪影那几排）。
2. **默认（`debug=0`）** ⇒ 人物外侧那圈**白色亮边应消失**（同一现象的正常模式表现）。
3. **`ssr.debug=1`** ⇒ 剪影附近的法线不再乱偏；若还乱 ⇒ 转第 4 条。
4. **绿边仍在** ⇒ 归因被推翻 ⇒ 记下别硬改，转查 **`ssr.debug=4`（段后水深灰度）**看剪影处的 `uWdep`
   是否被采样过滤污染（461/520 若非同格式 ⇒ `viewZ(a) > viewZ(b)` 可能因精度差假成立 ⇒ 守卫失效）。
5. 其余旋钮（`fov=58.7155` / `steps=128` / `ripplesz` / `ripplemode` / `blur`）与 §14.28 第 3..9 条、
   §14.29.1 复测清单**照旧，这轮不叠加**；`§14.25.1` 三条隔离试验（`strength=0` / `ripple=0` /
   `blur=0`）**至今仍未跑**。
6. 倒影位置/比例仍按 §14.22 的 R4 表 + §14.29.1 判 4 的预言（**fov 改动应不移动落点**）。

**这一轮的降级线（任一步只关自己）**：轮廓守卫**没有 ini 键**，真回退就改 `ssr.frag` 删 `okL..okD`
那段重编（等价于回到 `v0.18.12` 的法线路径）；其余降级线全部同 §14.28（`ssr.edge=1` 退 `v0.18.10`、
`ssr.ripple=0`、`ssr.ripplemode=1`、`ssr.strength=0`、`ssr.base585=0`、`ssr.wdep=0` 退 `v0.18.7`、
`ssr.mode=0`、总闸 `ssr=0` / `ssr.vkout=0` / `ssr.v1=0`）。**这版不改任何通路时序**（入向 2c / 2c-β /
2d / §#15 / 12e / desc / 哨兵 / 底色 / 水深 全当回归对照）；判读入档补 §14.30.1。

#### 14.30.1 实跑记录（2026-10-07，`v0.18.13` = `52ca006` 代码批 + `4609c86` docs 批，CI SUCCESS 双 job）

**用户原话**：「ssr.debug=0已经跑完了，人物周身确实没有一圈白亮边了，其他没变化，倒影质量比较差，
倒影区域水波纹比较小。」

| 判据 | 期望 | 实测 | 判 |
|---|---|---|---|
| 人物/礁石外圈**白亮边**（默认画面） | 消失 | **消失** | ✅ **归因坐实** |
| 人物外圈 **4px 绿边**（`debug=2`） | 消失 | 未单拍（默认画面的白边 = 同一批像素的 hit 表现，已足够） | 待补 |
| 其余画面 | 与 `v0.18.12` 基本一致 | **其他没变化** | ✅ 「邻点全水面时逐位一致」的预期成立 |
| 帧时 5% 闸 | 16.85 ± 5% | 未报 | 待补 |

⇒ **`v0.18.13` 的轮廓守卫通过**：§14.28.1 判 2 那条「**法线差分邻点跨剪影**」的归因**成立**，
`nbWater` + 单侧差分没有引入可见副作用（否则画面会变）。**§14.28 判 1 那条旧归因
（前景遮挡假命中）到此正式作废** —— 守卫本身保留，但它治的不是这个症状。

**同轮遗留两项（用户反馈）**：① 倒影质量比较差；② 倒影区域水波纹比较小。
二者**不是 `v0.18.13` 的回归**（这版没动行进、没动回注），对应 §14.29 的 **P2（行进器）** 与
**P4（真水法线）** 方向；其中 ② 的机制分析、可证伪点与试验清单单列 **§14.30.2**。

#### 14.30.2 波纹偏小的归因 + 「要不要区分流动水/静水」的判断（2026-10-07，用户提问）

**先把机制摆正**：原版波纹 = 段16 那 16 个 draw 按**水面涟漪法线**采 cubemap 写 585 ⇒ 扰动的是
**反射方向（角量）**。我们换掉 585 之后靠**回注**把它补回来，而回注是从 585 的**亮度结构**反推位移
（`ssr.frag:396` `ripd = clamp((g1-g2)*10, -1, 1) * (ripAmp*px) * ripA`）—— 这是**近似**，不是法线本身。
于是「偏小」有三个具体上限：

| # | 上限 | 代码 | 后果 |
|---|---|---|---|
| 1 | **幅度天花板** | `ripAmp = clamp(Rb*1.5, 6, 16)`（`ssr.frag:395`） | 默认 `ripplesz=4` ⇒ **6px 封顶**、`ripplesz=16` 也只到 16px。原版是角量：法线倾角 θ ⇒ 反射方向偏 2θ ⇒ 屏上位移 ≈ 2θ × 焦距；90° hFOV / 1920px ⇒ **21.3px/度**，θ=1° 就约 43px ⇒ **与 6px 不是一个量级**（此为**推导**，须由 `debug=7` 实测复核） |
| 2 | **波长档位** | `g1-g2` = 「1px 梯度 − Rb 梯度」带通 | **原写「只对波长 ≈ 2·`ripplesz` 最有效」已由 §14.30.3 更正：λ ≈ 4·Rb 才是最大响应，λ ≈ 2·Rb 反而是 null**；档位失配 ⇒ 信号变弱 ⇒ 观感更小 |
| 3 | **5tap 平滑** | `ssr.blur=1`，半径 = `ssr.smooth` = 4px | 相邻像素位移被平均 ⇒ 更平、更"糊" |

**另有一个结构性问题**：`ripplesz` **一旋钮两用**（既当带通窗口、又当幅度 `1.5·Rb`）⇒
**没法只提幅度不改档位**。而幅度本该由**波的陡度**定、不是由**波长**定 —— `v0.18.12` 把两者绑死是
基于「波长越大幅度越大」的经验假设，**这条假设本身待验**（⇒ 候选改动 `v0.18.14`：把 `ripAmp` 与 `Rb`
解耦成独立键，**先跑完下面的实测再决定要不要动代码**）。

**「是不是水在流动的原因？要不要区分流动/静水？」——分三层回答**：

1. ❌ **不是「流动导致信号丢失」**。回注是**逐帧读当前 `uBase`（585 段16 后）**，没有冻结、没有缓存、
   没有时间平均 ⇒ 流水的波一样被实时读进来。流动只让波纹**移动**，不让我们**读不到**。
2. ⚠️ **但你的直觉对着一个真问题**：流水的波通常**更长 + 有方向**，而我们的带通是**各向同性单标量**
   `ripplesz`（x/y 共用同一个 `Rb`）⇒ 方向性波在某一轴上失配 ⇒ 响应被削弱 ⇒ **看起来更小**。
   所以「流动」与「波纹小」之间**有相关性**，但机制是**波长/方向失配**，不是「水在动」这件事本身。
3. ✅ **因此不需要按「流/不流」分两套逻辑**：法线是同一个物理量，流水与静水的区别只在**波长与波向分布**。
   要做的是**按波长（乃至方向）自适应** —— 而这正好是 **P4（真水法线，第 6 张镜像 582 R8G8 / 349
   R16G16）** 顺带解决的：拿到法线就直接 `reflect(V, Nwater)`，**波长、方向、幅度全部由法线自带**，
   流水静水自动都对，回注这条 hack 可以退成 0 —— 这也正是外部评审第 5 条与本仓 backlog 同源的那条。

**试验清单（一次一个旋钮，按信息量排序）**：

| # | 键 | 判什么 |
|---|---|---|
| 1 | ~~`ssr.debug=7`~~ | ✅ **已跑（2026-10-07）** ⇒ 结果与**规则更正**见 §14.30.3：**λ ≈ 30px**、位移直估法作废、`λ/2` 是错的 |
| 2 | **`ssr.ripplesz=8`** | §14.30.3：λ=30 ⇒ **λ/4 = 7.5 ⇒ 8 是对的档**，且天花板 6→12px ⇒ 位移 ≈2×；唯一要盯的副作用 = 低频泄漏 ×2.3（山/天轮廓被印进水里） |
| 3 | `ssr.ripplemode=0` | 亮度那一半：命中区 sparkle 实测 **−32%**（§14.30.3），看回不回得来 |
| 4 | `ssr.blur=0` | 第 3 条上限（5tap 糊掉的）—— 若波纹立刻变清楚 ⇒ 主因是它 |
| 5 | `ssr.debug=6` | 位移归一化场：大面积偏亮 ⇒ 「天花板在咬」坐实 |

**倒影质量差（同轮另一项）**：行进 32 步 × 5000 单位 = **156 单位/步** + 二分 5 次 ⇒ 远处命中精度
≈ 4.9 单位（约 1.6px，尚可）、近处可达 ~9px（可见块状）；再叠 `ssr.blur=1` 的 4px 半径 ⇒ 观感偏糊。
**旋钮**：`ssr.blur=0`（锐度）、`ssr.steps=64/128`（步进密度，A/B 已见命中变多）；**根治 = P2 Hi-Z**。
（另注：`ssr.fov=58.7155` 按 §14.29.1 判 3 只影响**横向步进密度与射程 ÷k=1.1326**，不改落点 ⇒
它对「质量」的贡献是覆盖率而非清晰度，别指望它治糊。）

#### 14.30.3 `ssr.debug=7` 实测（2026-10-07）：原版波纹波长 **λ ≈ 30px**，`ripplesz` 该取 **λ/4 不是 λ/2**

**输入**：`docs/analysis/Screenshot_ssr.debug=7.png`（原版层原样）× `Screenshot_v0.18.12 默认.png`（我方输出）；
工具 `tools/px_ripple.ps1`（ASCII-only 无需 BOM；CRLF=183 / UTF8 strict OK）。

**方法上踩了两个坑，都修了（留档免得再踩）**：
1. **必须先带限再 ACF**：白色高光斑只有 2px，方差盖过波纹 ⇒ 直接 ACF 看不到周期。
   `BAND = MA(41) − MA(9)` 只留 9..41px 后曲线才干净。
2. **ACF 分母 bug**：第一版拿**方差总和** `v = Σ(x−m)²` 当分母、而分子是均值 ⇒ 整条曲线被压成 `1/n`
   （≈0.001）⇒ 表现为「`lambda: NONE`」。分母必须是**均方** `v/n`。修完立刻出形状。
3. **弃用一个量**：位移直估 `d = −HP/梯度` 在两图给出几乎相同的 8.58 / 8.76px（p75/p90 也重合）
   ⇒ 测的是高光斑噪声、不是位移 ⇒ **该方法作废**，别再用。

**实测 1 — 波长**（4 条曲线：横/竖 × 2 图 × 2 框，均为 55..161 条扫描的**平均 ACF**）：

| 曲线 | 过零 | **谷** | 回零 | 正峰 |
|---|---|---|---|---|
| ORIG boxR 竖 | lag7 | **lag15（−0.455）** | lag24 | lag28（+0.087） |
| ORIG boxR 横 | lag7 | **lag15（−0.530）** | lag25 | lag30（+0.090） |
| ORIG boxL 竖 | lag7 | **lag15（−0.464）** | lag24 | lag28（+0.107） |
| ORIG boxL 横 | lag7 | **lag15（−0.497）** | lag25 | lag30（+0.082） |
| SSR boxR 竖/横 | lag7/8 | lag15/16（−0.453/−0.477） | lag26/27 | lag30（+0.022/+0.053） |
| SSR boxL 竖/横 | lag7 | lag15/16（−0.460/−0.474） | lag25 | lag29/32（+0.053/+0.042） |

⇒ **λ ≈ 28~32 px**（正弦 ACF：过零 = λ/4、**谷 = λ/2**、正峰 = λ，三个刻度互洽）。四条原版曲线完全重合。

**判读 1（关键）**：**原版单独就定死了 λ≈30**（boxR 是暗水+高光，那个尺度上唯一结构就是波纹）；
我方输出的谷/正峰也落在同一处 ⇒ 位移场**确实跟着波走、周期没错** ⇒ 问题在**幅度**与**亮度**，
**不是「档位频率跟不对」**。

**实测 2 — 带通增益与天花板**：`kx = px*Rb`（`ssr.frag:363/384`）⇒ g2 基线 = `2·Rb`；对波长 λ 的正弦波
`|g1−g2| = 2·|sin(2π/λ) − sin(2πRb/λ)| · A`（g1、g2 同相，直接相减）。实测幅度 A：
`bandRMS(9..41px)` = **15.6 luma = 0.061 归一** ⇒ 正弦幅度 A ≈ 0.087。

| `ripplesz`(Rb) | 相对 λ=30 | `2|sin−sin|` | `(g1−g2)·10` | 位移（`ripAmp=clamp(1.5Rb,6,16)`） |
|---|---|---|---|---|
| **4（现值）** | λ/7.5 | 1.070 | **0.93**（波向斜时各分量 ≈0.65） | **≈4~6px ⇒ 天花板在咬** |
| **8** | λ/3.75 | 1.573 | 1.35 ⇒ clamp 到 1 | **8~12px ⇒ 约 2×** |
| 14 | **λ/2 − 1** | **0.000（null）** | 0 | **无位移** |
| 15 | λ/2 | 0.416 | 0.36 | ≈5.8px |

⇒ **两个硬结论**：
1. **6px 天花板确实在咬**（信号 0.65~0.93，已接近 `clamp(±1)`）⇒ 抬 `ripplesz` 能**直接**放大位移。
2. **更正设计注释（`ssr.frag:379`）与 §14.28 第 6 条、§14.30.2 的「`ripplesz ≈ 波长/2`」是错的**：
   λ/2 恰好落在响应的 **null**（`sin(2πRb/λ) = sin(2π/λ) ⇒ Rb ≈ λ/2 − 1`）上 —— **照它调会把波纹调没**。
   正确档是 **λ/4（同相瓣）**；另有一个更大的反相瓣在 3λ/4（本例 =22，超出 `1..16` 上限，
   且低频泄漏 ×7），不可用。
   （`ssr.frag:379` 那行注释留到 **v0.18.14** 一并改 —— 不为纯注释重编 `.spv`；`§14.28` 第 6 条同批回改。）

**副作用预估**：低频泄漏（把山/天轮廓印进水里）∝ `(Rb−1)/λ` ⇒ Rb 4→8 ⇒ **约 ×2.3** ——
这是 `ripplesz=8` 唯一要盯的副作用。

**实测 3 — sparkle 在命中区丢了三分之一**（同一屏幕框，两图对比）：

| 框 | 我方框内内容 | 原版 sparkle | 我方 sparkle | 差 |
|---|---|---|---|---|
| boxR `x1050-1850 y430-700` | **命中**区（镜面倒影） | 1.45% | **0.98%** | **−32%** |
| boxL `x60-420 y470-900` | **未命中**区（贴原版层） | 1.44% | 1.49% | 持平 ✓ |

⇒ **命中区确实丢了约 1/3 的白色高光斑**，未命中区因原样贴原版而持平 ⇒ 这是 `ssr.ripplemode=0`
（亮度那一半）值得单跑一轮的**量化依据**。（`bandRMS` 两图都是 15~16.5，框里混了倒影内容自身纹理，
**不能**用来判幅度，故不列。）

**下一步（一次一个旋钮，全部是可证伪预言）**：

| # | 键 | 预言 |
|---|---|---|
| 1 | **`ssr.ripplesz=8`** | 位移幅度 **≈2×**（4~6px → 8~12px），波纹肉眼明显变大；**若同时出现山/天轮廓被印进水里** ⇒ 泄漏超预期，退回 4，改走 v0.18.14（`ripAmp` 与 `Rb` 解耦 + 独立增益）。若**没变化** ⇒ 说明 A 被高估、信号其实远没到天花板，改查 `ripA`/`blur` |
| 2 | `ssr.ripplemode=0` | 命中区白色 sparkle 回来（对照 boxR 的 −32%）；若反过来把岸/山轮廓印上来 ⇒ 退回 `1` |
| 3 | `ssr.debug=6` | 位移归一化场；按上表**应大面积偏亮**（0.65~0.93）⇒ 坐实「天花板在咬」；若反而偏暗 ⇒ 信号没到天花板，抬 `ripplesz` 的收益来自档位而非幅度 |

#### 14.30.4 `ssr.ripplesz=8` 实跑（2026-10-07）：**带通根本没带通** —— 位移场被白色高光斑劫持

**用户原话**：「倒影处的波纹还是明显的比无倒影的小，而且密集许多，倒影质量倒是还行，就是视角拉动的时候
倒影变化的太快了。」**输入**：`docs/analysis/Screenshot_ssr.ripplesz=8.png`；**工具**：
`tools/px_ripple.ps1` 本轮加两列（`FINE(3-21px)` 高光斑尺度 / `WAVE(21-61px)` 波纹尺度）+ 自动挂第三张图。

**⚠ 先声明比较有效性（同 §14.29.1 A/B 那次的坑，这次又差点踩）**：

| 图 | boxR 均值 | boxL 均值 |
|---|---|---|
| `debug=7`（原版层） | 79.1 | 64.3 |
| `v0.18.12 默认` | 80.4 | 67.7 |
| **`ripplesz=8`（本张）** | **103.5** | **90.0** |

⇒ 三张**不是同一机位** ⇒ 跨图的 FINE/WAVE/sparkle **一律不可比**。**只有同帧内「命中区 vs 未命中区」有效**：

| 同帧区域（本张） | 内容 | sparkle | FINE(3-21) | WAVE(21-61) | fine/wave |
|---|---|---|---|---|---|
| boxR `x1050-1850 y430-700` | **命中**（镜面倒影） | **0.21%**（115 个、均长 1.3px） | 11.27 | 9.72 | 1.16 |
| boxL `x60-420 y470-900` | 未命中（贴原版层） | 1.03%（400 个、均长 1.6px） | 15.22 | 11.70 | 1.30 |

（⚠ 命中区背景亮 mean 103 vs 90 ⇒ 对比度型 sparkle 检测会**低估**，0.21% 只当方向性证据；
FINE/WAVE **更不能**反推位移场 —— 命中区是「平滑内容被位移调制」、未命中区是「高光斑自身亮度」，
是两种东西。**位移场本身唯一可信的量测是 `ssr.debug=6`**，见下。）

**归因（代码层面：排除法 + 响应曲线，不靠图上目测）**

1. **倒影里的波纹 100% 来自 `ripd`**：`ssr.ripple=0 ⇒ ripd=0` 时反射就是干净镜面；现在有波纹 ⇒ 全是回注画的。
2. **回注取的 `uBase` 在开阔水域 = 暗水 + 密集 2px 白色高光斑**（`debug=7` 图里 boxR 就是这个），
   λ30 那一档只占其中一部分能量。
3. **`g1 − g2` 不是带通，是「细结构高通」**：`g1` 基线 2px、`g2` 基线 2Rb。对尺度 ≪ Rb 的**孤立**结构
   （高光斑），`g2` 的两个采样根本采不到 ⇒ `g2 ≈ 0` ⇒ `g1 − g2 ≈ g1` ⇒ **细结构原样通过，
   而且对 `ripplesz` 完全不敏感**（Rb=4 与 Rb=8 都采不到）。
4. **冲激响应 = 4 根单位幅值尖峰**：算子展开 `f(x+1) − f(x−1) − f(x+Rb) + f(x−Rb)`
   ⇒ 一个孤立高光斑（幅度 A）在 `x = ∓1` 与 `x = ±Rb` 各打一根 **±A**。
   `A ≈ 0.7`（归一亮度 2px 内 0.235→0.95）⇒ `(g1−g2)·10 ≈ 7` ⇒ **顶死 `clamp(±1)`
   ⇒ 每个高光斑位置都吃满 `ripAmp`**。
5. **正弦响应表**（`2|sin(2π/T) − sin(2πRb/T)|`，以 λ=30 的波为基准）：

| 尺度 T | Rb=4 | Rb=8 | Rb=8 相对波 |
|---|---|---|---|
| 3px | 0.00 | **3.46** | **2.20×** |
| 4px | 2.00 | 2.00 | 1.27× |
| **5px** | **3.80** | **3.08** | **1.96×** |
| 6px | 3.46 | 0.00 | — |
| 7px | 2.43 | 0.00 | — |
| 8px | 1.41 | 1.41 | 0.90× |
| **10px** | 0.00 | **3.08** | **1.96×** |
| 15px | 1.18 | 1.23 | 0.78× |
| 20px | 1.28 | 0.56 | 0.35× |
| **30px（波）** | **1.07** | **1.57** | 1.00× |
| 40px | 0.86 | 1.59 | 1.01× |

⇒ **3/4/5/10px 四档全部 ≥ 2.0，是 λ30 波的 1.27~2.20 倍**（Rb=4 时 5px 更是 **3.55×**）；
曲线在 T 上**剧烈振荡**（6/7px 又掉到 0）—— 这是「两根梳齿相减」的指纹：**既不压高频、也不压低频**，
只在特定 T 上有 null。
6. **为什么 `ripplesz` 4→8 救不了**：冲激（高光斑）响应 `g2 = 0` ⇒ **Rb=4 与 Rb=8 完全相同**，
   而波的响应只从 1.07 涨到 1.57（×1.47）⇒ 细/波比只从 **3.55× 改善到 1.96×**，
   高光斑那一半**纹丝未动** ⇒ 与用户观察到的「还是密集许多」逐字对上。
   **结论：换档位治不了这个病，必须换算子。**

（`§14.30.3` 的 λ/4 结论对**旧算子**仍然成立 —— 它算的是「波」那一档；但**旧算子本身是错的**，
两件事别混。`ssr.frag:379` 注释与 `§14.28` 第 6 条的更正照旧并入 v0.18.14。）

**幅度上的第二个病：整片削顶**。波那档 Rb=4 = `1.07 × 0.087 × 10 = 0.93`、Rb=8 = `1.37`
⇒ **波本身也贴着/顶着 `clamp(±1)`** ⇒ 位移场 =「平滑的波（已被削成方波）+ 满幅的高光斑点」，
两块都贴天花板 ⇒ `ripAmp` 想调也调不动。**那个 `×10` 是拍脑袋定的**，必须与 `ripAmp` 一起解耦重标定。

**下一步（1 个旋钮、不改代码）：`ssr.debug=6`** ⇒ 位移归一化场 `length(ripd)/(ripAmp·px)`。

- **修订后的预言**（覆盖 §14.30.3 第 3 行的旧说法）：命中区**整体偏亮**（波那档 0.93~1.37 贴顶）
  **但叠加密集的满幅亮点/短条**（高光斑位置），**不是**平滑的 ~30px 条带；未命中区无回注、应为空/暗。
- 这一张同时是 v0.18.14 修完后的「修后对照」基线 ⇒ **必须先拍**。

**v0.18.14 设计（草案，待 `debug=6` 确认后动代码）**

| 方案 | 做法 | 成本 | 评估 |
|---|---|---|---|
| **A（推荐）输入端鲁棒化** | 取梯度**之前**先把高光斑压掉：局部软限幅 `min(t, 局部均值 + K)`，或 3×3 中值 | +0~9 取样（只在回注路径） | 直接打掉根因；线性带通做不到（见下） |
| **B 亮度域真带通** | `band = 模糊(r=Rb) − 模糊(r≈4~6·Rb)`，不取梯度 | 需**真正宽**的盒式/可分离模糊 | ⚠ **5-tap cross 不行**：`H(r,T) = [3+2cos(2πr/T)]/5`，取 r1=8/r2=32 时 **T=7(0.61)、T=15(0.66) 仍全通、还比波(0.41)大** ⇒ cross 是梳齿不是模糊 |
| **C `ripAmp`/增益解耦** | 新增 `ssr.ripamp`（默认 6 ⇒ 同量级）+ 把 `×10` 拆成可调增益 | 1 新键 | **必做**：§14.30.2 点名的结构性问题，不解耦就没法单独 A/B 幅度 |

**为什么纯线性带通不够（这条值得记住）**：高光斑 `A_glint ≈ 0.7`、波只有 `A_wave ≈ 0.087`
⇒ **8 倍落差**。要靠「压细尺度」把高光斑压到波以下，需要窗口 `w1` 够大；
但 `w1` 一大 λ30 自己也被 `sinc` 削（`w1=16` 只剩 0.59、`w1=30` 直接 null）。
把 w1=8/10/12/16/24/30 全代进去算，**最优也就 1.06**（`w1=12, w2=48`：带通后高光斑 0.0875 vs 波 0.0823）
⇒ **线性窗口最多做到 1:1、永远压不到波以下** ⇒ 必须上非线性（限幅/中值）。

**同轮第三项：「视角拉动时倒影变化太快」—— 三个候选机制，别混为一谈**

| # | 机制 | 现象特征 | 怎么验 |
|---|---|---|---|
| a | **原版是 cubemap 探针**（段16 那 16 个 draw 按涟漪法线采 cubemap，见 `docs/00:40`），立方体贴图低分辨率、**转动时几乎不变**；我方是**活的屏幕镜面**，屏幕内容本身在变 ⇒ 变化快得多 | **平滑滑动**、随视角走 | `ssr.strength=0`（回原版 cubemap）转动 ⇒ 若**基本不动** ⇒ 就是这条。**物理上我方更正确**，只是跟原版比显得"活" |
| b | **命中/未命中切换** + `hitUV` 4% 淡出 | 局部**跳变/闪烁**、集中在倒影边界、一块块换 | 看是不是边界处跳；是 ⇒ 走 `ssr.edge=1` A/B 或加大淡出 |
| c | **1 帧延迟**（我方输出下帧才拷进 585） | 倒影**滞后**于山/地面，像"拖着走" | 结构性（要时域重投影 = P3），先记录现象 |

（补充：**俯仰**时镜像相对直视场景按 **2×** 角速度走（`R·Rot_x(δ) = Rot_x(−δ)·R`）、
**偏航**时两者同步（相对静止）—— 这部分是**镜面物理、不是 bug**，别拿它当缺陷修。）


---

#### 14.30.5 `ssr.debug=6` 位移场实拍量测（2026-10-07）：**深度增益失配**新发现 + 「视角拉动」收口

**输入**：`docs/analysis/Screenshot_ssr.debug=6.png`
（`ssr.debug=6`、`ssr.ripplesz=4` 回默认、`ssr.ripplemode=1`、`ssr.steps=32`，其余 §14.30 基线不变）。
**工具**：`Temp\opencode\px_dbg6.ps1`（临时，纯 ASCII）—— 直方图 + 原始 ACF + 三个带限 ACF，四个水框逐个出。

**A. 同轮两条用户判定（收口）**

| 项 | 用户判定 | 结论 | 处置 |
|---|---|---|---|
| 「视角拉动时倒影变化太快」 | 「倒影变化时**平滑滑动**」 | **机制 a**：原版 cubemap 探针（低分辨率、转动时几乎不变）vs 我方活屏幕镜面（逐像素重采样）⇒ 平滑滑过 | **关闭**，不跟；b（命中/未命中切换跳变）、c（1 帧延迟滞后）一并排除 |
| 「俯仰时镜像按 2× 角速度」 | 「不是 bug 就算了」 | 镜面物理（`R` 对入射角的导数 = 2） | **接受，关闭** |

**B. 实测 1 —— 场的直方图（四框全是水）**

| 框 | mean | 零(≤3) | 满(≥252) | 中间 | 白饱和段长 |
|---|---|---|---|---|---|
| far `x200-1700 y210-380` | **220.9** | **0.0%** | **41.8%** | 58.2% | 4.7px |
| midR `x1050-1850 y430-700` | 121.3 | 5.3% | 19.7% | 74.9% | **4.3px** |
| nearL `x60-420 y760-1070` | **51.5** | **41.6%** | 10.0% | 48.4% | 8.0px |
| nearR `x1450-1900 y760-1070` | **33.0** | **51.4%** | 6.4% | 42.2% | 6.4px |

⇒ **本轮新发现（与带限无关的硬数）：位移场电平随深度摆 6.7×（220.9 → 33.0）。**
**远处整片削顶**（41.8% 满、0% 归零）、**近处基本死掉**（42~51% 归零）。
§14.30.3 / §14.30.4 都没有这个数 —— 那两节只在**同框同一深度**上比过。

**C. 为什么：固定像素尺度的差分器 × 透视变化的屏幕波长**

`g1 − g2` 的基线是**像素**（`Rb` px），而波的**屏幕**波长随透视变：远处压到 4~6px、中景 ~30px、近处摊到 100px+。
对固定幅度 A，响应 `2|sin(2π/T) − sin(2πRb/T)|`（Rb=4）取：

| 屏幕波长 T | 5px | 30px | 150px |
|---|---|---|---|
| 响应 | **3.80** | **1.07** | **0.25** |

⇒ **15× 摆幅**，与实测 6.7× 同向同量级（实测含削顶压缩所以更小）。
⇒ **`ripplesz` 只能把这个固定窗口挪一点点，治不了深度失配** —— 这是 §14.30.4「4→8 救不了」的第二条独立理由。

**D. 实测 2 —— 场的细节尺度**

- **白色饱和段长度 4.3~8.0px** ⇒ 可见细节就是 **4~8px**，而原版波纹 λ≈30 ⇒ **4~7 倍之细** ⇒
  与用户「倒影处的波纹明显比无倒影处的小，而且密集许多」**逐字对上** ✓。
  与 §14.30.4 的「冲激响应在 ±1、±Rb 各打满幅」因果闭合：**亮斑 → 冲激 → 4~8px 饱和块**。
- **细带 ACF `BAND(3..21)` 谷一律 lag 8~9** ⇒ 特征周期 17~19px ≈ λ/2。注意 `debug=6` 画的是
  `length(ripd)`（**幅值**），正弦的幅值周期天生是 λ/2 ⇒ **与 λ30 自洽**；但**不能**据此断言
  「场是波驱动」—— 高光斑坐在波峰上，同样带这个周期，二者此图不可分。
- **原始（不带限）ACF 单调衰减、48 倍滞后内无谷** ⇒ 场里**没有单一主周期**；
  `BAND(9..121)` 谷落在 lag>48 ⇒ 还叠着一层 **>96px 的斑块包络**（=「哪里有纹理」的分布，
  对应图上那些大白块）。
- **方法更正（作废 §14.30.4 一条）**：用 `BAND(9..41)` 去找周期是**自证**（带内只能找到 30 左右），
  该做法作废；周期结论改由**原始 ACF** + 细带 ACF 给。
- 原始 ACF lag1 各向异性：far 竖 **0.59** / 横 0.87（远处纹理被透视压扁）、near 竖横 0.93~0.94
  （最平滑）⇒ **场随距离变细** ✓，与 B 表同向。

**E. ⚠ 视图加权已知问题（v0.18.14 不改，改了前后不可比）**

`ssr.frag:482` 的 `length(ripd)/(ripAmp*px.x)` 把 y 分量按 aspect **放大 1.778×**
（正确写法 `length(ripd/(ripAmp*px))`）⇒ 远处竖向分量主导的数字**被高估**。
即使除掉：220.9/1.778 = 124 仍是 nearR 33.0 的 **3.8×** ⇒ **深度失配结论不变**。
为保住本表作为 v0.18.14 的**前后可比基线**，本版**不动这行**；v0.18.15 与归一化一起改并重取基线。
另：`ssr.frag:360` 注释只写了「debug=6 画 kf」**一半**（实际只有 `ripplemode=0` 才画 kf，`mode=1` 画
`ripd`，本例正是 mode=1 ⇒ 看这行注释会找错图），本版补全。

**F. v0.18.14 定稿：只加旋钮，三个默认值全部 = v0.18.13 行为（零回归风险）**

§14.30.4 的草案经本轮**线性量级复核**后收敛为「输入端软限幅 + 幅度/增益解耦」，
push constant 80B → 96B（新增 `p5` 四格）：

| 键 | 语义 | 默认 | 扫描 |
|---|---|---|---|
| **`ssr.ripk`** | 输入端**软限幅**阈值（线性亮度绝对值）：取梯度**之前**把每个采样点夹进 `本环 4 tap 均值 ± K`；**0 = 关闭** | **0** | 0.02 / **0.05** / 0.10 |
| **`ssr.ripamp`** | `ripAmp` 位移幅度 (px)，**0 = 自动** `clamp(ripplesz*1.5, 6, 16)` | **0** | 8 / 12 / 16（限幅压能量后把幅度抬回） |
| **`ssr.ripgain`** | 梯度增益（现写死 `×10`），**0 = 自动 (10)** | **0** | 5 / 7（仅当 K 打到 0.10 仍整片削顶时才用） |

**为什么必须做成旋钮而不是硬编码**：阈值依赖 `uBase` 的**局部亮度**（同帧暗水 A≈0.03、亮反射区 A≈0.05
线性量级，二者差 1.7×），本机没有渲染端量测手段，写死必错一档 ⇒ 一次会话三档重启即可定，不用重编。

**限幅为什么零额外取样就能一档治三病**（`lo` 由**已取的** 4 个 tap 平均而来，不加任何取样）：

1. **亮斑/冲激封顶**：亮斑超出本环均值的量被夹到 `K` ⇒ `|g1|、|g2| ≤ 2K` ⇒ `|g1−g2| ≤ 4K` ⇒
   **场被硬性封顶 `40·K`**，且单侧亮斑的现实值约 `20·K` ⇒ **不再整片削顶**（K=0.05 ⇒ 现实值 1.0 上下）；
2. **轮廓泄漏**：阶跃两侧的超额同样被夹 ⇒ 两支都进 `[−K, +K]` ⇒ **相减仍归 0** ⇒
   §14.30.4 担心的「`ripplesz` 调大把山/天轮廓印上去」被结构性堵住；
3. **深度失配的远端**：远处被压细的波相对本环均值的超额远大于 `K` ⇒ 被压 ⇒ far 的 41.8% 满格回落。
   （近端「死掉」那一半限幅治不了，要靠归一化 / 随深度变的基线 ⇒ **记到 v0.18.15**。）

**同批注释更正**：`ssr.frag:360`（只写了 mode0 那一半：`debug=6` 仅在 `ripplemode=0` 时画 kf，
`mode=1` 画 `ripd`）、`ssr.frag:381`（过期：「波长 ≈ 2·Rb 才起效」—— §14.30.3 实测是 **λ/4**）。

**判据（跑完怎么算过）**
- 视觉：倒影处的波纹**不再细碎密集**、尺度接近无倒影处；亮度不足就抬 `ssr.ripamp`。
- 量化：`px_dbg6` 复测 midR `满%` 应从 **19.7%** 明显回落、白饱和段长从 **4.3px** 变长；
  far `满%` 应从 **41.8%** 回落。
- 若 `ssr.ripk=0.10` 仍整片削顶 ⇒ 阈值取小了，往 0.2 / 0.4 续扫，同时 `ssr.ripgain` 降到 5~7；
  若波纹**整体变淡/消失** ⇒ K 取小了，回 0.10。

---

### §14.30.6 v0.18.14 实跑判读（2026-10-07）：三旋钮扫描定档 + 两条新观察（倒影区缺高光 / 扇形区与两侧无倒影）

**输入**：回归 ini 基线不变（`ssr=1 / ssr.v1=1 / ssr.smooth=4 / ssr.blur=1 / ssr.base585=1 / ssr.wdep=1 /
ssr.ripple=1 / ssr.ripplemode=1 / ssr.ripplesz=4 / ssr.edge=0 / ssr.strength=1 / ssr.debug=0`）
只动 v0.18.14 的三个新旋钮，每档单独重启，实跑 5 档：

| 档 | 图（`docs/analysis/`） |
|---|---|
| `ssr.ripk=0.02` | `Screenshot_ssr.ripk=0.02.png` |
| `ssr.ripk=0.05` | `Screenshot_ssr.ripk=0.05.png` |
| `ssr.ripk=0.10` | `Screenshot_ssr.ripk=0.10.png` |
| `ssr.ripk=0.10 + ssr.ripamp=16` | `Screenshot_ssr.ripk=0.10 ssr.ripamp=16.png` |
| `ssr.ripk=0.10 + ssr.ripamp=16 + ssr.ripgain=5` | `Screenshot_ssr.ripk=0.10 ssr.ripamp=16 ssr.ripgain=5.png` |

#### 1. 波纹判据：限幅确实压掉了能量，必须 `ripamp` 配对抬回

- 用户视觉判据：**`ssr.ripamp=16` 后「波纹现在好点了」** ⇒ 与 §14.30.5 的预案一致：软限幅把位移场封顶以后
幅度必然变小，`ripk` 单开不够，`ripk` 与 `ripamp` 要**成对使用**。
- 定档（后续试验基线）：**`ssr.ripk=0.10` + `ssr.ripamp=16`**，`ssr.ripgain` 回到 0（自动 = 10 = v0.18.13 增益）——
`ripgain=5` 那一档跑过但没有明确结论，只在「0.10 仍整片削顶」时才降增益。
- **三档 `ripk` 之间不做跨图量化比较**：5 张图机位/视角各不相同（§14.30.4 已记「跨图比较因机位不同作废」），
只采纳用户在同场景下的相对视觉判据。
- 由此 v0.18.14 的**目标达成**：位移场不再被高光斑劫持成整片饱和块，波纹恢复到用户可接受水平；
`ssr.debug=6` 的量化复测（四框 `full%` / 白饱和段长 / 6.7× 落差）**留待下一轮在同机位下补**。

#### 2. 新观察 A：倒影区没有普通区的明亮区

- **现象**：SSR 命中区（远处那条灰绿色的镜面带）里没有普通区那片密集的白亮斑/高光。
- **归因（待证）**：`ssr.strength=1` ⇒ `oColor = mix(baseRGB, refl, 1)` = **原版 585 反射层被 100% 顶掉**；
原版水的白亮斑是**原版 shader 现算的 specular**，只存在于 585 层、**不在 520 屏幕内容里** ⇒
我方 SSR 是拿屏幕内容做镜像，采不到那一层 ⇒ 命中区天然没有那片亮斑；未命中处回退 585 ⇒ 亮斑还在。
- **与 v0.18.14 无关**：`ripk/ripamp/ripgain` 只改**位移场**（`baseRGB` 独立取样、不经限幅），不改颜色混合，
也不会增减高光 ⇒ 属既有行为，本轮第一次被注意到。
- **单旋钮诊断（下一轮）**：`ssr.strength=0.7`（其余全部不动）⇒ 亮斑按 ~30% 回来即坐实；
站得住的正解是给命中区补一路原版高光，或把 `strength` 当长期旋钮用（代价是反射变糊）。

#### 3. 新观察 B：倒影区始终是扇形，屏幕两侧各有一小块没有倒影

- **归因（待证）**：`ssr.edge=0` 时**射线飞出屏 ⇒ 未命中 ⇒ 回原版层**，命中区的边界 =
「反射射线还能留在屏内」的像素集合 ⇒ 天然呈**扇形**；近处射线先飞出**屏顶**、
屏左右两侧的射线先撞**左右边** ⇒ 左右各必有一小块 miss。miss 处回退 585，而那些像素上
原版本身几乎无反射 ⇒ 看起来「没有倒影」。
- **与 v0.18.14 无关**：该几何只由 `ssr.edge` / `ssr.mode` / 相机姿态决定，三个新旋钮不参与。
- **单旋钮诊断（下一轮）**：`ssr.debug=2` 出 hit/miss 图（绿=命中、红=未命中）⇒ 逐一核对
扇形边界与两侧小块是不是就是 miss 区；同时补一张 `ssr.debug=0` 同机位图做 A/B。
- **备选修复（确认后再动，别一次改两个）**：
1) `ssr.edge=1` 屏幕边缘延展 —— §14.27 已证它带来「扇形下半拿 `lastUV` 采到岸边原位画面（未镜像）
贴进水里 ⇒ 错位重影」，属换汤不换药，这正是默认值取 0 的原因；
2) 正解仍是 §14.29 方案C 的**「屏幕里看得见的用 SSR，看不见的交给 cubemap」** —— 但现架构里
回退源就是 585（= 原版 cubemap 反射层），它在那些像素本来就没有内容 ⇒ 要给 miss 像素
单独提供一路反射源，属 v0.18.15 量级的活，不塞进本轮。

#### 4. 本轮结论

- 三旋钮按预期工作，**不改代码**；A / B 两条按「每步只改一个旋钮」排到下一轮，
先 `ssr.strength=0.7`（A），再 `ssr.debug=2`（B），一次一档、各自单独重启。
- v0.18.14 实跑白亮边/细碎饱和块的**量化复测**（同机位 `ssr.debug=6` 四框直方图）与本轮两条诊断
合并到 §14.30.7。

---

### §14.30.7 v0.18.14 两条新观察的单旋钮诊断（2026-10-07）：A/B 双坐实，且同源

**输入**：基线 `ssr.ripk=0.10 + ssr.ripamp=16`（`ssr.ripgain` 删掉 = 自动 10），其余回归 ini 不变，
一次只改一个旋钮、各自单独重启。

| 试验 | 只改的一个旋钮 | 图（`docs/analysis/`） |
|---|---|---|
| A | `ssr.strength=1 → 0.7` | `Screenshot_ssr.strength=0.7 v0.18.14.png` |
| B | `ssr.debug=0 → 2`（绿=命中、红=未命中） | `Screenshot_ssr.debug=2 v0.18.14.png` |

#### 1. A 坐实：`ssr.strength=1` 把原版 585 的现算 specular 100% 顶掉

- **结果（用户判据）**：「改 `ssr.strength=0.7` 后**有明显亮斑**，但是**不如正常的亮**」。
- **判读**：`oColor = mix(baseRGB, refl, 0.7)` ⇒ 原版高光按 **30%** 权重回流 ⇒「有明显亮斑」= 它回来了、
「不如正常的亮」= 只有 0.3×。与线性混合模型**定量一致**（不是「差不多」，就是 0.7 / 0.3 那个比例）。
- ⇒ **假设 A 坐实**：那片白亮斑是原版 shader 在 585 层**现算的 specular**，不在 520 屏幕内容里 ⇒
我方拿屏幕内容做镜像，物理上采不到；未命中处回退 585 所以普通区一直看得见。
- ⇒ **推论：单纯调 `strength` 无法两全** —— 要 100% 亮斑就得 `strength=0`（= 放弃 SSR）。
`0.7` 只是「既要有倒影又要有亮斑」的折中点；继续扫 `0.5 / 0.85` 属调参，不是修复。
- **正解（v0.18.15 候选）**：`strength` 保持 1 吃满屏幕镜面，同时把原版层的**高频分量**补回去
（`baseRGB − MA(baseRGB)` 作 detail 叠加，或与 `refl` 取 `max` 亮部），等价于「SSR 出形、原版出高光」。
需改代码，不塞进本轮。

#### 2. B 坐实：命中区边界 = 「反射射线还能留在屏内」的像素集合

- **实拍判读（`Screenshot_ssr.debug=2 v0.18.14.png`，1920×1080 水面像素着色）**：
  - **绿（hit）**：贴远岸（上界 y≈370，即岸/水交界线）向下的一条带，**中部最厚**（x≈400–700 可压到 y≈650）；
  - **红（miss）**：整个近场（屏幕下约 40%）+ **左右屏边各一小块** —— 左缘 x<250 处红从 y≈430 就开始、
  右缘 x>1750 处红从 y≈420 开始，而相邻中部绿能到 y≈600+；
  - ⇒ 绿带**中部厚、两侧薄**，轮廓正是用户描述的**扇形**；屏两侧那两小块是**必 miss** ✓。
- **机理两条全部对上**：
  1. 近场射线仰角大 ⇒ 先穿出**屏顶** ⇒ miss（红）；
  2. 屏左右侧射线带横向分量 ⇒ 先撞**左右边** ⇒ miss（红）；
  3. 绿带之所以存在：那些射线飞得够远、**撞上对岸岩壁** ⇒ hit（对岸线 = 绿带上界）。
- ⇒ **假设 B 坐实**，且与 v0.18.14 无关（几何只由 `ssr.edge` / `ssr.mode` / 相机姿态决定）。
- ⇒ **`ssr.edge=1` 不是解**：§14.27 已实跑证过它拿 `lastUV` 采到岸边原位画面（未镜像）贴进水里 ⇒
错位重影（对照图 `Screenshot_ssr.edge=0.png` / `=1.png` 已入库），这正是默认值取 0 的原因。
- **正解（v0.18.15 候选）**：miss 像素单独给一路反射源 = §14.29 方案C 的「看不见的交给 cubemap」。
现架构的回退源就是 585，而它在这些像素 fresnel 低、本来就没内容 ⇒ 得真取 cubemap（或我方按
`lastUV` 反射方向自己采 cubemap），属 v0.18.15 量级。

#### 3. A 与 B 同源（本轮最重要的判读）

- **同一个根因的两面**：命中区被**全量替换**（丢掉原版高光），未命中区**原样保留**（留着原版高光）⇒
两区亮度差被拉到最大 ⇒ 用户先看到「扇形分界」（B），紧接着看到「倒影区不亮」（A）。
- 所以这两条**不该分头修**：只要把原版高频补回命中区（A 的正解），
扇形分界的**亮度对比**会同时被压下去；剩下纯几何的边界仍需 cubemap 兜底（B 的正解）。

#### 4. 本轮结论

- 两条假设**全部坐实**，且都不是 v0.18.14 引入 ⇒ **v0.18.14 收官**：
三旋钮定档 **`ssr.ripk=0.10` + `ssr.ripamp=16`**，`ssr.ripgain` 保持 0（自动 = 10）。
- 本轮**不改代码**；两条正解（原版高频回补 / cubemap 兜底）写进 §14.31 候选（v0.18.15）。
- **遗留量化复测**：同机位 `ssr.debug=6` 四框直方图仍未拍（midR `满%` 19.7% → ?、白饱和段 4.3px → ?、
四框 6.7× 落差 → ?），需固定机位补一张再入档。

---

### §14.30.8 v0.18.14 结项量化（2026-10-07）：机位闸 PASS 后的同机位配对 —— ripk 作用真实但小，深度落差归 `ripA`，`ripamp` 在 debug=6 中不可见

**为什么必须重拍**：上一次复测（`Screenshot_ v0.18.14 ssr.debug=6.png`）与基线 `Screenshot_ssr.debug=6.png` 机位不符，
工具实测 **纵向 dy=+60px（对齐后 r 从 0.772 → 0.979）、横向 dx≈+220px** ⇒ 四个固定屏幕测量框取到了不同深度的水面，
按 §14.30.4 立的规矩「跨图量化因机位不同一律作废」，那一轮数字不入档。

**方法（本轮第一次用工具化机位闸）**：旧基线机位无法精确复现 ⇒ 不去追它。利用 §14.30.5 定稿的性质
「三旋钮全 0 = 与 v0.18.13 逐位等同」，**用当前 DLL 直接清零旋钮重拍基线档**，与新档组成同存档、同站位的配对：

| 档 | 旋钮 | 图（`docs/analysis/`） |
|---|---|---|
| 基线档 | `ssr.ripk=0` `ssr.ripamp=0` `ssr.ripgain=0`（= v0.18.13 逐位行为） | `Screenshot_ v0.18.14 ssr.debug=6 ripk0 ripamp0.png` |
| 新档 | `ssr.ripk=0.10` `ssr.ripamp=16`（`ripgain` 删 = 自动 10） | `Screenshot_ v0.18.14 ssr.debug=6 ripk010 ripamp16.png` |

其余回归 ini 一字未动；两档各自重启（ini 启动只读一次），同存档加载后不移动不转视角即截图。

**机位闸实测（`tools/check_frame.ps1`，新增）**：

```
VERT  best dy = +0 px   MAD=1.16  r=1.000
HORIZ best dx = +0 px   MAD=2.88  r=0.959
VERDICT: FRAMING OK   EXIT=0        （闸: |dy|<=6px, |dx|<=30px）
```

⇒ 本次两图同机位、同光照，**数字有效**。

#### 1. 四框直方图（`tools/px_dbg6.ps1`，新增入库）

| 框 | 基线档 mean / full% / run / zero% | 新档 mean / full% / run / zero% | Δ mean | Δ full% |
|---|---|---|---|---|
| far | 189.3 / 35.3% / 6.1px / 0.0% | 187.2 / 33.1% / 5.5px / 0.0% | −2.1 | **−2.2pp** |
| midR | 160.3 / 32.4% / 5.0px / 2.7% | 158.9 / 31.9% / 4.7px / 2.9% | −1.4 | −0.5pp |
| nearL | 58.8 / 10.1% / 5.9px / 30.8% | 71.1 / 13.7% / 4.7px / 29.5% | **+12.3** | **+3.6pp** |
| nearR | 41.6 / 8.1% / 7.6px / 49.8% | 37.1 / 7.4% / 4.6px / 46.3% | −4.5 | −0.7pp |

**落差**：
- mean 落差（max/min）：基线档 189.3 / 41.6 = **4.55×** → 新档 187.2 / 37.1 = **5.05×**
- full% 落差：基线档 35.3% / 8.1% = **4.36×** → 新档 33.1% / 7.4% = **4.47×**

#### 2. 判读

1. **`ripk=0.10` 的作用真实但小**：far / midR / nearR 三框饱和率微降（−2.2 / −0.5 / −0.7pp），
   两框近处的死像素率也降了（nearL zero 30.8% → 29.5%、nearR 49.8% → 46.3%）。
   **方向与其设计完全一致** —— 把每个采样点夹进本环 4 tap 均值 ±K = 削高补低；
   但幅度只有 ±3~4pp，远小于 §14.30.5 的预期。
2. **深度电平落差没有被 `ripk` 改善**（4.55× → 5.05×，基本持平）⇒ **归因更正**：
   落差来自 **`ripA` 深度响应**（§14.30.3 实测 15× 摆幅），而 `ssr.ripk` 只限梯度、
   **根本不参与 `ripA`** ⇒ §14.30.5 把「四框 6.7× 落差收窄」列为 `ripk` 的预期目标属**判据选错**，作废。
   这一条要在 §14.31 单独立项（调 `ripA` 的深度归一），不是 v0.18.15 的活。
3. **`ripamp` 在 `debug=6` 中结构性不可见**：
   `ssr.frag:425` `ripd = clamp((g1-g2)*ripGain,-1,1) * (ripAmp*px) * ripA`，
   `ssr.frag:482` `clamp(length(ripd)/(ripAmp*px.x),0,1)` ⇒ `ripAmp` **精确约掉**（夹取发生在乘 `ripAmp` 之前）。
   ⇒ `debug=6` 是**幅度无关**的量，用户实跑看到的「`ripamp=16` 后波纹好点了」是**幅度轴**的变化，
   这条指标**原理上测不到** ⇒ v0.18.14 的视觉改善无法用 `debug=6` 验收。
4. **旧 6.7× 是异机位数字，不可同列**：该机位 far mean = 220.9，本机位 = 189.3，
   两者不是同一取景。§14.30.5 的表格**保留为历史记录**，但引用结项数字时以**本表基线档 4.55×（mean）/ 4.36×（full%）** 为准。

#### 3. 本轮结论

- **v0.18.14 正式收官**：三旋钮定档 `ssr.ripk=0.10 + ssr.ripamp=16`（`ripgain`=0 自动 10）。
  `ripk` 的收益 = 梯度限幅带来的削高补低（±3~4pp），`ripamp` 的收益 = 幅度抬升（肉眼可见，指标不可见）。
- **方法论沉淀**：`tools/check_frame.ps1`（机位闸）与 `tools/px_dbg6.ps1`（四框直方图）入库；
  **今后任何跨图量化必须先过机位闸**，`VERDICT: FRAMING OK` 才允许出数。
- **挂起项（§14.31 候选，勿塞进 v0.18.15）**：
  a) 需要一个**对幅度敏感**的指标来验收 `ssr.ripamp`（候选：同机位 `debug=0` 与 `debug=6` 配对，
  或直接量反射采样点的位移像素数）；
  b) `ripA` 深度响应归一（4.55× 落差的正主）。

---

### §14.31 挂起项队列与 v0.18.15 实现（2026-10-08）：命中区高光回补 `ssr.v1det`

**本章是 §14.30.7 / §14.30.8 约定要开的那本账**（`§14.30.7` 末：「两条正解（原版高频回补 / cubemap 兜底）
写进 §14.31 候选（v0.18.15）」；`§14.30.8` 末：「挂起项（§14.31 候选，勿塞进 v0.18.15）」）。
本节一次把队列立起来，并记录**其中第一项已经被取走**（= v0.18.15）。

#### 1. 队列总表

| # | 项 | 出处 | 状态 |
|---|---|---|---|
| 1 | A 的正解：命中区回补原版高频/高光 | §14.30.7 A 坐实 | ✅ **已实现 = v0.18.15**（`213becc`） |
| 2 | B 的正解：miss 像素真 cubemap 兜底 | §14.30.7 B 坐实 | 🚧 P2 通路实拍走通（16d 配对：契约/数字闸 PASS、内容 FAIL，见 §14.16），**卡 P3 方向**（视图空间 Rf 直喂世界 cube + 疑 D3D↔VK Y 翻转）；FG 亮度 P3 修完再判 |
| 3 | `ripA` 深度响应归一（四框 4.55× 落差的正主） | §14.30.8 判读2 | ⏸ 待开 |
| 4 | 对幅度敏感的指标（`ripamp` 目前在 `debug=6` 中测不到） | §14.30.8 判读3 | ⏸ 待开（方法已被 §5 的 `debug=8` 打通：**直接渲染该量**而非跨图做差，位移量可另开一档） |
| 5 | `ssr.debug=6` 的 aspect 加权 bug（`:482` 放大 y 1.778×） | §14.30.5 | ⏸ 与 #4 一起改，改完**重取基线** |
| 6 | `ssr.ripplemode=0` 对照 / `ssr.fov=58.7155` 不带 debug / `debug=1` / `ssr.blur=0` / §14.25.1 三条隔离试验 / §14.30.1 补帧时闸 | 更早 | ⏸ 旧账 |
| 7 | 探针 cube **周期性重传**（16c 实测读回 sig 5s 即变 = 内容动态，现 `g_probeDumpN>=2` 后永久停在第 2 代会陈旧） | §14.15 日志判据 | ⏸ 待开 |
| 8 | **P3 朝向校准**：① `Rf` 视图→世界（数据源拍板 = B 挂 VSSetConstantBuffers 读回；push 96→128B p6/p7）② D3D↔VK cube 轴/Y 翻转校准（对照游戏 585 反射定符号） | §14.16 根因、**§14.17 实装**、**条目18 死锁修复** | 🚧 v0.18.17 已实装但实拍日志证实**读回死锁**（q 恒 0、旋转从未生效，见条目18）；**v0.18.18 改 14 槽全扫 + 首候选锁定 + 绑定普查**，待实拍（普查定槽 → camidx/camconj/cubeflip 迭代）后收口 |

#### 2. v0.18.15 实现：`ssr.v1det` 命中区高光回补

**为什么必须在合成式上动手**（§14.30.8 已把旋钮侧的路堵死）：
`ripk` 只削高补低 ±3~4pp 且根本不参与 `ripA`；`ripamp` 在 `debug=6` 分子分母精确约掉、原理上测不到。
所以 A 的缺口只能补在最后那行 mix 上。

**改法**（`ssr.frag` 合成段）：

```glsl
float wSsr   = hit ? (k * bf) : 0.0;      // k = ssr.strength, bf = 贴边淡出权重
vec3  outRGB = mix(baseRGB, refl, k);     // = v0.18.14 原式
if (v1det > 0.001 && wSsr > 0.0001) {
    float lb = dot(baseRGB, vec3(0.2126, 0.7152, 0.0722));
    float gb = smoothstep(0.10, 0.45, lb);   // 初值 0.35/0.85，实跑重标定见 §6
    outRGB += max(baseRGB - refl, vec3(0.0)) * (v1det * gb * wSsr);
}
oColor = vec4(outRGB, base.a);
```

- **`max(baseRGB - refl, 0)` 只补不压**：暗处、SSR 本就占优处完全不受影响；亮处取二者较亮的一侧。
- **`gb` 是绝对亮度门**（原版层 luma 在 0.10 以下完全不参与、0.45 以上全量参与），
  挡住「中等亮度整片倒回原版层、把 SSR 削弱」这条副作用。
  **初值 0.35/0.85 已被 §6 实拍证伪** —— 585 层亮度中位数只有 0.21~0.33，旧门限高于中位数，
  把 76~94% 的水面像素挡在门外 ⇒ 重标定为 0.10/0.45。
- **`wSsr = strength × 贴边淡出权重 bf`**：只有 SSR 真正接管的像素才补。
  `bf` 原本是 `if (hit)` 块内的局部变量，本轮**提到函数作用域**（唯一的一处结构性改动）。

**三层零回归**（任一成立即退化回原式）：

| 条件 | 结果 |
|---|---|
| miss | `wSsr = 0` ⇒ 纯原版逐位不动（v0.18.11 回退契约不变） |
| `ssr.strength = 0` | `k = 0` ⇒ 仍是「逐字节原版」（v0.18.9 契约不变） |
| `ssr.v1det = 0` | 整块不进 ⇒ **逐位 = v0.18.14**（默认值即 0） |

**push constant**：复用 `p5` 空槽 `w`（v0.18.14 占了 x=ripk / y=ripamp / z=ripgain）⇒
**仍 96B，不动 `pcr.size`、不加 p6**，爆炸半径最小。配套：`vkrenderer.h` 加 `extern g_ssrV1Det`；
`vkrenderer.cpp` PC 填充 `p5[3]` + `g_ssrV1Det` 进 `ssrV1Sig()`（push 变了必须重录）+ ready 回显；
`main.cpp` 全局定义 + `ini ssr.v1det` 读取 + 0..1 钳位 + 侦察回显 + 版本横幅 → `v0.18.15`。

**提交与闸**：`213beccdc70144ec287b11a21ea4dc2cb8e79429`，6 files / +947 −881；
`make_shaders` OK（`ssr.frag.spv` 30232 → 31248 B）、`quotescan3` **oddQuoteLines=0**、
`check1` **RESULT OK**、CI **SUCCESS** 双 job（polled = local = remote = `213becc`，二次查询复核）。
**位移轴一律未碰**（`ripk`/`ripamp`/`ripgain`/`debug=6` 绘制路径）⇒ §14.30.8 的数字仍可比。

#### 3. 为什么 B 拆 v0.18.16（不是忘了做）

`ssr.edge=1` 已被 §14.27 判为换汤不换药（拿 `lastUV` 采到的是岸边原位画面），所以 B 必须有**真反射源**。
本轮查明 `probe` 这把钥匙的真身是**探针升质**——拦 `CreateTexture2D` 把游戏的 reflection probe cube
从 512² 升到 1024²，`g_probeCube` 就是那个 `ID3D11Texture2D*`。也就是说：

- 我们**确实握着游戏的 cubemap 对象**；
- 但它是 **D3D11 侧、且不是 SHARED/NT handle** ⇒ 走不了深度和 585 那条 KMT 导入通路，
  要给 Vulkan 用得**另开一条跨 API 拷贝/导入**，是独立的重活。

⇒ 归 v0.18.16 单独立项，**本轮一行都没碰**（已写进版本横幅备查）。

#### 4. v0.18.15 实测指引（等用户跑）

- ini 在现有 `ssr.ripk=0.10` / `ssr.ripamp=16` 基础上**只加一行 `ssr.v1det=`**，其余一字不动；
- **从 `0.3` 起扫**，`0` 必须先拍一张当对照档（= 逐位 v0.18.14，可用来验零回归）；
- 预期现象：命中区（尤其原版 585 层里有白亮斑的位置）亮斑按 `det` 比例回来；
  无副作用时四框 `debug=6` 数字**不该变**（该路径在 debug 分支之前 `return`，本就不受影响）；
- 若出现「整片泛亮 / 倒影反而变弱」⇒ 是 `gb` 亮度门（初值 0.35/0.85）定得不合适，改这两个常数，**不要动 `v1det`**；
  反向（热力图全空 ⇒ 门关太死）走同一条路 —— §6 已按实拍分布把它重标定为 **0.10/0.45**。

**任一档失败只关自己，WARN 不连坐**；机位仍按 §14.30.8 立的规矩，跨图比较先过 `tools/check_frame.ps1`。

#### 5. v0.18.15 实跑方法学改造（2026-10-08）：逐像素差分判死，改 `ssr.debug=8` 直出诊断

**两次失败，是两个互相独立的原因。**

**第一层 —— 两档其实是同一档。** 用户首测的 `Screenshot_ssr.v1det=0 .png`（00:24:48）与
`Screenshot_ssr.v1det=0.3 .png`（00:26:22）看着成对，但全盘唯一的 `poc-presenter.ini`
（mtime 00:23:12）当时仍写着 `ssr.v1det=0`，且三次启动回显全是 `v1det=0.000000`
⇒ **`0.3` 从来没写进游戏**。这条教训直接生出本章新增的旋钮回显闸 `tools/knobchk.ps1`：
**「改了 ini」证明不了任何事，只有日志回显能证明这一档真的跑起来了。**

**第二层 —— 写对了也量不出来。** 用户改对后重拍（ini mtime 00:33:19 → 启动 00:34:28
回显 `v1det=0.300000` → 截图 00:35:12），单旋钮、机位闸 PASS（`dy=+2 dx=0 r=0.998`）。但两档相隔
**10.4 分钟**，逐像素统计给出：

| 区域 | 亮斑数（luma>0.85）00:24:48 → 00:35:12 | 变化 |
|---|---|---|
| **四框之外**（天空/地形/HUD —— `v1det` **物理上碰不到**） | 22256 → 25948 | **+3692（+16.6%）** |
| far（有反射命中） | 827 → 966 | +139（+16.9%） |
| midR / nearL / nearR | 3816 / 2296 / 2965 → 2834 / 1425 / 1862 | −982 / −871 / −1103 |
| **四框合计（水面）** | 9904 → 7087 | **−28.4%** |

第一行是判决性的：`v1det` 只在 `hit` 像素上 `+= max(baseRGB-refl,0) * …`，**改不到天空和地形**，
可它们 10 分钟里亮斑多了 16.6% —— 这就是纯光照漂移的量级；水面整体反而暗了 28.4%，
回补量被这股漂移整个盖住，只有 far 框方向为正（与旋钮一致但同量级）。再叠加先前量到的噪音底
（同设置两图 86.9% 像素变、`meanDelta=28.77`），于是：

> **结论：跨图逐像素差分对「百分之几亮度」量级的改动已经到天花板 —— 不是旋钮无效，是尺子不够。**

**改法 —— 把要量的量直接画出来，不再做两张图之差**（与 `debug=6` 量位移场同一个路子）：

- **`ssr.debug=8`**（`ssr.frag`，`p3.z > 7.5` 最先返回）渲染**单位回补量**
  `add = max(baseRGB - refl, 0) * (gb * wSsr)` —— 与合成式**逐项同式，但不乘 `ssr.v1det`**，
  经 Reinhard `y = l/(1+l)` 压进 8bit（读数 `l = y/(1-y)`）。⇒ 它表示 **det=1 的满量程**，
  所以 `ssr.v1det=0` 也能拍；**单张直出 ⇒ 没有第二张图 ⇒ 机位闸、光照漂移、水面动画全部不参与**。
- **无深度像素输出 0 而不是 6/7 的灰 0.5**：共用分支 `p3.z > 1.5` 原本对所有 debug≥2 统一写 `vec4(0.5)`，
  经反解就是 `l = 1.0` ⇒ 天空会被读成「补得最猛」，热力图整片 `#`、`meanL` 被撑爆、VERDICT 正好反着来。
  改成 `p3.z > 7.5 ? vec4(0.0) : vec4(0.5)`（`2/6/7` 一字未动）。
- **读数器 `tools/px_v1det.ps1`**：四框 `nz% / meanL / p99L / p99.9L / maxL(含热点坐标)` + 64×24 热力图，
  并直接给出 `det=0.3 ⇒ meanL × 0.3` 的换算。
- **闸 `tools/knobchk.ps1`**：解析末次启动块的旋钮回显，容差 1e-4。两个已修的坑：
  ① 插件回显是短名（`v1det=0.3`）而 ini 键是 `ssr.v1det` ⇒ 必须长短名都匹配，否则永远误报 FAIL；
  ② **横幅那一行自己就含 `14.30.5 debug=6` 这类历史注释**，起点若取横幅行会吃到假值
  （预检实测：`-Key ssr.debug -Value 8` 报「used 6」，而真回显在 1797 行明明是 `debug=0`）
  ⇒ 扫描起点改成 `$last + 1`。

**环境教训（省下 6 次失败）**：向 GitHub 推送连续 6 次 `Internal Server Error`
（status 页全绿、`--dry-run` 通过、no-op push 正常 ⇒ 通道没问题、是 pack 本身），
真凶是 **thin pack** —— `git push` 默认打 thin delta，服务端算不出来。
**`git push --no-thin origin master` 一次过**，后续三批均一把推上。再撞 500 直接加 `--no-thin`。

**提交线**（全部 CI SUCCESS 双 job）：`caf68f7`（`ssr.debug=8` + 横幅 + 钳位 7→8）→
`489ae0d`（`tools/knobchk.ps1`）→ `90a1bba`（`tools/px_v1det.ps1` + `vkrenderer.h/.cpp` 注释）→
`e046c8f`（无深度改输出 0，`ssr.frag.spv` 32448 → 32588 B）→ `e9f468f`（knobchk 扫描起点）。

**对 §4 的修正**：实测**不再需要配对拍两张** —— 只拍一张 `ssr.debug=8`，`ssr.v1det` 保持 0 即可
（图表示的是 det=1 满量程）。判读口令：热力图基本全空 ⇒ `gb` 亮度门（初值 0.35/0.85，§6 已重标定 0.10/0.45）或
`baseRGB <= refl` 把回补掐死，按 §4 改这两个常数、**不动 `v1det`**；水面出现 `-`/`+`/`*`
⇒ 旋钮有效，`p99L × det` 就是实际加回量，可直接回答「为什么肉眼只有细微亮斑」。

**任一档失败只关自己，WARN 不连坐**；本节只改诊断通路，`ssr.debug=0`（默认）分支一字未动、
push constant 仍 96B、位移轴未碰 ⇒ §14.30.8 的四框数字照旧可比。

#### 6. v0.18.15 实跑判读（2026-10-08）：`debug=8` 出图坐实「只有细微亮斑」，`gb` 门被实测证伪在分布之外

**两次截图都过了闸**（闸 = 日志回显，不是「改过 ini」）：

| 档 | 末次启动 | knobchk | 截图 | 文件 |
|---|---|---|---|---|
| `ssr.debug=8` | 12:41:27 | `echoed ssr.debug = 8`（line 2491）exit 0 | 12:42:22 | `Screenshot_v0.18.15 ssr.debug=8.png` 2306977 B |
| `ssr.debug=7` | 12:56:15 | `echoed ssr.debug = 7`（line 3152）exit 0 | 12:57:05 | `Screenshot_v0.18.15 ssr.debug=7 .png` 3816823 B |

**读图 —— 判决不靠四框数字，靠两图对照。**

`debug=8`：**水面整片黑 + 零星白点/白丝**；岩壁、人物、船是正常彩色场景 —— 这不是 bug，
段17 契约决定了 585 层只在水面合成，非水面像素根本看不到我们的输出。
与 `Screenshot_ssr.v1det=0` 对照（同一片水在正常画面里是蓝绿底 + **满屏白亮片**）可确认黑区确是
**我们的回补量输出**，而非场景本身暗。

> **⚠ 由此派生的读图前提：凡非水面像素，屏上显示的是场景，不是回补量。**
> 所以全图热力图**上半部那些 `#` 是岸石亮度**，与 `add` 无关；判读只看水面那几行，
> 而「哪几行是水」必须靠 `-Mark` 认。这条与下面的框判读是同一个坑的两面。

> 这就是「为什么肉眼只有细微亮斑」的答案：三个有效框的回补量**上界**只有
> `0.0134 ~ 0.0523`（`far` 那个 0.544 是岸、不计），`det=0.3` 再乘 0.3
> ⇒ **最多补 0.4% ~ 1.6% 的亮度**，肉眼自然只见那批白点。

**⚠ 四框里 `far` 这批机位下无效（其余三框有效）。** 判据用的是本轮新增的
`tools\px_v1det.ps1 -Mark` —— 直接把四个框画到截图上（`Screenshot_v0.18.15 ssr.debug=8.boxes.png`）：

- `far` `x200-1700 y210-380` **横跨岸线**：上半压在卵石滩 / 船 / 崖脚，只有下沿约 40px 是水
  ⇒ 它读出的 `meanL=0.544 / p99.9L=30.875` 是**岸石亮度**，不是回补量；
- `midR` / `nearL` / `nearR` 三框**完全落在黑水里** ⇒ 数字有效。

> **方法学修正（这一步判错了，就地纠正）。** 我最初拿「灰度像素占比」当水面判据
> （`far 0.011% / midR 0.000% / nearR 0.652% / nearL 45.695%`）得出「三个框都压在岸上」——
> **这个推断是错的**：`midR`、`nearR` 明明整框都在水里。原因是**我们的输出本身为 0 的黑水像素，
> 也会因 585/588 掺合而带色**，于是「非灰度」既可能是岸、也可能是「回补量恰好为 0 的水」，
> 两者根本分不开。
> ⇒ **灰度占比不能当水面判据**；`-Mark` 直接看图才算数。
> 新规矩：测量框是姿势相关的，跨图比较除了先过 `tools/check_frame.ps1`，
> 还必须用 `-Mark` 复核框是否仍落在水面 —— §14.30.8 的四框数字只在那批姿势下成立。

**读数上限（`out = mix(585, 588, w)`，585 占 ~90%，§14.29）**：屏上看到的是
`y = 0.9·add + 0.1·588`，反解得 `add = (y − 0.1·588)/0.9`。因 `588 ≥ 0`，**读数一律是回补量的上界**
（最多高估 11%）；同时 `nz%` 会被 588 那 10% 撑起来，**不能直接当「有回补」的比例**。
扣掉这两点，三个有效框的实测量级是：`midR meanL=0.0523`、`nearL 0.0134`、`nearR 0.0143` ——
换算成 8bit 只有 1~11 级，**肉眼看就是黑**，与图上观感一致。

**`gb` 门的量化判决。** `ssr.debug=7` 输出的正是 `baseRGB`（`base585=1` ⇒ `texture(uBase, uv).rgb`），
它**不是黑的**：完整的崖壁 / 船 / 树倒影 + 满屏亮片。三块水面实测（隔行采样）：

| 区块 | meanL | `luma>0.35`（旧门开） | `luma>0.85`（旧门满） |
|---|---|---|---|
| leftwater `x80-620 y600-1000` | 0.2125 | **10.14%** | 1.28% |
| rightwater `x1400-1900 y480-880` | 0.2355 | **6.06%** | 1.58% |
| farwater `x300-1350 y330-430` | 0.3258 | **24.40%** | 0.52% |

> **旧门限 0.35 高于它所门控那层的亮度中位数（0.21~0.33）** ⇒ `gb` 在 **76~94%** 的水面像素上
> 恒为 0，`add` 被直接乘死 —— 这解释了热力图为何几乎全黑，而不是「本来就没得补」。
> 0.35 这个数当初是拍脑袋定的，没有对着 585 层的实际亮度分布校准过。

**动作（§4 预授权的单旋钮，`v1det` 一字不动）**：`ssr.frag` 两处
`smoothstep(0.35, 0.85, lb)` → **`smoothstep(0.10, 0.45, lb)`** —— 下沿放到水体典型亮度之下、
满量程收到 0.45：亮片仍满开，水体部分参与。安全性不变：`max(baseRGB-refl,0)` 只加不减 +
`v1det` 缩放 + `wSsr` 命中门；**本块 `v1det=0` 整块不进 ⇒ 逐位 v0.18.14，零回归**。
提交 `a497d10`（`ssr.frag.spv` 32588 B 不变）。

**尚未分离的两个因子（本轮只定了 `gb` 一个）**：`add = max(baseRGB − refl, 0) × gb × wSsr`
三个因子里，`gb` 已量化，`max(…)`（原版本身是否更亮）与 `wSsr`（是否命中 / 贴边淡出）**还没分开**。

**下一张 `debug=8` 的判读口令**（ini `ssr.debug=8` → 重启 → `knobchk -Key ssr.debug -Value 8` PASS → 截图）：

- 热力图明显张开（水面出现成片 `-` / `*`）⇒ **`gb` 就是凶手**，改动成立；
  接着拍 `ssr.v1det=0.3` 正常图看观感，确认没有「整片泛亮 / 倒影反而变弱」
- 热力图**依旧基本全空** ⇒ **`gb` 不是凶手**，把常数回退；
  元凶在 `baseRGB <= refl` 或 `wSsr = 0`（miss，即 B 的问题），那时才需要
  `debug=9` 三因子 RGB 直出（R=`gb` / G=`wSsr` / B=`max(baseRGB−refl,0)`）来分家

**任一档失败只关自己，WARN 不连坐**；本节改的是 `v1det>0` 才进的块，
`ssr.v1det=0` 默认档与 `ssr.debug=0` 默认档均逐位未动。

#### 7. 第三层失败：`dllchk` v1 自己判错了 —— 时区陷阱（2026-10-08）

**这一层失败的不是 DLL，是判据本身。**

**现象**：13:47 那轮 `gb010` 截图，`knobchk -Key ssr.debug -Value 8` **PASS**、ini 三项全对，
我却拿 `tools/dllchk.ps1` **v1** 查出「`poc-presenter.dll` mtime = `2026-10-08 05:36:52`，
而 `a497d10` 是 **13:10:05** 提交 ⇒ 早 463 分钟 ⇒ `STALE BUILD`」，
据此宣告整轮作废、让用户重拍。**结论是假阳性，那张图本来是有效的。**

**根因（两个时钟被当成了一个）**：

| 来源 | 时钟 |
|---|---|
| `git show --format=%ci` | 本机 **+0800** 挂钟 |
| GitHub Actions runner / artifact zip 的 DOS 时间戳 | **UTC** 挂钟（runner 无时区） |

zip 条目记录的是**写入方的挂钟**；解压到 +0800 机器上，PowerShell 把它当本地时间打印，
于是「`05:36:52`」其实是 **UTC**，对应本地 **13:36:52**。8 小时的差被我读成了「旧 7.5 小时」。

**两条数据把这个解释钉死**（都精确对上）：

| DLL 打印 mtime | 解释为 UTC ⇒ 本地 | 吻合的提交（UTC） |
|---|---|---|
| `05:36:52` | 13:36:52 | `a011bd9` = `05:35:08Z`（+104 s 出包） |
| `05:59:46` | 13:59:46 | `30bb459` = `05:58:38Z`（+68 s 出包） |

⇒ 13:47 那轮跑的**就是含新 `gb` 的 DLL**（`a497d10` 是 `05:10:05Z`），图有效。

**教训（本节的真正价值）**：**跨时区构建链上，时间戳比对不可靠。**
时间是间接证据，得先知道每个来源用的是哪个时钟；**内容才是判据。**

**`dllchk.ps1` v2 —— 内容为主、时间为辅**：

| 检查 | 作用 | 性质 |
|---|---|---|
| `loaded` | 从日志 `… poc-presenter.dll ==` 反查**进程实际映射的文件**，核对路径 | 路径 |
| `spv` | **DLL 是否逐字节内嵌当前 `ssr.frag.spv`**（实测内嵌于偏移 `0x5dda0`，比对前 128 B） | **时区无关，主判据** |
| `-After "<本机挂钟>"` | 构建时间先归一化 `打印值 + UTC偏移` 再与提交时间比 | 可选，只抓 C++ 侧陈旧 |

附**退化输入守卫**：针的前 128 B 若不足 8 种不同字节（如全零）会误命中 DLL 自身的填充区，
此时直接 `FAIL` 而不是搜索 —— 反例实测 `-Spv <全零文件>` ⇒ `needle is degenerate (1 distinct bytes)` exit 1。
三例正反测试：正常 **PASS exit 0** / 退化针 **FAIL exit 1** / 换成别的真实文件 **FAIL exit 1**。

**拍图前的完整闸序**：

```
1. tools\dllchk.ps1 [-After "<本机提交时间>"]      ← 证明是新二进制 (内容比对)
2. 改 ini → 重启
3. tools\knobchk.ps1 -Key <键> -Value <值>        ← 证明旋钮真跑起来了
   → 截图
4. tools\check_frame.ps1 -A <上张> -B <本张>      ← 跨图比较前, 证明机位可比
   → tools\px_v1det.ps1 -Path <本张> -Mark        ← 先看框位, 再看数字
```

#### 8. `gb` 重标定实拍判读：**`gb` 不是凶手**（2026-10-08 14:05）

**闸全绿**：`dllchk` v2 `spv PASS` + 归一化构建时间 `13:59:46 ≥ a497d10 13:10:05`；`knobchk`
**PASS**（启动 14:04:29 → 截图 14:05:13，`echoed ssr.debug = 8` line 4595）。
`check_frame` 对 12:42 那张判 `dy=-46 dx=+188` ⇒ **跨图逐像素对比作废**，只做单图判读。
`px_v1det -Mark` 复核：`midR` / `nearL` / `nearR` 整框在水上（`midR` 左上角含一块岩石，占 9.2%），
`far` 仍横跨岸线、作废。

##### 8.1 先纠一个肉眼判读的坑：热力图的 `-` 是底噪，不是回补

`px_v1det` 热力图按 **y（0..1 显示值）** 分箱：

```
' ' ≤0.001   '.' <0.02   '-' <0.06   '+' <0.15   '*' <0.40   '#'
```

而 `585/588` 掺合底噪的 `l = 0.0324` ⇒ `y = 0.0314` ⇒ **恰好落在 `-` 档**。
所以「水面成片 `-`」**看起来像有回补，实际是没有任何回补的掺合底噪**。
肉眼判读在这一档完全失效，必须看 `y ≥ 0.06` 的像素占比。

##### 8.2 量化：`y≥0.06`（高于底噪）的像素占比

读数用 `px_v1det` 内置的 **floor test** 段（按 **B 通道**分箱，与上面的热力图取同一通道，两者永远一致）：

```
floor test (588 bleed l=0.0324 => y=0.0314 => byte 8;  real addend = y>=0.06 = byte>=102):
box        <=.001     <.02     <.06     <.15     <.40    >=.40   ABOVE-FLOOR
midR       0.00%    0.11%   94.98%    0.03%    2.63%    2.24%         4.91%
nearL     54.03%   42.96%    0.00%    0.24%    2.61%    0.17%         3.01%
nearR      4.48%   61.90%   33.62%    0.00%    0.00%    0.00%         0.00%
far        0.00%    0.00%   26.27%   33.39%   17.89%   22.46%        73.73%   (跨岸线·作废)
```

| 框 | OLD `gb 0.35/0.85`（12:42，机位不同·**仅参考**） | **NEW `gb 0.10/0.45`（14:05，B 通道）** |
|---|---|---|
| `midR` | 21.57%（RGB 亮度法） | **4.91%** ← 同框左上角岩石占 9.2%，亮部同量级，**水面分离不出来，按 0 计** |
| `nearL` | 2.25% | **3.01%**（零星白点） |
| `nearR` | 0.00% | **0.00%** |
| `far`（跨岸线·作废） | 100% | 73.73% |

**通道敏感性已核**：`nearL 3.01%` / `nearR 0.00%` 用 B 通道与 RGB 亮度两种算法**完全一致**；
差异（`midR 4.91 vs 8.39`、`far 73.73 vs 99.88`）**全部落在彩色场景像素**里 ——
`debug=8` 自己的输出是灰度，但屏上是 `mix(585, 588, w)` 而 **`588` 带颜色**，
只剩底噪的地方 B ≠ 亮度。工具取 B 是为了和热力图同源，水面结论不受影响。

**`gb` 门从 0.35/0.85 降到 0.10/0.45 之后，水面回补量依旧 ≈ 0** ⇒ 走 §6 第二分支：

> **`gb` 不是凶手。** 按 debug=7 实测水面 `meanL = 0.2125 / 0.2355 / 0.3258`，
> 新门应给出 `gb ≈ 0.23 ~ 0.71`（**非零**）；乘积仍为 0 ⇒ 元凶在另外两项。

**重标定本身保留**（不回退）：旧门限 0.35 高于被门控层的亮度中位数，这是 `debug=7` 直接测出来的
设计错误，与「它是否是本次元凶」无关 —— 回退等于把一个已证伪的设计放回去。

##### 8.3 缩小嫌疑：`bf` 是**平方**出现的

`ssr.frag:471` `refl = mix(baseRGB, refl, bf)`，代入 `:493` 得

```
max(baseRGB - refl, 0) = bf · max(baseRGB - refl_ssr, 0)
w8 = strength · bf                                        (:487)
⇒  add8 = strength · gb8 · bf² · max(baseRGB - refl_ssr, 0)，外层再乘 hit 门
```

即真正的三个旋钮是 **`hit`（0/1）**、**`bf²`（贴边淡出被平方，双重压制）**、
**`max(baseRGB − refl_ssr, 0)`（原版层是否比 SSR 亮）**。`gb` 已洗清。

##### 8.4 下一发：`ssr.debug=2` 零代码分家 `hit` vs `max()`

`ssr.frag:344` 的 **`debug=2`（绿=命中 / 红=未命中）是现成的 `hit` 探针**，只改 ini、不用重编：

- 水面**大片红** ⇒ `hit=false` ⇒ `w8=0` 是元凶 ⇒ 那就是 §14.31 队列 **#2 的 B 问题（miss 真 cubemap）**，
  A 的生效范围只限岸边那条命中带；
- 水面**大片绿**但回补仍为 0 ⇒ `hit` 无辜 ⇒ 元凶是 `bf²` 或 `max(baseRGB − refl_ssr, 0)`，
  才轮到加 `debug=9` 三因子直出（R=`gb` / G=`wSsr` / B=`max()`）分家。

**读数器 `tools/px_hitmiss.ps1` 已就绪**（分类按 **`G−R` 的符号与幅度**，容差 ±0.15 ——
因为 `debug=2` 同样要过 `mix(585,588,w)`、585 占 ~90%，纯绿读作 `G−R≈+0.9`、
纯红 `R−G≈+0.9`、中性场景 `≈0`；**只按绝对值判会把场景当命中**）。

**在已入库的 `Screenshot_ssr.debug=2 v0.18.14.png` 上预演（同一读数器，**机位是 v0.18.14
那次、只作预测不作定案**）**：

```
box        water%     hit%    miss%
midR        96.99%   51.78%  48.22%
nearL       96.89%    0.00% 100.00%   <- 整框 miss
nearR      100.00%    0.00% 100.00%   <- 整框 miss
ALL         30.74%   33.34%  66.66%
```

命中带贴岸（图上第 8–11 行成片 `H`），**近场整片 `m`**。这与 §8.2 的 `debug=8` 读数互相印证：
岸边带有 `+`/`*` 回补，而 `nearR 0.00%` / `nearL 3.01%` —— **miss 处 `wSsr=0`，本来就不该有回补**。
待现机位复核后才写定论。

#### 9. 受控比较：`hit`/`miss` 掩码 × `debug=8` 回补量 —— A 在命中区成立，缺口全在 miss

**闸全绿**：`dllchk` `spv PASS`（loaded 15:00:53）；`knobchk` **PASS**
（启动 15:00:52 → 截图 15:01:30，`echoed ssr.debug = 2` line 5292）；ini `ssr.debug=2`、
`ripk=0 / ripamp=0 / v1det=0.3` 全对。

##### 9.1 跨档比较的第一道坎：`check_frame` 在这里失效

`check_frame` 对 `debug=8`（14:05）与 `debug=2`（15:01）报 `best dy=+256 r=0.870` —— **假的**。
它对**整图**做互相关，而这两档的水面是**反相**的（黑 vs 红/绿），零位移处 `r=-0.734` 正是反相签名。
逐像素差分同样无分辨力（场景带 `mean|dR| ≈ 44`，连已知机位不同的两对也是 45.5）。

改用**归一化行剖面的一维互相关**（先去均值 ⇒ 对全局亮度系数免疫）：

| 配对 | 最佳 lag | r |
|---|---|---|
| `dbg8 14:05` vs `dbg2 15:01` | 40 px | 0.9500 |
| `dbg8 14:05` vs `dbg8 12:42` | 36 px | 0.9626 |
| **`dbg2 15:01` vs `dbg8 12:42`** | **4 px** | **0.9826**（lag0 = 0.9811） |

⇒ **`{12:42 debug=8, 15:01 debug=2}` 同机位**，`14:05` 是异类（偏 ~40px）。
副产物：`12:42` 与 `15:01` 隔 1 小时 20 分、中间还重启过仍对齐 ⇒ **不挪机位时跨重启可复现**。

> **闸（新增）**：**跨 `debug` 档比较必须先用场景带行剖面互相关验机位**，`check_frame` 只能用于同档之间。
> 这条已由 `tools/px_cond.ps1` 强制（不带 `-SceneChecked` 直接 exit 2 拒跑）。

##### 9.2 `hit`/`miss` 分布（姿势 A，`px_hitmiss`）

```
box        water%    hit%    miss%
nearL       96.31%   0.00%  100.00%     <- 整框 miss
nearR      100.00%   0.00%  100.00%     <- 整框 miss
midR        97.11%  20.74%  79.26%
far         41.53%  99.76%   0.24%      <- 横跨岸线，仅参考
ALL         57.90%  25.77%  74.23%      <- 全图
VERDICT: 74.2% MISS -> wSsr=0 is the gate
```

> **`ALL` 行曾是错的（本轮发现并修掉）**：`px_hitmiss.ps1` 的框循环末尾有 `break`，
> 而 `ALL` 是排在四个命名框之后的全图框 —— 框内像素命中第一个框就跳出，
> **永远进不了 `ALL`**。于是 `ALL` 实际统计的是「四框之外」，`$n` 却打印成 `1920×1080`，
> 表现为 `water% + other% = 65.17%` 而非 100%。修复前那行是 `30.77% / 25.13% / 74.87%`，
> **作废**。四个命名框的数字不受影响（四框互不重叠）。
>
> 交叉验证：修复后 `ALL` 与 `px_cond` 的独立实现**逐位一致** ——
> 姿势 A `1,200,704 = 309,479 + 891,225`、姿势 B `1,299,674 = 240,772 + 1,058,902`。

##### 9.3 受控比较：miss 组是**天然对照**

屏上 `y = 0.9·add + 0.1·588`；而 `miss ⇒ add ≡ 0`（`ssr.frag:487/493`）。
所以 **miss 组的 `y` 分布 = 纯掺合**，hit 组 = 掺合 + 回补：

```
group        n       mean y     p50      p90      p99     max y   y>=.06   y>=.10
HIT       309479    0.1147   0.0706   0.2902   0.6353  1.0000   75.39%   14.35%
MISS      891225    0.0385   0.0275   0.0510   0.4353  1.0000    5.39%    3.27%
```

> **口径变更（P0 期间发现）**：`px_cond` 的像素值已由 **G 通道**改为 **Rec.709 luma**。
> 屏上像素 = `0.9·灰值 + 0.1·588场景色`，而 **588 是彩色的** ⇒ `G ≠ luma`
> （`G` 带绿偏的场景掺合，`luma` 才是真亮度，也正是 shader 自己用的量 `l8 = dot(add8, luma)`）。
> 本表已按 luma 重算。**实测影响很小、结论不变**：姿势 A `ratio 4.36 → 4.39`、
> `y≥0.10` `14.24% → 14.35%`；姿势 B `4.94 → 5.11`、`19.82% → 20.39%`。

三条互相印证的证据：

1. **对照自洽**：miss 组 `mean y = 0.0385`，与 §8.2 那个「底噪 `l = 0.0314`」同量级 ⇒ 对照可信；
2. **严格界**：`588 ≤ 1` ⇒ **掺合最多贡献 `0.1`**，所以 **`y ≥ 0.10` 不可能来自掺合** ——
   HIT `14.35%` vs MISS `3.27%`（**4.39×**）；
3. **量级**：HIT `p90 = 0.2902`，是掺合上限 0.1 的 2.9 倍；`dMean = 0.0762` ⇒ 真实回补量 ≈ `0.085`。

> **`y≥0.06` 这一档不再可信**：它只要 `588 ≥ 0.6` 就能被掺合撑起来。
> **判读一律看 `y ≥ 0.10` 列。**

##### 9.4 修正 §8.2 的解读

§8.2 的三个水框里 `nearL`/`nearR` 是 **100% MISS**、`midR` 是 **79% MISS** ⇒
那些「回补 ≈ 0」的数字**量的是 miss 区，根本没测到 `gb` 的效果**。
同理，§8.2 里「底噪恒为 `l=0.0324`」也是**那一块的 `588` 恰好暗**，不是常数 ——
`px_cond` 已改用 miss 组作对照，不再依赖固定阈值。

`gb 不是凶手` 的结论**仍然成立**（真正卡住的是 `hit` 门），但证据改挂在本节；
`gb` 重标定在命中带的实际效果见 §9.6。

##### 9.5 结论与待办

- **`ssr.v1det` 在命中区确实生效**（75.39% 的命中像素 `y≥0.06`、14.24% `y≥0.10`，对照组仅 5.39% / 3.26%）；
- **缺口 100% 在 miss 区**（姿势 A 74.23%、姿势 B 81.47% 的水面）—— 那是 `wSsr=0` 的**按设计**行为，
  即 §14.31 队列 **#2 的 B 问题**（miss 回真 cubemap，v0.18.16）；
- **`gb` 复测**：已在同一姿势 B 下补齐（§9.6），结论不变。

##### 9.6 同姿势复测新 `gb`（姿势 B）+ 机位配对方法

**闸**：`dllchk` `spv PASS`（loaded 15:32:23）；`knobchk` PASS
（`debug=8` 启动 15:28:33 → 截图 15:29:06、`echoed ssr.debug=8` line 5871；
`debug=2` 启动 15:32:23 → 截图 15:32:53、`echoed ssr.debug=2` line 6372）；两项皆只动 `ssr.debug`。

**机位配对（决定成败的一步）**。一度判为「不合格」，是用了 `y0..420` 的行剖面 ——
**该区间含水面行，两档反相会把相关性拖到 `r(lag0)=-0.02`**。换成**纯场景带 `y0..280`** 后反转：

| 配对（纯场景带 `y0..280`，lag ±120） | best lag | r | r(lag0) |
|---|---|---|---|
| **新掩码(15:32) vs 加成图(15:29)** | **−2 px** | **0.9920** | **0.9773** ✅ |
| 新掩码(15:32) vs 旧掩码(15:01) | 64 px | 0.9253 | −0.2507（异机位） |
| 旧掩码(15:01) vs `dbg8 12:42`［已知好对］ | 4 px | 0.9474 | 0.9239 ✅ |
| 加成图(15:29) vs `dbg8 14:05` | 8 px | 0.9891 | 0.9312 ✅ |
| 加成图(15:29) vs `dbg8 12:42` | 72 px | 0.9407 | −0.3163（异机位） |

⇒ 两套机位自洽：**A = {12:42 旧 `gb`, 15:01 掩码}**、**B = {14:05, 15:29, 15:32}**，A/B 俯仰差 ~44–50px。

> **闸（再收紧）**：**行剖面必须取 `y0..280`（纯场景带）**。取到 `y0..420` 会把水面算进去，
> 对「好对」给出 `r(lag0)=-0.02` 的假阴性、对坏对反而可能给高分。

**受控比较结果（新 `gb`，姿势 B）**：

```
group        n       mean y     p50      p90      p99    max y   y>=.06   y>=.10
HIT       240772   0.1201   0.0745   0.2902  0.6275  1.0000   83.17%   20.39%
MISS     1058902   0.0423   0.0275   0.0549  0.4980  1.0000    6.98%    3.99%
dMean=0.0778   y>=.10 ratio=5.11        （按 §9.3 的 luma 口径）
```

对照姿势 A 的旧 `gb`（§9.3）：`y≥0.10` HIT `14.35% → 20.39%`、`y≥0.06` `75.39% → 83.17%`、
`ratio 4.39 → 5.11`。**但 A/B 机位不同，跨图量化按纪律作废**，这两个数只作为
「新 `gb` 下同一套受控判据依然成立、且余量不劣于旧 `gb`」的**定性**旁证；
`gb` 增益的**定量**需要 A/B 同机位的一对 `debug=8`，目前不具备。

**唯一可跨图下定论的，是同图内的 HIT vs MISS** —— 而这在两个姿势、两种 `gb` 下
都给出同向结论：`ratio` 4.39 与 5.11、`p90 = 0.2902`（掺合上限 0.1 的 2.9 倍）。

**任一档失败只关自己，WARN 不连坐**；本节全程只动 `ssr.debug` 一个旋钮，
`v1det>0` 合成块与 `v1det=0` 默认档均逐位未动。

##### 10 P0：issue B 正常档量化基线（`px_cond -Normal`）

**动机**：§9 / §9.6 测的是**加成量**（`debug=8`），只能证明 `A` 成立、缺口全在 miss 区；
缺口在**正常档画面**上到底有多大，从来没量过。P0 补这个数 —— **零渲染改动**，
只把 `px_cond` 像素口径换成真 luma、并加一个 `-Normal` 档（`5246bdf`）。

**旋钮**：只动 `ssr.debug 2 → 0`。`ssr.v1det=0.3` / `ripk=0` / `ripamp=0` / **`ssr.edge=0`** 全部未动
（`edge=0` 是基线的必要条件：miss 必须真的回退到原版层，否则量的不是 B 的起点）。

**闸**：
- `dllchk` `spv PASS`（loaded 16:11:09）
- `knobchk` PASS（启动 16:11:09 → 截图 16:11:48，`echoed ssr.debug = 0` line 6847）
- 图：`docs/analysis/Screenshot_v0.18.15 ssr.debug=0 samepose.png`

**机位配对（本次原判「不过」，靠两条补检放行）**

行剖面给出 `dy=+6, r=0.8235`（`r(lag0)=0.7397`），**低于 §9.6 立的 `r ≥ 0.97` 门槛**。逐条查因：

| 检查 | 结果 | 结论 |
|---|---|---|
| **列剖面**（唯一能看见 `dx` 的量） | `dx = 0, r = 0.9704` | 水平完全对齐 |
| 行剖面 | `dy = +6, r = 0.8235` | 竖直仅 6px |
| 光照漂移（纯场景带按 `y` 分带的 `meanR` 比值） | `1.087 / 1.074 / 1.077 / 1.167` | 前三带均匀 **+7~9%** ⇒ 15:32→16:11 的**内容漂移**是 `r` 掉下来的主因，不是错位 |
| **掩码竖移敏感性** `dy ∈ {−6,−3,0,+3,+6}` | `0.446 / 0.441 / 0.437 / 0.437 / 0.433` | **±1.6%**，`nHit` 几乎不变（240771 vs 240772） |
| **掩码横移敏感性** `dx ∈ {−12,−6,0,+6,+12}` | `0.437 / 0.437 / 0.437 / 0.441 / 0.441` | **±0.9%** |

⇒ 判**放行**：判决阈值 `0.90`、实测 `0.437`，**差 2 倍，±6px 的配准误差够不着判决边界**。

> **闸（§9.6 三道之外的第④道）**：**行剖面看不见水平位移** —— 整行均值对 `dx` 不变。
> 跨时段/跨光照的配对**必须再过一次列剖面**取 `dx`；当 `r` 因内容漂移而不达标时，
> 唯一诚实的放行依据是**掩码平移敏感性**（配准误差对读数的实际影响），而不是硬凑一个 `r`。

**踩坑（PS5.1 变量名大小写撞车）**：敏感性脚本第一版把命中 `p50` 存进 `$hM`、
miss 直方图存进 `$hm` —— PowerShell 变量**大小写不敏感**，两者是**同一个变量**，
`$hM = …` 一赋值就把直方图覆盖成标量：`p50(miss)` 走兜底返回 `1.0`（打出 `2.476 = 1.0/0.404`）、
`≥0.10 占比` 打出 `0.00`。**破案线索是自相矛盾** —— `missMean = 55.03` 与 `nMiss = 1,058,902`
是直方图之外独立累加的（正确），不可能与「`p50 = 1.0`、`0%` 超过 0.10」并存。
改名 `histA/histB` 后自洽。`px_cond.ps1` 本体无此问题（直方图 `$hh/$hm`、分位数 `$hP50/$mP50`）。

**受控比较结果（正常档，姿势 B）**：

```
group        n       mean y     p50      p90      p99    max y   y>=.06   y>=.10
HIT       240772   0.4206   0.4039   0.6039  0.7961  1.0000  100.00%   99.99%
MISS     1058902   0.2158   0.1765   0.3255  1.0000  1.0000   99.81%   99.05%

mean                 miss/hit = 0.513
p50                  miss/hit = 0.437     ← 判决列
structure (p90-p50)  hit 0.2000  miss 0.1490   miss/hit = 0.745
```

> `y≥.06 / y≥.10` 两列是 `debug=8` 加成档的**掺合界判据**，在正常档里两组都是满值、**无信息**。
> `-Normal` 只看 `p50` 与 `structure`。

**判定：`B CONFIRMED`**（`p50` 比值 `0.437` ≪ 阈值 `0.90`）。
近场与屏两侧的水面亮度只有命中带的 **43.7%**、结构量只有 **74.5%** ⇒ 缺口在正常档画面上
是**肉眼可见**的。**队列 #2 不降级，转 P1**（probe 探测日志：只加日志、零渲染改动，
用以判定 `C1` 路线的两个前提 —— staging 可读？probe 内容静态？）。

> **重要保留（它决定验收口径）**：`hit` 与 `miss` 是**不同的水域、反射的是不同的（合法的）环境** ——
> 近场水面本来就该反射暗的近岸。所以 `0.437` 里混着「真缺反射」与「本来就该暗」两部分，
> 是**缺口的上界**，不是缺口本身。
> ⇒ **验收不能写「比值涨向 1.0」**（那会把一个正确的 cubemap 误判成失败）。正确口径是**同机位配对**：
> 同一张 `debug=2` 掩码下 `edge=0` 与 `edge=2` 各跑一次 `-Normal`，比 **MISS 行自己的 `p50` / `structure` 位移**，
> 同时 **HIT 行必须一动不动**（A 的回归闸），最后**人眼看扇形边界** ——
> 用户抱怨的是一道**人为的边**，不是一个亮度台阶。该口径已写进 `px_cond -Normal` 的判读尾注。

**任一档失败只关自己，WARN 不连坐**；本节全程只动 `ssr.debug` 一个旋钮，
`v1det` / `ripk` / `ripamp` / `ssr.edge` 均逐位未动，§9 的加成判据不受影响。

##### 11 v0.18.16 代码批：P1 探针探测 + P2 `ssr.edge=2` 真 cubemap（合并一发）

**为什么合并**：P1（只加日志）与 P2（shader + 上传通路）**各自都要出 DLL、都要手动换包重启**，
两件事合成一次下载换 DLL 省一整轮体力活（用户拍板）。分工不混、互不干扰：

- **P1 答「技术上通不通」** —— 探针能否被 CPU 读回、内容是否静态；
- **P2 答「值不值」** —— 翻 `ssr.edge=2` 拍图看画面；
- `ssr.edge` 默认仍 0 ⇒ **渲染逐位等于 v0.18.15**，P2 的风险对默认档零暴露。

**改动面**

| 文件 | 改动 |
|---|---|
| `shaders/ssr.frag` | `binding = 4 uniform samplerCube uProbe`；miss 分支加第三档；文件头补 v0.18.16 段。**只动 `refl`** |
| `pocb_shaders.h` | `make_shaders.ps1` 重编，`ssr.frag.spv` 32588 → **33228 B** |
| `vkrenderer.h` | `POCB_DEV_FNS` 补 `X(vkCmdCopyBufferToImage)`（core 1.0，原表漏了）；交接 `g_probeCpu / W / H / Layers / CpuTick` |
| `vkrenderer.cpp` | cube 资源一套 + `ssrV1CubeMake` / `ssrV1CubeSync` / `ssrV1EffEdge`；描述符 **4 → 5 binding**；签名加 `g_ssrV1CubeSrc` + `g_probeCpuTick` |
| `main.cpp` | `ssr.edge` 读入口钳 0..2；`probeDumpTick`（P1）；交接变量定义；横幅 → v0.18.16 |
| `tools/check1.ps1` | `$need` 补 11 个新符号（字节级替换，BOM / 纯 LF 逐项未变） |

**三个必须点名的决定**

**① 签名与 push 必须用「有效 edge」，不能用 ini 原值。**
`edge=2` 但探针数据还没到时，描述符只能绑 **1×1 全零占位 cube**，而
`outRGB = mix(baseRGB, refl, k)` 在 `strength=1` 下就是 `mix(baseRGB, 0, 1)` =
**miss 区整片黑水** —— 这是 P2 最危险的失败模式。
⇒ 新增 `ssrV1EffEdge()`：没真 cube 时把 2 退回 0（回原版层 = v0.18.15 行为），cube 一到自动升 2。
⇒ **`ssrV1Sig()` 与 `pc.p4[3]` 必须同源**，否则换档那次既不重录、推给着色器的还是旧值 ⇒ 画面看着没反应。
触发点是 `g_probeCpuTick` 进签名：数据到位 → 签名变 → 重录 → 上传发生在
`ssrV1CubeSync` → 其后才填描述符 ⇒ 正好指真 cube。

**② `samplerCube` 的描述符不能像 binding2/3 那样拿 2D view 兜底。**
着色器**静态引用** binding4：空 view = 未定义行为，指向 2D view = 类型不匹配。
⇒ 常驻一张 1×1 占位 cube（6 面 RGBA16F，48 B），`edge` 是几都建。
代价是 `ssrV1CubeSync` 失败会 `fail()` 关掉 v1 —— 属「关自己」；且格式与游戏自用的
512² RGBA16F 探针同款，实际不会失败。

**③ 上传用自己开的 command pool，不借共用 pool。**
`vkResetCommandPool` 把它名下**所有**命令 buffer 拉回 initial，借共用 pool 会把当帧
正在录的命令一起 reset 掉（`g_ssrVkCmd*` 全废）。⇒ 单开 pool + fence + `ONE_TIME_SUBMIT`，
布局 `UNDEFINED → TRANSFER_DST → SHADER_READ_ONLY`，源 buffer 取
`HOST_VISIBLE|COHERENT`（表里没有 `vkInvalidateMappedMemoryRanges`，不 COHERENT 没法保证 CPU 写可见）。

**P1 日志怎么读**（`notePresent` 每帧喂，只跑两次）：

| 日志行 | 含义 |
|---|---|
| `P1探针: desc W=… fmt=… mips=… array=…` | C1 的 `VkImage` 必须与它 1:1；`array≠6 / mips≠1 / 非 RGBA16F / 非方形` ⇒ **不发布给 VK**（但读回照做） |
| `P1探针: STAGING CreateTexture2D ok (MiscFlags=…)` | **① 可读性**：带 `TEXTURECUBE` 失败会自动退 `MiscFlags=0` 再试一次并都记日志；两次全失败 ⇒ **C1 不可行，转 Plan B（C5 假环境）** |
| `P1探针: Map(sub=…) hr=… ⇒ 读回失败` | 同判 C1 死活（逐 subresource 读，6 面 × 全 mip） |
| `P1探针: 第1次读回 ok sig=…` / `第2次读回 ok sig=… 相隔 Ns ⇒ 内容静态 / 内容有变` | **② 静态性**：相隔 ≥5s 的两次 FNV-1a 全量签名。一致 ⇒ 一次上传即可；不一致 ⇒ 按代重传（`g_probeCpuTick` 进签名已兜住） |
| `P1探针: 已发布 CPU 镜像 1024² × 6 面, 第 1 代` | 48 MiB 交接完成，VK 侧随即可上传 |
| `ssr.edge=2 但探针 cube 还没就绪 -> 暂按 edge=0 走…` | ① 里的黑水护栏，只打一次 |

> **时序护栏**：第一次读回要等探针出现后 **≥2s**（否则读到还没被渲染过的初始内容，
> 第二次必然不同 ⇒ 会把「静态」误判成「在动」）；第二次再隔 **≥5s**；探针换对象则配对作废重来。

**P2 验收**（沿用 §10 的口径，**不是**「比值涨向 1.0」）：
同一张 `debug=2` 掩码下 `edge=0` 与 `edge=2` 各跑一次 `px_cond -Normal` ——
**HIT 行必须一动不动**（A 的回归闸）、**MISS 行比它自己的 `p50` / `structure` 位移**、
外加**人眼看扇形边界**。朝向（D3D11 与 Vulkan 的 cube 手性/面序差异）归 P3，
本版只保证「采得到、不是花屏」。

**本地闸**：`make_shaders.ps1` OK（`ssr.frag.spv` **33228 B**）、`check1.ps1` **RESULT OK**、
`quotescan3` **oddQuoteLines=0**。**CI**：`2560b31` **FAILURE**，且**只有一条**编译错 ——
`vkrenderer.cpp(3642): error C2065 'VK_PIPELINE_STAGE_FRAGMENT_BIT': undeclared identifier`
（Vulkan 里从来没有 `FRAGMENT_BIT`，只有 `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`；
同文件既有 8 处写的都是后者，新写的 cube 上传收尾 barrier 漏了 `_SHADER`）；
`14b74c2` 一行对齐后 **SUCCESS**。

> **取 CI 报错的新姿势**（旧 `cierr.ps1` / `get_log.ps1` 已失效）：Actions 日志 API 现在
> 要求认证，`actions/runs/{id}/logs` 一律 **403**；但 **job 页的 Annotations 是公开的** ——
> 无需登录、浏览器打开 job 页，`document.querySelectorAll('[class*="annotation"]')` 取
> `innerText`，每条 `error C####` 是一个独立节点（本轮 2 条注解 = `exit code 2` + 唯一那条
> `error C2065`）。所以**一次就能看全全部报错**，不用猜。

##### 12 v0.18.16 实跑：P1 两问当场答完（C1 成立），同一发在加载存档时 CTD = 必填参数传空

**① P1 实测结果**（2026-10-08 20:06:51 启动，`ssr.edge=0` 正常进图，日志逐字）：

```
[20:06:59] P1探针: desc W=1024 H=1024 fmt=10(RGBA16F=1) mips=1 array=6 sample=1 bind=40 misc=4 cpuAccess=0
[20:06:59] P1探针: STAGING CreateTexture2D ok (MiscFlags=4)
[20:06:59] P1探针: 第1次读回 ok sig=13856201724594908557 (Map 全 6 面/全 mip ⇒ STAGING 可读)
[20:06:59] P1探针: 已发布 CPU 镜像 1024² × 6 面, 第 1 代
[20:07:04] P1探针: 第2次读回 ok sig=13856201724594908557 与第1次相隔 5s ⇒ 内容静态 (一次上传即可, C1 成立)
```

⇒ **两问都答了**：① 可读（`TEXTURECUBE` 的 STAGING 一次成功，`MiscFlags=4` 没用到退路）；
② 静态（两次全量 FNV-1a 逐位相同，一次上传即可）。且 `desc` 正好落在 C1 的 1:1 前提上
（RGBA16F / array=6 / mips=1 / 1024² 方形）⇒ **C1 路线技术上通了**，剩下的只有 P3 朝向校准。

**② 但这一发在加载存档时 CTD**（`crash-2026-10-08-20-07-11.log`），根因是 C1 通路里
`vkCreateFence` 的 **`pCreateInfo` 传了 `nullptr`** —— 这是**必填参数**，非法 Vulkan：

```cpp
// 错 (v0.18.16 首版): pCreateInfo 必填, 传 nullptr ⇒ 下游解引用空指针
if (!g_ssrV1CubeFence && c.fns.vkCreateFence(c.vdev, nullptr, nullptr, &g_ssrV1CubeFence) != VK_SUCCESS)
// 对 (与同文件 :998 既有写法一致)
VkFenceCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fci.flags = 0;
if (!g_ssrV1CubeFence && c.fns.vkCreateFence(c.vdev, &fci, nullptr, &g_ssrV1CubeFence) != VK_SUCCESS)
```

三条证据互相咬合，指向唯一调用点：

| 证据 | 内容 |
|---|---|
| 栈顶 | `renderdoc.dll+1290F77  movups xmm2, [r8]`，`r8 = 0` ⇒ 读 0 地址的 16 字节 = `VkFenceCreateInfo` 的 `{sType, pNext}` |
| 调用方向 | 帧 2..9 全是 `poc-presenter.dll`、帧 10 才是 `SkyrimSE.exe` ⇒ **我们**调进去的；栈扫描里 `nvoglv64 ↔ vulkan-1 ↔ renderdoc` ⇒ 走的是 renderdoc 的 **Vulkan 层**（不是 D3D11 钩） |
| 帧数 | `Present → notePresent → pocbFrame → ssrV1Build → ssrV1CubeSync → ssrV1CubeMake → vkCreateFence` 正好 8 层 |
| 日志断点 | 死在 `[2d-5] 深度KMT 导入OK` 之后、**`[v1-cube] 1×1 占位 cube 建好` 之前**；且 `ssr.edge=0` ⇒ 真 cube 分支根本不进，占位 cube 这条路**必经** fence |

> **为什么以前没炸**：全文件 `vkCreate*(..., nullptr, ...)` 只有这一处（`:998` 既有那处传的是正经
> `VkFenceCreateInfo`）。已加一道静态扫描：`vk(Create|Allocate)\w*\(\s*[^,]+,\s*nullptr\s*,`，
> 提交前跑一次，把「必填参数传空」一类一次找干净（本轮 `badNullCreateInfo=0`）。

**③ 顺手修的第二处：占位 cube 建不出不该连带关掉既有 SSR**
`ssrV1CubeSync` 原来是 `bool`，占位图建不出就 `return fail("…")` ⇒ 把整个 v1 关掉。
占位图是 P2 **新加**的东西，拿新特性的失败去关旧功能，等于**违反「任一失败只关自己」**。
已改 `void`：建不出就 `di[4].imageView = VK_NULL_HANDLE`（设备 Vulkan 1.4，空描述符合法）并记日志 ——
`edge=0/1` 分支根本不读 `binding4`，`edge=2` 由 `ssrV1EffEdge()` 退回 0（它只看**真** cube）⇒
既有行为逐位不变，只是 P2 档不可用。

**本地闸**：`check1` **RESULT OK**、`quotescan3` **oddQuoteLines=0**、
`badNullCreateInfo=0`、源文件 BOM=False / 纯 LF 未动。

##### 13 v0.18.16a 拍图判读 → v0.18.16b 定案：`ssr.edge=2` 从来没生效 —— 一道 v0.18.11 的老钳位把 2 静默改成 1

**现象**（用户三张实拍，同一 DLL、零代码判读）：标着 `ssr.edge=2` 的水面不是 cubemap 倒影，
而是**被放大的屏幕内容**；左右边缘有百叶窗条纹与硬矩形分块；**左右窄带有一小块逐位没变**
（用户原话：「左右边缘区域还有一小块区域是没有变化的原本水面」）。

**先记一条走过的弯路**：`ssr.frag` 的 `viewPos()/viewZ()`(:171/:181)、`Rf = reflect(V,N)`(:314)
都是**视图空间**，而 `uProbe` 是**世界空间** cube ⇒ 一度判定为「视图空间 vs 世界空间」错配。
**该假设被实拍反证**：转 180° 之后倒影内容**跟着世界变了**（真错配的话，转 180° 应当复现原机位
那套崖壁/小船内容，实际反的是正前方的蓝树）。真因是**压根没走到那行代码**。

**根因（三证互咬，零代码确认）**

| # | 证据 | 出处 |
|---|---|---|
| ① | ini 回显 `ssr.edge = 2 (v0.18.16 未命中采真 cubemap)`，**紧邻下一行生效值却是 `edge=1`** | `poc-presenter.log` 四发全同：20:25:21 / 21:04:41 / 21:05:36 / 21:07:04 |
| ② | 全文件**没有一行**「`[v1-cube] 真 cube 上传完成`」，也**没有一行** `ssrV1EffEdge()` 的「还没就绪 → 暂按 edge=0 走」 | 后者只在 `g_ssrV1Edge >= 2` 时打印 ⇒ **`g_ssrV1Edge` 从未 ≥ 2** |
| ③ | 画面 = v0.18.11 的**屏幕边缘延展**：内容随世界变 / 屏幕边条纹 / `lastUV == uv` 处逐位回原版 | 三张实拍 + `edge2_vs_edge0_diffmask.png` |

代码（v0.18.11 起就在，当时 `ssr.edge` 只有 0/1 两档，注释也写着「未命中回退源 0/1」）：

```cpp
if (g_ssrV1Edge != 0)
    g_ssrV1Edge = 1;      // ← v0.18.16 P2 加档位 2 时忘了放开: 2 被静默改成 1, 且不打日志
```

后果链，一环扣一环：

1. ini 解析成 2、并打日志「`ssr.edge = 2 …`」⇒ **看起来生效了**；
2. 紧接着老钳位改成 1（**无声，无任何回显**）；
3. `ssrV1CubeSync` 的闸 `g_ssrV1Edge >= 2` ⇒ **永假** ⇒ 真 cube **永不上传**；
4. `ssrV1EffEdge()` 恒走 `return g_ssrV1Edge` ⇒ `pc.p4.w = 1` ⇒ shader 落到
   `else if (w < 0.5 || lastUV == uv)` 那一支 ⇒ `refl` 停在 `texture(uColor, ruv)`
   ⇒ 采的是**屏幕自己**，不是 `uProbe`。

⇒ **P2 从 v0.18.16 起就是「关着的坏功能」，一次都没跑过**；所谓 edge=2 实为 edge=1。

**掩码图给出 hit/miss 的真实分布**（`Screenshot_v0.18.16a ssr.debug=2 samepose .png`，逐行带）：

```
    y  | 纯红miss 纯绿hit 平坦灰128 自然色
 0..199 |    0       0       0      100     ← 不在本 pass 输出层
240..279|    0      27.1      0      72.9    ← 水线, hit 带起
280..439|  0.4~4.6  84~86     0      ~12    ← 远水 = hit
480..759|   ~88      0        0      ~12    ← 前景水 = miss
760..1079| 78→43.6   0        0    22→56    ← 礁石/人物(非水)挤进
```

- **前景水几乎全是 miss（94–100% 红）** ⇒ 修好之后这整片会换成 `uProbe`；**命中只贴着远岸那条绿带**。
- **平坦灰 128 = 0%（全图）** 且上半屏逐点与 edge=2 图几乎相等（例 (900,40) 58,80,84 vs 51,75,79）
  ⇒ **上半屏不在本 pass 的输出层**（游戏后续把那部分重画 / 本层只落到水面），
  与 diffmask「上半屏只变 0.1–5.1%、水面变 92–93%」互相印证。
- ⇒ **判读口径：一律只在水带（y≥240）内做**；同时坐实「改一个 `edge` 只动水面」的契约成立，
  人物/前景礁石属非水面，不受 `edge` 影响。

**修复**（与本诊断同一 commit，只改这一处语义）：按 0..2 钳，只有真越界才回退，并**补日志**
（老钳位是无声的 —— 这正是它能藏住 P2 整整一版的原因）：

```cpp
if (g_ssrV1Edge < 0 || g_ssrV1Edge > 2)
{
    logLine("SSR侦察: ssr.edge 不合法 (" + std::to_string(g_ssrV1Edge) +
            ") → 回退 1 (0 回原版层 / 1 屏幕边缘延展 / 2 采真 cubemap)");
    g_ssrV1Edge = 1;
}
```

**修完的行为（三段递进，逐段有据）**：

| 阶段 | `g_ssrV1Edge` | `ssrV1EffEdge()` | 画面 |
|---|---|---|---|
| ini 解析 | 2 | — | 与 v0.18.16 的解析一致 |
| 真 cube 未就绪 | 2 | 0（并打「还没就绪 → 暂按 edge=0 走」） | **逐位等于 v0.18.15** = 零回归窗口 |
| 真 cube 就绪 | 2 | 2 | 才真正开跑 P2 |

`ssrV1Sig()` 收的是**有效** edge（:3281）⇒ 0→2 变化会自动重录描述符与命令缓冲，不需额外开关；
真 cube 一次上传即可（§12 P1 已证两次读回 FNV 逐位相同 = 内容静态）。

**下一轮待验（P3 —— 真 cube 至今没跑过一次）**：D3D11 → Vulkan 的**面序 / 面内朝向**。
旁证未消：`S4f` 六面导出里**天顶天空落在 face4**，而 D3D11 子资源序的 +Y 应该是 face2
（也可能是 dump 工具自己的排序，未定）。⇒ 换 DLL 后先拍同机位 `edge=0` / `edge=2` 配对 +
`debug=2` 掩码，按配对口径判读；若 cube 内容整体转/翻，再动 `ssrV1CubeMake` 上传侧的面映射。

**本地闸**：`check1` **RESULT OK**、`quotescan3` **oddQuoteLines=0**、
`enc_gate` **ENC GATE PASS**、`badNullCreateInfo=0`、
`main.cpp` UTF-8 无 BOM / CR=0 纯 LF 未动；编译闸 = CI。

##### 14 v0.18.16c：P1「内容静态」是假阳性 —— 读回闸挂在建纹理时刻，12/12 会话都早于游戏首渲 4~24s

**第一层根因（16b 已修、实机坐实）**：`main.cpp:3153` 老钳位把 `ssr.edge=2` 静默改成 1 ⇒ 改 0..2 钳
+ 日志后，16b 日志出现「真 cube 上传完成 1024² × 6 面, 第 1 代 (ssr.edge=2 生效)」。
但画面 FG-left 仍 =(5.8,3.9,1.7) 近黑 ⇒ 还有第二层。

**第二层根因（本轮定案）：读回时机太早，不是 `CopyResource` 静默丢**。
16b 日志 12/12 个会话同一形状：

| 事件 | 时刻（例：22:27 会话） |
|---|---|
| `回写desc=ok`（建 1024² cube） | 22:27:46 |
| `P1探针: 第1次读回`（+2s 闸） | 22:27:50 |
| `P1探针: 第2次读回`（+7s 闸） | 22:27:55 |
| `RSSetViewports 512²→1024² (probe cube 绑定中) 第 1 次` = **游戏首次真正渲染探针** | **22:28:03** |

⇒ 两次读回都落在「游戏一次都没往探针里画」的窗口里（建纹理发生在过加载屏时，首渲在玩家进水面
场景后才来，间隔 4~24s）。读回闸是「建纹理 +2s / +7s」，读到的是刚建出来、没人画过的**裸 0** ⇒
sig 恒等于全 0 的 FNV 反算值 **13856201724594908557**（12/12 会话逐位相同）。
之前 P1 的「两次读回 FNV 一致 = 内容静态 = C1 成立」（§12）是**假阳性** —— FNV 对全 0 也给常数。
附带反证：VK 侧「真 cube 上传完成」(22:22:26 / 22:28:04) 都在首渲后 1s，但拿的仍是早前发布的**零**
镜像 ⇒ `edge=2` 采样全 0 ⇒ FG 近黑。分叉 A（qrenderdoc 抓帧旁证）由此判**不必要**。

**v0.18.16c 修法（诊断 + 修复同一发）**：

1. **读回闸改挂「探针首渲」**：`hookedRSSetViewports`(:1051) 每次命中（= 游戏正在往探针里画）
   首次记 `g_probeCubeRenderT`（新 cube 创建时清零）；`probeDumpTick` 的首读闸从「建纹理 +2s」
   改成「首渲 +2s」（等这一帧内容落定）。
2. **全 0 不算数**：新增阶段1逐面扫描（整行 uint64 OR 快路径，全 0 行免逐像素）——
   六面全 0 ⇒ **不发布、不计成功**，1s 节流重试，上限 15 次；每次打「首渲信号有/无」+ 每面
   （非0像素数 / 量级最大值 `raw & 0x7FFF`）。有信号仍全 0 ⇒ 指向 `CopyResource` 没落地 (b)；
   无信号 ⇒ 指向时机 (a)。首渲信号挂 `RSSetViewports` 命中，**无需任何新钩子**。
3. **兜底**：建好 60s 仍无首渲信号（钩没命中？）⇒ 记一次日志后按老口径直读，不永久哑掉。
4. **C1 判据不变**：首读非 0 才发布（`++g_probeCpuTick` ⇒ VK 上传）；第 2 次读回仍 +5s 比 FNV
   判「内容静态 / 需按代重传」——这次是在**真内容**上比，不再是全 0 假阳性。

**换 DLL 后判读**（同机位 `edge=0` / `edge=2` 配对 + `debug=2` 掩码，机位闸 / `knobchk` 照旧）：

- 日志先看 `P1探针: 面统计 nz/量级max f0=… f5=…` —— 六面 nz 全 0 ⇒ (a)/(b) 二分见上；
  nz > 0 ⇒ 内容到位，看 FG 是否从 (5.8,3.9,1.7) 抬起来；
- 再看「第2次读回 … ⇒ 内容静态 / 内容有变」——「有变」不失败（`ssrV1Sig` 按代重传已兜住），
  但意味着后续要从「一次上传」改成周期性重传（挂队列）。

**本地闸**：`check1` **RESULT OK**、`quotescan3` **oddQuoteLines=0**、
`enc_gate` **ENC GATE PASS**、`badNullCreateInfo=0`、
`main.cpp` UTF-8 无 BOM / CR=0 纯 LF 未动；编译闸 = CI。

##### 15 v0.18.16c 实拍判读（2026-10-09）：读回闸修对了 —— 六面全非零、双掩码配对 PASS；探针内容是动态的 ⇒ 周期性重传入队（队列 #7）

**换 DLL 后日志（两个会话同形状，判据 = 上文「换DLL后判读」列的两行）**：

| 事件 | 会话1 | 会话2 |
|---|---|---|
| `回写desc=ok`（建 1024² cube） | 00:57:01 | 00:59:11 |
| `RSSetViewports … 第 1 次`（首渲，16c 读回闸挂这里） | 00:57:24 | 00:59:29 |
| `P1探针: 面统计 nz/量级max f0..f5` | `1048575~1048576 / 14763~15908` | 同 |

- 六面 nz ≈ 1024² 满像素、量级 ~1.5 万（半 float 尾数）⇒ **cube 内容真到位**；
  读回闸挂首渲后不再读裸 0 —— 16b 时代「sig 恒为全 0 常数 `13856201724594908557`」已消失，
  sig 脱离常数 ⇒ C1 判据这次走到了「真内容」分支。
- `真 cube 上传完成 1024² × 6 面, 第 1 代 (ssr.edge=2 生效)` + `第 2 代` ⇒
  16b 的 0..2 钳与 16c 的内容闸都生效（`edge=2` 首次真正跑起来）。
- **第 2 次读回 sig 变了**（会话1 `2103105937634954453`→`6209820897396773986`；
  会话2 `7130806349903485787`→`7565397293480307069`，两次读回相隔 5s）⇒
  探针每帧重渲、内容**动态**，C1「两次一致 = 静态」不成立；而 `g_probeDumpN>=2` 后
  永久停在第 2 代会越来越陈旧 ⇒ **「cube 周期性重传」入队（队列表 #7）**。

**双掩码配对判读（`Screenshot_v0.18.16c edge=0 debug=2.png` / `Screenshot_v0.18.16c edge=2 debug=2.png`，同机位闸 PASS）**：
全图逐像素分类（绿=hit、红=miss、其它=非本层）得混淆矩阵（行=edge0 类，列=edge2 类）：

| | edge2 G | edge2 R | edge2 O |
|---|---|---|---|
| **edge0 G** | 217965 | 78 | 240 |
| **edge0 R** | 30 | 1015828 | 4655 |
| **edge0 O** | 302 | 830641 | 3861 |

- hit/miss 类翻转 `GR+RG` = **108 px**（水带内 108/1238796 ≈ 0.009%）⇒ hit/miss 分裂跨 edge
  几乎逐位一致 = **「`edge` 只改 miss 分支内容、不碰 hit」契约成立**。
- 掩码↔场景翻转 `RO+OR+GO+OG` = 9058 px 全贴水线边界 ⇒ 两次会话（相隔 2 分钟）**≤2px 抖动**，
  不是机位漂移 ⇒ **机位闸 PASS**。
- 原始像素差 735080 px（35.4%）几乎全落在非掩码场景区（`OO`=830641 内的场景动画/亚像素位移），
  掩码区内平色逐位一致。
- **掩码判不了 cube 内容**：FG 是否从 (5.8,3.9,1.7) 抬起，仍需 `debug=0` 的 `edge=0`/`edge=2`
  同机位配对正常图（待拍）。

**同批：启动横幅精简（本批唯一代码改动，`main.cpp` `SKSEPlugin_Load` 内单条 `logLine`）**：
原横幅 **4939 字符**，把 v0.18.1…v0.18.16 整部版本史内联进标题；改 **324 字符**当前版短标题，
版本史改指向 git log 与 docs。下游契约逐条保住：

| 消费方 | 依赖 | 新横幅 |
|---|---|---|
| `tools\knobchk.ps1` | 末条 `==== poc-presenter v` 行作会话头；`poc-presenter (v[0-9.]+)` 取版本；扫描从 `$last+1` 起（横幅恰一条 `logLine`） | 前缀保留、版本仍 `v0.18.16`、单行、引号=2、全仓 `bannerLines=1` |
| `tools\run2c.ps1` | `.Contains("==== poc-presenter ")` 切片；#0 信息行查 `ssr.shared` / `Step2c-β` / `ssr.sentinel` / `ssr.vkout` 四 token | 四 token 全在（`Step2c-β` 用码点 `[char]0x03B2` 复核 —— 直接在无 BOM 的临时脚本里写字面量会被 PS5.1 按 ANSI 读成假阴性） |

（`run2c` #0 的 `expect v0.18.14` 早在 v0.18.15 起就已陈旧、与本批无关，未动。）

**本地闸**：`check1` **RESULT OK**、`quotescan3` **oddQuoteLines=0**、
`enc_gate` **ENC GATE PASS**、`badNullCreateInfo=0`、
`main.cpp` UTF-8 无 BOM / CR=0 纯 LF 未动；编译闸 = CI。

##### 16 v0.18.16d debug=0 同机位配对实拍：契约 PASS、数字闸 PASS、**内容 FAIL** ⇒ 卡 P3（采样方向）

**两张图**：`Screenshot_v0.18.16d debug=0  edge=0 .png` / `Screenshot_v0.18.16d debug=0  edge=2 .png`
（`debug=0` 后双空格、`.png` 前有空格；同机位 = 场景带 y40..280 目检与 16c 掩码对齐
✓ 船/对岸/右岩/人位置一致，`-SceneChecked` 前置义务满足）。用户原话：
「edge=2 时除倒影区域以外的区域全部出现问题了」。

**`px_cond -Normal` 双跑**（同一张 `Screenshot_v0.18.16c edge=0 debug=2.png` 掩码）：

| 组 | edge=0 | edge=2 | 判 |
|---|---|---|---|
| HIT mean / p50 | 0.4232 / 0.4078 | 0.4016 / 0.3843 | **定住**（p50 −5.8% = 会话内动画/光照漂移量级）⇒ A 回归闸 PASS |
| MISS mean / p50 | 0.2144 / **0.1765** | 0.4212 / **0.4235** | **动了 2.4×** ⇒「MISS must MOVE」PASS |
| MISS structure (p90−p50) | 0.1412 | 0.2353 | 1.67× |

- edge=0 发的工具 VERDICT 复现 P0 基线（miss/hit p50 = 0.433 vs 上次 0.437）
  ⇒ 机位/掩码对齐坐实（两发 n 完全同值 240238 / 1050046 = 同一分类）。
- edge=2 发工具自动结论「B is SMALL / PRIORITY LOW」**不采信**：它只看 miss/hit 比值
  （现 1.102 就判「已闭合」）；验收原文要求 fan boundary **目检** —— 目检 FAIL（下条）。
  数字上 miss 比 hit 还亮（p50 1.102×、structure 1.154×），但亮的是河床不是倒影。

**目检判读（决定性）**：
- hit 带（远岸绿带对应水域）edge=0/edge=2 内容一致正常 ⇒ 契约「edge 只动 miss、不碰 hit」成立；
  掩码里的绿块（蓝结构倒影那一格，x≈380..580/y≈340..480）两张同内容 = hit 不动的直接目证。
- miss 区整片被替换为**黄绿色水下河床样内容**（左中一块蓝结构倒影残片），边界形状 = 掩码扇形波浪线
  ⇒ **采样链路全通、`edge=2` 首次真生效**（16c 内容 + 16d 采样链路都走通），**但采到的方向/面是错的**
  ——不是倒影。右侧灰岩是真实岸壁（非水面几何，不受 edge 影响），非 cube 内容。

**根因**：

1. **（代码实锤）视图空间直喂世界 cube —— 即文件头挂账的 P3**：`ssr.frag:313-314`
   `V=normalize(P); Rf=reflect(V,N)` 全在**视图空间**（P/N 由深度反推），`:496`
   `texture(uProbe, Rf/rl)` 直接把视图空间方向当世界方向采世界空间探针
   ⇒ 采到哪个面随相机姿态乱转。文件头 `:126-128` 早写明「视图→世界缺相机朝向数据，
   归 P3 朝向校准」，`debug=0` 实拍首次显形。
2. **（观察推断，待方向修完实拍定）D3D↔VK cube 轴约定疑叠加 Y 翻转**：俯拍机位下
   Rf_view 分像素手算（θ=20°）得 (0,0.87,−0.5) 等恒 y>0 —— 直喂应读到「上」面（天空/崖顶）；
   实拍却是河床样内容 ⇒ 方向 Y 分量疑被翻（D3D 左手系 vs VK/GL 右手系手性差）。
- 上传侧**无嫌疑**（已排除）：`g_probeCpu` 逐面 `sub=L` 顺序读回（D3D 面序 +X,−X,+Y,−Y,+Z,−Z）
  → `ssrV1CubeMake` 紧密 memcpy 直灌 VK **同序** 6 层，面序一致、行距 8192=W*8 无跨面串行。

**P3 修法 = 两件事**（队列 #2 现状：P2 通路走通、卡 P3）：

1. 视图→世界旋转 `Rf_world = M_camRot · Rf_view`。**缺相机朝向数据源**：深度只能反推视图空间
   + 世界 Y 轴（水面法线给出 1 列），**yaw 不可观测**，必须外部数据。push 已 96B 满
   （`p5.w`=v1det，`:557`）⇒ 扩 p6=16B 四元数（96→112B，≤设备下限 128B，代价 = `ssrV1Sig()`
   重录命令通路）或新增 UBO binding（描述符每帧重写通路已有）。数据源候选：
   **A** = 固定版游戏（1.5.97.0）相机对象签名/固定地址读变换（RE 风险，本项目尚无 RE 基建；
   版本已钉死、错则目检可见、可截图迭代）；
   **B** = 挂 `VSSetConstantBuffers` 抓游戏 view 矩阵所在 cbuffer + `CopyResource`（槽47 已挂）
   到 staging 下帧 `Map` 读（1 帧延迟可接受；需正交性校验识别哪个 buffer，GPU 读回复杂度高）。
2. 轴/手性校准（若 1 修完仍不对）：采样处 `(d.x, −d.y, ±d.z)` swizzle 或上传侧重排面序，
   **对照游戏自己的 585 反射**（游戏采同一块 cube 是对的）定符号 —— 靠 585 对齐校。

**FG 亮度悬案（43.7% 缺口）**：edge=2 的 miss p50 已 1.102× HIT，但那是河床不是倒影 ⇒
**被错内容淹没，P3 修完重拍 `debug=0` 配对再判**。

##### 17 v0.18.17 P3 朝向校准实装（方案 B：槽7 cbuffer 读回；队列 #8）

**拍板**：数据源 = **B 挂 `VSSetConstantBuffers` 读游戏 cbuffer**（A 游戏内存签名读被否）；
传输 = push 扩 **p6/p7，96B → 128B**（128 = 规范下限 `maxPushConstantsSize`，必合法），
不走 UBO —— host-visible UBO 虽免重录，但要新增资源+描述符+内存类型选择，动的面比
「重录一条本就每帧跑的提交链」更大；每帧重录代价 ≈ vkBegin/End 一条小 cmdbuf（µs 级），
而 fence 每帧本来就全同步 ⇒ 无新增停顿。

**槽号双证（`VSSetConstantBuffers` = 槽7）**：
- 官方 `d3d11.h` MIDL 声明序：IUnknown 0-2 + `ID3D11DeviceChild` 3-6 之后的**第一个**接口方法
  即 `VSSetConstantBuffers` ⇒ raw index 7，与 33/47/50 同一张权威表（`docs\analysis\d3d11-ctx-slots.txt`）。
- 安装口与槽47 同款 `isD3D11Family(vtbl[7])` 阀：原值必须来自 d3d11/renderdoc，不合格**只降级槽7**
  （不挂），33/47/50 照常 —— 挂错槽的代价 = 游戏每帧绑定全丢（比 47 挂错更致命），阀不能省。
- 日志**新增独立行**（`ctx slot7(VSSetConstantBuffers) 原值=… 来自 …` / `ctx槽7 … 已挂 —`），
  **不改既有 `vtable=` 与 `ctx槽33/50` 行格式 ⇒ run2c/工具 grep 口径零影响**。

**读回链路（全在 `main.cpp`，只读不动游戏任何绑定）**：
1. 槽7 钩 `hookedVSSetConstantBuffers`：本帧在 `ssr.camslot` 槽上**第一次**非空绑定的 buffer →
   当场 `CopyResource` 到自建 STAGING（`g_ssrSelfCopy` 包住 ⇒ 不进 [2a] 的 COPY= 计数与身份学习）；
   每帧至多 1 次（帧号闸，其余调用整型比较早退）；staging 按源 desc 惰性重建（ByteWidth/
   MiscFlags/StructureByteStride 三身份；先原样 MiscFlags 建、失败退 0 —— 同 probeDumpTick 口径）。
2. `notePresent` 末 `camMapTick()`：帧末纪律同 `probeDumpTick`（不插进游戏记录流、Map 不卡 draw；
   上下文从 staging `GetDevice→GetImmediateContext` 自取，不持悬垂指针）；上一帧发的拷贝此刻
   GPU 必已完成（本文件每处 submit 都紧跟 fence wait）⇒ 零卡顿；Map 成功才清 pending，失败下帧重试。
3. 解析 `camParse`：每 16B 对齐偏移取 4x4 左上 3x3，行两两正交 + 单位长 + **det≈+1** 才入候选
   （上限 8）→ 按 `ssr.camidx` 选 → Shepperd 提取四元数归一化 → `ssr.camconj` 共轭位 → 发布
   `g_ssrV1CamQ`。要点：**行列存法与 view/invView 的二义都只差转置 = 四元数共轭** ⇒ 全部折叠成
   一个共轭位；det=-1 镜像块拒收；轴符号二义不在这层（交 p7.xyz）。日志：解析前 6 次 + 之后每
   128 次打一条 `[cam] 解析#N bytes=… 候选=K [#0@0,#1@256,…] 选 idx=… q=(…)`；连续 180 帧无候选
   且 `camslot=-1` 自动轮槽（0→1→2→3→0，可 ini 固定）。

**VK 侧**：
- push 96B → **128B**：`pcr.size=128`、struct 增 `p6`=相机四元数、`p7`=(轴符号 ±1×3, w=开关)；
  p7.w = `camrot 且 q≠(0,0,0,0)`，**q 全 0 ⇒ 不旋 = v0.18.16d 逐位一致**（逃生门语义保留）。
- `ssr.frag` edge=2 采样处（**只动采 uProbe 这一步，行进仍用视图空间 Rf** —— ray march 是屏内几何
  与世界无关）：`rw = Rf + 2×cross(q.xyz, cross(q.xyz, Rf) + q.w×Rf)`（单位四元数旋转，保长）→
  `d = normalize(rw) × p7.xyz`。
- **相机进 `ssrV1Sig()`（仅 `ssrV1EffEdge()>=2` 时掺）**：push 内容在录制期写死 ⇒ 必须重录才生效；
  edge<2 不掺，避免相机一动就无谓重录。配套把 `ssrOutVkFrame` 的 dirty 分支从「重录后 return 不提交」
  改成 **「重录成功后当帧照常提交」**（录失败仍 return，RecSig 已清 0 下帧重试）：相机一动 sig 就变，
  旧行为会变成*永远只重录不提交 ⇒ 出向冻结*；本文件每处 `vkQueueSubmit` 都紧跟 `vkWaitForFences`，
  上一帧 cmdbuf 必已 retire ⇒ 重录 + 当帧提交合法。

**五个新旋钮**（默认全安全 = 不改 ini 也能跑）：

| 键 | 默认 | 语义 |
|---|---|---|
| `ssr.camrot` | 1 | 总开关；0 ⇒ p7.w 恒 0 = v0.18.16d 行为 |
| `ssr.camslot` | -1 | -1=自动（条目17 为 ~3s 无候选轮槽 0..3；**条目18 改为 14 槽全扫 + 首候选锁定**）；0..3=固定 VS 槽（**条目18 起扩为 0..13**） |
| `ssr.camidx` | 0 | 候选 4x4 下标（看 `[cam]` 日志选） |
| `ssr.camconj` | 0 | 1=取四元数共轭（view/invView/行列二义一位切换） |
| `ssr.cubeflip` | 0 | bit0=x bit1=y bit2=z 置 1 = 该轴取反（D3D↔VK 手性校准） |

**校准协议（实拍定符号，判读口径同 §14.16）**：
1. ini 保持 `ssr.edge=2`、`ssr.debug=0`、其余默认 → 重启 → `dllchk`→改 ini+重启→`knobchk` 三闸
   → 与 16d **同机位**拍一张 debug=0。
2. 目检：miss 区应呈「远岸倒影向近处的自然延续」；同时抓 `[cam]` 日志（候选数/offset/选中 q）。
3. 不对按表迭代（每轮 = 改 ini + 重启 + 一拍，**无需重编译**）：
   - 乱面/整片换内容 → `ssr.camidx` 换候选；
   - 方向反着转/镜像 → `ssr.camconj=1`；
   - 整体错轴（上下/左右翻） → `ssr.cubeflip` 置 2(y) / 1(x) / 4(z) / 组合；
   - 全程无候选 → 按 `[cam]` 轮槽日志用 `ssr.camslot` 固定到有候选的槽。
4. 定符号后与游戏自身 585 反射并排对照（§14.16 口径）过 = 方向修完；随后复拍 debug=0 同机位配对
   重判 FG 亮度 43.7% 缺口。

**改动面**：`ssr.frag`（PC + 采样点 + 文件头 P3 注记）、`vkrenderer.h`（3 extern）、
`vkrenderer.cpp`（pcr=128B / struct+fill / sig / dirty 当帧提交 / 就绪日志 5 binding）、
`main.cpp`（`SsrCtxEntry.orig7`、钩+解析链路、installSsrRecon、5 旋钮、notePresent、
横幅 v0.18.16→**v0.18.17**、补 `<cmath>`）。编译闸 = CI（本地无编译）。

> **⚠️ 条目18 修订**：本条描述的「自动轮槽」实拍证实是**死锁**（没拷就没解析、没解析就换不了
> 槽 ⇒ 永远卡槽0，q 恒 0），v0.18.18 已改为 14 槽全扫 + 首候选锁定 + 绑定普查 —— 见下条目18。

---

##### 18 v0.18.18 P3 读回死锁修复：VS cb 槽 0..13 全扫 + 每槽独立 staging + 绑定普查（队列 #8 二段）

**为什么还有这一发（v0.18.17 实拍日志诊断，2026-10-09）**：v0.18.17（`b2ac5f6`，CI SUCCESS）
用户实拍回报「还是问题严重」，但日志证明**旋转链路从未生效** —— 全会话只有两条 `[cam]` 行：

| 日志证据 | 含义 |
|---|---|
| `[cam] ini camrot=1 camslot=-1 camidx=0 camconj=0 cubeflip=0` | 五旋钮默认值读入正常（启动回显） |
| `[cam] 解析#1 bytes=128 无正交旋转候选 (槽0)`（13:55:08 加载期） | **全程唯一一次解析**，此后零拷贝、零解析 |

⇒ `g_ssrV1CamQ` 恒全 0 ⇒ `p7.w=0` ⇒ 着色器不旋 ⇒ 画面 = 16d 原行为。截图 miss 区的放射状
涂抹是该机位下视图空间直喂的表现，与 16d 的差异仅机位差，**不构成朝向判读依据**。
其余链路全正常（已排除）：`v1 就绪 128B / 5 binding`、`edge=2` 全程、`[2a]` 心跳照常、
无 `ssrWatch` 身份失配、`ctx vtable=` 10 行 = 5 次启动×2 行（探针+SSR）装钩正常。

**根因 = 结构性死锁（不是朝向算错）**：
- 拷贝条件 = 「`camslot` 槽上有非空绑定」，而 `camslot=-1` 时当前槽 `g_camCurSlot` 初始 0；
- 自动换槽判据 = 「`camParse` 里连续 180 次无候选」；
- **没有拷贝 ⇒ 没有解析 ⇒ `g_camNoCand` 永不增 ⇒ 永远卡槽0** —— 判据的输入依赖判据自己的输出；
- 实拍里槽0 恰只在加载期绑过一次 128B（无旋转块）⇒ 链路从此静默，日志不再有任何 `[cam]` 行。

**修法（仅 `main.cpp`，shader/p6p7 语义不动）**：

1. **14 槽全扫**：`camslot=-1` 时本帧对 VS 常量缓冲槽 0..13 **每个**槽取第一次非空绑定 →
   各自 `CopyResource` 到**每槽独立的 staging**（帧号闸按槽记，每帧每槽至多 1 次；
   staging 身份 ByteWidth/MiscFlags/StructureByteStride 三元组按槽惰性重建）。
   `camslot=0..13` 固定值语义不变（只拷那一个槽），ini 校验从 0..3 放宽到 **0..13**。
2. **锁定替代轮槽**：`camParse` 带槽号参数；全扫时**首个产出正交候选的槽**写进 `g_camLockSlot`
   并打 `[cam] 自动锁定 槽N` 行，锁后只拷/只发布该槽（收敛回单槽成本）。换槽判据从
   「解析次数」改成「真实解析结果」，死锁结构性消失；180 次轮槽分支已删除
   （`g_camCurSlot`/`g_camNoCand` 两个全局已删）。
3. **绑定普查 `camCensus`**：每 ~300 帧（首拍提前到 ~60 帧）打一行
   `[cam] 普查#N 帧=F (槽:非空/空/字节/拷) 槽0:b/n/w/c 槽1:… 槽13:…`
   —— 一拍看全各槽绑定次数 / 空绑次数 / 最近真拷贝字节数 / 拷贝次数，
   **相机 cb 在哪槽直接从这行读**；计数在 `camVSBOnce` 里逐绑定累计（纯整型，无额外 vtable 调用，
   字节数只在真拷贝处取 `GetDesc`）。
4. **`camMapTick` 帧末逐槽 Map/Unmap**：全无 pend 时帧级早退；设备从任一有 staging 的槽
   `GetDevice → GetImmediateContext` 自取（与 `probeDumpTick` 同纪律，不持悬垂指针）；
   Map 成功才清该槽 pending，失败下帧重试。

**新日志速览（实拍判读口径）**：

- `ctx槽7(VSSetConstantBuffers) 已挂 — v0.18.18 P3 相机朝向读回 开 (camrot=1, VS cb 槽0..13 全扫)` = 新 DLL 装钩；
- `[cam] ini camrot=… camslot=… camidx=… camconj=… cubeflip=…` = 五旋钮启动回显（**行格式不变，knobchk 口径不变**）；
- `[cam] 普查#…` = 各槽绑定画像（定槽依据）；
- `[cam] 自动锁定 槽N` = 相机 cb 所在槽已锁定（此后可 `ssr.camslot=N` 固定，需重启）；
- `[cam] 解析#… 候选=… q=(…)` = 朝向真发布；q 全 0 或无解析 ⇒ `p7.w=0` = 16d 行为。

**五旋钮语义变化**：仅 `ssr.camslot` —— 默认 -1 仍为自动，含义从「~3s 无候选轮槽 0..3」改为
「14 槽全扫 + 首候选锁定」，固定值范围 0..3 → **0..13**；`camrot/camidx/camconj/cubeflip`
不变，回显行格式不变。五旋钮默认值仍全安全（不改 ini 即可跑）。

**改动面**：仅 `main.cpp` —— 全局状态改 14 槽数组 + `g_camLockSlot` + 普查计数、
`camStgRelease(int)` 按槽释放、`camParse(…, int slot)`、`camMapTick` 逐槽、`camCensus` 新增、
`camVSBOnce` 全槽扫描、ini 校验与回显、挂载日志行、横幅 v0.18.17→**v0.18.18**。
本地闸：`check1` RESULT OK / `quotescan3` oddQuoteLines=0 / `enc_gate` ENC GATE PASS /
`badNullCreateInfo=0`；编译闸 = CI（本地无编译）。
**判新旧注意**：本发 shader/spv 未动 ⇒ `dllchk` 的 spv 内容闸对 17/18 两版 DLL 都会 PASS，
**版本以日志横幅 `==== poc-presenter v0.18.18` 为准**；`dllchk -After "<commit 的 %ci 时间>"`
可查 C++ 侧新旧（artifact 时区已归一）。

**下一发交接（实拍三闸照旧）**：artifact zip 覆盖 DLL → `dllchk` → ini（`ssr.edge=2`、
`ssr.debug=0`，旋钮全默认）→ 改完重启 → `knobchk` PASS → **同机位**拍图 + 回传含
`[cam] 普查` / `自动锁定` / `解析` 行的完整日志 → 按普查行确认锁定槽 →
迭代 `camidx`（乱面）/ `camconj=1`（反转镜像）/ `cubeflip`（错轴）至 miss 区呈
「远岸倒影向近处自然延续」→ 与游戏 585 反射并排对照过 → 队列 #8 收口、#2 走通。
