/*
 * capture-helper — Skyrim SE 进程内 RenderDoc 抓帧引导插件
 *
 * 作用:
 *   游戏启动时 (SKSE 插件加载点, 早于 D3D11 设备创建) 在进程内加载 renderdoc.dll,
 *   设置厂商扩展白名单 (修复 NvAPI 被禁用导致的启动崩溃, 参考 Community Shaders
 *   的已验证做法), 并配置抓帧输出路径。之后使用 RenderDoc 默认热键 (F12 /
 *   PrintScreen) 即可直接抓帧, .rdc 文件写入磁盘, 事后用 RenderDoc UI 打开分析。
 *
 * 与 RenderDoc UI 外部注入方式的区别:
 *   - 不依赖 MO2/子进程注入链, 无注入冲突
 *   - 进程内 API 方式可设置 UI 中不存在的 AllowUnsupportedVendorExtensions 选项
 *
 * 构建: GitHub Actions (见 .github/workflows/build.yml), 本地不编译
 * 安装: capture-helper.dll 放入 <游戏>/Data/SKSE/Plugins/
 * 日志: <游戏>/Data/SKSE/Plugins/capture-helper.log
 *
 * 可选配置: 在插件同目录创建 capture-helper.ini:
 *   renderdocDll=C:\Program Files\RenderDoc\renderdoc.dll
 */

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "renderdoc_app.h"
#include "skse_abi.h"

namespace {

HMODULE g_hModule = nullptr;
RENDERDOC_API_1_7_0* g_rd = nullptr; // 各版本 API 结构体为前缀兼容布局
std::string g_logPath;

// ---------- 日志 ----------

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

// ---------- 路径工具 ----------

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

std::string gameRoot()   // 上溯 Plugins -> SKSE -> Data -> 游戏根目录
{
	std::string d = pluginDir();
	d = dirOf(d);
	d = dirOf(d);
	return dirOf(d);
}

bool fileExists(const std::string& p)
{
	const DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 读取 capture-helper.ini 中的 renderdocDll= 键
std::string iniRenderDocPath()
{
	std::ifstream f(pluginDir() + "\\capture-helper.ini");
	std::string line;
	while (std::getline(f, line))
	{
		const auto eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = line.substr(0, eq);
		std::string val = line.substr(eq + 1);
		// trim
		auto trim = [](std::string& s) {
			while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t'))
				s.pop_back();
			size_t i = 0;
			while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
				++i;
			s = s.substr(i);
		};
		trim(key);
		trim(val);
		if (key == "renderdocDll" && !val.empty())
			return val;
	}
	return {};
}

// ---------- RenderDoc 加载 ----------

RENDERDOC_API_1_7_0* loadRenderDoc()
{
	HMODULE h = GetModuleHandleW(L"renderdoc.dll");
	std::string how = "已由其他组件加载";

	if (!h)
	{
		std::string ini = iniRenderDocPath();
		if (!ini.empty())
			logLine("ini 指定 renderdocDll = " + ini);

		const std::string root = gameRoot();
		const std::vector<std::string> candidates = {
			ini,
			root + "\\Data\\Renderdoc\\renderdoc.dll", // Community Shaders 约定位置
			pluginDir() + "\\renderdoc.dll",
			root + "\\renderdoc.dll",
		};

		for (const auto& c : candidates)
		{
			if (c.empty() || !fileExists(c))
				continue;
			h = LoadLibraryA(c.c_str());
			if (h)
			{
				how = c;
				break;
			}
			logLine("LoadLibrary 失败: " + c);
		}
	}

	if (!h)
	{
		logLine("未找到 renderdoc.dll。请安装 RenderDoc 后在 capture-helper.ini 中指定路径, 例如:");
		logLine("  renderdocDll=C:\\Program Files\\RenderDoc\\renderdoc.dll");
		logLine("(游戏可正常运行, 只是抓帧功能不可用)");
		return nullptr;
	}

	auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(h, "RENDERDOC_GetAPI"));
	if (!getApi)
	{
		logLine("RENDERDOC_GetAPI 导出不存在, renderdoc.dll 无效: " + how);
		return nullptr;
	}

	// 从高到低尝试 API 版本 (结构体前缀兼容)
	const RENDERDOC_Version versions[] = {
		eRENDERDOC_API_Version_1_7_0,
		eRENDERDOC_API_Version_1_6_0,
		eRENDERDOC_API_Version_1_5_0,
	};
	for (RENDERDOC_Version v : versions)
	{
		void* api = nullptr;
		if (getApi(v, &api) == 1 && api)
		{
			logLine("RenderDoc API 已连接 (版本 " + std::to_string(static_cast<int>(v)) + "), 来源: " + how);
			return static_cast<RENDERDOC_API_1_7_0*>(api);
		}
	}

	logLine("RENDERDOC_GetAPI 所有版本均失败: " + how);
	return nullptr;
}

// ---------- 抓帧监控线程: 记录新生成的 .rdc 路径 ----------

void monitorProc()
{
	uint32_t last = g_rd->GetNumCaptures();
	if (last > 0)
		logLine("已存在历史抓帧 " + std::to_string(last) + " 个");

	for (;;)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		const uint32_t n = g_rd->GetNumCaptures();
		while (last < n)
		{
			// 两段式查询 (参考 Community Shaders 的已验证用法):
			// 先取所需长度, 再取路径
			uint32_t need = 0;
			if (g_rd->GetCapture(last, nullptr, &need, nullptr) && need > 0)
			{
				std::vector<char> buf(need + 1, '\0');
				if (g_rd->GetCapture(last, buf.data(), &need, nullptr))
					logLine(std::string("抓帧已保存: ") + buf.data());
				else
					logLine("抓帧 #" + std::to_string(last) + " 无法获取路径");
			}
			else
			{
				logLine("抓帧 #" + std::to_string(last) + " 索引无效");
			}
			++last;
		}
	}
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
	info->name = "CaptureHelper";
	info->version = (1u << 16) | (0u << 8) | 0u; // 1.0.0

