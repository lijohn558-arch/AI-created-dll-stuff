# skyrim-vulkan

Skyrim Special Edition 的 Vulkan 直接对接项目 — 让游戏以 Vulkan 渲染并重构渲染管线以达到最佳性能。

## 环境分工（重要）

| 环境 | 职责 |
|---|---|
| **本地** | **编辑**（Notepad++）+ **测试**（RenderDoc 抓帧、游戏内验证、性能测试） |
| **GitHub Actions** | **编译**（本地不编译；push 后 CI 产出 dll，从 Actions Artifacts 下载） |

## 文档索引

| 文档 | 内容 |
|---|---|
| [docs/00-总体规划-实施步骤.md](docs/00-总体规划-实施步骤.md) | 九阶段总体规划、里程碑、风险清单、首周行动项 |
| [docs/01-CommunityShaders项目调研.md](docs/01-CommunityShaders项目调研.md) | CS 项目可复用资料调研（HLSL 源码、Hook 清单、NvAPI 坑） |
| [docs/02-RenderDoc抓帧操作清单.md](docs/02-RenderDoc抓帧操作清单.md) | 抓帧执行手册（环境、场景、记录表、提取清单、无头批跑 §10、配对 harness §11） |
| [docs/03-API配对验证收益分析.md](docs/03-API配对验证收益分析.md) | 配对锚点判定口径、收益/风险、现代化扩展顺序（水体→天空→光照→材质→超分→帧生成，见 §6.7） |
| [docs/04-NIF资源格式调研.md](docs/04-NIF资源格式调研.md) | NIF 顶点格式→Vulkan 映射、材质参数、骨骼蒙皮结构；风险项 #2（蒙皮复杂度）评估（`tools/parse-nif.ps1`） |
| [docs/05-SSR节点替换实施设计.md](docs/05-SSR节点替换实施设计.md) | SSR 同位置节点替换实施设计：D1 触发判据、D2 共享输入通路、D3 输出回写（2d）、D4 shader、D5 逃生门、R1–R6 风险 |

## 仓库结构

```
skyrim-vulkan/
├── .github/workflows/build.yml   # CI: MSVC x64 编译 capture-helper
├── docs/                          # 项目文档（进 git）
├── src/
│   ├── capture-helper/            # SKSE 插件: 进程内 RenderDoc 抓帧引导
│   │   ├── main.cpp
│   │   ├── skse_abi.h             # SKSE64 2.0.20 最小 ABI 声明（poc-presenter 共用）
│   │   └── renderdoc_app.h        # RenderDoc 官方 in-app API 头 (MIT)
│   └── poc-presenter/             # SKSE 插件: PoC-A/B + SSR Step1~2d（main.cpp 4043 行）
│       ├── main.cpp               # hook 面 / interop（2c 入向、2d 出向回写、ini、日志与判读）
│       ├── vkrenderer.h           # VK 函数表 POCB_DEV_FNS + pocmain 反向声明（v0.17.0 拆出）
│       └── vkrenderer.cpp         # 自写 VK 渲染器模块：设备/导入/命令录制/入向出向帧循环
├── tools/                         # 分析脚本（qrenderdoc 内嵌 Python 运行）
│   ├── rdc_extract.py + rdc_pass2~6.py  # 六轮提取（RDC_SCENE 选场景，无头批跑）
│   ├── rdc_run.ps1                # 无头批跑 runner（-Scene/-Targets/-PsEvents/-PsWL）
│   ├── rdc_compare.py / rdc_compare.ps1  # 配对判定 harness（10 锚点 PASS/DIFF/SKIP）
│   ├── rdc_seg16_state.py         # 段16 逐 Draw blend/depth/stencil 探针（D-2/D-3，§14.16）
│   ├── run2c.ps1                  # 跑图判读脚本（§#0–#12 自动结论；banner 版本期望随版本升）
│   └── check1.ps1                 # main.cpp / vkrenderer.h/cpp 结构自检（提交前必跑，须 RESULT OK）
└── captures/                      # .rdc 原始文件（不进 git）
```

