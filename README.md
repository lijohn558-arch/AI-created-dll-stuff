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
| [docs/03-API配对验证收益分析.md](docs/03-API配对验证收益分析.md) | 配对锚点判定口径、收益/风险、现代化扩展顺序（水体→天空→材质→超分→帧生成） |

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
│   └── poc-presenter/             # SKSE 插件: PoC-A Present Hook 验证（main.cpp）
├── tools/                         # 分析脚本（qrenderdoc 内嵌 Python 运行）
│   ├── rdc_extract.py + rdc_pass2~6.py  # 六轮提取（RDC_SCENE 选场景，无头批跑）
│   ├── rdc_run.ps1                # 无头批跑 runner（-Scene/-Targets/-PsEvents/-PsWL）
│   └── rdc_compare.py / rdc_compare.ps1  # 配对判定 harness（10 锚点 PASS/DIFF/SKIP）
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

**机制（v1.3，六通道）**：
1. **同步安装（v1.2 主修正）**——钩子在 `SKSEPlugin_Load` 内同步装完（等模块 ≤5s、等
   renderdoc ≤2s 兜加载序）；SKSE 主线程加载插件，Load 不返回游戏就无法继续初始化，
   由此**保证先于游戏交换链创建**（v1.1 后台线程安装曾输给渲染器初始化的竞态）；
2. **多 vtable 收集**——dummy1（ForHwnd→SwapChain1 口径）+ dummy2（工厂 v0 CreateSwapChain 口径）
   + 两者各自 QI SwapChain/1/2/3，每张不同 vtable 挂槽 4（Present），经 1 口径见过的加挂槽 18（Present1）；
3. **五口径工厂拦截**——CreateDXGIFactory 与 CreateDXGIFactory1 各配 IID0/1/2，加
   CreateDXGIFactory2：RenderDoc 包装类若按「所请求接口」分化，游戏要的那张也在登记之列；
   挂槽 10/14/15/17 四个交换链创建方法，游戏创建交换链时拿到其对象就地打 vtable，日志带 ★；
4. **设备链通道（v1.3 新增）**——device → QI `IDXGIDevice` → `GetAdapter` → `GetParent`
   三 IID 取工厂（游戏常见拿工厂路径，非导出）；每个新工厂先建辅助 v0 dummy（拿它所属类的
   交换链 vtable——若是真 dxgi 类，游戏 Present 即被槽 4 截住），再挂其创建方法；
5. **安全阀**——原值须落在 dxgi.dll / renderdoc.dll 内才挂（防错槽位）；14/15/17 仅对
   IID2 口径读（防越界）；
6. **看门狗**——每 5s 复查已挂槽位，被第三方改写则记录并打回；120s 仍 0 次 Present 给汇总告警。

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
   - `...: vtable 已登记 (与既有同类) 0x...`（游戏交换链与 dummy 同类的证据——
     此时槽 4 已挂，下一行就该是 Present）
   - `登记完成: 交换链 vtable N 个 (含 Present 挂钩), 工厂 vtable M 个 (含创建方法挂钩)`
   - `★ 工厂拦截 ...`（游戏创建交换链时——出现即证明时机+口径都已覆盖）
   - **`第 1 次 Present (经 Present): ... 1920x1080 ... title="..."`**（关键行——证明拦到游戏 Present）
   - `Present 已开始触发 ... — PoC-A 验收通过`，之后每 600 帧一行 `近600帧 xx.x FPS`
4. 游戏画面应完全正常（本 PoC 不改变呈现）。若 120s 后仍 0 次触发，看日志里各 vtable 的
   本体/原值模块、★工厂拦截是否出现、有无「被改写」记录，贴日志迭代（多通道自证价值）

验收通过后进入 **PoC-B**：在 Present 里创建 Vulkan instance/swapchain → 游戏窗口出
清屏/三角形 → F12 抓这帧 → `rdc_run` 提取链 → `rdc_compare -Base S4 -Cand <候选>` 首次候选比对。
