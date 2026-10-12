# 天空 Pass（Sky / Atmosphere / Volumetric Clouds）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 以原生 VK 侧 pass 接管游戏天空渲染并做视觉升级（天空 + 大气散射 + 体积云），效果优先、性能分档可调，不扰动既有配对锚点。

**Architecture:** 沿用 docs/05 节点替换模板（已测边界 → 同位置替换 → declared-diff 预案 → 闸）与架构口径①（docs/00 §1.1.1：合成式 + 自写渲染器立为主干，新增 pass 代码全在 `vkrenderer.cpp`）。**默认路线 = 段7-end 全帧合成回写**：全仓库无天空 draw 定位（Research B 否定结果，仅段7 主不透明 MRT×3 为最强嫌疑）+ Draw Hook 不存在（docs/05 L65-67）+ 无 HLSL 工具链 ⇒ 出路只有段级插入 —— 段7 结束（ev38661 换绑）时我方发行 321 快照拷贝，VK 合成（远深掩码 = 搭车游戏 ev21505 的 461→520 深度拷贝），回写 321，须在段9（ev38816）消费前完成。大气/体积云为纯增量 pass（docs/03 L135：9 既有 CS 逐字节同构 ⇒ 新增不扰锚点）。探测探针 501 内的天空留待 Plan 2 dynamic cubemap 统一（本计划不动段1）。质量控制走 `sky.preset` 档位 + 子旋钮，档位表编入 DLL、换档只改 ini。

**Tech Stack:** C++（SKSE 插件 DLL，D3D11 钩子 + PoC-B Vulkan 注入渲染器）；GLSL → SPIR-V（`tools/make_shaders.ps1` 仅支持 vert/frag，条目表 L53-56；体积云 = frag 半分辨率 raymarch，无 .comp 工具链改动）；RenderDoc 抓帧 + `tools/rdc_compare` 配对判定；GitHub Actions CI（`build.yml` L78 编 main.cpp + vkrenderer.cpp 成同一 DLL）为唯一编译闸。

**Spec:** 本文件 §Spec（用户需求 2026-10-12 会话原话 + 门槛）；上游文档：`docs/03` §6.2（L133-138）+ §7.2 天空进入门槛（L218）、`docs/00` §4.6（L470-476）/ §4.7（L478-489）。

## Global Constraints

- 编译：本地无编译；GitHub Actions CI 是唯一编译闸。shader 变更必须本地跑 `tools/make_shaders.ps1` 重新生成 `pocb_shaders.h`（生成物勿手改）后 spv 一并入库，`tools/spvsync.ps1` 强制源/spv 字节一致。
- 代码位置：新增 pass 代码全部写进 `src/poc-presenter/vkrenderer.cpp`（`vkrenderer.h` L8-10 约定）；main.cpp 只保留 Present/vtable hook 面 + D3D11 interop + 日志/ini；build.yml 只编两个 TU ⇒ **默认不新增编译单元**。
- 钩子面：ctx 槽 33/47/50 挂载门在 `installSsrRecon`（main.cpp L3600-3616），现条件 = `pocbEnabled && probe && ssr` ⇒ 天空必须扩门（T3）。**Draw Hook 不存在**（docs/05 L65-67）+ **无 HLSL/DXBC 工具链** ⇒ 一切「按 draw 抑制/替换」与「D3D11 侧新增 draw」的路线出局。
- 硬约束：生产通路**禁止每帧同步读回**（docs/00 L109）——必须共享纹理（NT handle）+ fence 同步。
- 推送：`git -C C:\Users\joker\skyrim-vulkan -c core.quotepath=false push --no-thin origin master`；两条 git 命令不可并行执行。
- 文档编码：UTF-8 无 BOM 纯 LF。PS 脚本纯 ASCII（PS5.1 按 ANSI 读无 BOM UTF-8、CJK 字面量必失配），以 `powershell -NoProfile -ExecutionPolicy Bypass -File` 执行；CJK 路径不经 shell -Command 传递。
- 提交：docs + 证据 + 代码同一提交，UTF-8 `-F` 消息文件；五闸 `tools/gates4.ps1`（check1 / quotescan3 / verify_banner / enc_gate / spvsync）全 PASS 才许 push。
- 换 DLL 后：`tools/dllchk.ps1 -After "<commit %ci 本地时间去时区后缀>"`；改 ini 后：`tools/knobchk.ps1` 验回显（回显缺失/错值 ⇒ 拍摄作废）。
- 许可：CommunityShaders = GPL-3.0 WITH Modding Exception + Linking Exception（docs/01 L11-14）——**禁止复制其代码**，仅可借鉴思路与接口信息，实现独立重写（Bruneton/Hillaire 类方案按公开论文思路自写）。
- 验收总公式 = **5% 闸 + 10 锚点**（docs/00 L254）：
  - **5% 闸**（docs/00 L167-168/L246-247）：新增 pass 增量帧时 (注入−基线)/基线 ≤5%（16.67ms 预算的 5% ≈0.83ms，约束「别花超」不约束「必须变快」）；基线 = ssr=0 会话同机位采、候选注入局同机位采，每 600 帧『帧时基线 CPU/GPU』行、字段口径 docs/02 §14.14。
  - **10 锚点**：`tools/rdc_compare.py` + `.ps1` 把 docs/03 §3 锚点表落成 10 条 PASS/DIFF/SKIP（id-free 签名）；「锚点全绿」分级口径 = A 类逐帧确定**硬门槛**全绿 + F 类注入指纹**必现** + B 类会话敏感项按 docs/02 §12.6 看图**归因后无注入不可解释项**（docs/03 L91；字面 10/10 仅同帧自比成立）；`errors=[]` = 本轮成功（docs/03 L83）。