## 子项目：capture-helper

**作用**：解决 RenderDoc 外部注入的两个问题（MO2 注入链断裂、NvAPI 选项无 UI 入口）。
游戏启动时在进程内加载 `renderdoc.dll`，设置 NvAPI 白名单（`AllowUnsupportedVendorExtensions=0x10DE`，
参考 Community Shaders 已验证做法），之后用 RenderDoc 默认热键抓帧。

### 构建
push 代码 → GitHub Actions `build` 工作流 → 从 **Actions → Artifacts** 下载 `capture-helper.zip`

### 安装
1. 解压得到 `capture-helper.dll`，放入 `<游戏>/Data/SKSE/Plugins/`
2. `renderdoc.dll` 放到以下任一位置：
   - `<游戏>/Data/Renderdoc/renderdoc.dll`（推荐，与 CS 约定一致）
   - 或在 `capture-helper.ini` 中指定：`renderdocDll=C:\Program Files\RenderDoc\renderdoc.dll`
3. 启动游戏（SKSE 正常路径）

### 验证与使用
- 看 `Data/SKSE/Plugins/capture-helper.log`：应有
  `AllowUnsupportedVendorExtensions(0x10DE NvAPI) = 1 (OK)` 和 `就绪` 字样
- 游戏内出现 RenderDoc 角标 = 成功
- **F12 / PrintScreen** 抓帧 → `.rdc` 存于 `Data/SKSE/Plugins/captures/`
- 用 RenderDoc UI（File → Open Capture）打开分析

### 日常抓帧
若只想抓帧不想构建插件，也可直接用 RenderDoc UI 的 Launch Application 外部注入
（配置见 `docs/02-RenderDoc抓帧操作清单.md` §2）；插件方式为更稳定的备选路径。

---

## 子项目：poc-presenter（PoC-A：Present Hook 验证）

**作用**：`docs/00` 首周行动项 #4 / 最高风险项 #1（渲染管线可否 Hook）的第一环——
用 SKSE 插件载体证明能在真实游戏进程内拦截 `IDXGISwapChain::Present`。
本步**不碰 Vulkan、不改变画面**，只产出日志。

> **状态（2026-10-06）：插件 `poc-presenter` v0.18.3（⏳ 待 CI + 真机）—— 阶段1 已全绿收口（2026-10-04）、阶段2 进行中：
> SSR Step1/2a/2b/2c 已收口 + **2d-1 出向回写两轮真机回归均 [PASS] 全绿**（`ssr.vkout`；
> `v0.18.0` §14.15.1、代码批 `v0.18.1` §14.17 = 6001 帧），其中 C-6 把 2d 回写从 **28.77 → 0.68
> 次/帧**、会话 GPU **17.16 → 16.82 ms**；**第三轮 `v0.18.2`（2d-3 深度格式探测 + C-7 读回节流，
> `86aa133` CI SUCCESS）也 [PASS] 全绿（§14.18.1，4980 帧）**：C-7 `出向读回` 恒 3 行 → **9 行**、
> §#13 = **路线 1 前置 6/6 成立**（首选 `R32_FLOAT × BindFlags 0x28`）、**R2 归因更正为
> `D24 家族 × SHARED_NTHANDLE` 组合**（另立候选路线 1′ KMT 直入待估）、帧时 16.84/16.89 无回退。
> 下一步 = **`v0.18.3` 本轮**（2d-4 路线 1′ KMT 探测 + C-8 + O-1 入向 `Flush`，判读 docs/02 §14.19）
> 跑完定路线 1 / 1′，再进 SSR v1 shader；renderer 已拆成
> `vkrenderer.h/cpp` 独立模块（v0.17.0）；CI 双 job 出包。**（下述 0.9.3 及以前为 PoC-A/B 期历史记录）**
> —— 带 renderdoc 局（7201 次 / 60 FPS，判读 §7.6）+ 无 renderdoc/GFE 局（★ 中、6001 次 / 60 FPS，判读 §7.7）
> 闭合第一环「Present 可拦」；**PoC-B v0.1 于 0.9.1 局闭合第二环「写」**：Vulkan 离屏 512×512 → 读回 →
> `CopySubresourceRegion` 进 backbuffer (16,16)，4200 帧零失败、均值 **6.47 ms/帧**、近 600 帧恒 60 FPS；
> 验收三步 = 两场景目视 ✓ + 像素探针 `SENTINEL_FOUND` ×2 ✓ + 结构配对 `+1 Copy` 指纹（三口径互证）✓，
> 详见 docs/01 **§7.8**（含整帧 PNG 图证与差异归因）。
> **遗留双清零（0.9.2 对照局，docs/01 §7.9）**：① 同日同机位无注入对照帧 `S5c.rdc`
> → `rdc_compare -Base S5c -Cand S5b` = **pass6/diff4**（`clear` 两档 + `transparent 161=161` 转 PASS，
> 余下 DIFF 只剩注入指纹 `copies 3→4` 与 draws −55 的连带）；② 方案B 独立计数 = **`第 1 次 Present
> (经 函数detour)`**、3001 次、~60 FPS 无告警。**0.9.3** 修掉本局发现的缺口（函数层 detour 此前不驱动
> PoC-B），逃生门 `vulkan=0` 的运行时验证留作可选项。

