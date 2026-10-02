/*
 * poc-presenter — PoC-A v1.1 (插件版本 0.2.0): SKSE 插件载体的 Present Hook 验证
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
 *      每张不同 vtable 都挂槽 4 (Present); 经 1 口径见过的 vtable 额外挂槽 18 (Present1,
 *      越界保护: 只有 QI 上 1 的 vtable 才读 18);
 *   2. 工厂拦截: 挂我们工厂 vtable 的槽 10 (CreateSwapChain)、槽 14/15/17 (ForHwnd /
 *      ForCoreWindow / ForComposition) —— 游戏经同款工厂创建交换链时, 拿到游戏自己的
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
 *      游戏要的那张类也在登记之列; QI 向上 Factory2 成功即布局含槽14/15/17 (安全升级)。
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
 * 钩子转调: 按"调用方 vtable 地址"查登记表取原函数 —— 多张 vtable 各存各的原值。
 * 线程安全: 登记表/槽位写入全部走同一把 CRITICAL_SECTION (可重入)。
 *
 * 验收标准 (本地游戏内, 日志 = <游戏>/Data/SKSE/Plugins/poc-presenter.log):
 *   [..] renderdoc.dll = ...            (in-app 加载状态)
 *   [..] dummy1 ... / dummy2 swapchain ...
 *   [..] dummy1(ForHwnd): vtable=0x... 来自 ...      (vtable 本体所在模块, 判包装类)
 *   [..] ...: slot4 原值=0x... 来自 ...
 *   [..] 登记完成: 交换链 vtable N 个, 工厂 vtable M 个
 *   [..] ★ 工厂拦截 CreateSwapChain... → swapchain=0x...   (可选, 视游戏创建路径)
 *   [..] 第 1 次 Present (经 Present): ... ← 关键行 (证明拦到游戏 Present)
 *   [..] Present 已开始触发 — PoC-A 验收通过
 *   [..] Present 计数 600  近600帧 xx.x FPS          (周期行)
 *   且游戏画面完全正常 (本 PoC 不改变呈现)。
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
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

// 与 capture-helper 共用同一份最小 ABI 声明 (单源, 避免漂移)
#include "..\capture-helper\skse_abi.h"

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

// ---------- 登记表 (按调用方 vtable 查原函数) ----------

struct SwpEntry
{
	void** vtbl;
	void*  orig4;   // Present
	void*  orig18;  // Present1 (仅经 1+ 口径见过的 vtable 才有)
	bool   has18;
};

struct FacEntry
{
	void** vtbl;
	void*  o10;  // CreateSwapChain
	void*  o14;  // CreateSwapChainForHwnd
	void*  o15;  // CreateSwapChainForCoreWindow
	void*  o17;  // CreateSwapChainForComposition
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

void* lookupPresentOrig4(void** vtbl)
{
	for (int i = 0; i < g_swpN; ++i)
		if (g_swp[i].vtbl == vtbl)
			return g_swp[i].orig4;
	return nullptr;
}

void* lookupPresentOrig18(void** vtbl)
{
	for (int i = 0; i < g_swpN; ++i)
		if (g_swp[i].vtbl == vtbl && g_swp[i].has18)
			return g_swp[i].orig18;
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
		case 14: return g_fac[i].o14;
		case 15: return g_fac[i].o15;
		case 17: return g_fac[i].o17;
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
		void* o4 = vtbl[4];
		logLine(std::string(tag) + ": vtable=" + hexOf(vtbl) + " 来自 " + modulePathOf(vtbl));
		logLine(std::string(tag) + ": slot4 原值=" + hexOf(o4) + " 来自 " + modulePathOf(o4));
		if (!isDxgiFamily(o4))
		{
			logLine(std::string(tag) + ": slot4 原值不在 dxgi/renderdoc — 跳过该 vtable");
			LeaveCriticalSection(&g_cs);
			return;
		}
		idx = g_swpN++;
		g_swp[idx].vtbl = vtbl;
		g_swp[idx].orig4 = o4;
		g_swp[idx].orig18 = nullptr;
		g_swp[idx].has18 = false;
	}
	else
	{
		// 匹配路径原先是静默的 — v1.4 补日志: 游戏交换链若与 dummy 同类, 此行即证据
		logLine(std::string(tag) + ": vtable 已登记 (与既有同类) " + hexOf(vtbl));
	}
	if (plus && !g_swp[idx].has18)
	{
		// 只有确认实现 1 及以上接口的 vtable 才安全读槽 18 (布局至少 19 项)
		void* o18 = vtbl[18];
		logLine(std::string(tag) + ": slot18 原值=" + hexOf(o18) + " 来自 " + modulePathOf(o18));
		if (isDxgiFamily(o18))
		{
			g_swp[idx].orig18 = o18;
			g_swp[idx].has18 = true;
		}
		else
		{
			logLine(std::string(tag) + ": slot18 原值不在 dxgi/renderdoc — 不挂 Present1");
		}
	}
	patchSlotLocked(&vtbl[4], reinterpret_cast<void*>(&hookedPresent));
	if (g_swp[idx].has18)
		patchSlotLocked(&vtbl[18], reinterpret_cast<void*>(&hookedPresent1));
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
		idx = g_facN++;
		g_fac[idx].vtbl = vtbl;
		g_fac[idx].o10 = nullptr;
		g_fac[idx].o14 = nullptr;
		g_fac[idx].o15 = nullptr;
		g_fac[idx].o17 = nullptr;
		logLine(std::string(tag) + ": factory vtable=" + hexOf(vtbl) + " 来自 " + modulePathOf(vtbl));
	}
	FacEntry& e = g_fac[idx];
	tryFacSlot(vtbl, 10, &e.o10, reinterpret_cast<void*>(&hookedCreateSwapChain), "CreateSwapChain", tag);
	if (plus2)  // 槽 14/15/17 只存在于 Factory2 布局, 防越界
	{
		tryFacSlot(vtbl, 14, &e.o14, reinterpret_cast<void*>(&hookedCreateSwapChainForHwnd),
		           "CreateSwapChainForHwnd", tag);
		tryFacSlot(vtbl, 15, &e.o15, reinterpret_cast<void*>(&hookedCreateSwapChainForCoreWindow),
		           "CreateSwapChainForCoreWindow", tag);
		tryFacSlot(vtbl, 17, &e.o17, reinterpret_cast<void*>(&hookedCreateSwapChainForComposition),
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
	if (FAILED(sc->GetDesc(&d)))   // 槽 8 未挂钩, 可安全直调
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

// ---------- 钩子本体 ----------

HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
	notePresent("Present", sc);
	void** vtbl = *reinterpret_cast<void***>(sc);
	void* orig = nullptr;
	EnterCriticalSection(&g_cs);
	orig = lookupPresentOrig4(vtbl);
	LeaveCriticalSection(&g_cs);
	if (!orig)
	{
		logLine("!! hookedPresent: vtable " + hexOf(vtbl) + " 无登记原函数 — 返回 E_FAIL");
		return E_FAIL;
	}
	return reinterpret_cast<Present_t>(orig)(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hookedPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                          const DXGI_PRESENT_PARAMETERS* params)
{
	notePresent("Present1", sc);
	void** vtbl = *reinterpret_cast<void***>(sc);
	void* orig = nullptr;
	EnterCriticalSection(&g_cs);
	orig = lookupPresentOrig18(vtbl);
	LeaveCriticalSection(&g_cs);
	if (!orig)
	{
		logLine("!! hookedPresent1: vtable " + hexOf(vtbl) + " 无登记原函数 — 返回 E_FAIL");
		return E_FAIL;
	}
	return reinterpret_cast<Present1_t>(orig)(sc, sync, flags, params);
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
		g_gameSc.store(*out);
		registerSwp(*out, false, "★游戏 CreateSwapChain");
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
	orig = lookupFacOrig(vtbl, 14);
	LeaveCriticalSection(&g_cs);
	if (!orig)
		return E_FAIL;
	HRESULT hr = reinterpret_cast<CreateSwapChainForHwnd_t>(orig)(self, dev, hwnd, desc, fs, restrictOut, out);
	if (SUCCEEDED(hr) && out && *out)
	{
		logLine("★ 工厂拦截 CreateSwapChainForHwnd → swapchain=" + hexOf(*out) + " | " + swapDesc(*out));
		g_gameSc.store(*out);
		registerSwp(*out, true, "★游戏 CreateSwapChainForHwnd");
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
	orig = lookupFacOrig(vtbl, 15);
	LeaveCriticalSection(&g_cs);
	if (!orig)
		return E_FAIL;
	HRESULT hr = reinterpret_cast<CreateSwapChainForCoreWindow_t>(orig)(self, dev, window, desc, restrictOut, out);
	if (SUCCEEDED(hr) && out && *out)
	{
		logLine("★ 工厂拦截 CreateSwapChainForCoreWindow → swapchain=" + hexOf(*out) + " | " + swapDesc(*out));
		g_gameSc.store(*out);
		registerSwp(*out, true, "★游戏 CreateSwapChainForCoreWindow");
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
	orig = lookupFacOrig(vtbl, 17);
	LeaveCriticalSection(&g_cs);
	if (!orig)
		return E_FAIL;
	HRESULT hr = reinterpret_cast<CreateSwapChainForComposition_t>(orig)(self, dev, desc, out);
	if (SUCCEEDED(hr) && out && *out)
	{
		logLine("★ 工厂拦截 CreateSwapChainForComposition → swapchain=" + hexOf(*out) + " | " + swapDesc(*out));
		g_gameSc.store(*out);
		registerSwp(*out, true, "★游戏 CreateSwapChainForComposition");
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
	// QI 向上 Factory2: QI 成功即该指针布局保证含槽 14/15/17, 升级安全
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

	// 7) 收集并挂所有交换链 vtable 变体 (槽 4 与安全的槽 18)
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
					logLine("设备链工厂 IID" + std::to_string(i) + " = " + hexOf(f) + " 来自 " + modulePathOf(f));
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
	//    plus2 仅对 IID2 口径为 true (该布局保证含槽14/15/17, 防越界)
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
		for (int i = 0; i < g_swpN; ++i)
			if (g_swp[i].orig4)
				++swpPatched;
		logLine("登记完成: 交换链 vtable " + std::to_string(swpPatched) + " 个 (含 Present 挂钩), " +
		        "工厂 vtable " + std::to_string(g_facN) + " 个 (含创建方法挂钩); Present 钩子=" +
		        hexOf(reinterpret_cast<const void*>(&hookedPresent)));
	}

	// v1.5 探针 B: 原实现前 32 字节 — 为函数级 detour 选安全窃取长度
	if (g_swpN > 0)
	{
		if (g_swp[0].orig4)
			logLine("探针: Present 原实现前32字节 @ " + hexOf(g_swp[0].orig4) + " = " +
			        hexBytes(g_swp[0].orig4, 32));
		if (g_swp[0].orig18)
			logLine("探针: Present1 原实现前32字节 @ " + hexOf(g_swp[0].orig18) + " = " +
			        hexBytes(g_swp[0].orig18, 32));
	}
	if (g_facN > 0 && g_fac[0].o10)
		logLine("探针: CreateSwapChain 原实现前32字节 @ " + hexOf(g_fac[0].o10) + " = " +
		        hexBytes(g_fac[0].o10, 32));
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
		if (e.vtbl[4] != reinterpret_cast<void*>(&hookedPresent))
		{
			logLine("!! vtable " + hexOf(e.vtbl) + " slot4 被改写: 现=" + hexOf(e.vtbl[4]) +
			        " 来自 " + modulePathOf(e.vtbl[4]) + " → 打回我们的钩子");
			patchSlotLocked(&e.vtbl[4], reinterpret_cast<void*>(&hookedPresent));
		}
		if (e.has18 && e.vtbl[18] != reinterpret_cast<void*>(&hookedPresent1))
		{
			logLine("!! vtable " + hexOf(e.vtbl) + " slot18 被改写: 现=" + hexOf(e.vtbl[18]) +
			        " 来自 " + modulePathOf(e.vtbl[18]) + " → 打回我们的钩子");
			patchSlotLocked(&e.vtbl[18], reinterpret_cast<void*>(&hookedPresent1));
		}
	}
	for (int i = 0; i < g_facN; ++i)
	{
		FacEntry& e = g_fac[i];
		const int   slots[4] = { 10, 14, 15, 17 };
		void* const hooks[4] = {
			reinterpret_cast<void*>(&hookedCreateSwapChain),
			reinterpret_cast<void*>(&hookedCreateSwapChainForHwnd),
			reinterpret_cast<void*>(&hookedCreateSwapChainForCoreWindow),
			reinterpret_cast<void*>(&hookedCreateSwapChainForComposition)
		};
		void* const origs[4] = { e.o10, e.o14, e.o15, e.o17 };
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
	void* lastS4 = nullptr;
	for (;;)
	{
		Sleep(1000);
		++ticks;
		if (ticks % 5 == 0)
		{
			verifyAndRepair();

			// v1.5 探针 A: 对象级监视 — 类 vtable 稳不代表游戏对象的 vptr/槽位没变
			void* gsc = g_gameSc.load();
			if (gsc && memReadable(gsc, sizeof(void*)))
			{
				void** gv = *reinterpret_cast<void***>(gsc);
				if (memReadable(gv, 5 * sizeof(void*)))
				{
					if (gv != lastGV)
					{
						logLine("探针: 游戏交换链 vptr=" + hexOf(gv) + " 来自 " + modulePathOf(gv) +
						        (lastGV ? " (变更! 旧=" + hexOf(lastGV) + ")" : " (基线)"));
						lastGV = gv;
						lastS4 = nullptr;
					}
					void* s4 = gv[4];
					if (s4 != lastS4)
					{
						logLine("探针: 游戏交换链 slot4=" + hexOf(s4) + " 来自 " + modulePathOf(s4) +
					                (s4 == reinterpret_cast<void*>(&hookedPresent)
					                     ? " (我们的钩子 ✓)"
					                     : " (≠我们的钩子 !!)"));
						lastS4 = s4;
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
			logLine("告警: 安装成功但 120s 内 0 次 Present — 已登记 交换链vtable=" +
			        std::to_string(swpN) + " 工厂vtable=" + std::to_string(facN) +
			        "; 按日志中 vtable 模块 / ★工厂拦截 / 被改写记录迭代");
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
	info->version = (0u << 16) | (6u << 8) | 0u; // 0.6.0

	if (skse->isEditor)  // 只进游戏本体, 不进 Creation Kit
		return false;

	return true;
}

__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* skse)
{
	g_logPath = pluginDir() + "\\poc-presenter.log";
	logLine("==== poc-presenter v0.6.0 (PoC-A v1.5: 同步安装 + 五口径 + 设备链 + 对象/字节/窗口三探针) ====");

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