- 新增节点约束：大气/体积云必须是**纯增量**，既有 9 CS 字节码/调度/资源链零改动（docs/03 L135）；R2 = ResourceId 抓帧局部量，跨抓帧对齐用 (绑定槽, 尺寸, 格式, 哈希)（docs/03 L110）。
- 单变量轮：每轮只动一个视觉变量再复拍判读（本仓惯例）。
- 质量优先（用户 2026-10-12）：闸内选效果最好的技术路线；分档用于暴露取舍而非降低上限。
- 性能分档（用户 2026-10-12）：`sky.preset=0..3`（低/中/高/极致，缺省 2）主控；子旋钮独立可覆盖（微调不换档、换档不覆盖微调）；档位表编入 DLL ⇒ **换档 = 改 ini 不重编**；启动回显 + knobchk 可验。
- 降级判据（docs/00 L175-176）：单 pass 连续两次迭代锚点与帧时目标双不过 ⇒ 回退口径③（自建呈现），前置补齐「不抢 HWND」验证；逃生门链 `ssr=0 → probe=0 → sky=0 → vulkan=0 → 口径③`（sky 一级由本计划 T3 新增）。

## Review Focus

（规格暗示、但没有任务天然覆盖的失败面；每条已回填所属任务测试。）

1. **A/F 锚点回归** → T7：新天空 pass 扰动硬锚点（RT 格式 / CS 调度 / CS 字节码 / clear / copy 结构）或 F 类注入指纹缺失 ⇒ 配对报废。测试：S1 + S3 注入前后 `rdc_compare -Strict`（A 全绿 + F 必现 + B 按 §12.6 归因）。
2. **室内场景污染** → T3 + T7：室内帧无天空（户外条件节点口径 5 常驻 + 4 户外条件，docs/analysis S3 L88）⇒ 室内不得产生段7-end 触发/新增节点。测试：T3 日志断言 S2 会话触发计数=0；T7 S2 配对 + 出门/入门复拍。
3. **档位回显与回退** → T4 + T7：preset 解析错/回显缺失 ⇒ 整轮作废；0 档必须是可用回退。测试：knobchk 逐键 + 档位切换重启回显全键值。
4. **天空→反射耦合** → T7（Plan 2 收口）：501 探针内容 = 天空 + 场景（docs/03 §2.3），本计划不动段1 ⇒ 反射里暂时仍是原版天空（与升级后实天空不一致的中间态，声明为已知项）。测试：S4 水面复拍不劣化；一致性由 Plan 2 dynamic cubemap 统一。
5. **预算** → T7：preset=2 在 GTX 1660 Ti 增量 >5% ⇒ 闸不过（本路线两次全帧拷贝 + 事件闸 CPU 停顿是主要嫌疑，docs/07 §4 钉预案）。测试：S1/S3/S5 同机位基线/注入对比，逐档回填档位表。
6. **时域伪影** → T6 + T7：云走时域复用（preset≥1）时镜头急转/进出室内拖影。测试：T6 快摆无残影累积；T7 出入门往返复拍。
7. **SSR 关停承诺不被破坏** → T3 + T7：`sky=1` 时 `ssr=0/probe=0` 语义不得变质 —— SSR/探针/普查日志不复活、`ssr=0` 时槽位零开销承诺改为「仅 sky=1 时挂」。测试：T3 审计挂载门/C-5 文案/各 logLine 的 g_ssrOn 前提；T7 grep 断言 `SSR侦察:` 不出现、`天空: sky=1` 出现。