**机制（v1.7，七通道）**：
1. **同步安装（v1.2 主修正）**——钩子在 `SKSEPlugin_Load` 内同步装完（等模块 ≤5s、等
   renderdoc ≤2s 兜加载序）；SKSE 主线程加载插件，Load 不返回游戏就无法继续初始化，
   由此**保证先于游戏交换链创建**（v1.1 后台线程安装曾输给渲染器初始化的竞态）；
2. **多 vtable 收集**——dummy1（ForHwnd→SwapChain1 口径）+ dummy2（工厂 v0 CreateSwapChain 口径）
   + 两者各自 QI SwapChain/1/2/3，每张不同 vtable 挂槽 8（Present），经 1 口径见过的加挂槽 22（Present1）；
   （v1.7 勘误：v1.0~v1.6 挂的槽 4/18 按 MS dxgi.h 实为 SetPrivateDataInterface/GetDesc1，
   从未挂到 Present——这就是历次 0 计数的根因，真槽为 8/22）
3. **五口径工厂拦截**——CreateDXGIFactory 与 CreateDXGIFactory1 各配 IID0/1/2，加
   CreateDXGIFactory2：RenderDoc 包装类若按「所请求接口」分化，游戏要的那张也在登记之列；
   挂槽 10/15/16/24 四个交换链创建方法（CreateSwapChain / ForHwnd / ForCoreWindow /
   ForComposition，MS dxgi.h 定位；旧 14/17 实为 IsWindowedStereoEnabled/GetSharedResourceAdapterLuid
   = 潜在崩溃点，v1.7 改正），游戏创建交换链时拿到其对象就地打 vtable，日志带 ★；
4. **设备链通道（v1.3 新增）**——device → QI `IDXGIDevice` → `GetAdapter` → `GetParent`
   三 IID 取工厂（游戏常见拿工厂路径，非导出）；每个新工厂先建辅助 v0 dummy（拿它所属类的
   交换链 vtable——若是真 dxgi 类，游戏 Present 即被槽 8 截住），再挂其创建方法；
5. **安全阀**——原值须落在 dxgi.dll / renderdoc.dll 内才挂（防错槽位）；15/16/24 仅对
   IID2 口径读（Factory2 布局末项=24，防越界）；
6. **看门狗**——每 5s 复查已挂槽位，被第三方改写则记录并打回；120s 仍 0 次 Present 给汇总告警
   （附：游戏交换链是否取得、方案B detour 是否已装）；
