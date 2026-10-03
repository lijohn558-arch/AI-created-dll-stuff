/*
 * poc-presenter — PoC-A v1.7 (Present 拦截) + PoC-B v0.1 (Vulkan 注入), 插件版本 0.9.0
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
 * 构建: GitHub Actions (build.yml job "poc-presenter"), 本地不编译
 * 安装: poc-presenter.dll 放入 <游戏>/Data/SKSE/Plugins/
 */

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

// 与 capture-helper 共用同一份最小 ABI 声明 (单源, 避免漂移)
#include "..\capture-helper\skse_abi.h"

// PoC-B: Vulkan 注入用 (本机/CI 都没有 Vulkan SDK → 不链 vulkan-1.lib, 只要头文件 +
// 运行时 GetProcAddress 自己装; 头由 build.yml 额外 checkout KhronosGroup/Vulkan-Headers 提供)
#include <vulkan/vulkan.h>
// SPIR-V 字节 (由 tools\make_shaders.ps1 从 shaders\pocb.vert/.frag 生成, 随源码入库)
#include "pocb_shaders.h"

namespace {

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

void logLine(const std::string& msg)
{
	const std::string line = "[" + timestamp() + "] " + msg + "\r\n";
	OutputDebugStringA(line.c_str());
	if (!g_logPath.empty())
	{
		std::ofstream f(g_logPath, std::ios::app);
		if (f)
			f << line;
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

void registerSwp(void* obj, bool plus, const char* tag)
{
	if (!obj)
		return;
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
	patchSlotLocked(&vtbl[8], reinterpret_cast<void*>(&hookedPresent));
	if (g_swp[idx].has22)
		patchSlotLocked(&vtbl[22], reinterpret_cast<void*>(&hookedPresent1));
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

void notePresent(const char* via, IDXGISwapChain* sc)
{
	const uint64_t n = g_presentCount.fetch_add(1) + 1;
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
		notePresent("函数detour", sc);
	typedef HRESULT(STDMETHODCALLTYPE* Fn)(IDXGISwapChain*, UINT, UINT);
	return reinterpret_cast<Fn>(g_detP.resume)(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE detouredPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                                  const DXGI_PRESENT_PARAMETERS* params)
{
	if (!g_detP1.resume)
		return E_FAIL;
	if (!g_inVtableHook)
		notePresent("函数detour1", sc);
	typedef HRESULT(STDMETHODCALLTYPE* Fn)(IDXGISwapChain1*, UINT, UINT,
	                                       const DXGI_PRESENT_PARAMETERS*);
	return reinterpret_cast<Fn>(g_detP1.resume)(sc, sync, flags, params);
}

// ---------- PoC-B (v0.9.0): 在 Present 里跑 Vulkan, 并把像素写进呈现帧 ----------

// 目的 (docs/00 首周行动项 #4 第二环 / 风险项 #1 的"写"这半):
//   PoC-A 只证明"能拦 Present"(读); PoC-B 证明"能在拦到的 Present 里跑 Vulkan, 并把
//   Vulkan 渲出的像素写进游戏最终呈现的那一帧"(写) —— DX11→Vulkan 混合方案的前提。
//
// 路径 (刻意不开 Vulkan surface/swapchain, 不抢游戏窗口所有权):
//   Vulkan 离屏渲 512×512 (洋红清屏 + 三角形)
//     → vkCmdCopyImageToBuffer 读回 (持久映射 host 内存)
//     → UpdateSubresource 灌进我方 512×512 D3D11 纹理
//     → CopySubresourceRegion 拷进交换链 backbuffer 的 (16,16)
//     → 调原 Present
//   为什么不直接开 Vulkan swapchain:
//     1) 两个 swapchain 抢同一个 HWND, 会与游戏 / RenderDoc / GFE 争呈现所有权, 结果不可判;
//     2) 像素落在 D3D11 帧内 ⇒ F12 抓帧必然记录这次拷贝, rdc 提取链查得到 ——
//        "抓帧可查"本身就是 PoC-B 的验收项之一 (配对 harness 只认 D3D11 帧)。
//
// 失败策略: 任一步失败 ⇒ logLine + state=3 ⇒ 完全不注入, 游戏画面不受影响;
//           失败前已创建的对象不再回收 (进程内弃用, PoC 口径)。
// 逃生门:   <pluginDir>\poc-presenter.ini 写 vulkan=0 → 不重编译即可关掉注入。
// 规模:     每帧 1×512×512×4B = 1MB 读回 + 1MB 上传 + 一次 fence 等待 (耗时见日志)。

#define POCB_W 512
#define POCB_H 512
#define POCB_X 16
#define POCB_Y 16
#define POCB_BPP 4

// Vulkan 函数表分三层加载: 全局 (vkCreateInstance) → 实例 → 设备。
// 本机与 CI 都没有 Vulkan SDK ⇒ 不链 vulkan-1.lib, 全部 GetProcAddress 自装。
#define POCB_INST_FNS(X) \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkCreateDevice) \
	X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)

#define POCB_DEV_FNS(X) \
	X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
	X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
	X(vkCreateImageView) X(vkDestroyImageView) \
	X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
	X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkBindBufferMemory) \
	X(vkMapMemory) X(vkUnmapMemory) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
	X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
	X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
	X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandPool) \
	X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
	X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) X(vkCmdCopyImageToBuffer) \
	X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) X(vkQueueSubmit)