## Spec（真源）

**用户需求（2026-10-12 会话原话）：**

1. 「视觉升级，除了基础的天空，大气，体积云这些，阴影，环境光，dynamic cubemap这些也要有」——阴影/环境光归 Plan 3（光照 pass）、dynamic cubemap 归 Plan 2，本计划只含天空/大气/体积云。
2. 「视觉升级以效果最好的方式来。」——技术选型以效果为先（在 5% 闸内）。
3. 「性能方面可以做出几档选择，方便后面配置升级了做改变。」——分档设计见下。

**进入门槛（docs/03 §7.2 L218 原句）：** 天空 = 配对通过 + §4.6 大气/体积雾基础节点在 Vulkan 侧可挂载。前置（§4.7 L480）：API 配对完成、基线复现与收益验证通过 —— 已达成（阶段1 全绿收口 2026-10-04）。

**技术方向（docs/00 原文）：**

- §4.6 L473：天空 / 大气渲染 — **Precomputed Atmospheric Scattering / Bruneton**。
- §4.6 L474：体积云 / 雾 — **Ray Marching + 3D Noise Texture**。
- §4.7 L489：天空现代化 — 大气散射 / 体积云升级（4.6 的升级版）— **顺位 2**（与水体共享 cubemap/CB 输入面，复用同一组配对锚点）。
- §4.7 L486：扩展顺序 = ① 水体 → ② 天空 → ③ 光照 → ④ 材质 → ⑤ 超分 → ⑥ 帧生成（本机出局）。
- 证据完备度：高 —— VL 链全量在档、太阳方向/雾参/体素域在 per-frame CB（docs/03 L135）。

**分档设计（起点值即缺省档位表，实施轮内可按实测调）：**

| ini 键（子旋钮） | 0 低 | 1 中 | 2 高（缺省） | 3 极致 |
|---|---|---|---|---|
| `sky.preset` | — | — | **2** | — |
| `sky.cloudscale` 云渲染分辨率比例 | 0.50 | 0.66 | 0.75 | 1.00 |
| `sky.cloudsteps` raymarch 步数 | 32 | 48 | 64 | 128 |
| `sky.cloudtemporal` 时域复用 | 0 | 1 | 1 | 1 |
| `sky.lutscale` 天空视图 LUT 分辨率倍率 | 0.5 | 1 | 1 | 2 |

规则：`sky.preset` 先解析展开为子旋钮缺省值，随后逐键读显式 ini 覆盖（微调不换档）；全部键进启动回显行，knobchk 可验；0 档同时是**性能回退档**。

**验证资产（已就位）：** 场景集 S1 白天白漫城 / S3 树林 godrays / S5 夜间（天空可见；pass5 段表 37/38/42 段均在档）、S2 室内（早退验证）、S4 水面（耦合验证）；工具链 `rdc_pass5/6/7`（段表、消费链+PS 反汇编、像素信标）、`rdc_seg16_state.py`（逐 draw 状态模板）、`px_*`（截图区域统计）、五闸 + ci 流水线、frametime 每 600 帧基线行。

## Tasks

### Task 1: 天空节点离线侦察（定位天空 draw + 两硬事实）

**Files:** 新 `tools/rdc_pass8_sky.py`（只读分析，仿 `rdc_pass7_pixels.py` / `rdc_seg16_state.py` 风格）；新 `docs/analysis/Sky-recon.md`；读 `docs/analysis/S1-extract-pass5.json`、`S1-extract-pass6.json`、`docs/analysis/S4-water.md` L70-99（段表）；数据源 = `tools/rdc_run.ps1` 既有捕获路径（S1/S3/S5 .rdc）。

**Interfaces:** `rdc_pass8_sky.py` 输出 = ① 段7（ev21582–38636，MRT×3 [321/349/339] + DSV461）逐 draw 状态表（事件区间、视口、深度写掩码、PS id、绑点）② 屏幕固定采样点（上 1/3 中点等 3-5 点）的 461 深度 + 321 颜色回读 → Markdown 写入 Sky-recon.md。只读，不改帧。

**Steps:**