7. **阳性确认（v1.7 新增）**——安装完成与 ★ 拦到游戏交换链时，都打印对象 vptr 与 vtbl[8]
   是否等于我们的钩子；方案A（renderdoc 转发桩 `[obj+0x28]` 解包真对象）作为方案B 的候选靶来源。

钩子按「调用方 vtable 地址」查表转调原函数；原函数若来自 RenderDoc 包装层则抓帧链路不受影响。
> 实测链：v1.0 单挂槽4 → 0 触发（类不匹配）；v1.1 四通道 → 0 触发、无 ★，overlay 暴露
> 游戏窗口先于我们的 dummy → P1 时序确诊 → v1.2 改同步安装；**v1.2 双跑**：有
> capture-helper 时五口径全 OK 但都在 renderdoc 类上、仍 0 触发，无 capture-helper 时
> ★ 在 +7s 于 dxgi 真类正常触发 → **P3 类分叉确诊**（游戏拿工厂不走导出 / renderdoc
> 生效前已有真对象）→ v1.3 增设设备链通道做双类覆盖；**v1.3 双跑实测**：设备链因
> 误用 QI 而静默失败（适配器要 GetParent），且无 renderdoc 时 ★ +8s 后日志终止（匹配
> 路径静默、无 Present 行）→ v1.4 改 GetParent、补失败日志/匹配日志、★ 行附 swapDesc；
> **v1.4 双跑**：设备链/辅助 dummy 全通，但无 renderdoc 时★拦到游戏交换链且其 vptr=
> 我们挂好的类、120s 槽位未被改写，**Present 计数却恒 0**（游戏虚调用没走到我们的槽）
> → v1.5 加三探针：A 对象 vptr 盯梢、B 原实现前 32 字节（为函数级 detour 铺路）、
> W 本进程窗口枚举（找第二个交换链宿主），附 FLIP_DISCARD 口径 dummy3。
> **用户取证补充**：run2 那 120s 画面正常动画（Present 必然在发生）+ 本机有 **GeForce
> 覆盖层（GFE，启动时注入）** → 调研 CS/ReShade/RenderDoc 的拦截姿势（docs/01 §7）：
> CS 根本不碰 DXGI Present（引擎层注入）；ReShade/RenderDoc 均为**包装对象自有 vtable +
> 导出级/函数级 detour**、不修补公共类 vtable → v1.6 备选 = 对 dxgi 真 Present 实现做
> 函数级 detour（探针 B 字节选窃取长度），兜底 = 引擎层注入；
> **v1.5 单跑（带 capture-helper，23:26）**：★ 仍 0 ⇒ 对比两跑可锁定——**游戏工厂恒为真
> dxgi 类**（无 renderdoc 时我方导出=真类故 ★ 中；有 renderdoc 时我方只能拿到包装类而游戏
> 仍持真类故 ★ 不中）；探针 B 实锤 renderdoc 的 Present 桩 = 11 字节三指令**动态转发 thunk**
> （`mov rcx,[rcx+10]; mov rax,[rcx]; jmp [rax+20]` → 转发到真对象**当前** slot4）⇒ 只要握有
> 真类 slot4，包装层最终必落我方钩子（★ 中否不再影响计数）——〔**v1.7 勘误**：被转发的槽 4
> 实为 `SetPrivateDataInterface`，此推论不成立，真条件是槽 8，见下方 v1.7 段〕；CreateSwapChain 序言 15 字节
> 全位置无关（可安全窃取）；探针 W 仅唯一游戏窗口（+Steam `DIEmWin`），无第二宿主；
> dummy3 FLIP 被系统拒 0x80070005（低优先级）。**修法 = 加载序**：`capture-helper.dll` 改名
> `z-capture-helper.dll` 让 poc-presenter 先装真类 → 待双跑验证（无 helper 取证 GFE / 改名后
> 带 helper 验收）。
> **v1.6 落地（外部同好建议采纳 + 本机字节取齐）**：建议四方案——A 解包 renderdoc 包装取真
> 对象、B 函数级 detour（更彻底）、C 不再追真工厂 vtable（★ 仅诊断，目标是 Present）、
> D 加载序（改名零成本可测、与 B 并行不冲突），外加顺手修「设备链工厂行打对象地址恒 ?」
> 的 bug。本机用 PowerShell `Add-Type` 只读转储取齐真存根字节（dxgi 与游戏同 boot 同基址
> `0x7FFBBACF0000`，三址全验证落在本进程 dxgi 内）：Present `@dxgi+0x2E460` 前 **14B 恰为
> 指令边界且零地址依赖**（`48 83 EC 38 4C 89 44 24 50 4C 8D 4C 24 50`）、Present1
> `@dxgi+0x4EE60` 边界在 **15B**、CreateSwapChain 序言 25B 起有 RIP 相对寻址（18B 可窃，
> 但按 C 不挂工厂）→ **v1.6 = 方案B**：入口只改 5B `E9 rel32` → 近端跳板（±2GB 内
> VirtualAlloc 存 FF25 绝对跳到钩子）——只碰前 5 字节，不破坏第三方 trampoline 的续接区；
> 入口若已被 GFE/renderdoc 改成跳板（E9/FF25/mov rax,jmp rax）→ **CHAIN** 计数后直接转发
> 其目标、链式共存；前导字节与预期不符 → 保守跳过并留档；vtable 钩子转调期间置 TLS 标记、
> 函数钩子据此跳过计数（双层不双计）——**任何调用路径（类 vtable 虚调用 / GFE 包装绕行 /
> renderdoc 动态转发）最终都落进同一函数入口，计数必然发生**（v1.4「计数恒 0」的破局点）。
> 方案A 暂缓为 v1.7 备选（B 若通则 A 仅诊断增益）。
> **v1.7 = 根因勘误 + 全量槽位修正（2026-10-03，外部同好指正 + MS dxgi.h 与本地实证）**：
> 上面 v1.0~v1.6 全链「挂槽 4/18」按 MS ABI 实为 `SetPrivateDataInterface`（IDXGIObject 第 4 项）
> 与 `GetDesc1`（SwapChain1 第 18 项），**从未挂到 Present ⇒ 历次 0 计数恒真**；v1.6 的
> `0x2E460/0x4EE60` 双守卫就是这两槽的存根本体 = 探针 B 与守卫**互相自证循环**（两者恒一致）。
> 真 ABI：`Present=槽8 → dxgi+0x19000`（前 14B `48 89 5C 24 10 48 89 74 24 18 55 57 41 56`，
> 恰为指令边界）、`Present1=槽22 → dxgi+0x194A0`（前 14B 末字节 `54` 与 Present 的 `56` 可区分，
> 边界 14/16）——`tools\dxbytes.ps1` 已同步（旧两址保留为历史条目）。工厂按 dxgi.h 重排：
> 保留 10、`ForHwnd 14→15`、`ForCoreWindow 15→16`、`ForComposition 17→24`（外部建议 13/14/22
> 与 ABI 不符：13=IsCurrent、14=IsWindowedStereoEnabled、22=RegisterOcclusionStatusEvent，均非创建路径）。
> 方案B 靶子改**运行时取**：① 活交换链 `vtbl[8]/[22]` 直读 → ② 方案A 解包真对象 `vtbl[8]/[22]`
> （renderdoc 转发桩 `[obj+0x28]` 存真对象指针）→ ③ dxgi RVA 兜底，每候选过 14B 字节守卫；
> 旧「交叉验证行」删除。**阳性确认**：安装完成与 ★ 处打印对象 vptr + `vtbl[8]` 是否等于我们的钩子
> （修槽前只证明过「挂了 SetPrivateDataInterface」= 假阳性根源）；TLS 双层不双计与 CHAIN 不变。
> **v1.7 实测（2026-10-03 13:44，带 renderdoc 局）= PoC-A 验收通过**：安装后 6s 出
> `Present 已开始触发 — PoC-A 验收通过`，2 分钟 7201 次、近600帧恒 60.0 FPS 画面正常，
> 无告警/无被改写；阳性确认 `vtbl[8]/vtbl[22] ==我们的钩子 ✓`（槽位修对铁证）；方案A
> `[obj+0x28]` 解包得真对象 `vtbl=dxgi+0xCD688`、`真vtbl[8]=dxgi+0x19000`、`真vtbl[22]=dxgi+0x194A0`
> ——与 `dxbytes.ps1`、`dxdump3.cs` 三方逐位吻合；方案B 候选1（活交换链）读到 renderdoc
> 32B 序言存根 → 字节不符保守跳过留档 → 候选2（方案A真vtbl）`RAW 已装 窃取14B`；
> **第 1 次 Present (经 Present): vtable=renderdoc 包装类 … title="Skyrim Special Edition"**。
> 本局**无 ★**（游戏交换链未走我方工厂）但 vtable 同类直接命中 ⇒ 印证「★ 中否不影响计数」；
> 因 vtable 层先命中 + TLS 去重，**方案B 层的独立有效性本局未证明** —— **该悬念已由下述第二局闭合**
> （无 renderdoc/GFE 局实测：★ 中、方案A 按设计跳过、方案B 候选① 即真 dxgi `vtbl[8]`、RAW 已装 14B）。详见 docs/01 §7.6。
> **v1.7 第二局（13:59，无 capture-helper / GFE 在场）= 双环境闭环**：`renderdoc.dll 未加载` →
> 交换链回真 dxgi 类（vtable/slot8/slot22/工厂 10/15/16/24 原值全 dxgi）；探针 B 这次打到
> **真 dxgi Present 存根**（前 14B `48 89 5C 24 10 … 55 57 41 56` + 第 15 字节起 `48 8D 6C 24 90`
> = 边界 14）⇒ 与 `dxbytes.ps1`、`dxdump3` **三方逐字节闭合**；方案A 按设计跳过（非包装类）；
> **方案B 候选①（活交换链）→ RAW 已装 14B**（与上局"候选①被跳过、靠候选②"正好互补）；
> **★ 工厂拦截 CreateSwapChain（4s）+ 阳性确认(★游戏) `vtbl[8]/[22] ==我们的钩子 ✓`** +
> 探针A 基线/钩子自证；**第 1 次 Present (经 Present): vtable=dxgi 真类**，6001 次 / 60.0 FPS、
> 无告警无被改写。**未证项**：方案B 函数层的独立计数两局均被 vtable 层 + TLS 去重盖住
> （它只作 vtable 类挂不上时的兜底，验证止于"字节守卫通过 + RAW 已装"）。详见 docs/01 §7.7。

