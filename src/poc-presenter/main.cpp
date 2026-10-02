/*
 * poc-presenter — PoC-A: SKSE 插件载体的 Present Hook 验证
 *
 * 目的 (docs/00 首周行动项 #4 / 最高风险项 #1 的第一环):
 *   证明能在真实游戏进程内拦截 IDXGISwapChain::Present —— 这是 PoC-B (在 Present 里
 *   创建 Vulkan instance/swapchain 并注入呈现) 的先决条件。本步只记日志、不碰 Vulkan,
 *   不改变任何画面内容, 失败面最小。
 *
 * 机制 (dummy 交换链 → 类级改写 vtable 槽 4):
 *   1. 等待进程内 d3d11.dll / dxgi.dll 就位 (SKSE 加载点早于设备创建, 最多等 30s);
 *   2. 自建隐藏窗口 + dummy D3D11 设备 + dummy swapchain —— 拿到当前进程里"游戏同款"
 *      交换链对象的 vtable;
 *   3. VirtualProtect 改写 vtable 槽 4 (COM 接口序固定: QI/AddRef/Release/GetDevice/
 *      Present, 故 Present=4) → 类级生效, 游戏交换链的 Present 同样走我们的函数;
 *   4. 我们的 Present: 计数 + 首帧打印交换链描述 + 每 600 帧打印 FPS, 然后转调原函数。
 *
 * 为什么 dummy 和游戏的交换链共享同一张 vtable:
 *   - 无 RenderDoc: 双方都来自真实 dxgi.dll 的交换链实现类, vtable 是类级共享 (.rdata);
 *   - 有 RenderDoc (in-app, capture-helper 已加载): 其对 CreateDXGIFactory*/D3D11CreateDevice
 *     的拦截让 dummy 与游戏拿到同一包装类 → 改的仍是游戏实际用的那张 vtable, 且"原函数"
 *     指向 RenderDoc 的包装 Present → 抓帧链路不受影响 (先我们、后 RenderDoc、再真实呈现);
 *   - 若因加载时序导致两边不同类 (装好但不触发), 日志 30s 后给出告警 → 按证据迭代。
 *     PoC-A 的价值就是把这类时序问题在上 Vulkan 之前暴露出来。
 *
 * 验收标准 (本地游戏内):
 *   <游戏>/Data/SKSE/Plugins/poc-presenter.log 依次出现:
 *     [..] d3d11.dll = ... / dxgi.dll = ...
 *     [..] dummy vtable = 0x... slot4 Present = 0x... 来自 ...
 *     [..] Hook 安装成功: Present 槽4 -> 0x...
 *     [..] 第 1 次 Present: 1920x1080 format=... 窗口="..."   ← 关键行
 *     [..] Present 已开始触发 — PoC-A 验收通过
 *     [..] Present 计数 600  近600帧 xx.x FPS (周期行)
 *   且游戏画面完全正常 (本 PoC 不改变呈现)。
 *
 * 构建: GitHub Actions (build.yml job "poc-presenter"), 本地不编译
 * 安装: poc-presenter.dll 放入 <游戏>/Data/SKSE/Plugins/
 * 日志: <游戏>/Data/SKSE/Plugins/poc-presenter.log
 */

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
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

// ---------- Present 钩子本体 ----------

using Present_t = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain* sc, UINT sync, UINT flags);

Present_t g_origPresent = nullptr;
std::atomic<uint64_t> g_presentCount{0};
std::chrono::steady_clock::time_point g_lastLog{};
uint64_t g_lastLogCount = 0;

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

HRESULT STDMETHODCALLTYPE hookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
	const uint64_t n = g_presentCount.fetch_add(1) + 1;
	if (n == 1)
	{
		g_lastLog = std::chrono::steady_clock::now();
		g_lastLogCount = 1;
		logLine("第 1 次 Present: " + swapDesc(sc));
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
	return g_origPresent ? g_origPresent(sc, sync, flags) : E_FAIL;
}

// ---------- Hook 安装 ----------