- [ ] 先读 S1 pass5/pass6 JSON 提取段7 draw 数与 42 个 PS 清单；JSON 粒度不足处由脚本补。
- [ ] 写 `tools/rdc_pass8_sky.py` 并跑 S1、S3、S5（S5 若缺 pass6 先 `rdc_run.ps1 -Script rdc_pass6.py` 补）。
- [ ] 写 `docs/analysis/Sky-recon.md`：天空 draw 定位结论（event 区间 / PS id / 深度态）**或**按绑定签名不可分离的否定结论（附证据）；必含两硬事实：① 天空像素深度 = 远平面（520 掩码路线成立性）② 段7-end 触发结构可判（MRT3 谓词所需字段清单）。
- [ ] `tools/gates4.ps1` 全 PASS → commit（docs+脚本+JSON 同提交，UTF-8 -F）→ push → CI SUCCESS。

**完成判据：** Sky-recon.md 含定位或否定结论 + 两硬事实，三场景数据在档，CI 绿。

### Task 2: 实施设计条目（docs/07）+ 计划回填

**Files:** 新 `docs/07-天空节点替换实施设计.md`（骨架仿 `docs/05`）；回填本计划 T3/T5/T6 参数节。

**Interfaces:** docs/07 §7 Step 梯子 ↔ 本计划 T3-T7 双向映射。

**Steps:**

- [ ] 成文：§1 被替换节点已测边界（引 T1 + 段表）；§2 路线裁定 —— **默认 = 段7-end 全帧合成回写**（判据：无 Draw Hook + 无 HLSL + 321 是天空唯一落点；T1 反证则在此改写并回填本计划）；§3 I/O 与触发谓词（MRT3 段类、arm 点、掩码 = 520 远深、入向 = 我方 321 快照 + ev21505 深度拷贝搭车）；§4 同步与时序（copyQ 事件闸模板 main.cpp L2446-2464；完成窗口 ev38661→ev38816；超阈值预案 —— 子矩形 `CopySubresourceRegion`，多缓冲不可行因相机依赖）；§5 声明式 diff 预案（B 类预期项跑闸前逐条预声明，docs/02 §13.3 口径）；§6 风险册（反射探针仍旧天空→Plan 2 收口；段15 HDR 写入为后续合并写者需验证不覆盖；室内早退；时域鬼影）。
- [ ] 回填本计划 T3/T5/T6：触发谓词字段、arm 时机、回写 API、日志文案、键名。
- [ ] 五闸全 PASS → commit → push → CI SUCCESS。
- [ ] **用户过目 docs/07 后才进 T3**（人工闸，设计先行惯例）。

**完成判据：** docs/07 在档 + 本计划回填完毕 + 用户认可路线。

### Task 3: sky 总闸 + 段7-end 结构触发（main.cpp）

**Files:** `src/poc-presenter/main.cpp` —— `installSsrRecon` L3600-3616（挂载门 + C-5 日志）、`ssrReconOm` L1551-1761（谓词区 L1691-1747 新段类 + 一次性旗标）、`ssrReconPresent` L3704-3782（帧复位）、ini 解析 + 启动回显块；新 ASCII 小脚本（ini 加 `sky=1`，仿 `ini_ssr_off.ps1`）。

**Interfaces:**

- `g_skyOn`（`std::atomic<bool>`，ini 键 `sky`，0/1，本任务缺省 0，T7 改 1）
- 挂载门 → `pocbEnabled() && (g_probeOn || g_ssrOn || g_skyOn)`；C-5 失败日志加 `sky=` 字段，逃生链文案 → `ssr=0 → probe=0 → sky=0 → vulkan=0`
- 新段类：主不透明 MRT3（RT0=321 RGBA16F + 349 R16G16 + 339 RGBA8 + DSV461 @1080p，逐段 entry 计数）；首次换绑离场 = 段7-end → `g_skySeg7EndPending` 每帧一次性（`ssrReconPresent` 复位）
- 触发计数走 60 帧心跳节流日志 `[sky] 段7-end 触发`；挂载打印 `天空: sky=1 → 挂 ctx 槽33/47/50`

**Steps:**

