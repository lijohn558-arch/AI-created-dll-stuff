/*
 * poc-presenter — PoC-A v1.7 (Present 拦截) + PoC-B v0.1 (Vulkan 注入), 插件版本 0.13.0
 *
 * 目的 (docs/00 首周行动项 #4 / 最高风险项 #1 的第一环):
 *   证明能在真实游戏进程内拦截 IDXGISwapChain::Present —— 这是 PoC-B (在 Present 里
 *   创建 Vulkan instance/swapchain 并注入呈现) 的先决条件。本步只记日志、不碰 Vulkan,
 *   不改变任何画面内容, 失败面最小。
 *
 * v1.0 实测结论 (2026-10-02, 用户日志):
 *   - dummy 走 CreateSwapChainForHwnd 得到的交换链, 其 vtable 槽 4 的原值指向
 *     renderdoc.dll (in-app 已先行挂钩), 安装成功;
 *   - 但 30s 内 0 次 Present 触发, 游戏画面正常 → 游戏实际使用的交换链 vtable 与
 *     dummy 的不是同一张 (接口级 v0 与 v1 布局不同, 或 RenderDoc 包装类/实例隔离)。
 *
 * v1.1 对策 (四通道 + 自证):
 *   1. 多 vtable 收集: dummy1 (ForHwnd, SwapChain1 口径) + dummy2 (工厂 v0 CreateSwapChain
 *      口径) + 两者各自 QI SwapChain / SwapChain1 / SwapChain2 / SwapChain3 —— 收集到的
 *      每张不同 vtable 都挂槽 4 (当时标为 Present — v1.7 勘误: 实为 SetPrivateDataInterface,
 *      真 Present=槽8); 经 1 口径见过的 vtable 额外挂槽 18 (当时标为 Present1, 勘误:
 *      实为 GetDesc1, 真 Present1=槽22; 越界保护: 只有 QI 上 1 的 vtable 才读 18/22);
 *   2. 工厂拦截: 挂我们工厂 vtable 的槽 10 (CreateSwapChain ✓)、槽 14/15/17 (当时标为
 *      ForHwnd / ForCoreWindow / ForComposition — v1.7 勘误: 实为 IsWindowedStereoEnabled /
 *      ForHwnd / GetSharedResourceAdapterLuid, 真槽为 15/16/24) —— 游戏经同款工厂创建交换链时, 拿到游戏自己的
 *      对象引用并就地打它的 vtable (兜住"每实例一张 vtable"的情形), 日志带 ★ 标记;
 *   3. 挂钩原值安全阀: 只有原值落在 dxgi.dll 或 renderdoc.dll 内才挂 (防错槽位);
 *   4. 看门狗: 每 5s 复查所有已挂槽位, 被第三方改写则记录并打回; 120s 仍 0 次 Present
 *      给出汇总告警 (已登记 vtable 数), 按日志证据迭代。
 *
 * v1.2 对策 (v1.1 实测 2026-10-02: 仍 0 触发、无 ★, 但游戏内 RenderDoc overlay 出现
 *   "D3D11 window0 / window 1 active" —— 证明游戏与 dummy 两个交换链都被 RenderDoc 登记,
 *   且 window0 (游戏) 排在我们的 dummy 之前):
 *   1. [主因 P1 时序] SKSE 在主线程同步加载插件, v1.1 在后台线程安装 (21:16:20 完成),
 *      游戏渲染器初始化夹在 Load 返回与后台安装完成之间就把交换链建好了 → 钩子全晚。
 *      v1.2 改为在 SKSEPlugin_Load 内同步安装 (等模块 ≤5s、等 renderdoc ≤2s 兜加载序),
 *      Load 不返回主线程就无法继续初始化 → 保证先于游戏交换链创建;
 *   2. [保险 P2 口径] 五个工厂口径全取: CreateDXGIFactory 与 CreateDXGIFactory1 各配
 *      IID0/1/2, 加 CreateDXGIFactory2 —— RenderDoc 包装类若按"所请求接口"分化,
 *      游戏要的那张类也在登记之列; QI 向上 Factory2 成功即布局含槽14/15/17 (安全升级;
 *      v1.7 勘误: 真槽应为 15/16/24, 见 v1.7 段)。
 *
 * v1.3 对策 (v1.2 双跑实测 2026-10-02 — 同步安装 P1 已修: 两跑均"同步安装完成"):
 *   - 有 capture-helper: 五口径工厂全 OK 但同为 renderdoc.dll 一张类, 120s 仍无 ★、
 *     0 次 Present → 游戏的工厂不是我们 patch 的那张类 (P3);
 *   - 无 capture-helper: ★ 在 +7s 正常触发 (CreateSwapChain → dxgi 真类) →
 *     游戏拿工厂的路径在无 renderdoc 世界 = 真 dxgi 类, 有 renderdoc 时落到
 *     "非导出"路径 (设备链 GetParent, 或 renderdoc 生效前已取得的真对象)。
 *   → v1.3 增设设备链通道: device → QI IDXGIDevice → GetAdapter → GetParent 三 IID,
 *     每个新工厂先建辅助 v0 dummy (拿它所属类的交换链 vtable — 若是真 dxgi 类,
 *     游戏 Present 即被槽4截住), 再挂其创建方法; 日志带"设备链"前缀自证落在哪个模块。
 *
 * v1.4 对策 (v1.3 双跑实测 2026-10-02 22:15/22:18 — 同步安装两跑均 OK):
 *   - 两跑都有"设备链 GetAdapter"行但零"设备链工厂"行 → v1.3 代码 bug: 在适配器上
 *     用了 QI 而非 GetParent (适配器不实现工厂接口, QI 必失败且被静默吞掉) →
 *     v1.4 改 GetParent + 失败打日志 + 适配器行改打 vtable 所在模块;
 *   - 无 capture-helper: ★ 于 +8s 触发后日志终止 — registerSwp 对"与 dummy 同类"
 *     的 vtable 原是静默匹配, 之后无 Present 行 → v1.4 给匹配路径补日志、★ 行附
 *     swapDesc, 下一跑即可分辨"同类已挂" vs "新类" vs "挂后无 Present";
 *   - 有 capture-helper: 仍 0 ★ → 与"游戏工厂在 SKSE/renderdoc 生效前已是真 dxgi
 *     类"假说一致; GetParent 若拿到真类即双类覆盖成立, 若仍 renderdoc 类 →
 *     下一步加载序方案 (先于 capture-helper 加载, 拿未被 renderdoc 拦截的真导出)。
 *
 * v1.5 探针 (v1.4 双跑实测 2026-10-02 22:34/22:38 — ★ 链全通但 Present 恒 0):
 *   - 无 renderdoc: ★ 拦到游戏交换链 (1920x1080 title="Skyrim Special Edition"),
 *     对象 vptr = 我们挂好的类 vtable, 看门狗 120s 未见槽位被改写, 但计数恒 0 →
 *     游戏的 Present 虚调用根本没走到我们的槽 (对象 vptr 后变 / 另一交换链宿主 /
 *     覆盖层包装 / 或未在渲) — 只能加探针取证;
 *   - 有 renderdoc: 设备链 GetParent 成功但三工厂+辅助 dummy 全落 renderdoc 类,
 *     游戏工厂仍不在其中 → 0 ★; 需真 dxgi 类对象 (加载序方案, v1.6 备选)。
 *   探针 A (对象级): ★ 记录游戏交换链指针, 看门狗每 5s 重读其 vptr/slot4, 变更即打日志;
 *   探针 B (字节): 安装末尾打 Present/Present1/CreateSwapChain 原实现前 32 字节,
 *     为函数级 detour (任何虚调用路径都会经过的真实现) 选安全窃取长度;
 *   探针 W (窗口): 安装末尾与 t=10s/60s 枚举本进程顶层窗口, 找第二个交换链宿主;
 *   附带: dummy3 FLIP_DISCARD 口径纳入登记 (翻转类 vtable 覆盖)。
 *
 * v1.6 对策 (v1.5 实测 2026-10-02/03 — renderdoc 存根字节实锤 + 本机 dxgi 字节取齐;
 *   采纳外部同好对四方案的建议, 落地方案B + 顺手 bug 修):
 *   - 有 renderdoc: 探针 B 实锤包装类 slot4 = 11 字节动态转发存根
 *     (48 8B 49 10 / 48 8B 01 / 48 FF 60 20) — 每次调用重取真对象 slot4, 不缓存 FP;
 *   - 无 renderdoc (GFE 在场, run2): 类 vtable 挂好、看门狗零改写、计数恒 0 →
 *     存在绕过类 vtable 的 Present 调用路径 (覆盖层包装 / 缓存函数指针);
 *   - 本机同 boot 只读转储取齐 dxgi 真存根字节 (与游戏同 dxgi 基址):
 *     Present  @dxgi+0x2E460 前14B 恰为指令边界且零地址依赖;
 *     Present1 @dxgi+0x4EE60 边界在 15B — 窃取长度与字节守卫由此定死。
 *     〔v1.7 勘误: 这两处实为 slot4/slot18 存根本体 = 自证循环; 真 Present/Present1
 *       是 dxgi+0x19000 / 0x194A0, 见 v1.7 段。〕
 *   → 方案B (函数级 detour, 覆盖一切调用路径 — v1.4 计数恒 0 的破局点):
 *     1) 目标 = dxgi 存根 (RVA + 前导字节双校验; 字节不符 → 保守跳过并留档);
 *     2) 入口只改 5 字节 E9 → 近端跳板 (±2GB 内 VirtualAlloc, 存 FF25 绝对跳到钩子)
 *        — 只碰前 5 字节, 不破坏第三方 trampoline 的续接字节;
 *     3) 入口若已被第三方 (GFE/renderdoc) 改成跳板 (E9 / FF25 / mov rax,jmp rax)
 *        → CHAIN: 不做 trampoline, 钩子计数后直接转发其目标, 链式共存;
 *     4) vtable 钩子转调期间置 TLS 标记, 函数钩子据此跳过计数 — 双层不双计,
 *        任何单一路径必被其一计到。
 *   附带: 设备链工厂 IID 行补打 vtable 所在模块 (原打对象地址=堆上, 恒 "来自 ?")。
 *   暂缓: 方案A (解包 renderdoc 包装取真对象) 留作 v1.7 备选 (B 若通则 A 仅诊断增益);
 *         方案D (capture-helper 改名 z- 先加载) 零成本可顺手验证; 按建议不再追真
 *         工厂 vtable (★ 仅诊断, 目标是 Present 而非创建路径)。
 *
 * v1.7 根因勘误 + 全量槽位修正 (v1.5/v1.6 实测 + 本地 ABI/字节实证 2026-10-03):
 *   [根因] v1.0~v1.6 挂的"槽4/槽18"按 MS dxgi.h 实为 SetPrivateDataInterface (IDXGIObject
 *     第4项) 与 GetDesc1 (SwapChain1 第18项) — 从未挂到 Present, 旧 0 计数恒真;
 *     旧字节守卫 dxgi+0x2E460/0x4EE60 就是这两槽的存根本体 (探针B 与守卫互相自证循环)。
 *     正确 ABI (dxgi.h + 本地 dxdump3 双证): Present=槽8, Present1=槽22;
 *     GetDesc=槽12 (swapDesc 直调安全)。上述历史段落中的 槽4/槽18 一律按 8/22 读。
 *   1. 交换链全量改号: 登记/挂钩/查原值/看门狗/探针A/探针B 全部 vtbl[8] (Present) +
 *     vtbl[22] (Present1, 仅 QI 上 1+ 的 vtable 才读);
 *   2. 方案B 靶子改运行时取: 优先直读活交换链 vtbl[8]/vtbl[22] (彻底摆脱硬编码 RVA),
 *     其次方案A 解包出的真对象 vtbl[8]/[22], 最后回退本机 dxgi RVA 0x19000/0x194A0;
 *     前导 14B 守卫 (末字节 56=Present / 54=Present1 可区分), 旧"交叉验证行"删除,
 *     0x2E460/0x4EE60 作废 (那是 slot4/slot18 存根, 不是 Present);
 *   3. 工厂槽位按 MS dxgi.h 重排: 保留 10 (CreateSwapChain, run2 ★ 实证),
 *     ForHwnd 14→15、ForCoreWindow 15→16、ForComposition 17→24 —
 *     旧 14 实为 IsWindowedStereoEnabled、旧 17 实为 GetSharedResourceAdapterLuid,
 *     签名不符属潜在崩溃点 (旧 15 虽是 ForHwnd 却被挂了 CoreWindow 钩子, 同样错位);
 *     15/16/24 仍仅在 QI Factory2 成功后才挂 (Factory2 布局末项=24, 防越界)。
 *     (外部建议的 13/14/22 与 MS ABI 不符: 13=IsCurrent、14=IsWindowedStereoEnabled、
 *      22=RegisterOcclusionStatusEvent, 均非创建路径 — 以 dxgi.h 为准。)
 *   4. 阳性确认: ★ 拦到游戏交换链与安装完成时, 都打印对象 vptr + vtbl[8] 是否等于我们的
 *     钩子 (并区分"已是钩子 / 尚未挂钩→就地挂"); 120s 告警行附 g_gameSc 是否取得 —
 *     摆脱 v1.0~v1.6 "只证明挂了错槽"的假阳性 (旧 ★0 / g_gameSc 空即此类);
 *   5. 方案A 落地 (renderdoc 转发桩解包): 桩自曝偏移 — v0 系方法读 [obj+0x28] 取真对象 →
 *     *(void**)(wrapper+0x28) = 真 IDXGISwapChain*, 其 vtbl[8] 即 dxgi 真 Present
 *     (版本无关且覆盖真类); 仅对 renderdoc 包装类对象尝试 (读内存恒安全), 结果只作
 *     方案B 的一个候选 (可执行页 + 14B 字节守卫双重兜底);
 *   6. 兜底不变: 函数级 detour 是唯一与对象无关的漏斗 (vtable 层到不到, 它都到),
 *     TLS 双层不双计逻辑原样保留。
 *
 * 钩子转调: 按"调用方 vtable 地址"查登记表取原函数 —— 多张 vtable 各存各的原值。
 * 线程安全: 登记表/槽位写入全部走同一把 CRITICAL_SECTION (可重入)。
 *
 * 验收标准 (本地游戏内, 日志 = <游戏>/Data/SKSE/Plugins/poc-presenter.log):
 *   [..] renderdoc.dll = ...            (in-app 加载状态)
 *   [..] dummy1 ... / dummy2 swapchain ...
 *   [..] dummy1(ForHwnd): vtable=0x... 来自 ...      (vtable 本体所在模块, 判包装类)
 *   [..] ...: slot8 原值=0x... 来自 ...               (槽8=Present, 槽22=Present1)
 *   [..] 登记完成: 交换链 vtable N 个, 工厂 vtable M 个
 *   [..] 阳性确认 (dummy1 安装完成): obj=... vptr=... vtbl[8]=... ==我们的Present钩子 ✓  ← 槽位修对的铁证
 *   [..] ★ 工厂拦截 CreateSwapChain... → swapchain=0x...   (可选, 视游戏创建路径)
 *   [..] 阳性确认 (★游戏 CreateSwapChain): obj=... vptr=... vtbl[8]=... (已是钩子 / 尚未挂钩→就地挂)
 *   [..] 第 1 次 Present (经 Present): ... ← 关键行 (证明拦到游戏 Present)
 *   [..] Present 已开始触发 — PoC-A 验收通过
 *   [..] Present 计数 600  近600帧 xx.x FPS          (周期行)
 *   且游戏画面完全正常 (本 PoC 不改变呈现)。
 *
 * ---- PoC-B v0.1 (插件 0.9.0, 2026-10-03): 在 Present 里跑 Vulkan 并注入像素 ----
 *   PoC-A 双环境验收已通过 (带 renderdoc / 无 renderdoc, 判读见 docs/01 §7.6 §7.7);
 *   PoC-B 接着回答风险项 #1 的"写"这半 —— 拦到 Present 之后, 能不能把 Vulkan 渲出的
 *   像素送进游戏最终呈现的那一帧。这是 docs/00 收尾决策 #6 (全原生 vs 混合) 的判据。
 *   路径: Vulkan 离屏 512×512 (洋红清屏 + 三角形) → vkCmdCopyImageToBuffer 读回 →
 *     UpdateSubresource → CopySubresourceRegion 拷进 backbuffer 的 (16,16) → 调原 Present。
 *     刻意不开 Vulkan swapchain: 两个 swapchain 抢同一个 HWND 会与游戏 / RenderDoc / GFE
 *     争呈现所有权且结果不可判; 而像素落在 D3D11 帧内 ⇒ F12 抓帧必然记录这次拷贝。
 *   关键日志:
 *     PoC-B init: backbuffer = 1920x1080 format=28 (期望28=R8G8B8A8_UNORM)
 *     PoC-B init: GPU = "..." LUID匹配=是|否 apiVer=1.x
 *     PoC-B init 完成: 离屏 512x512 (洋红清屏+三角形) → 读回 → CopySubresourceRegion 到 (16,16)
 *     PoC-B 第 1 帧注入: 渲染+读回+拷贝 x.xx ms → ... 已提交
 *     PoC-B 注入 600 帧, 累计均值 x.xx ms/帧 (本帧 x.xx ms)
 *   失败即关: 任一步失败打 "PoC-B 失败: <步骤> code=... — 已关闭注入, 游戏照常呈现";
 *   逃生门: <pluginDir>\poc-presenter.ini 写 vulkan=0 → 不重编译即可关掉注入。
 *   验收: 游戏画面左上角出现 512×512 洋红块 + 三角形, 且 F12 抓的帧里能查到该像素
 *     (提取链: tools\rdc_pass7_pixels.py 读 backbuffer 探针点)。
 *
 * ---- v0.9.1 (2026-10-03): 修实例级函数表误查设备级 + 空指针按名落日志 ----
 *   首局根因: v0.9.0 把设备级 vkGetDeviceQueue 混进 vkGetInstanceProcAddr 之后的必查项
 *   (规范允许对设备级返回 NULL, renderdoc 包装层更倾向如此) → 卡死在"实例级函数表不完整"。
 *   修法: 拆 POCB_INST_REQ_FNS 必查 7 项 (全实例级, properties2 降可选), 实例级/设备级都用
 *   X-macro 全表校验, 空指针按函数名进日志。次局 4200 帧零失败, 均值 6.47 ms/帧。
 *
 * ---- v0.9.2 (2026-10-03): ini 开关 vtable=0 → 方案B 独立计数 ----
 *   遗留(docs/01 §7.7 末): vtable 层恒先命中 + TLS 去重, 方案B 函数层的计数从未被单独观察。
 *   新增 iniFlag("vtable"): 写 0 → 不挂交换链槽 8/22、看门狗不"打回"、阳性确认改口径、
 *   登记行明示未挂 ⇒ 游戏 Present 只能从方案B 的函数级 detour 进来, 日志应出现
 *   "第 1 次 Present (经 函数detour)"。可与 vulkan=0 同用 (一次跑局清两条遗留)。
 *
 * ---- v0.9.3 (2026-10-03): 函数层 detour 也驱动 PoC-B (兜底模式补注入) ----
 *   0.9.2 对照局发现: pocbFrame 只在 hookedPresent/hookedPresent1 (vtable 层) 里调,
 *   vtable=0 时函数层 detour 只计数不注入 ⇒ 兜底模式下 PoC-B 永远不跑, 逃生门
 *   pocbEnabled() 也走不到 (日志看不到 "关闭了 vulkan 注入" 那行)。
 *   修法: detouredPresent/detouredPresent1 在 !g_inVtableHook 分支里也调 pocbFrame
 *   (经 vtable 转来的调用 TLS 置位, 不会二次注入)。
 *
 * ---- v0.10.0 (2026-10-03): 阶段1 主体 —— 共享纹理通路 + 水体探针升质 ----
 *   ① 共享纹理 (docs/00 §1.1 决策4): 替掉 6.47ms/帧 的每帧同步读回。
 *      路径: D3D11 创建 SHARED|NTHANDLE 纹理 → IDXGIResource1::CreateSharedHandle
 *      → VK 经 VK_KHR_external_memory_win32 导成 VkImage (OPAQUE_WIN32) →
 *      每帧 renderpass 后 vkCmdCopyImage 离屏图→共享图 → D3D11 CopySubresourceRegion
 *      直接从共享图拷进 backbuffer (不再 vkCmdCopyImageToBuffer + UpdateSubresource)。
 *      同步: 仍是 CPU fence 交接 (vkWaitForFences), 另加 D3D11_EVENT 查询闸 —— 上一帧
 *      D3D11 拷贝没跑完不提交本帧 VK 写, 关掉"帧N读 vs 帧N+1写"跨 API 竞态。
 *      降级: 扩展缺失 / NT handle 失败 / 导入失败 / ini shared=0 → 自动回退读回路径。
 *   ② 探针升质 (水体样本第一步, docs/03 §6.1): 拦 ID3D11Device::CreateTexture2D (槽5),
 *      501 = 6面512² RGBA16F 环境反射 cubemap 探针的描述符命中时升成 1024² (只动创建
 *      参数, 游戏自己的 pass 照常渲染, 采样端自动变清晰)。
 *      逃生门: probe=0 不挂槽; vulkan=0 同样不挂 (S4c 基线纯净)。
 *   关键日志 (新增):
 *     探针升质: 设备 vtable=... slot5 原值=... 来自 d3d11.dll / renderdoc.dll
 *     探针升质: 槽5 已挂 (512² cube→1024², 逃生门 probe=0 / vulkan=0)
 *     探针升质: 512² RGBA16F cube(6面) → 1024² 第 N 次, hr=0
 *     PoC-B init: 共享纹理 NT handle OK / 共享图导入 OK (D3D11 NT handle → VkImage)
 *     PoC-B init 完成: ... → 共享纹理 (NT handle+event闸, 无读回) → ...
 *     PoC-B 第 1 帧注入: 渲染+共享拷贝 x.xx ms (fence x.xx / 闸 x.xx) → ...
 *
 * ---- v0.11.0 (2026-10-03): 修探针升质两处缺陷 (S4d/S4d2 判读定案, docs/02 §14.4/14.5) ----
 *   S4d 双帧判读: v0.10.0 升质只翻 cube 不翻 depth → OM = RT1024² + DSV512² 尺寸不匹配
 *   → 绑定判非法作废 (回放 8 条 High "Invalid output merger"; 初态内容链证真机 draw 同样
 *   未落地 = 反射 cube 全帧平铺清屏色 = 游戏内视觉回归); 且游戏用创建时缓存 desc →
 *   viewport 恒 512² 字面量 (即便①修好也只渲 1/4 面)。两处同修:
 *   ① 配对 depth 同步升质: cube 命中后开窗, 512² D24 家族 (R24G8_TYPELESS 等 4 个合法
 *      DXGI 枚举 —— RenderDoc 显示名 "D24S8_TYPELESS", 枚举名里没有 D24S8_TYPELESS;
 *      纯2D, mips=1/array=1, DSV bind) → 1024²。依据 rdc_tex_desc 实测: 全帧唯一 512² 深度
 *      = 探针 depth; ResourceId 时序 544(cube) < 552(depth) → 前向开窗即可配对。
 *   ② 升质宽高回写游戏自己那一份 desc (const 是 API 约定, 游戏栈变量可写; 写前 VirtualQuery
 *      查页保护, 只读页放弃+告警不崩) —— 游戏缓存的 viewport 等派生值跟着变 1024²。
 *   逃生门不变: probe=0 不挂槽; vulkan=0 同样不挂。
 *   验证闭环 (抓 S4e 后): rdc_api_scan 看 probe 段 viewport=1024² / rdc_dump_cubefaces
 *      看 0 条 OM 诊断 + 六面内容互异 / rdc_dump_cubefaces 面 PNG 对照 S4d 平铺色。
 *   关键日志 (新增):
 *     探针配对depth: 512² D24 → 1024² 第 N 次, hr=0
 *     探针升质: 游戏描述符页只读 … — 放弃回写 (仅异常内存布局出现, 出现即 §14.5 plan-B)
 *     探针配对depth: 带初始数据 — 不升 (升了会越界读), 配对失败告警
 *
 * ---- v0.12.0 (2026-10-04): plan-B —— 绑定感知拦 RSSetViewports/RSSetScissorRects ----
 *   S4e/S4e2 双帧实测 (docs/02 §14.7): v0.11 回写两行全 ok (depth 552 实升 1024², 判据①
 *   0 诊断 + RT/DS 配对 PASS) 但 probe 段 RSSetViewports 恒 512² (全帧无 1024² 字面量)
 *   → 游戏 viewport 来源不在被回写的那份 desc → 回写改不到, 六面只渲 TL 1/4 象限。
 *   plan-B (§14.6 预declared 分支): context vtable 层拦 —— RSSetViewports(槽44) /
 *   RSSetScissorRects(槽45) 调用时, ① 视口/裁剪恰 512² 且 ② 当前 OM RT0 解析回 probe
 *   cube (OMGetRenderTargets→GetResource 对象身份) → 翻 1024² 再下传; 否则原样下传。
 *   绑定感知, 不做无条件 512² 全拦 (防误伤其他 512² pass)。槽号双证: 官方 d3d11.h MIDL
 *   声明序 (IUnknown 0-2 + DeviceChild 3-6 + 接口偏移 7+37/7+38) 与 xosh vtable 表吻合。
 *   逃生门不变: probe=0 / vulkan=0 → 槽44/45 同样不挂 (与槽5 同门)。
 *   验证闭环 (抓 S4f 后): rdc_api_scan 看 probe 段 RSSetViewports=1024² / rdc_dump_cubefaces
 *      看六面全幅内容 (四象限互异) + 0 条 OM 诊断, 对照 S4e 的 TL 1/4 象限。
 *   关键日志 (新增):
 *     探针升质: ctx vtable=… slot44(RSSetViewports) 原值=… 来自 …; slot45(RSSetScissorRects) …
 *     探针升质: ctx槽44/45 已挂 — probe cube 绑定中 512²→1024² (v0.12 plan-B)
 *     探针升质: RSSetViewports 512²→1024² (probe cube 绑定中) 第 N 次
 *     探针升质: GetImmediateContext 空 / ctx slot44/45 原值不在 d3d11/renderdoc — 跳过
 *
 * ---- v0.13.0 (2026-10-04): SSR Step 1 侦察钩 —— ctx 槽33/50 只记日志 (docs/05 §5) ----
 *   SSR 节点替换第一步: 真机验证反射段边界特征可否稳定识别 (R1: RenderDoc 事件号是抓帧
 *   局部量, 真机不可用)。挂 immediate context 槽33 (OMSetRenderTargets) + 槽50
 *   (ClearRenderTargetView), 只读解析调用参数, 完全不改渲染 (不动任何参数/状态)。
 *   槽号双证 (官方 d3d11.h MIDL 声明序, 与 xosh 44/45 同口径): 33/34/44/45/47/50/53/89;
 *   游戏 api-scan 零命中槽34 (OMSetRenderTargetsAndUnorderedAccessViews) → 不挂;
 *   OMGetRenderTargets 实为槽89 (main.cpp 旧注释误写 33, 已修正)。
 *   判据 (docs/05 D1/D3a, 按对象身份 QI + 帧号 g_presentCount):
 *     候选   = 首个非空 RT 视图为 1920x1080 R16G16B16A16_FLOAT (按非空视图数, 防 n=8 带 null);
 *     强特征 = 候选 + DSV 1920x1080 D24 家族 —— **按"进入"计数, 不按次绑**: 真机段16 每个
 *              Draw 前都重绑一次 (一帧 16 次), 按次计则 fs 恒=16, "同帧恰1" 判据必挂;
 *     特征A  = 强特征通道"进行期间"的 ClearRenderTargetView (被清对象不参与判定);
 *     特征B  = 强特征后首次 OMSet 换绑到别的 RT0 (段16 → 段17 ev39512 的 321+591)。
 *   S4 api-scan 实测修正两点 (docs/05:105 与 S4-water.md:114 的旧说法作废):
 *     ① ev39511 清的是 view592 = 纹理 591 (段17 的 RT1, R10G10B10A2), 585 整帧从不被清 ——
 *        pass5 的 clears.rts 取自 bound_targets(), 是"当时绑着谁"不是"清谁";
 *     ② D1 的"同帧唯一"不成立: 同帧有 2 个签名完全相同的强特征段 —— 段16 反射段
 *        (585, 16 Draw) 与 段19 后期前处理 (321, 1 Draw, PS1628)。故另记判别子
 *        "本帧 RT0 绑定次数/连续段数": S4 实测 585=16次/1段、321=50次/4段 (全帧共 142 次
 *        OMSet, 按本函数同口径回放 `S4-api-scan.txt` 得出)。次只差 3 倍不可靠, **判别取
 *        "1 段 vs 4 段"** —— 反射目标按定义整帧只写一次即被段17 消费, 主 HDR 缓冲处处复用。
 *        注意该判别只在"强特征对象集合内"有效: 全帧另有 339 = 17次/1段 (段签名非强特征),
 *        所以不能拿"次数≈16 且 1 段"去全帧海选, 只能用来在 2 个强特征段里二选一。
 *        Step 2 固化 D1 时用它二选一。
 *   挂载门: pocbEnabled (vulkan 总闸) + g_probeOn + ini ssr=1 (D5 默认 0 → 零新增拦截);
 *   ssr=0 时连槽33/50 都不挂, 基线帧与 v0.12.0 完全同路径。
 *   日志节流: 候选详情按 (对象,强征与否) 去重后前16条 + 每64条; A/B 详情前16条 + 每64条;
 *   notePresent 内帧汇总 (前5帧恒打 + 每60帧有活动时 + 异常帧恒打)。
 *   真机判读 (2026-10-04, 4472 帧, MO2 overwrite\SKSE\Plugins\poc-presenter.log):
 *     挂载 ok (槽33/50 原值来自 renderdoc.dll, 7 秒后 context 换指针 → vtbl 观察 fallback 生效);
 *     判别子完全成立: 反射段 10~19次/1段 vs 主HDR 10~370次/4~6段, 零重叠;
 *     objN 恒<=2 (2481 帧=2 / 126 帧=1), 特征A 游戏帧 100%=1 (64/66 归因反射段, 被清对象
 *     64/66 = 1920x1080 R10G10B10A2 即段17 RT1 格式), 特征B 100%=1 (游戏帧 48/54 = 非空2
 *     RGBA16F 即段17 MRTx2)。
 *   正常帧基线 = distinct<=2 + 特征A1 + 特征B1; 异常 = 有候选却强特征0 /
 *   有强特征而A+B==0 / A 单项>1 / B 单项>1 / distinct>2 (A、B 分开计阈值,
 *   不用 A+B>1 —— 两者在正常帧里本就各 1 次); 加载过场无候选无强特征, 不算异常。
 *   注意: fs (强特征=进入次数) 真机恒 3~4 (主HDR 2~3 次进入), **不作为异常判据** ——
 *   旧规则 "fs>2" 基于 S4 单帧真值, 真机一跑就出 2571 条假阳性把日志冲到 1MB, 已删除。
 *   段16 缺席 (objN=1 且唯一对象 runs=5) 真机出现于连续窗 f2552-f2652 (≈1.7s, 场景无反射),
 *   是取景状态不是钩子失效 —— 由每60帧定期打印可见 (distinct=1 + runs=5), 不设专门判据。
 *   关键日志 (新增):
 *     SSR侦察: ctx vtable=… slot33(OMSetRenderTargets) 原值=… 来自 …; slot50(ClearRTV) …
 *     SSR侦察: ctx槽33/50 已挂 — 只记日志不动渲染 (v0.13.0 Step1; 逃生门 ssr=0)
 *     SSR侦察: 槽33候选#N 帧=F 本帧第M次候选 n=1 非空=1 RT0=0x… (1920x1080 RGBA16F) DS=… 强特征
 *     SSR侦察: 特征A(强特征期间清)#N 帧=F 被清=0x… … 强特征RT0=0x…
 *     SSR侦察: 特征B(换绑)#N 帧=F 新RT0=0x… (…) 非空=2
 *     SSR侦察: 帧=F OM=N 候选=N 强特征=N distinct=N 特征A=N 特征B=N | 累计候选=N 强特征=N
 *     SSR侦察: 帧=F 强特征对象i=0x… 本帧RT0绑定=N次/M个连续段   ← 判别子 (强特征集合内 1段 vs 4段)
 *
 * ---- v0.14.0 (2026-10-04): SSR Step 2a/2b —— 槽47 身份识别 + 通路哨兵 (docs/05 §5) ----
 *   Step 1 (v0.13.0) 证明反射段真机可定位; Step 2 把"观察"推进到"介入"。拆两半先做 2a/2b
 *   (零 VK 风险、抓帧可验收), 过了再做 2c 共享入向 + 2d VK passthrough。
 *   2a 身份识别 —— 324/520/585/321 是抓帧 ResourceId, 跨帧不可用 (R1), 只能按"来源"认:
 *     585 反射目标 = 强特征集合里 runs==1 那个 (Step 1 判别子), 上一帧 Present 定;
 *     321 主 HDR   = 强特征集合里 runs>1 那个, 同上; 两者帧间持久不复位;
 *     324 场景色快照 = 新挂 ctx 槽47 (CopyResource), src 为 1920x1080 RGBA16F 时的 dst
 *                    (S4 全帧 3 条 Copy 里唯一 src 为 RGBA16F 的就是 ev39225 的 321→324,
 *                     两条深度拷贝 src 是 D24 家族被格式挡住);
 *     520 深度快照   = 本帧**首条** src 为 1920x1080 D24 家族的拷贝之 dst (S4 ev21505 的
 *                    461→520; 本帧第二条 461→466 是后期深度, 靠帧号锁 g_ssrCopyFrame
 *                    只取首条)。2c 才用到, 本轮只学并打日志。
 *     身份统一走 QI ID3D11Texture2D 的指针值 (COM 契约: 同对象同 IID 必返同指针, 与
 *     ssrViewDesc 同口径) ⇒ OMSet 侧与 CopyResource 侧可直接比对。
 *     但**裸指针值不能解引用** (QI 后即 Release) —— 2b 要真调 CopyResource, 故另持两份
 *     活引用: g_ssrSceneRes (324, 来自 CopyResource 的 pDst AddRef) 与 g_ssrStrRes
 *     (最近强特征对象, 来自 RTV GetResource)。持 AddRef 也顺带保证对象不被游戏销毁。
 *   2b 通路哨兵 (docs/05 §5 Step 2 行): 特征B(段16→段17) 判定 + "刚离开的强特征对象==585"
 *     才排队, 在 hookedOMSetRenderTargets 的 **real() 之后** 执行 CopyResource(585<-324):
 *       · 放 real() 后 = 段17 的 {321,591} 已绑、585 恰好已解绑 → 写非绑定资源最干净;
 *       · 段17 的 1 Draw (ev39530) 尚未发生 ⇒ 它随后读到的 585 已是 324 的内容 = 可辨识的
 *         "旧场景色"错误画面 (类洋红哨兵思路), 这就是 Step 2 的验收画面;
 *       · 段16 缺席的窗口里第一个强特征是段19(321)≠585 ⇒ 不排队, 不写错缓冲。
 *     逃生门与总门分开: ssr=1 只挂槽+记日志 (2a), **ssr.sentinel=1 才回写** (2b) —— 首次
 *     动渲染的操作值得单独开关, 出问题改 0 即退回纯观察, 2a 的身份行照常打。
 *   2c 共享入向 (v0.15.0, docs/05 D2a-a + 风险 R2): 触发点与 2b **同位** (特征B → real()
 *     后), 开关独立 (ssr.shared, 默认 0)。做法 = 建 2 张 SHARED|NTHANDLE 镜像**照抄** 324 色
 *     与 520 深度的 desc, 每帧 CopyResource(源→镜像) 各一下, CreateSharedHandle 取 NT handle
 *     交给 2c-β 的 VK (OPAQUE_WIN32) 导入。
 *       · **只往我方镜像写, 不动游戏资源、不改画面** —— 与 2b 的"改 585"性质不同, 故另给一道门;
 *       · 不要求 lastStr==585 (入向不碰 585), 但要求双强特征格局已确立 (菜单期不 arm, 同 D2a-3a);
 *       · R2 验收点 = D24 深度与 FP16 这两个 PoC-B 没验过的格式: 能否建出 SHARED、DXGI 是否
 *         允许共享、拷进去后 STAGING 读回校验和是否 ≠ "未拷"基线 —— CopyResource 返回 void、
 *         失败静默 (2b 踩过), 所以必须自己读回证一次; 读回阻塞故节流 (前 3 次 + 每 600 次)。
 *     逃生门同前: ssr.shared=0 即退回 v0.14.0 行为; ssr=0 则三槽全不挂。
 *   槽47 校验失败只降级该槽 (33/50 照常), 绝不挂 orig47 为空的槽 —— 否则 hookedCopyResource
 *     早退会把真调用丢掉, 游戏所有拷贝消失、渲染直接崩。
 *   帧汇总新增 COPY= (本帧 CopyResource 次数)、哨兵= (2b 累计回写) 与 入向= (2c 累计触发);
 *   强特征对象行新增 [585]/[321] 标注。
 *   状态: **2a/2b 已真机验收通过** (2026-10-04, `407c8b3`, 5520 帧, 11/11 全绿、0 [异常]、
 *   PoC-B 0.88–0.98 ms/帧; 目视形态 = 水几乎消失, 见 docs/02 §14.11.1 与 docs/05 D2a-3)。
 *   **2c-α (v0.15.0) 真机判读结果 (2026-10-04, 3455 帧, docs/02 §14.12)**:
 *     ✅ **色通路全绿** —— RGBA16F/1920x1080 SHARED|NTHANDLE 建得出 + CreateSharedHandle 拿到
 *     NT handle + CopyResource 落地 + STAGING 读回 7/7 全 `≠基线✓` (0 个 `=基线`),
 *     非零样本 7143–7157/8192 且校验和逐帧变 = 内容随场景真实变化; 入向=2576、COPY=3、
 *     哨兵=0、[异常]=0、PoC-B 0.91 ms/帧。
 *     ❌ **R2 深度未过** —— D24 家族 `CreateTexture2D` 直接 `E_INVALIDARG (0x80070057)`。
 *   **v0.16.0 = R2 归因重试 + Step 2c-β VK 交叉校验**:
 *     · 归因重试: 分不清"格式不在 SHARED 白名单" vs "源带 DEPTH_STENCIL 而 SHARED 不许带"
 *       ⇒ 首次失败后按 BindFlags 递减 (SRV→0) 再试两次并逐次记日志, **Format 一律不动**
 *       (CopyResource 要求 src/dst 格式完全一致)。三种全失败 = 格式禁共享 ⇒ 深度需改道。
 *     · **2c-β**: 把色镜像的 NT handle 用 VK `OPAQUE_WIN32` 导成 VkImage, 建 host-coherent
 *       读回 buffer 持久映射, `vkCmdCopyImageToBuffer` 读回后用**与 D3D11 侧同一个**
 *       `ssrFnvSample` 算校验和, 与同帧 D3D11 STAGING 读回的值比对 —— 相等即证明
 *       "D3D11 写、VK 读"这条入向通路的字节视图一致 (SSR v1 每帧都要走的路)。
 *       节奏搭 D3D11 读回的便车 (前3次+每600次), 不做每帧读回; 借用 PoC-B 的
 *       device/queue/pool/fence ⇒ **依赖 PoC-B 开着**, 失败只关自己不碰 `pocbFail`。
 *   **v0.16.1 = 2c-β 导入归因矩阵 + 2x2 探针**:
 *     · 真机首次导入报 `VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)` (PoC-B 的 512² RGBA8 一直好好的,
 *       色镜像是 1920x1080 RGBA16F)。差异只有 格式 / 尺寸 / 内存类型 ⇒ 按
 *       [尺寸候选 x 允许内存类型] 矩阵逐个试并逐次记码, 成了记"哪一组合赢";
 *     · 矩阵全失败 ⇒ 再建两张**探针纹理**做 2x2 归因: A=RGBA8@1920x1080 (只换格式)、
 *       B=RGBA16F@512x512 (只换尺寸), 各自导入一次后即销毁 —— 四种结果直接给出
 *       "病因是格式" 还是 "病因是尺寸", 两条改道方向完全不同。
 *   **v0.16.2 = 帧时基线 (`frametime`)**:
 *     · 谁要: ① 阶段2 / SSR Step4 的过闸判据"帧时不劣于基线 5%"一直**没有基线数**可比;
 *       ② 后续 compute culling + indirect draw 要按帧预算调剔除力度与间接 draw 上限;
 *       ③ `docs/00` §6.5 动态帧预算的数据源。
 *     · CPU = Present-to-Present 帧间隔 (steady_clock, 每帧 1 样本); GPU = `D3D11_QUERY_TIMESTAMP`
 *       + `TIMESTAMP_DISJOINT` 在 Present 处成对打点 (End(ts0) 开窗、下次 Present End(ts1) 收窗),
 *       结果一律 `DONOTFLUSH` 非阻塞读, 没就绪就跳过本帧 —— 观测通路自己不能变成卡顿源;
 *       同一时刻只允许一个 disjoint 活跃 ⇒ 三条全拿到才开下一窗, GPU 样本约帧数的一半。
 *     · 每 600 帧出一行分布统计 (均值/中位/p95/最大 + >20ms 计数), 另有会话累计;
 *       单帧 >=1s (读盘/切场景) 只计数不进分布。逃生门 `ini frametime=0` (默认 1)。
 *   **v0.16.3 = β3 不一致归因三件套** (2c-β 首跑交叉校验 4/4 全不一致, 一次跑图分清三种归因):
 *     · 同刻 D3D11 再读一次 —— `同刻D3D11=0x…=VK✓时序差` ⇒ 不是布局/行距, 是两边读的时刻不同;
 *       `≠帧内(镜像帧内被改)` ⇒ 镜像在特征B 之后又被写过 (时间窗有洞);
 *     · 前帧 D3D11 校验和 —— `前帧D3D11=0x…=VK✓差一帧` ⇒ VK 提交时机比 D3D11 写早一帧;
 *     · 按 D3D11 的 STAGING `RowPitch` 重算 VK —— `VK按D3D11行距=0x…=D3D11✓行距归因`
 *       ⇒ 两边抽样点位差在行距 (归因3), 与共享内存内容无关。
 *   **v0.16.4 = β3 归因二轮: 变体B (usage 对齐 D3D11)** (v0.16.3 真机三件套命中: 时序/行距/
 *     镜像被改 **全 0** ⇒ 排除归因2/3, 落在归因1 布局; 而仓内对照是 PoC-B 的 512² RGBA8 用
 *     `usage = COLOR_ATTACHMENT|TRANSFER_SRC` 能与 D3D11 双向对上, 本图只给了 `TRANSFER_SRC`,
 *     D3D11 侧源却是 `BIND_RENDER_TARGET|SHADER_RESOURCE` ⇒ 驱动给"只读传输图"挑的物理布局
 *     很可能与 D3D11 的不一致):
 *     · 同一块导入内存再建**第二张** VkImage (逐字段同主图, 只把 usage 换成
 *       `COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC|TRANSFER_DST`), 自己的导入分配 + 初转/读回
 *       两条命令; 交叉校验时连着提交两次, **一次打两个 hash**:
 *       `变体B=0x…=D3D11✓usage病因` ⇒ usage 就是病因, 主图改同款 usage 即可;
 *       `变体B=0x…=主图(usage非病因)` ⇒ 两个图读法一致, 病因在别处 (tiling/handle 类型);
 *       `变体B=0x…≠两边` ⇒ 变体也读不出 D3D11 的内容, 继续往 tiling 方向查。
 *       【已废弃】真机结果: 变体B 与主图 hash **完全相同** ⇒ usage 非病因; 且**双图并存
 *       (两张 VkImage 同时绑同一块导入内存) 让驱动返回 VK_ERROR_DEVICE_LOST (-4)**,
 *       PoC-B 被连坐关闭注入 ⇒ 画面上的 VK 三角消失。见 §14.13.3 与 v0.16.5。
 *   **v0.16.5 = 轮换单图槽** (治 v0.16.4 的 DEVICE_LOST + 接着查归因1 的病因;
 *     真机结果 → v0.16.6, 见下):
 *     · **任何时刻只有一张 VkImage 持有导入内存**: 每次交叉校验机会先"按槽建图+初转"
 *       (本帧不比 —— UNDEFINED→GENERAL 按规范可能丢内容), 下一次机会才读回比对,
 *       读完 `vkDeviceWaitIdle` + 销毁, 换下一槽。
 *     · 4 个槽各换一个候选参数, 头号嫌疑在前:
 *       `D3D11句柄` = handleType 换 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`
 *         (被导入对象就是 D3D11 纹理, OPAQUE_WIN32 的语义是"我不解析句柄布局" ⇒ 最可疑);
 *       `LINEAR` = tiling 换 `VK_IMAGE_TILING_LINEAR` (若 D3D11 共享纹理是线性的,
 *         VK 按 OPTIMAL 解读必然错位);
 *       `全程GENERAL` = 命令不转 TRANSFER_SRC, 在 GENERAL 下直接 copy, 只用 access mask
 *         做内存栅栏 (文档里一直挂着的保守姿势);
 *       `对照原样` = 原参数 (OPAQUE_WIN32 + OPTIMAL + 转 layout), 用来自证轮换机制没引入新问题。
 *     · 建图失败 (如 external + LINEAR 不被支持) 只记日志换槽; 连挂 4 个槽才关整条通路;
 *       读回提交返回 `-4` 单独打 `[异常]` 行便于判读 (PoC-B 会被连坐, 三角会消失)。
 *     · 换槽/建图失败**不拖累 PoC-B**, 只记日志跳过; 读回成功后 `vkDeviceWaitIdle` + 销毁本槽。
 *   **v0.16.6 = 病因定死, 收敛到唯一正确参数** (v0.16.5 真机跑图, docs/02 §14.13.5):
 *     · **槽0 `D3D11句柄` 命中: `D3D11=0x7581B71D2B801E3D VK=0x7581B71D2B801E3D **一致✓**`**
 *       ⇒ 归因1 的病因 = **handle 类型用错**: `OPAQUE_WIN32` 的语义是"我不解析这个句柄的
 *       布局信息", 而被导入对象就是 D3D11 纹理 ⇒ 必须用 `D3D11_TEXTURE_BIT` 才让驱动按
 *       D3D11 的布局解读。旁证: 同一 handleType 下 `VK报size=17694720` 第 1/1 组合就导成,
 *       而 OPAQUE 要换到对齐值 `16588800` (第5/5) 才成 —— 正是 v0.16.1 那个 `-2` 的来由。
 *     · 槽1 `LINEAR` 不一致 (没丢设备, 也没解决问题); 槽3 `对照原样` 没跑到。
 *     · 槽2 `全程GENERAL` 读回 19.43ms → **同秒 `PoC-B 失败: … code=-4` 关闭注入 ⇒ 三角消失**:
 *       "不转 layout、直接在 GENERAL 下跨 API 并发读"这个姿势有害 ⇒ **该槽已删**。
 *       注意这次 DEVICE_LOST 是 **PoC-B 的提交**报出来的 (`PoC-B 失败: … code=-4`),
 *       2c-β 自己那次提交是成功的 ⇒ 判读必须抓 `PoC-B 失败 … code=-4` 这一行。
 *     · 改法: 槽表只剩 `D3D11句柄` 一个参数 (`NSlot=1` ⇒ `g_ssrVkRotate=false`), **图常驻**,
 *       每次交叉校验机会都出一行对比 (样本翻倍), 用 ≥4 次 `一致✓` 确认后 2c 收口。
 *   **v0.17.0 = 拆出 vk renderer 模块 (方向 A 第一步, docs/00 §1.1.1) — 纯代码搬迁, 行为不变**:
 *     · 新建 src/poc-presenter/vkrenderer.h + vkrenderer.cpp; build.yml 改成同编两个 .cpp,
 *       链成同一个 poc-presenter.dll (本机仍无编译器, 继续靠 CI 迭代)。
 *     · 搬走的: PoC-B 宏/函数表/结构 (POCB_* + PocbFns + PocbCtx + g_pocb) 与全部实现
 *       (pocbCode/pocbFail/pocbEnabled/pocbInit/pocbInject), 以及 2c-β 的 g_ssrVk* 状态、
 *       候选槽表 + ssrInVkBuild/ssrVkFree/ssrInVkFrame。main.cpp 只留 Present/vtable 钩面 +
 *       D3D11 interop (共享镜像/句柄/哈希) + 日志/ini: 5576 行 → 3854 行。
 *     · main.cpp 的匿名 namespace 改具名 `pocmain` (文件末尾 `using namespace pocmain;` 后
 *       DllMain/SKSEPlugin_Load 照常可见); renderer 需要的 10 个函数 + 9 个全局经 vkrenderer.h
 *       的 `namespace pocmain` 声明互通 —— **这是过渡态**: 2d 收敛成"入参结构体"后整段删掉。
 *     · 方向: 2d 的出向回写 / descriptor+sampler / 深度改道 (VK 自建 VK_FORMAT_D24_SFLOAT 图)
 *       全部写进 vkrenderer.cpp, 不再往 main.cpp 里堆; C (混合+自建呈现) 仍是降级备选。
 *     · 搬迁即改 linkage: 上述 10 个函数 + 8 个全局去 static, g_pocb 改为 vkrenderer.cpp 里
 *       的全局定义 + 头文件 extern。**行为不变 ⇒ 必须回归**: 2000+ 帧仍应 6/6 一致、
 *       PoC-B 失败行 0、code=-4 计 0、[异常] 0、PoC-B ≈1ms 不劣化。
 *   **v0.18.0 = Step 2d-1 出向回写 (docs/05 D3) —— 首次把 VK 渲出的内容写回游戏资源**:
 *     · 通路: 第 3 张 SHARED|NTHANDLE 镜像 (desc 照抄 324 ≡ 585) → VK 按 D3D11_TEXTURE_BIT
 *       导入 → **本帧 Present** VK 把入向镜像拷进出向镜像 (vkCmdCopyImage + CPU fence)
 *       → **下一帧特征B** D3D11 CopyResource(出向镜像 → 585), 时机与 2b/2c 同位 (段17 的
 *       Draw ev39530 还没发生)。全部新代码写进 vkrenderer.cpp (ssrOutVkBuild/ssrOutVkFrame)。
 *     · **1 帧延迟换零跨 API 栅栏**: 不在 D3D11 钩子里等 VK GPU, 也不在 VK 里等 D3D11 GPU
 *       (D3D11 没有 VK 能等的 fence) —— 只靠"上一帧已填、本帧才读" + CPU 的 vkWaitForFences。
 *       代价 = 反射内容比画面晚 1 帧 (16ms), v0 passthrough 下不可见。
 *     · 逃生门 = ini ssr.vkout (默认 0); 开着时 **2b 哨兵让位** ⇒ 585 内容只可能来自 VK。
 *     · 自校验 = 节流读回 (前3次+每600次) 比"入向(VK 消费的那帧)"与"出向(VK 写回的)":
 *       v0 passthrough 下两者应逐字节相等 = 整条 出向通路成立。
 *     · 隔离: 出向建图/导入/提交任一步失败只打 [2d] 日志 + 关自己, **不连坐入向 2c 与 PoC-B**;
 *       失败时 g_ssrOutReady=false ⇒ D3D11 侧不回写 ⇒ 画面保持游戏原样 (最稳的降级)。
 *     · 顺带: POCB_DEV_FNS 扩 descriptor/sampler 8 个函数 (全部 core 1.0, 供 SSR v1 采样用)。
 *   关键日志 (新增):
 *     SSR侦察: ctx vtable=… slot33 … ; slot50 … ; slot47(CopyResource) 原值=… 来自 …
 *     SSR侦察: ctx槽33/50/47 已挂 — 2a 只记日志, 2b 回写需 ssr.sentinel=1 (v0.16.0 Step2 …)
 *     SSR侦察: ini ssr.sentinel=1 → 特征B 处 324→585 回写 (2b 哨兵 …)
 *     SSR侦察: [2a] 场景色快照 324 = 0x… (src=0x… 1920x1080 RGBA16F) 活引用已持 帧=F
 *     SSR侦察: [2a] 深度快照 520(假设) = 0x… (src=0x… D24家族) 本帧首条深度拷贝 帧=F
 *     SSR侦察: 槽47Copy#K 帧=F dst=0x… … <- src=0x… …          ← 前24条 + 每64条
 *     SSR侦察: 特征B(换绑)#K 帧=F 新RT0=0x… … [2b哨兵已排队]
 *     SSR侦察: [2b] 哨兵#K 585=0x… <- 324=0x… 帧=F
 *     SSR侦察: ini ssr.shared=1 → 2c 共享入向: 324/520 拷进 SHARED|NTHANDLE 镜像 …
 *     SSR侦察: [2c] 色324 SHARED 镜像 OK = 0x… (1920x1080 RGBA16F) handle=0x… 源=0x…
 *     SSR侦察: [2c] 深度520 SHARED 镜像 CreateTexture2D 失败 0x… ← R2 风险点
 *     SSR侦察: [2c] 共享入向就绪 — 色324=OK 深度520=OK; 基线校验和(建好未拷) 色=0x… 深=0x…
 *     SSR侦察: ini ssr.vkout=1 → 2d 出向回写: VK(读324镜像)→出向镜像→ 下帧特征B …
 *     SSR侦察: [2d] 出向镜像 OK = 0x… (1920x1080 RGBA16F BindFlags=0x…) handle=0x… 源=0x…
 *     SSR侦察: [2d] 出向图就绪: 1920x1080 RGBA16F TRANSFER_DST → 导入 handle=0x… 命令已录
 *     SSR侦察: [2d] VK出向#K 入向→出向 1920x1080 RGBA16F fence=0.0ms 帧=F [下帧特征B 回写585]
 *     SSR侦察: [2d] 回写#K 585=0x… ← 出向=0x… 帧=F [desc一致]          ← 前8条 + 每128条
 *     SSR侦察: [2d] 出向读回#K 入向=0x… 出向=0x… **一致✓** …            ← 前3条 + 每600条
 *     SSR侦察: [2c] 入向拷贝#K 色324=0x…→0x… 深520=0x…→0x… [2c读回 色=0x…≠基线✓ …] 帧=F
 *     SSR侦察: 特征B(换绑)#K … [2b哨兵已排队] [2c入向已排队]
 *     SSR侦察: [2c] 重试 深度520 BindFlags=0x… (原 0x…) → OK/仍失败 … ← R2 归因
 *     SSR侦察: [2c-β]   导入矩阵成功: 第K/N 组合 size=… (VK报=… 朴素=…) type=… → 病因=… ← v0.16.1
 *     SSR侦察: [2c-β]   导入矩阵 N 次全失败 码=… (size候选x类型) VK报size=… 朴素size=…
 *     SSR侦察: [2c-β] 归因探测A RGBA8@1920x1080(只换格式) 导入 OK/失败 … VK报size=…
 *     SSR侦察: [2c-β] 归因探测B RGBA16F@512x512(只换尺寸) 导入 OK/失败 … VK报size=…
 *     SSR侦察: [2c-β]   归因结论: 病因是格式/尺寸/组合 …   ← 2c-β 改道依据
 *     帧时基线 CPU: 均值 16.62 中位 16.61 p95 17.05 最大 42.10 ms >20ms=3/600 | GPU: 均值 …
 *         (样本 N) | 会话 CPU n=… 均值 … 最大 … / GPU n=… … | 丢弃>=1s …   ← 每 600 帧一行
 *     帧时基线: GPU 时间戳查询就绪 (TIMESTAMP+DISJOINT) … / 创建失败 … -> 本局只记 CPU 侧
 *     SSR侦察: [2c-β] VK 导入 OK: 色镜像 NT handle → VkImage (OPAQUE_WIN32) 1920x1080 RGBA16F …
 *     SSR侦察: [2c-β] 交叉校验#K D3D11=0x… VK=0x… 一致则 **一致✓**、否则 **不一致✗** VK非零=…
 *         行距=<D3D11>/<VK> 同刻D3D11=0x…(=VK✓时序差/≠VK/≠帧内) 前帧D3D11=0x…(=VK✓差一帧)
 *         本帧VK读回=…ms 帧=F   ← v0.16.3 归因字段 + v0.16.5 的 "槽K(名字)" 标识
 *     SSR侦察: [2c-β] 槽1(LINEAR) 建图+导入OK: 1920x1080 → 本帧只建不比 … 下次交叉校验出对比值
 *     SSR侦察: [异常] [2c-β] 槽N(名字) 读回提交 DEVICE_LOST (-4) → 2c-β 停用 (PoC-B 可能已被连坐)
 *
 * 构建: GitHub Actions (build.yml job "poc-presenter"), 本地不编译
 * 安装: poc-presenter.dll 放入 <游戏>/Data/SKSE/Plugins/
 */

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

// 与 capture-helper 共用同一份最小 ABI 声明 (单源, 避免漂移)
#include "..\capture-helper\skse_abi.h"

// PoC-B: Vulkan 注入用 (本机/CI 都没有 Vulkan SDK → 不链 vulkan-1.lib, 只要头文件 +
// 运行时 GetProcAddress 自己装; 头由 build.yml 额外 checkout KhronosGroup/Vulkan-Headers 提供)
// VK_USE_PLATFORM_WIN32_KHR: 共享纹理导入 (VkImportMemoryWin32HandleInfoKHR) 需要
// vulkan_win32.h; windows.h 已在上面包含, 顺序没问题。
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
// SPIR-V 字节 (由 tools\make_shaders.ps1 从 shaders\pocb.vert/.frag 生成, 随源码入库)
#include "pocb_shaders.h"
#include "vkrenderer.h" // 方向A: 自写 VK 渲染器模块 (PoC-B + 2c-β 都在里面, 2d 也写进去)

namespace pocmain { // v0.17.0: 原匿名 namespace, 改具名以便 vkrenderer.cpp 引用

HMODULE g_hModule = nullptr;
std::string g_logPath;

// ---------- 日志 (与 capture-helper 同范式) ----------

std::string timestamp()
{
	std::time_t t = std::time(nullptr);
	std::tm tmv{};
	localtime_s(&tmv, &t);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
	return buf;
}

// C-1 (待修复事项总结 C-1): 原实现每行 open+分配缓冲+write+close+析构 —— 每行一次文件开关,
// 既贵又污染"帧时基线"基线 (P0-6 的测量源)。改成常驻 FILE* + 64KB 缓冲 + 每行一次
// fflush (1 次 write syscall)。由 DllMain 的 DLL_PROCESS_DETACH 关闭。
static FILE* g_logFp = nullptr;

void logLine(const std::string& msg)
{
	const std::string line = "[" + timestamp() + "] " + msg + "\r\n";
	OutputDebugStringA(line.c_str());
	if (!g_logPath.empty())
	{
		if (!g_logFp)
		{
			g_logFp = fopen(g_logPath.c_str(), "ab");
			if (g_logFp)
				setvbuf(g_logFp, nullptr, _IOFBF, 64 * 1024);
		}
		if (g_logFp)
		{
			fwrite(line.data(), 1, line.size(), g_logFp);
			fflush(g_logFp); // 每行必须落盘: 判读/崩溃前最后几行不能丢
		}
	}
}

void logLineFlush() // DllMain detach 用
{
	if (g_logFp)
	{
		fflush(g_logFp);
		fclose(g_logFp);
		g_logFp = nullptr;
	}
}

std::string dirOf(const std::string& p)
{
	const auto pos = p.find_last_of("\\/");
	return pos == std::string::npos ? p : p.substr(0, pos);
}

std::string moduleFilePath()
{
	char buf[MAX_PATH]{};
	GetModuleFileNameA(g_hModule, buf, MAX_PATH);
	return buf;
}

std::string pluginDir()  // ...\Data\SKSE\Plugins
{
	return dirOf(moduleFilePath());
}

std::string hexOf(const void* p)
{
	char b[32];
	std::snprintf(b, sizeof(b), "%p", p);
	return b;
}

std::string hexHr(long hr)
{
	char b[16];
	std::snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(hr));
	return b;
}

// 地址 → 所属模块路径 (判 vtable/原函数来自 dxgi 还是 RenderDoc 包装)
std::string modulePathOf(const void* addr)
{
	HMODULE h = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCWSTR>(addr), &h) || !h)
		return "?";
	wchar_t wbuf[MAX_PATH]{};
	GetModuleFileNameW(h, wbuf, MAX_PATH);
	char buf[MAX_PATH]{};
	WideCharToMultiByte(CP_ACP, 0, wbuf, -1, buf, MAX_PATH, nullptr, nullptr);
	return buf;
}

std::string lowerCopy(std::string s)
{
	for (auto& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// 安全阀: 原值必须落在 dxgi.dll 或 renderdoc.dll 内才允许挂 (防错槽位)
bool isDxgiFamily(const void* p)
{
	const std::string m = lowerCopy(modulePathOf(p));
	return m.find("dxgi.dll") != std::string::npos || m.find("renderdoc.dll") != std::string::npos;
}

// ---------- 函数签名与前置声明 ----------

using Present_t = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain* sc, UINT sync, UINT flags);
using Present1_t = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                                const DXGI_PRESENT_PARAMETERS* params);
using CreateSwapChain_t = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory* self, IUnknown* dev,
                                                       const DXGI_SWAP_CHAIN_DESC* desc,
                                                       IDXGISwapChain** out);
using CreateSwapChainForHwnd_t = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2* self, IUnknown* dev, HWND hwnd,
                                                              const DXGI_SWAP_CHAIN_DESC1* desc,
                                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                              IDXGIOutput* restrictOut, IDXGISwapChain1** out);
using CreateSwapChainForCoreWindow_t = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2* self, IUnknown* dev,
                                                                    IUnknown* window,
                                                                    const DXGI_SWAP_CHAIN_DESC1* desc,
                                                                    IDXGIOutput* restrictOut,
                                                                    IDXGISwapChain1** out);
using CreateSwapChainForComposition_t = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2* self, IUnknown* dev,
                                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                                     IDXGISwapChain1** out);

HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags);
HRESULT STDMETHODCALLTYPE hookedPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* params);
HRESULT STDMETHODCALLTYPE hookedCreateSwapChain(IDXGIFactory* self, IUnknown* dev,
                                                 const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out);
HRESULT STDMETHODCALLTYPE hookedCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* desc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                       IDXGIOutput* restrictOut, IDXGISwapChain1** out);
HRESULT STDMETHODCALLTYPE hookedCreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* dev, IUnknown* window,
                                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                                             IDXGIOutput* restrictOut, IDXGISwapChain1** out);
HRESULT STDMETHODCALLTYPE hookedCreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* dev,
                                                              const DXGI_SWAP_CHAIN_DESC1* desc,
                                                              IDXGISwapChain1** out);

void patchSlotLocked(void** slot, void* target);
void registerSwp(void* obj, bool plus, const char* tag);
void registerFac(void* obj, bool plus2, const char* tag);
void pocbFrame(IDXGISwapChain* sc); // PoC-B: 在 Present 里跑 Vulkan 并注入像素 (v0.9.0)

// ---------- 登记表 (按调用方 vtable 查原函数) ----------

struct SwpEntry
{
	void** vtbl;
	void*  orig8;   // Present  (槽8)
	void*  orig22;  // Present1 (槽22, 仅经 1+ 口径见过的 vtable 才有)
	bool   has22;
};

struct FacEntry
{
	void** vtbl;
	void*  o10;  // CreateSwapChain (槽10)
	void*  o15;  // CreateSwapChainForHwnd (槽15)
	void*  o16;  // CreateSwapChainForCoreWindow (槽16)
	void*  o24;  // CreateSwapChainForComposition (槽24)
};

SwpEntry g_swp[8];
int      g_swpN = 0;
FacEntry g_fac[4];
int      g_facN = 0;

CRITICAL_SECTION g_cs;  // 可重入, 在 SKSEPlugin_Load 中初始化

std::atomic<uint64_t> g_presentCount{0};
std::chrono::steady_clock::time_point g_lastLog{};
uint64_t g_lastLogCount = 0;
std::atomic<void*> g_gameSc{nullptr}; // ★ 拦到的游戏交换链对象 (对象级探针用)

void* lookupPresentOrig8(void** vtbl)
{
	for (int i = 0; i < g_swpN; ++i)
		if (g_swp[i].vtbl == vtbl)
			return g_swp[i].orig8;
	return nullptr;
}

void* lookupPresentOrig22(void** vtbl)
{
	for (int i = 0; i < g_swpN; ++i)
		if (g_swp[i].vtbl == vtbl && g_swp[i].has22)
			return g_swp[i].orig22;
	return nullptr;
}

void* lookupFacOrig(void** vtbl, int slot)
{
	for (int i = 0; i < g_facN; ++i)
	{
		if (g_fac[i].vtbl != vtbl)
			continue;
		switch (slot)
		{
		case 10: return g_fac[i].o10;
		case 15: return g_fac[i].o15;
		case 16: return g_fac[i].o16;
		case 24: return g_fac[i].o24;
		default: return nullptr;
		}
	}
	return nullptr;
}

// ---------- 槽位改写 (调用前须持有 g_cs) ----------

void patchSlotLocked(void** slot, void* target)
{
	if (*slot == target)
		return;
	DWORD oldProt = 0;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
	{
		logLine("VirtualProtect 失败 err=" + std::to_string(GetLastError()));
		return;
	}
	InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), target);
	DWORD tmp = 0;
	VirtualProtect(slot, sizeof(void*), oldProt, &tmp);
}

// ---------- 交换链 vtable 登记 + 挂 Present / Present1 ----------

// v0.9.2 实验开关 —— 同一份 <pluginDir>\poc-presenter.ini (与 pocbEnabled 共用文件):
//   vulkan=0 → 关 PoC-B 注入 (见 pocbEnabled)
//   ssr=1    → v0.13.0/v0.14.0 SSR Step1+2a: 挂 ctx 槽33/47/50 观察记日志 (默认 0)
//   ssr.sentinel=1 → v0.14.0 Step2b 通路哨兵: 特征B 处 CopyResource(585<-324) 回写
//              (默认 0; **首次会动渲染**, 与 ssr 分开独立逃生门, ssr=0 时它一并失效)
//   vtable=0 → 停用 vtable 层: 不挂交换链槽 8/22, 看门狗也不打回 ⇒ 游戏 Present 只能
//              从方案B 的函数级 detour 进来 —— 用于给方案B 做"独立计数"
//              (docs/01 §7.7 末遗留项: vtable 层恒先命中, TLS 去重吃掉函数层计数)
static bool g_vtableLayer = true;

bool iniFlag(const char* key, bool def)
{
	std::ifstream f(pluginDir() + "\\poc-presenter.ini");
	std::string line;
	bool out = def;
	while (std::getline(f, line))
	{
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = lowerCopy(line.substr(0, eq));
		while (!k.empty() && std::isspace(static_cast<unsigned char>(k.back())))
			k.pop_back(); // 允许 "vtable = 0" 写法
		if (k != key)
			continue;
		for (const char ch : line.substr(eq + 1))
		{
			if (std::isspace(static_cast<unsigned char>(ch)))
				continue;
			out = !(ch == '0' || ch == 'f' || ch == 'F' || ch == 'n' || ch == 'N');
			break;
		}
	}
	return out;
}

// v0.18.6: 数值型 ini (ssr.fov / ssr.near / ssr.far / ssr.steps / ssr.strength / ssr.dist /
// ssr.mode) —— 与 iniFlag 同一份文件、同一套解析口径 (键小写比较, 允许 "key = value"、
// 行尾 "#..." 注释)。ini 启动只读一次 (调用方在 probeIniRead 一次性块里)。
double iniNum(const char* key, double def)
{
	std::ifstream f(pluginDir() + "\\poc-presenter.ini");
	std::string line;
	double out = def;
	while (std::getline(f, line))
	{
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = lowerCopy(line.substr(0, eq));
		while (!k.empty() && std::isspace(static_cast<unsigned char>(k.back())))
			k.pop_back();
		if (k != key)
			continue;
		std::string v = line.substr(eq + 1);
		const size_t cm = v.find_first_of("#;");
		if (cm != std::string::npos)
			v.erase(cm);
		const size_t b0 = v.find_first_not_of(" \t\r");
		if (b0 == std::string::npos)
			continue;
		v.erase(0, b0);
		const size_t e0 = v.find_last_not_of(" \t\r");
		v.erase(e0 + 1);
		out = std::atof(v.c_str());
	}
	return out;
}

// ---------- 探针升质 hook (v0.11.0 + v0.12.0 plan-B): CreateTexture2D 槽5 + context 槽44/45 ----------
// 水体样本第一步 (docs/00 §1.1 / docs/03 §6.1): 501 = 6面512² RGBA16F 环境反射 cubemap
// 探针, 命中描述符时把宽高翻倍 (512²→1024²)。v0.12 再补: 绑定感知拦 RSSetViewports/
// RSSetScissorRects (plan-B, 见下 installCtxProbe)。
// v0.11 (docs/02 §14 判读定案) 两处修正, 缺一不可:
//   ① 配对 depth 同步升质 —— v0.10.0 只翻 cube → OM = RT1024²+DSV512² 尺寸不匹配 →
//      绑定判非法作废 (回放 8 条 High 诊断, 真机 draw 同样未落地, 反射平铺清屏色)。
//   ② 升质宽高回写游戏自己那份 desc —— 旧注释「viewport/RTV 从 GetDesc 自动适配」
//      实测不成立 (v0.10.0 局 probe 段 RSSetViewports 恒 512² 字面量 = 游戏用创建时
//      缓存值)。RTV/DSV/SRV 的 desc 结构不带宽高 (随纹理对象派生), 只有游戏侧 viewport/
//      scissors 这类缓存值依赖 desc 宽高 → 回写精准命中要修的东西。
//      v0.12 勘误 (S4e 判据②): 回写两行全 ok 但 viewport 仍 512² —— 游戏 viewport 来源
//      不在被回写的那份 desc → 转 plan-B: 绑定感知拦槽44/45 (见 installCtxProbe)。
// 挂法与交换链同款: vtable 槽改写, 原值必须落在 d3d11.dll / renderdoc.dll 才挂 (防错槽位)。
//   probe=0 (ini) → 完全不挂槽; vulkan=0 → 同样不挂 (S4c 基线口径, 由 pocbEnabled 把关)。

struct DevEntry
{
	void** vtbl;
	void*  orig5; // CreateTexture2D (ID3D11Device vtable 槽5)
};
static DevEntry g_dev[4];
static int      g_devN = 0;
static std::atomic<bool> g_probeOn{false};
static std::atomic<long> g_probeLogN{0};
static std::atomic<bool> g_probeDepthArmed{false}; // v0.11: cube 命中后开窗, 等配对 depth
static std::atomic<long> g_probeDepthN{0};
static std::atomic<unsigned long long> g_probeArmTick{0}; // v0.11: 开窗时刻, 10s 过窗即收
// v0.12.0 plan-B (S4e 验证判据② 未达成, docs/02 §14.7): pokeDescSize 实测改不到游戏
// viewport 来源 (probe 段 RSSetViewports 恒 512², 全帧无 1024² 字面量) → 改在 context
// vtable 层拦: 当前 OM RT0 解析回 probe cube 且视口/裁剪恰 512² 时翻 1024² 再下传。
// 绑定感知 (OMGetRenderTargets→GetResource 对象身份) 而非无条件 512² 全拦 —— 防误伤其他
// 512² pass。槽号双证 (官方 d3d11.h MIDL 声明序: IUnknown 0-2 + DeviceChild 3-6 + 接口
// 偏移 7+37/7+38; 与 xosh vtable 表 44/45 吻合)。
struct CtxEntry
{
	void** vtbl;
	void*  orig44; // RSSetViewports   (ID3D11DeviceContext vtable 槽44)
	void*  orig45; // RSSetScissorRects (槽45)
};
static CtxEntry g_ctx[4];
static int      g_ctxN = 0;
static std::atomic<ID3D11Texture2D*> g_probeCube{nullptr}; // cube 升质成功后记住对象 (绑定判据)
static unsigned long long g_probeCubeT = 0; // v0.18.16 P1: 上面那张出现的时刻 (兜底计时, 见 probeDumpTick)
static unsigned long long g_probeCubeRenderT = 0; // v0.18.16c: 游戏第一次往探针里画的时刻 —— 读回真正的起算点
static bool g_probeRenderWaitLg = false; // v0.18.16c: 60s 等首渲兜底日志只打一次
static std::atomic<long> g_vpRewriteN{0};
static std::atomic<long> g_scRewriteN{0};

void* lookupDevTex2D(void** vtbl)
{
	if (!vtbl)
		return nullptr;
	for (int i = 0; i < g_devN; ++i)
		if (g_dev[i].vtbl == vtbl)
			return g_dev[i].orig5;
	return nullptr;
}

// 安全阀: 原值必须落在 d3d11.dll 或 renderdoc.dll 内才允许挂 (与 isDxgiFamily 同思路)
bool isD3D11Family(const void* p)
{
	const std::string m = lowerCopy(modulePathOf(p));
	return m.find("d3d11.dll") != std::string::npos || m.find("renderdoc.dll") != std::string::npos;
}

// 501 探针的创建描述符 (RenderDoc 实测: 512² mips=1 arraysize=6 RGBA16F
// TextureCubeArray, Usage ColorTarget|ShaderRead)
static bool probeDesc(const D3D11_TEXTURE2D_DESC* d)
{
	return d->Width == 512 && d->Height == 512 && d->MipLevels == 1 && d->ArraySize == 6 &&
	       d->Format == DXGI_FORMAT_R16G16B16A16_FLOAT && d->SampleDesc.Count == 1 &&
	       d->Usage == D3D11_USAGE_DEFAULT &&
	       (d->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0 &&
	       (d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) ==
	           (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
}

// v0.11 ①: 探针配对 depth (docs/02 §14.4) —— rdc_tex_desc 实测: 512² D24 家族
// (RenderDoc 显示名 "D24S8_TYPELESS" = DXGI 枚举 D24 家族, 枚举名里没有 D24S8_TYPELESS,
// 列全 4 个合法成员: R24G8_TYPELESS(游戏大概率用这个) / D24_UNORM_S8_UINT /
// R24_UNORM_X8_TYPELESS / X24_TYPELESS_G8_UINT) 纯 2D (arraySize=1, mips=1, DSV bind),
// 全帧唯一 512² 深度; cube(544) 早于 depth(552) 创建 → cube 命中后开窗
// (g_probeDepthArmed) 等它, 收窗后其余 512² depth 一概不碰
// (别的 pass 若 RT 还是 512² 而 depth 被升 = 复刻同一个 bug, 绝不能无条件升)。
static bool probeDepthDesc(const D3D11_TEXTURE2D_DESC* d)
{
	return d->Width == 512 && d->Height == 512 && d->MipLevels == 1 && d->ArraySize == 1 &&
	       (d->Format == DXGI_FORMAT_R24G8_TYPELESS || d->Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
	        d->Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
	        d->Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT) &&
	       d->SampleDesc.Count == 1 && d->Usage == D3D11_USAGE_DEFAULT &&
	       (d->BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
}

// v0.11 ②: 把升质宽高回写游戏自己那份 desc (docs/02 §14.5)。const 是 API 约定, 游戏栈上
// 的变量可写; 但写前 VirtualQuery 查页保护 —— 万一 desc 落在 .rdata 只读常量页, 直接写会
// AV 崩游戏, 所以只读页放弃 + 告警 (降级为 v0.10.0 行为, 由后续抓帧 viewport 判读察觉)。
static bool pokeDescSize(const D3D11_TEXTURE2D_DESC* desc, UINT w, UINT h)
{
	if (!desc)
		return false;
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(desc, &mbi, sizeof(mbi)) == 0)
		return false;
	const DWORD p = mbi.Protect & 0xff;
	if (!(p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE ||
	      p == PAGE_EXECUTE_WRITECOPY))
	{
		logLine("探针升质: 游戏描述符页只读 (Protect=" + std::to_string(mbi.Protect) +
		        ") — 放弃回写, viewport 可能仍 512² (docs/02 §14.5 plan-B)");
		return false;
	}
	D3D11_TEXTURE2D_DESC* g = const_cast<D3D11_TEXTURE2D_DESC*>(desc);
	g->Width = w;
	g->Height = h;
	return true;
}

HRESULT STDMETHODCALLTYPE hookedCreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* desc,
                                                const D3D11_SUBRESOURCE_DATA* init,
                                                ID3D11Texture2D** out)
{
	using CT2D_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
	                                           const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
	const CT2D_t real = reinterpret_cast<CT2D_t>(
	    lookupDevTex2D(dev ? *reinterpret_cast<void***>(dev) : nullptr));
	if (!real) // 理论不可达: 只有被我们改过的槽才会进本函数
		return E_INVALIDARG;
	if (g_probeOn.load(std::memory_order_relaxed) && desc)
	{
		// ---- 探针 cube 升质 (v0.10.0; v0.11 补回写 + 开窗) ----
		if (probeDesc(desc))
		{
			if (init) // 带初始数据的不换 (探针是 RT 不该有, 有的话宁可不升)
			{
				logLine("探针升质: cube 描述符命中但带初始数据 — 不升 (保持 512²)");
				return real(dev, desc, init, out);
			}
			D3D11_TEXTURE2D_DESC d = *desc;
			d.Width *= 2;
			d.Height *= 2;
			const HRESULT hr = real(dev, &d, init, out);
			const long n = g_probeLogN.fetch_add(1, std::memory_order_relaxed) + 1;
			bool poked = false;
			if (SUCCEEDED(hr))
			{
				// v0.11 ②: 游戏缓存的 desc 宽高跟着变 (viewport/scissors 等派生值)
				poked = pokeDescSize(desc, d.Width, d.Height);
				// v0.11 ①: 开窗等配对 depth (cube 544 先于 depth 552 创建, 同一突发)
				g_probeArmTick.store(GetTickCount64(), std::memory_order_relaxed);
				g_probeDepthArmed.store(true, std::memory_order_release);
				// v0.12 plan-B: 记住升质后的 cube 对象 —— viewport/scissor 拦截的绑定判据
				if (out && *out)
				{
					g_probeCube.store(*out, std::memory_order_release);
					g_probeCubeT = GetTickCount64(); // v0.18.16 P1: 探针出现时刻 (只作 60s 兜底计时)
					// v0.18.16c: 首渲信号跟着新 cube 一起清零 —— 读回要等**这张**被画过
					g_probeCubeRenderT = 0;
					g_probeRenderWaitLg = false;
				}
			}
			if (n <= 16 || (n % 64) == 0)
				logLine("探针升质: 512² RGBA16F cube(6面) → " + std::to_string(d.Width) + "² 第 " +
				        std::to_string(n) + " 次, hr=" + std::to_string(hr) +
				        ", 回写desc=" + (poked ? "ok" : "no"));
			return hr;
		}
		// ---- 配对 depth 升质 (v0.11 ①): 仅 cube 命中后 10s 窗内、一次性 ----
		if (g_probeDepthArmed.load(std::memory_order_acquire) && probeDepthDesc(desc))
		{
			// 窗口依据: cube→depth 同一初始化突发 (ResourceId 544<552, 毫秒级)。过窗即收,
			// 会话后期出现的别的 512² D24 一概不碰 —— 误升会复刻 RT/depth 尺寸不匹配。
			if (GetTickCount64() - g_probeArmTick.load(std::memory_order_relaxed) > 10000)
			{
				g_probeDepthArmed.store(false, std::memory_order_release);
				logLine("探针配对depth: 开窗 10s 已过仍未见 depth — 收窗 (配对失败? 需查)");
				return real(dev, desc, init, out);
			}
			g_probeDepthArmed.store(false, std::memory_order_release); // 一次性收窗
			if (init)
			{
				logLine("探针配对depth: 描述符命中但带初始数据 — 不升 (升了会越界读), 配对失败告警");
				return real(dev, desc, init, out);
			}
			D3D11_TEXTURE2D_DESC d = *desc;
			d.Width *= 2;
			d.Height *= 2;
			const HRESULT hr = real(dev, &d, init, out);
			const long n = g_probeDepthN.fetch_add(1, std::memory_order_relaxed) + 1;
			bool poked = false;
			if (SUCCEEDED(hr))
				poked = pokeDescSize(desc, d.Width, d.Height);
			logLine("探针配对depth: 512² D24 → " + std::to_string(d.Width) + "² 第 " +
			        std::to_string(n) + " 次, hr=" + std::to_string(hr) +
			        ", 回写desc=" + (poked ? "ok" : "no"));
			return hr;
		}
	}
	return real(dev, desc, init, out);
}

static CtxEntry* lookupCtx(void** vtbl)
{
	if (!vtbl)
		return nullptr;
	for (int i = 0; i < g_ctxN; ++i)
		if (g_ctx[i].vtbl == vtbl)
			return &g_ctx[i];
	return nullptr;
}

// v0.12 plan-B 绑定感知: 当前 OM RT0 是否即探针 cube (拿真实绑定 —— OMGetRenderTargets
// 走真槽89, 未被我们挂 (我们挂的是 33/44/45/50), 不递归)
static bool omBoundToProbeCube(ID3D11DeviceContext* ctx)
{
	ID3D11Texture2D* cube = g_probeCube.load(std::memory_order_acquire);
	if (!ctx || !cube)
		return false;
	ID3D11RenderTargetView* rtv = nullptr;
	ctx->OMGetRenderTargets(1, &rtv, nullptr);
	if (!rtv)
		return false;
	ID3D11Resource* res = nullptr;
	rtv->GetResource(&res);
	// RTV 由该游戏纹理创建 → GetResource 回同一对象身份 (renderdoc wrapper 亦保持)
	const bool hit = (res == static_cast<ID3D11Resource*>(cube));
	if (res)
		res->Release();
	rtv->Release();
	return hit;
}

// v0.12 plan-B: probe 段视口 512²→1024² —— 只在 probe cube 绑定中改写, 其余原样下传
static void STDMETHODCALLTYPE hookedRSSetViewports(ID3D11DeviceContext* ctx, UINT n,
                                                   const D3D11_VIEWPORT* vp)
{
	using Fn_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
	CtxEntry* e = lookupCtx(ctx ? *reinterpret_cast<void***>(ctx) : nullptr);
	const Fn_t real = e ? reinterpret_cast<Fn_t>(e->orig44) : nullptr;
	if (!real) // 理论不可达: 只有被我们改过的槽才会进本函数
		return;
	if (n == 1 && vp && vp[0].Width == 512.0f && vp[0].Height == 512.0f &&
	    g_probeCube.load(std::memory_order_relaxed) && omBoundToProbeCube(ctx))
	{
		D3D11_VIEWPORT v2 = vp[0];
		v2.Width = 1024.0f;
		v2.Height = 1024.0f;
		const long c = g_vpRewriteN.fetch_add(1, std::memory_order_relaxed) + 1;
		// v0.18.16c: 这里 = 游戏**正在往探针里画** ⇒ 记首渲时刻, probeDumpTick 的读回闸挂它
		// (挂 g_probeCubeT 建纹理时刻的老闸 12/12 会话都落在首渲之前 4~24s, 读回全是裸 0)
		if (g_probeCubeRenderT == 0)
			g_probeCubeRenderT = GetTickCount64();
		if (c == 1 || (c % 256) == 0)
			logLine("探针升质: RSSetViewports 512²→1024² (probe cube 绑定中) 第 " +
			        std::to_string(c) + " 次");
		real(ctx, 1, &v2);
		return;
	}
	real(ctx, n, vp);
}

// v0.12 plan-B: 同款拦裁剪矩形 (若游戏开 scissor 测, viewport 翻了不翻裁剪会照旧只渲 1/4)
static void STDMETHODCALLTYPE hookedRSSetScissorRects(ID3D11DeviceContext* ctx, UINT n,
                                                      const D3D11_RECT* rc)
{
	using Fn_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
	CtxEntry* e = lookupCtx(ctx ? *reinterpret_cast<void***>(ctx) : nullptr);
	const Fn_t real = e ? reinterpret_cast<Fn_t>(e->orig45) : nullptr;
	if (!real)
		return;
	if (n == 1 && rc && (rc[0].right - rc[0].left) == 512 && (rc[0].bottom - rc[0].top) == 512 &&
	    g_probeCube.load(std::memory_order_relaxed) && omBoundToProbeCube(ctx))
	{
		D3D11_RECT r2 = rc[0];
		r2.right = r2.left + 1024;
		r2.bottom = r2.top + 1024;
		const long c = g_scRewriteN.fetch_add(1, std::memory_order_relaxed) + 1;
		if (c == 1 || (c % 256) == 0)
			logLine("探针升质: RSSetScissorRects 512²→1024² (probe cube 绑定中) 第 " +
			        std::to_string(c) + " 次");
		real(ctx, 1, &r2);
		return;
	}
	real(ctx, n, rc);
}

// v0.12 plan-B: 挂 immediate context vtable 槽44/45 —— 幂等 (按 vtable 去重), 与槽5 同款
// 安全阀 (原值须在 d3d11.dll/renderdoc.dll)。GetImmediateContext 是类型化调用, 不依赖槽号。
static void installCtxProbe(ID3D11Device* dev)
{
	if (!dev || !g_probeOn.load(std::memory_order_relaxed))
		return;
	ID3D11DeviceContext* ctx = nullptr;
	dev->GetImmediateContext(&ctx);
	if (!ctx)
	{
		logLine("探针升质: GetImmediateContext 空 — plan-B viewport 拦截未挂 (判据② 仍会失败)");
		return;
	}
	void** vtbl = *reinterpret_cast<void***>(ctx);
	EnterCriticalSection(&g_cs);
	if (lookupCtx(vtbl))
	{
		LeaveCriticalSection(&g_cs);
		ctx->Release();
		return;
	}
	void* o44 = vtbl[44];
	void* o45 = vtbl[45];
	logLine("探针升质: ctx vtable=" + hexOf(vtbl) + " slot44(RSSetViewports) 原值=" + hexOf(o44) +
	        " 来自 " + modulePathOf(o44) + "; slot45(RSSetScissorRects) 原值=" + hexOf(o45) +
	        " 来自 " + modulePathOf(o45));
	if (!isD3D11Family(o44) || !isD3D11Family(o45))
	{
		logLine("探针升质: ctx slot44/45 原值不在 d3d11/renderdoc — 跳过 (防错槽位, plan-B 失效)");
		LeaveCriticalSection(&g_cs);
		ctx->Release();
		return;
	}
	if (g_ctxN >= (int)(sizeof(g_ctx) / sizeof(g_ctx[0])))
	{
		logLine("探针升质: ctx 登记表已满 — 跳过 (防越界)");
		LeaveCriticalSection(&g_cs);
		ctx->Release();
		return;
	}
	g_ctx[g_ctxN].vtbl = vtbl;
	g_ctx[g_ctxN].orig44 = o44;
	g_ctx[g_ctxN].orig45 = o45;
	++g_ctxN;
	patchSlotLocked(&vtbl[44], reinterpret_cast<void*>(&hookedRSSetViewports));
	patchSlotLocked(&vtbl[45], reinterpret_cast<void*>(&hookedRSSetScissorRects));
	logLine("探针升质: ctx槽44/45 已挂 — probe cube 绑定中 512²→1024² (v0.12 plan-B)");
	LeaveCriticalSection(&g_cs);
	ctx->Release();
}

// ---------- SSR Step 1/2a/2b 侦察+介入钩 (v0.13.0/v0.14.0, docs/05 §5) ----------
// 挂 ctx 槽33 (OMSetRenderTargets) + 槽50 (ClearRenderTargetView) + 槽47 (CopyResource):
//   Step1 (v0.13.0) —— 只读观察: 真机上反射段 (段16: RT0=585 / DS=461) 的绑定特征是否
//     可辨、段16 结束信号 (特征A/B) 哪个稳定; 只解析参数, 不改任何渲染状态。
//   Step2a (v0.14.0) —— 槽47 按"来源"学 324/520 身份 (见下方 Step 2a 全局注释), 仍只读。
//   Step2b (v0.14.0) —— 特征B 处 CopyResource(585<-324) 回写 = 通路哨兵; **首次动渲染**,
//     独立开关 ssr.sentinel (默认 0), ssr=0 则三槽全不挂 (D5, 基线帧与 v0.12.0 同路径)。
// 槽号双证: 官方 d3d11.h MIDL 声明序 (与 xosh 44/45 同口径); 槽34 游戏 api-scan 零命中不挂。
struct SsrCtxEntry
{
	void** vtbl;
	void*  imm;    // 安装时抓到的 immediate context (指针值作身份; 延迟上下文跳过观察)
	void*  orig33; // OMSetRenderTargets    (ID3D11DeviceContext 槽33)
	void*  orig50; // ClearRenderTargetView (槽50, 特征A "用后清")
	void*  orig47; // CopyResource          (槽47, Step 2a 身份识别 + 2b 回写通道)
};
static SsrCtxEntry g_ssrCtx[4];
static int         g_ssrCtxN = 0;
static std::atomic<bool> g_ssrOn{false}; // ini ssr (D5 默认 0)
static bool        g_ssrWatchAll = false; // 指针身份失配后转按 vtbl 观察全部上下文 (见 ssrWatch)
// 本帧状态 (Present 时清零; 观察只发生在 immediate context 上 = 渲染线程)
static std::atomic<long> g_ssrFOm{0}, g_ssrFCand{0}, g_ssrFStr{0}, g_ssrFA{0}, g_ssrFB{0};
static void* g_ssrFObj[4];           // 本帧强特征对象身份 (判 distinct 唯一性)
static int   g_ssrFObjN = 0;
static void* g_ssrLastStr = nullptr; // 最近强特征的 RT0 身份 (特征A/B 判据)
static bool  g_ssrInStrong = false;  // 强特征通道"进行中" —— 特征A 的触发前提; 任一次
                                     // 解绑/换到非强绑定即中断 (见 ssrReconOm 顶部复位)
static bool  g_ssrAFired = false, g_ssrBFired = false; // 每帧各至多一次, 只在 Present 复位
static long  g_ssrStrTot = 0;                          // 累计强特征 (按"进入"计, 非按次绑)
static long  g_ssrCandN = 0;                           // 累计候选
static long  g_ssrCandLogN = 0;                        // 候选详情已打条数 (按对象去重后节流)
static long  g_ssrALogN = 0, g_ssrBLogN = 0;           // A/B 详情日志节流
static void* g_ssrLastCand = nullptr;                  // 候选详情按 (对象,强征与否) 去重
static bool  g_ssrLastCandStr = false;                 //   防 16 次重绑刷屏 (见 ssrReconOm 末)
// 判别子 —— 真机同帧有两个签名完全相同的强特征段 (docs/05 D1 实测推翻"同帧唯一"):
//   段16   反射段    RT0=585 16 Draw → S4 实测全帧只绑 16 次、只在 1 个连续绑定段里出现
//   段19   后期前处理 RT0=321 1 Draw  → S4 实测全帧绑 50 次、散在 4 个连续绑定段里
// 次数只差 3 倍 (16/50, 场景一变可能更近), "1 段 vs 4 段"才是结构性区别:
// 反射目标按定义整帧只写一次 (写完即被段17 消费), 主 HDR 缓冲则处处复用。
// 判别只在"强特征对象集合内"有效 —— 全帧另有 339 = 17次/1段, 不能拿它去全帧海选。
struct SsrRtN { void* obj; long n; long runs; };
static SsrRtN g_ssrRtN[32];
static int    g_ssrRtNN = 0;
static int    g_ssrRtNOver = 0;
static void*  g_ssrPrevRt0 = nullptr; // 上一次绑定的 RT0 (RT0 变化 = 换段, 用于 runs)

// ---- Step 2a (v0.14.0, docs/05 D2a): 真机身份 —— 324/520/585/321 是抓帧 ResourceId,
//      跨帧不可用 (R1), 只能按"来源"认。本组全部**帧间持久**, 帧末不复位:
//   585 反射目标 = 强特征集合里 runs==1 那个 (Step 1 判别子), 上一帧 Present 定;
//   321 主 HDR   = 强特征集合里 runs>1  那个, 同上;
//   324 场景色快照 = CopyResource 的 src 是 1920x1080 RGBA16F 时的 dst
//                  (S4 全帧 3 条 Copy 里唯一 src 为 RGBA16F 的 = ev39225 的 321→324);
//   520 深度快照   = 本帧**首个** src 为 1920x1080 D24 家族的 CopyResource 的 dst
//                  (S4: ev21505 的 461→520; 本帧第二条深度拷贝 461→466 是后期深度,
//                   不是 520 —— 靠"每帧首条"区分, 由 g_ssrCopyFrame 记帧号复位)。
//   资源对象身份统一走 QI ID3D11Texture2D 的指针值 (COM 契约: 同对象同 IID 必返同指针,
//   与 ssrViewDesc 的取法一致 ⇒ OMSet 侧与 CopyResource 侧可直接比对)。
static void* g_ssrReflRt = nullptr;    // 585 (2b 判据: 上一强特征对象 == 它才回写)
static void* g_ssrMainHdr = nullptr;   // 321
static void* g_ssrSceneObj = nullptr;  // 324 的 Texture2D 身份 (日志/换代检测)
static void* g_ssrDepthObj = nullptr;  // 520 的 Texture2D 身份 (2c VK 输入, 本轮只学不使用)
static ID3D11Resource* g_ssrSceneRes = nullptr; // 324 活引用 (ssrReconCopy 持, 2b 的源)
static ID3D11Resource* g_ssrStrRes = nullptr;   // 最近强特征对象的活引用 (2b 的写目标)
static void* g_ssrStrResObj = nullptr;          // 它的 Texture2D 身份
static long  g_ssrCopyN = 0;           // 本帧 CopyResource 次数 (进帧汇总)
static long  g_ssrCopyLogN = 0;        // 详情日志节流
static unsigned long long g_ssrCopyFrame = 0; // "本帧首条深度拷贝" 判据的帧号
static bool  g_ssrSentinelPending = false;    // 特征B 已判 → real() 返回后执行回写
static bool  g_ssrOutPending = false;         // C-6: 2d 出向回写的同款一次性门 (特征B 置位, 消费即清)
static long  g_ssrSentN = 0;           // 哨兵累计执行次数
static bool  g_ssrSelfCopy = false;    // 2b 哨兵自己的 CopyResource 进行中 → 不计数不重复学习
static std::atomic<bool> g_ssrSentinelOn{false}; // ini ssr.sentinel (2b 回写独立逃生门)

// ---- Step 2c (v0.15.0, docs/05 D2a-a + 风险 R2): 共享入向 —— D3D11 侧 ----
// 2 张 SHARED|NTHANDLE 纹理镜像游戏资源: 324 场景色 (1920x1080 RGBA16F) 与 520 深度
// (1920x1080 D24 家族)。触发点与 2b 哨兵同位 (特征B → real() 后), 每次 CopyResource
// 源→镜像各一下; CreateSharedHandle 各取一个 NT handle → 2c-β 由 VK 经
// VK_KHR_external_memory_win32 (OPAQUE_WIN32) 导成 VkImage。
// 验收重点 = R2: **D24 深度与 FP16 这两个 PoC-B 没验过的格式能否建出 SHARED、能否被
// DXGI 允许共享、拷进去后能不能读回** —— 这正是 docs/05 写的"深度共享无先例"。
// 门控: ini ssr.shared (默认 0) 且 ssr=1 是总门; **只往我方镜像写, 不动游戏资源, 不改画面**
// (与 2b 哨兵的"改 585"性质完全不同, 所以单独给一道独立开关, D5 的逃生门思路一致)。
static ID3D11Resource*  g_ssrDepthRes = nullptr; // 520 活引用 (2a 只学身份, 2c 补持一份)
ID3D11Texture2D* g_ssrInTexC = nullptr;   // 324 的 SHARED 镜像 (色)
ID3D11Texture2D* g_ssrInTexD = nullptr;   // 520 的 SHARED 镜像 (深度, 路线1′ 老式 SHARED)
ID3D11Texture2D* g_ssrInStgC = nullptr;   // 色镜像的 STAGING 镜像 (读回自校验用)
static ID3D11Texture2D* g_ssrInStgD = nullptr;   // 深度镜像的 STAGING 镜像
HANDLE g_ssrInHC = nullptr;               // 色镜像 NT handle (2c-β 交 VK 导入)
HANDLE g_ssrInHD = nullptr;               // 深度镜像 handle (NTHANDLE=NT; 路线1′=老式 KMT, 二者都交 VK)
std::atomic<bool> g_ssrSharedOn{false};   // ini ssr.shared
static bool  g_ssrInPending = false;             // 特征B 已判 → real() 后执行入向拷贝
static bool  g_ssrInBuilt = false;               // 两张镜像 + 暂存已建、句柄已取
static bool  g_ssrInDepthFail = false;           // 深度镜像建不出 (R2 的一半答案)
static long  g_ssrInN = 0;                       // 入向拷贝触发累计
static long  g_ssrInLogN = 0;                    // 读回自校验节流 (前3次 + 每600次)
static unsigned long long g_ssrInBaseC = 0;      // 建好但**未拷**时的色镜像校验和 (基线)
static unsigned long long g_ssrInBaseD = 0;      // 同上, 深度

// ---- 路线1′ (v0.18.5, docs/05 D2a-4 定案): 深度走"单独 SHARED + 老式 handle → KMT 直入" ----
// 2c 的 NTHANDLE 轴对 D24 家族实测失败 (R2 真凶 = D24 家族 × SHARED_NTHANDLE 这个组合),
// 2d-3/2d-4 两轮探测证明: 深度源 × 单独 SHARED → 建得出 + 老式 handle OK + VK KMT 直入 bind=0。
// 于是深度镜像改走老式 GetSharedHandle, VK 侧**另开导入分支**用 KMT handleType (与色的
// D3D11_TEXTURE_BIT 互不相干) —— 成立则省掉路线1 那每帧一次全屏 PS。
bool g_ssrInDepthKmt = false;              // 深度镜像已按路线1′ 建成 (老式 handle = KMT)

unsigned long long g_ssrInChkD = 0;             // 本帧 D3D11 读回的深度校验和 (每像素4字节全量)
unsigned long long g_ssrInChkD3 = 0;            // 同上, 每像素只喂前3字节 (跳过 stencil 字节)
bool  g_ssrInChkDValid = false;                  // 上两行可用 (本帧做过深度读回), 一次性消费

unsigned long long g_ssrInChkC = 0;             // 本帧 D3D11 侧读回的色校验和
bool  g_ssrInChkCValid = false;                  // 上一行是否可用 (本帧做过 D3D11 读回)
// ---- β3 不一致归因三件套 (v0.16.3): 一次跑图就能把"时序/行距/布局"三分开 ----
unsigned long long g_ssrInChkCPrev = 0;          // 上一次 D3D11 校验和 (差一帧? 用它对)
size_t g_ssrInChkPitch = 0;                      // D3D11 STAGING 实际 RowPitch (行距?)


// ---- O-1 正式修法 (v0.18.4, docs/02 §14.20 / docs/05 O-1): 2c 入向的跨 API EVENT 闸 ----
// 为什么 Flush (v0.18.3) 不够: Flush 只提交不等待, 不构成跨 API 顺序保证 ⇒ 非读回帧到
// Present 时拷贝可能还没跑完, VK 读到上一帧那份入向 (v0.18.3 实测 12e 仍 3/8 不一致)。
// 做法: 拷完 End 一条 D3D11 EVENT 查询 (main 侧), renderer 侧在提交任何读入向镜像的 VK
// 命令前把它等掉 —— 与 PoC-B copyQ 同款范式, 只不过那边管它自己的写, 这边管 2c 入向。
ID3D11Query* g_ssrInQ = nullptr;     // 惰性创建; 建不出 → 降级为无闸 (只打一次日志)
bool  g_ssrInQLive = false;          // 已 End、还没等到 (两侧共享, 一次性消费)
long  g_ssrInGateN = 0;              // 等到次数
double g_ssrInGateMs = 0;            // 累计等待 ms
static bool g_ssrInGateWarn = false; // 创建失败只打一次

// ---- v0.18.8 正解B (docs/02 §14.24 / docs/05 D4 补遗): 段后水深镜像 (第5张) ----
// 水面像素的深度在**段16 之前的快照 520** 里是河床 (水那时还没画) ⇒ 逐像素差分拿到的法线
// = 河床三角面法线 ⇒ 倒影碎成"不规则多边形拼图" (v0.18.7 实跑归因, docs/02 §14.22.1)。
// 段17 那笔"深度测关写开"的水体并回会把**真·水面深度**写进源 461 ⇒ 拷一份当法线输入,
// 520 照旧只当 ray march 的层级 (否则射线一出发就打在自己脚下的水面上, 一个也命中不了)。
// 拷贝时机 = 特征B 之后的**第一次换绑** (段18 的 ev39585): 段17 那笔 draw 已排完, 还没走到
// UI 中途 ClearDS (放到 Present 再拷会被清成全 1.0)。源 desc 变了 (换分辨率) 自动重建。
std::atomic<bool> g_ssrWDepOn{true};    // ini ssr.wdep (默认 1) —— 只关自己
ID3D11Texture2D* g_ssrWDepTex = nullptr; // 段后水深镜像 (desc 照抄源 461)
HANDLE g_ssrWDepH = nullptr;             // 老式 handle(KMT) 或 NT handle (与深度镜像同轴)
bool g_ssrWDepKmt = false;               // true = 老式 SHARED → KMT (本机深度走的那条)
ID3D11Resource* g_ssrWDepSrc = nullptr;  // 源 461 活引用 (特征B 时绑定的那个 DSV 资源)
ID3D11Query* g_ssrWDepQ = nullptr;       // EVENT: 拷完 End, ssrInGateWait 提交 VK 前等掉
bool  g_ssrWDepQLive = false;            // 已 End、还没等到 (两侧共享, 一次性消费)
static long  g_ssrWDepN = 0;             // 拷贝计数 (前8条 + 每128条打日志)
static bool  g_ssrWDepArm = false;       // 特征B 置位 = 段17 已绑, 下一次换绑即可拷
static bool  g_ssrWDepFire = false;      // 本帧待拷 (hook 在 real() 后执行)
static bool  g_ssrWDepOff = false;       // 建不出 / desc 不一致 ⇒ 只关自己, 不再重试
static bool  g_ssrWDepWarn = false;      // 失败日志只打一次

// ---- Step 2d-1 (v0.18.0, docs/05 D3): 出向回写 —— VK 渲完的结果拷进 585 ----
// 第 3 张 SHARED|NTHANDLE 镜像, desc 照抄 324 (= 585 的 desc, 2b 的 [desc一致] 已证)
// → VK 导入后每帧 Present 拷一次 → **下一帧**特征B CopyResource(出向镜像 → 585)。
// 1 帧延迟换零跨 API GPU 栅栏 (理由见 vkrenderer.cpp ssrOutVkBuild 头注释)。
// 逃生门: ini ssr.vkout (默认 0); 开着时 2b 哨兵让位 (585 的内容只可能来自 VK, 归因干净)。
std::atomic<bool> g_ssrVkOutOn{false}; // ini ssr.vkout
ID3D11Texture2D* g_ssrOutTexC = nullptr; // 出向 SHARED 镜像
static ID3D11Texture2D* g_ssrOutStg = nullptr; // 出向镜像 STAGING (节流读回自校验用)
HANDLE g_ssrOutHC = nullptr;             // 出向镜像 NT handle (VK 导入源)
bool  g_ssrOutReady = false;             // VK 已填好 → 本帧特征B 可回写 (renderer 写, hook 读)
long  g_ssrOutN = 0;                     // 2d 回写 585 累计次数
static long  g_ssrOutChkN = 0;           // 出向读回自校验节流 (前3次 + 每600次, 同 2c)。
                                         // C-7: 计数 = 回写事件数, 进门先 ++ 再判节流

// ---- SSR v1 shader 采样 (v0.18.6, docs/05 D4 / Step 3; R4 已按"反推 inv(投影)"定案) ----
// 输出仍走 2d 出向回写 (1 帧延迟, 零跨 API 栅栏), 变的只是**出向内容**:
// 2d 是"入向原样拷过去", v1 是"VK 跑一次全屏三角: 采 324 + 520 深度, 视图空间 ray march
// 出屏幕空间反射, fresnel 合成后写出向镜像"。逃生门 = ini ssr.v1 (默认 0)。
std::atomic<bool> g_ssrV1On{false}; // ini ssr.v1
int   g_ssrV1Mode = 1;              // ini ssr.mode  0=透传(≈2d 原行为) 1=SSR
float g_ssrV1Fov = 65.0f;           // ini ssr.fov   垂直视场角 (度) —— R4 反推 inv(投影) 用
float g_ssrV1Near = 10.0f;          // ini ssr.near  视图单位, 与游戏投影不符时反射比例会偏
float g_ssrV1Far = 100000.0f;       // ini ssr.far
float g_ssrV1Steps = 32.0f;         // ini ssr.steps ray march 步数 (越大越准也越贵)
float g_ssrV1Strength = 1.0f;       // ini ssr.strength **SSR 替换比 0..1** (v0.18.9 起; 0 = 原版 cubemap 反射)
float g_ssrV1Dist = 500.0f;         // ini ssr.dist  ray march 最大距离 (**以 near 为单位**)
float g_ssrV1Rev = 0.0f;            // ini ssr.rev   游戏用反向深度 (近平面->1) 时置 1
int   g_ssrV1Smooth = 4;            // ini ssr.smooth 法线差分邻域 (px) —— v0.18.7 抗"倒影破碎"
int   g_ssrV1Blur = 1;              // ini ssr.blur   反射色 5-tap 空间平滑 0/1
int   g_ssrV1Debug = 0;             // ini ssr.debug  0 正常 / 1 法线 / 2 命中 / 3 深度 / 4 段后水深 / 5 水面像素 / 6 回注可视化 / 7 uBase 原样 / 8 v0.18.15 单位回补量
float g_ssrV1Ripple = 1.0f;         // ini ssr.ripple 涟漪回注量 0..1 (v0.18.9: 段16 的高频涟漪调制乘回 SSR)
int   g_ssrV1RippleSz = 4;          // ini ssr.ripplesz 回注带宽 (px, 1..16) —— v0.18.10: 治"波纹非常细小" (原写死 2px)
int   g_ssrV1RippleMode = 1;        // ini ssr.ripplemode 0=亮度调制 1=位移扭曲 —— v0.18.10: 原版涟漪本是方向扰动
int   g_ssrV1Edge = 0;              // ini ssr.edge 0=未命中回原版层(默认) 1=屏幕边缘延展 2=采真 cubemap —— v0.18.11: 治"扇形区内错位倒影"
                                    //            2=未命中采真 cubemap —— v0.18.16 (issue B), 读入时钳到 0..2
// ---- v0.18.14 回注三旋钮 (三个默认 0 = 全部沿用 v0.18.13 行为, 零回归) ----
float g_ssrV1RipK = 0.0f;    // ini ssr.ripk   输入端软限幅阈值 (线性亮度) 0=关 —— 治位移场被亮斑劫持成 4~8px 细碎饱和块
float g_ssrV1RipAmp = 0.0f;  // ini ssr.ripamp 位移幅度 (px) 0=自动 clamp(ripplesz*1.5, 6, 16) —— 幅度解耦
float g_ssrV1RipGain = 0.0f; // ini ssr.ripgain 梯度增益 0=自动 (=10) —— 增益解耦 (原写死 ×10)
// ---- v0.18.15 高光回补 (§14.30.7 A 的正解): SSR 命中区把 585 层被顶掉的亮斑按比例加回 ----
float g_ssrV1Det = 0.0f; // ini ssr.v1det 回补量 0..1, 0 = 关 = v0.18.14 行为 (零回归)

// ---- v0.18.16 (issue B / P1 探测): 水体探针 cube 的 CPU 侧镜像, 交给 VK 上传 ----
// D3D11 这半边 (probeDumpTick) 用 STAGING 把 g_probeCube 读回来, 紧凑排成 6 面;
// VK 那半边 (ssrV1CubeSync) 看到 tick 变就 vkCmdCopyBufferToImage 灌进自建 VkImage。
// 只读方: vkrenderer.cpp (声明见 vkrenderer.h)。非 edge=2 时 VK 侧根本不建真图, 不花这笔显存。
unsigned char*          g_probeCpu       = nullptr; // 6 面紧密排列 (每面 w*h*8B), 没数据 = nullptr; 非 const: D3D11 侧 malloc/free/memcpy
int                  g_probeW         = 0;
int                  g_probeH         = 0;
int                  g_probeLayers    = 0;
unsigned long long   g_probeCpuTick   = 0;       // 内容代数, 变了才重传

// ---- v0.18.7 B 水色保留 (docs/05 D4 补遗): 585 在被出向回写覆盖**之前**抢一份当合成底色 ----
// 为什么必须有它: 合成底色原先 = 324 快照 = 段16 (水体 pass) **之前**的画面, 里面没画水;
// fresnel 正对相机时反射权重只有 ~0.08 ⇒ 92% 来自"没水的画面" ⇒ 水看起来仍然几乎透明无色。
// 这张镜像 = 段16 画完水之后的 585 (含水), 与 324 同款 SHARED|NTHANDLE, VK 导入后当 binding2。
// 逃生门 = ini ssr.base585 (默认 1); 建不出 → 退回 324 底色 (只关自己, v1/2d 照跑)。
std::atomic<bool> g_ssrBaseOn{false}; // ini ssr.base585
ID3D11Texture2D* g_ssrBaseTex = nullptr; // 底色 SHARED 镜像 (585 段16 后 = 含水画面)
HANDLE g_ssrBaseH = nullptr;          // 底色镜像 NT handle (VK 导入源)
long   g_ssrBaseN = 0;                // 底色拷贝计数 (节流日志, 前8条+每128条)

static SsrCtxEntry* lookupSsrCtx(void** vtbl)
{
	if (!vtbl)
		return nullptr;
	for (int i = 0; i < g_ssrCtxN; ++i)
		if (g_ssrCtx[i].vtbl == vtbl)
			return &g_ssrCtx[i];
	return nullptr;
}

// 视图 → 底层 2D 纹理 desc + 对象身份 (QI ID3D11Texture2D 的指针值作帧内身份: 同对象 QI
// 结果稳定; 只取指针值立即 Release, 不持引用 —— 资源仍归游戏)
static bool ssrViewDesc(ID3D11View* v, D3D11_TEXTURE2D_DESC* d, void** obj)
{
	if (!v || !d)
		return false;
	ID3D11Resource* res = nullptr;
	v->GetResource(&res);
	if (!res)
		return false;
	ID3D11Texture2D* tex = nullptr;
	const HRESULT hr = res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
	res->Release();
	if (FAILED(hr) || !tex)
		return false;
	tex->GetDesc(d);
	if (obj)
		*obj = reinterpret_cast<void*>(tex);
	tex->Release();
	return true;
}

static bool ssrIsD24(DXGI_FORMAT f)
{
	return f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT ||
	       f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || f == DXGI_FORMAT_X24_TYPELESS_G8_UINT;
}

// 资源 → 底层 2D 纹理 desc + 对象身份。取法与 ssrViewDesc 完全一致 (QI ID3D11Texture2D
// 的指针值作身份, 用完即 Release 不持引用) ⇒ CopyResource 拿到的 ID3D11Resource* 与
// OMSetRenderTargets 侧 view->GetResource() 再 QI 的结果可直接比对。
static bool ssrResObj(ID3D11Resource* res, D3D11_TEXTURE2D_DESC* d, void** obj)
{
	if (!res)
		return false;
	ID3D11Texture2D* tex = nullptr;
	const HRESULT hr = res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
	if (FAILED(hr) || !tex)
		return false;
	tex->GetDesc(d);
	if (obj)
		*obj = reinterpret_cast<void*>(tex);
	tex->Release();
	return true;
}

std::string ssrFmtName(DXGI_FORMAT f)
{
	switch (f)
	{
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
	case DXGI_FORMAT_R10G10B10A2_UNORM:  return "R10G10B10A2";
	case DXGI_FORMAT_R8G8B8A8_UNORM:     return "RGBA8";
	case DXGI_FORMAT_R16G16_FLOAT:       return "RG16F";
	// 2d-3 深度改道路线 1 的候选格式 (docs/05 D2a-4「R2 深度改道实现缺口」):
	// D3D11 侧把深度转进**可共享**的浮点颜色镜像, 再复用 2c-β 已验通的 D3D11_TEXTURE_BIT 导入。
	case DXGI_FORMAT_R32_FLOAT:          return "R32F";
	case DXGI_FORMAT_R16_FLOAT:          return "R16F";
	case DXGI_FORMAT_R32_TYPELESS:       return "R32TL";
	case DXGI_FORMAT_D32_FLOAT:          return "D32F";
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
	case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return "D24家族";
	default: return "fmt" + std::to_string(static_cast<int>(f));
	}
}

// 上下文身份核对: 安装时抓的 immediate 指针若与调用方不一致 (RenderDoc 包装/重取等),
// 不能就此哑掉 —— 告警一次后转为按 vtbl 观察全部上下文 (g_ssrWatchAll), 宁可多记不漏记
static bool ssrWatch(SsrCtxEntry* e, ID3D11DeviceContext* ctx)
{
	if (!e)
		return false;
	if (e->imm == ctx)
		return true;
	if (g_ssrWatchAll)
		return true;
	g_ssrWatchAll = true;
	logLine("SSR侦察: 上下文指针与安装时不同 (imm=" + hexOf(e->imm) + " 调用方=" + hexOf(ctx) +
	        ") → 转为按 vtbl 观察全部上下文 (宁多记不漏记)");
	return true;
}

// 槽33 观察: 只读解析参数 (候选 / 强特征 / 特征B), 不动渲染
static void ssrReconOm(UINT n, ID3D11RenderTargetView* const* ppRTV, ID3D11DepthStencilView* pDSV)
{
	g_ssrFOm.fetch_add(1, std::memory_order_relaxed);
	// ---- v0.18.8 B: 特征B (段16→段17 换绑) 之后的**第一次换绑** = 段17 那笔 draw 已排完 ----
	// 放在最顶上 (早于一切 early return): 段17 与段18 之间的换绑才是要的时点, 中间若夹一条
	// NumViews=0 的解绑也一样成立 (段17 的 draw 已经排进队列)。置 fire 后由 hook 在 real()
	// 之后执行拷贝 —— 拷的源 = 下面特征B 里存下的那个 DSV 资源 (真机 461)。
	if (g_ssrWDepArm && !g_ssrWDepFire)
	{
		g_ssrWDepArm = false;
		g_ssrWDepFire = true;
	}
	// 每次换绑先假定"强特征通道已中断", 解析成功且确为强特征才回置 true —— 特征A 靠它
	// 判断 clear 是否落在强特征通道进行期间 (真机 585 整帧不被清, 见 ssrReconClear)
	const bool wasStrong = g_ssrInStrong;
	g_ssrInStrong = false;
	if (!ppRTV || n == 0 || n > 8)
		return;
	// 按非空视图数计 (防 NumViews=8 带 null), 取首个非空视图作 RT0 身份
	UINT nonNull = 0;
	ID3D11RenderTargetView* first = nullptr;
	for (UINT i = 0; i < n; ++i)
		if (ppRTV[i])
		{
			if (!first)
				first = ppRTV[i];
			++nonNull;
		}
	if (!first)
		return;
	D3D11_TEXTURE2D_DESC rd{};
	void* robj = nullptr;
	if (!ssrViewDesc(first, &rd, &robj))
		return;
	// 与 notePresent 的 fetch_add+1 口径对齐: 事件时点 present 计数还是上一帧的, 本帧 = +1
	const unsigned long long fr = g_presentCount.load(std::memory_order_relaxed) + 1;

	// 判别子: 本帧以该对象为 RT0 的 OMSetRenderTargets 次数 + 跨几个连续绑定段
	// (Present 时对强特征对象报数)。S4 全帧实测 142 次 OMSet (按本函数同口径回放
	// `S4-api-scan.txt`): 585 = 16 次/1 段, 321 = 50 次/4 段 —— "1 段 vs 4 段" 是
	// 反射目标与主 HDR 缓冲的结构性区别; 且只在强特征集合内有效 (339 也 17次/1段)。
	{
		int i = 0;
		for (; i < g_ssrRtNN; ++i)
			if (g_ssrRtN[i].obj == robj)
				break;
		if (i == g_ssrRtNN)
		{
			if (g_ssrRtNN < (int)(sizeof(g_ssrRtN) / sizeof(g_ssrRtN[0])))
			{
				g_ssrRtN[g_ssrRtNN].obj = robj;
				g_ssrRtN[g_ssrRtNN].n = 0;
				g_ssrRtN[g_ssrRtNN].runs = 0;
				++g_ssrRtNN;
			}
			else if (++g_ssrRtNOver == 1)
				logLine("SSR侦察: 本帧 RT0 身份表已满(32) — 新 RT0 不再计绑定次数");
		}
		if (i < g_ssrRtNN)
		{
			++g_ssrRtN[i].n;
			if (robj != g_ssrPrevRt0) // RT0 换对象 = 换段, 该对象新开一个连续绑定段
			{
				++g_ssrRtN[i].runs;
				g_ssrPrevRt0 = robj;
			}
		}
	}

	// 特征B: 强特征之后首次换绑到别的 RT0 (段16 → 段17 ev39512 的 321+591)。fired 后不再记;
	// 不清 lastStr —— 特征A 可能晚于 B 出现, 两个信号独立计数, 顺序不预设 (docs/05 二选一)
	if (g_ssrLastStr && robj != g_ssrLastStr && !g_ssrBFired)
	{
		g_ssrBFired = true;
		g_ssrFB.fetch_add(1, std::memory_order_relaxed);
		// Step 2b 通路哨兵 (docs/05 §5): 仅当"刚离开的强特征对象 = 上一帧判别出的反射目标
		// 585" 才标记回写 —— 段16 缺席的窗口里第一个强特征是段19(321), 不等于 585 ⇒ 不标记,
		// 避免写错缓冲。真正执行在 hookedOMSetRenderTargets 的 real() 之后 (那时段17 已绑,
		// 585 恰好已解绑, CopyResource 写进去最安全)。
		// 额外要求 g_ssrMainHdr 已学到且 != g_ssrReflRt —— 即"runs==1 与 runs>1 是两个不同
		// 对象"的正常双强特征格局已确立。菜单/加载期只有一个强特征段, 只会设到其中一个变量,
		// 该条件不成立 ⇒ 过渡期不会拿菜单对象当 585 去写。
		if (g_ssrSentinelOn.load(std::memory_order_relaxed) &&
		    !g_ssrVkOutOn.load(std::memory_order_relaxed) && g_ssrReflRt &&
		    g_ssrMainHdr && g_ssrMainHdr != g_ssrReflRt &&
		    g_ssrLastStr == g_ssrReflRt && g_ssrStrRes)
			g_ssrSentinelPending = true;
		// 2d 出向开着时 2b 让位 (上面不 arm) ⇒ 日志里"哨兵="恒 0, 585 的内容只可能来自 VK,
		// 归因干净; 特征B 那行也就不会出现 [2b哨兵已排队]。
		if (g_ssrVkOutOn.load(std::memory_order_relaxed) && g_ssrOutReady &&
		    g_ssrStrRes && g_ssrLastStr == g_ssrReflRt)
			g_ssrSentinelPending = false; // 双保险: 过渡帧残留的排队标记一并清掉
		// Step 2d-1 出向回写的 **一次性门** (C-6): 与 2b 同一触发点 (特征B)、同一组前置
		// (双强特征格局已确立 + 刚离开的是 585), 只是开关互斥 (2b 要求 !vkout, 2d 要求 vkout)。
		// 没有这道门时 2d 只判身份条件, 而段16 的 16 次 OMSet 重绑让该条件每帧恒成立
		// ⇒ 2026-10-06 实测 172597 次 / 6019 帧 = 28.7 次/帧 (设计每帧 1 次, 2b 验收 0.8 次/帧)。
		if (g_ssrVkOutOn.load(std::memory_order_relaxed) && g_ssrOutReady && g_ssrReflRt &&
		    g_ssrMainHdr && g_ssrMainHdr != g_ssrReflRt &&
		    g_ssrLastStr == g_ssrReflRt && g_ssrStrRes)
			g_ssrOutPending = true;
		// Step 2c 共享入向 (v0.15.0, docs/05 D2a-a): 与 2b **同一触发点** (特征B), 但开关独立。
		// 只要求"双强特征格局已确立"(菜单期 g_ssrMainHdr == g_ssrReflRt ⇒ 不 arm, 同 D2a-3a),
		// **不要求 lastStr == 585** —— 入向拷的是 324/520, 与 585 是谁无关, 段16 缺席帧照拷。
		if (g_ssrSharedOn.load(std::memory_order_relaxed) && g_ssrReflRt &&
		    g_ssrMainHdr && g_ssrMainHdr != g_ssrReflRt && g_ssrSceneRes)
			g_ssrInPending = true;
		// ---- v0.18.8 B: 记下段17 的 DSV 资源 (真机 461) 并 arm 下一次换绑去拷段后水深 ----
		// 只在这里存 (段17 的 pDSV 才是"会写水面深度"的那块); 上一帧若没等到换绑就把 fire 清掉,
		// 免得下一帧开头拿**旧帧**的水深去拷 (那份已被本帧的清屏/重画作废)。
		if (g_ssrWDepOn.load(std::memory_order_relaxed) && pDSV)
		{
			ID3D11Resource* r = nullptr;
			pDSV->GetResource(&r); // ID3D11View::GetResource 返回 void, 出参由被调方 AddRef
			if (r)
			{
				if (g_ssrWDepSrc != r)
				{
					if (g_ssrWDepSrc)
						g_ssrWDepSrc->Release();
					g_ssrWDepSrc = r; // 活引用: 换绑换缓冲才换, 拷贝时一直可用
				}
				else
					r->Release();
			}
			if (g_ssrWDepSrc)
			{
				g_ssrWDepFire = false;
				g_ssrWDepArm = true;
			}
		}
		const long k = ++g_ssrBLogN;
		if (k <= 16 || (k % 64) == 0)
			logLine("SSR侦察: 特征B(换绑)#" + std::to_string(k) + " 帧=" + std::to_string(fr) +
			        " 新RT0=" + hexOf(robj) + " (" + std::to_string(rd.Width) + "x" +
			        std::to_string(rd.Height) + " " + ssrFmtName(rd.Format) + ") 非空=" +
			        std::to_string(nonNull) + (g_ssrSentinelPending ? " [2b哨兵已排队]" : "") +
			        (g_ssrOutPending ? " [2d出向已排队]" : "") +
			        (g_ssrInPending ? " [2c入向已排队]" : ""));
	}

	// 候选: 恰1个非空视图且 1920x1080 RGBA16F (段15 无 DS 也命中; 段7/17/18 是 MRT×2/×3
	// 靠视图数排除, 非空=1 是强特征的前置条件 —— 否则段17 会被误判第二个强特征)
	if (nonNull != 1 || rd.Width != 1920 || rd.Height != 1080 ||
	    rd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
		return;
	const long c = ++g_ssrCandN;
	g_ssrFCand.fetch_add(1, std::memory_order_relaxed);
	// 强特征: 再 + 1920x1080 D24 家族 DSV (段16)
	std::string dsTxt = "无";
	bool strong = false;
	if (pDSV)
	{
		D3D11_TEXTURE2D_DESC dd{};
		void* dobj = nullptr;
		if (ssrViewDesc(pDSV, &dd, &dobj))
		{
			dsTxt = hexOf(dobj) + " " + std::to_string(dd.Width) + "x" + std::to_string(dd.Height) +
			        " " + ssrFmtName(dd.Format);
			strong = dd.Width == 1920 && dd.Height == 1080 && ssrIsD24(dd.Format);
		}
		else
			dsTxt = "有(QI失败)";
	}
	if (strong)
	{
		g_ssrInStrong = true;
		// 强特征"进入"才计数 —— 真机段16 每个 Draw 前重绑一次 (一帧 16 次), 按次计则
		// fs 恒=16, "同帧强特征恰1" 判据必挂。换对象 或 通道被中断后再绑回 才算新的一段。
		if (robj != g_ssrLastStr || !wasStrong)
		{
			g_ssrStrTot++;
			g_ssrFStr.fetch_add(1, std::memory_order_relaxed);
			g_ssrLastStr = robj;
			// 2b 写目标的活引用 —— robj 是 ssrViewDesc QI 后即 Release 的指针值, 只能比对
			// 不能解引用; CopyResource 要真对象。换强特征对象才换 ref (段16 的 16 次重绑
			// 同对象不换), 每帧至多换几次。段17 非强特征不进来 ⇒ 特征B 触发时这里存的
			// 仍是刚离开的段16 的资源。
			if (robj != g_ssrStrResObj)
			{
				if (g_ssrStrRes)
					g_ssrStrRes->Release();
				g_ssrStrRes = nullptr;
				// ID3D11View::GetResource 返回 **void** (不是 HRESULT), 出参由被调方 AddRef
				first->GetResource(&g_ssrStrRes);
				if (g_ssrStrRes)
					g_ssrStrResObj = robj;
			}
			// 这里不复位 A/B fired —— 只在 Present 复位, 每帧各至多触发一次。否则段19
			// 进入时复位, 段19 之后 ev44352 换绑到 35 会再触发一次 B → fb=2 误判异常。
			bool dup = false;
			for (int i = 0; i < g_ssrFObjN; ++i)
				if (g_ssrFObj[i] == robj)
					dup = true;
			if (!dup && g_ssrFObjN < (int)(sizeof(g_ssrFObj) / sizeof(g_ssrFObj[0])))
				g_ssrFObj[g_ssrFObjN++] = robj;
		}
	}
	// 候选详情按 (对象, 是否强特征) 去重后再节流 —— 真机段16 每个 Draw 前都重绑一次
	// (一帧 16 次), 不去重则首 16 行全是同一行, 预算烧光也看不到别的候选对象 (段15/19 的 321)
	if (robj != g_ssrLastCand || strong != g_ssrLastCandStr)
	{
		g_ssrLastCand = robj;
		g_ssrLastCandStr = strong;
		const long k = ++g_ssrCandLogN;
		if (k <= 16 || (c % 64) == 0)
			logLine("SSR侦察: 槽33候选#" + std::to_string(k) + " 帧=" + std::to_string(fr) +
			        " 本帧第" + std::to_string(c) + "次候选 n=" + std::to_string(n) +
			        " 非空=" + std::to_string(nonNull) + " RT0=" + hexOf(robj) + " (" +
			        std::to_string(rd.Width) + "x" + std::to_string(rd.Height) + " " +
			        ssrFmtName(rd.Format) + ") DS=" + dsTxt + (strong ? " 强特征" : ""));
	}
}

// 槽50 观察: 特征A = 强特征通道"进行期间"发生的 ClearRenderTargetView。
// 修正 (S4 api-scan 实测, 推翻 docs/05:105 的 "585 用后清"): ev39511 清的是 view 592
// = 纹理 591 (段17 的 RT1, R10G10B10A2), **585 整帧从不被清** —— pass5 的 clears.rts 字段
// 取自 bound_targets(), 是"当时绑着谁"而非"清谁", 旧判据 (清对象==强特征对象) 永不触发。
// 但该 clear 发生时 585 仍是绑定中的 RT0, 正是段16 结束的前兆, 故判据改为"清在强特征通道
// 进行期间", 被清对象是谁不参与判定, 只记进日志供判读。
static void ssrReconClear(ID3D11RenderTargetView* pRTV)
{
	if (!pRTV || !g_ssrLastStr || !g_ssrInStrong || g_ssrAFired)
		return;
	g_ssrAFired = true;
	g_ssrFA.fetch_add(1, std::memory_order_relaxed);
	D3D11_TEXTURE2D_DESC d{};
	void* obj = nullptr;
	std::string tgt = "QI失败";
	if (ssrViewDesc(pRTV, &d, &obj))
		tgt = hexOf(obj) + " " + std::to_string(d.Width) + "x" + std::to_string(d.Height) + " " +
		      ssrFmtName(d.Format);
	const long k = ++g_ssrALogN;
	if (k <= 16 || (k % 64) == 0)
		logLine("SSR侦察: 特征A(强特征期间清)#" + std::to_string(k) + " 帧=" +
		        std::to_string(g_presentCount.load(std::memory_order_relaxed) + 1) + " 被清=" +
		        tgt + " 强特征RT0=" + hexOf(g_ssrLastStr));
}

// 槽47 观察 (v0.14.0 Step 2a, docs/05 D2a): 每条 CopyResource 记 src/dst 对象与 desc,
// 并按"来源"学 324/520 身份 —— 抓帧 ResourceId 跨帧不可用 (R1), 只能认来源:
//   324 = src 为 1920x1080 RGBA16F 的拷贝之 dst (S4 全帧 3 条 Copy 里唯一 src 为 RGBA16F
//         的就是 ev39225 的 321→324; 深度两条 src 是 D24 家族, 被格式挡住)
//   520 = 本帧**首条** src 为 1920x1080 D24 家族的拷贝之 dst (S4: ev21505 的 461→520;
//         本帧第二条 461→466 是后期深度 —— 靠 g_ssrCopyFrame 锁帧号只取首条)
// 只读, 不改渲染。324 的活引用在这里一并持有 (pDst 由游戏持有, AddRef 一份保证 2b 执行时
// 对象仍存活; ssrViewDesc/ssrResObj 返回的是 QI 后即 Release 的指针值, 不能拿去解引用)。
static void ssrReconCopy(ID3D11Resource* pDst, ID3D11Resource* pSrc)
{
	g_ssrCopyN++;
	const unsigned long long fr = g_presentCount.load(std::memory_order_relaxed) + 1;
	D3D11_TEXTURE2D_DESC sd{}, dd{};
	void* sobj = nullptr;
	void* dobj = nullptr;
	const bool sok = ssrResObj(pSrc, &sd, &sobj);
	const bool dok = ssrResObj(pDst, &dd, &dobj);
	if (sok && dok)
	{
		if (sd.Width == 1920 && sd.Height == 1080 &&
		    sd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
		{
			if (g_ssrSceneObj != dobj)
			{
				if (g_ssrSceneRes)
					g_ssrSceneRes->Release();
				pDst->AddRef();
				g_ssrSceneRes = pDst;
				g_ssrSceneObj = dobj;
				logLine("SSR侦察: [2a] 场景色快照 324 = " + hexOf(dobj) + " (src=" +
				        hexOf(sobj) + " " + std::to_string(sd.Width) + "x" +
				        std::to_string(sd.Height) + " " + ssrFmtName(sd.Format) +
				        ") 活引用已持 帧=" + std::to_string(fr));
			}
		}
		else if (sd.Width == 1920 && sd.Height == 1080 && ssrIsD24(sd.Format) &&
		         g_ssrCopyFrame != fr)
		{
			g_ssrCopyFrame = fr;
			if (g_ssrDepthObj != dobj)
			{
				if (g_ssrDepthRes)
					g_ssrDepthRes->Release();
				pDst->AddRef(); // 2c: 与 g_ssrSceneRes 同法持一份活引用 (源是 520)
				g_ssrDepthRes = pDst;
				g_ssrDepthObj = dobj;
				logLine("SSR侦察: [2a] 深度快照 520(假设) = " + hexOf(dobj) + " (src=" +
				        hexOf(sobj) + " " + ssrFmtName(sd.Format) +
				        ") 活引用已持 本帧首条深度拷贝 帧=" + std::to_string(fr));
			}
		}
	}
	const long k = ++g_ssrCopyLogN;
	if (k <= 24 || (k % 64) == 0)
	{
		const std::string sTxt = sok
		    ? (hexOf(sobj) + " " + std::to_string(sd.Width) + "x" + std::to_string(sd.Height) +
		       " " + ssrFmtName(sd.Format))
		    : std::string("QI失败");
		const std::string dTxt = dok
		    ? (hexOf(dobj) + " " + std::to_string(dd.Width) + "x" + std::to_string(dd.Height) +
		       " " + ssrFmtName(dd.Format))
		    : std::string("QI失败");
		logLine("SSR侦察: 槽47Copy#" + std::to_string(k) + " 帧=" + std::to_string(fr) +
		        " dst=" + dTxt + " <- src=" + sTxt);
	}
}

static void STDMETHODCALLTYPE hookedCopyResource(ID3D11DeviceContext* ctx, ID3D11Resource* pDst,
                                                 ID3D11Resource* pSrc)
{
	using Fn_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
	SsrCtxEntry* e = lookupSsrCtx(ctx ? *reinterpret_cast<void***>(ctx) : nullptr);
	const Fn_t real = e ? reinterpret_cast<Fn_t>(e->orig47) : nullptr;
	if (!real) // 只有 isD3D11Family 校验通过才会被 patch, 见 installSsrRecon
		return;
	if (!g_ssrSelfCopy && ssrWatch(e, ctx) && g_ssrOn.load(std::memory_order_relaxed))
		ssrReconCopy(pDst, pSrc); // 2b 哨兵自己的拷贝跳过 —— 计数与学习都只认游戏的
	real(ctx, pDst, pSrc);
}

// ---- Step 2c (v0.15.0): 十六进制/校验和小工具 ----
std::string uhex64(unsigned long long v)
{
	static const char* d = "0123456789ABCDEF";
	std::string s(16, '0');
	for (int i = 15; i >= 0; --i)
	{
		s[i] = d[v & 0xF];
		v >>= 4;
	}
	return s;
}

// FNV-1a 抽样 (64 行 x 16 列, 不做全图读)。**D3D11 STAGING 读回与 2c-β 的 VK 映射
// buffer 共用这一个采样器** —— 两边必须逐字节算同一份数据, 校验和才有可比性:
// 交叉校验的全部意义就是"同一张共享镜像, D3D11 侧读到的值 == VK 侧读到的值",
// 若不等即说明两套 API 对这块共享内存的视图/布局/字节序不一致 (2c-β 的核心未知数)。
// rowPitch 由调用方给: D3D11 是 Mapped.RowPitch, VK 是 width*8 (紧密排列)。
// 按 (像素间隔 stride, 每像素喂哈希的字节数 nB) 抽样算 FNV-1a。
// 色镜像 stride=nB=8; 深度候选三档 (路线1′ 跨API比对用): (4,4)=D3D 全量含 stencil 字节,
// (4,3)=跳过 stencil 字节只看低24位, (3,3)=VK 若把深度按 24 位紧密排布时的行式。
unsigned long long ssrFnvSampleAdv(const void* data, size_t rowPitch, unsigned width,
                                   unsigned height, int stride, int nB, long* nz)
{
	if (nz)
		*nz = 0;
	if (!data || stride <= 0 || nB <= 0 || nB > stride || !width || !height)
		return 0;
	unsigned long long h = 14695981039346656037ULL; // FNV-1a offset basis (0xcbf29ce484222325)
	long nzc = 0;
	const int rows = 64;
	const int cols = 16;
	for (int r = 0; r < rows && r < (int)height; ++r)
	{
		const unsigned char* row = static_cast<const unsigned char*>(data) + (size_t)r * rowPitch;
		for (int c = 0; c < cols; ++c)
		{
			const int x = (int)((long long)c * width / cols);
			const unsigned char* px = row + (size_t)x * stride;
			for (int b = 0; b < nB; ++b)
			{
				h ^= px[b];
				h *= 1099511628211ULL; // FNV prime
				if (px[b])
					++nzc;
			}
		}
	}
	if (nz)
		*nz = nzc;
	return h;
}

// 原接口 = 每像素取满 bpp 字节 (色16F 走 bpp=8)。调用面不改, 实现委托给上面那个带参版本。
unsigned long long ssrFnvSample(const void* data, size_t rowPitch, unsigned width,
                                       unsigned height, int bpp, long* nz)
{
	return ssrFnvSampleAdv(data, rowPitch, width, height, bpp, bpp, nz);
}

// 把 STAGING 镜像读回算 FNV-1a 校验和。
// **为什么必须自校验**: CopyResource 返回 void, desc 不一致时 D3D11 静默丢弃、日志照打
// (2b 已踩过这坑, 见 [2b] 的 desc 预检) ⇒ "镜像建得出"不能证明"拷进去了", 必须读回看内容。
// 基线 = 建镜像时(尚未拷过)的校验和; 之后读到的值与基线不同 = 拷贝真落地。
// CPU 读回阻塞 (docs/05 约束 2 禁每帧读回) ⇒ 由调用方节流; 内部把 g_ssrSelfCopy 置位,
// 免得自己这道暂存拷贝被槽47 钩计进 2a 的 COPY=/学习里。
unsigned long long ssrInFnv(ID3D11DeviceContext* ctx, ID3D11Texture2D* mir,
                                   ID3D11Texture2D* stg, const char* nm, long* nz,
                                   size_t* pitch, // v0.16.3: 把实际 RowPitch 带出去
                                   unsigned long long* alt3) // v0.18.5: 深度低24位候选 (跳 stencil)
{
	if (nz)
		*nz = 0;
	if (pitch)
		*pitch = 0;
	if (alt3)
		*alt3 = 0;
	if (!ctx || !mir || !stg)
		return 0;
	const bool prevSelf = g_ssrSelfCopy;
	g_ssrSelfCopy = true;
	ctx->CopyResource(stg, mir);
	D3D11_MAPPED_SUBRESOURCE m{};
	const HRESULT hr = ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m);
	if (FAILED(hr) || !m.pData)
	{
		g_ssrSelfCopy = prevSelf;
		logLine("SSR侦察: [2c读回] " + std::string(nm) + " Map(STAGING) 失败 " + hexHr(hr));
		return 0;
	}
	D3D11_TEXTURE2D_DESC md{};
	mir->GetDesc(&md);
	const int bpp = (md.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4;
	if (pitch)
		*pitch = m.RowPitch; // β3 归因: D3D11 侧真正用的行距 (与 VK 的 width*8 比)
	const unsigned long long h = ssrFnvSample(m.pData, m.RowPitch, md.Width, md.Height, bpp, nz);
	// 路线1′: 深度(4字节/像素) 顺手再算一份"只喂前3字节"的 —— DXGI 把 depth 放低24位、
	// stencil 放高8位, VK 侧 depth aspect 拷出来的 stencil 字节未定义 ⇒ 低24位才是可比口径。
	if (alt3 && bpp == 4)
		*alt3 = ssrFnvSampleAdv(m.pData, m.RowPitch, md.Width, md.Height, 4, 3, nullptr);
	ctx->Unmap(stg, 0);
	g_ssrSelfCopy = prevSelf;
	return h;
}

// ---- 2d-3 深度格式探测 (v0.18.2, docs/05 D2a-4「R2 深度改道实现缺口」/ :431) ----
// 2c-α 现有的重试矩阵只动 **BindFlags** (0x48 → 0x08 → 0x00) 三次全失败 => 已定案"病因=格式"。
// 本函数在它旁边**再加一列格式**: R32_FLOAT / R16_FLOAT / R32_TYPELESS × SHARED|NTHANDLE
// (正是路线 1 要写进去的那个镜像格式), 外加"源格式 × 单独 SHARED"一格, 用来排除 MiscFlags 轴。
// 每格问两个问题: ① CreateTexture2D 建得出吗 ② 建得出的话 DXGI 让共享吗
// (NT handle 优先 —— VK 的 D3D11_TEXTURE_BIT 只认这种; 没带 NTHANDLE 的那格退回老式 handle 验一下)。
// **纯发现**: 建出来的立刻 Release, 不写任何 g_ssrIn* 状态、不改本轮任何行为 (docs/05 明写
// "结论出来前不写死实现")。整轮只跑一次 (静态 flag), 且只在深度那条路上跑。
static void ssrDepthFormatProbe(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& sd, const char* nm)
{
	static bool s_probed = false;
	if (s_probed || !dev)
		return;
	if (std::string(nm).find("深度") == std::string::npos)
		return; // 只为深度改道取数 (色324 实测本来就能共享)
	s_probed = true;

	const UINT bf0 = 0u;
	const UINT bfShader = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; // 路线1 的 PS 要写它
	const UINT miscNt = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	const UINT miscLegacy = D3D11_RESOURCE_MISC_SHARED;
	struct Cell
	{
		DXGI_FORMAT fmt;
		UINT bf;
		UINT misc;
		const char* tag;
	};
	const Cell cells[] = {
		{ sd.Format, bf0, miscLegacy, "源格式" },
		{ DXGI_FORMAT_R32_FLOAT, bf0, miscNt, "R32_FLOAT" },
		{ DXGI_FORMAT_R32_FLOAT, bfShader, miscNt, "R32_FLOAT" },
		{ DXGI_FORMAT_R16_FLOAT, bf0, miscNt, "R16_FLOAT" },
		{ DXGI_FORMAT_R16_FLOAT, bfShader, miscNt, "R16_FLOAT" },
		{ DXGI_FORMAT_R32_TYPELESS, bf0, miscNt, "R32_TYPELESS" },
		{ DXGI_FORMAT_R32_TYPELESS, bfShader, miscNt, "R32_TYPELESS" },
	};
	auto addTag = [](std::string& s, const char* tag) {
		if (s.find(tag) == std::string::npos)
		{
			if (!s.empty())
				s += ", ";
			s += tag;
		}
	};
	logLine("SSR侦察: [2d-3] 格式探测开始 (2c-α BindFlags 矩阵的第 2 列: 格式) 源=" +
	        ssrFmtName(sd.Format) + "(Format=" + std::to_string(static_cast<int>(sd.Format)) + ") " +
	        std::to_string(sd.Width) + "x" + std::to_string(sd.Height) +
	        " — 纯发现用, 建出来的立刻 Release, 不改本轮回写行为");
	std::string okBuilt, okNt;
	// C-8 (v0.18.3): 记下第 1 格 (源格式 × 单独 SHARED) 的实测结果, 结论行后半句照它写。
	bool srcBuiltOk = false, srcOldOk = false;
	for (int i = 0; i < static_cast<int>(sizeof(cells) / sizeof(cells[0])); ++i)
	{
		const Cell& c = cells[i];
		D3D11_TEXTURE2D_DESC td = sd; // 尺寸/mips/msaa 照抄源, 只换 Format/BindFlags/MiscFlags
		td.Format = c.fmt;
		td.BindFlags = c.bf;
		td.CPUAccessFlags = 0;
		td.MiscFlags = c.misc;
		ID3D11Texture2D* t = nullptr;
		const HRESULT hr = dev->CreateTexture2D(&td, nullptr, &t);
		std::string res;
		if (SUCCEEDED(hr) && t)
		{
			addTag(okBuilt, c.tag);
			res = "建=OK";
			if (i == 0)
				srcBuiltOk = true;
			HRESULT hr2 = E_FAIL;
			HANDLE h = nullptr;
			IDXGIResource1* r1 = nullptr;
			hr2 = t->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&r1));
			if (SUCCEEDED(hr2) && r1)
				hr2 = r1->CreateSharedHandle(nullptr,
				                             DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
				                             nullptr, &h);
			if (SUCCEEDED(hr2) && h)
			{
				res += " handle=OK(NT)";
				addTag(okNt, c.tag);
			}
			else if (r1)
			{
				// 老式 handle = **IDXGIResource::GetSharedHandle** (不是 ID3D11Device 的方法,
				// 首版写成 dev->GetSharedHandle 被 CI 抓成 C2039): 证明"格式本身可共享",
				// 但 VK 的 D3D11_TEXTURE_BIT 只认 NT handle, 这种导不了。
				HANDLE hOld = nullptr;
				if (SUCCEEDED(r1->GetSharedHandle(&hOld)) && hOld)
				{
					res += " handle=老式OK(NT=FAIL " + hexHr(hr2) + ")";
					if (i == 0)
						srcOldOk = true;
				}
				else
					res += " handle=FAIL " + hexHr(hr2);
			}
			else
				res += " handle=FAIL(拿不到 IDXGIResource1) " + hexHr(hr2);
			if (r1)
				r1->Release();
			t->Release();
		}
		else
		{
			if (t) // 理论上 FAILED 时不会给指针, 但不白赌一次 Release
			{
				t->Release();
				t = nullptr;
			}
			res = "建=FAIL " + hexHr(hr);
		}
		logLine("SSR侦察: [2d-3]   格式探测#" + std::to_string(i + 1) + " " + c.tag + "(Format=" +
		        std::to_string(static_cast<int>(c.fmt)) + ") BindFlags=0x" +
		        uhex64(c.bf).substr(8) + " misc=" +
		        ((c.misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) ? "SHARED|NTHANDLE" : "SHARED") +
		        " → " + res);
	}
	// C-8 (v0.18.3): 结论行后半句必须按第 1 格**实测**写。v0.18.2 把它写死成"源格式 不可共享",
	// 与同一批第 1 格 "建=OK 老式handle=OK" 自相矛盾 (§14.18.1 查出), 会把人带回
	// "病因 = 格式不在 D3D11 SHARED 白名单" 这个已被推翻的口径 —— 真实病因是
	// "D24 家族 × SHARED_NTHANDLE 这个组合" (只动 BindFlags 那轮矩阵少排除了一个轴)。
	std::string tail;
	if (okNt.empty())
		tail = " → 路线1 前置不成立, 退回路线2 (仅颜色 SSR + 屏幕边缘 fallback)";
	else
	{
		std::string srcPart;
		if (srcBuiltOk && srcOldOk)
			srcPart = "可单独 SHARED 共享(老式 handle 拿得到)、只是拿不到 NT handle ⇒ 对当前走 "
			          "D3D11_TEXTURE_BIT 的导入路径无用, 病因是 D24 家族 × SHARED_NTHANDLE 组合";
		else
			srcPart = "不可共享 (BindFlags 矩阵 + 本表第1格双重确认)";
		tail = " → 路线1 前置成立: D3D11 侧 shader 把深度转进 " + okNt +
		       " 的 SHARED 镜像, 再走 2c-β 同一条导入; 源格式 " + ssrFmtName(sd.Format) + " " + srcPart;
	}
	logLine("SSR侦察: [2d-3]   格式探测 结论: 建得出 = " +
	        (okBuilt.empty() ? std::string("无") : okBuilt) +
	        "; 建得出且拿得到 NT handle (VK D3D11_TEXTURE_BIT 可导入) = " +
	        (okNt.empty() ? std::string("无") : okNt) + tail);
}

// ---- 2d-4 路线 1' 探测 (v0.18.3, docs/05 D2a-4 末「路线 1'」/ docs/02 §14.19) ----
// D3D11 侧两问: 源格式 × **单独 SHARED (故意不带 NTHANDLE)** 在 BindFlags {0x00, 0x48} 下
// ① CreateTexture2D 建得出吗 ② 拿得到老式 handle 吗 (IDXGIResource::GetSharedHandle)。
// 拿到就把 handle 交给 vkrenderer 的 ssrKmtProbeVk 走 VK 半边 (#3 支持度查询 / #4 实测导入)。
// 拿到 handle 的那张纹理**活到 VK 半边跑完才 Release** —— 中途释放会让 handle 悬垂。
// **纯发现**: 不写任何 g_ssrIn* 与 g_ssrOut* 状态、不改本轮回写行为; 与 2d-3 同点触发、整轮只跑一次。
// 结果读法: 若 #1/#2 老式 handle OK 且 #4 导入+绑定 OK ⇒ 深度可原样进 VK, 省掉路线1 的每帧全屏 PS。
static void ssrKmtProbe(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC& sd, const char* nm)
{
	static bool s_probed = false;
	if (s_probed || !dev)
		return;
	if (std::string(nm).find("深度") == std::string::npos)
		return; // 只为深度改道取数 (色324 实测本来就能 NT 共享, 走不到这条分支)
	s_probed = true;

	const UINT bfCand[2] = {0u, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL}; // 0x00 / 0x48
	HANDLE hKmt = nullptr;
	ID3D11Texture2D* kept = nullptr; // 拿到 handle 的那张, 活到 VK 半边跑完
	logLine("SSR侦察: [2d-4] 路线1' 探测开始 (D24 走老式 SHARED → KMT handle → VK 直入, 目标是省掉"
	        " 路线1 的每帧全屏 PS) 源=" + ssrFmtName(sd.Format) + "(Format=" +
	        std::to_string(static_cast<int>(sd.Format)) + ") " + std::to_string(sd.Width) + "x" +
	        std::to_string(sd.Height) + " — 纯发现, 不改本轮回写行为");
	for (int i = 0; i < 2; ++i)
	{
		D3D11_TEXTURE2D_DESC td = sd; // 尺寸/mips/msaa 照抄源, 只换 BindFlags/MiscFlags
		td.BindFlags = bfCand[i];
		td.CPUAccessFlags = 0;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // ← 老式 SHARED, **不带** NTHANDLE (2c 的失败轴)
		ID3D11Texture2D* t = nullptr;
		const HRESULT hr = dev->CreateTexture2D(&td, nullptr, &t);
		std::string res;
		if (SUCCEEDED(hr) && t)
		{
			res = "建=OK";
			IDXGIResource* r0 = nullptr;
			HANDLE h = nullptr;
			HRESULT hr2 = t->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&r0));
			if (SUCCEEDED(hr2) && r0)
				hr2 = r0->GetSharedHandle(&h);
			if (r0)
				r0->Release();
			if (SUCCEEDED(hr2) && h)
			{
				res += " 老式handle=OK";
				if (!kept) // 第一张拿到的留用, 第二张当场放
				{
					hKmt = h;
					kept = t;
					t = nullptr;
				}
			}
			else
				res += " 老式handle=FAIL " + hexHr(hr2);
			if (t)
				t->Release();
		}
		else
		{
			if (t)
			{
				t->Release();
				t = nullptr;
			}
			res = "建=FAIL " + hexHr(hr);
		}
		logLine("SSR侦察: [2d-4]   KMT探测#" + std::to_string(i + 1) + " D3D11 源格式 BindFlags=0x" +
		        uhex64(bfCand[i]).substr(8) + " misc=SHARED → " + res);
	}
	bool vkOk = false;
	if (hKmt)
		vkOk = ssrKmtProbeVk(hKmt, sd);
	else
	{
		logLine("SSR侦察: [2d-4]   KMT探测#3 VK 支持度查询跳过 (D3D11 侧没拿到老式 handle)");
		logLine("SSR侦察: [2d-4]   KMT探测#4 VK 导入实测跳过 (D3D11 侧没拿到老式 handle)");
	}
	if (kept)
	{
		kept->Release(); // VK 半边已把自己建的 VkImage/VkDeviceMemory 销毁完, 这里才放
		kept = nullptr;
	}
	std::string concl;
	if (!hKmt)
		concl = "路线1' 前置不成立 (D3D11 侧连老式 handle 都拿不到) → 回到路线1 (R32_FLOAT 全屏 PS)";
	else if (vkOk)
		concl = "路线1' 前置成立 (老式 handle + VK 导入 + 绑定 全过) → 深度可原样直入 VK, "
		        "省掉路线1 的每帧全屏 PS —— 落地前仍按 D2a-4 另开导入分支评估";
	else
		concl = "路线1' 前置不成立 (老式 handle 拿到了, VK 侧没走通, 看 #3/#4 卡在哪一步) "
		        "→ 回到路线1 (R32_FLOAT 全屏 PS)";
	logLine("SSR侦察: [2d-4]   KMT探测 结论: " + concl);
}

// 建 1 张 SHARED|NTHANDLE 镜像 (desc 照抄源) 并取 NT handle。失败只降级、不抛。
// v0.18.5 路线1′: **深度**源在 NTHANDLE 轴三次全败后, 退回"单独 SHARED + 老式 handle"
// (2d-3 实测建得出、2d-4 实测 VK KMT 直入 bind=0), 成功则 *kmtOut=true 交调用方走 KMT 分支。
static bool ssrInMakeShared(ID3D11Device* dev, ID3D11Resource* src, ID3D11Texture2D** out,
                            HANDLE* hOut, const char* nm, bool* kmtOut = nullptr)
{
	*out = nullptr;
	*hOut = nullptr;
	if (kmtOut)
		*kmtOut = false;
	D3D11_TEXTURE2D_DESC sd{};
	if (!ssrResObj(src, &sd, nullptr))
	{
		logLine("SSR侦察: [2c] " + std::string(nm) + " 镜像: 源 QI ID3D11Texture2D 失败 → 跳过");
		return false;
	}
	if (sd.Usage != D3D11_USAGE_DEFAULT)
	{
		logLine("SSR侦察: [2c] " + std::string(nm) + " 镜像: 源 Usage 非 DEFAULT → 跳过");
		return false;
	}
	D3D11_TEXTURE2D_DESC td = sd; // **全抄** (含 BindFlags) —— 满足 CopyResource 对 src/dst
	                              // 一致性最严的解释, 少一个字段对不上就是静默丢弃
	td.CPUAccessFlags = 0;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	const UINT origBF = td.BindFlags;
	HRESULT hr = dev->CreateTexture2D(&td, nullptr, out);
	if (FAILED(hr) || !*out)
	{
		// ---- R2 归因诊断 (v0.16.0): 真机实测深度报 E_INVALIDARG, 必须分清两种病因 ----
		//   (a) **格式**不在 D3D11 SHARED 白名单 (D24 家族大概率如此) → 递减 BindFlags 仍全失败;
		//   (b) 源带 D3D11_BIND_DEPTH_STENCIL 而 SHARED 不许带 → 放宽 BindFlags 后成功。
		// 只重试 BindFlags、**不动 Format** —— CopyResource 要求 src/dst 格式完全一致,
		// 改了格式拷贝必失败。放宽后 desc 与源不一致的风险由 [2c读回 ≠基线/=基线] 判据兜底。
		const HRESULT hrFirst = hr;
		logLine("SSR侦察: [2c] " + std::string(nm) + " SHARED 镜像 CreateTexture2D 失败 " +
		        hexHr(hrFirst) + " (" + std::to_string(td.Width) + "x" + std::to_string(td.Height) +
		        " Format=" + std::to_string(static_cast<int>(td.Format)) +
		        " " + ssrFmtName(td.Format) + " BindFlags=0x" + uhex64(origBF).substr(8) +
		        " mips" + std::to_string(td.MipLevels) +
		        " msaa" + std::to_string(td.SampleDesc.Count) + ") ← R2 风险点");
		const UINT cand[2] = { D3D11_BIND_SHADER_RESOURCE, 0 };
		for (int ci = 0; ci < 2 && (FAILED(hr) || !*out); ++ci)
		{
			if (cand[ci] == origBF)
				continue; // 与原始相同 —— 上面那次已经试过了
			td.BindFlags = cand[ci];
			hr = dev->CreateTexture2D(&td, nullptr, out);
			logLine("SSR侦察: [2c]   重试 " + std::string(nm) + " BindFlags=0x" +
			        uhex64(cand[ci]).substr(8) + " (原 0x" + uhex64(origBF).substr(8) + ") → " +
			        ((SUCCEEDED(hr) && *out)
		                     ? std::string("OK = 病因是 BindFlags, 格式可共享!")
		                     : std::string("仍失败 ") + hexHr(hr) + " = 病因是格式"));
		}
		if (FAILED(hr) || !*out)
		{
			logLine("SSR侦察: [2c] " + std::string(nm) + " 三种 BindFlags 全失败 → NTHANDLE 轴到此为止: " +
			        ssrFmtName(sd.Format) + " (Format=" +
			        std::to_string(static_cast<int>(sd.Format)) +
			        ") × SHARED_NTHANDLE 不让共享 —— R2 归因=这个组合 (明细看 [2d-3]/[2d-4] 探测)");
			ssrDepthFormatProbe(dev, sd, nm); // 2d-3: 在 BindFlags 矩阵旁再加一列格式 (纯发现)
			ssrKmtProbe(dev, sd, nm);        // 2d-4: 路线 1' 老式 SHARED → KMT → VK 直入 (纯发现)
			// ---- 路线1′ 正式落地 (v0.18.5, docs/05 D2a-4 定案): 深度改走"单独 SHARED" ----
			// 只对**深度**源开这条道 —— 色324 的 NTHANDLE 本来就通 (根本走不到这里), 且 VK 侧
			// 本轮只给深度开了 KMT 导入分支; 色若失败仍按 v0.18.4 行为停用入向, 不连坐。
			// 两档 BindFlags 正是 2d-4 实测过的两格 (源 desc 与 0x00), 不引入未测组合。
			const bool depthSrc = (std::string(nm).find("深度") != std::string::npos);
			td = sd;
			td.CPUAccessFlags = 0;
			td.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // ← 单独 SHARED, **不带** NTHANDLE (2d-3 已证可建)
			const UINT lc[2] = { origBF, 0u };
			for (int li = 0; li < 2 && depthSrc && (FAILED(hr) || !*out); ++li)
			{
				if (li > 0 && lc[li] == lc[li - 1])
					continue;
				td.BindFlags = lc[li];
				hr = dev->CreateTexture2D(&td, nullptr, out);
				logLine("SSR侦察: [2d-5]   老式SHARED 重试 " + std::string(nm) + " BindFlags=0x" +
				        uhex64(lc[li]).substr(8) + " → " +
				        ((SUCCEEDED(hr) && *out) ? std::string("OK (路线1′ 可建)")
				                                 : std::string("失败 ") + hexHr(hr)));
			}
			if (FAILED(hr) || !*out)
			{
				*out = nullptr;
				if (!depthSrc)
					return false; // 色源: 维持 v0.18.4 行为 (入向停用), 上面的 R2 日志已打足
				logLine("SSR侦察: [2d-5] " + std::string(nm) + " 老式 SHARED 也建不出 → 结论: " +
				        ssrFmtName(sd.Format) + " 不可 legacy-shared → 路线1′ 关闭, 回落路线1 (R32_FLOAT 全屏 PS)");
				return false;
			}
			if (kmtOut)
				*kmtOut = true;
		}
	}
	if (td.BindFlags != origBF)
		logLine("SSR侦察: [2c] " + std::string(nm) + " 注意: 镜像 BindFlags 已放宽 0x" +
		        uhex64(origBF).substr(8) + "→0x" + uhex64(td.BindFlags).substr(8) +
		        " (与源不一致, CopyResource 能否落地看 [2c读回] 判据)");
	const bool legacyKmt = (kmtOut && *kmtOut); // 路线1′: 老式 handle (KMT) 走另一条取法
	if (legacyKmt)
	{
		// 老式 handle = **IDXGIResource::GetSharedHandle** —— 不是 CreateSharedHandle
		// (那个 API 要求 SHARED_NTHANDLE, 而深度正是栽在这一轴)。返回值归资源所有,
		// **不 CloseHandle**, 与 2d-4 探测同一处理; 老式 handle 即 KMT, 交 VK 的
		// VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT 导入分支。
		IDXGIResource* r0 = nullptr;
		hr = (*out)->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&r0));
		if (SUCCEEDED(hr) && r0)
		{
			hr = r0->GetSharedHandle(hOut);
			r0->Release();
		}
		if (FAILED(hr) || !*hOut)
		{
			logLine("SSR侦察: [2d-5] " + std::string(nm) + " GetSharedHandle 失败 " + hexHr(hr) +
			        " (" + ssrFmtName(td.Format) + ") → 路线1′ 关闭 (老式句柄没拿到)");
			(*out)->Release();
			*out = nullptr;
			*hOut = nullptr;
			if (kmtOut)
				*kmtOut = false;
			return false;
		}
	}
	else
	{
		IDXGIResource1* r1 = nullptr;
		hr = (*out)->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&r1));
		if (SUCCEEDED(hr) && r1)
		{
			hr = r1->CreateSharedHandle(nullptr,
			                            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
			                            nullptr, hOut);
			r1->Release();
		}
		if (FAILED(hr) || !*hOut)
		{
			logLine("SSR侦察: [2c] " + std::string(nm) + " CreateSharedHandle 失败 " + hexHr(hr) +
			        " (" + ssrFmtName(td.Format) + ") ← R2 风险点: D3D11 建得出但 DXGI 不让共享");
			(*out)->Release();
			*out = nullptr;
			*hOut = nullptr;
			return false;
		}
	}
	logLine("SSR侦察: [2c] " + std::string(nm) + " SHARED 镜像 OK = " + hexOf(*out) + " (" +
	        std::to_string(td.Width) + "x" + std::to_string(td.Height) +
	        " Format=" + std::to_string(static_cast<int>(td.Format)) + " " +
	        ssrFmtName(td.Format) + " BindFlags=0x" + uhex64(td.BindFlags).substr(8) +
	        ") handle=" + hexOf(*hOut) + (legacyKmt ? " [路线1′ 老式SHARED/KMT]" : "") +
	        " 源=" + hexOf(src));
	return true;
}

// ---- Step 2d-1 (v0.18.0): 出向 SHARED 镜像 —— VK 渲完的结果落这儿, 再由 D3D11 拷进 585 ----
// desc **照抄 324**: 2b 的 [desc一致] 已经证明 324 与 585 的 desc 逐字段相同 ⇒ 这张镜像
// 既能被 VK 按 D3D11_TEXTURE_BIT 导入 (入向同法), 也能被 CopyResource 原样拷进 585。
// 不带 BindFlags 重试矩阵 —— RGBA16F 能建 SHARED 已在 2c 实证过, 失败就按调用方给的降级文案关闸,
// 绝不连坐入向 (入向是已收口的 2c 成果)。
// SHARED 镜像通用建法 (C-2 同理的收敛): 2d 出向镜像 与 v0.18.7 底色镜像 的建法完全一样
// —— 照抄源 desc (含 BindFlags, 满足 CopyResource 最严解释) + SHARED|NTHANDLE + CreateSharedHandle
// —— 只有日志标签与降级文案不同, 故一份实现两个调用面, 避免两份独立演进导致判读口径分叉。
// tag = 日志方括号 (如 "[2d]" / "[base]"), name = 镜像名, dgr = 失败降级文案, tail = 成功行尾巴。
static bool ssrMakeSharedTo(ID3D11Device* dev, ID3D11Resource* src, ID3D11Texture2D** outTex,
                            HANDLE* outH, const char* tag, const char* name, const char* dgr,
                            const char* tail)
{
	*outTex = nullptr;
	*outH = nullptr;
	D3D11_TEXTURE2D_DESC sd{};
	if (!ssrResObj(src, &sd, nullptr) || sd.Usage != D3D11_USAGE_DEFAULT)
	{
		logLine(std::string("SSR侦察: ") + tag + " " + name + "镜像: 源 QI/Usage 不可用 → " + dgr);
		return false;
	}
	D3D11_TEXTURE2D_DESC td = sd; // 全抄 (含 BindFlags) —— 与入向同法, 满足 CopyResource 最严解释
	td.CPUAccessFlags = 0;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	ID3D11Texture2D* t = nullptr;
	HRESULT hr = dev->CreateTexture2D(&td, nullptr, &t);
	if (FAILED(hr) || !t)
	{
		logLine(std::string("SSR侦察: ") + tag + " " + name + " SHARED 镜像 CreateTexture2D 失败 " +
		        hexHr(hr) + " (" + std::to_string(td.Width) + "x" + std::to_string(td.Height) + " " +
		        ssrFmtName(td.Format) + " BindFlags=0x" + uhex64(td.BindFlags).substr(8) +
		        ") → " + dgr);
		return false;
	}
	IDXGIResource1* r1 = nullptr;
	hr = t->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&r1));
	if (SUCCEEDED(hr) && r1)
	{
		hr = r1->CreateSharedHandle(nullptr,
		                            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
		                            nullptr, outH);
		r1->Release();
	}
	if (FAILED(hr) || !*outH)
	{
		logLine(std::string("SSR侦察: ") + tag + " " + name + " CreateSharedHandle 失败 " + hexHr(hr) +
		        " (" + ssrFmtName(td.Format) + ") → " + dgr);
		t->Release();
		*outH = nullptr;
		return false;
	}
	*outTex = t;
	logLine("SSR侦察: " + std::string(tag) + " " + name + "镜像 OK = " + hexOf(*outTex) + " (" +
	        std::to_string(td.Width) + "x" + std::to_string(td.Height) + " " +
	        ssrFmtName(td.Format) + " BindFlags=0x" + uhex64(td.BindFlags).substr(8) +
	        ") handle=" + hexOf(*outH) + " 源=" + hexOf(src) + " " + tail);
	return true;
}

// 2d 出向镜像 (v0.18.0): desc 照抄 324 (≡585), VK 侧 ssrOutVkBuild 导入
static bool ssrOutMakeShared(ID3D11Device* dev, ID3D11Resource* src)
{
	return ssrMakeSharedTo(dev, src, &g_ssrOutTexC, &g_ssrOutHC, "[2d]", "出向",
	                       "出向回写不启用 (入向照常)", "— desc 照抄324(≡585), VK 侧 ssrOutVkBuild 导入");
}

// 首次触发时建两张镜像 + 各自 STAGING + 记"未拷"基线校验和。幂等。
// 延迟到首次触发才建: 324/520 的身份要到进实机场景后才齐 (菜单期两值相等、不 arm)。
static void ssrInBuild(ID3D11DeviceContext* ctx)
{
	if (g_ssrInBuilt || !g_ssrSceneRes || !g_ssrDepthRes)
		return;
	ID3D11Device* dev = nullptr;
	ctx->GetDevice(&dev);
	if (!dev)
		return;
	ID3D11Texture2D* tc = nullptr;
	ID3D11Texture2D* td = nullptr;
	HANDLE hc = nullptr;
	HANDLE hd = nullptr;
	bool kmtC = false; // 色源不会走路线1′ (NTHANDLE 本来就通), 留着只为调用面统一
	bool kmtD = false;
	if (!ssrInMakeShared(dev, g_ssrSceneRes, &tc, &hc, "色324", &kmtC))
	{
		dev->Release();
		return; // 色建不出 → 入向整段停用 (上面已打 R2 风险点日志)
	}
	if (!ssrInMakeShared(dev, g_ssrDepthRes, &td, &hd, "深度520", &kmtD))
	{
		g_ssrInDepthFail = true; // R2 的一半答案: 深度不让共享 —— 色照跑, 不连坐
		if (td)
		{
			td->Release();
			td = nullptr;
		}
		if (hd)
		{
			CloseHandle(hd); // 只有 NTHANDLE 路径的 hd 归我方; 老式 handle 失败时早已置空
			hd = nullptr;
		}
	}
	// 路线1′ (v0.18.5): 深度镜像是老式 SHARED 建成的 ⇒ 告诉 renderer 用 KMT handleType 导入
	g_ssrInDepthKmt = (kmtD && td && hd);
	auto mkStg = [&](ID3D11Texture2D* src, ID3D11Texture2D** out) {
		*out = nullptr;
		if (!src)
			return;
		D3D11_TEXTURE2D_DESC md{};
		src->GetDesc(&md);
		D3D11_TEXTURE2D_DESC sd = md;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		if (FAILED(dev->CreateTexture2D(&sd, nullptr, out)) || !*out)
		{
			*out = nullptr;
			logLine("SSR侦察: [2c] STAGING 镜像 CreateTexture2D 失败 " +
			        ssrFmtName(sd.Format) + " → 该项读回自校验不可用");
		}
	};
	mkStg(tc, &g_ssrInStgC);
	mkStg(td, &g_ssrInStgD);
	// ---- 2d-1 出向镜像 (独立门 ssr.vkout, 默认 0 ⇒ 上面那段完全不变) ----
	if (g_ssrVkOutOn.load(std::memory_order_relaxed))
	{
		if (ssrOutMakeShared(dev, g_ssrSceneRes))
			mkStg(g_ssrOutTexC, &g_ssrOutStg);
		else
			g_ssrVkOutOn.store(false, std::memory_order_relaxed); // 建不出 → 关闸, 不再重试
	}
	// ---- v0.18.7 B 底色镜像 (独立门 ssr.base585): 抢段16 画完水之后的 585 当合成底色 ----
	// 源优先用本帧活的 585 (g_ssrStrRes, 身份已在本帧特征B 核过), 没有就退回 324 (desc 等价)。
	// 与出向镜像同一份实现, 失败只关自己: 底色退回 324 (v1 照常出倒影, 只是水仍偏透明)。
	if (g_ssrBaseOn.load(std::memory_order_relaxed) &&
	    g_ssrVkOutOn.load(std::memory_order_relaxed) &&
	    g_ssrV1On.load(std::memory_order_relaxed))
	{
		ID3D11Resource* srcB = g_ssrStrRes ? g_ssrStrRes : g_ssrSceneRes;
		if (!ssrMakeSharedTo(dev, srcB, &g_ssrBaseTex, &g_ssrBaseH, "[base]", "底色",
		                     "底色合成不启用 (v1 退回 324 底色)",
		                     "— 源=585(段16 后, 含水), 每帧特征B 在 2d 回写之前抢一份"))
			g_ssrBaseOn.store(false, std::memory_order_relaxed); // 建不出 → 关闸, 不再重试
	}
	dev->Release();
	g_ssrInTexC = tc;
	g_ssrInHC = hc;
	g_ssrInTexD = td;
	g_ssrInHD = hd;
	long nz = 0;
	g_ssrInBaseC = ssrInFnv(ctx, g_ssrInTexC, g_ssrInStgC, "色基线", &nz);
	long nzd = 0;
	g_ssrInBaseD = (g_ssrInTexD && g_ssrInStgD)
	    ? ssrInFnv(ctx, g_ssrInTexD, g_ssrInStgD, "深基线", &nzd)
	    : 0;
	g_ssrInBuilt = true;
	logLine("SSR侦察: [2c] 共享入向就绪 — 色324=OK 深度520=" +
	        std::string(g_ssrInDepthFail
	                        ? "FAIL(R2 未过)"
	                        : (g_ssrInDepthKmt ? "OK(路线1′ 老式SHARED→KMT)" : "OK")) +
	        "; 基线校验和(建好未拷) 色=0x" + uhex64(g_ssrInBaseC) +
	        " 深=0x" + uhex64(g_ssrInBaseD) +
	        "; 自此每帧特征B 处 CopyResource(324/520 → SHARED 镜像)" +
	        (g_ssrVkOutOn.load(std::memory_order_relaxed)
	             ? "; [2d] 出向镜像已一并建好 (ssr.vkout=1, 回写 585 走 VK)"
	             : "") +
	        (g_ssrBaseOn.load(std::memory_order_relaxed)
	             ? "; [base] 底色镜像已一并建好 (ssr.base585=1, 段16 后的 585 当合成底色)"
	             : ""));
}

// 触发点: 与 2b 哨兵同位 (特征B → real() 后), 把 324/520 拷进各自 SHARED 镜像。
static void ssrInQueue(ID3D11DeviceContext* ctx)
{
	if (!g_ssrSharedOn.load(std::memory_order_relaxed) || g_ssrSelfCopy || !ctx)
		return;
	if (!g_ssrInBuilt)
	{
		ssrInBuild(ctx);
		if (!g_ssrInBuilt)
			return;
	}
	g_ssrSelfCopy = true; // 自己的拷贝不回灌进 2a 的 COPY= 计数与身份学习 (与 2b 同法)
	if (g_ssrSceneRes && g_ssrInTexC)
		ctx->CopyResource(g_ssrInTexC, g_ssrSceneRes); // 324 → 色镜像
	if (g_ssrDepthRes && g_ssrInTexD)
		ctx->CopyResource(g_ssrInTexD, g_ssrDepthRes); // 520 → 深度镜像
	g_ssrSelfCopy = false;
	// O-1 (v0.18.3): 把"入向已可见"的隐含假设补成**每帧**成立。
	// 原来只有 2c 的阻塞读回 (前3次 + 每600次, 见下方 ssrInFnv 的 Map) 才把 D3D11 队列等完 ⇒
	// 非读回帧到 Present 时 D3D11 这条 CopyResource 可能还没交到 GPU, VK 就按零跨API栅栏的
	// 设计读了入向镜像, 读到的是**上一帧**那份 ⇒ [2d] 出向读回 出现"出向 = 2 帧前的入向"差一帧
	// 不一致 (v0.18.2 实测 9 行里 5 行如此, 而每帧都阻塞读回的头 3 行全一致 = 证据链)。
	// Flush 只提交不等待: 尽早把拷贝交给 GPU, 让本帧 Present 时 VK 读到本帧这份。
	// —— 但 v0.18.3 实测证明**这不够**: 12e 仍 3/8 不一致 (Flush 不构成跨 API 顺序保证),
	// 故下面补 O-1 的正式修法 (v0.18.4): 拷完 End 一条 D3D11 EVENT 查询, 由 renderer 侧的
	// ssrInGateWait 在提交任何读入向镜像的 VK 命令前把它等掉 (PoC-B copyQ 同款范式)。
	// 查询惰性建: 建不出就降级为"无闸", 只打一次日志 (那时 12e 可能仍差一帧)。
	if (!g_ssrInQ)
	{
		ID3D11Device* dev = nullptr;
		ctx->GetDevice(&dev);
		if (dev)
		{
			D3D11_QUERY_DESC qd{};
			qd.Query = D3D11_QUERY_EVENT;
			const HRESULT hq = dev->CreateQuery(&qd, &g_ssrInQ);
			dev->Release();
			if (FAILED(hq) || !g_ssrInQ)
			{
				g_ssrInQ = nullptr;
				if (!g_ssrInGateWarn)
				{
					g_ssrInGateWarn = true;
					logLine("SSR侦察: [2c] 入向 EVENT 查询创建失败 " + hexHr(hq) +
					        " → O-1 闸不生效 (12e 可能仍差一帧)");
				}
			}
		}
	}
	// 上一条查询结果还没被 renderer 消费 (VK 侧本帧没跑) 就不再 End,
	// 免得 debug layer 报"查询结果未取又 End"。
	if (g_ssrInQ && !g_ssrInQLive)
	{
		ctx->End(g_ssrInQ);
		g_ssrInQLive = true;
	}
	ctx->Flush(); // End 之后再 Flush: 查询连同拷贝一起进驱动队列 (GetData 带 DONOTFLUSH, 不替我们提交)
	const long k = ++g_ssrInN;
	const long c = ++g_ssrInLogN;
	const bool logThis = (k <= 8) || (k % 128) == 0;
	const bool chk = (c <= 3) || (c % 600) == 0; // 读回阻塞, 只做极少数次
	if (!logThis && !chk)
		return;
	std::string s = "SSR侦察: [2c] 入向拷贝#" + std::to_string(k) + " 色324=" +
	                hexOf(g_ssrSceneObj) + "→" + hexOf(g_ssrInTexC);
	s += (g_ssrDepthRes && g_ssrInTexD ? " 深520=" + hexOf(g_ssrDepthObj) + "→" +
	                                         hexOf(g_ssrInTexD)
	                                   : " 深=跳过(R2 未过)");
	if (chk)
	{
		long nz = 0;
		size_t pit = 0;
		const unsigned long long h = ssrInFnv(ctx, g_ssrInTexC, g_ssrInStgC, "色", &nz, &pit);
		// 留给 2c-β: 同一帧内 VK 侧要用**同一个** ssrFnvSample 算一遍与 h 比对。
		// 时序安全: 这里的 Map 返回已保证 D3D11 把镜像写完了 (CPU 都读到了), 而
		// 镜像只在特征B 被写、到 Present 之间没人再动它 ⇒ Present 时 VK 读到的是同一份字节。
		g_ssrInChkC = h;
		g_ssrInChkCValid = true;
		g_ssrInChkPitch = pit; // β3 归因: 若 ≠ width*8 ⇒ 两边抽样点位不同 (归因3 行距)
		s += " [2c读回 色=0x" + uhex64(h) + (h != g_ssrInBaseC ? "≠基线✓" : "=基线✗") +
		     " 非零" + std::to_string(nz);
		if (g_ssrInTexD && g_ssrInStgD)
		{
			long nzd = 0;
			unsigned long long h2lo3 = 0;
			const unsigned long long h2 =
			    ssrInFnv(ctx, g_ssrInTexD, g_ssrInStgD, "深", &nzd, nullptr, &h2lo3);
			s += " 深=0x" + uhex64(h2) + (h2 != g_ssrInBaseD ? "≠基线✓" : "=基线✗") +
			     " 非零" + std::to_string(nzd);
			// 路线1′: 两个口径一并带出, 给 [2d-5] 的 VK 侧三候选比对当 D3D11 侧基准。
			// 低3字节 = 跳过 stencil (DXGI 把 depth 放低24位), VK depth aspect 的 stencil 未定义。
			g_ssrInChkD = h2;
			g_ssrInChkD3 = h2lo3;
			g_ssrInChkDValid = g_ssrInDepthKmt; // 只有 KMT 分支消费它 (色/NT 路径不看)
			s += " 低3=0x" + uhex64(h2lo3);
		}
		s += "]";
	}
	s += " 帧=" + std::to_string(g_presentCount.load(std::memory_order_relaxed) + 1);
	logLine(s);
}

static void ftNoteCtx(ID3D11DeviceContext* ctx); // 定义在下方"帧时基线"块 (它在本函数之后)

// ---- C-3: desc 比较走字段, 字符串只在不匹配时构造 ----
// 原实现把 desc 转成 string 再比 (`dsc(sd) == dsc(dd)`), 既多一次分配, 又**漏了 ArraySize**
// (两处 dsc 都没打 ArraySize ⇒ 数组片数不同的两张图会被判成"一致"而白拷一次)。
static bool descEqual(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b)
{
	return a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels &&
	       a.ArraySize == b.ArraySize && a.Format == b.Format &&
	       a.SampleDesc.Count == b.SampleDesc.Count;
}

static std::string descStr(const D3D11_TEXTURE2D_DESC& d) // 仅日志用 (含 ArraySize)
{
	return std::to_string(d.Width) + "x" + std::to_string(d.Height) + " " +
	       ssrFmtName(d.Format) + " mips" + std::to_string(d.MipLevels) + " arr" +
	       std::to_string(d.ArraySize) + " msaa" + std::to_string(d.SampleDesc.Count);
}

// ---- C-2: 2b 哨兵 / 2d 出向 的 desc 预检 + CopyResource + 节流日志 —— **唯一一份实现** ----
// CopyResource 返回 **void**: src/dst 尺寸/格式/多重采样/片数不一致时不会给你 HRESULT,
// 只会静默失败 (画面没变化, 日志照打) ⇒ 判读会误判成"回写没生效"。所以必须先比 desc 再拷,
// 并把两边 desc 打进日志 ([desc一致] 才算通路验通)。原先 2b/2d 各写一份、逐字重复,
// 两处独立演进 → 判读口径分叉风险, 现收敛成一份 (待修复事项总结 C-2)。
// linePrefix = 调用方拼好的行首 (含 #k、两个资源 id、帧号), k 决定节流 (前8条 + 每128条)。
// 返回 desc 是否一致。
static bool copyResDescChecked(ID3D11DeviceContext* ctx, ID3D11Resource* dst,
                               ID3D11Resource* src, const std::string& linePrefix,
                               const char* dstTag, const char* srcTag, long k)
{
	D3D11_TEXTURE2D_DESC sd{}, dd{};
	void* so = nullptr;
	void* dso = nullptr;
	const bool sok = ssrResObj(src, &sd, &so);
	const bool dok = ssrResObj(dst, &dd, &dso);
	const bool same = sok && dok && descEqual(sd, dd);
	g_ssrSelfCopy = true;
	ctx->CopyResource(dst, src);
	g_ssrSelfCopy = false;
	if (k <= 8 || (k % 128) == 0)
		logLine(linePrefix +
		        (same ? std::string(" [desc一致]")
		              : std::string(" [desc不一致! ") + dstTag + "=" +
		                    (dok ? descStr(dd) : std::string("QI失败")) + " " + srcTag + "=" +
		                    (sok ? descStr(sd) : std::string("QI失败")) + "]"));
	return same;
}

// ---- v0.18.8 B: 段后水深 (源 461) → 第5张镜像 + EVENT 闸 ----
// 调用点 = hookedOMSetRenderTargets 的 real() 之后, 且 g_ssrWDepFire 已置 (见 ssrReconOm):
// 即特征B (段16→段17 换绑) 之后的**第一次换绑** —— 段17 那笔 depth 只写不测的水体并回已排完,
// 源里是**真·水面深度**, 且还没走 UI 中途 ClearDS (晚到 Present 会被清成全 1.0)。
// 与 2c 入向同一手法: 先比 desc 再拷 (CopyResource 失败不给码), 拷完 End 一条 EVENT, 由
// renderer 侧 ssrInGateWait 在提交任何读水深的 VK 命令前等掉 (2c 那道闸排在本拷贝之前, 等不到它)。
static void ssrWDepQueue(ID3D11DeviceContext* ctx)
{
	if (!g_ssrWDepOn.load(std::memory_order_relaxed) || g_ssrWDepOff || !ctx || !g_ssrWDepSrc)
		return;
	ID3D11Device* dev = nullptr;
	ctx->GetDevice(&dev);
	if (!dev)
		return;
	D3D11_TEXTURE2D_DESC sd{};
	if (!ssrResObj(g_ssrWDepSrc, &sd, nullptr)) // 源 QI 不出 ID3D11Texture2D ⇒ 换不到 desc, 只能关
	{
		dev->Release();
		return;
	}
	// 源 desc 变了 (换分辨率 / 换深度缓冲) ⇒ 退掉旧镜像按新源重建。VK 侧靠 handle 变化
	// (g_ssrWDepH 变) 自动摘 view + 重导入, 与底色图同一套自愈路径。
	if (g_ssrWDepTex)
	{
		D3D11_TEXTURE2D_DESC md{};
		g_ssrWDepTex->GetDesc(&md);
		if (!descEqual(md, sd))
		{
			logLine("SSR侦察: [wdep] 源 desc 变了 (" + descStr(md) + " → " + descStr(sd) +
			        ") → 水深镜像重建");
			g_ssrWDepTex->Release();
			g_ssrWDepTex = nullptr;
			g_ssrWDepH = nullptr; // 老式 handle 归资源所有, 不 CloseHandle (与 2c 同口径)
			g_ssrWDepKmt = false;
		}
	}
	if (!g_ssrWDepTex)
	{
		// 名字必须含"深度": ssrInMakeShared 的老式 SHARED 兜底只对深度源开 (与 520 同一道)
		if (!ssrInMakeShared(dev, g_ssrWDepSrc, &g_ssrWDepTex, &g_ssrWDepH, "段后深度461",
		                     &g_ssrWDepKmt))
		{
			g_ssrWDepOff = true;
			logLine("SSR侦察: [wdep] 段后水深镜像建不出 → 水深法线关自己 (法线退回 520 河床 "
			        "= v0.18.7 行为, 倒影仍是多边形拼图; 只关自己, 底色/行进/2d 全不受影响)");
			dev->Release();
			return;
		}
		logLine("SSR侦察: [wdep] 段后水深镜像就绪: 源=0x" + hexOf(g_ssrWDepSrc) + " → 镜像=0x" +
		        hexOf(g_ssrWDepTex) + " handle=0x" + hexOf(g_ssrWDepH) +
		        (g_ssrWDepKmt ? " [KMT/老式SHARED]" : " [NT]") + " — 本帧起当法线/原点输入");
	}
	// desc 预检: 按 CopyResource 合同只比 项/尺寸/格式/mips/数组/采样 (BindFlags 不比 ——
	// 镜像 desc 照抄源建, 若因 SHARED 放宽过也不影响拷本身)。不一致 = 静默丢弃, 必须先拦。
	{
		D3D11_TEXTURE2D_DESC xd{};
		g_ssrWDepTex->GetDesc(&xd);
		if (!descEqual(xd, sd))
		{
			if (!g_ssrWDepWarn)
			{
				g_ssrWDepWarn = true;
				logLine("SSR侦察: [wdep] 源/镜像 desc 不一致 (镜像=" + descStr(xd) +
				        " 源=" + descStr(sd) + ") → 水深拷贝停用 (法线退回 520)");
			}
			g_ssrWDepOff = true;
			dev->Release();
			return;
		}
	}
	const long k = ++g_ssrWDepN;
	g_ssrSelfCopy = true; // 自己的拷贝不回灌进 2a 的 COPY= 计数与身份学习 (与 2b/2c 同法)
	ctx->CopyResource(g_ssrWDepTex, g_ssrWDepSrc);
	g_ssrSelfCopy = false;
	if (k <= 8 || (k % 128) == 0)
		logLine("SSR侦察: [wdep] 水深拷贝#" + std::to_string(k) + " 源461=0x" + hexOf(g_ssrWDepSrc) +
		        " → 镜像=0x" + hexOf(g_ssrWDepTex) + " 帧=" +
		        std::to_string(g_presentCount.load(std::memory_order_relaxed) + 1));
	// EVENT 闸 (与 2c 同款): 本拷贝排在 2c 的 g_ssrInQ End **之后**, 那道闸等不到它 ⇒ 自己 End
	// 一条。建不出就降级为 Flush (与 2c 同一口径: 只慢一帧的可见性, 不卡帧)。
	if (!g_ssrWDepQ)
	{
		D3D11_QUERY_DESC qd{};
		qd.Query = D3D11_QUERY_EVENT;
		const HRESULT hq = dev->CreateQuery(&qd, &g_ssrWDepQ);
		if (FAILED(hq) || !g_ssrWDepQ)
		{
			g_ssrWDepQ = nullptr;
			logLine("SSR侦察: [wdep] EVENT 查询建不出 " + hexHr(hq) + " → 水深无闸 (降级为 Flush)");
		}
	}
	if (g_ssrWDepQ && !g_ssrWDepQLive)
	{
		ctx->End(g_ssrWDepQ);
		g_ssrWDepQLive = true;
	}
	ctx->Flush(); // 尽早提交; 正式的等待在 renderer 侧 ssrInGateWait (零跨API栅栏设计不变)
	dev->Release();
}

static void STDMETHODCALLTYPE hookedOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n,
                                                       ID3D11RenderTargetView* const* ppRTV,
                                                       ID3D11DepthStencilView* pDSV)
{
	using Fn_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
	                                      ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
	SsrCtxEntry* e = lookupSsrCtx(ctx ? *reinterpret_cast<void***>(ctx) : nullptr);
	const Fn_t real = e ? reinterpret_cast<Fn_t>(e->orig33) : nullptr;
	if (!real) // 理论不可达: 只有被我们改过的槽才会进本函数
		return;
	// 延迟上下文与 immediate 共享 vtable —— 身份核对只观察 immediate (失配则转全观察)
	if (ssrWatch(e, ctx) && g_ssrOn.load(std::memory_order_relaxed))
		ssrReconOm(n, ppRTV, pDSV);
	real(ctx, n, ppRTV, pDSV);
	// ---- 帧时基线 (v0.16.2): 顺手在这条每帧必经的 ctx 钩上拿到 immediate context ----
	// 只认 e->imm == ctx (安装时记下的 immediate 指针), 延迟上下文不拿来建查询;
	// PoC-B 开着时 notePresent 已经能拿到 ctx, 这里是它关掉时的兜底。
	if (e && e->imm == ctx)
		ftNoteCtx(ctx);
	// ---- Step 2b 通路哨兵 (docs/05 §5 Step 2): 特征B(段16→段17) 判定后的回写 ----
	// 放在 real() **之后**: 此刻段17 的 {321,591} 已绑上, 585 恰好已解绑 —— CopyResource
	// 写一个非绑定资源最干净 (在 real() 之前写则 585 还是绑定中的 RT0)。段17 的 1 Draw
	// (ev39530) 尚未发生 ⇒ 它随后读到的 585 已是 324 的内容 = 可辨识的"旧场景色"错误画面。
	// 写目标 = g_ssrStrRes (段16 资源的活引用), 源 = g_ssrSceneRes (324 的活引用),
	// 两者都持 AddRef, 不依赖已被 Release 的裸指针值。
	if (g_ssrSentinelPending)
	{
		g_ssrSentinelPending = false;
		// 四重保险: 开关 + 两个活引用都在 + 写目标身份确为刚离开的强特征对象 (= g_ssrReflRt
		// 的本帧实例)。g_ssrStrResObj == g_ssrLastStr 这条防"GetResource 失败导致 strResObj
		// 残留旧值"的边角 —— 那种情况下 g_ssrStrRes 本来也是空, 但两处都查更稳。
		if (g_ssrSentinelOn.load(std::memory_order_relaxed) &&
		    !g_ssrVkOutOn.load(std::memory_order_relaxed) && g_ssrSceneRes && g_ssrStrRes &&
		    g_ssrStrResObj == g_ssrLastStr && g_ssrLastStr == g_ssrReflRt)
		{
			// desc 预检 + CopyResource + 节流日志 → copyResDescChecked (C-2/C-3)。
			// 那份函数的注释说明了为什么必须"先比 desc 再拷": CopyResource 返回 void,
			// desc 不一致时静默丢弃但日志照打 ⇒ 判读会误判成"哨兵没生效"。
			const long k = ++g_ssrSentN;
			const std::string pfx = "SSR侦察: [2b] 哨兵#" + std::to_string(k) +
			                        " 585=" + hexOf(g_ssrStrResObj) + " <- 324=" +
			                        hexOf(g_ssrSceneObj) + " 帧=" +
			                        std::to_string(g_presentCount.load(std::memory_order_relaxed) + 1);
			copyResDescChecked(ctx, g_ssrStrRes, g_ssrSceneRes, pfx, "585", "324", k);
		}
	}
	// ---- v0.18.7 B: 段16 刚画完水, 585 此刻**含水** ⇒ 在 2d 回写覆盖它之前抢一份当合成底色 ----
	// 位置是硬约束: 下面那条 CopyResource(585 ← 出向) 会把水覆盖掉, 排晚了就抢不到。
	// 一次性门借 g_ssrInPending (特征B 置位, 本函数末尾才消费) ⇒ 每帧恰好拷一次;
	// 身份三重判据与 2d 回写同一套 (g_ssrStrResObj == g_ssrLastStr == g_ssrReflRt)。
	// 时序安全: 这条拷贝排在 ssrInQueue 的 EVENT 闸 End **之前** ⇒ VK 同帧 Present 读底色时
	// 已被闸等完 (与 324/520 那两条同一口径, 零跨 API 栅栏的设计不变)。
	if (g_ssrBaseOn.load(std::memory_order_relaxed) &&
	    g_ssrVkOutOn.load(std::memory_order_relaxed) &&
	    g_ssrV1On.load(std::memory_order_relaxed) && g_ssrInPending && g_ssrBaseTex &&
	    g_ssrStrRes && g_ssrStrResObj == g_ssrLastStr && g_ssrLastStr == g_ssrReflRt)
	{
		const long kb = ++g_ssrBaseN;
		const std::string pfxB = "SSR侦察: [base] 底色#" + std::to_string(kb) +
		                         " 585=" + hexOf(g_ssrStrResObj) + " → 底色镜像=" +
		                         hexOf(g_ssrBaseTex) + " 帧=" +
		                         std::to_string(g_presentCount.load(std::memory_order_relaxed) + 1);
		copyResDescChecked(ctx, g_ssrBaseTex, g_ssrStrRes, pfxB, "底色", "585", kb);
	}
	// ---- v0.18.8 B: 段后水深 (第5张镜像) —— 特征B 之后第一次换绑时拷 (ssrReconOm 置的 fire) ----
	// 此刻段17 的水体并回 (depth 只写不测) 已排完 ⇒ 源 461 里是**真·水面深度**, 法线/反射原点
	// 由此改从它算; 520 快照继续当行进层级。放在 2d 回写/2c 入向之前无所谓先后 (只读源、只写我方镜像)。
	if (g_ssrWDepFire)
	{
		g_ssrWDepFire = false;
		if (g_ssrV1On.load(std::memory_order_relaxed) &&
		    g_ssrVkOutOn.load(std::memory_order_relaxed) &&
		    g_ssrSharedOn.load(std::memory_order_relaxed))
			ssrWDepQueue(ctx);
	}
	// ---- Step 2d-1 出向回写 (v0.18.0, docs/05 D3): VK 上一帧 Present 填好的结果 → 585 ----
	// 1 帧延迟: 本帧 Present 才由 VK 填出向镜像 ⇒ 这里回写的是**上一帧**的 SSR 结果。
	// 时机与 2b/2c 同位 —— 段17 的 Draw (ev39530) 尚未发生, 写进 585 正是它要读的那份。
	// **必须排在 2c 入向之前**: 读回自校验要拿"入向镜像里 VK 消费过的那帧"与"出向镜像"
	// 比 (v0 passthrough 下两者应逐字节相等), 而 2c 马上就会把入向镜像覆盖成本帧新内容。
	// 2d 开着时 2b 哨兵让位 ⇒ 585 的内容只可能来自 VK, 归因干净。
	// C-6 一次性门: g_ssrOutPending 由特征B 置位 (ssrReconOm 内), 此处才触发一次;
	// 后面的身份条件只是"还在段16 窗口内"的补充判据, **不能**单独当触发条件 (否则段16 的
	// 16 次 OMSet 重绑会让它每帧命中 28.7 次)。
	if (g_ssrVkOutOn.load(std::memory_order_relaxed) && g_ssrOutPending && g_ssrOutReady &&
	    g_ssrOutTexC && g_ssrStrRes && g_ssrStrResObj == g_ssrLastStr &&
	    g_ssrLastStr == g_ssrReflRt)
	{
		// 消费一次性门 (C-6): 走到这里 = 本帧特征B + 身份=585 都成立, 只允许写这一次。
		g_ssrOutPending = false;
		// desc 预检 + 拷 + 日志都在下方 copyResDescChecked (C-2/C-3), 与 2b 共用同一份。
		// 读回自校验 (节流 前3次+每600次, 同 2c): 入向=VK 消费的那帧, 出向=VK 写回的那份。
		// 只有**两个 STAGING 都在**才比; 阻塞读回, 绝不能每帧做 (docs/05 约束 2)。
		// C-7: 计数**进门先推进** (同 2c 的 `++g_ssrInLogN`)。原写法是 `cc = N+1`、
		// 只在打日志的分支里回填 `g_ssrOutChkN = cc` => cc 停在 4 永不前进、`cc%600` 永不命中,
		// "每600次采一次"从未发生 (v0.18.0 / v0.18.1 两轮 [2d] 出向读回 都恰好只有 3 行,
		// 后段帧从没被采过)。
		const long cc = ++g_ssrOutChkN;
		if ((g_ssrOutStg && g_ssrInStgC && g_ssrInTexC) && ((cc <= 3) || (cc % 600) == 0))
		{
			long nzi = 0, nzo = 0;
			const unsigned long long hIn =
			    ssrInFnv(ctx, g_ssrInTexC, g_ssrInStgC, "2d入向", &nzi);
			const unsigned long long hOut =
			    ssrInFnv(ctx, g_ssrOutTexC, g_ssrOutStg, "2d出向", &nzo);
			logLine("SSR侦察: [2d] 出向读回#" + std::to_string(cc) + " 入向=0x" + uhex64(hIn) +
			        " 出向=0x" + uhex64(hOut) +
			        (hIn != 0 && hIn == hOut
			             ? " **一致✓** (v0 passthrough 逐字节还原 = 出向通路成立)"
			             : " 不一致✗ (帧错位/行距/时序)") +
			        " 非零" + std::to_string(nzo) +
			        " 帧=" + std::to_string(g_presentCount.load(std::memory_order_relaxed) + 1));
		}
		const long k = ++g_ssrOutN;
		const std::string pfx = "SSR侦察: [2d] 回写#" + std::to_string(k) +
		                        " 585=" + hexOf(g_ssrStrResObj) + " ← 出向=" +
		                        hexOf(g_ssrOutTexC) + " 帧=" +
		                        std::to_string(g_presentCount.load(std::memory_order_relaxed) + 1);
		copyResDescChecked(ctx, g_ssrStrRes, g_ssrOutTexC, pfx, "585", "出向", k);
	}
	// ---- Step 2c 共享入向 (v0.15.0): 与 2b 同一触发点, 独立开关 ----
	// 放在 real() 之后的理由同 2b (此刻段17 已绑、585 已解绑) —— 入向拷贝不碰 585,
	// 只往我方 SHARED 镜像写, 但沿用同一时机, 2c-β 的 VK 读回与 2d 的回写都在这条时间线上。
	if (g_ssrInPending)
	{
		g_ssrInPending = false;
		if (g_ssrSharedOn.load(std::memory_order_relaxed) &&
		    g_ssrOn.load(std::memory_order_relaxed))
			ssrInQueue(ctx);
	}
}

static void STDMETHODCALLTYPE hookedClearRTV(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* pRTV,
                                             const FLOAT ColorRGBA[4])
{
	using Fn_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
	SsrCtxEntry* e = lookupSsrCtx(ctx ? *reinterpret_cast<void***>(ctx) : nullptr);
	const Fn_t real = e ? reinterpret_cast<Fn_t>(e->orig50) : nullptr;
	if (!real)
		return;
	if (ssrWatch(e, ctx) && g_ssrOn.load(std::memory_order_relaxed))
		ssrReconClear(pRTV);
	real(ctx, pRTV, ColorRGBA);
}

// 挂 immediate context 槽33/50 —— 幂等 (按 vtable 去重); 门控 = pocbEnabled + probe + ssr
static void installSsrRecon(ID3D11Device* dev)
{
	if (!dev || !pocbEnabled() || !g_probeOn.load(std::memory_order_relaxed) ||
	    !g_ssrOn.load(std::memory_order_relaxed))
	{
		// C-5 (待修复事项总结 C-5): 门不满足时**必须**说清是谁关的 —— 原来是一声不吭 return,
		// 而日志里那行 "ini ssr=1 → 挂 ctx 槽33/47/50" 只是**意图打印** (与门无关),
		// 看着像挂上了, 实际槽33/47/50 一个都没挂 ⇒ OM=0 → 入向=0 → run2c 一口气 10 条
		// FAIL 全是同一根因 (2026-10-06 曾白跑两轮 3600 + 8400 帧)。
		static std::atomic<bool> s_gateLogged{false};
		if (!s_gateLogged.exchange(true, std::memory_order_relaxed))
			logLine(std::string("SSR侦察: 挂载门未满足 — ctx槽33/50 未挂 (dev=") +
			        (dev ? "OK" : "空") + " vulkan=" + (pocbEnabled() ? "1" : "0") +
			        " probe=" + (g_probeOn.load(std::memory_order_relaxed) ? "1" : "0") +
			        " ssr=" + (g_ssrOn.load(std::memory_order_relaxed) ? "1" : "0") +
			        ") — 开关顺序 ssr / probe / vulkan (逃生门链 ssr=0 → probe=0 → vulkan=0)");
		return;
	}
	ID3D11DeviceContext* ctx = nullptr;
	dev->GetImmediateContext(&ctx);
	if (!ctx)
	{
		logLine("SSR侦察: GetImmediateContext 空 — 槽33/50 未挂");
		return;
	}
	void** vtbl = *reinterpret_cast<void***>(ctx);
	EnterCriticalSection(&g_cs);
	if (lookupSsrCtx(vtbl))
	{
		LeaveCriticalSection(&g_cs);
		ctx->Release();
		return;
	}
	void* o33 = vtbl[33];
	void* o50 = vtbl[50];
	void* o47 = vtbl[47]; // CopyResource (Step 2a 身份识别通道)
	logLine("SSR侦察: ctx vtable=" + hexOf(vtbl) + " slot33(OMSetRenderTargets) 原值=" + hexOf(o33) +
	        " 来自 " + modulePathOf(o33) + "; slot50(ClearRenderTargetView) 原值=" + hexOf(o50) +
	        " 来自 " + modulePathOf(o50) + "; slot47(CopyResource) 原值=" + hexOf(o47) +
	        " 来自 " + modulePathOf(o47));
	if (!isD3D11Family(o33) || !isD3D11Family(o50))
	{
		logLine("SSR侦察: ctx slot33/50 原值不在 d3d11/renderdoc — 跳过 (防错槽位)");
		LeaveCriticalSection(&g_cs);
		ctx->Release();
		return;
	}
	// slot47 校验失败只降级该槽 (不挂), 33/50 照常 —— 324 身份学不到则 2b 哨兵不排队,
	// 日志里看不到 [2a] 行即可判读; 绝不能挂一个 orig47 为空的槽 (hookedCopyResource 会
	// 直接 return 掉真调用 ⇒ 游戏拷贝全丢, 渲染崩)。
	const bool ok47 = isD3D11Family(o47);
	if (!ok47)
		logLine("SSR侦察: ctx slot47 原值不在 d3d11/renderdoc — 不挂 (身份识别降级, 槽33/50 照常)");
	if (g_ssrCtxN >= (int)(sizeof(g_ssrCtx) / sizeof(g_ssrCtx[0])))
	{
		logLine("SSR侦察: ctx 登记表已满 — 跳过 (防越界)");
		LeaveCriticalSection(&g_cs);
		ctx->Release();
		return;
	}
	g_ssrCtx[g_ssrCtxN].vtbl = vtbl;
	g_ssrCtx[g_ssrCtxN].imm = ctx; // 指针值作身份, 下面立即 Release 不持引用
	g_ssrCtx[g_ssrCtxN].orig33 = o33;
	g_ssrCtx[g_ssrCtxN].orig50 = o50;
	g_ssrCtx[g_ssrCtxN].orig47 = ok47 ? o47 : nullptr;
	++g_ssrCtxN;
	patchSlotLocked(&vtbl[33], reinterpret_cast<void*>(&hookedOMSetRenderTargets));
	patchSlotLocked(&vtbl[50], reinterpret_cast<void*>(&hookedClearRTV));
	if (ok47)
		patchSlotLocked(&vtbl[47], reinterpret_cast<void*>(&hookedCopyResource));
	logLine("SSR侦察: ctx槽33/50" + std::string(ok47 ? "/47" : "") +
	        " 已挂 — 2a 只记日志, 2b 回写需 ssr.sentinel=1 (v0.16.0 Step2; 逃生门 ssr=0)");
	LeaveCriticalSection(&g_cs);
	ctx->Release();
}

// notePresent 调: 帧末汇总 + 清零本帧状态。判据 (docs/05 §5 Step1; S4 api-scan 实测 +
// 2026-10-04 真机 4472 帧判读双向校准):
//   基线 = 同帧 distinct<=2 —— 反射段 与 主HDR 两个同签名强特征对象 (S4: 585 段16 与 321 段19;
//   D1 原假设"同帧唯一"被推翻, 真机复现 distinct 恒<=2) + 特征A/B 各恰 1 次。
//   异常帧恒打: 有候选却强特征 0 / 有强特征而 A+B==0 / A 单项>1 / B 单项>1 /
//   distinct>2; 前 5 帧恒打 (无水场景也能证明钩子活着), 有活动的每 60 帧心跳。
//   注意 "强特征=fs" 是**进入次数不是唯一性** (真机主HDR 有 2~3 次进入 ⇒ fs 恒 3~4): 旧规则
//   "fs>2" 按 S4 单帧真值定, 真机一跑刷出 2571 条假阳性把日志冲到 1MB, 已删 ——
//   唯一性只看 distinct (真机恒<=2), 反射段定位只看 objRuns (1 vs 4~6)。
//   加载/过场 (无候选无强特征) 不算异常, 免得刷屏。
// 判别子: 每个强特征对象另打一行"本帧 RT0 绑定次数/连续段数" —— S4 实测 585=16次/1段、
//   321=50次/4段; 真机复现 反射段=10~19次/1段、主HDR=10~370次/4~6段, 零重叠。量只差 3 倍
//   不可靠, 判别取"1 段 vs 多段"(反射目标整帧只写一次), 且只在强特征对象集合内有效
//   (全帧另有 339 = 17次/1段)。
static void ssrReconPresent(uint64_t n)
{
	if (!g_ssrOn.load(std::memory_order_relaxed))
		return;
	const long fo = g_ssrFOm.exchange(0, std::memory_order_relaxed);
	const long fc = g_ssrFCand.exchange(0, std::memory_order_relaxed);
	const long fs = g_ssrFStr.exchange(0, std::memory_order_relaxed);
	const long fa = g_ssrFA.exchange(0, std::memory_order_relaxed);
	const long fb = g_ssrFB.exchange(0, std::memory_order_relaxed);
	// 先把本帧强特征对象 + 各自 RT0 绑定次数/连续段数取到本地, 再立刻清帧状态 (表不跨帧)
	const int objN = g_ssrFObjN;
	void* objs[4];
	long  objBind[4];
	long  objRuns[4];
	for (int i = 0; i < objN; ++i)
	{
		objs[i] = g_ssrFObj[i];
		objBind[i] = 0;
		objRuns[i] = 0;
		for (int j = 0; j < g_ssrRtNN; ++j)
			if (g_ssrRtN[j].obj == objs[i])
			{
				objBind[i] = g_ssrRtN[j].n;
				objRuns[i] = g_ssrRtN[j].runs;
				break;
			}
	}
	// ---- Step 2a 身份落定 (docs/05 D2a): 判别子 runs 一出来就按它分派585/321 ----
	//   runs==1 ⇒ 反射目标 585 (段16 整帧只写一次), runs>1 ⇒ 主 HDR 321 (4~6 段)。
	//   帧间持久不复位: 下一帧 ssrReconCopy 见 src==321 就认 324, 特征B 见"刚离开的强特征
	//   对象==585" 才排队回写。资源换代后新对象在本帧末才会顶上 ⇒ 有一帧延迟, 无妨。
	for (int i = 0; i < objN; ++i)
	{
		if (objRuns[i] == 1)
			g_ssrReflRt = objs[i];
		else if (objRuns[i] > 1)
			g_ssrMainHdr = objs[i];
	}
	g_ssrFObjN = 0;
	g_ssrRtNN = 0;
	g_ssrRtNOver = 0;
	g_ssrPrevRt0 = nullptr;
	g_ssrLastStr = nullptr;
	g_ssrInStrong = false;
	g_ssrLastCand = nullptr;
	g_ssrLastCandStr = false;
	g_ssrAFired = g_ssrBFired = false;
	const long copyN = g_ssrCopyN;
	g_ssrCopyN = 0;
	g_ssrSentinelPending = false; // 兜底: 上一帧没被 real() 消费掉的标记不带进新帧
	g_ssrOutPending = false;      // 同上 (C-6: 2d 出向一次性门)
	g_ssrInPending = false;       // 同上 (2c 入向标记)
	const bool act = fc > 0 || fs > 0 || fa > 0 || fb > 0;
	// 真机判读 (2026-10-04, 4472 帧): 2571 条 [异常] 全部来自旧规则 "fs>2" —— fs 是"进入次数"
	// 不是"唯一性", S4 只有单帧真值 fs=2, 真机主 HDR 有 2~3 次强特征进入 ⇒ fs=3~4 恒成立。
	// 唯一性看 objN (真机恒<=2), 反射段定位看 objRuns (1 vs 4~6), 两者都正常 ⇒ 删掉 fs>2。
	// 其余五条判据真机一次都没触发 (fs 最小=1, objN 最大=2, A、B 均恒<=1)。
	const bool anom = (fs == 0 && fc > 0) || (fs > 0 && (fa + fb) == 0) || fa > 1 || fb > 1 ||
	                  objN > 2;
	if (n <= 5 || (n % 60 == 0 && act) || anom)
	{
		std::string s = "SSR侦察: 帧=" + std::to_string(n) + " OM=" + std::to_string(fo) +
		                " 候选=" + std::to_string(fc) + " 强特征=" + std::to_string(fs) +
		                " distinct=" + std::to_string(objN) + " 特征A=" + std::to_string(fa) +
		                " 特征B=" + std::to_string(fb) + " COPY=" + std::to_string(copyN) +
		                " 哨兵=" + std::to_string(g_ssrSentN) +
		                " 入向=" + std::to_string(g_ssrInN) +
		                " 出向=" + std::to_string(g_ssrOutN) + " | 累计候选=" +
		                std::to_string(g_ssrCandN) + " 累计强特征=" + std::to_string(g_ssrStrTot);
		if (anom)
			s += " [异常]";
		logLine(s);
		for (int i = 0; i < objN; ++i)
			logLine("SSR侦察: 帧=" + std::to_string(n) + " 强特征对象" + std::to_string(i + 1) +
			        "=" + hexOf(objs[i]) + " 本帧RT0绑定=" + std::to_string(objBind[i]) +
			        "次/" + std::to_string(objRuns[i]) + "个连续段" +
			        (objs[i] == g_ssrReflRt ? " [585]" : "") +
			        (objs[i] == g_ssrMainHdr ? " [321]" : ""));
	}
}

// 挂设备 vtable 槽5 —— 幂等 (按 vtable 去重), 首次调用读 ini
void installProbeOn(ID3D11Device* dev)
{
	if (!dev || !pocbEnabled())
		return;
	void** vtbl = *reinterpret_cast<void***>(dev);
	EnterCriticalSection(&g_cs);
	static bool probeIniRead = false;
	if (!probeIniRead)
	{
		probeIniRead = true;
		g_probeOn.store(iniFlag("probe", true), std::memory_order_relaxed);
		if (!g_probeOn.load(std::memory_order_relaxed))
			logLine("探针升质: ini probe=0 → 不挂 CreateTexture2D (探针保持 512²)");
		// SSR Step1 侦察 (D5): 默认 0 → 零新增拦截; 1 才挂 ctx 槽33/47/50 (与 probe 同门)
		g_ssrOn.store(iniFlag("ssr", false), std::memory_order_relaxed);
		if (g_ssrOn.load(std::memory_order_relaxed))
			logLine("SSR侦察: ini ssr=1 → 挂 ctx 槽33/47/50 (2a 只记日志) v0.16.0 Step2");
		// Step 2b 通路哨兵的独立逃生门 —— 首次会动渲染 (324→585 回写), 与"只观察"分开:
		// ssr=1 但 ssr.sentinel=0 ⇒ 纯观察, 2a 身份照样学; 出问题改 0 即退回只读。
		g_ssrSentinelOn.store(iniFlag("ssr.sentinel", false), std::memory_order_relaxed);
		if (g_ssrSentinelOn.load(std::memory_order_relaxed) &&
		    g_ssrOn.load(std::memory_order_relaxed))
			logLine("SSR侦察: ini ssr.sentinel=1 → 特征B 处 324→585 回写 (2b 哨兵, 反射区"
			        " 会显示成可辨识的旧场景色错误画面; 逃生门 ssr.sentinel=0)");
		else if (g_ssrSentinelOn.load(std::memory_order_relaxed))
			logLine("SSR侦察: ini ssr.sentinel=1 但 ssr=0 → 哨兵不生效 (ssr 是总门)");
		// Step 2c 共享入向的独立开关 (v0.15.0) —— 默认 0 → 与 v0.14.0 的 2b 路径完全一致。
		// 1 才建 SHARED|NTHANDLE 镜像并每帧拷一次; **只写我方镜像, 不动游戏资源、不改画面**
		// (与 2b 的"改 585"性质不同, 所以另给一道门; 逃生门思路同 D5)。
		g_ssrSharedOn.store(iniFlag("ssr.shared", false), std::memory_order_relaxed);
		if (g_ssrSharedOn.load(std::memory_order_relaxed) &&
		    g_ssrOn.load(std::memory_order_relaxed))
			logLine("SSR侦察: ini ssr.shared=1 → 2c 共享入向: 324/520 拷进 SHARED|NTHANDLE "
			        "镜像 + CreateSharedHandle (R2 验证; 只写镜像不改画面; 逃生门 "
			        "ssr.shared=0)");
		else if (g_ssrSharedOn.load(std::memory_order_relaxed))
			logLine("SSR侦察: ini ssr.shared=1 但 ssr=0 → 入向不生效 (ssr 是总门)");
		// Step 2d-1 出向回写 (v0.18.0) —— **会动渲染** (VK 结果拷进 585), 所以再给一道独立门。
		// 开着时 2b 哨兵让位 ⇒ 585 内容只可能来自 VK; 逃生门改 0 即退回"不动 585"的 2c 形态。
		// 1 帧延迟: 本帧 Present 由 VK 填出向镜像, **下一帧**特征B 才拷进 585 (零跨 API 栅栏)。
		g_ssrVkOutOn.store(iniFlag("ssr.vkout", false), std::memory_order_relaxed);
		if (g_ssrVkOutOn.load(std::memory_order_relaxed) &&
		    g_ssrSharedOn.load(std::memory_order_relaxed) &&
		    g_ssrOn.load(std::memory_order_relaxed))
			logLine("SSR侦察: ini ssr.vkout=1 → 2d 出向回写: VK(读324镜像)→出向镜像→"
			        " 下帧特征B CopyResource(出向→585); 2b 哨兵让位; 反射区显示上一帧"
			        " 场景色错误画面 (与 2b 同观感, 验通路; 逃生门 ssr.vkout=0)");
		else if (g_ssrVkOutOn.load(std::memory_order_relaxed))
			logLine("SSR侦察: ini ssr.vkout=1 但 ssr.shared=0 或 ssr=0 → 出向不生效 "
			        "(需要 ssr=1 + ssr.shared=1 是总门)");
		// ---- SSR v1 shader 采样 (v0.18.6): 出向内容从"原样拷"换成"跑一次全屏三角" ----
		g_ssrV1On.store(iniFlag("ssr.v1", false), std::memory_order_relaxed);
		g_ssrV1Mode = static_cast<int>(iniNum("ssr.mode", 1.0));
		g_ssrV1Fov = static_cast<float>(iniNum("ssr.fov", 65.0));
		g_ssrV1Near = static_cast<float>(iniNum("ssr.near", 10.0));
		g_ssrV1Far = static_cast<float>(iniNum("ssr.far", 100000.0));
		g_ssrV1Steps = static_cast<float>(iniNum("ssr.steps", 32.0));
		g_ssrV1Strength = static_cast<float>(iniNum("ssr.strength", 1.0));
		g_ssrV1Dist = static_cast<float>(iniNum("ssr.dist", 500.0));
		g_ssrV1Rev = static_cast<float>(iniNum("ssr.rev", 0.0));
		// ---- v0.18.7 A 平滑批 + B 底色门 (破碎倒影 / 水色缺失 两个观感问题的开关) ----
		g_ssrBaseOn.store(iniFlag("ssr.base585", true), std::memory_order_relaxed);
		g_ssrV1Smooth = static_cast<int>(iniNum("ssr.smooth", 4.0));
		g_ssrV1Blur = static_cast<int>(iniNum("ssr.blur", 1.0));
		g_ssrV1Debug = static_cast<int>(iniNum("ssr.debug", 0.0));
		// ---- v0.18.9: 涟漪回注量 (治"有倒影处水面变平面/流动波纹消失") ----
		g_ssrV1Ripple = static_cast<float>(iniNum("ssr.ripple", 1.0));
		// ---- v0.18.10: 回注带宽 (治"波纹非常细小") + 回注方式 (亮度调制 / 位移扭曲) ----
		g_ssrV1RippleSz = static_cast<int>(iniNum("ssr.ripplesz", 4.0));
		g_ssrV1RippleMode = static_cast<int>(iniNum("ssr.ripplemode", 1.0));
		// ---- v0.18.11: 未命中的回退源 (治"扇形范围内是错误的倒影") ----
		// v0.18.16 (issue B): 扩到 2 = 未命中采真 cubemap。钳位必须在**入口**做, 因为
		// ssrV1Sig() / push constant p4.w / 着色器分支三方都只读 g_ssrV1Edge 这一个变量 ——
		// 越界值 (手滑写 7) 会走进着色器里"既不是 0/1 也不是 2"的分支, 结果不可复现。
		{
			const int ev = static_cast<int>(iniNum("ssr.edge", 0.0));
			g_ssrV1Edge = (ev < 0) ? 0 : ((ev > 2) ? 2 : ev);
			if (g_ssrV1Edge != ev)
				logLine("ssr.edge = " + std::to_string(ev) + " 越界 -> 钳到 " +
				        std::to_string(g_ssrV1Edge) + " (合法值 0/1/2)");
			else if (g_ssrV1Edge == 2)
				logLine("ssr.edge = 2 (v0.18.16 未命中采真 cubemap) — 探针数据就绪前会先绑 1×1 占位图");
		}
		// ---- v0.18.14: 回注三旋钮 (软限幅 / 位移幅度 / 梯度增益), 默认 0 = 关或自动 ----
		g_ssrV1RipK = static_cast<float>(iniNum("ssr.ripk", 0.0));
		g_ssrV1RipAmp = static_cast<float>(iniNum("ssr.ripamp", 0.0));
		g_ssrV1RipGain = static_cast<float>(iniNum("ssr.ripgain", 0.0));
		// ---- v0.18.15: 原版高光回补量 (0 = 关 = v0.18.14 行为) ----
		g_ssrV1Det = static_cast<float>(iniNum("ssr.v1det", 0.0));
		// ---- v0.18.8 正解B: 段后水深当法线/原点输入 (只关自己) ----
		g_ssrWDepOn.store(iniFlag("ssr.wdep", true), std::memory_order_relaxed);
		if (g_ssrV1Near < 0.01f || g_ssrV1Far <= g_ssrV1Near)
		{
			logLine("SSR侦察: ssr.near/ssr.far 不合法 (" + std::to_string(g_ssrV1Near) +
			        " / " + std::to_string(g_ssrV1Far) + ") → 回退 10 / 100000");
			g_ssrV1Near = 10.0f;
			g_ssrV1Far = 100000.0f;
		}
		if (g_ssrV1Steps < 4.0f) // 着色器 stepLen = maxT / steps, 0 会除零
		{
			logLine("SSR侦察: ssr.steps 不合法 (" + std::to_string(g_ssrV1Steps) + ") → 回退 32");
			g_ssrV1Steps = 32.0f;
		}
		if (g_ssrV1Dist < 1.0f)
		{
			logLine("SSR侦察: ssr.dist 不合法 (" + std::to_string(g_ssrV1Dist) + ") → 回退 500");
			g_ssrV1Dist = 500.0f;
		}
		if (g_ssrV1Rev != 0.0f && g_ssrV1Rev != 1.0f)
			g_ssrV1Rev = g_ssrV1Rev > 0.5f ? 1.0f : 0.0f;
		// v0.18.9: strength 语义 = SSR 替换比 ⇒ 上限由 4 收到 1 (老 ini 写 >1 的会被夹到 1)
		if (g_ssrV1Strength < 0.0f)
			g_ssrV1Strength = 0.0f;
		if (g_ssrV1Strength > 1.0f)
			g_ssrV1Strength = 1.0f;
		// v0.18.9: 涟漪回注量夹 0..1 (0 = 关, 1 = 全量)
		if (g_ssrV1Ripple < 0.0f)
			g_ssrV1Ripple = 0.0f;
		if (g_ssrV1Ripple > 1.0f)
			g_ssrV1Ripple = 1.0f;
		if (g_ssrV1Smooth < 1 || g_ssrV1Smooth > 16)
		{
			logLine("SSR侦察: ssr.smooth 不合法 (" + std::to_string(g_ssrV1Smooth) +
			        ") → 回退 4 (邻域 1..16 px)");
			g_ssrV1Smooth = 4;
		}
		if (g_ssrV1Blur != 0)
			g_ssrV1Blur = 1;
		if (g_ssrV1Debug < 0 || g_ssrV1Debug > 8)
		{
			logLine("SSR侦察: ssr.debug 不合法 (" + std::to_string(g_ssrV1Debug) +
			        ") → 回退 0 (0 正常/1 法线/2 命中/3 深度/4 段后水深/5 水面像素/"
			        "6 回注可视化/7 uBase 原样)");
			g_ssrV1Debug = 0;
		}
		// v0.18.10: 回注带宽 1..16 px, 回注方式 0/1; v0.18.11: 未命中回退源 0/1/2 (2 = v0.18.16 采真 cubemap)
		if (g_ssrV1RippleSz < 1 || g_ssrV1RippleSz > 16)
		{
			logLine("SSR侦察: ssr.ripplesz 不合法 (" + std::to_string(g_ssrV1RippleSz) +
			        ") → 回退 4 (1..16 px)");
			g_ssrV1RippleSz = 4;
		}
		if (g_ssrV1RippleMode != 0)
			g_ssrV1RippleMode = 1;
		// v0.18.16b 定案: ssr.edge 现在是 0/1/2 三档 (0 = 未命中回原版层 / 1 = 屏幕边缘延展 /
		// 2 = 未命中采真 cubemap)。这里原本是 v0.18.11 的老钳位「非 0 一律钳成 1」—— 那时只有
		// 两档, P2 加档位 2 时忘了放开这道钳, 于是 2 被**静默**改成 1 而且不打日志:
		//   · ssrV1CubeSync 的 `g_ssrV1Edge >= 2` 闸永假 => 真 cube 永远不上传
		//     (日志全程搜不到「真 cube 上传完成」, 也搜不到 ssrV1EffEdge 的「还没就绪」)
		//   · pc.p4.w 恒 = 1 => shader 走 v0.18.11 的边缘延展 refl = texture(uColor, ruv),
		//     采的是**屏幕自己**而不是 uProbe
		// 实拍坐实 (v0.18.16a, 零代码三证): ① ini 回显 ssr.edge=2 但下一行生效值 edge=1;
		// ② 转 180 度后倒影内容跟着世界变 (屏幕空间才会这样, 世界 cube 不会);
		// ③ 左右窄带逐位不变 = lastUV == uv 回原版层 —— 正是用户报的那小块没变的水面。
		// => 改成按 0..2 钳, 只有真越界才回退, 并补日志 (老钳位是无声的)。
		if (g_ssrV1Edge < 0 || g_ssrV1Edge > 2)
		{
			logLine("SSR侦察: ssr.edge 不合法 (" + std::to_string(g_ssrV1Edge) +
			        ") → 回退 1 (0 回原版层 / 1 屏幕边缘延展 / 2 采真 cubemap)");
			g_ssrV1Edge = 1;
		}
		// v0.18.14: 三个旋钮 0 = 关/自动, 只夹负值与离谱上限 (ripk 实机扫 0.02~0.4, 上限给 4)
		if (g_ssrV1RipK < 0.0f)
			g_ssrV1RipK = 0.0f;
		if (g_ssrV1RipK > 4.0f)
			g_ssrV1RipK = 4.0f;
		if (g_ssrV1RipAmp < 0.0f)
			g_ssrV1RipAmp = 0.0f;
		if (g_ssrV1RipAmp > 64.0f)
			g_ssrV1RipAmp = 64.0f;
		if (g_ssrV1RipGain < 0.0f)
			g_ssrV1RipGain = 0.0f;
		if (g_ssrV1RipGain > 64.0f)
			g_ssrV1RipGain = 64.0f;
		if (g_ssrV1Det < 0.0f)
			g_ssrV1Det = 0.0f;
		if (g_ssrV1Det > 1.0f)
			g_ssrV1Det = 1.0f;
		if (g_ssrV1On.load(std::memory_order_relaxed) &&
		    g_ssrVkOutOn.load(std::memory_order_relaxed) &&
		    g_ssrSharedOn.load(std::memory_order_relaxed) &&
		    g_ssrOn.load(std::memory_order_relaxed))
			logLine("SSR侦察: ini ssr.v1=1 → 出向内容改由 VK 全屏三角产出 (采 324+520, "
			        "视图空间 ray march) mode=" + std::to_string(g_ssrV1Mode) +
			        " fov=" + std::to_string(g_ssrV1Fov) + " near=" +
			        std::to_string(g_ssrV1Near) + " far=" + std::to_string(g_ssrV1Far) +
			        " steps=" + std::to_string(static_cast<int>(g_ssrV1Steps)) +
			        " dist=" + std::to_string(static_cast<int>(g_ssrV1Dist)) +
			        " strength=" + std::to_string(g_ssrV1Strength) +
			        " rev=" + std::to_string(static_cast<int>(g_ssrV1Rev)) +
			        " smooth=" + std::to_string(g_ssrV1Smooth) +
			        " blur=" + std::to_string(g_ssrV1Blur) +
			        " debug=" + std::to_string(g_ssrV1Debug) + " base585=" +
			        std::to_string(g_ssrBaseOn.load(std::memory_order_relaxed) ? 1 : 0) +
			        " wdep=" +
			        std::to_string(g_ssrWDepOn.load(std::memory_order_relaxed) ? 1 : 0) +
			        " ripple=" + std::to_string(g_ssrV1Ripple) +
			        " ripplesz=" + std::to_string(g_ssrV1RippleSz) +
			        " ripplemode=" + std::to_string(g_ssrV1RippleMode) +
			        " edge=" + std::to_string(g_ssrV1Edge) +
			        " ripk=" + std::to_string(g_ssrV1RipK) +
			        " ripamp=" + std::to_string(g_ssrV1RipAmp) +
			        " ripgain=" + std::to_string(g_ssrV1RipGain) +
			        " v1det=" + std::to_string(g_ssrV1Det) +
			        "; strength 现在是 SSR 替换比(0=原版); dist 单位 = near; 逃生门 ssr.v1=0 "
			        "(R4 = 反推 inv(投影), 倒影位置/比例不对先调 fov, 深度反向先试 ssr.rev=1)");
		else if (g_ssrV1On.load(std::memory_order_relaxed))
			logLine("SSR侦察: ini ssr.v1=1 但 ssr/ssr.shared/ssr.vkout 有没开的 → v1 不生效 "
			        "(需要 ssr=1 + ssr.shared=1 + ssr.vkout=1)");
	}
	if (!g_probeOn.load(std::memory_order_relaxed))
	{
		LeaveCriticalSection(&g_cs);
		return;
	}
	for (int i = 0; i < g_devN; ++i)
		if (g_dev[i].vtbl == vtbl)
		{
			LeaveCriticalSection(&g_cs);
			return;
		}
	void* o5 = vtbl[5];
	logLine("探针升质: 设备 vtable=" + hexOf(vtbl) + " slot5(CreateTexture2D) 原值=" + hexOf(o5) +
	        " 来自 " + modulePathOf(o5));
	if (!isD3D11Family(o5))
	{
		logLine("探针升质: slot5 原值不在 d3d11/renderdoc — 跳过该 vtable (防错槽位)");
		LeaveCriticalSection(&g_cs);
		return;
	}
	if (g_devN >= (int)(sizeof(g_dev) / sizeof(g_dev[0])))
	{
		logLine("探针升质: 设备登记表已满 — 跳过 (防越界)");
		LeaveCriticalSection(&g_cs);
		return;
	}
	g_dev[g_devN].vtbl = vtbl;
	g_dev[g_devN].orig5 = o5;
	++g_devN;
	patchSlotLocked(&vtbl[5], reinterpret_cast<void*>(&hookedCreateTexture2D));
	logLine("探针升质: 槽5 已挂 (512² cube+配对depth→1024² + 回写desc, 逃生门 probe=0 / vulkan=0)");
	LeaveCriticalSection(&g_cs);
	installCtxProbe(dev); // v0.12 plan-B: 同设备 immediate context 槽44/45 (自身幂等)
	installSsrRecon(dev); // v0.14.0 Step1/2a: SSR 侦察+身份 ctx 槽33/47/50 (幂等, 门控 ssr=1)
}

// 由 registerSwp (交换链一出现) / pocbInit (双保险) 调 —— 必须早于游戏创建探针
static void installProbeHook(void* swpObj)
{
	if (!swpObj)
		return;
	ID3D11Device* dev = nullptr;
	reinterpret_cast<IDXGISwapChain*>(swpObj)->GetDevice(__uuidof(ID3D11Device),
	                                                     reinterpret_cast<void**>(&dev));
	if (!dev)
		return;
	installProbeOn(dev);
	dev->Release();
}

void registerSwp(void* obj, bool plus, const char* tag)
{
	if (!obj)
		return;
	installProbeHook(obj); // 探针升质 (v0.10.0): 交换链一登记就抓设备 —— 必须早于游戏创建探针
	EnterCriticalSection(&g_cs);
	void** vtbl = *reinterpret_cast<void***>(obj);
	int idx = -1;
	for (int i = 0; i < g_swpN; ++i)
		if (g_swp[i].vtbl == vtbl)
		{
			idx = i;
			break;
		}
	if (idx < 0)
	{
		void* o8 = vtbl[8];
		logLine(std::string(tag) + ": vtable=" + hexOf(vtbl) + " 来自 " + modulePathOf(vtbl));
		logLine(std::string(tag) + ": slot8 原值=" + hexOf(o8) + " 来自 " + modulePathOf(o8));
		if (!isDxgiFamily(o8))
		{
			logLine(std::string(tag) + ": slot8 原值不在 dxgi/renderdoc — 跳过该 vtable");
			LeaveCriticalSection(&g_cs);
			return;
		}
		if (g_swpN >= (int)(sizeof(g_swp) / sizeof(g_swp[0])))
		{
			logLine(std::string(tag) + ": 交换链登记表已满 (" + std::to_string(g_swpN) +
			        ") — 跳过该 vtable (防越界)");
			LeaveCriticalSection(&g_cs);
			return;
		}
		idx = g_swpN++;
		g_swp[idx].vtbl = vtbl;
		g_swp[idx].orig8 = o8;
		g_swp[idx].orig22 = nullptr;
		g_swp[idx].has22 = false;
	}
	else
	{
		// 匹配路径原先是静默的 — v1.4 补日志: 游戏交换链若与 dummy 同类, 此行即证据
		logLine(std::string(tag) + ": vtable 已登记 (与既有同类) " + hexOf(vtbl));
	}
	if (plus && !g_swp[idx].has22)
	{
		// 只有确认实现 1 及以上接口的 vtable 才安全读槽 22 (布局至少 23 项)
		void* o22 = vtbl[22];
		logLine(std::string(tag) + ": slot22 原值=" + hexOf(o22) + " 来自 " + modulePathOf(o22));
		if (isDxgiFamily(o22))
		{
			g_swp[idx].orig22 = o22;
			g_swp[idx].has22 = true;
		}
		else
		{
			logLine(std::string(tag) + ": slot22 原值不在 dxgi/renderdoc — 不挂 Present1");
		}
	}
	if (!g_vtableLayer)
	{
		// v0.9.2 vtable=0: 只登记不挂槽 —— 让 Present 只能经方案B 的函数级 detour 进来
		logLine(std::string(tag) + ": vtable=0 (ini) → 不挂槽 8/22, 本 vtable 交由方案B 覆盖");
	}
	else
	{
		patchSlotLocked(&vtbl[8], reinterpret_cast<void*>(&hookedPresent));
		if (g_swp[idx].has22)
			patchSlotLocked(&vtbl[22], reinterpret_cast<void*>(&hookedPresent1));
	}
	LeaveCriticalSection(&g_cs);
}

// ---------- 工厂 vtable 登记 + 挂交换链创建方法 ----------

void tryFacSlot(void** vtbl, int slot, void** origField, void* hookFn, const char* nm, const char* tag)
{
	if (*origField)
		return;
	void* cur = vtbl[slot];
	logLine(std::string(tag) + ": slot" + std::to_string(slot) + " " + nm +
	        " 原值=" + hexOf(cur) + " 来自 " + modulePathOf(cur));
	if (!isDxgiFamily(cur))
	{
		logLine(std::string(tag) + ": slot" + std::to_string(slot) + " 原值不在 dxgi/renderdoc — 跳过");
		return;
	}
	*origField = cur;
	patchSlotLocked(&vtbl[slot], hookFn);
}

void registerFac(void* obj, bool plus2, const char* tag)
{
	if (!obj)
		return;
	EnterCriticalSection(&g_cs);
	void** vtbl = *reinterpret_cast<void***>(obj);
	int idx = -1;
	for (int i = 0; i < g_facN; ++i)
		if (g_fac[i].vtbl == vtbl)
		{
			idx = i;
			break;
		}
	if (idx < 0)
	{
		if (g_facN >= (int)(sizeof(g_fac) / sizeof(g_fac[0])))
		{
			logLine(std::string(tag) + ": 工厂登记表已满 (" + std::to_string(g_facN) +
			        ") — 跳过该 vtable (防越界)");
			LeaveCriticalSection(&g_cs);
			return;
		}
		idx = g_facN++;
		g_fac[idx].vtbl = vtbl;
		g_fac[idx].o10 = nullptr;
		g_fac[idx].o15 = nullptr;
		g_fac[idx].o16 = nullptr;
		g_fac[idx].o24 = nullptr;
		logLine(std::string(tag) + ": factory vtable=" + hexOf(vtbl) + " 来自 " + modulePathOf(vtbl));
	}
	FacEntry& e = g_fac[idx];
	tryFacSlot(vtbl, 10, &e.o10, reinterpret_cast<void*>(&hookedCreateSwapChain), "CreateSwapChain", tag);
	if (plus2)  // 槽 15/16/24 只存在于 Factory2 布局 (末项=24), 防越界
	{
		tryFacSlot(vtbl, 15, &e.o15, reinterpret_cast<void*>(&hookedCreateSwapChainForHwnd),
		           "CreateSwapChainForHwnd", tag);
		tryFacSlot(vtbl, 16, &e.o16, reinterpret_cast<void*>(&hookedCreateSwapChainForCoreWindow),
		           "CreateSwapChainForCoreWindow", tag);
		tryFacSlot(vtbl, 24, &e.o24, reinterpret_cast<void*>(&hookedCreateSwapChainForComposition),
		           "CreateSwapChainForComposition", tag);
	}
	LeaveCriticalSection(&g_cs);
}

// ---------- Present 计数与日志 ----------

const char* swapFxName(UINT e)
{
	switch (e)
	{
	case 0: return "DISCARD";
	case 1: return "SEQUENTIAL";
	case 2: return "FLIP_SEQUENTIAL";
	case 3: return "FLIP_DISCARD";
	default: return "?";
	}
}

std::string hexBytes(const void* addr, int n)
{
	const unsigned char* p = static_cast<const unsigned char*>(addr);
	std::string s;
	char b[8];
	for (int i = 0; i < n; ++i)
	{
		std::snprintf(b, sizeof(b), "%02X ", p[i]);
		s += b;
	}
	return s;
}

// 对象级探针读内存前的存活校验 (游戏交换链可能已被释放, 防悬垂指针崩溃)
bool memReadable(const void* p, size_t n)
{
	if (!p)
		return false;
	MEMORY_BASIC_INFORMATION mbi{};
	if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
		return false;
	if (mbi.State != MEM_COMMIT)
		return false;
	DWORD prot = mbi.Protect & 0xFF;
	if (prot == PAGE_NOACCESS || prot == 0)
		return false;
	if (mbi.Protect & PAGE_GUARD)
		return false;
	const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
	return reinterpret_cast<uintptr_t>(p) + n <= begin + mbi.RegionSize;
}

static DWORD g_enumPid = 0;
BOOL CALLBACK enumWndLog(HWND hw, LPARAM)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hw, &pid);
	if (pid != g_enumPid)
		return TRUE;
	char cls[128]{}, ttl[256]{};
	GetClassNameA(hw, cls, sizeof(cls));
	GetWindowTextA(hw, ttl, sizeof(ttl));
	logLine(std::string("  窗口: hwnd=") + hexOf(static_cast<const void*>(hw)) +
	        " visible=" + (IsWindowVisible(hw) ? "1" : "0") +
	        " class=\"" + cls + "\" title=\"" + ttl + "\"");
	return TRUE;
}

// v1.5 探针 W: 本进程顶层窗口 — 找可能漏钩的第二个交换链的宿主窗口
void logProcessWindows(const std::string& tag)
{
	g_enumPid = GetCurrentProcessId();
	logLine("窗口枚举 (" + tag + "):");
	EnumWindows(enumWndLog, 0);
}

std::string swapDesc(IDXGISwapChain* sc)
{
	DXGI_SWAP_CHAIN_DESC d{};
	if (FAILED(sc->GetDesc(&d)))   // GetDesc=槽12 未挂钩, 可安全直调
		return "GetDesc 失败";
	char title[128]{};
	if (d.OutputWindow)
		GetWindowTextA(d.OutputWindow, title, sizeof(title));
	char buf[320];
	std::snprintf(buf, sizeof(buf),
	    "%ux%u format=%d swapEffect=%s(%u) buffers=%u sample=%u windowed=%d window=%p title=\"%s\"",
	    d.BufferDesc.Width, d.BufferDesc.Height, static_cast<int>(d.BufferDesc.Format),
	    swapFxName(d.SwapEffect), d.SwapEffect, d.BufferCount, d.SampleDesc.Count,
	    static_cast<int>(d.Windowed), static_cast<void*>(d.OutputWindow), title);
	return buf;
}

// ========== 帧时基线 (v0.16.2, docs/00 6.5 方案A / docs/05 R3) ==========
// 谁要这份数据:
//   1) 阶段2 与 SSR Step4 的过闸判据一直是"帧时不劣于基线 5%" (docs/00:96, docs/05:386),
//      但基线数从来没实测过 —— 闸没有数可比;
//   2) 后续 compute culling + indirect draw 要按帧预算调剔除力度/meshlet 阈值/间接 draw
//      上限, 没有帧时分布就只能拍脑袋;
//   3) 6.5 动态帧预算(超标帧压缩脚本预算)的数据源就是它。
// 口径 (两条分开, 语义不同, 别混用):
//   CPU = Present-to-Present 帧间隔 (steady_clock), 每帧 1 样本;
//   GPU = D3D11 TIMESTAMP + TIMESTAMP_DISJOINT, 在 Present 处 End(ts0) 开窗、下一次 Present
//         End(ts1) 收窗, 时间差 = 一帧的 GPU 提交跨度。查询结果一律 DONOTFLUSH 非阻塞读,
//         没就绪就跳过本帧绝不等 —— 观测通路自己不能变成卡顿源。D3D11 规定同一时刻只能有
//         一个 disjoint 活跃, 故三条(Disj/Ts0/Ts1)全拿到才开下一窗 => GPU 样本约帧数的一半。
// 逃生门: ini frametime=0 (默认 1); 拿不到 immediate context (PoC-B/探针/ssr 全关) 只记 CPU。
static double g_ftCpu[600];
static int g_ftCpuN = 0;
static double g_ftGpu[600];
static int g_ftGpuN = 0;
static uint64_t g_ftCpuAll = 0, g_ftGpuAll = 0;
static double g_ftCpuSumAll = 0.0, g_ftGpuSumAll = 0.0;
static double g_ftCpuMaxAll = 0.0, g_ftGpuMaxAll = 0.0;
static uint64_t g_ftHitch = 0; // 单帧间隔 >= 1s (读盘/切场景), 不进分布只计数
static std::chrono::steady_clock::time_point g_ftLast{};
static bool g_ftHas = false;
static bool g_ftOn = true;
static bool g_ftIniRead = false;
static ID3D11DeviceContext* g_ftCtx = nullptr; // 裸指针只作调用入口 (immediate context 与设备同寿)
static ID3D11Query* g_ftDisj = nullptr;
static ID3D11Query* g_ftTs0 = nullptr;
static ID3D11Query* g_ftTs1 = nullptr;
static bool g_ftQFail = false; // 建查询失败 => 本局只记 CPU
static int g_ftState = 0;      // 0=没窗 1=ts0 已 End(开窗) 2=ts1 已 End(等三条就绪)

// 对 a 就地排序后给均值/中位/95分位/最大值 (窗口 600 个, 每 600 帧一次, 忽略不计)
static void ftStats(double* a, int n, double& mean, double& p50, double& p95, double& mx)
{
	mean = p50 = p95 = mx = 0.0;
	if (n <= 0)
		return;
	std::sort(a, a + n);
	double s = 0.0;
	for (int i = 0; i < n; ++i)
		s += a[i];
	mean = s / n;
	p50 = a[n / 2];
	p95 = a[(n - 1) * 95 / 100];
	mx = a[n - 1];
}

// 建三只查询, 幂等; 任一步失败 => g_ftQFail (日志只打一次)
static void ftMakeQueries(ID3D11DeviceContext* ctx)
{
	if (g_ftQFail || g_ftDisj || !ctx)
		return;
	ID3D11Device* dev = nullptr;
	ctx->GetDevice(&dev);
	if (!dev)
	{
		g_ftQFail = true;
		return;
	}
	// D3D11 的 CreateQuery 只吃 (DESC*, ppQuery) —— 查询类型写在 desc.Query 里 (D3D10 才是三参)
	D3D11_QUERY_DESC qd{};
	qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
	HRESULT hr = dev->CreateQuery(&qd, &g_ftDisj);
	if (SUCCEEDED(hr))
	{
		qd.Query = D3D11_QUERY_TIMESTAMP;
		hr = dev->CreateQuery(&qd, &g_ftTs0);
	}
	if (SUCCEEDED(hr))
		hr = dev->CreateQuery(&qd, &g_ftTs1);
	dev->Release();
	if (FAILED(hr) || !g_ftDisj || !g_ftTs0 || !g_ftTs1)
	{
		// 半成品必须放掉: ftMakeQueries 靠 g_ftDisj 判重, 留着半只会让本局再也建不起来
		if (g_ftDisj)
		{
			g_ftDisj->Release();
			g_ftDisj = nullptr;
		}
		if (g_ftTs0)
		{
			g_ftTs0->Release();
			g_ftTs0 = nullptr;
		}
		if (g_ftTs1)
		{
			g_ftTs1->Release();
			g_ftTs1 = nullptr;
		}
		g_ftQFail = true;
		logLine("帧时基线: D3D11 时间戳查询创建失败 " + hexHr(hr) + " -> 本局只记 CPU 侧");
		return;
	}
	g_ftCtx = ctx;
	logLine("帧时基线: GPU 时间戳查询就绪 (TIMESTAMP+DISJOINT), 每帧打点, 结果非阻塞读");
}

// 每次 Present 调: 开窗 / 收窗 / 读结果, 全程非阻塞
static void ftGpuPresent()
{
	if (!g_ftOn || g_ftQFail || !g_ftCtx)
		return;
	if (g_ftState == 0)
	{
		g_ftCtx->Begin(g_ftDisj);
		g_ftCtx->End(g_ftTs0);
		g_ftState = 1;
		return;
	}
	if (g_ftState == 1)
	{
		g_ftCtx->End(g_ftTs1);
		g_ftCtx->End(g_ftDisj);
		g_ftState = 2;
		return;
	}
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
	UINT64 t0 = 0, t1 = 0;
	const UINT fl = D3D11_ASYNC_GETDATA_DONOTFLUSH;
	if (g_ftCtx->GetData(g_ftDisj, &dj, sizeof(dj), fl) != S_OK ||
	    g_ftCtx->GetData(g_ftTs0, &t0, sizeof(t0), fl) != S_OK ||
	    g_ftCtx->GetData(g_ftTs1, &t1, sizeof(t1), fl) != S_OK)
		return; // GPU 还没跑到这里 -> 下帧再试, 绝不阻塞
	if (!dj.Disjoint && dj.Frequency && t1 > t0)
	{
		const double ms = static_cast<double>(t1 - t0) * 1000.0 /
		                  static_cast<double>(dj.Frequency);
		if (ms > 0.0 && ms < 1000.0 && g_ftGpuN < 600)
		{
			g_ftGpu[g_ftGpuN++] = ms;
			++g_ftGpuAll;
			g_ftGpuSumAll += ms;
			if (ms > g_ftGpuMaxAll)
				g_ftGpuMaxAll = ms;
		}
	}
	g_ftCtx->Begin(g_ftDisj);
	g_ftCtx->End(g_ftTs0);
	g_ftState = 1;
}

// 由 ctx 钩子回调: 只做"顺手拿 immediate context 建查询", 不碰任何渲染
static void ftNoteCtx(ID3D11DeviceContext* ctx)
{
	if (g_ftOn && !g_ftQFail && !g_ftDisj)
		ftMakeQueries(ctx);
}

static ID3D11DeviceContext* ftPocbCtx(); // 定义在 PocbCtx/g_pocb 之后 (本块在它们之前)

// ==== v0.18.16 P1 (issue B / 队列 #2): 水体探针 cube 的 CPU 可读性 + 静态性探测 ====
// 回答 C1 路线 (D3D11 STAGING 读回 -> CPU -> 灌自建 VkImage) 的两个前提:
//   ① STAGING 能不能 Map —— 不能 ⇒ 整条 CPU 中转路线作废, 直接转 Plan B (C5 假环境);
//   ② 相隔 >=5s 的两次读回 FNV 是否一致 —— 一致 ⇒ 内容静态, 一次上传就够; 不一致 ⇒ 要按代重传
//      (ssrV1Sig() 已把 g_probeCpuTick 算进签名, 换代会自动重录 + 重填描述符)。
// 只跑两次, 由 notePresent 每帧喂, 两次之后每帧只落一次早退比较 = 零开销。
// **不碰画面**: 只 CopyResource + Map + Unmap, 与 slot5/44/45 的 viewport 重写无关,
// 也不改变 hit/miss 分裂 ⇒ §14.31 §9/§10 的判据不受影响。
//
// v0.18.16c 第二层根因 (12/12 会话时间线坐实, 见 docs/02 §14): 读回闸原来挂 g_probeCubeT
//   (建纹理时刻), 但建纹理发生在过加载屏时, 游戏第一次往探针里画要晚 4~24s ⇒ 两次读回
//   全落在「一次都没画过」的窗口里, 读到的是刚建出来的裸 0 (sig 恒 = 全 0 反算值
//   13856201724594908557), 之前的 P1「内容静态/C1 成立」是假阳性 (FNV 对全 0 也给常数)。
//   修法两件套: ① 闸改挂 g_probeCubeRenderT (hookedRSSetViewports 首次命中 = 正在往探针里画)
//              ② 全 0 不算数: 不发布不计数, 1s 节流重试 (上限 15 次), 每次打每面非0像素数
//                 与量级最大值 —— 首渲信号有无直接分诊 (a) 时机 / (b) 拷贝没落地。
static int                  g_probeDumpN = 0; // 已成功(非0)读回次数 (0..2)
static int                  g_probeDumpZeroN = 0; // v0.18.16c: 读到全 0 的次数 (重试闸)
static unsigned long long   g_probeDumpT = 0; // 第一次成功读回的时刻 (第二次要 >= +5s)
static unsigned long long   g_probeDumpTryT = 0; // v0.18.16c: 上次全 0 读回时刻 (1s 节流)
static ID3D11Texture2D*     g_probeDumpCube = nullptr;
static ID3D11Texture2D*     g_probeDumpStg = nullptr; // STAGING 镜像 (复用, 换探针才重建)
static bool                 g_probeDumpDescLg = false;
static unsigned long long   g_probeDumpH0 = 0; // 第一次读回的总签名

static unsigned long long fnv1a64(const void* p, size_t n, unsigned long long h)
{
	const unsigned char* b = static_cast<const unsigned char*>(p);
	for (size_t i = 0; i < n; ++i)
	{
		h ^= static_cast<unsigned long long>(b[i]);
		h *= 1099511628211ULL;
	}
	return h;
}

static void probeDumpTick()
{
	if (g_probeDumpN >= 2)
		return;
	ID3D11Texture2D* cube = g_probeCube.load(std::memory_order_acquire);
	if (!cube)
		return;
	if (cube != g_probeDumpCube)
	{ // 探针换过 (换区/换分辨率) ⇒ 上一对作废, 重新配对
		g_probeDumpCube = cube;
		g_probeDumpN = 0;
		g_probeDumpZeroN = 0;
		g_probeDumpT = 0;
		g_probeDumpTryT = 0;
		g_probeDumpDescLg = false;
		if (g_probeDumpStg) { g_probeDumpStg->Release(); g_probeDumpStg = nullptr; }
		if (g_probeCpu) { free(g_probeCpu); g_probeCpu = nullptr; }
		g_probeW = g_probeH = g_probeLayers = 0;
	}
	const unsigned long long now = GetTickCount64();
	if (g_probeDumpZeroN && now - g_probeDumpTryT < 1000)
		return; // 上一次读回六面全 0 ⇒ 1s 节流后重试 (两种读都盖住, 免得每帧白拷 48MB)
	if (g_probeDumpN == 0)
	{
		// v0.18.16c: 读回闸挂「首渲」而非「建纹理」—— 12/12 会话实测建纹理比游戏第一次往
		// 探针里画早 4~24s (建在过加载屏时), 老闸 (+2s/+7s) 两次读回全落在一次都没画过的
		// 窗口里 ⇒ 读到刚建出来的裸 0, sig 恒 = 全 0 反算值 13856201724594908557,
		// 之前的「内容静态/C1 成立」是假阳性 (FNV 对全 0 也给常数)。
		if (g_probeCubeRenderT == 0)
		{
			// 还没见首渲信号 (hookedRSSetViewports 未命中) ⇒ 最多等 60s
			if (g_probeCubeT && now - g_probeCubeT < 60000)
				return;
			if (!g_probeRenderWaitLg)
			{
				g_probeRenderWaitLg = true;
				logLine("P1探针: 建好 60s 仍未见首渲信号 (RSSetViewports 钩没命中?) ⇒ 兜底按建纹理时刻直读");
			}
		}
		else if (now - g_probeCubeRenderT < 2000)
			return; // 首渲之后再等 2s, 让这一帧的内容落定
	}
	else if (now - g_probeDumpT < 5000)
		return;

	D3D11_TEXTURE2D_DESC d{};
	cube->GetDesc(&d);
	ID3D11Device* dev = nullptr;
	cube->GetDevice(&dev);
	ID3D11DeviceContext* ctx = nullptr;
	if (dev)
		dev->GetImmediateContext(&ctx);
	if (!ctx)
	{
		logLine("P1探针: 拿不到 immediate context, 放弃第 " + std::to_string(g_probeDumpN + 1) + " 次读回");
		if (dev)
			dev->Release();
		return;
	}

	if (!g_probeDumpDescLg)
	{
		g_probeDumpDescLg = true;
		const bool fmtOk = (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
		logLine("P1探针: desc W=" + std::to_string(d.Width) + " H=" + std::to_string(d.Height) +
		        " fmt=" + std::to_string(static_cast<int>(d.Format)) + "(RGBA16F=" + std::to_string(fmtOk ? 1 : 0) +
		        ") mips=" + std::to_string(d.MipLevels) + " array=" + std::to_string(d.ArraySize) +
		        " sample=" + std::to_string(d.SampleDesc.Count) + " bind=" + std::to_string(d.BindFlags) +
		        " misc=" + std::to_string(d.MiscFlags) + " cpuAccess=" + std::to_string(d.CPUAccessFlags));
		// C1 的 VkImage 必须与它 1:1 (R16G16B16A16_SFLOAT / 6 面 / 非 mips), 不合就提前说清楚
		if (!(fmtOk && d.ArraySize == 6 && d.MipLevels == 1 && d.Width == d.Height))
			logLine("P1探针: desc 不满足 C1 的 1:1 前提 (需 RGBA16F + array=6 + mips=1 + 方形) ⇒ 读回照做, 但不发布给 VK");
	}

	// --- STAGING 镜像 (一次性) ---
	if (!g_probeDumpStg)
	{
		D3D11_TEXTURE2D_DESC sd = d;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = d.MiscFlags; // 先按原样带 TEXTURECUBE
		HRESULT hr = dev ? dev->CreateTexture2D(&sd, nullptr, &g_probeDumpStg) : E_FAIL;
		if (FAILED(hr))
		{
			sd.MiscFlags = 0; // 部分实现的 staging 不认 TEXTURECUBE ⇒ 退回普通数组
			hr = dev ? dev->CreateTexture2D(&sd, nullptr, &g_probeDumpStg) : E_FAIL;
			logLine("P1探针: staging 带 TEXTURECUBE 失败(hr=" + std::to_string(static_cast<long>(hr)) +
			        "), 退回 MiscFlags=0 再试");
		}
		if (FAILED(hr) || !g_probeDumpStg)
		{
			logLine("P1探针: STAGING CreateTexture2D 失败(hr=" +
			        std::to_string(static_cast<long>(hr)) + ") ⇒ C1 不可行, 转 Plan B (C5 假环境)");
			g_probeDumpN = 2; // 停止重试
			if (dev)
				dev->Release();
			ctx->Release();
			return;
		}
		logLine("P1探针: STAGING CreateTexture2D ok (MiscFlags=" + std::to_string(sd.MiscFlags) + ")");
	}

	const UINT layers = d.ArraySize;
	const UINT mips = d.MipLevels;
	const bool bpp8 = (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
	const bool pubOk = (bpp8 && layers == 6 && mips == 1 && d.Width == d.Height);
	if (g_probeDumpN == 0 && pubOk && !g_probeCpu)
	{
		const size_t sz = (size_t)6 * (size_t)d.Width * (size_t)d.Height * 8;
		g_probeCpu = static_cast<unsigned char*>(malloc(sz));
		if (!g_probeCpu)
			logLine("P1探针: 48MiB CPU 缓冲申请失败 ⇒ 只读不发布 (C1 判失败)");
	}

	ctx->CopyResource(g_probeDumpStg, cube);

	// ---- v0.18.16c 阶段1: 逐面扫描 (诊断件) ----
	// 每面给 (非0像素数 / 最大量级位型)。整行先做 uint64 OR 快路径, 全 0 行免逐像素;
	// 六面全 0 ⇒ 不进阶段2 (免白跑 48MB FNV) 也不发布, 1s 节流重试。
	// 量级取 raw & 0x7FFF: 半浮点符号-阶码-尾数的该位段对 |值| 单调 ⇒ 免转 float 就能看大小。
	unsigned long long faceNz[6] = {0, 0, 0, 0, 0, 0};
	unsigned int       faceMax[6] = {0, 0, 0, 0, 0, 0};
	bool anyData = false;
	if (bpp8)
	{
		const UINT m = 0; // 只扫 mip0 (mips=1 时即全分辨率)
		for (UINT L = 0; L < layers && L < 6; ++L)
		{
			const UINT sub = m + mips * L;
			D3D11_MAPPED_SUBRESOURCE mr{};
			if (FAILED(ctx->Map(g_probeDumpStg, sub, D3D11_MAP_READ, 0, &mr)))
				continue; // 阶段2 再 Map 会报同一个错, 不在这里重复喊
			UINT w = d.Width >> m;
			UINT h = d.Height >> m;
			if (!w) w = 1;
			if (!h) h = 1;
			for (UINT y = 0; y < h; ++y)
			{
				const unsigned char* row =
				    static_cast<const unsigned char*>(mr.pData) + (size_t)y * mr.RowPitch;
				const size_t rowBytes = (size_t)w * 8;
				// 快路径: 整行 8 字节步进 OR, 全 0 直接下一行 (零内容的重试只花这一档钱)
				unsigned long long rowOr = 0;
				const unsigned long long* q64 = reinterpret_cast<const unsigned long long*>(row);
				for (size_t i = 0; i < rowBytes / 8; ++i)
					rowOr |= q64[i];
				if (!rowOr)
					continue;
				anyData = true;
				const unsigned short* u = reinterpret_cast<const unsigned short*>(row);
				const size_t n16 = rowBytes / 2;
				for (size_t i = 0; i + 4 <= n16; i += 4)
				{
					if (u[i] | u[i + 1] | u[i + 2] | u[i + 3])
						++faceNz[L];
					for (int k = 0; k < 4; ++k)
					{
						const unsigned int mag = (unsigned int)u[i + k] & 0x7FFFu;
						if (mag > faceMax[L])
							faceMax[L] = mag;
					}
				}
			}
			ctx->Unmap(g_probeDumpStg, sub);
		}
	}
	std::string fstat;
	for (UINT L = 0; L < layers && L < 6; ++L)
	{
		if (L)
			fstat += " ";
		fstat += "f" + std::to_string(L) + "=" + std::to_string(faceNz[L]) + "/" +
		         std::to_string(faceMax[L]);
	}
	if (bpp8 && !anyData)
	{
		// 全 0: 首渲信号是 (a)/(b) 的分水岭 —— 有信号仍全 0 ⇒ CopyResource 静默丢 (b);
		// 无信号 (60s 兜底直读) ⇒ 游戏压根没画过 (a)。
		++g_probeDumpZeroN;
		g_probeDumpTryT = now;
		logLine("P1探针: 第 " + std::to_string(g_probeDumpZeroN) +
		        " 次读回六面全 0 (首渲信号=" + (g_probeCubeRenderT ? std::string("有") : std::string("无")) +
		        ") ⇒ " + (g_probeCubeRenderT ? "有信号仍全0, 指向拷贝没落地" : "没画过, 指向时机") +
		        ", 1s 后重试; 面统计 nz/量级max " + fstat);
		if (g_probeDumpZeroN >= 15)
		{
			logLine("P1探针: 连续 15 次读回全 0 ⇒ 放弃 (C1 判失败, 转 Plan B (C5 假环境)); 面统计 " + fstat);
			g_probeDumpN = 2; // 停止重试
		}
		ctx->Release();
		if (dev)
			dev->Release();
		return;
	}
	logLine("P1探针: 面统计 nz/量级max " + fstat);

	// ---- 阶段2: 全 6 面 FNV 签名 + 收进 CPU 缓冲 (原有逻辑, 只在有内容时才跑) ----
	unsigned long long acc = 14695981039346656037ULL;
	bool allOk = true;
	for (UINT L = 0; L < layers && allOk; ++L)
	{
		unsigned long long lh = 14695981039346656037ULL;
		for (UINT m = 0; m < mips; ++m)
		{
			const UINT sub = m + mips * L; // D3D11CalcSubresource
			D3D11_MAPPED_SUBRESOURCE mr{};
			const HRESULT hr = ctx->Map(g_probeDumpStg, sub, D3D11_MAP_READ, 0, &mr);
			if (FAILED(hr))
			{
				allOk = false;
				logLine("P1探针: Map(sub=" + std::to_string(sub) + ") hr=" +
				        std::to_string(static_cast<long>(hr)) + " ⇒ 读回失败");
				break;
			}
			UINT w = d.Width >> m;
			UINT h = d.Height >> m;
			if (!w) w = 1;
			if (!h) h = 1;
			if (bpp8)
			{
				const size_t rowBytes = (size_t)w * 8;
				for (UINT y = 0; y < h; ++y)
				{
					const unsigned char* row =
					    static_cast<const unsigned char*>(mr.pData) + (size_t)y * mr.RowPitch;
					lh = fnv1a64(row, rowBytes, lh); // 逐行取 rowBytes, 跳过 RowPitch 填充 (填充内容不保证稳定)
					// 只把 mip0 收进 CPU 缓冲 —— 自建 VkImage 本来就是 mips=1, mip0 即全分辨率
					if (m == 0 && g_probeCpu)
						memcpy(g_probeCpu + (size_t)L * d.Width * d.Height * 8 + (size_t)y * rowBytes,
						       row, rowBytes);
				}
			}
			else
				lh = fnv1a64(mr.pData, (size_t)mr.RowPitch * h, lh);
			ctx->Unmap(g_probeDumpStg, sub);
		}
		acc = fnv1a64(&lh, sizeof lh, acc);
	}

	if (!allOk)
	{
		logLine("P1探针: 读回失败 ⇒ C1 不可行, 转 Plan B (C5 假环境)");
		g_probeDumpN = 2;
		if (g_probeCpu) { free(g_probeCpu); g_probeCpu = nullptr; }
		g_probeW = g_probeH = g_probeLayers = 0;
		ctx->Release();
		if (dev)
			dev->Release();
		return;
	}

	++g_probeDumpN;
	if (g_probeDumpN == 1)
	{
		g_probeDumpT = now;
		g_probeDumpH0 = acc;
		g_probeDumpZeroN = 0; // 零内容连败清零 —— 这次有内容了, 1s 节流随之解除
		logLine("P1探针: 第1次读回 ok  sig=" + std::to_string(acc) +
		        " (Map 全 6 面/全 mip 通过 ⇒ STAGING 可读)");
		if (g_probeCpu)
		{
			g_probeW = static_cast<int>(d.Width);
			g_probeH = static_cast<int>(d.Height);
			g_probeLayers = 6;
			++g_probeCpuTick; // 发布: VK 侧看到 tick 变就上传 (ssrV1Sig 会跟着换)
			logLine("P1探针: 已发布 CPU 镜像 " + std::to_string(g_probeW) + "² × 6 面, 第 " +
			        std::to_string(g_probeCpuTick) + " 代");
		}
	}
	else
	{
		const bool same = (acc == g_probeDumpH0);
		logLine("P1探针: 第2次读回 ok  sig=" + std::to_string(acc) + " 与第1次相隔 " +
		        std::to_string((unsigned long long)((now - g_probeDumpT) / 1000)) + "s ⇒ " +
		        (same ? "内容静态 (一次上传即可, C1 成立)" : "内容有变 (需按代重传, ssrV1Sig 已兜住)"));
		if (!same && g_probeCpu)
			++g_probeCpuTick; // 用新内容覆盖 CPU 缓冲 (循环里已经 memcpy 过了)
	}
	ctx->Release();
	if (dev)
		dev->Release();
}

void notePresent(const char* via, IDXGISwapChain* sc)
{
	const uint64_t n = g_presentCount.fetch_add(1) + 1;
	ssrReconPresent(n); // v0.13.0 Step1: 帧末汇总 (ssr=0 时内部早退, 零开销)
	// ---- v0.18.8 B: 帧末对齐 —— 正常帧里 fire 早在"段18 换绑"处就消费完了, 两条都是 false;
	// 段17 出现过却没有后续换绑 (特殊帧/段17 缺席) 时把 arm 丢掉, 免得下一帧开头拿**上一帧**
	// 的水深去拷 —— 那时源 461 已被 UI 的 ClearDS 清掉或被本帧重画覆盖 (拷回来会被着色器的
	// zW>zPre 判据拒掉, 不会算错, 但白拷一次还误导日志)。
	g_ssrWDepArm = false;
	g_ssrWDepFire = false;
	// ---- v0.18.16 P1: 探针 cube 的 CPU 可读性/静态性探测 (跑满 2 次后每帧只落一个早退) ----
	// 放在帧末 = 不插进游戏的记录流中间, Map 也不会卡在某个 draw 之间。
	probeDumpTick();
	// ---- 帧时基线 (v0.16.2): 每帧一次, 采样点就放在 Present 这里 ----
	if (!g_ftIniRead)
	{
		g_ftIniRead = true;
		g_ftOn = iniFlag("frametime", true);
		if (!g_ftOn)
			logLine("帧时基线: ini frametime=0 -> 不采样不出统计");
	}
	if (g_ftOn)
	{
		const auto now = std::chrono::steady_clock::now();
		if (g_ftHas)
		{
			const double ms =
			    std::chrono::duration<double, std::milli>(now - g_ftLast).count();
			if (ms >= 1000.0)
				++g_ftHitch; // 读盘/切场景的长间隔, 不是帧时 -> 只计数不进分布
			else if (ms > 0.0 && g_ftCpuN < 600)
			{
				g_ftCpu[g_ftCpuN++] = ms;
				++g_ftCpuAll;
				g_ftCpuSumAll += ms;
				if (ms > g_ftCpuMaxAll)
					g_ftCpuMaxAll = ms;
			}
		}
		g_ftLast = now;
		g_ftHas = true;
		ID3D11DeviceContext* fc = g_ftCtx;
		if (!fc)
			fc = ftPocbCtx(); // PoC-B 已 init 就有 immediate context (最常见路径)
		if (fc && !g_ftQFail && !g_ftDisj)
			ftMakeQueries(fc);
		if (g_ftDisj)
			ftGpuPresent();
	}
	if (n == 1)
	{
		g_lastLog = std::chrono::steady_clock::now();
		g_lastLogCount = 1;
		void** vtbl = *reinterpret_cast<void***>(sc);
		logLine(std::string("第 1 次 Present (经 ") + via + "): vtable=" + hexOf(vtbl) +
		        " 来自 " + modulePathOf(vtbl) + " | " + swapDesc(sc));
	}
	else if (n - g_lastLogCount >= 600)
	{
		const auto now = std::chrono::steady_clock::now();
		const double sec = std::chrono::duration<double>(now - g_lastLog).count();
		const double fps = sec > 0 ? static_cast<double>(n - g_lastLogCount) / sec : 0.0;
		char buf[96];
		std::snprintf(buf, sizeof(buf), "Present 计数 %llu  近600帧 %.1f FPS",
		    static_cast<unsigned long long>(n), fps);
		logLine(buf);
		// ---- 帧时基线 (v0.16.2): 与上面那行同频出分布统计, 两行分开免得破既有匹配 ----
		if (g_ftOn && (g_ftCpuN > 0 || g_ftGpuN > 0))
		{
			int over = 0;
			for (int i = 0; i < g_ftCpuN; ++i)
				if (g_ftCpu[i] > 20.0)
					++over; // 必须在排序前数 (ftStats 会就地排序)
			double cMean = 0, cP50 = 0, cP95 = 0, cMax = 0;
			double gMean = 0, gP50 = 0, gP95 = 0, gMax = 0;
			const int cN = g_ftCpuN, gN = g_ftGpuN;
			ftStats(g_ftCpu, cN, cMean, cP50, cP95, cMax);
			ftStats(g_ftGpu, gN, gMean, gP50, gP95, gMax);
			g_ftCpuN = 0;
			g_ftGpuN = 0;
			char gpu[200];
			if (gN > 0)
				std::snprintf(gpu, sizeof(gpu),
				              "GPU: 均值 %.2f 中位 %.2f p95 %.2f 最大 %.2f ms (样本 %d)",
				              gMean, gP50, gP95, gMax, gN);
			else
				std::snprintf(gpu, sizeof(gpu), "GPU: 无样本 (ctx 没拿到或查询未就绪)");
			char buf2[512];
			std::snprintf(buf2, sizeof(buf2),
			              "帧时基线 CPU: 均值 %.2f 中位 %.2f p95 %.2f 最大 %.2f ms >20ms=%d/%d"
			              " | %s | 会话 CPU n=%llu 均值 %.2f 最大 %.2f / GPU n=%llu 均值 %.2f"
			              " 最大 %.2f | 丢弃>=1s %llu",
			              cMean, cP50, cP95, cMax, over, cN, gpu,
			              static_cast<unsigned long long>(g_ftCpuAll),
			              g_ftCpuAll ? g_ftCpuSumAll / static_cast<double>(g_ftCpuAll) : 0.0,
			              g_ftCpuMaxAll, static_cast<unsigned long long>(g_ftGpuAll),
			              g_ftGpuAll ? g_ftGpuSumAll / static_cast<double>(g_ftGpuAll) : 0.0,
			              g_ftGpuMaxAll, static_cast<unsigned long long>(g_ftHitch));
			logLine(buf2);
		}
		g_lastLog = now;
		g_lastLogCount = n;
	}
}

// ---------- v1.7 方案A: renderdoc 转发桩解包取真交换链 ----------
// renderdoc 包装桩是动态转发存根 (v1.6 实测 48 8B 49 10 / 48 8B 01 / 48 FF 60 20 =
// 每次调用重取真对象, 不缓存 FP)。同好静态分析: 包装桩取真对象的偏移 — v0 系方法
// (GetDesc/ResizeTarget/GetFrameStatistics/GetLastPresent) 为 [obj+0x28],
// idx4 桩为 [obj+0x10] ⇒ 包装对象 +0x28 (首选) / +0x10 (备选) 处存着真对象指针。
// 解出真对象后读它的 vtbl[8]/vtbl[22] 就是 dxgi 真 Present/Present1 (版本无关且覆盖真类)。
// 安全性: 只对 vtable 落在 renderdoc.dll 的包装类对象尝试 (真 dxgi 对象上这些偏移是
// 内部字段, 直接跳过); 全程 memReadable 兜底; 解出的值只作方案B 的候选靶, 最终由
// 可执行页 + 14B 前导字节守卫把关 — 解错也不会写任何东西。
// 返回真对象指针 (由调用方读 vtbl[8]/[22]), 失败返回 nullptr。
void* unwrapRealSwapChain(void* obj, const char* tag)
{
	const std::string tg = std::string("方案A(") + tag + ")";
	if (!obj || !memReadable(obj, 0x30 + sizeof(void*)))
	{
		logLine(tg + ": 对象不可读 — 跳过");
		return nullptr;
	}
	void** vtbl = *reinterpret_cast<void***>(obj);
	if (!memReadable(vtbl, sizeof(void*)))
		return nullptr;
	if (lowerCopy(modulePathOf(vtbl)).find("renderdoc.dll") == std::string::npos)
	{
		logLine(tg + ": 对象 " + hexOf(obj) + " vtable 不在 renderdoc 包装类 — 这些偏移不适用, 跳过");
		return nullptr;
	}
	// 桩自曝偏移: 0x28 (v0 系方法) 首选, 0x10 (idx4 桩) 备选
	static const int kOffsets[2] = { 0x28, 0x10 };
	for (int k = 0; k < 2; ++k)
	{
		void* real = nullptr;
		memcpy(&real, static_cast<char*>(obj) + kOffsets[k], sizeof(void*));
		logLine(tg + ": 包装对象 " + hexOf(obj) + " vtbl=" + hexOf(vtbl) +
		        " [obj+0x" + (kOffsets[k] == 0x28 ? "28" : "10") + "]=" + hexOf(real));
		if (!real || !memReadable(real, sizeof(void*)))
		{
			logLine(tg + ": 该偏移不是可读指针 — 换下一偏移");
			continue;
		}
		void** rvt = *reinterpret_cast<void***>(real);
		if (!memReadable(rvt, 23 * sizeof(void*)))  // 至少 SwapChain1 布局 (槽22) 才完整
		{
			logLine(tg + ": 该偏移处对象的 vtable 不可读/不足 23 项 — 换下一偏移");
			continue;
		}
		logLine(tg + ": 真对象=" + hexOf(real) + " (经 +0x" + (kOffsets[k] == 0x28 ? "28" : "10") +
		        ") vtbl=" + hexOf(rvt) + " 来自 " + modulePathOf(rvt) +
		        " | 真vtbl[8]=" + hexOf(rvt[8]) + " 来自 " + modulePathOf(rvt[8]) +
		        " | 真vtbl[22]=" + hexOf(rvt[22]) + " 来自 " + modulePathOf(rvt[22]));
		return real;
	}
	logLine(tg + ": 两个候选偏移都解不出可用真对象 — 放弃");
	return nullptr;
}

// ---------- v1.6 方案B: 真 Present/Present1 函数级 detour ----------
// 目标 = 真 Present/Present1 函数入口 (v1.7: 优先活交换链 vtbl[8]/vtbl[22] 运行时直读,
// 其次方案A 解包真对象的 vtbl[8]/[22], 最后回退本机 dxgi RVA 0x19000/0x194A0;
// 前导 14B 字节双校验防漂移)。任何调用路径 — 类 vtable 虚调用、GFE 包装直调、renderdoc
// 包装层动态转发 — 最终都落进这个函数入口, 计数必然发生。
//
// 入口只写 5 字节 E9 rel32 → 近端跳板 (±2GB 内分配, 存 FF25 绝对跳转到钩子):
//   - 只碰前 5 字节 → 即便第三方 (GFE/renderdoc) 先前的 detour 只窃取了 5~N 字节,
//     其 trampoline 续接区 (t+5 起) 保持原样, 不会踩坏;
//   - 钩子函数可能离 dxgi 超过 ±2GB (模块高位分布), 故不能直接 E9, 必须近端中转;
//   - 入口已是他人跳板 → CHAIN 模式: 不建 trampoline, 钩子计数后把原跳板目标当作
//     resume 直接转发, 与既有 detour 链式共存。
// trampoline (RAW) = 窃取的原字节 + FF25 绝对跳回 target+steal, 分配在任意位置。

struct CodeDetour
{
	void* target = nullptr;
	void* resume = nullptr;  // RAW: trampoline; CHAIN: 第三方跳板目标
	bool active = false;
};

static CodeDetour g_detP, g_detP1;
// vtable 钩子转调原函数期间置位 → 函数钩子识别为"已被 vtable 层计过数", 只转发
static thread_local bool g_inVtableHook = false;

static bool isExecutablePage(const void* p)
{
	MEMORY_BASIC_INFORMATION mbi{};
	if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
		return false;
	const DWORD prot = mbi.Protect & 0xFF;
	return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
	       prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

// 5 字节 E9 rel32; 目标超出 ±2GB 时返回 false (不写)
static bool writeRelJump(void* at, void* to)
{
	const intptr_t rel = reinterpret_cast<const unsigned char*>(to) -
	                     (reinterpret_cast<const unsigned char*>(at) + 5);
	if (rel < INT32_MIN || rel > INT32_MAX)
		return false;
	DWORD old = 0;
	if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	unsigned char b[5] = { 0xE9, 0, 0, 0, 0 };
	const int32_t r32 = static_cast<int32_t>(rel);
	memcpy(b + 1, &r32, 4);
	memcpy(at, b, 5);
	DWORD tmp = 0;
	VirtualProtect(at, 5, old, &tmp);
	FlushInstructionCache(GetCurrentProcess(), at, 5);
	return true;
}

// 在 target ±64MB 内 (64KB 粒度) 找空闲页, 存放 FF25 → hook 的 14 字节跳板
static void* allocNearCode(void* target, size_t size)
{
	const uintptr_t t = reinterpret_cast<uintptr_t>(target);
	const uintptr_t base = t & ~uintptr_t(0xFFFF);
	static const uintptr_t kSteps[] = {
		0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000,
		0x400000, 0x800000, 0x1000000, 0x2000000, 0x4000000 };
	for (size_t i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); ++i)
	{
		if (base <= kSteps[i])
			break;
		const uintptr_t cands[2] = { base - kSteps[i], base + kSteps[i] };
		for (int c = 0; c < 2; ++c)
		{
			void* got = VirtualAlloc(reinterpret_cast<void*>(cands[c]), size,
			                          MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (!got)
				continue;
			const intptr_t d = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(got) - t);
			if (d >= INT32_MIN + 32 && d <= INT32_MAX - 32)
				return got;
			VirtualFree(got, 0, MEM_RELEASE);
		}
	}
	return nullptr;
}

// 入口跳转: 先试直写 E9 (同 ±2GB), 否则建近端跳板再 E9
static bool writeEntryJump(void* target, void* hook)
{
	if (writeRelJump(target, hook))
		return true;
	void* stub = allocNearCode(target, 64);
	if (!stub)
		return false;
	unsigned char b[14] = { 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	memcpy(b + 6, &hook, 8);
	memcpy(stub, b, 14);
	FlushInstructionCache(GetCurrentProcess(), stub, 14);
	return writeRelJump(target, stub);
}

// trampoline = 窃取的原字节 + FF25 绝对跳回 target+steal (自身内存, 无距离约束)
static void* allocTrampoline(const unsigned char* stolen, int steal, void* backTo)
{
	unsigned char* p = static_cast<unsigned char*>(
	    VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (!p)
		return nullptr;
	memcpy(p, stolen, steal);
	p[steal] = 0xFF;
	p[steal + 1] = 0x25;
	p[steal + 2] = p[steal + 3] = p[steal + 4] = p[steal + 5] = 0;
	memcpy(p + steal + 6, &backTo, 8);
	FlushInstructionCache(GetCurrentProcess(), p, steal + 14);
	return p;
}

static bool installCodeDetour(CodeDetour& d, void* target, void* hook,
                              const unsigned char* expect, int expectLen, int steal,
                              const char* tag)
{
	const std::string tagStr = std::string("方案B ") + tag;
	if (!isExecutablePage(target))
	{
		logLine(tagStr + ": 目标 " + hexOf(target) + " 不可执行, 跳过");
		return false;
	}
	unsigned char orig[16] = {};
	memcpy(orig, target, sizeof(orig));

	if (memcmp(orig, expect, expectLen) == 0)
	{
		// RAW: 入口是原封的 dxgi 存根 → 窃取字节做 trampoline
		void* tr = allocTrampoline(orig, steal, static_cast<unsigned char*>(target) + steal);
		if (!tr)
		{
			logLine(tagStr + ": trampoline VirtualAlloc 失败");
			return false;
		}
		if (!writeEntryJump(target, hook))
		{
			logLine(tagStr + ": 入口跳转写入失败 (近端跳板分配或 VirtualProtect 拒绝)");
			return false;
		}
		d.resume = tr;
		d.active = true;
		logLine(tagStr + ": RAW 已装 target=" + hexOf(target) +
		        " trampoline=" + hexOf(tr) + " 窃取=" + std::to_string(steal) + "B (入口 5B E9)");
		return true;
	}

	// 入口已被第三方改成跳板? → CHAIN: 计数后直接转发其目标, 保留其 handler 链
	void* chain = nullptr;
	const char* how = "";
	if (orig[0] == 0xE9)
	{
		int32_t r = 0;
		memcpy(&r, orig + 1, 4);
		chain = static_cast<unsigned char*>(target) + 5 + r;
		how = "E9 rel32";
	}
	else if (orig[0] == 0xFF && orig[1] == 0x25)
	{
		int32_t disp = 0;
		memcpy(&disp, orig + 2, 4);
		const void** slot = reinterpret_cast<const void**>(
		    static_cast<unsigned char*>(target) + 6 + disp);
		if (memReadable(slot, sizeof(void*)))
			memcpy(&chain, slot, sizeof(void*));
		how = "FF25 abs-indirect";
	}
	else if (orig[0] == 0x48 && orig[1] == 0xB8 && orig[10] == 0xFF && orig[11] == 0xE0)
	{
		memcpy(&chain, orig + 2, 8);
		how = "mov rax,imm64; jmp rax";
	}
	if (chain && isExecutablePage(chain))
	{
		if (!writeEntryJump(target, hook))
		{
			logLine(tagStr + ": CHAIN 模式入口跳转写入失败");
			return false;
		}
		d.resume = chain;
		d.active = true;
		logLine(tagStr + ": CHAIN 已装 (入口原为 " + std::string(how) + ") target=" +
		        hexOf(target) + " → 第三方目标 " + hexOf(chain) + " 来自 " +
		        modulePathOf(chain) + ", 原入口前16字节=" + hexBytes(orig, 16));
		return true;
	}

	logLine(tagStr + ": 入口字节与预期不符且非可识别跳板, 保守跳过; target=" +
	        hexOf(target) + " 前16字节=" + hexBytes(orig, 16));
	return false;
}

// 函数级钩子: TLS 置位 = 由 vtable 钩子转来 (它已计数) → 只转发;
// 否则 = 绕过类 vtable 的直呼路径 (GFE 包装 / 缓存 FP / 包装层动态转发) → 计数。
static HRESULT STDMETHODCALLTYPE detouredPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
	if (!g_detP.resume)
		return E_FAIL;
	if (!g_inVtableHook)
	{
		notePresent("函数detour", sc);
		pocbFrame(sc); // v0.9.3: vtable 层不可用时 (vtable=0 或挂不上) 也得注入 —— 否则
		               // PoC-B 只在 vtable 模式下工作, 逃生门 pocbEnabled() 也永远走不到
	}
	typedef HRESULT(STDMETHODCALLTYPE* Fn)(IDXGISwapChain*, UINT, UINT);
	return reinterpret_cast<Fn>(g_detP.resume)(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE detouredPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                                  const DXGI_PRESENT_PARAMETERS* params)
{
	if (!g_detP1.resume)
		return E_FAIL;
	if (!g_inVtableHook)
	{
		notePresent("函数detour1", sc);
		pocbFrame(sc); // v0.9.3: 同 detouredPresent —— 兜底模式也注入
	}
	typedef HRESULT(STDMETHODCALLTYPE* Fn)(IDXGISwapChain1*, UINT, UINT,
	                                       const DXGI_PRESENT_PARAMETERS*);
	return reinterpret_cast<Fn>(g_detP1.resume)(sc, sync, flags, params);
}

// 帧时基线 (v0.16.2) 借 PoC-B 的 immediate context —— 定义必须在 g_pocb 之后,
// 因为 notePresent 在文件更靠前的位置只能拿到前向声明。
static ID3D11DeviceContext* ftPocbCtx()
{
	return (g_pocb.state.load(std::memory_order_relaxed) == 2) ? g_pocb.ctx : nullptr;
}

// 由 hookedPresent / hookedPresent1 在调原 Present 之前调用
void pocbFrame(IDXGISwapChain* sc)
{
	if (!sc || !pocbEnabled())
		return;
	PocbCtx& c = g_pocb;
	const int st = c.state.load();
	if (st == 3)
		return;
	if (st == 0)
	{
		int expect = 0;
		if (!c.state.compare_exchange_strong(expect, 1))
			return; // 另一线程正在初始化
		if (!pocbInit(sc))
		{
			c.state.store(3);
			return;
		}
		c.state.store(2);
	}
	if (c.state.load() != 2)
		return;
	pocbInject(c, sc);
}

// ---------- 钩子本体 ----------

HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
	notePresent("Present", sc);
	pocbFrame(sc); // PoC-B: 在调原 Present 之前把 Vulkan 渲出的像素写进 backbuffer
	void** vtbl = *reinterpret_cast<void***>(sc);
	void* orig = nullptr;
	EnterCriticalSection(&g_cs);
	orig = lookupPresentOrig8(vtbl);
	LeaveCriticalSection(&g_cs);
	if (!orig)
	{
		logLine("!! hookedPresent: vtable " + hexOf(vtbl) + " 无登记原函数 — 返回 E_FAIL");
		return E_FAIL;
	}
	g_inVtableHook = true; // 转调期间置位: orig(存根) 现已被方案B detour, 其钩子据此不双计
	const HRESULT hr = reinterpret_cast<Present_t>(orig)(sc, sync, flags);
	g_inVtableHook = false;
	return hr;
}

HRESULT STDMETHODCALLTYPE hookedPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                          const DXGI_PRESENT_PARAMETERS* params)
{
	notePresent("Present1", sc);
	pocbFrame(sc); // PoC-B: 同 hookedPresent (交换链若走 Present1 口径也照注入)
	void** vtbl = *reinterpret_cast<void***>(sc);
	void* orig = nullptr;
	EnterCriticalSection(&g_cs);
	orig = lookupPresentOrig22(vtbl);
	LeaveCriticalSection(&g_cs);
	if (!orig)
	{
		logLine("!! hookedPresent1: vtable " + hexOf(vtbl) + " 无登记原函数 — 返回 E_FAIL");
		return E_FAIL;
	}
	g_inVtableHook = true;
	const HRESULT hr22 = reinterpret_cast<Present1_t>(orig)(sc, sync, flags, params);
	g_inVtableHook = false;
	return hr22;
}

// v1.7 阳性确认: 打印对象 vptr 与 vtbl[8] (Present) / vtbl[22] (Present1) 是否等于我们的
// 钩子 — 修槽之前 v1.0~v1.6 只证明过"挂了 SetPrivateDataInterface", 这行才是槽位修对的铁证
void confirmPositive(void* obj, const char* nm)
{
	const std::string tg = std::string("阳性确认 (") + nm + "): ";
	if (!obj || !memReadable(obj, sizeof(void*)))
	{
		logLine(tg + "对象不可读");
		return;
	}
	void** gv = *reinterpret_cast<void***>(obj);
	if (!memReadable(gv, 9 * sizeof(void*)))
	{
		logLine(tg + "vptr 不可读");
		return;
	}
	logLine(tg + "obj=" + hexOf(obj) + " vptr=" + hexOf(gv) + " 来自 " + modulePathOf(gv) +
	        " | vtbl[8]=" + hexOf(gv[8]) + " 来自 " + modulePathOf(gv[8]) +
	        (gv[8] == reinterpret_cast<void*>(&hookedPresent)
	             ? " ==我们的Present钩子 ✓"
	             : (g_vtableLayer ? " ≠我们的Present钩子 (尚未挂钩 → 随后就地挂)"
	                              : " ≠我们的Present钩子 (vtable=0 预期内: 本局不挂槽, 由方案B detour 覆盖)")));
	if (memReadable(gv, 23 * sizeof(void*)))
		logLine(tg + "vtbl[22]=" + hexOf(gv[22]) + " 来自 " + modulePathOf(gv[22]) +
		        (gv[22] == reinterpret_cast<void*>(&hookedPresent1)
		             ? " ==我们的Present1钩子 ✓"
		             : (g_vtableLayer ? " ≠我们的Present1钩子"
		                              : " ≠我们的Present1钩子 (vtable=0 预期内)")));
}

// v1.7: 判定对象 vtable 是否含槽 22 (SwapChain1 布局) — 用 QI 实测而非猜布局,
// 免得 v0 口径创建的 ★ 游戏交换链漏挂 Present1 (也不会越界读短 vtable)
static bool qiuHasSwapChain1(void* sc)
{
	if (!sc)
		return false;
	void* p = nullptr;
	if (FAILED(reinterpret_cast<IUnknown*>(sc)->QueryInterface(__uuidof(IDXGISwapChain1), &p)) || !p)
		return false;
	reinterpret_cast<IUnknown*>(p)->Release();
	return true;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChain(IDXGIFactory* self, IUnknown* dev,
                                                 const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
	void** vtbl = *reinterpret_cast<void***>(self);
	void* orig = nullptr;
	EnterCriticalSection(&g_cs);
	orig = lookupFacOrig(vtbl, 10);
	LeaveCriticalSection(&g_cs);
	if (!orig)
		return E_FAIL;
	HRESULT hr = reinterpret_cast<CreateSwapChain_t>(orig)(self, dev, desc, out);
	if (SUCCEEDED(hr) && out && *out)
	{
		logLine("★ 工厂拦截 CreateSwapChain → swapchain=" + hexOf(*out) + " | " + swapDesc(*out));
		confirmPositive(*out, "★游戏 CreateSwapChain");
		g_gameSc.store(*out);
		registerSwp(*out, qiuHasSwapChain1(*out), "★游戏 CreateSwapChain");
		unwrapRealSwapChain(*out, "★游戏 CreateSwapChain");
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* desc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                       IDXGIOutput* restrictOut, IDXGISwapChain1** out)
{
	void** vtbl = *reinterpret_cast<void***>(self);
	void* orig = nullptr;
	EnterCriticalSection(&g_cs);
	orig = lookupFacOrig(vtbl, 15);
	LeaveCriticalSection(&g_cs);
	if (!orig)
		return E_FAIL;
	HRESULT hr = reinterpret_cast<CreateSwapChainForHwnd_t>(orig)(self, dev, hwnd, desc, fs, restrictOut, out);
	if (SUCCEEDED(hr) && out && *out)
	{
		logLine("★ 工厂拦截 CreateSwapChainForHwnd → swapchain=" + hexOf(*out) + " | " + swapDesc(*out));
		confirmPositive(*out, "★游戏 CreateSwapChainForHwnd");
		g_gameSc.store(*out);
		registerSwp(*out, true, "★游戏 CreateSwapChainForHwnd");
		unwrapRealSwapChain(*out, "★游戏 CreateSwapChainForHwnd");
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChainForCoreWindow(IDXGIFactory2* self, IUnknown* dev, IUnknown* window,
                                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                                             IDXGIOutput* restrictOut, IDXGISwapChain1** out)
{
	void** vtbl = *reinterpret_cast<void***>(self);
	void* orig = nullptr;
	EnterCriticalSection(&g_cs);
	orig = lookupFacOrig(vtbl, 16);
	LeaveCriticalSection(&g_cs);
	if (!orig)
		return E_FAIL;
	HRESULT hr = reinterpret_cast<CreateSwapChainForCoreWindow_t>(orig)(self, dev, window, desc, restrictOut, out);
	if (SUCCEEDED(hr) && out && *out)
	{
		logLine("★ 工厂拦截 CreateSwapChainForCoreWindow → swapchain=" + hexOf(*out) + " | " + swapDesc(*out));
		confirmPositive(*out, "★游戏 CreateSwapChainForCoreWindow");
		g_gameSc.store(*out);
		registerSwp(*out, true, "★游戏 CreateSwapChainForCoreWindow");
		unwrapRealSwapChain(*out, "★游戏 CreateSwapChainForCoreWindow");
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE hookedCreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* dev,
                                                              const DXGI_SWAP_CHAIN_DESC1* desc,
                                                              IDXGISwapChain1** out)
{
	void** vtbl = *reinterpret_cast<void***>(self);
	void* orig = nullptr;
	EnterCriticalSection(&g_cs);
	orig = lookupFacOrig(vtbl, 24);
	LeaveCriticalSection(&g_cs);
	if (!orig)
		return E_FAIL;
	HRESULT hr = reinterpret_cast<CreateSwapChainForComposition_t>(orig)(self, dev, desc, out);
	if (SUCCEEDED(hr) && out && *out)
	{
		logLine("★ 工厂拦截 CreateSwapChainForComposition → swapchain=" + hexOf(*out) + " | " + swapDesc(*out));
		confirmPositive(*out, "★游戏 CreateSwapChainForComposition");
		g_gameSc.store(*out);
		registerSwp(*out, true, "★游戏 CreateSwapChainForComposition");
		unwrapRealSwapChain(*out, "★游戏 CreateSwapChainForComposition");
	}
	return hr;
}

// ---------- vtable 变体收集 ----------

void collectSwpVariants(IDXGISwapChain* any, bool rawPlus, const char* tag)
{
	registerSwp(any, rawPlus, tag);
	IID iids[4];
	iids[0] = __uuidof(IDXGISwapChain);
	iids[1] = __uuidof(IDXGISwapChain1);
	iids[2] = __uuidof(IDXGISwapChain2);
	iids[3] = __uuidof(IDXGISwapChain3);
	const bool plus[4] = { false, true, true, true };
	const char* nm[4] = { "QI-SwapChain", "QI-SwapChain1", "QI-SwapChain2", "QI-SwapChain3" };
	for (int i = 0; i < 4; ++i)
	{
		void* p = nullptr;
		if (SUCCEEDED(any->QueryInterface(iids[i], &p)) && p)
		{
			registerSwp(p, plus[i], (std::string(tag) + " " + nm[i]).c_str());
			reinterpret_cast<IUnknown*>(p)->Release();
		}
	}
}

void collectFacVariants(void* obj, bool plus2, const char* tag)
{
	registerFac(obj, plus2, tag);
	// QI 向下 (Factory1 / Factory0): 只挂槽 10 — 任何工厂布局都有槽 10, 安全
	IID down[2];
	down[0] = __uuidof(IDXGIFactory1);
	down[1] = __uuidof(IDXGIFactory);
	const char* dn[2] = { "QI-Factory1", "QI-Factory" };
	for (int i = 0; i < 2; ++i)
	{
		void* p = nullptr;
		if (SUCCEEDED(reinterpret_cast<IUnknown*>(obj)->QueryInterface(down[i], &p)) && p)
		{
			registerFac(p, false, (std::string(tag) + " " + dn[i]).c_str());
			reinterpret_cast<IUnknown*>(p)->Release();
		}
	}
	// QI 向上 Factory2: QI 成功即该指针布局保证含槽 15/16/24 (Factory2 末项=24), 升级安全
	if (!plus2)
	{
		void* p = nullptr;
		if (SUCCEEDED(reinterpret_cast<IUnknown*>(obj)->QueryInterface(__uuidof(IDXGIFactory2), &p)) && p)
		{
			registerFac(p, true, (std::string(tag) + " QI-Factory2").c_str());
			reinterpret_cast<IUnknown*>(p)->Release();
		}
	}
}

// ---------- Hook 安装 ----------

bool waitForModules(int timeoutMs, HMODULE& hd3d11, HMODULE& hdxgi)
{
	hd3d11 = nullptr;
	hdxgi = nullptr;
	for (int waited = 0; waited < timeoutMs; waited += 10)
	{
		hd3d11 = GetModuleHandleW(L"d3d11.dll");
		hdxgi = GetModuleHandleW(L"dxgi.dll");
		if (hd3d11 && hdxgi)
			break;
		Sleep(10);
	}
	if (!hd3d11 || !hdxgi)
	{
		logLine("等待 d3d11/dxgi 超时 (" + std::to_string(timeoutMs) + "ms): d3d11=" +
		        (hd3d11 ? "已载入" : "未载入") + " dxgi=" + (hdxgi ? "已载入" : "未载入"));
		return false;
	}
	{
		char p1[MAX_PATH]{}, p2[MAX_PATH]{};
		GetModuleFileNameA(hd3d11, p1, MAX_PATH);
		GetModuleFileNameA(hdxgi, p2, MAX_PATH);
		logLine("d3d11.dll = " + std::string(p1));
		logLine("dxgi.dll  = " + std::string(p2));
	}
	return true;
}

// 加载序兜底: 正常应由 capture-helper 先加载 renderdoc; 若我们先被加载,
// 等它出现 (+500ms 让其挂钩完成) 再安装 — 保证 dummy 与游戏走同一套包装类。
void waitRenderDocSettle()
{
	if (GetModuleHandleW(L"renderdoc.dll"))
		return;
	for (int i = 0; i < 200; ++i)
	{
		if (GetModuleHandleW(L"renderdoc.dll"))
		{
			logLine("renderdoc.dll 在等待后才出现 (加载序兜底) — 再等 500ms 让其挂钩完成");
			Sleep(500);
			return;
		}
		Sleep(10);
	}
	logLine("等待 renderdoc.dll 2s 未出现 (无 capture-helper 场景) — 按当前状态安装");
}

bool doInstall(HMODULE hd3d11, HMODULE hdxgi)
{
	// 0) RenderDoc 加载状态 (判 dummy 是否走其包装类)
	if (HMODULE hrd = GetModuleHandleW(L"renderdoc.dll"))
	{
		char pr[MAX_PATH]{};
		GetModuleFileNameA(hrd, pr, MAX_PATH);
		logLine("renderdoc.dll = " + std::string(pr) + " (已加载 — 交换链可能走其包装类)");
	}
	else
	{
		logLine("renderdoc.dll 未加载 (未装 capture-helper?)");
	}

	// 2) 动态取导出 (不静态链接 d3d11/dxgi, 由我们控制首次加载时机)
	auto pCreateDevice = reinterpret_cast<HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
	                                                       const D3D_FEATURE_LEVEL*, UINT, UINT,
	                                                       ID3D11Device**, D3D_FEATURE_LEVEL*, UINT*)>(
	    GetProcAddress(hd3d11, "D3D11CreateDevice"));
	auto pCreateFactory0 = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
	    GetProcAddress(hdxgi, "CreateDXGIFactory"));
	auto pCreateFactory1 = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
	    GetProcAddress(hdxgi, "CreateDXGIFactory1"));
	auto pCreateFactory2 = reinterpret_cast<HRESULT(WINAPI*)(UINT, REFIID, void**)>(
	    GetProcAddress(hdxgi, "CreateDXGIFactory2"));
	if (!pCreateDevice || !pCreateFactory1)
	{
		logLine("导出函数获取失败: D3D11CreateDevice=" + hexOf(reinterpret_cast<const void*>(pCreateDevice)) +
		        " CreateDXGIFactory1=" + hexOf(reinterpret_cast<const void*>(pCreateFactory1)));
		return false;
	}
	if (!pCreateFactory0)
		logLine("导出 CreateDXGIFactory 不存在 — 跳过该口径");
	if (!pCreateFactory2)
		logLine("导出 CreateDXGIFactory2 不存在 (Win8.1 以下) — 跳过该口径");

	// 3) 隐藏窗口 (swapchain 需要 HWND; 不显示, 进程生命周期内保持存活)
	WNDCLASSA wc{};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = g_hModule;
	wc.lpszClassName = "PocPresenterDummyWnd";
	RegisterClassA(&wc);  // 已存在时失败无妨
	HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "poc-presenter dummy", WS_OVERLAPPED,
	                            0, 0, 4, 4, nullptr, nullptr, g_hModule, nullptr);
	if (!hwnd)
	{
		logLine("隐藏窗口创建失败 err=" + std::to_string(GetLastError()));
		return false;
	}

	// 4) dummy 设备 (先硬件, 失败退 WARP)
	ID3D11Device* dev = nullptr;
	D3D_FEATURE_LEVEL got{};
	HRESULT hr = pCreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
	                           D3D11_SDK_VERSION, &dev, &got, nullptr);
	std::string adapterHow = "HARDWARE";
	if (FAILED(hr))
	{
		logLine("硬件设备创建失败 " + hexHr(hr) + " → 试 WARP");
		hr = pCreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
		                   D3D11_SDK_VERSION, &dev, &got, nullptr);
		adapterHow = "WARP";
	}
	if (FAILED(hr) || !dev)
	{
		logLine("D3D11CreateDevice 失败 " + hexHr(hr));
		return false;
	}

	// 5) 五个口径取工厂对象 (游戏可能走不同导出或不同 IID, RenderDoc 包装类可能按口径分化;
	//    此处只取对象不挂钩, 钩子统一在第 8 步装, 避免自己的 dummy 创建走钩子产生噪音)
	void* facObjs[5] = {};
	{
		HRESULT fhr = E_FAIL;
		if (pCreateFactory0)
		{
			fhr = pCreateFactory0(__uuidof(IDXGIFactory), &facObjs[0]);
			logLine("工厂口径0 CreateDXGIFactory+IID0: " +
			        ((SUCCEEDED(fhr) && facObjs[0]) ? std::string("OK ") + hexOf(facObjs[0])
			                                        : "失败 " + hexHr(fhr)));
		}
		fhr = pCreateFactory1(__uuidof(IDXGIFactory), &facObjs[1]);
		logLine("工厂口径1 CreateDXGIFactory1+IID0: " +
		        ((SUCCEEDED(fhr) && facObjs[1]) ? std::string("OK ") + hexOf(facObjs[1])
		                                        : "失败 " + hexHr(fhr)));
		fhr = pCreateFactory1(__uuidof(IDXGIFactory1), &facObjs[2]);
		logLine("工厂口径2 CreateDXGIFactory1+IID1: " +
		        ((SUCCEEDED(fhr) && facObjs[2]) ? std::string("OK ") + hexOf(facObjs[2])
		                                        : "失败 " + hexHr(fhr)));
		fhr = pCreateFactory1(__uuidof(IDXGIFactory2), &facObjs[3]);
		logLine("工厂口径3 CreateDXGIFactory1+IID2: " +
		        ((SUCCEEDED(fhr) && facObjs[3]) ? std::string("OK ") + hexOf(facObjs[3])
		                                        : "失败 " + hexHr(fhr)));
		if (pCreateFactory2)
		{
			fhr = pCreateFactory2(0, __uuidof(IDXGIFactory2), &facObjs[4]);
			logLine("工厂口径4 CreateDXGIFactory2+IID2: " +
			        ((SUCCEEDED(fhr) && facObjs[4]) ? std::string("OK ") + hexOf(facObjs[4])
			                                        : "失败 " + hexHr(fhr)));
		}
	}
	IDXGIFactory2* factory = reinterpret_cast<IDXGIFactory2*>(facObjs[3]);
	if (!factory)
		factory = reinterpret_cast<IDXGIFactory2*>(facObjs[4]);
	if (!factory)
	{
		logLine("无可用 Factory2 对象 (口径3/4 均失败) — 无法创建 dummy, 安装失败");
		return false;
	}
	// 5b) dummy1 (v1 路径: CreateSwapChainForHwnd)
	DXGI_SWAP_CHAIN_DESC1 sd{};
	sd.Width = 4;
	sd.Height = 4;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	IDXGISwapChain1* sc1 = nullptr;
	hr = factory->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc1);
	if (FAILED(hr) || !sc1)
	{
		logLine("CreateSwapChainForHwnd 失败 " + hexHr(hr));
		return false;
	}
	logLine("dummy1 swapchain (" + adapterHow + " 设备, ForHwnd 路径): " + swapDesc(sc1));

	// 6) dummy2 (v0 路径: 工厂 CreateSwapChain — 覆盖游戏可能的 v0 接口口径)
	IDXGISwapChain* sc0 = nullptr;
	{
		DXGI_SWAP_CHAIN_DESC d0{};
		d0.BufferDesc.Width = 4;
		d0.BufferDesc.Height = 4;
		d0.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d0.SampleDesc.Count = 1;
		d0.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		d0.BufferCount = 2;
		d0.OutputWindow = hwnd;
		d0.Windowed = TRUE;
		d0.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		hr = factory->CreateSwapChain(dev, &d0, &sc0);
		if (FAILED(hr) || !sc0)
		{
			logLine("dummy2 CreateSwapChain(v0 路径) 失败 " + hexHr(hr));
			sc0 = nullptr;
		}
		else
		{
			logLine("dummy2 swapchain (v0 路径): " + swapDesc(sc0));
		}
	}

	// 6b) dummy3 (FLIP_DISCARD 口径 — 若游戏或系统另有一个翻转类交换链, 也纳入登记)
	IDXGISwapChain1* scF = nullptr;
	{
		DXGI_SWAP_CHAIN_DESC1 f{};
		f.Width = 4;
		f.Height = 4;
		f.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		f.SampleDesc.Count = 1;
		f.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		f.BufferCount = 2;
		f.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		f.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		HRESULT fhr = factory->CreateSwapChainForHwnd(dev, hwnd, &f, nullptr, nullptr, &scF);
		if (SUCCEEDED(fhr) && scF)
			logLine("dummy3 swapchain (FLIP_DISCARD 路径): " + swapDesc(scF));
		else
		{
			logLine("dummy3 (FLIP_DISCARD) 创建失败 " + hexHr(fhr) + " — 跳过该口径");
			scF = nullptr;
		}
	}

	// 7) 收集并挂所有交换链 vtable 变体 (槽 8 与安全的槽 22)
	collectSwpVariants(sc1, true, "dummy1(ForHwnd)");
	if (sc0)
		collectSwpVariants(sc0, false, "dummy2(v0)");
	if (scF)
		collectSwpVariants(scF, true, "dummy3(FLIP)");

	// 7b) 设备链取工厂 (游戏常见路径: device → IDXGIDevice → GetAdapter → GetParent)。
	//     实测 P3 (v1.2 双跑 2026-10-02): 有 renderdoc 时五口径导出工厂全被其包装
	//     (vtable 都在 renderdoc.dll), 游戏却没走我们 patch 的那张类 — 120s 无 ★ 无 Present;
	//     无 renderdoc 时同一条钩子链在 dxgi 真类上 ★ 正常触发 (+7s) → 游戏拿工厂走的不是
	//     导出 (或在 renderdoc 生效前就拿到了真对象), 其类 = 真 dxgi 类。本步经设备链探
	//     GetParent: 若落到真 dxgi 类, 辅助 dummy 会把真交换链类也挂上 (双类覆盖);
	//     先统一建辅助 dummy (此时还没挂这些工厂的槽, 不产生自身 ★ 噪音), 再统一挂钩。
	{
		IDXGIDevice* gid = nullptr;
		if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&gid))) && gid)
		{
			IDXGIAdapter* adp = nullptr;
			if (SUCCEEDED(gid->GetAdapter(&adp)) && adp)
			{
				{
					void** avtbl = *reinterpret_cast<void***>(adp);
					logLine("设备链 GetAdapter = " + hexOf(adp) + " vtable=" + hexOf(avtbl) +
					        " 来自 " + modulePathOf(avtbl));
				}
				void* seen[8] = {};
				int nSeen = 0;
				for (int i = 0; i < 5; ++i)
					if (facObjs[i])
						seen[nSeen++] = facObjs[i];
				auto isSeen = [&](const void* p) {
					for (int k = 0; k < nSeen; ++k)
						if (seen[k] == p)
							return true;
					return false;
				};
				void* newFacs[3] = {};
				bool newPlus2[3] = {};
				int newIdx[3] = {};
				int nNew = 0;
				IID fids[3];
				fids[0] = __uuidof(IDXGIFactory2);
				fids[1] = __uuidof(IDXGIFactory1);
				fids[2] = __uuidof(IDXGIFactory);
				for (int i = 0; i < 3; ++i)
				{
					void* f = nullptr;
					// 注意: 不是 QI — 适配器不实现工厂接口, 须 GetParent 上溯
					HRESULT qhr = adp->GetParent(fids[i], &f);
					if (FAILED(qhr) || !f)
					{
						logLine("设备链工厂 IID" + std::to_string(i) + " GetParent 失败 " + hexHr(qhr));
						continue;
					}
					if (isSeen(f))
					{
						logLine("设备链工厂 IID" + std::to_string(i) + " = " + hexOf(f) + " (与已有重复, 跳过)");
						continue;
					}
					seen[nSeen++] = f;
					// 外部建议采纳: 对象在堆上, modulePathOf(对象) 恒 "?",
					// 须打它 vtable 落在哪个模块 (renderdoc 类 vs 真 dxgi 类一眼可辨)
					void** fvt = *reinterpret_cast<void***>(f);
					logLine("设备链工厂 IID" + std::to_string(i) + " = " + hexOf(f) +
					        " vtbl=" + hexOf(fvt) + " 来自 " + modulePathOf(fvt));
					DXGI_SWAP_CHAIN_DESC d{};
					d.BufferDesc.Width = 4;
					d.BufferDesc.Height = 4;
					d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					d.SampleDesc.Count = 1;
					d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
					d.BufferCount = 2;
					d.OutputWindow = hwnd;
					d.Windowed = TRUE;
					d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
					IDXGISwapChain* aux = nullptr;
					HRESULT ahr = reinterpret_cast<IDXGIFactory*>(f)->CreateSwapChain(dev, &d, &aux);
					if (FAILED(ahr) || !aux)
					{
						logLine("设备链辅助 dummy IID" + std::to_string(i) + " CreateSwapChain 失败 " + hexHr(ahr));
					}
					else
					{
						logLine("设备链辅助 dummy IID" + std::to_string(i) + ": " + swapDesc(aux));
						collectSwpVariants(aux, false, ("设备链dummy IID" + std::to_string(i)).c_str());
					}
					newFacs[nNew] = f;
					newPlus2[nNew] = (i == 0);
					newIdx[nNew] = i;
					++nNew;
				}
				// pass2: 统一挂这些新工厂的交换链创建方法 (辅助 dummy 已先建, 无自身 ★)
				for (int k = 0; k < nNew; ++k)
					collectFacVariants(newFacs[k], newPlus2[k],
					                    ("设备链GetParent工厂 IID" + std::to_string(newIdx[k])).c_str());
				adp->Release();
			}
			gid->Release();
		}
		else
		{
			logLine("设备链: QI IDXGIDevice 失败 — 跳过该通道");
		}
	}

	// 8) 挂五个口径工厂的交换链创建方法 — 游戏创建交换链时就地打它的 vtable;
	//    plus2 仅对 IID2 口径为 true (该布局保证含槽15/16/24, 防越界)
	{
		const char* facTags[5] = {
			"工厂口径0(Factory导出+IID0)", "工厂口径1(Factory1导出+IID0)",
			"工厂口径2(Factory1导出+IID1)", "工厂口径3(Factory1导出+IID2)",
			"工厂口径4(Factory2导出+IID2)" };
		for (int i = 0; i < 5; ++i)
			if (facObjs[i])
				collectFacVariants(facObjs[i], i >= 3, facTags[i]);
	}

	// 9) 汇总 (sc0/sc1/factory/dev 故意不释放: 对象驻留 = 钩子常驻)
	{
		int swpPatched = 0;
		EnterCriticalSection(&g_cs);
		for (int i = 0; i < g_swpN; ++i)
			if (g_swp[i].orig8)
				++swpPatched;
		LeaveCriticalSection(&g_cs);
		logLine("登记完成: 交换链 vtable " + std::to_string(swpPatched) + " 个 (" +
		        (g_vtableLayer ? "含 Present 挂钩" : "vtable=0 未挂 Present") + "), " +
		        "工厂 vtable " + std::to_string(g_facN) + " 个 (含创建方法挂钩); Present 钩子=" +
		        hexOf(reinterpret_cast<const void*>(&hookedPresent)));
		// v1.7 阳性确认: 槽位修对后, dummy 对象的 vtbl[8] 必须就是我们挂的 Present 钩子
		// (v1.0~v1.6 只验证过 vtbl[4] — 那是 SetPrivateDataInterface, 假阳性根源)
		confirmPositive(sc1, "dummy1 安装完成");
		if (sc0)
			confirmPositive(sc0, "dummy2 安装完成");
		if (scF)
			confirmPositive(scF, "dummy3 安装完成");
	}

	// v1.5 探针 B: 原实现前 32 字节 — 为函数级 detour 选安全窃取长度 (v1.7: 真槽 8/22)
	{
		void* o8 = nullptr;
		void* o22 = nullptr;
		EnterCriticalSection(&g_cs);
		if (g_swpN > 0)
		{
			o8 = g_swp[0].orig8;
			if (g_swp[0].has22)
				o22 = g_swp[0].orig22;
		}
		LeaveCriticalSection(&g_cs);
		if (o8)
			logLine("探针: Present(slot8) 原实现前32字节 @ " + hexOf(o8) + " = " + hexBytes(o8, 32));
		if (o22)
			logLine("探针: Present1(slot22) 原实现前32字节 @ " + hexOf(o22) + " = " + hexBytes(o22, 32));
	}
	if (g_facN > 0 && g_fac[0].o10)
		logLine("探针: CreateSwapChain 原实现前32字节 @ " + hexOf(g_fac[0].o10) + " = " +
		        hexBytes(g_fac[0].o10, 32));

	// v1.7 方案B: 对真 Present/Present1 打函数级 detour — 靶子按优先级运行时取:
	//   ① 活交换链 vtbl[8]/vtbl[22] 直读 (硬编码零依赖, 调用方真正会走的入口);
	//   ② 方案A 解包 (renderdoc 包装对象 [obj+0x28] → 真对象) 的 vtbl[8]/vtbl[22];
	//   ③ 本机 dxgi RVA 0x19000/0x194A0 兜底 (文件版本恒定; 旧 0x2E460/0x4EE60 已作废)。
	// 每个候选都过 可执行页 + 14B 前导字节双校验 (末字节 56=Present / 54=Present1);
	// 字节不符 → 保守跳过并留档, 换下一候选; 全部失败只丢函数层, vtable 层仍在。
	{
		// 14B 前导字节守卫 (本机 dxgi 实测): 前 12B 两函数相同, 第 14B 56=Present / 54=Present1
		static const unsigned char kPresentBytes[14] = {
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x56 };
		static const unsigned char kPresent1Bytes[14] = {
			0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54 };
		struct Cand { void* t; std::string src; };
		Cand cp[8];
		Cand cp1[8];
		int np = 0, np1 = 0;
		auto add = [](Cand* arr, int& n, void* t, const char* src) {
			if (!t || n >= 8)
				return;
			for (int i = 0; i < n; ++i)
				if (arr[i].t == t)
					return;
			arr[n].t = t;
			arr[n].src = src;
			++n;
		};
		// ① 活交换链 vtbl[8]/[22] (登记表原值 = 钩子换掉前的真实入口)
		EnterCriticalSection(&g_cs);
		for (int i = 0; i < g_swpN; ++i)
		{
			add(cp, np, g_swp[i].orig8, "活交换链vtbl[8]");
			if (g_swp[i].has22)
				add(cp1, np1, g_swp[i].orig22, "活交换链vtbl[22]");
		}
		LeaveCriticalSection(&g_cs);
		// ② 方案A 解包 (仅 renderdoc 包装对象产生候选, 其余对象在函数内自行跳过)
		{
			void* srcs[3] = { sc1, sc0, scF };
			for (int i = 0; i < 3; ++i)
			{
				if (!srcs[i])
					continue;
				void* real = unwrapRealSwapChain(srcs[i], "安装期解包");
				if (!real)
					continue;
				void** rvt = *reinterpret_cast<void***>(real);
				if (memReadable(rvt, 9 * sizeof(void*)))
					add(cp, np, rvt[8], "方案A真vtbl[8]");
				if (memReadable(rvt, 23 * sizeof(void*)))
					add(cp1, np1, rvt[22], "方案A真vtbl[22]");
			}
		}
		// ③ 本机 dxgi RVA 兜底
		if (HMODULE hdx = GetModuleHandleA("dxgi.dll"))
		{
			add(cp, np, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hdx) + 0x19000),
			    "dxgi+0x19000 兜底");
			add(cp1, np1, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hdx) + 0x194A0),
			    "dxgi+0x194A0 兜底");
		}
		// 逐候选安装 (第一个通过字节守卫的胜出)
		bool okP = false;
		for (int i = 0; i < np && !okP; ++i)
		{
			logLine("方案B Present 候选 " + std::to_string(i + 1) + "/" + std::to_string(np) + ": " +
			        hexOf(cp[i].t) + " 来自 " + cp[i].src + " (" + modulePathOf(cp[i].t) + ")");
			okP = installCodeDetour(g_detP, cp[i].t, reinterpret_cast<void*>(&detouredPresent),
			                        kPresentBytes, 14, 14, "Present");
		}
		if (!okP)
			logLine("方案B Present: " + std::to_string(np) + " 个候选全部未装 (或无候选) — 函数层缺失, vtable 层仍覆盖");
		bool okP1 = false;
		for (int i = 0; i < np1 && !okP1; ++i)
		{
			logLine("方案B Present1 候选 " + std::to_string(i + 1) + "/" + std::to_string(np1) + ": " +
			        hexOf(cp1[i].t) + " 来自 " + cp1[i].src + " (" + modulePathOf(cp1[i].t) + ")");
			okP1 = installCodeDetour(g_detP1, cp1[i].t, reinterpret_cast<void*>(&detouredPresent1),
			                         kPresent1Bytes, 14, 14, "Present1");
		}
		if (!okP1)
			logLine("方案B Present1: " + std::to_string(np1) + " 个候选全部未装 (或无候选) — 函数层缺失, vtable 层仍覆盖");
	}

	// v1.5 探针 W: 本进程顶层窗口 — 找可能漏钩的第二个交换链的宿主窗口
	logProcessWindows("安装完成时");
	return true;
}

// 统一安装入口: 等模块 (≤waitMs) + 等 renderdoc 加载序落定 + 完整安装
bool installHook(int waitMs)
{
	HMODULE hd3d11 = nullptr, hdxgi = nullptr;
	if (!waitForModules(waitMs, hd3d11, hdxgi))
		return false;
	waitRenderDocSettle();
	return doInstall(hd3d11, hdxgi);
}

// ---------- 看门狗: 每 5s 复查被改写槽位并打回 (须持 g_cs) ----------

void verifyAndRepair()
{
	EnterCriticalSection(&g_cs);
	// v0.9.2 vtable=0: 交换链槽不挂也不"打回" (否则看门狗 5s 后会把钩子又装回去)
	for (int i = 0; g_vtableLayer && i < g_swpN; ++i)
	{
		SwpEntry& e = g_swp[i];
		if (e.vtbl[8] != reinterpret_cast<void*>(&hookedPresent))
		{
			logLine("!! vtable " + hexOf(e.vtbl) + " slot8 被改写: 现=" + hexOf(e.vtbl[8]) +
			        " 来自 " + modulePathOf(e.vtbl[8]) + " → 打回我们的钩子");
			patchSlotLocked(&e.vtbl[8], reinterpret_cast<void*>(&hookedPresent));
		}
		if (e.has22 && e.vtbl[22] != reinterpret_cast<void*>(&hookedPresent1))
		{
			logLine("!! vtable " + hexOf(e.vtbl) + " slot22 被改写: 现=" + hexOf(e.vtbl[22]) +
			        " 来自 " + modulePathOf(e.vtbl[22]) + " → 打回我们的钩子");
			patchSlotLocked(&e.vtbl[22], reinterpret_cast<void*>(&hookedPresent1));
		}
	}
	for (int i = 0; i < g_facN; ++i)
	{
		FacEntry& e = g_fac[i];
		const int   slots[4] = { 10, 15, 16, 24 };
		void* const hooks[4] = {
			reinterpret_cast<void*>(&hookedCreateSwapChain),
			reinterpret_cast<void*>(&hookedCreateSwapChainForHwnd),
			reinterpret_cast<void*>(&hookedCreateSwapChainForCoreWindow),
			reinterpret_cast<void*>(&hookedCreateSwapChainForComposition)
		};
		void* const origs[4] = { e.o10, e.o15, e.o16, e.o24 };
		for (int k = 0; k < 4; ++k)
		{
			if (!origs[k])
				continue;
			if (e.vtbl[slots[k]] != hooks[k])
			{
				logLine("!! factory vtable " + hexOf(e.vtbl) + " slot" + std::to_string(slots[k]) +
				        " 被改写: 现=" + hexOf(e.vtbl[slots[k]]) + " 来自 " +
				        modulePathOf(e.vtbl[slots[k]]) + " → 打回我们的钩子");
				patchSlotLocked(&e.vtbl[slots[k]], hooks[k]);
			}
		}
	}
	LeaveCriticalSection(&g_cs);
}

void watchdogThread()
{
	bool reported = false, alarmed = false;
	int ticks = 0;
	void** lastGV = nullptr;
	void* lastS8 = nullptr;
	for (;;)
	{
		Sleep(1000);
		++ticks;
		if (ticks % 5 == 0)
		{
			verifyAndRepair();

			// v1.7 探针 A (改读真槽 8): 对象级监视 — 类 vtable 稳不代表游戏对象的 vptr/槽位没变
			void* gsc = g_gameSc.load();
			if (gsc && memReadable(gsc, sizeof(void*)))
			{
				void** gv = *reinterpret_cast<void***>(gsc);
				if (memReadable(gv, 9 * sizeof(void*)))
				{
					if (gv != lastGV)
					{
						logLine("探针: 游戏交换链 vptr=" + hexOf(gv) + " 来自 " + modulePathOf(gv) +
						        (lastGV ? " (变更! 旧=" + hexOf(lastGV) + ")" : " (基线)"));
						confirmPositive(gsc, "探针A vptr基线/变更");  // v1.7 阳性确认: vptr + vtbl[8] 全量打
						lastGV = gv;
						lastS8 = nullptr;
					}
					void* s8 = gv[8];
					if (s8 != lastS8)
					{
						logLine("探针: 游戏交换链 slot8=" + hexOf(s8) + " 来自 " + modulePathOf(s8) +
					                (s8 == reinterpret_cast<void*>(&hookedPresent)
					                     ? " (我们的钩子 ✓)"
					                     : " (≠我们的钩子 !!)"));
						lastS8 = s8;
					}
				}
			}
		}
		if (ticks == 10 || ticks == 60)
			logProcessWindows("运行中 t=" + std::to_string(ticks) + "s");

		const uint64_t n = g_presentCount.load();
		if (!reported && n > 0)
		{
			reported = true;
			logLine("Present 已开始触发 (计数 " + std::to_string(n) + ") — PoC-A 验收通过");
		}
		if (!alarmed && n == 0 && ticks >= 120)
		{
			alarmed = true;
			int swpN = 0, facN = 0;
			EnterCriticalSection(&g_cs);
			swpN = g_swpN;
			facN = g_facN;
			LeaveCriticalSection(&g_cs);
			void* gsc = g_gameSc.load();
			logLine("告警: 安装成功但 120s 内 0 次 Present — 已登记 交换链vtable=" +
			        std::to_string(swpN) + " 工厂vtable=" + std::to_string(facN) +
			        "; 游戏交换链=" +
			        (gsc ? hexOf(gsc) + " (★ 已取得, 见探针A)" : std::string("未取得 (★=0 / g_gameSc 空)")) +
			        "; 方案B Present detour=" + std::string(g_detP.active ? "已装" : "未装") +
			        "; 按日志中 vtable 模块 / 阳性确认 / ★工厂拦截 / 被改写记录迭代");
		}
	}
}

// 后备: 同步安装未就绪 (模块/加载序) 时, 后台再等最多 30s 安装, 然后进入看门狗
void fallbackInstallThread()
{
	logLine("后台兜底线程: 最多再等 30s 安装");
	if (installHook(30000))
		logLine("后台安装完成");
	else
		logLine("后台安装失败 (详见上方日志)");
	watchdogThread();
}

} // namespace pocmain

using namespace pocmain; // 之后的 DllMain / SKSEPlugin_Load 照常可见

// ---------- DLL 入口 ----------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_hModule = hModule;
		DisableThreadLibraryCalls(hModule);
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		logLineFlush(); // C-1: 常驻日志文件句柄在此收尾
	}
	return TRUE;
}

// ---------- SKSE 插件入口 ----------

extern "C" {

__declspec(dllexport) bool SKSEPlugin_Query(const SKSEInterface* skse, PluginInfo* info)
{
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "PocPresenter";
	info->version = (0u << 16) | (10u << 8) | 0u; // 0.10.0

	if (skse->isEditor)  // 只进游戏本体, 不进 Creation Kit
		return false;

	return true;
}

__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* skse)
{
	g_logPath = pluginDir() + "\\poc-presenter.log";
	logLine("==== poc-presenter v0.18.16 (PoC-A v1.7 + PoC-B 共享纹理通路 NT handle+fence / 水体探针升质 512²→1024² 含配对depth+回写desc + plan-B 绑定感知拦RSSetViewports + SSR Step1/2a 侦察拦 ctx槽33/47/50 只记日志 + Step2b 通路哨兵 ssr.sentinel + Step2c 共享入向 ssr.shared + R2 归因重试 + Step2c-β VK 交叉校验 + 导入归因矩阵/2x2探针 + 帧时基线 frametime + β3归因三件套 + 单图常驻 定案D3D11_TEXTURE_BIT + 拆出renderer模块vkrenderer.h/cpp + Step2d-1 出向回写 ssr.vkout 1帧延迟零跨API栅栏 + POCB_DEV_FNS扩descriptor/sampler + v0.18.1 代码批: desc预检收敛copyResDescChecked(含ArraySize) / logLine常驻FILE / 挂载门日志 / 2d回写一次性门(28.7次每帧->1) / vkFreeDescriptorSets + v0.18.2: 2d-3 深度格式探测(格式列 R32F/R16F/R32TL × SHARED|NTHANDLE) / C-7 出向读回节流进门先++ + v0.18.3: 2d-4 路线1' KMT 探测(老式SHARED→KMT handle→VK导入实测) / C-8 格式探测结论行按第1格实测分支 / O-1 2c拷贝后 Flush 补每帧可见 + v0.18.4: O-1 正式修法 EVENT 闸(提交VK前等入向拷贝跑完, 12e 应全一致) + v0.18.5: 路线1' 导入分支落地(深度改单独SHARED老式handle→VK KMT 直入, [2d-5] 深度跨API三候选比对) + v0.18.6: SSR v1 shader 采样(ssr.vert/ssr.frag 全屏三角, descriptor+2 sampler+push constant 相机, 出向由 2d 原样拷改为 shader 产出, R4=反推inv投影, 门 ssr.v1/ssr.mode/ssr.fov/ssr.near/ssr.far/ssr.steps/ssr.dist/ssr.strength) + v0.18.7: A 平滑批(法线差分邻域 ssr.smooth + 反射 5tap 空间平滑 ssr.blur + 诊断 ssr.debug 0正常/1法线/2命中/3深度, push constant 48B→64B) + B 水色保留(第4张 SHARED 底色镜像 = 段16 后的 585 含水画面, 每帧特征B 在 2d 回写前抢一份, VK 侧 binding2 当合成底色, 门 ssr.base585) + v0.18.8: 正解B 段后水深当法线(第5张 SHARED 镜像 = 特征B 后第一次换绑时拷的 461, 段17 已写入真·水面深度, 520 照旧只当行进层级, 着色器按 zW>zPre 判水面像素, VK binding3 + p2.w, 门 ssr.wdep, debug 4段后水深/5水面像素) + v0.18.9: 585 纯反射层契约(段17 PS17586 反汇编坐实 out=mix(585,588@涟漪扭曲UV, w) 水面像素 585 占 ~90% ⇒ 585 只放一种反射: 去掉自算 fresnel 与 cubemap 掺底 ⇒ 治「倒影多层堆叠」; ssr.strength 语义改 **SSR 替换比 0..1**(0=原版 cubemap)) + 涟漪回注(段16 16 个 draw 按水面涟漪法线采 cubemap 写 585, 换内容等于把涟漪换掉 ⇒ 把 uBase 高频亮度结构乘回 SSR, 门 ssr.ripple, push constant 64B→80B p4) + v0.18.10: 回注调谐与自诊断(ssr.ripplesz 回注带宽 1..16px, 原写死 2px 只抓得住像素级噪点 ⇒ 波纹细小 / ssr.ripplemode 0=亮度调制 1=位移扭曲, 位移式的梯度取「1px 梯度-Rb 梯度」: 阶跃轮廓两支相近相减归 0 不印轮廓, 波纹波长≈2·Rb 才起效 ⇒ 天生只认 ripplesz 那一档 / debug 6=回注可视化 7=uBase 原样(定 ripplesz 的依据) / 段17 权重逐条算死 w≤0.095、段18 首笔绑定不读 585、321→324=ev39225 在段16 之前 ⇒ 排除双读与跨帧递归, 堆叠归因改走只动一个旋钮的隔离试验 strength=0→ripple=0→blur=0) + v0.18.11: 未命中回退源(ssr.edge 0=未命中一律回原版层(默认)/1=屏幕边缘延展; 归因: 扇形下半是射线飞出屏顶后拿 lastUV 做边缘延展, 采到的是岸边/树的原位画面(未镜像) 贴进水里 ⇒ 错位重影, 两侧第一跳就在屏外走的是原版层兜底才是「两边水纹正常」; 附带 hit 但 hitUV 贴屏边 4% 淡回原版层, 让扇形边界不过渡硬) + v0.18.12: 命中收紧与回注调谐(三张实跑图坐实归因后按用户反馈修三处: ①前景遮挡假命中 = 射线从原点往深处走碰不到站在它前面的人/石头, 但高度场在轮廓处突然变浅会被判成命中 ⇒ 身体轮廓糊进水里(人物身体一圈) ⇒ 命中加 sz<=P.z*0.98 守卫 ②二分 3->5 次 治倒影块状 ③位移幅度由写死 6px 改随 ripplesz 缩放 clamp(1.5*ripplesz,6,16) 默认 4px 仍=6px 治倒影区域水波较小) + v0.18.13: 法线差分轮廓守卫(水面法线只由「同样是水面」的邻点差出 ⇒ 单侧越界退成单侧差分 / 两侧越界走原退化兜底 ⇒ 回原版层; 实测三张跑图的「人物一圈」环宽中位数 = 4px = ssr.smooth 差分半径, 根因是邻点踩到前景人物/礁石的 3D 位置 ⇒ 叉出乱法线 ⇒ 反射方向被甩进屏内 ⇒ 平白多一次 hit ⇒ 采到对岸亮岩 = 白亮边) + v0.18.14: 输入端软限幅与幅度/增益解耦(§14.30.5 debug=6 实拍: 位移场电平随深度摆 6.7× —— 远处 41.8% 满格削顶/近处 51% 归零, 白饱和段 4.3~8px 而原版波长 λ≈30 ⇒ 细 4~7 倍 = 用户说的「倒影处波纹小而密集」; 根因 = 高光斑幅度比波大 ~8 倍 且冲激响应在 ±1、±Rb 各打满幅 ⇒ 位移场被亮斑劫持) 新增三旋钮: ssr.ripk 输入端软限幅阈值(取梯度前把每个采样点夹进本环 4 tap 均值 ±K, 0=关, 零额外取样 ⇒ 夹住后 |g1−g2| ≤ 4K 把场硬性封顶 40·K 不再整片削顶, 且阶跃两支同被夹住相减仍归 0 ⇒ 轮廓照旧不泄漏) + ssr.ripamp 位移幅度 px(0=自动 clamp(ripplesz*1.5,6,16)) + ssr.ripgain 梯度增益(0=自动 =10, 原写死 ×10); 三个默认 0 = 逐位等同 v0.18.13 零回归; push constant 80B→96B p5) + v0.18.15: 命中区高光回补(A 的正解, §14.30.7 坐实: strength=1 时 mix 100% 用 refl 顶掉 baseRGB ⇒ 585 层里游戏自算的白亮斑跟着一起没, 0.7 档 30% 回流 = 「有亮斑但不如正常亮」; §14.30.8 机位闸配对后再坐实 ripk 只削高补低 ±3~4pp、四框落差 4.55× 归 ripA 深度响应与 ripk 无关、ripamp 在 debug=6 分子分母精确约掉故测不到) 新增 ssr.v1det 回补量 0..1: 只在 SSR 真正接管的像素上 (wSsr = strength × 贴边淡出权重 bf) 把 baseRGB 比 refl 亮出来的那部分按 det 加回, 附绝对亮度门 smoothstep(0.35,0.85,luma) 防中等亮度整片倒回原版层削弱 SSR; miss ⇒ wSsr=0 ⇒ 纯原版逐位不动, det=0 ⇒ 逐位 = v0.18.14 零回归; 复用 p5 空槽, push constant 仍 96B; 附诊断 ssr.debug=8 单位回补量可视化 (Reinhard 灰度 y=l/(1+l), 公式与合成式逐项同式但**不乘 det** ⇒ v1det=0 也能拍, 一张图直接读出「哪里能加、能加多少」, 免去跨图差分 —— 实测 10 分钟光照漂移把水面外亮斑推 +16.6% 已盖过回补量本身); miss 像素真 cubemap 兜底 (B 的正解) 因 probe cube 是 D3D11 非共享对象、要另开跨 API 导入通路 ⇒ 拆 v0.18.16) + v0.18.16 (issue B / 队列 #2 落地, P1+P2 合并一发, 零渲染改动的部分只有 P1): **P1** = 探针 cube CPU 可读性与静态性探测 (probeDumpTick, 每帧喂入但只跑两次: STAGING CreateTexture2D + 逐 subresource CopyResource/Map/FNV1a, 第二次要隔 >=5s, 日志前缀 P1探针:, 两次签名一致 ⇒ 内容静态一次上传即可, 不一致 ⇒ 按代重传, 读回失败 ⇒ C1 不可行转 Plan B); **P2** = ssr.edge 扩第三档 2 = 未命中采真 cubemap 兜底 (ssr.frag miss 分支按 Rf=reflect(V,N) 采 binding4 samplerCube, 先判长度防 NaN; **只动 refl, 不碰 hit** ⇒ hit/miss 仍是纯几何, **不碰 wSsr** ⇒ v1det 回补在 miss 区照旧不进 = A 逐位不动; mode0 与无深度在前面就 oColor=base 提前返回, 到不了这个分支 ⇒ 透传契约不破; push constant 体积/语义位不变, **零新 ini 键**, 读入时把 ssr.edge 钳到 0..2) + **C1 上传通路** (main.cpp 把 STAGING 读回的 6 面紧凑缓冲交 vkrenderer, 看到 g_probeCpuTick 变就灌自建 R16G16B16A16_SFLOAT 6 面 CUBE_COMPATIBLE VkImage: HOST_VISIBLE|COHERENT 源 buffer -> vkCmdCopyBufferToImage (POCB_DEV_FNS 补这一条, core 1.0), 自带 command pool/fence **不借共用 pool** (借会把别人在录的命令一起 reset), 布局 UNDEFINED->TRANSFER_DST->SHADER_READ_ONLY, 描述符扩到 5 binding 且**常驻 1×1 占位 cube** 保证 samplerCube 描述符恒有效 —— 着色器静态引用它, 空 view = 未定义行为; g_ssrV1CubeSrc 与 g_probeCpuTick 都进 ssrV1Sig ⇒ 占位->真 cube 换档与内容换代都会自动重录+重填) ====");

	g_vtableLayer = iniFlag("vtable", true);
	if (!g_vtableLayer)
		logLine("PoC-A: ini vtable=0 → vtable 层停用 (不挂槽 8/22, 看门狗不打回), 只留方案B 函数级 detour —— 方案B 独立计数实验");

	InitializeCriticalSection(&g_cs);

	// SKSE 版本打包格式: MAKE_EXE_VERSION = (major<<24)|(minor<<16)|(build<<4)|sub
	const UInt32 rt = skse->runtimeVersion;
	const UInt32 sv = skse->skseVersion;
	{
		char buf[160];
		std::snprintf(buf, sizeof(buf),
		    "游戏版本: 0x%08X -> %u.%u.%u.%u | SKSE: 0x%08X -> %u.%u.%u.%u",
		    rt,
		    (rt >> 24) & 0xFF, (rt >> 16) & 0xFF, (rt >> 4) & 0xFFF, rt & 0xF,
		    sv,
		    (sv >> 24) & 0xFF, (sv >> 16) & 0xFF, (sv >> 4) & 0xFFF, sv & 0xF);
		logLine(buf);
	}

	// v1.2 核心: 同步安装 — SKSE 在主线程加载插件, Load 不返回游戏就无法继续初始化,
	// 由此保证钩子先于游戏交换链创建 (v1.1 后台线程安装曾输给渲染器初始化的竞态)。
	// Load 内阻塞有界: 等模块 ≤5s + 等 renderdoc ≤2s; 未就绪则退后台兜底线程。
	if (installHook(5000))
	{
		logLine("同步安装完成 (SKSE 加载线程内, 先于游戏渲染器初始化)");
		std::thread(watchdogThread).detach();
	}
	else
	{
		logLine("同步安装未完成 → 后台兜底线程");
		std::thread(fallbackInstallThread).detach();
	}
	logLine("就绪 — 不改变画面; 进游戏后观察本日志");
	return true;
}

} // extern "C"