#define POCB_DECL_FN(n) PFN_##n n = nullptr;

struct PocbFns
{
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
	PFN_vkCreateInstance      CreateInstance = nullptr; // 全局级, 只此一个
	POCB_INST_FNS(POCB_DECL_FN)
	POCB_DEV_FNS(POCB_DECL_FN)
};

struct PocbCtx
{
	std::atomic<int> state{0}; // 0=未开始 1=初始化中 2=可用 3=失败已禁用
	// D3D11 侧
	ID3D11Device*        dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	ID3D11Texture2D*     tex = nullptr; // 我方 512×512 DEFAULT 纹理 (接收读回像素)
	DXGI_FORMAT          fmt = DXGI_FORMAT_UNKNOWN;
	// Vulkan 侧
	PocbFns          fns;
	VkInstance       inst = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice         vdev = VK_NULL_HANDLE;
	VkQueue          queue = VK_NULL_HANDLE;
	uint32_t         qfi = 0;
	VkImage          img = VK_NULL_HANDLE;
	VkDeviceMemory   imgMem = VK_NULL_HANDLE;
	VkImageView      view = VK_NULL_HANDLE;
	VkRenderPass     rp = VK_NULL_HANDLE;
	VkFramebuffer    fb = VK_NULL_HANDLE;
	VkShaderModule   vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	VkPipelineLayout pl = VK_NULL_HANDLE;
	VkPipeline       pipe = VK_NULL_HANDLE;
	VkCommandPool    pool = VK_NULL_HANDLE;
	VkCommandBuffer  cmd = VK_NULL_HANDLE;
	VkBuffer         rbuf = VK_NULL_HANDLE;
	VkDeviceMemory   rbufMem = VK_NULL_HANDLE;
	void*            mapped = nullptr;
	VkFence          fence = VK_NULL_HANDLE;
	// 统计
	uint64_t frames = 0;
	double   accMs = 0.0;
};
static PocbCtx g_pocb;

static std::string pocbCode(long v)
{
	char b[40];
	std::snprintf(b, sizeof(b), "%ld (0x%08lX)", v, static_cast<unsigned long>(v));
	return b;
}

// 失败统一出口: 记日志 + 关注入 (游戏画面从此不受本模块影响)
static void pocbFail(const char* what, long code)
{
	g_pocb.state.store(3);
	logLine(std::string("PoC-B 失败: ") + what + " code=" + pocbCode(code) + " — 已关闭注入, 游戏照常呈现");
}

static void pocbFail(const char* what)
{
	pocbFail(what, 0);
}