- [ ] 写码（门 / 谓词 / 旗标 / 帧复位 / 回显）+ **日志静默审计**：`ssr=0` 时所有 `SSR侦察:`、`[585]`、探针与普查 logLine 全部静默（逐个确认 g_ssrOn 前提，C-5 区文案同步）。
- [ ] `tools/check1.ps1` RESULT OK + `tools/gates4.ps1` 全 PASS。
- [ ] commit → push → ci_once → SUCCESS → artifact_fetch。
- [ ] 换 DLL：dllchk -After；ini 加 `sky=1`；knobchk -Key sky -Value 1；启动回显见 `天空:` 行。
- [ ] （需用户重启游戏）日志断言：`段7-end 触发` 心跳计数 ≈ 户外帧数；**`SSR侦察:` 等旧日志不出现**；S2 室内会话触发计数 = 0。

**完成判据：** 户外每帧恰一次触发、室内零触发、ssr/probe 日志不复活，CI 绿 + knobchk 过。

### Task 4: sky.preset 分档系统（独立任务）

**Files:** `src/poc-presenter/main.cpp`（解析 / 回显行 / 档位表 `constexpr`）；一致性核对对象 = 本计划 Spec 表 + docs/07 对应节（三处一致）。

**Interfaces:**

- `sky.preset` ∈ {0,1,2,3} 缺省 2；解析次序 = 先 preset 展开子键缺省表（DLL 内 constexpr ⇒ 换档不重编），后逐键读显式 ini 覆盖
- 子键：`sky.cloudscale` / `sky.cloudsteps` / `sky.cloudtemporal` / `sky.lutscale`；全部键（含 preset 与最终解析值）进启动回显行（与 ssr.* 同格式）

**Steps:**

- [ ] 写码 + 回显行；check1/gates4 全 PASS。
- [ ] commit → push → CI SUCCESS → 换 DLL → dllchk。
- [ ] knobchk 逐键：`-Key sky.preset -Value 0..3`（各档重启后回显）+ 四子键显式覆盖生效（单键改 ini → 重启 → 该键=覆盖值、其余随档）。
- [ ] 档位表三处一致核对（本计划 / docs/07 / DLL 回显）。

**完成判据：** 换档只改 ini、覆盖与回显语义全验证，CI 绿。

### Task 5: 入向数据通路（arm → 镜像 → 配平）

**Files:** `src/poc-presenter/main.cpp`（段7-end 触发点发行自拷贝、`ssrReconCopy` 搭车深度、自 copy 排除标记、心跳日志）；`src/poc-presenter/vkrenderer.cpp`（共享镜像导入，仿 2c 入向）。

**Interfaces:**

- 段7-end 触发 → 发行 `CopyResource(321 → skyIn)`（自发行先例：哨兵 copy main.cpp L2812-2831；自 copy 用既有排除标记，不污染 `copy.sequence` 锚点）
- 深度：游戏 `461→520` copy（ev21505）处搭车 → `skyDepth` 镜像（在 `ssrReconCopy` L1797 结构内新增分支，`ssr=0` 时按 `g_skyOn` 门控）
- 镜像 = 共享 NT handle（skyIn: RGBA16F 1920×1080；skyDepth: 同尺寸深度）；入向拷贝走既有 fence 闸
- 60 帧心跳日志：镜像就绪（尺寸/格式）、触发计数与拷贝计数**配平**（比值 = 1）

**Steps:**

- [ ] 写码（arm / 搭车 / 排除标记 / 心跳）；check1/gates4 全 PASS。
- [ ] commit → push → CI SUCCESS → 换 DLL → dllchk。
- [ ] （需用户重启游戏）日志断言：触发数 ≈ skyIn 拷贝数 ≈ 帧数配平、镜像字段正确、无重复/漏拷；`copy.sequence` 锚点在本任务抓帧里不受污染（自 copy 已排除）。

**完成判据：** 数据每帧准时到位且计数配平，CI 绿。

### Task 6: VK 天空渲染 pass + 出向回写 + 同步 + 早退

**Files:** 新 GLSL `src/poc-presenter/shaders/sky_lut.frag`、`sky_cloud.frag`、`sky_comp.frag`（vert 复用全屏三角，绑定不匹配时新增 `sky.vert`）；`tools/make_shaders.ps1`（条目表 L53-56 + L71 注释）；生成 `pocb_shaders.h` + `.spv` 入库；`src/poc-presenter/vkrenderer.cpp/.h`（三 pass + 档位 spec constants + 出向回写 + 事件闸）。

**Interfaces:**