### 构建
push 代码 → GitHub Actions `build` 工作流（`poc-presenter` job）→ 从 **Actions → Artifacts**
下载 `poc-presenter.zip`

### 安装与验收
1. `poc-presenter.dll` 放入 `<游戏>/Data/SKSE/Plugins/`（与 capture-helper.dll 并列）
2. MO2 正常启动游戏，进到有画面的场景
3. 看 `Data/SKSE/Plugins/poc-presenter.log`，依次应出现：
   - `同步安装完成 (SKSE 加载线程内, 先于游戏渲染器初始化)`（v1.2 关键行——时序竞态已排除）
   - `工厂口径0..4: OK 0x...`（五口径取工厂对象的结果）
   - `设备链 GetAdapter = 0x... vtable=0x... 来自 ...` + `设备链工厂 IID0/1/2 = ... 来自 ...`
     （v1.4 关键行——若「来自」落在 **dxgi.dll** 即拿到真类，双类覆盖成立；
     若 GetParent 失败会显式打 `GetParent 失败 hr=...`）
   - `设备链辅助 dummy IID...: ...`（该类交换链 vtable 也被收集的证据）
   - `...: slot8 原值=0x... 来自 dxgi.dll` + `...: slot22 原值=0x... 来自 ...`
     （v1.7 起读的是**真槽**：槽8=Present、槽22=Present1，且原值须落在 dxgi/renderdoc 才挂）
   - `...: vtable 已登记 (与既有同类) 0x...`（游戏交换链与 dummy 同类的证据——
     此时槽 8 已挂，下一行就该是 Present）
   - `登记完成: 交换链 vtable N 个 (含 Present 挂钩), 工厂 vtable M 个 (含创建方法挂钩)`
   - `阳性确认 (dummy1 安装完成): obj=... vptr=... vtbl[8]=... ==我们的Present钩子 ✓`
     （**v1.7 关键行——槽位修对的铁证**：v1.0~v1.6 只验证过 vtbl[4]，那是
     SetPrivateDataInterface，属假阳性；此行若 ≠钩子即槽位又错了）
   - `方案B Present 候选 1/3: 0x... 来自 活交换链vtbl[8] (dxgi.dll)` → 紧接
     `方案B Present: RAW 已装 target=... trampoline=... 窃取14B (入口 5B E9)`；
     活交换链读到的若是 renderdoc 存根会先打 `入口字节与预期不符 ... 保守跳过`（附前
     16 字节留档），再按 `方案A真vtbl[8]` → `dxgi+0x19000 兜底` 逐候选试——**v1.7 关键行：
     靶子来源（活交换链 / 方案A解包 / dxgi RVA 兜底）一目了然**；
     入口若已被 GFE/renderdoc 先占则为 `CHAIN 已装 (入口原为 ...)`——两者都是成功
     （函数级 detour 生效，绕过 vtable 的 Present 也被计数）
   - `方案A(安装期解包): 包装对象 0x... vtbl=0x... [obj+0x28]=0x...` → 紧接
     `方案A(安装期解包): 真对象=0x... (经 +0x28) vtbl=0x... 来自 dxgi.dll | 真vtbl[8]=... `
     （仅 renderdoc 包装类对象出现——方案A 解包是否成立看这两行；非包装类打
     `vtable 不在 renderdoc 包装类 — 这些偏移不适用, 跳过`）
   - `★ 工厂拦截 ...`（游戏创建交换链时——出现即证明时机+口径都已覆盖）
   - `阳性确认 (★游戏 ...): obj=... vptr=... vtbl[8]=... （已是钩子 / 尚未挂钩→就地挂）`
   - **`第 1 次 Present (经 Present / 函数detour): ... 1920x1080 ... title="..."`**
     （关键行——证明拦到游戏 Present；经 `函数detour` 即方案B 独立命中）
   - `Present 已开始触发 ... — PoC-A 验收通过`，之后每 600 帧一行 `近600帧 xx.x FPS`