// 逃生门: <pluginDir>\poc-presenter.ini 里 vulkan=0 / pocb=0 关掉注入 (不重编译)
static bool pocbEnabled()
{
	static int cached = -1;
	if (cached >= 0)
		return cached == 1;
	cached = 1;
	std::ifstream f(pluginDir() + "\\poc-presenter.ini");
	std::string line;
	while (std::getline(f, line))
	{
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = lowerCopy(line.substr(0, eq));
		while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
			key.pop_back(); // 允许 "vulkan = 0" 这种带空格写法
		if (key != "vulkan" && key != "pocb")
			continue;
		for (const char ch : line.substr(eq + 1))
		{
			if (std::isspace(static_cast<unsigned char>(ch)))
				continue;
			cached = (ch == '0' || ch == 'f' || ch == 'F' || ch == 'n' || ch == 'N') ? 0 : 1;
			break;
		}
	}
	if (!cached)
		logLine("PoC-B: poc-presenter.ini 关闭了 vulkan 注入 (vulkan=0)");
	return cached == 1;
}

// 找满足属性的内存类型; want=0 表示只要类型位允许
static int pocbMemType(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
	{
		if ((bits & (1u << i)) != 0 && (mp.memoryTypes[i].propertyFlags & want) == want)
			return static_cast<int>(i);
	}
	return -1;
}

static bool pocbAllocMem(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkDeviceSize size,
                         VkMemoryPropertyFlags want, VkDeviceMemory* out, const char* what)
{
	int t = pocbMemType(mp, bits, want);
	if (t < 0)
		t = pocbMemType(mp, bits, 0); // 退而求其次: 任一可用类型
	if (t < 0)
	{
		pocbFail(what);
		return false;
	}
	VkMemoryAllocateInfo ai{};
	ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.allocationSize = size;
	ai.memoryTypeIndex = static_cast<uint32_t>(t);
	const VkResult vr = g_pocb.fns.vkAllocateMemory(g_pocb.vdev, &ai, nullptr, out);
	if (vr != VK_SUCCESS)
	{
		pocbFail(what, vr);
		return false;
	}
	return true;
}

// 一次性初始化: D3D11 口径 + Vulkan 全套离屏管线 (在首次 Present 时同步完成)
static bool pocbInit(IDXGISwapChain* sc)
{
	PocbCtx& c = g_pocb;
	PocbFns& fns = c.fns;
	logLine("PoC-B init: 开始 (首次 Present 触发, 一次性)");

	// --- D3D11 侧口径 ---
	HRESULT hr = sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&c.dev));
	if (FAILED(hr) || !c.dev)
	{
		pocbFail("交换链 GetDevice(ID3D11Device)", hr);
		return false;
	}
	c.dev->GetImmediateContext(&c.ctx);
	if (!c.ctx)
	{
		pocbFail("ID3D11Device::GetImmediateContext");
		return false;
	}
	ID3D11Texture2D* bb = nullptr;
	hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
	if (FAILED(hr) || !bb)
	{
		pocbFail("IDXGISwapChain::GetBuffer(0)", hr);
		return false;
	}
	D3D11_TEXTURE2D_DESC bd{};
	bb->GetDesc(&bd);
	bb->Release();
	logLine("PoC-B init: backbuffer = " + std::to_string(bd.Width) + "x" + std::to_string(bd.Height) +
	        " format=" + std::to_string(static_cast<int>(bd.Format)) + " (期望28=R8G8B8A8_UNORM)");
	if (bd.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
	{
		pocbFail("backbuffer 不是 R8G8B8A8_UNORM — 字节布局假设不成立", static_cast<long>(bd.Format));
		return false;
	}
	if (bd.SampleDesc.Count != 1)
	{
		// CopySubresourceRegion 不支持 跨 采样数 拷贝 (MSAA backbuffer 走不通这条路)
		pocbFail("backbuffer 是多重采样 (CopySubresourceRegion 跨采样数无效)", static_cast<long>(bd.SampleDesc.Count));
		return false;
	}
	if (bd.Width < POCB_X + POCB_W || bd.Height < POCB_Y + POCB_H)
	{
		pocbFail("backbuffer 比注入矩形还小");
		return false;
	}
	c.fmt = bd.Format;
	D3D11_TEXTURE2D_DESC td{};
	td.Width = POCB_W;
	td.Height = POCB_H;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = bd.Format;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	hr = c.dev->CreateTexture2D(&td, nullptr, &c.tex);
	if (FAILED(hr) || !c.tex)
	{
		pocbFail("ID3D11Device::CreateTexture2D (注入用 512×512)", hr);
		return false;
	}

	// --- Vulkan loader ---
	HMODULE hvk = LoadLibraryW(L"vulkan-1.dll");
	if (!hvk)
	{
		pocbFail("LoadLibrary vulkan-1.dll (驱动未装 Vulkan loader?)", static_cast<long>(GetLastError()));
		return false;
	}
	fns.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(hvk, "vkGetInstanceProcAddr"));
	fns.CreateInstance = fns.GetInstanceProcAddr
	    ? reinterpret_cast<PFN_vkCreateInstance>(fns.GetInstanceProcAddr(nullptr, "vkCreateInstance"))
	    : nullptr;
	if (!fns.CreateInstance)
	{
		pocbFail("取 vkCreateInstance 失败");
		return false;
	}

	VkApplicationInfo ai{};
	ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	ai.pApplicationName = "poc-presenter PoC-B";
	ai.applicationVersion = 1;
	ai.pEngineName = "poc-presenter";
	ai.engineVersion = 1;
	ai.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{};
	ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo = &ai;
	VkResult vr = fns.CreateInstance(&ici, nullptr, &c.inst);
	if (vr == VK_ERROR_INCOMPATIBLE_DRIVER)
	{
		ai.apiVersion = VK_API_VERSION_1_0;
		vr = fns.CreateInstance(&ici, nullptr, &c.inst);
		logLine("PoC-B init: 1.1 被拒(INCOMPATIBLE_DRIVER) → 回退 1.0, 放弃 LUID 匹配");
	}
	if (vr != VK_SUCCESS || !c.inst)
	{
		pocbFail("vkCreateInstance", vr);
		return false;
	}