- Pass 序（`g_skyOn` 且段7-end 触发）：① **LUT pass** —— 大气散射预计算（Bruneton/Hillaire 思路自写，GPL 红线见 Global Constraints），小 RT、按太阳方向变化阈值低频重算；太阳方向取 per-frame CB（既有入向）② **云 pass** —— 分辨率 = 原尺寸×`cloudscale`、raymarch 步数 = `cloudsteps`、3D 噪声纹理、`cloudtemporal=1` 时 ping-pong 时域复用；以 skyDepth 远深掩码限定工作域 ③ **合成 pass** —— 全屏三角：天空色 = 散射 LUT(viewray) + 云，掩码内替换 skyIn 原色；push constant ≤128B（相机/太阳/时间/参数，上限既有 :4268）；档位数值经 `VkSpecializationInfo` 烘焙（仿 kMissFade 模式）
- 出向：VK → 出向共享镜像 → `CopyResource(out → 321)`，发行点 = 段7-end 触发钩子链内；完成窗口 ev38661→ev38816（docs/07 §4 钉死）
- 同步：事件闸 + `vkWaitForFences`（copyQ 闸模板 main.cpp L2446-2464）；超阈值预案按 docs/07 §4（子矩形回写）
- 早退：`sky=0` 不挂门（T3）；`vulkan=0` 全灭（PoC-B 语义天然）；掩码全空时合成 pass 零工作（可选优化）
- make_shaders：新增 frag 条目、L71 注释同步；新 spv 入库过 spvsync

**Steps:**

- [ ] GLSL 三件 + make_shaders 条目 → 本地跑 `make_shaders.ps1` → `pocb_shaders.h` 再生（源/spv 同步）。
- [ ] vkrenderer.cpp 组三 pass（loadOp=DONT_CARE 模板、`vkCmdDraw(c.cmd,3,1,0,0)`、spec constants 档位烘焙）+ 出向回写 + 事件闸接线，全部受 `g_skyOn` 门控。
- [ ] check1 + gates4（含 spvsync）→ commit → push → CI SUCCESS → 换 DLL → dllchk。
- [ ] （需用户重启游戏）视觉初验：天空/云出现、构图不破；定点左右快摆无残影累积；S2 室内无新节点痕迹。

**完成判据：** 合成画面落地且无时域伪影，CI 绿，三 pass 均有 sky=0 关断路径。

### Task 7: 过闸与默认开（配对 + 5% + 回填收口）

**Files:** 注入抓帧 .rdc（S1/S3/S5/S2/S4）；`tools/rdc_compare.py/.ps1 -Strict`；declared-diff 清单（docs/07 §5）；`docs/00` 状态头、`docs/02` 新条目、`docs/07` 实测回填；本计划复选勾销；`main.cpp`（`sky` 缺省 0→1）。

**Steps:**

- [ ] 跑闸前把 B 类预期项写入 declared-diff 清单（docs/02 §13.3 口径，含新 PSO/纹理/拷贝计数逐条归因）。
- [ ] S1/S3 注入抓帧 → 同套提取链 → `rdc_compare.ps1 -Strict`：A 全绿 + F 必现 + B 归因、`errors=[]`；S2 室内配对同判（触发=0 侧证）。
- [ ] 5% 闸：S1/S3/S5 同机位 ssr=0 基线 vs 注入（docs/02 §14.14 口径，每 600 帧帧时行）；`sky.preset` 0..3 逐档实测回填档位表。
- [ ] knobchk 全键（sky + preset + 4 子键）+ 同存档同机位截图（R6：视觉无自动判据 → 截图 + `px_*` 区域统计对照）+ S4 水面复拍（耦合项）。
- [ ] `sky` 缺省 0→1（改码）→ 再一轮 CI + 换 DLL + 复验逃生门：`sky=0` 即时回到原版天空（一键回退承诺）。
- [ ] 文档回填：docs/00 状态头、docs/02 新条目、docs/07 实测数据；五闸全 PASS → 最终 commit → push → CI SUCCESS → DLL 收口 + dllchk。

**完成判据：** 5% 闸 + 10 锚点全过、档位表有实测数、默认开且 sky=0 可回退、文档三处回填、CI 绿。

## Pending

- Research A：✅ 已回报并入（§4.6 L470-476 全文、阶段2 门槛 L167-168、§1.1.1 架构①、模块布局与新代码位置约定、5% 闸口径、10 锚点分级口径、降级判据）。
- Research B：✅ 已回报并入（36 段表、天空 draw 未定位的否定结果、docs/05 节点替换模板与 Step 梯子、钩子槽位与谓词区行号、挂载门 L3602 条件、make_shaders 仅 vert/frag、Draw Hook 不存在）。