bool installHook()
{
	// 1) 等 d3d11/dxgi 就位 (最多 30s)
	HMODULE hd3d11 = nullptr, hdxgi = nullptr;
	for (int i = 0; i < 3000; ++i)
	{
		hd3d11 = GetModuleHandleW(L"d3d11.dll");
		hdxgi = GetModuleHandleW(L"dxgi.dll");
		if (hd3d11 && hdxgi)
			break;
		Sleep(10);
	}
	if (!hd3d11 || !hdxgi)
	{
		logLine(std::string("等待 d3d11/dxgi 超时 (30s): d3d11=") +
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

	// 2) 动态取导出 (不静态链接 d3d11/dxgi, 由我们控制首次加载时机)
	auto pCreateDevice = reinterpret_cast<HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
	                                                       const D3D_FEATURE_LEVEL*, UINT, UINT,
	                                                       ID3D11Device**, D3D_FEATURE_LEVEL*, UINT*)>(
	    GetProcAddress(hd3d11, "D3D11CreateDevice"));
	auto pCreateFactory = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
	    GetProcAddress(hdxgi, "CreateDXGIFactory1"));
	if (!pCreateDevice || !pCreateFactory)
	{
		logLine("导出函数获取失败: D3D11CreateDevice=" + hexOf(reinterpret_cast<const void*>(pCreateDevice)) +
		        " CreateDXGIFactory1=" + hexOf(reinterpret_cast<const void*>(pCreateFactory)));
		return false;
	}

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

	// 5) dummy swapchain (与游戏同款路径: CreateSwapChainForHwnd)
	IDXGIFactory2* factory = nullptr;
	hr = pCreateFactory(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
	if (FAILED(hr) || !factory)
	{
		logLine("CreateDXGIFactory1(IDXGIFactory2) 失败 " + hexHr(hr));
		return false;
	}
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
	logLine("dummy swapchain (" + adapterHow + " 设备): " + swapDesc(sc1));

	// 6) 改写 vtable 槽 4 = Present (类级生效)
	void** vtbl = *reinterpret_cast<void***>(sc1);
	void* orig = vtbl[4];
	logLine("dummy vtable = " + hexOf(vtbl) + "  slot4 Present = " + hexOf(orig) +
	        " 来自 " + modulePathOf(orig));
	logLine("slot18(疑 Present1) = " + hexOf(vtbl[18]) + " 来自 " + modulePathOf(vtbl[18]));

	DWORD oldProt = 0;
	if (!VirtualProtect(&vtbl[4], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
	{
		logLine("VirtualProtect 失败 err=" + std::to_string(GetLastError()));
		return false;
	}
	g_origPresent = reinterpret_cast<Present_t>(orig);
	InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&vtbl[4]),
	                           reinterpret_cast<void*>(&hookedPresent));
	DWORD tmp = 0;
	VirtualProtect(&vtbl[4], sizeof(void*), oldProt, &tmp);

	// sc1/factory/dev 故意不释放: vtable 类驻留 = hook 常驻 (进程生命周期)
	logLine("Hook 安装成功: Present 槽4 -> " + hexOf(reinterpret_cast<const void*>(&hookedPresent)) +
	        " (原 " + hexOf(orig) + ")");
	return true;
}

void installThread()
{
	logLine("Hook 安装线程启动 (等 d3d11/dxgi 就位, 最多 30s)");
	if (!installHook())
	{
		logLine("Hook 安装失败 (详见上方日志)");
		return;
	}
	// 30s 观察窗: 触发即验收通过; 不触发则给出时序告警
	for (int i = 0; i < 300; ++i)
	{
		Sleep(100);
		if (g_presentCount.load() > 0)
		{
			logLine("Present 已开始触发 (计数 " +
			        std::to_string(g_presentCount.load()) + ") — PoC-A 验收通过");
			return;
		}
	}
	logLine("告警: Hook 安装成功但 30s 内 0 次 Present — 可能与游戏交换链不同 vtable 类"
	        "(加载时序: dummy 与游戏创建路径被 RenderDoc 拦截前后不一致), 按日志迭代");
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
	info->version = (0u << 16) | (1u << 8) | 0u; // 0.1.0

	if (skse->isEditor)  // 只进游戏本体, 不进 Creation Kit
		return false;

	return true;
}

__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* skse)
{
	g_logPath = pluginDir() + "\\poc-presenter.log";
	logLine("==== poc-presenter v0.1.0 (PoC-A Present Hook) ====");

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

	std::thread(installThread).detach();
	logLine("就绪 — 不改变画面; 进游戏后观察本日志");
	return true;
}

} // extern "C"