#define POCB_LOAD_I(n) fns.n = reinterpret_cast<PFN_##n>(fns.GetInstanceProcAddr(c.inst, #n));
	POCB_INST_FNS(POCB_LOAD_I)
#undef POCB_LOAD_I
	if (!fns.vkEnumeratePhysicalDevices || !fns.vkCreateDevice || !fns.vkGetDeviceQueue)
	{
		pocbFail("实例级函数表不完整 (vkGetInstanceProcAddr 返回空)");
		return false;
	}

	// --- 物理设备: 优先选与 D3D11 适配器同 LUID 的那块 (多 GPU 时才不是同一块) ---
	uint32_t ndev = 0;
	fns.vkEnumeratePhysicalDevices(c.inst, &ndev, nullptr);
	if (ndev == 0)
	{
		pocbFail("vkEnumeratePhysicalDevices 无设备");
		return false;
	}
	if (ndev > 8)
		ndev = 8;
	VkPhysicalDevice devs[8]{};
	fns.vkEnumeratePhysicalDevices(c.inst, &ndev, devs); // 3 参: (instance, count*, devices*)

	LUID adapterLuid{};
	bool haveLuid = false;
	IDXGIDevice* gdev = nullptr;
	if (SUCCEEDED(c.dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&gdev))) && gdev)
	{
		IDXGIAdapter* ad = nullptr;
		if (SUCCEEDED(gdev->GetAdapter(&ad)) && ad)
		{
			DXGI_ADAPTER_DESC d{}; // GetDesc (v0 接口) 就有 AdapterLuid, 不必 QI 到 Adapter1
			if (SUCCEEDED(ad->GetDesc(&d)))
			{
				adapterLuid = d.AdapterLuid;
				haveLuid = true;
			}
			ad->Release();
		}
		gdev->Release();
	}
	VkPhysicalDevice chosen = devs[0];
	bool luidHit = false;
	if (haveLuid && fns.vkGetPhysicalDeviceProperties2)
	{
		for (uint32_t i = 0; i < ndev; ++i)
		{
			VkPhysicalDeviceProperties2 p2{};
			p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			VkPhysicalDeviceIDProperties idp{};
			idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
			p2.pNext = &idp;
			fns.vkGetPhysicalDeviceProperties2(devs[i], &p2);
			if (idp.deviceLUIDValid && std::memcmp(&adapterLuid, idp.deviceLUID, sizeof(LUID)) == 0)
			{
				chosen = devs[i];
				luidHit = true;
				break;
			}
		}
	}
	c.phys = chosen;
	VkPhysicalDeviceProperties props{};
	fns.vkGetPhysicalDeviceProperties(chosen, &props);
	logLine(std::string("PoC-B init: GPU = \"") + props.deviceName + "\" LUID匹配=" +
	        (luidHit ? "是" : "否") + " apiVer=" +
	        std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." +
	        std::to_string(VK_API_VERSION_MINOR(props.apiVersion)));

	// --- 队列族 + 设备 ---
	uint32_t nq = 0;
	fns.vkGetPhysicalDeviceQueueFamilyProperties(chosen, &nq, nullptr);
	if (nq > 64)
		nq = 64;
	VkQueueFamilyProperties qprops[64]{};
	fns.vkGetPhysicalDeviceQueueFamilyProperties(chosen, &nq, qprops);
	uint32_t qf = UINT32_MAX;
	for (uint32_t i = 0; i < nq; ++i)
	{
		if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
		{
			qf = i;
			break;
		}
	}
	if (qf == UINT32_MAX)
	{
		pocbFail("物理设备没有图形队列族");
		return false;
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci{};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = qf;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci{};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	vr = fns.vkCreateDevice(chosen, &dci, nullptr, &c.vdev);
	if (vr != VK_SUCCESS || !c.vdev)
	{
		pocbFail("vkCreateDevice", vr);
		return false;
	}
#define POCB_LOAD_D(n) fns.n = reinterpret_cast<PFN_##n>(fns.vkGetDeviceProcAddr(c.vdev, #n));
	POCB_DEV_FNS(POCB_LOAD_D)
#undef POCB_LOAD_D
	if (!fns.vkCreateImage || !fns.vkQueueSubmit || !fns.vkCmdDraw || !fns.vkGetDeviceQueue)
	{
		pocbFail("设备级函数表不完整 (vkGetDeviceProcAddr 返回空)");
		return false;
	}
	c.qfi = qf;
	fns.vkGetDeviceQueue(c.vdev, qf, 0, &c.queue); // 必须在上面 load 之后 (v0.9.0 踩过: 先调后 load = 空指针)

	VkPhysicalDeviceMemoryProperties mem{};
	fns.vkGetPhysicalDeviceMemoryProperties(chosen, &mem);

	// --- 离屏图 (COLOR_ATTACHMENT | TRANSFER_SRC, 一次 renderpass 清屏+画三角形后读回) ---
	VkImageCreateInfo imci{};
	imci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imci.imageType = VK_IMAGE_TYPE_2D;
	imci.format = VK_FORMAT_R8G8B8A8_UNORM;
	imci.extent = VkExtent3D{POCB_W, POCB_H, 1};
	imci.mipLevels = 1;
	imci.arrayLayers = 1;
	imci.samples = VK_SAMPLE_COUNT_1_BIT;
	imci.tiling = VK_IMAGE_TILING_OPTIMAL;
	imci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	imci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	vr = fns.vkCreateImage(c.vdev, &imci, nullptr, &c.img);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkCreateImage", vr);
		return false;
	}
	VkMemoryRequirements ireq{};
	fns.vkGetImageMemoryRequirements(c.vdev, c.img, &ireq);
	if (!pocbAllocMem(mem, ireq.memoryTypeBits, ireq.size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	                  &c.imgMem, "离屏图 vkAllocateMemory"))
		return false;
	if (fns.vkBindImageMemory(c.vdev, c.img, c.imgMem, 0) != VK_SUCCESS)
	{
		pocbFail("vkBindImageMemory (离屏图)");
		return false;
	}
	VkImageViewCreateInfo vci{};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = c.img;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8G8B8A8_UNORM;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vr = fns.vkCreateImageView(c.vdev, &vci, nullptr, &c.view);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkCreateImageView", vr);
		return false;
	}

	// --- renderpass: UNDEFINED 清屏 → 转到 TRANSFER_SRC_OPTIMAL 供读回 ---
	VkAttachmentDescription att{};
	att.format = VK_FORMAT_R8G8B8A8_UNORM;
	att.samples = VK_SAMPLE_COUNT_1_BIT;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	VkAttachmentReference cref{};
	cref.attachment = 0;
	cref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkSubpassDescription sub{};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &cref;
	VkSubpassDependency deps[2]{};
	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[0].srcAccessMask = 0;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].srcSubpass = 0;
	deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	VkRenderPassCreateInfo rpci{};
	rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpci.attachmentCount = 1;
	rpci.pAttachments = &att;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	rpci.dependencyCount = 2;
	rpci.pDependencies = deps;
	vr = fns.vkCreateRenderPass(c.vdev, &rpci, nullptr, &c.rp);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkCreateRenderPass", vr);
		return false;
	}
	VkFramebufferCreateInfo fci{};
	fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fci.renderPass = c.rp;
	fci.attachmentCount = 1;
	fci.pAttachments = &c.view;
	fci.width = POCB_W;
	fci.height = POCB_H;
	fci.layers = 1;
	vr = fns.vkCreateFramebuffer(c.vdev, &fci, nullptr, &c.fb);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkCreateFramebuffer", vr);
		return false;
	}

	// --- 着色器 (SPIR-V 字节来自 pocb_shaders.h) ---
	VkShaderModuleCreateInfo smv{};
	smv.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smv.codeSize = sizeof(kPocbVertSpv);
	smv.pCode = kPocbVertSpv;
	vr = fns.vkCreateShaderModule(c.vdev, &smv, nullptr, &c.vs);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkCreateShaderModule (vertex)", vr);
		return false;
	}
	VkShaderModuleCreateInfo smf{};
	smf.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smf.codeSize = sizeof(kPocbFragSpv);
	smf.pCode = kPocbFragSpv;
	vr = fns.vkCreateShaderModule(c.vdev, &smf, nullptr, &c.fs);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkCreateShaderModule (fragment)", vr);
		return false;
	}
	VkPipelineLayoutCreateInfo plci{};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	vr = fns.vkCreatePipelineLayout(c.vdev, &plci, nullptr, &c.pl);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkCreatePipelineLayout", vr);
		return false;
	}

	VkPipelineShaderStageCreateInfo stg[2]{};
	stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stg[0].module = c.vs;
	stg[0].pName = "main";
	stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stg[1].module = c.fs;
	stg[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vin{};
	vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; // 无顶点缓冲: gl_VertexIndex 取常量
	VkPipelineInputAssemblyStateCreateInfo ias{};
	ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps{};
	vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vps.viewportCount = 1;
	vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo cbs{};
	cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cbs.attachmentCount = 1;
	cbs.pAttachments = &cba;
	VkDynamicState dyn[2]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dss{};
	dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dss.dynamicStateCount = 2;
	dss.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo gpci{};
	gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gpci.stageCount = 2;
	gpci.pStages = stg;
	gpci.pVertexInputState = &vin;
	gpci.pInputAssemblyState = &ias;
	gpci.pViewportState = &vps;
	gpci.pRasterizationState = &rs;
	gpci.pMultisampleState = &ms;
	gpci.pColorBlendState = &cbs;
	gpci.pDynamicState = &dss;
	gpci.layout = c.pl;
	gpci.renderPass = c.rp;
	gpci.subpass = 0;
	vr = fns.vkCreateGraphicsPipelines(c.vdev, VK_NULL_HANDLE, 1, &gpci, nullptr, &c.pipe);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkCreateGraphicsPipelines", vr);
		return false;
	}

	// --- 读回 buffer (host 可见+一致, 一次 map 长期持有) ---
	VkBufferCreateInfo bci{};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = static_cast<VkDeviceSize>(POCB_W) * POCB_H * POCB_BPP;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	vr = fns.vkCreateBuffer(c.vdev, &bci, nullptr, &c.rbuf);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkCreateBuffer (读回)", vr);
		return false;
	}
	VkMemoryRequirements breq{};
	fns.vkGetBufferMemoryRequirements(c.vdev, c.rbuf, &breq);
	if (!pocbAllocMem(mem, breq.memoryTypeBits, breq.size,
	                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                  &c.rbufMem, "读回 buffer vkAllocateMemory"))
		return false;
	if (fns.vkBindBufferMemory(c.vdev, c.rbuf, c.rbufMem, 0) != VK_SUCCESS)
	{
		pocbFail("vkBindBufferMemory (读回)");
		return false;
	}
	vr = fns.vkMapMemory(c.vdev, c.rbufMem, 0, VK_WHOLE_SIZE, 0, &c.mapped);
	if (vr != VK_SUCCESS || !c.mapped)
	{
		pocbFail("vkMapMemory (读回)", vr);
		return false;
	}

	// --- 命令: 录一次, 每帧重复提交 (renderpass initialLayout=UNDEFINED 允许逐帧丢弃重清) ---
	VkCommandPoolCreateInfo cpci{};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.queueFamilyIndex = qf;
	vr = fns.vkCreateCommandPool(c.vdev, &cpci, nullptr, &c.pool);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkCreateCommandPool", vr);
		return false;
	}
	VkCommandBufferAllocateInfo cbai{};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandPool = c.pool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	vr = fns.vkAllocateCommandBuffers(c.vdev, &cbai, &c.cmd);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkAllocateCommandBuffers", vr);
		return false;
	}
	VkCommandBufferBeginInfo cbBegin{};
	cbBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbBegin.flags = 0; // 可重复提交 (ONE_TIME_SUBMIT 只能提一次)
	if (fns.vkBeginCommandBuffer(c.cmd, &cbBegin) != VK_SUCCESS)
	{
		pocbFail("vkBeginCommandBuffer");
		return false;
	}
	// 洋红清屏 —— 像素探针的哨兵色 (游戏里不可能出现的 255,0,255)
	VkClearValue clr{};
	clr.color.float32[0] = 1.0f;
	clr.color.float32[1] = 0.0f;
	clr.color.float32[2] = 1.0f;
	clr.color.float32[3] = 1.0f;
	VkRenderPassBeginInfo rpbi{};
	rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpbi.renderPass = c.rp;
	rpbi.framebuffer = c.fb;
	rpbi.renderArea = VkRect2D{VkOffset2D{0, 0}, VkExtent2D{POCB_W, POCB_H}};
	rpbi.clearValueCount = 1;
	rpbi.pClearValues = &clr;
	fns.vkCmdBeginRenderPass(c.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	fns.vkCmdBindPipeline(c.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipe);
	VkViewport vp{};
	vp.x = 0.0f;
	vp.y = 0.0f;
	vp.width = static_cast<float>(POCB_W);
	vp.height = static_cast<float>(POCB_H);
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;
	fns.vkCmdSetViewport(c.cmd, 0, 1, &vp);
	VkRect2D sci{VkOffset2D{0, 0}, VkExtent2D{POCB_W, POCB_H}};
	fns.vkCmdSetScissor(c.cmd, 0, 1, &sci);
	fns.vkCmdDraw(c.cmd, 3, 1, 0, 0); // 无顶点缓冲的三角形
	fns.vkCmdEndRenderPass(c.cmd);
	VkBufferImageCopy bic{};
	bic.bufferOffset = 0;
	bic.bufferRowLength = 0;   // 0 = 紧凑排布 (512×4 字节/行)
	bic.bufferImageHeight = 0;
	bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	bic.imageOffset = VkOffset3D{0, 0, 0};
	bic.imageExtent = VkExtent3D{POCB_W, POCB_H, 1};
	fns.vkCmdCopyImageToBuffer(c.cmd, c.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.rbuf, 1, &bic);
	if (fns.vkEndCommandBuffer(c.cmd) != VK_SUCCESS)
	{
		pocbFail("vkEndCommandBuffer");
		return false;
	}
	VkFenceCreateInfo fci2{};
	fci2.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fci2.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	vr = fns.vkCreateFence(c.vdev, &fci2, nullptr, &c.fence);
	if (vr != VK_SUCCESS)
	{
		pocbFail("vkCreateFence", vr);
		return false;
	}

	char sum[320];
	std::snprintf(sum, sizeof(sum),
	    "PoC-B init 完成: 离屏 %dx%d (洋红清屏+三角形) → 读回 → CopySubresourceRegion 到 backbuffer (%d,%d)",
	    POCB_W, POCB_H, POCB_X, POCB_Y);
	logLine(sum);
	return true;
}

