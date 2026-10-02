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
│   └── capture-helper/            # SKSE 插件: 进程内 RenderDoc 抓帧引导
│       ├── main.cpp
│       ├── skse_abi.h             # SKSE64 2.0.20 最小 ABI 声明
│       └── renderdoc_app.h        # RenderDoc 官方 in-app API 头 (MIT)
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