4. 游戏画面应完全正常（本 PoC 不改变呈现）。若 120s 后仍 0 次触发，看日志里各 vtable 的
   本体/原值模块、★工厂拦截是否出现、有无「被改写」记录，以及 `告警:` 行自带的三项快照
   （游戏交换链是否取得 / 方案B detour 是否已装 / 登记表规模），贴日志迭代（多通道自证价值）

验收通过后进入 **PoC-B**：在 Present 里创建 Vulkan instance → 离屏渲 512×512（洋红清屏 +
三角形）→ 读回 → `CopySubresourceRegion` 拷进 backbuffer `(16,16)` → 调原 Present。
**刻意不开 Vulkan swapchain**（不与游戏 / RenderDoc / GFE 争 HWND 所有权，且像素落在 D3D11
帧内 ⇒ F12 抓帧必然记录这次拷贝——配对 harness 只认 D3D11 帧）。

**PoC-B 三步验收（v0.9.1 / PoC-B v0.1 —— 2026-10-03 已闭合，实测值见 docs/01 §7.8）**：

1. **游戏内目视 ✓**：左上角 512×512 洋红块 + 渐变三角形（顶点绿/黄/蓝）；日志
   `PoC-B init 完成: 离屏 512x512 ... CopySubresourceRegion 到 backbuffer (16,16)`、
   `PoC-B 第 1 帧注入: 渲染+读回+拷贝 7.14 ms`，此后每 600 帧一行——**4200 帧零失败、
   累计均值 6.47 ms/帧、近 600 帧恒 60 FPS**。