// 每帧: 提交一次 Vulkan 命令 → fence 等待 → 读回像素 → 拷进 backbuffer
static void pocbInject(PocbCtx& c, IDXGISwapChain* sc)
{
	const auto t0 = std::chrono::steady_clock::now();

	// 每帧取一次 backbuffer (ResizeBuffers 后对象会换, 缓存会悬垂 → 每帧取最稳)
	ID3D11Texture2D* bb = nullptr;
	HRESULT hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
	if (FAILED(hr) || !bb)
	{
		pocbFail("运行期 IDXGISwapChain::GetBuffer(0)", hr);
		return;
	}
	D3D11_TEXTURE2D_DESC bd{};
	bb->GetDesc(&bd);
	if (bd.Format != c.fmt || bd.SampleDesc.Count != 1 ||
	    bd.Width < POCB_X + POCB_W || bd.Height < POCB_Y + POCB_H)
	{
		bb->Release();
		logLine("PoC-B: backbuffer 口径变化 (format=" + std::to_string(static_cast<int>(bd.Format)) +
		        " " + std::to_string(bd.Width) + "x" + std::to_string(bd.Height) +
		        " samples=" + std::to_string(bd.SampleDesc.Count) + ") → 本帧跳过注入");
		return;
	}

	c.fns.vkResetFences(c.vdev, 1, &c.fence);
	VkSubmitInfo si{};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &c.cmd;
	VkResult vr = c.fns.vkQueueSubmit(c.queue, 1, &si, c.fence);
	if (vr == VK_SUCCESS)
		vr = c.fns.vkWaitForFences(c.vdev, 1, &c.fence, VK_TRUE, UINT64_MAX);
	if (vr != VK_SUCCESS)
	{
		bb->Release();
		pocbFail("vkQueueSubmit / vkWaitForFences", vr);
		return;
	}

	c.ctx->UpdateSubresource(c.tex, 0, nullptr, c.mapped, POCB_W * POCB_BPP, 0);
	D3D11_BOX box{0, 0, 0, static_cast<UINT>(POCB_W), static_cast<UINT>(POCB_H), 1};
	c.ctx->CopySubresourceRegion(bb, 0, POCB_X, POCB_Y, 0, c.tex, 0, &box);
	bb->Release();

	const auto t1 = std::chrono::steady_clock::now();
	const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	c.frames++;
	c.accMs += ms;
	if (c.frames == 1)
	{
		char b[224];
		std::snprintf(b, sizeof(b),
		    "PoC-B 第 1 帧注入: 渲染+读回+拷贝 %.2f ms → CopySubresourceRegion(%d,%d %dx%d) 已提交",
		    ms, POCB_X, POCB_Y, POCB_W, POCB_H);
		logLine(b);
	}
	else if (c.frames % 600 == 0)
	{
		char b[160];
		std::snprintf(b, sizeof(b), "PoC-B 注入 %llu 帧, 累计均值 %.2f ms/帧 (本帧 %.2f ms)",
		    static_cast<unsigned long long>(c.frames), c.accMs / static_cast<double>(c.frames), ms);
		logLine(b);
	}
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
	             : " ≠我们的Present钩子 (尚未挂钩 → 随后就地挂)"));
	if (memReadable(gv, 23 * sizeof(void*)))
		logLine(tg + "vtbl[22]=" + hexOf(gv[22]) + " 来自 " + modulePathOf(gv[22]) +
		        (gv[22] == reinterpret_cast<void*>(&hookedPresent1)
		             ? " ==我们的Present1钩子 ✓"
		             : " ≠我们的Present1钩子"));
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
		logLine("登记完成: 交换链 vtable " + std::to_string(swpPatched) + " 个 (含 Present 挂钩), " +
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
	for (int i = 0; i < g_swpN; ++i)
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

} // namespace

// ---------- DLL 入口 ----------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_hModule = hModule;
		DisableThreadLibraryCalls(hModule);
	}
	return TRUE;
}

// ---------- SKSE 插件入口 ----------

extern "C" {

__declspec(dllexport) bool SKSEPlugin_Query(const SKSEInterface* skse, PluginInfo* info)
{
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "PocPresenter";
	info->version = (0u << 16) | (9u << 8) | 0u; // 0.9.0

	if (skse->isEditor)  // 只进游戏本体, 不进 Creation Kit
		return false;

	return true;
}

__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* skse)
{
	g_logPath = pluginDir() + "\\poc-presenter.log";
	logLine("==== poc-presenter v0.9.0 (PoC-A v1.7 验收通过 + PoC-B v0.1: Vulkan 离屏渲染 → 读回 → 注入 backbuffer) ====");

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