	// 仅在游戏本体加载, 不进 Creation Kit
	if (skse->isEditor)
		return false;

	return true;
}

__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* skse)
{
	g_logPath = pluginDir() + "\\capture-helper.log";
	logLine("==== capture-helper v1.0.0 ====");

	// 记录环境信息 (写入基线记录表用)
	const UInt32 rt = skse->runtimeVersion;
	{
		char buf[160];
		std::snprintf(buf, sizeof(buf),
		    "游戏版本: 0x%08X -> %u.%u.%u.%u | SKSE: 0x%08X (%u)",
		    rt,
		    (rt >> 24) & 0xFF, (rt >> 16) & 0xFF, (rt >> 8) & 0xFF, rt & 0xFF,
		    skse->skseVersion, skse->skseVersion);
		logLine(buf);
	}

	g_rd = loadRenderDoc();
	if (!g_rd)
		return true; // 不阻断游戏启动

	// 关键修复: 白名单放行厂商扩展 (NvAPI)。参考 Community Shaders
	// src/Features/RenderDoc.cpp 的已验证做法 — 不设置此项时,
	// RenderDoc 会禁用 NvAPI, 导致游戏首次 Present 时设备被驱动移除而崩溃。
	const int ret = g_rd->SetCaptureOptionU32(eRENDERDOC_Option_AllowUnsupportedVendorExtensions, 0x10DE);
	{
		char buf[96];
		std::snprintf(buf, sizeof(buf),
		    "AllowUnsupportedVendorExtensions(0x10DE NvAPI) = %d %s",
		    ret, ret == 1 ? "(OK)" : "(失败)");
		logLine(buf);
	}

	// 抓帧输出目录与文件名模板
	const std::string capDir = pluginDir() + "\\captures";
	CreateDirectoryA(capDir.c_str(), nullptr);
	const std::string tmpl = capDir + "\\skyrimse_frame";
	g_rd->SetCaptureFilePathTemplate(tmpl.c_str());
	logLine("抓帧输出: " + tmpl + "*.rdc");

	// 保持 RenderDoc 默认热键 (F12 / PrintScreen) 与默认 Overlay — 不做屏蔽,
	// 角标可见即为注入成功的确认信号。

	std::thread(monitorProc).detach();

	logLine("就绪 — 进入游戏后按 F12 / PrintScreen 抓帧, 用 RenderDoc UI 打开 .rdc");
	return true;
}

} // extern "C"