2. **机器判定（像素）✓ ×2**：F12 抓帧 → 改名成合法场景 ID（`S5.rdc`→`S5b.rdc`；
   `-Scene S5` 会映射到 10/2 基线，**不改名会误抓**）→
   `powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_pass7_pixels.py -Scene S5b`
   → `verdict=SENTINEL_FOUND`：四角全 `(255,0,255)`、三角形重心 `(86,217,85)`、对照点干净、errors=0
   （第二帧 `S6-pool` 同样 FOUND；阴性对照见基线 `SENTINEL_ABSENT`）。
3. **结构配对 ✓**：`rdc_run.ps1 -Script rdc_extract.py -Scene S5b`（pass4/5/6 同跑）→
   `powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_compare.ps1 -Base S5 -Cand S5b`
   → **`copies 3→4`、`copy.sequence` 尾部追加 `512x512 → 1920x1080`、`copies_total 3→4` 三口径同一指纹**，
   纹理格式 / CS dispatch / CS 字节码三项 PASS；draws 等其余 DIFF 由**整帧 PNG 图证**归因游戏状态
   （同机位不同时刻：基线=火焰战斗、候选=对话状态；注入侧零 Draw）。

**对照局 / 噪声底（2026-10-03 0.9.2 局实测，docs/01 §7.9）**：`poc-presenter.ini` 写
`vtable=0`+`vulkan=0` → 同机位 F12 得 **`S5c.rdc`**（无注入）→ 探针 `SENTINEL_ABSENT` ✓ +
整帧 PNG 无洋红 ✓ + pass5 只有 3 条 Copy ✓ 四路互证 →
`rdc_compare.ps1 -Base S5c -Cand S5b` = **pass6 / diff4**（同日同机位、唯一差别=注入）：
`clear.bound`/`clear.target`/`transparent.draws(161=161)` 三项**转 PASS**，copy 两锚点是唯一可归因
注入的差异；跨日噪声底（`S5 → S5c`，双无注入）= pass5/diff5、draws +464 → 同日收到 −55。
同局还拿到 **方案B 函数层独立计数**：`第 1 次 Present (经 函数detour)`、3001 次、~60 FPS。

整帧图证（新工具，约 7 s/张）：`powershell ... -File tools\rdc_run.ps1 -Script rdc_dump_backbuffer.py -Scene S5b`
→ `docs\analysis\<场景>-backbuffer.png`（已在库：`S5` / `S5b` / `S6` / `S5c` 四张，`S5b` 图上肉眼可见
洋红块、`S5c` 图无；**图证以字节为准**——曾出现看图工具串档，用 `.json` 的 `spotcheck` 与 pass7 探针点互证）。

失败即关注入（日志 `PoC-B 失败: <步骤> code=... — 已关闭注入, 游戏照常呈现`），逃生门
`<pluginDir>\poc-presenter.ini` 写 `vulkan=0`（或 `pocb=0`）——**尚未运行时实测**：0.9.2 对照局里
`pocbFrame()` 根本没被调到（vtable=0 时函数层 detour 只计数不驱动 PoC-B，docs/01 §7.9.5），
**0.9.3 已修**（`detouredPresent` 补 `pocbFrame`），下次跑局可顺带看那行
`PoC-B: poc-presenter.ini 关闭了 vulkan 注入 (vulkan=0)`。对照局的 ini 已删除，环境恢复默认。
