# 抓帧记录 — S7-heavy-mods（高负载场景，可选）

> **状态：已跳过（2026-10-02 定案，不抓帧）** — 跳过理由见 `docs/02-RenderDoc抓帧操作清单.md` §4.1：S1~S5 五次提取证明 pass 结构已收敛（高负载只增规模不增结构新知）；4K 材质不改管线结构且属 mod 侧；性能压测归移植后验证阶段。本表保留仅作占位，勿填写。
>
> ~~用途：压力测试（4K 材质 + 密集光照）~~

## 记录（抓完填写，*斜体* 为待填项）

- 场景 ID: S7-heavy-mods
- 日期: *YYYY-MM-DD（抓帧当天）*
- 游戏版本: 1.5.97.0 (0x01050610) | SKSE: 2.0.20 (0x02000140)
- GPU/驱动: NVIDIA GTX 1660 Ti / 驱动 617.14
- 分辨率/画质设置: 1920x1080 / 高画质
- mod 环境: MO2 启动 / 无着色器修改类模组（CS/ENB/ReShade 无）+ ussep、alternate start、racemenu、photo mode、skyui 及其前置；无其他材质/后处理 MOD
- 相机坐标: X= *____* Y= *____* Z= *____* | 朝向: *截图文件*
- 抓帧方式: capture-helper 插件 F12（RenderDoc 1.46，NvAPI 白名单已开）
- 帧数: 1
- .rdc 文件: *归档后的文件名*
- 备注: 

## 分析笔记（RenderDoc 打开后补填）

- Draw call 总数 / Dispatch 数:
- 管线数量（去重后 PSO 数）:
- 着色器数量（去重）:
- 主要 RT 格式:
- 发现的特殊点:
