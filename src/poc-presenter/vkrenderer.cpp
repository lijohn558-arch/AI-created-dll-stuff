// ===========================================================================
// vkrenderer.cpp —— 自写 Vulkan 渲染器模块 (方向 A / 第一步 B)
// v0.17.0 从 main.cpp 抽出 (**纯代码搬迁, 行为不变**): PoC-B 全套 + Step 2c-β 交叉校验。
// 2d 起的新增代码 (出向回写 / descriptor+sampler / 深度改道 VK 自建 D24 图) 都写在这里。
// 编译: build.yml 同时编 main.cpp 与本文件, 链成同一个 poc-presenter.dll。
#include "vkrenderer.h"
#include "pocb_shaders.h"

PocbCtx g_pocb; // 定义在这里 (vkrenderer.h 声明为 extern); 原 main.cpp 里是 static
using namespace pocmain; // hook 面提供的 日志/ini/哈希/D3D11 镜像 (声明见 vkrenderer.h)

// ---------- 原 main.cpp: PoC-B 段落说明 ----------
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


// ---------- 原 main.cpp: Step 2c-β 的 VK 侧状态 (g_ssrVk* + 候选槽表) ----------
// ---- Step 2c-β (v0.16.0, docs/05 D2a-4): VK 侧交叉校验 ----
// 把色镜像的 NT handle 用 VK_KHR_external_memory_win32 (OPAQUE_WIN32) 导成 VkImage,
// 再 vkCmdCopyImageToBuffer + vkMapMemory 把同一张镜像读回 CPU, 用**同一个**
// ssrFnvSample 算校验和, 与同一帧 D3D11 STAGING 读回的值比对 —— 相等 = 两套 API 对
// 这块共享内存的字节视图一致, D3D11→VK 入向通路成立 (SSR v1 每帧要用的那条路)。
// 节奏完全搭 D3D11 读回的便车 (前3次+每600次), **不做每帧读回**, 不违约束 2。
// 深度镜像 (R2) 未过 ⇒ 本步只导色; 镜像建不出时整段自动不跑。
static VkImage        g_ssrVkImg = VK_NULL_HANDLE;     // 从色镜像 NT handle 导入的 VkImage
static VkDeviceMemory g_ssrVkMem = VK_NULL_HANDLE;
static VkBuffer       g_ssrVkBuf = VK_NULL_HANDLE;     // 读回 buffer (TRANSFER_DST)
static VkDeviceMemory g_ssrVkBufMem = VK_NULL_HANDLE;
static void*          g_ssrVkBufPtr = nullptr;         // 持久映射 (vkMapMemory 一次)
static VkCommandBuffer g_ssrVkCmd = VK_NULL_HANDLE;    // 录一次永久复用 (借用 PoC-B 的 command pool)
static int   g_ssrVkState = 0;                          // 0=未建 1=OK 2=失败禁用
static long  g_ssrVkN = 0;                              // 交叉校验次数
static VkCommandBuffer g_ssrVkCmd0 = VK_NULL_HANDLE;    // 布局初转命令 (分配一次, 每槽重录)
// ---- 轮换单图槽 (v0.16.5) —— 任何时刻**只有一张** VkImage 持有这块导入内存 ----
// v0.16.4 的教训: 变体B 与主图**同时**绑定同一块导入内存 ⇒ 帧1051 交叉校验后驱动返回
// VK_ERROR_DEVICE_LOST (-4), PoC-B 被连坐关闭注入 (画面上的 VK 三角消失)。
// 改法: 每次交叉校验只测一个候选参数 —— 建图+初转 → **本帧不比** (初转按规范可能丢内容)
// → 下次交叉校验才读回比对 → 读完立刻 vkDeviceWaitIdle + 销毁 → 换下一槽。
// ---- v0.16.5 真机跑图结果 (docs/02 §14.13.5) ⇒ v0.16.6 收敛到唯一正确参数 ----
//   · 槽0 `D3D11句柄` (handleType=D3D11_TEXTURE_BIT) **命中: VK hash == D3D11 hash**
//     ⇒ 归因1 的病因定死 = handle 类型用错 (OPAQUE_WIN32 的语义是"不解析句柄布局")。
//   · 槽1 `LINEAR` 不一致 (没丢设备, 但也没解决问题)。
//   · 槽2 `全程GENERAL` 读回 19.43ms → **同秒 PoC-B 报 DEVICE_LOST(-4) 关闭注入** ⇒
//     "不转 layout 直接在 GENERAL 下跨 API 并发读"这个姿势有害, 已删除。
//   · 槽3 `对照原样` 没跑到 (PoC-B 已先挂)。
// ⇒ v0.16.6: 表里只剩 `D3D11句柄` 一个参数, NSlot=1 ⇒ 不轮换、图常驻 (g_ssrVkRotate=false),
//   每次交叉校验机会都出一行对比 (样本翻倍); 将来要再试参数就往表里加, NSlot>1 自动恢复轮换。
struct SsrVkSlot
{
	const char* tag;
	int handleType; // VK_EXTERNAL_MEMORY_HANDLE_TYPE_*
	bool linear;    // VK_IMAGE_TILING_LINEAR (默认 OPTIMAL)
	bool noTrans;   // 读回不转 TRANSFER_SRC, 全程 GENERAL 只做内存栅栏
};
static const SsrVkSlot g_ssrVkSlots[1] = {
    {"D3D11句柄", VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, false, false},
};
static const int g_ssrVkNSlot = 1;
static const bool g_ssrVkRotate = (g_ssrVkNSlot > 1); // 多槽才"读完即毁换槽"; 单槽图常驻
static int   g_ssrVkSlotIdx = 0;   // 当前在测的槽 (读完一次换下一个)
static int   g_ssrVkMakeFail = 0;  // 连续建图失败数 (全槽都建不出才关闸)
static bool  g_ssrVkLogFirst = false; // "VK 导入 OK" 那行只打一次 (β2 判据要恰好 1 行)
static unsigned g_ssrVkW = 0, g_ssrVkH = 0;             // 镜像尺寸 (buffer 宽度用)

// ---- 2d-1 出向回写 (v0.18.0) —— VK 渲完的结果落到第 3 张 SHARED 纹理, 供 D3D11 拷进 585 ----
// 时序: 入向镜像在**本帧特征B** 被 D3D11 拷 (324→镜像); 本函数在**本帧 Present** 读它并写进出向镜像;
//       **下一帧特征B** D3D11 再把出向镜像拷进 585 ⇒ 跨 API 不做任何 GPU 栅栏, 全靠
//       "上一帧已填、本帧才读" 的 1 帧延迟 + CPU fence (vkWaitForFences 在 Readback 已证明可用)。
// 约束: 出向图绑的是**另一块** NT handle 的内存, 与 g_ssrVkImg 各自独立 —— 不会重演 v0.16.4
//       "两张 VkImage 同时绑同一块内存" 那个 DEVICE_LOST。
static VkImage        g_ssrVkImgOut = VK_NULL_HANDLE;
static VkDeviceMemory g_ssrVkMemOut = VK_NULL_HANDLE;
static VkCommandBuffer g_ssrVkCmdOut = VK_NULL_HANDLE; // 录一次永久复用 (ONE_TIME 提交完可重录)
static VkImage        g_ssrVkCmdOutSrc = VK_NULL_HANDLE; // 录制时的入向图 handle (变了才重录)
static int   g_ssrVkOutState = 0;   // 0=未建 1=OK 2=失败禁用 (只关自己, 不连坐入向)
static long  g_ssrVkOutN = 0;       // 出向提交次数

// ---------- 原 main.cpp: PoC-B 实现 (pocbCode/pocbFail/pocbEnabled/pocbInit/2c-β/pocbInject) ----------
static std::string pocbCode(long v)
{
	char b[40];
	std::snprintf(b, sizeof(b), "%ld (0x%08lX)", v, static_cast<unsigned long>(v));
	return b;
}

// 失败统一出口: 记日志 + 关注入 (游戏画面从此不受本模块影响)
void pocbFail(const char* what, long code)
{
	g_pocb.state.store(3);
	logLine(std::string("PoC-B 失败: ") + what + " code=" + pocbCode(code) + " — 已关闭注入, 游戏照常呈现");
}

void pocbFail(const char* what)
{
	pocbFail(what, 0);
}

// 逃生门: <pluginDir>\poc-presenter.ini 里 vulkan=0 / pocb=0 关掉注入 (不重编译)
bool pocbEnabled()
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
bool pocbInit(IDXGISwapChain* sc)
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
	installProbeOn(c.dev); // 探针升质双保险: registerSwp 没抓到的话这里补一次 (幂等)
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

	// --- 共享纹理 (v0.10.0): D3D11 NT handle —— VK 导入的另一半 ---
	// 任一步失败都只是"本局回退读回路径", 不是 init 失败 (注入照常, 只是没提速)。
	if (iniFlag("shared", true))
	{
		D3D11_TEXTURE2D_DESC sd{};
		sd.Width = POCB_W;
		sd.Height = POCB_H;
		sd.MipLevels = 1;
		sd.ArraySize = 1;
		sd.Format = bd.Format; // 已校验 = R8G8B8A8_UNORM
		sd.SampleDesc.Count = 1;
		sd.Usage = D3D11_USAGE_DEFAULT;
		sd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		hr = c.dev->CreateTexture2D(&sd, nullptr, &c.stex);
		IDXGIResource1* res1 = nullptr;
		if (SUCCEEDED(hr) && c.stex)
			hr = c.stex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&res1));
		HANDLE h = nullptr;
		if (SUCCEEDED(hr) && res1)
		{
			// 访问掩码各家文档口径不一 (GENERIC_* vs 0) —— 三种依次试, 第一个成的算
			const DWORD acc[3] = { GENERIC_READ | GENERIC_WRITE, GENERIC_ALL, 0 };
			for (int ai = 0; ai < 3; ++ai)
			{
				h = nullptr;
				hr = res1->CreateSharedHandle(nullptr, acc[ai], nullptr, &h);
				if (h)
					break;
			}
		}
		if (res1)
			res1->Release();
		if (SUCCEEDED(hr) && h)
		{
			c.shandle = h;
			// EVENT 查询: 共享路径的跨 API 竞态闸 (见 pocbInject), 建不出来就不敢走共享
			D3D11_QUERY_DESC qd{};
			qd.Query = D3D11_QUERY_EVENT;
			if (SUCCEEDED(c.dev->CreateQuery(&qd, &c.copyQ)) && c.copyQ)
			{
				c.useShared = true;
				logLine("PoC-B init: 共享纹理 NT handle OK (D3D11 EVENT 闸 OK)");
			}
			else
			{
				logLine("PoC-B init: D3D11 EVENT 查询创建失败 → 回退读回路径");
				CloseHandle(h);
				c.shandle = nullptr;
				c.stex->Release();
				c.stex = nullptr;
			}
		}
		else
		{
			logLine("PoC-B init: 共享纹理 NT handle 失败 hr=" + pocbCode(hr) + " → 回退读回路径");
			if (c.stex)
			{
				c.stex->Release();
				c.stex = nullptr;
			}
		}
	}
	else
		logLine("PoC-B init: ini shared=0 → 读回路径 (6.47ms 基线口径)");

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
	// 只校验**实例级**函数。设备级函数从 vkGetInstanceProcAddr 拿可能返回 NULL (规范允许,
	// renderdoc in-app 的包装层更倾向如此) —— v0.9.0 实测 vkGetDeviceQueue 就是空, 把整步卡死;
	// 设备级一律等 vkCreateDevice 之后走 vkGetDeviceProcAddr 重装 (那层必非空)。
	// 空指针按名字列进日志, 免得下次再靠猜。
	{
		std::string nul;
#define POCB_CHECK_I(n) if (!fns.n) { if (!nul.empty()) nul += ","; nul += #n; }
		POCB_INST_REQ_FNS(POCB_CHECK_I)
#undef POCB_CHECK_I
		if (!nul.empty())
		{
			logLine("PoC-B init: 实例级空指针 = " + nul);
			pocbFail("实例级函数表不完整 (vkGetInstanceProcAddr 返回空)");
			return false;
		}
		if (!fns.vkGetPhysicalDeviceProperties2)
			logLine("PoC-B init: vkGetPhysicalDeviceProperties2 为空 (1.0 实例, 跳过 LUID 匹配)");
		logLine("PoC-B init: 实例级函数表 OK (必查 7/7)");
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
	// --- 共享纹理设备扩展 (v0.10.0): VK_KHR_external_memory_win32 (+1.0 老设备的 base) ---
	// 查不到 / 上面共享没建成 ⇒ c.useShared=0 走读回兜底, vkCreateDevice 不带扩展照常过。
	const char* devExt[2] = {};
	uint32_t    devExtN = 0;
	if (c.useShared)
	{
		bool hasWin32 = false, hasBase = false;
		uint32_t ne = 0;
		if (fns.vkEnumerateDeviceExtensionProperties)
			fns.vkEnumerateDeviceExtensionProperties(chosen, nullptr, &ne, nullptr);
		static VkExtensionProperties eps[192]; // static: 别吃 Present 线程的栈
		if (ne > 192)
			ne = 192;
		if (ne != 0 && fns.vkEnumerateDeviceExtensionProperties)
			fns.vkEnumerateDeviceExtensionProperties(chosen, nullptr, &ne, eps);
		for (uint32_t i = 0; i < ne; ++i)
		{
			if (std::strcmp(eps[i].extensionName, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == 0)
				hasWin32 = true;
			else if (std::strcmp(eps[i].extensionName, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0)
				hasBase = true;
		}
		const bool api11 = props.apiVersion >= VK_API_VERSION_1_1;
		if (hasWin32 && (hasBase || api11))
		{
			devExt[devExtN++] = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
			if (!api11)
				devExt[devExtN++] = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
			dci.enabledExtensionCount = devExtN;
			dci.ppEnabledExtensionNames = devExt;
			logLine("PoC-B init: 共享纹理设备扩展已列 (win32" +
			        std::string(!api11 ? ", base" : ", api1.1核内") + ")");
		}
		else
		{
			c.useShared = false;
			logLine(std::string("PoC-B init: ") +
			        (hasWin32 ? "缺 VK_KHR_external_memory base" : "缺 VK_KHR_external_memory_win32") +
			        " → 共享纹理关, 回退读回路径");
		}
	}
	vr = fns.vkCreateDevice(chosen, &dci, nullptr, &c.vdev);
	if (vr != VK_SUCCESS || !c.vdev)
	{
		pocbFail("vkCreateDevice", vr);
		return false;
	}
#define POCB_LOAD_D(n) fns.n = reinterpret_cast<PFN_##n>(fns.vkGetDeviceProcAddr(c.vdev, #n));
	POCB_DEV_FNS(POCB_LOAD_D)
#undef POCB_LOAD_D
	// 设备级全表校验 (POCB_DEV_FNS 全部, 含 v0.10.0 新增的 vkCmdCopyImage/vkCmdPipelineBarrier)
	// —— 空的按名字列出来, 不再只报"不完整"
	{
		std::string nul;
#define POCB_CHECK_D(n) if (!fns.n) { if (!nul.empty()) nul += ","; nul += #n; }
		POCB_DEV_FNS(POCB_CHECK_D)
#undef POCB_CHECK_D
		if (!nul.empty())
		{
			logLine("PoC-B init: 设备级空指针 = " + nul);
			pocbFail("设备级函数表不完整 (vkGetDeviceProcAddr 返回空)");
			return false;
		}
		logLine("PoC-B init: 设备级函数表 OK (POCB_DEV_FNS 全查)");
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

	// --- 共享图导入 (v0.10.0): 把 stex 的 NT 句柄导成 VkImage ---
	// 之后每帧 renderpass 的产物 vkCmdCopyImage 拷进来, D3D11 直接从 stex 拷进 backbuffer
	// (GPU→GPU, 不再有 1MB 读回 + UpdateSubresource 上传)。失败 ⇒ 本局读回兜底。
	if (c.useShared)
	{
		VkExternalMemoryImageCreateInfo emi{};
		emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkImageCreateInfo simci{};
		simci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		simci.pNext = &emi;
		simci.imageType = VK_IMAGE_TYPE_2D;
		simci.format = VK_FORMAT_R8G8B8A8_UNORM; // 与离屏图同字节序, 拷贝不换序
		simci.extent = VkExtent3D{POCB_W, POCB_H, 1};
		simci.mipLevels = 1;
		simci.arrayLayers = 1;
		simci.samples = VK_SAMPLE_COUNT_1_BIT;
		simci.tiling = VK_IMAGE_TILING_OPTIMAL;
		simci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		simci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		simci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		vr = fns.vkCreateImage(c.vdev, &simci, nullptr, &c.simg);
		if (vr != VK_SUCCESS)
			logLine("PoC-B init: 共享图 vkCreateImage = " + pocbCode(vr));
		if (vr == VK_SUCCESS)
		{
			VkMemoryRequirements sreq{};
			fns.vkGetImageMemoryRequirements(c.vdev, c.simg, &sreq);
			int t = pocbMemType(mem, sreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (t < 0)
				t = pocbMemType(mem, sreq.memoryTypeBits, 0);
			if (t < 0)
			{
				logLine("PoC-B init: 共享图无可用内存类型 (memoryTypeBits=" +
				        std::to_string(sreq.memoryTypeBits) + ")");
				vr = VK_ERROR_OUT_OF_DEVICE_MEMORY;
			}
			else
			{
				VkImportMemoryWin32HandleInfoKHR imp{};
				imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
				imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
				imp.handle = c.shandle; // 分配即导入 (链进 VkMemoryAllocateInfo)
				VkMemoryAllocateInfo mai{};
				mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
				mai.pNext = &imp;
				mai.allocationSize = sreq.size;
				mai.memoryTypeIndex = static_cast<uint32_t>(t);
				vr = fns.vkAllocateMemory(c.vdev, &mai, nullptr, &c.simgMem);
				if (vr != VK_SUCCESS)
					logLine("PoC-B init: 共享图导入 vkAllocateMemory(NT handle) = " + pocbCode(vr));
			}
		}
		if (vr == VK_SUCCESS && fns.vkBindImageMemory(c.vdev, c.simg, c.simgMem, 0) != VK_SUCCESS)
		{
			logLine("PoC-B init: 共享图 vkBindImageMemory 失败");
			vr = VK_ERROR_INITIALIZATION_FAILED;
		}
		if (vr != VK_SUCCESS)
		{
			c.useShared = false;
			logLine("PoC-B init: 共享纹理导入失败 → 回退读回路径 (注入照常, 无提速)");
		}
		else
			logLine("PoC-B init: 共享图导入 OK (D3D11 NT handle → VkImage, OPAQUE_WIN32)");
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

	// --- 共享路径命令 (v0.10.0): 同一 renderpass, 尾部把离屏图拷进共享图 ---
	// 也录一次永久复用。共享图每帧被拷贝全域覆盖 ⇒ 逐帧从 UNDEFINED 起 (丢弃旧内容)合法;
	// 旧内容已由 pocbInject 的 event 闸保证被 D3D11 拷走, 不存在竞态。
	if (c.useShared)
	{
		cbai.commandBufferCount = 1;
		if (fns.vkAllocateCommandBuffers(c.vdev, &cbai, &c.cmdS) != VK_SUCCESS)
		{
			logLine("PoC-B init: 共享路径 vkAllocateCommandBuffers 失败 → 回退读回");
			c.useShared = false;
		}
		else if (fns.vkBeginCommandBuffer(c.cmdS, &cbBegin) != VK_SUCCESS)
		{
			logLine("PoC-B init: 共享路径 vkBeginCommandBuffer 失败 → 回退读回");
			c.useShared = false;
		}
		else
		{
			fns.vkCmdBeginRenderPass(c.cmdS, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
			fns.vkCmdBindPipeline(c.cmdS, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipe);
			fns.vkCmdSetViewport(c.cmdS, 0, 1, &vp);
			fns.vkCmdSetScissor(c.cmdS, 0, 1, &sci);
			fns.vkCmdDraw(c.cmdS, 3, 1, 0, 0);
			fns.vkCmdEndRenderPass(c.cmdS); // finalLayout = TRANSFER_SRC (离屏图作拷贝源)
			VkImageMemoryBarrier imb{};
			imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			imb.srcAccessMask = 0;
			imb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imb.image = c.simg;
			imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			fns.vkCmdPipelineBarrier(c.cmdS, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb);
			VkImageCopy icp{};
			icp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			icp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			icp.extent = VkExtent3D{POCB_W, POCB_H, 1};
			fns.vkCmdCopyImage(c.cmdS, c.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.simg,
			                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &icp);
			// TRANSFER_DST → GENERAL: 交给 D3D11 读 (跨 API 口径只认 GENERAL;
			// dstAccess=MEMORY_READ 让驱动把写做完再放行 —— 真正的交接仍靠 CPU fence)
			imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			imb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			fns.vkCmdPipelineBarrier(c.cmdS, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb);
			if (fns.vkEndCommandBuffer(c.cmdS) != VK_SUCCESS)
			{
				logLine("PoC-B init: 共享路径 vkEndCommandBuffer 失败 → 回退读回");
				c.useShared = false;
			}
		}
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
	    "PoC-B init 完成: 离屏 %dx%d (洋红清屏+三角形) → %s → CopySubresourceRegion 到 backbuffer (%d,%d)",
	    POCB_W, POCB_H, c.useShared ? "共享纹理 (NT handle+event闸, 无读回)" : "读回", POCB_X,
	    POCB_Y);
	logLine(sum);
	return true;
}

// ============ Step 2c-β (v0.16.0, docs/05 D2a-4): D3D11 SHARED 镜像 → VK 导入 + 交叉校验 ============
// 把 2c-α 建好的**色**镜像 NT handle 用 VK_KHR_external_memory_win32 导成 VkImage,
// 再建一个 host-visible|coherent 的 TRANSFER_DST 读回 buffer 并持久映射, 录两条命令读回用。
// 借用 PoC-B 已就绪的 device/queue/command pool/fence —— 故 **2c-β 依赖 PoC-B 开着**
// (poc-presenter.ini 的 vulkan/pocb 不为 0), 它一关这步自然不跑。
// **失败只关自己**: 一律 logLine + g_ssrVkState=2, 绝不碰 pocbFail (那会连坐关掉 PoC-B 注入)。
// 按槽建图 (v0.16.5): 每次交叉校验调一次 —— 建图 + 导入 + 录命令 + 初转, 由调用方读完即毁
// (任何时刻只有一张 VkImage 持有导入内存, 避开 v0.16.4 双图并存的 DEVICE_LOST)。
// 读回 buffer 与两条命令只在首次创建: POCB_DEV_FNS 里没有 vkFreeCommandBuffers, 反复分配会漏。
bool ssrInVkBuild(PocbCtx& c, int slot)
{
	if (g_ssrVkState == 2)
		return false;
	if (c.state.load() != 2 || !c.vdev || !c.queue || !c.pool || !c.fence)
		return false; // PoC-B 还没就绪, 下一帧再试
	if (!g_ssrSharedOn.load(std::memory_order_relaxed) || !g_ssrInHC || !g_ssrInTexC)
		return false;
	D3D11_TEXTURE2D_DESC md{};
	g_ssrInTexC->GetDesc(&md);
	if (md.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
	{
		// VK 侧映射表只写了色镜像这一种格式 (深度 R2 还没过, 不在本步范围)
		logLine("SSR侦察: [2c-β] 色镜像格式非 RGBA16F (" + ssrFmtName(md.Format) +
		        ") → 跳过 VK 导入 (D3D11 侧入向不受影响)");
		g_ssrVkState = 2;
		return false;
	}
	auto fail = [](const std::string& why) {
		logLine("SSR侦察: [2c-β] " + why + " → VK 交叉校验停用 (D3D11 侧入向照常)");
		g_ssrVkState = 2;
		return false;
	};
	VkResult vr = VK_SUCCESS;
	const SsrVkSlot& sl = g_ssrVkSlots[slot]; // 本槽候选参数

	// --- 1) 外部内存 VkImage (格式/尺寸逐字段对齐色镜像; tiling/handle 类型按槽换) ---
	VkExternalMemoryImageCreateInfo emi{};
	emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	emi.handleTypes = (VkExternalMemoryHandleTypeFlags)sl.handleType; // 槽: OPAQUE_WIN32 / D3D11_TEXTURE
	VkImageCreateInfo ici{};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = &emi;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R16G16B16A16_SFLOAT; // DXGI R16G16B16A16_FLOAT 的 VK 对应, 字节序一致
	ici.extent = VkExtent3D{md.Width, md.Height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = sl.linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL; // 槽: LINEAR 试线性布局
	ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // usage 已被 v0.16.4 变体B 排除 (见 §14.13.3)
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; // 规范只允许 UNDEFINED
	vr = c.fns.vkCreateImage(c.vdev, &ici, nullptr, &g_ssrVkImg);
	if (vr != VK_SUCCESS)
	{
		// 槽参数不被支持是**预期可能** (如 external + LINEAR) ⇒ 只记日志、换槽, 不关整条通路
		logLine("SSR侦察: [2c-β] 槽" + std::to_string(slot) + "(" + sl.tag + ") vkCreateImage = " +
		        pocbCode(vr) + " → 本槽跳过");
		g_ssrVkMakeFail++;
		if (g_ssrVkMakeFail >= g_ssrVkNSlot)
			return fail("所有槽 vkCreateImage 全失败");
		g_ssrVkSlotIdx = (g_ssrVkSlotIdx + 1) % g_ssrVkNSlot;
		return false;
	}
	// (连续失败计数 g_ssrVkMakeFail 不在这里清零 —— 要到整条建成才清, 建图成但导入失败同样算失败)

	// --- 2) 导入 NT handle: 分配即导入 (链进 VkMemoryAllocateInfo) ---
	// v0.16.1 真机归因重试: 首次导入报 VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)。同一套代码
	// PoC-B 的 512x512 RGBA8 导入一直是好的, 色镜像是 1920x1080 RGBA16F —— 差异只有
	// ①格式 ②尺寸 (512x512x8=2,097,152 恰好页对齐; 1920x1080x8=16,588,800 不对齐,
	// D3D11 共享分配八成向上对齐, 而 VK 报的是自己算的 size, 与句柄真实分配对不上就 -2)
	// ③内存类型。所以先跑 [尺寸候选 x 允许内存类型] 矩阵, 每个组合都记码;
	// 矩阵全失败再用两张探针纹理 (RGBA8@1920x1080 / RGBA16F@512x512) 做 2x2 归因,
	// 分清"RGBA16F 不能跨 API"还是"大图分配尺寸对不上" —— 两者改道方向完全不同。
	VkMemoryRequirements req{};
	c.fns.vkGetImageMemoryRequirements(c.vdev, g_ssrVkImg, &req);
	VkPhysicalDeviceMemoryProperties mp{};
	c.fns.vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
	int types[8];
	int nty = 0;
	for (uint32_t i = 0; i < mp.memoryTypeCount && nty < 8; ++i)
	{
		if (!(req.memoryTypeBits & (1u << i)))
			continue;
		if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
			types[nty++] = static_cast<int>(i);
	}
	for (uint32_t i = 0; i < mp.memoryTypeCount && nty < 8; ++i)
	{
		if (!(req.memoryTypeBits & (1u << i)))
			continue;
		bool dup = false;
		for (int j = 0; j < nty; ++j)
			dup = dup || (types[j] == static_cast<int>(i));
		if (!dup)
			types[nty++] = static_cast<int>(i);
	}
	if (nty == 0)
		return fail("色镜像导入无可用内存类型 (memoryTypeBits=" +
		            std::to_string(req.memoryTypeBits) + ")");
	VkDeviceSize cand[6];
	int ncd = 0;
	auto addSz = [&](VkDeviceSize s) {
		if (!s)
			return;
		for (int i = 0; i < ncd; ++i)
			if (cand[i] == s)
				return;
		if (ncd < 6)
			cand[ncd++] = s;
	};
	const VkDeviceSize raw =
	    (VkDeviceSize)md.Width * (VkDeviceSize)md.Height * 8ULL; // RGBA16F = 8B/px
	addSz(req.size);
	addSz((req.size + 65535ULL) & ~(VkDeviceSize)65535ULL);    // VK 报值 64KB 对齐
	addSz((req.size + 2097151ULL) & ~(VkDeviceSize)2097151ULL); // 2MB 对齐
	addSz(raw);
	addSz((raw + 65535ULL) & ~(VkDeviceSize)65535ULL);
	addSz((raw + 2097151ULL) & ~(VkDeviceSize)2097151ULL);
	VkResult vrFirst = VK_SUCCESS;
	std::string codes;
	int hitSz = -1, hitTy = -1, hitK = 0, ntry = 0;
	bool got = false; // 注意: 不能拿 vr 当循环条件 —— 进矩阵时 vr 正是 VK_SUCCESS (建图刚成)
	{
		VkImportMemoryWin32HandleInfoKHR imp{};
		imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
		imp.handleType = (VkExternalMemoryHandleTypeFlagBits)sl.handleType; // 槽: 换 handle 类型试导入
		imp.handle = g_ssrInHC; // 只借用不接管 —— 句柄归 D3D11 侧管理, 不 CloseHandle
		VkMemoryAllocateInfo mai{};
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.pNext = &imp;
		for (int si = 0; si < ncd && !got; ++si)
		{
			for (int ti = 0; ti < nty && !got; ++ti)
			{
				++ntry;
				mai.allocationSize = cand[si];
				mai.memoryTypeIndex = static_cast<uint32_t>(types[ti]);
				vr = c.fns.vkAllocateMemory(c.vdev, &mai, nullptr, &g_ssrVkMem);
				if (vr == VK_SUCCESS)
				{
					// 分配成了 ≠ 能绑上: 尺寸小于 req.size 时 bind 会失败 ⇒ 这种候选
					// 立即释放、继续试下一个, 免得把整条导入一票否决。
					if (c.fns.vkBindImageMemory(c.vdev, g_ssrVkImg, g_ssrVkMem, 0) == VK_SUCCESS)
					{
						got = true;
						hitSz = si;
						hitTy = types[ti];
						hitK = ntry;
						break;
					}
					c.fns.vkFreeMemory(c.vdev, g_ssrVkMem, nullptr);
					g_ssrVkMem = VK_NULL_HANDLE;
					if (codes.size() < 160)
						codes += std::string(codes.empty() ? "" : ",") + "size" +
						         std::to_string((long long)cand[si]) + ":bind失败";
					continue;
				}
				if (ntry == 1)
					vrFirst = vr;
				if (codes.size() < 160)
					codes += (codes.empty() ? "" : ",") + pocbCode(vr);
			}
		}
	}
	if (got)
	{
		std::string why;
		if (hitSz > 0)
			why = " → 病因=尺寸口径 (VK 报值不成, 换对齐值才成)";
		else if (hitK > 1)
			why = " → 病因=内存类型 (第一个类型不成)";
		else
			why = " → 首个组合即成 (尺寸/类型本来就没问题)";
		logLine("SSR侦察: [2c-β]   导入矩阵成功: 第" + std::to_string(hitK) + "/" +
		        std::to_string(ntry) + " 组合 size=" + std::to_string((long long)cand[hitSz]) +
		        " (VK报=" + std::to_string((long long)req.size) + " 朴素=" +
		        std::to_string((long long)raw) + ") type=" + std::to_string(hitTy) + why);
	}
	else
	{
		logLine("SSR侦察: [2c-β]   导入矩阵 " + std::to_string(ntry) + " 次全失败 码=" + codes +
		        " (size候选" + std::to_string(ncd) + "x类型" + std::to_string(nty) +
		        ") VK报size=" + std::to_string((long long)req.size) +
		        " 朴素size=" + std::to_string((long long)raw));
		if (slot != g_ssrVkNSlot - 1)
		{
			// 不是对照槽: 下面的 2x2 探针是按 OPAQUE+OPTIMAL 口径归因的, 对本槽的候选参数
			// 没有意义 ⇒ 只记一行就换槽 (本槽的失败本身就是结论: 该参数导入不了)。
			c.fns.vkDestroyImage(c.vdev, g_ssrVkImg, nullptr);
			g_ssrVkImg = VK_NULL_HANDLE;
			g_ssrVkMakeFail++;
			if (g_ssrVkMakeFail >= g_ssrVkNSlot)
				return fail("所有槽导入全失败 (连挂 " + std::to_string(g_ssrVkMakeFail) + " 个)");
			g_ssrVkSlotIdx = (g_ssrVkSlotIdx + 1) % g_ssrVkNSlot;
			return false;
		}
		// 对照槽: 走原来的 2x2 探针归因 (口径 = 已验过的 OPAQUE_WIN32 + OPTIMAL)
		// ---- 2x2 归因探针: 每张只改一个变量, 其余全抄色镜像 desc ----
		auto probe = [&](const char* tag, DXGI_FORMAT dfmt, VkFormat vfmt, UINT pw,
		                 UINT ph) -> std::string {
			D3D11_TEXTURE2D_DESC pd = md;
			pd.Width = pw;
			pd.Height = ph;
			pd.Format = dfmt;
			pd.MipLevels = 1;
			pd.ArraySize = 1;
			pd.SampleDesc.Count = 1;
			pd.Usage = D3D11_USAGE_DEFAULT;
			pd.CPUAccessFlags = 0;
			pd.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
			ID3D11Texture2D* pt = nullptr;
			HRESULT phr = c.dev ? c.dev->CreateTexture2D(&pd, nullptr, &pt) : E_FAIL;
			if (FAILED(phr) || !pt)
				return std::string(tag) + " 建 D3D11 纹理失败 " + hexHr(phr);
			IDXGIResource1* pres = nullptr;
			phr = pt->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&pres));
			HANDLE hh = nullptr;
			if (SUCCEEDED(phr) && pres)
			{
				phr = pres->CreateSharedHandle(nullptr,
				                               DXGI_SHARED_RESOURCE_READ |
				                                   DXGI_SHARED_RESOURCE_WRITE,
				                               nullptr, &hh);
				pres->Release();
			}
			if (FAILED(phr) || !hh)
			{
				pt->Release();
				return std::string(tag) + " CreateSharedHandle 失败 " + hexHr(phr);
			}
			VkExternalMemoryImageCreateInfo pem{};
			pem.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
			pem.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
			VkImageCreateInfo pic{};
			pic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			pic.pNext = &pem;
			pic.imageType = VK_IMAGE_TYPE_2D;
			pic.format = vfmt;
			pic.extent = VkExtent3D{pw, ph, 1};
			pic.mipLevels = 1;
			pic.arrayLayers = 1;
			pic.samples = VK_SAMPLE_COUNT_1_BIT;
			pic.tiling = VK_IMAGE_TILING_OPTIMAL;
			pic.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			pic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			pic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImage pimg = VK_NULL_HANDLE;
			VkResult pv = c.fns.vkCreateImage(c.vdev, &pic, nullptr, &pimg);
			if (pv != VK_SUCCESS)
			{
				CloseHandle(hh);
				pt->Release();
				return std::string(tag) + " vkCreateImage " + pocbCode(pv);
			}
			VkMemoryRequirements preq{};
			c.fns.vkGetImageMemoryRequirements(c.vdev, pimg, &preq);
			const int bpp = (dfmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4;
			VkDeviceSize pcand[3] = {
			    preq.size,
			    (preq.size + 65535ULL) & ~(VkDeviceSize)65535ULL,
			    (VkDeviceSize)pw * (VkDeviceSize)ph * (VkDeviceSize)bpp,
			};
			int pty[2] = {pocbMemType(mp, preq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
			              pocbMemType(mp, preq.memoryTypeBits, 0)};
			VkResult pv2 = VK_ERROR_INITIALIZATION_FAILED;
			int ptries = 0;
			bool got = false;
			VkDeviceMemory pmem = VK_NULL_HANDLE;
			for (int si = 0; si < 3 && !got; ++si)
			{
				for (int ti = 0; ti < 2 && !got; ++ti)
				{
					if (pty[ti] < 0 || (ti == 1 && pty[1] == pty[0]) || !pcand[si])
						continue;
					++ptries;
					VkImportMemoryWin32HandleInfoKHR pimp{};
					pimp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
					pimp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
					pimp.handle = hh;
					VkMemoryAllocateInfo pmai{};
					pmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
					pmai.pNext = &pimp;
					pmai.allocationSize = pcand[si];
					pmai.memoryTypeIndex = static_cast<uint32_t>(pty[ti]);
					pv2 = c.fns.vkAllocateMemory(c.vdev, &pmai, nullptr, &pmem);
					got = (pv2 == VK_SUCCESS);
				}
			}
			if (pv2 == VK_SUCCESS)
				c.fns.vkFreeMemory(c.vdev, pmem, nullptr);
			c.fns.vkDestroyImage(c.vdev, pimg, nullptr);
			CloseHandle(hh);
			pt->Release();
			return std::string(tag) +
			       (pv2 == VK_SUCCESS
			                ? std::string(" 导入 OK")
			                : (ptries == 0 ? std::string(" 无可用内存类型")
			                               : std::string(" 导入失败 ") + pocbCode(pv2))) +
			       " VK报size=" + std::to_string((long long)preq.size);
		};
		const std::string pA =
		    probe("归因探测A RGBA8@1920x1080(只换格式)", DXGI_FORMAT_R8G8B8A8_UNORM,
		          VK_FORMAT_R8G8B8A8_UNORM, md.Width, md.Height);
		const std::string pB = probe("归因探测B RGBA16F@512x512(只换尺寸)",
		                             DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
		                             512, 512);
		logLine("SSR侦察: [2c-β] " + pA);
		logLine("SSR侦察: [2c-β] " + pB);
		const bool aOK = pA.find("导入 OK") != std::string::npos;
		const bool bOK = pB.find("导入 OK") != std::string::npos;
		std::string concl;
		if (aOK && !bOK)
			concl = "病因是格式 — RGBA16F 跨不了 API, 色通路需改道 (换格式或退回 D3D11 STAGING)";
		else if (!aOK && bOK)
			concl = "病因是尺寸 — 大图分配尺寸与句柄真实分配对不上, 需按对齐口径修 allocationSize";
		else if (!aOK && !bOK)
			concl = "格式与尺寸单独都不行 — 只有既小又页对齐的组合能导入";
		else
			concl = "两个探针单独都能导入 — 主纹理失败在组合差异, 看上面矩阵哪一组合赢";
		logLine("SSR侦察: [2c-β]   归因结论: " + concl);
		return fail("vkAllocateMemory(NT handle 导入) = " + pocbCode(vrFirst) +
		            " (矩阵/探针归因见上, 病因=" + concl + ")");
	}
	// bind 已在矩阵里做掉 (got=true 即绑上); 这里只兜底断言一次 —— 防呆, 正常永不触发
	if (!g_ssrVkMem)
		return fail("vkBindImageMemory(色镜像): 分配在但没绑上");

	// --- 3) 读回 buffer: 紧密排列 width*height*8, host visible|coherent (持久映射) —— 只建一次 ---
	if (!g_ssrVkBuf)
	{
	g_ssrVkW = md.Width;
	g_ssrVkH = md.Height;
	{
		const VkDeviceSize sz = (VkDeviceSize)md.Width * (VkDeviceSize)md.Height * 8ULL;
		VkBufferCreateInfo bci{};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = sz;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		vr = c.fns.vkCreateBuffer(c.vdev, &bci, nullptr, &g_ssrVkBuf);
		if (vr != VK_SUCCESS)
			return fail("vkCreateBuffer(读回) = " + pocbCode(vr));
		VkMemoryRequirements breq{};
		c.fns.vkGetBufferMemoryRequirements(c.vdev, g_ssrVkBuf, &breq);
		// COHERENT 不可省: vkInvalidateMappedMemoryRanges 不在 POCB_DEV_FNS 里
		const int bt = pocbMemType(mp, breq.memoryTypeBits,
		                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (bt < 0)
			return fail("读回 buffer 无 host visible|coherent 内存类型");
		VkMemoryAllocateInfo mai{};
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = breq.size;
		mai.memoryTypeIndex = static_cast<uint32_t>(bt);
		vr = c.fns.vkAllocateMemory(c.vdev, &mai, nullptr, &g_ssrVkBufMem);
		if (vr != VK_SUCCESS)
			return fail("vkAllocateMemory(读回 buffer) = " + pocbCode(vr));
		if (c.fns.vkBindBufferMemory(c.vdev, g_ssrVkBuf, g_ssrVkBufMem, 0) != VK_SUCCESS)
			return fail("vkBindBufferMemory(读回 buffer)");
		vr = c.fns.vkMapMemory(c.vdev, g_ssrVkBufMem, 0, VK_WHOLE_SIZE, 0, &g_ssrVkBufPtr);
		if (vr != VK_SUCCESS || !g_ssrVkBufPtr)
			return fail("vkMapMemory(读回 buffer) = " + pocbCode(vr));
	}
	}

	// --- 4) 两条命令: [0] 布局初转, [1] 读回 —— **分配一次** (POCB_DEV_FNS 没有
	// vkFreeCommandBuffers, 每槽重分会漏), 每槽重新 begin 录制 (ONE_TIME 提交完自动回
	// initial state ⇒ 可以直接重录, 不用 reset)。
	if (!g_ssrVkCmd0 || !g_ssrVkCmd)
	{
		VkCommandBuffer cmds[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		VkCommandBufferAllocateInfo cbai{};
		cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cbai.commandPool = c.pool;
		cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbai.commandBufferCount = 2;
		if (c.fns.vkAllocateCommandBuffers(c.vdev, &cbai, cmds) != VK_SUCCESS)
			return fail("vkAllocateCommandBuffers(读回)");
		g_ssrVkCmd0 = cmds[0];
		g_ssrVkCmd = cmds[1];
	}
	VkCommandBufferBeginInfo cbb{};
	cbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	// 关键: 两条命令都按 ONE_TIME_SUBMIT 录 —— 每槽的 VkImage handle 不同, 每次建图都要
	// 重新录; ONE_TIME 提交完成后命令自动回 initial state ⇒ 可以直接重录 (不用 reset,
	// POCB_DEV_FNS 里也没有 vkResetCommandBuffer)。
	auto barrier = [&](VkCommandBuffer cm, VkImage img, VkPipelineStageFlags ss, VkAccessFlags sa,
	                   VkPipelineStageFlags ds, VkAccessFlags da, VkImageLayout ol,
	                   VkImageLayout nl) {
		VkImageMemoryBarrier imb{};
		imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imb.srcAccessMask = sa;
		imb.dstAccessMask = da;
		imb.oldLayout = ol;
		imb.newLayout = nl;
		imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imb.image = img;
		imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		c.fns.vkCmdPipelineBarrier(cm, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &imb);
	};
	// [0] UNDEFINED → GENERAL: initialLayout 只能是 UNDEFINED, 但读回命令复用后不能再出现
	// UNDEFINED (那等于每次读都允许丢内容) ⇒ 初转单独一条, 建好时立即提交并等完。
	// 初转按规范**可能丢掉本帧 D3D11 刚拷进来的内容** ⇒ 调用方在这次"建图帧"只建不比,
	// 等下一次交叉校验机会 (那帧特征B 已重新拷过) 才读回比对。
	cbb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (c.fns.vkBeginCommandBuffer(g_ssrVkCmd0, &cbb) != VK_SUCCESS)
		return fail("vkBeginCommandBuffer(初转)");
	barrier(g_ssrVkCmd0, g_ssrVkImg, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
	        VK_IMAGE_LAYOUT_GENERAL);
	if (c.fns.vkEndCommandBuffer(g_ssrVkCmd0) != VK_SUCCESS)
		return fail("vkEndCommandBuffer(初转)");
	// [1] 读回: 默认 GENERAL → TRANSFER_SRC → copy → TRANSFER_SRC → GENERAL;
	//     槽"全程GENERAL" 则不转 layout, 只用 access mask 做内存栅栏 (oldLayout==newLayout)。
	if (c.fns.vkBeginCommandBuffer(g_ssrVkCmd, &cbb) != VK_SUCCESS)
		return fail("vkBeginCommandBuffer(读回)");
	// srcAccess=MEMORY_WRITE: 跨 API 的这次写不归 VK 记账, 用"全部写"把 VK 侧缓存失效掉;
	// 而 D3D11 侧的 Map 返回已保证 D3D11 队列真的跑完了 (CPU 都读到过) ⇒ 不存在时序竞态。
	const VkImageLayout rdSrc = sl.noTrans ? VK_IMAGE_LAYOUT_GENERAL
	                                       : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier(g_ssrVkCmd, g_ssrVkImg, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, rdSrc);
	{
		VkBufferImageCopy bic{};
		bic.bufferRowLength = 0; // 0 = 紧密排列 ⇒ 行距 = width*8
		bic.bufferImageHeight = 0;
		bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		bic.imageOffset = VkOffset3D{0, 0, 0};
		bic.imageExtent = VkExtent3D{md.Width, md.Height, 1};
		c.fns.vkCmdCopyImageToBuffer(g_ssrVkCmd, g_ssrVkImg, rdSrc, g_ssrVkBuf, 1, &bic);
	}
	barrier(g_ssrVkCmd, g_ssrVkImg, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT, rdSrc,
	        VK_IMAGE_LAYOUT_GENERAL);
	if (c.fns.vkEndCommandBuffer(g_ssrVkCmd) != VK_SUCCESS)
		return fail("vkEndCommandBuffer(读回)");

	// 提交初转并等完 —— 之后 g_ssrVkImg 的既定布局就是 GENERAL, 读回命令的前提恒成立
	c.fns.vkResetFences(c.vdev, 1, &c.fence);
	{
		VkSubmitInfo si{};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &g_ssrVkCmd0;
		vr = c.fns.vkQueueSubmit(c.queue, 1, &si, c.fence);
		if (vr == VK_SUCCESS)
			vr = c.fns.vkWaitForFences(c.vdev, 1, &c.fence, VK_TRUE, 5000000000ULL);
	}
	if (vr != VK_SUCCESS)
		return fail("初转 vkQueueSubmit/vkWaitForFences = " + pocbCode(vr));

	g_ssrVkState = 1;
	g_ssrVkMakeFail = 0; // 整条建成 ⇒ 连续失败计数清零
	if (!g_ssrVkLogFirst)
	{
		g_ssrVkLogFirst = true;
		logLine("SSR侦察: [2c-β] VK 导入 OK: 色镜像 NT handle → VkImage (首次, " +
		        std::string(sl.tag) + ") " + std::to_string(md.Width) + "x" +
		        std::to_string(md.Height) + " RGBA16F, 读回buffer=" +
		        std::to_string((long long)md.Width * md.Height * 8) +
		        "B host-coherent, 命令已录 (初转+读回各一条) — 之后每槽现建现毁 (v0.16.5)");
	}
	else
	{
		logLine("SSR侦察: [2c-β] 槽" + std::to_string(slot) + "(" + sl.tag + ") 建图+导入OK: " +
		        std::to_string(md.Width) + "x" + std::to_string(md.Height) +
		        " → 本帧只建不比 (初转后按规范可能丢内容), 下次交叉校验出对比值");
	}
	return true;
}

// 读完立刻放: 任何时刻**只有一张** VkImage 持有这块导入内存 (v0.16.4 双图并存 →
// VK_ERROR_DEVICE_LOST → PoC-B 被连坐关闭注入, 画面上的 VK 三角消失)。
void ssrVkFree(PocbCtx& c)
{
	if (g_ssrVkImg)
	{
		c.fns.vkDeviceWaitIdle(c.vdev); // 先确认 GPU 真的不再碰这块内存, 再销毁
		c.fns.vkDestroyImage(c.vdev, g_ssrVkImg, nullptr);
		g_ssrVkImg = VK_NULL_HANDLE;
	}
	if (g_ssrVkMem)
	{
		c.fns.vkFreeMemory(c.vdev, g_ssrVkMem, nullptr);
		g_ssrVkMem = VK_NULL_HANDLE;
	}
	// 2d-1: 入向图没了 ⇒ 出向图绑定的 handle 也一并作废, 录制时记的 src handle 清掉,
	// 下一帧 ssrOutVkFrame 会走 g_ssrVkCmdOutSrc != g_ssrVkImg 重新建+重录。
	// (POCB_DEV_FNS 没有 vkFreeCommandBuffers ⇒ 命令 buffer 本身留着重录复用。)
	if (g_ssrVkImgOut)
	{
		c.fns.vkDeviceWaitIdle(c.vdev);
		c.fns.vkDestroyImage(c.vdev, g_ssrVkImgOut, nullptr);
		g_ssrVkImgOut = VK_NULL_HANDLE;
	}
	if (g_ssrVkMemOut)
	{
		c.fns.vkFreeMemory(c.vdev, g_ssrVkMemOut, nullptr);
		g_ssrVkMemOut = VK_NULL_HANDLE;
	}
	g_ssrVkCmdOutSrc = VK_NULL_HANDLE;
	g_ssrVkOutState = 0;
}

// 每帧收尾时调用 (只在 D3D11 侧做过读回的那几帧真正干活, 节奏 = 前3次+每600次)。
// v0.16.5 轮换单图节奏:
//   · 无活图 → 按当前槽建图 + 初转, **本帧不比** (UNDEFINED→GENERAL 按规范可能丢内容);
//   · 有活图 → 读回比对 (本帧特征B 已拷过, 内容是新的) → 立刻销毁 → 换下一槽。
//   ⇒ 每个槽要两次交叉校验机会 (一次建、一次读), 4 槽全覆盖 ≈ 8 次 ≈ 4100 帧。
void ssrInVkFrame(PocbCtx& c)
{
	const bool want = g_ssrInChkCValid;
	const unsigned long long d3dH = g_ssrInChkC;
	const unsigned long long d3dPrev = g_ssrInChkCPrev;
	g_ssrInChkCPrev = d3dH;
	g_ssrInChkCValid = false; // 一次性消费, 免得下一帧拿旧值去比
	if (!want)
		return;
	if (!g_ssrSharedOn.load(std::memory_order_relaxed) || !g_ssrInHC)
		return;
	if (g_ssrVkState == 2)
		return;
	const long k = ++g_ssrVkN; // 第 k 次交叉校验机会 (含"只建不比"的那一次)
	if (g_ssrVkImg == VK_NULL_HANDLE)
	{
		ssrInVkBuild(c, g_ssrVkSlotIdx); // 成功 → 本帧不比; 失败 → 内部已记日志并换槽/关闸
		return;
	}
	const int slot = g_ssrVkSlotIdx;
	const SsrVkSlot& sl = g_ssrVkSlots[slot];

	c.fns.vkResetFences(c.vdev, 1, &c.fence);
	VkSubmitInfo si{};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &g_ssrVkCmd;
	const auto t0 = std::chrono::steady_clock::now();
	VkResult vr = c.fns.vkQueueSubmit(c.queue, 1, &si, c.fence);
	if (vr == VK_SUCCESS)
		vr = c.fns.vkWaitForFences(c.vdev, 1, &c.fence, VK_TRUE, 5000000000ULL);
	if (vr != VK_SUCCESS || !g_ssrVkBufPtr)
	{
		// DEVICE_LOST 会连坐关掉 PoC-B (画面上的 VK 三角就没了) ⇒ 单独打 [异常] 便于判读
		if (vr == VK_ERROR_DEVICE_LOST)
			logLine("SSR侦察: [异常] [2c-β] 槽" + std::to_string(slot) + "(" + sl.tag +
			        ") 读回提交 DEVICE_LOST (-4) → 2c-β 停用 (PoC-B 可能已被连坐关掉)");
		else
			logLine("SSR侦察: [2c-β] 读回提交失败 " + pocbCode(vr) + " → VK 交叉校验停用");
		g_ssrVkState = 2;
		return;
	}
	long nzk = 0;
	// **同一个 ssrFnvSample**, 同样的 64行x16列/8字节抽样 ⇒ 两个值可直接相等比较
	const size_t vkPitch = (size_t)g_ssrVkW * 8;
	const unsigned long long vkH = ssrFnvSample(g_ssrVkBufPtr, vkPitch, g_ssrVkW, g_ssrVkH, 8, &nzk);
	// ---- v0.16.3 β3 归因三件套 (首跑 4/4 全不一致, docs/02 §14.13 归因1/2/3) ----
	// ① 同刻再读一次 D3D11: 若 同刻==VK ⇒ 不是布局/行距, 是**两边读的不是同一时刻**;
	//    若 同刻≠帧内那次 ⇒ 镜像在特征B 之后还被改过 (时间窗本身有洞)。
	// ② 前帧 D3D11 校验和: VK==前帧 ⇒ 差一帧 (提交时机在 D3D11 写之前)。
	// ③ 按 D3D11 的 RowPitch 重算 VK: 相等 ⇒ 两边抽样点位差在行距 (归因3), 与内容无关。
	unsigned long long d3dNow = 0;
	long nzNow = 0;
	if (c.ctx && g_ssrInTexC && g_ssrInStgC)
		d3dNow = ssrInFnv(c.ctx, g_ssrInTexC, g_ssrInStgC, "2c-β同刻", &nzNow);
	const unsigned long long vkH2 =
	    (g_ssrInChkPitch && g_ssrInChkPitch != vkPitch)
	        ? ssrFnvSample(g_ssrVkBufPtr, g_ssrInChkPitch, g_ssrVkW, g_ssrVkH, 8, nullptr)
	        : 0;
	const bool hitNow = (d3dNow != 0 && d3dNow == vkH);
	const bool hitPrev = (d3dPrev != 0 && vkH == d3dPrev);
	const bool hitPitch = (vkH2 != 0 && vkH2 == d3dH);
	const bool nowEqFrame = (d3dNow == d3dH);
	const double ms = std::chrono::duration<double, std::milli>(
	    std::chrono::steady_clock::now() - t0).count();
	std::string diag = " 槽" + std::to_string(slot) + "(" + sl.tag + ")" +
	                   " 行距=" + std::to_string((long long)g_ssrInChkPitch) + "/" +
	                   std::to_string((long long)vkPitch) +
	                   " 同刻D3D11=0x" + uhex64(d3dNow) +
	                   (hitNow ? "=VK✓时序差" : (nowEqFrame ? "≠VK" : "≠帧内(镜像帧内被改)")) +
	                   " 前帧D3D11=0x" + uhex64(d3dPrev) + (hitPrev ? "=VK✓差一帧" : "");
	if (vkH2 != 0)
		diag += " VK按D3D11行距=0x" + uhex64(vkH2) + (hitPitch ? "=D3D11✓行距归因" : "");
	logLine("SSR侦察: [2c-β] 交叉校验#" + std::to_string(k) + " D3D11=0x" + uhex64(d3dH) +
	        " VK=0x" + uhex64(vkH) + (d3dH == vkH ? " **一致✓**" : " **不一致✗**") +
	        " VK非零=" + std::to_string(nzk) + diag + " 本帧VK读回=" +
	        std::to_string(ms).substr(0, 5) + "ms" +
	        " 帧=" + std::to_string(g_presentCount.load(std::memory_order_relaxed)));
	// 不一致**不关闸** —— 它正是 2c-β 要采的样本 (各槽参数的字节视图对比, docs/02 §14.13.3)。
	// 单槽 (v0.16.6): 图常驻, 每次交叉校验机会都出一行对比; 多槽才"读完即毁换下一槽"。
	if (g_ssrVkRotate)
	{
		ssrVkFree(c);
		g_ssrVkSlotIdx = (slot + 1) % g_ssrVkNSlot;
	}
}

// ===========================================================================
// 2d-1 出向回写 (v0.18.0) —— VK 渲完 → 第 3 张 SHARED 纹理 → D3D11 CopyResource(585)
// ---------------------------------------------------------------------------
// 时序刻意做成 **1 帧延迟**, 免掉"在 D3D11 钩子里等 VK GPU"和"在 VK 里等 D3D11 GPU"
// 这两种跨 API 栅栏 (前者会拖住 D3D11 队列, 后者 D3D11 根本没有 VK 能等的 fence):
//   本帧特征B   : D3D11 CopyResource(324 → 入向镜像)        [2c, 已有]
//   本帧 Present: VK 读入向镜像 → SSR(v0 passthrough) → 写出向镜像, CPU fence 等完
//   下帧特征B   : D3D11 CopyResource(出向镜像 → 585)         [2d, 本函数的对端]
// 代价 = 反射内容比画面晚 1 帧 (16ms); 收益 = 零跨 API GPU 同步、零每帧阻塞点。
// v0 passthrough 下 585 拿到的就是 324 内容 (与 2b 哨兵同观感), 验的是通路不是画质。
void ssrOutVkBuild(PocbCtx& c)
{
	if (g_ssrVkOutState == 2)
		return;
	if (c.state.load() != 2 || !c.vdev || !c.queue || !c.pool || !c.fence)
		return; // PoC-B 还没就绪, 下一帧再试
	if (!g_ssrVkOutOn.load(std::memory_order_relaxed) ||
	    !g_ssrSharedOn.load(std::memory_order_relaxed))
		return;
	if (!g_ssrOutTexC || !g_ssrOutHC)
		return; // D3D11 侧出向镜像还没建, 下一帧再试
	if (g_ssrVkImg == VK_NULL_HANDLE)
		return; // 入向图还没建 (出向命令要录它) —— 先让 2c-β 把入向建起来
	D3D11_TEXTURE2D_DESC od{};
	g_ssrOutTexC->GetDesc(&od);
	if (od.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
	{
		logLine("SSR侦察: [2d] 出向镜像格式非 RGBA16F (" + ssrFmtName(od.Format) +
		        ") → 出向回写停用 (入向 2c 不受影响)");
		g_ssrVkOutState = 2;
		return;
	}
	auto failOut = [](const std::string& why) {
		logLine("SSR侦察: [2d] " + why + " → 出向回写停用 (入向 2c 照常)");
		g_ssrVkOutState = 2;
		return false;
	};

	// --- 1) 出向 VkImage: usage = TRANSFER_DST, handle 类型沿用已验通的 D3D11_TEXTURE_BIT ---
	VkExternalMemoryImageCreateInfo emi{};
	emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	VkImageCreateInfo ici{};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = &emi;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R16G16B16A16_SFLOAT; // DXGI R16G16B16A16_FLOAT, 字节序一致
	ici.extent = VkExtent3D{od.Width, od.Height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; // 只作拷贝目的地 (入向图才是 SRC)
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkResult vr = c.fns.vkCreateImage(c.vdev, &ici, nullptr, &g_ssrVkImgOut);
	if (vr != VK_SUCCESS)
	{
		failOut("出向 vkCreateImage = " + pocbCode(vr));
		return;
	}

	// --- 2) 导入 g_ssrOutHC (与入向那块**不同**的 NT handle ⇒ 不同内存, 不踩 v0.16.4) ---
	// 尺寸候选沿用 2c-β 归因结论 (req.size / 64KB / 2MB / 朴素), 每个候选都记码;
	// 一旦全失败只关出向, 入向照跑 —— 出向是新增通路, 没有"必须成"的理由。
	VkMemoryRequirements req{};
	c.fns.vkGetImageMemoryRequirements(c.vdev, g_ssrVkImgOut, &req);
	VkPhysicalDeviceMemoryProperties mp{};
	c.fns.vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
	int types[8];
	int nty = 0;
	for (uint32_t i = 0; i < mp.memoryTypeCount && nty < 8; ++i)
	{
		if (!(req.memoryTypeBits & (1u << i)))
			continue;
		if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
			types[nty++] = static_cast<int>(i);
	}
	for (uint32_t i = 0; i < mp.memoryTypeCount && nty < 8; ++i)
	{
		if (!(req.memoryTypeBits & (1u << i)))
			continue;
		bool dup = false;
		for (int j = 0; j < nty; ++j)
			dup = dup || (types[j] == static_cast<int>(i));
		if (!dup)
			types[nty++] = static_cast<int>(i);
	}
	const VkDeviceSize raw = (VkDeviceSize)od.Width * (VkDeviceSize)od.Height * 8ULL;
	VkDeviceSize cand[6];
	int ncd = 0;
	auto addSz = [&](VkDeviceSize s) {
		if (!s)
			return;
		for (int i = 0; i < ncd; ++i)
			if (cand[i] == s)
				return;
		if (ncd < 6)
			cand[ncd++] = s;
	};
	addSz(req.size);
	addSz((req.size + 65535ULL) & ~(VkDeviceSize)65535ULL);
	addSz((req.size + 2097151ULL) & ~(VkDeviceSize)2097151ULL);
	addSz(raw);
	addSz((raw + 65535ULL) & ~(VkDeviceSize)65535ULL);
	addSz((raw + 2097151ULL) & ~(VkDeviceSize)2097151ULL);
	if (nty == 0 || ncd == 0)
	{
		c.fns.vkDestroyImage(c.vdev, g_ssrVkImgOut, nullptr);
		g_ssrVkImgOut = VK_NULL_HANDLE;
		failOut("出向导入无可用内存类型/尺寸候选 (memoryTypeBits=" +
		        std::to_string(req.memoryTypeBits) + ")");
		return;
	}
	VkImportMemoryWin32HandleInfoKHR imp{};
	imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	imp.handle = g_ssrOutHC; // 只借用不接管 —— 句柄归 D3D11 侧, 不 CloseHandle
	VkMemoryAllocateInfo mai{};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.pNext = &imp;
	bool got = false;
	std::string codes;
	int ntry = 0;
	for (int si = 0; si < ncd && !got; ++si)
	{
		for (int ti = 0; ti < nty && !got; ++ti)
		{
			++ntry;
			mai.allocationSize = cand[si];
			mai.memoryTypeIndex = static_cast<uint32_t>(types[ti]);
			vr = c.fns.vkAllocateMemory(c.vdev, &mai, nullptr, &g_ssrVkMemOut);
			if (vr == VK_SUCCESS)
			{
				if (c.fns.vkBindImageMemory(c.vdev, g_ssrVkImgOut, g_ssrVkMemOut, 0) == VK_SUCCESS)
				{
					got = true;
					break;
				}
				c.fns.vkFreeMemory(c.vdev, g_ssrVkMemOut, nullptr);
				g_ssrVkMemOut = VK_NULL_HANDLE;
				if (codes.size() < 160)
					codes += std::string(codes.empty() ? "" : ",") + "bind失败";
				continue;
			}
			if (codes.size() < 160)
				codes += (codes.empty() ? "" : ",") + pocbCode(vr);
		}
	}
	if (!got)
	{
		c.fns.vkDestroyImage(c.vdev, g_ssrVkImgOut, nullptr);
		g_ssrVkImgOut = VK_NULL_HANDLE;
		failOut("出向导入矩阵 " + std::to_string(ntry) + " 次全失败 码=" + codes +
		        " (size候选" + std::to_string(ncd) + "x类型" + std::to_string(nty) + ")");
		return;
	}

	// --- 3) 一条命令: 入向→源 / 出向→目的 / copy / 两边复原 + 交接 GENERAL ---
	// ONE_TIME_SUBMIT 录完提交完自动回 initial state ⇒ 可直接重录 (POCB_DEV_FNS 没有
	// vkResetCommandBuffer); 句柄变了 (入向图被重建) 由 g_ssrVkCmdOutSrc 触发重录。
	if (!g_ssrVkCmdOut)
	{
		VkCommandBuffer cmds[1] = {VK_NULL_HANDLE};
		VkCommandBufferAllocateInfo cbai{};
		cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cbai.commandPool = c.pool;
		cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbai.commandBufferCount = 1;
		if (c.fns.vkAllocateCommandBuffers(c.vdev, &cbai, cmds) != VK_SUCCESS)
		{
			c.fns.vkDeviceWaitIdle(c.vdev);
			c.fns.vkDestroyImage(c.vdev, g_ssrVkImgOut, nullptr);
			g_ssrVkImgOut = VK_NULL_HANDLE;
			c.fns.vkFreeMemory(c.vdev, g_ssrVkMemOut, nullptr);
			g_ssrVkMemOut = VK_NULL_HANDLE;
			failOut("vkAllocateCommandBuffers(出向)");
			return;
		}
		g_ssrVkCmdOut = cmds[0];
	}
	auto barrier = [&](VkImage img, VkPipelineStageFlags ss, VkAccessFlags sa,
	                   VkPipelineStageFlags ds, VkAccessFlags da, VkImageLayout ol,
	                   VkImageLayout nl) {
		VkImageMemoryBarrier imb{};
		imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imb.srcAccessMask = sa;
		imb.dstAccessMask = da;
		imb.oldLayout = ol;
		imb.newLayout = nl;
		imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imb.image = img;
		imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		c.fns.vkCmdPipelineBarrier(g_ssrVkCmdOut, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &imb);
	};
	VkCommandBufferBeginInfo cbb{};
	cbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (c.fns.vkBeginCommandBuffer(g_ssrVkCmdOut, &cbb) != VK_SUCCESS)
	{
		c.fns.vkDeviceWaitIdle(c.vdev);
		c.fns.vkDestroyImage(c.vdev, g_ssrVkImgOut, nullptr);
		g_ssrVkImgOut = VK_NULL_HANDLE;
		c.fns.vkFreeMemory(c.vdev, g_ssrVkMemOut, nullptr);
		g_ssrVkMemOut = VK_NULL_HANDLE;
		failOut("vkBeginCommandBuffer(出向)");
		return;
	}
	// ① 入向 GENERAL → TRANSFER_SRC: srcAccess=MEMORY_WRITE 把 D3D11 上一帧的跨 API 写算进去
	//    (那笔写不归 VK 记账, 用"全部写"让 VK 侧缓存失效才读得到新内容)。
	barrier(g_ssrVkImg, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
	        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	// ② 出向 UNDEFINED → TRANSFER_DST: 每帧都整幅重写, oldLayout=UNDEFINED 允许丢弃上一帧
	barrier(g_ssrVkImgOut, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	{
		VkImageCopy icp{};
		icp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		icp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		icp.extent = VkExtent3D{od.Width, od.Height, 1};
		c.fns.vkCmdCopyImage(g_ssrVkCmdOut, g_ssrVkImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                     g_ssrVkImgOut, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &icp);
	}
	// ③ 入向复原 GENERAL —— 下一帧 D3D11 要直接往里 CopyResource, 布局不复原就会撞车
	barrier(g_ssrVkImg, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT,
	        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	// ④ 出向 TRANSFER_DST → GENERAL + dstAccess=MEMORY_READ: 跨 API 交接只认 GENERAL,
	//    让驱动把这次写做完再放行 (真正的交接仍靠 CPU 的 vkWaitForFences)
	barrier(g_ssrVkImgOut, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT,
	        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	if (c.fns.vkEndCommandBuffer(g_ssrVkCmdOut) != VK_SUCCESS)
	{
		c.fns.vkDeviceWaitIdle(c.vdev);
		c.fns.vkDestroyImage(c.vdev, g_ssrVkImgOut, nullptr);
		g_ssrVkImgOut = VK_NULL_HANDLE;
		c.fns.vkFreeMemory(c.vdev, g_ssrVkMemOut, nullptr);
		g_ssrVkMemOut = VK_NULL_HANDLE;
		failOut("vkEndCommandBuffer(出向)");
		return;
	}
	g_ssrVkCmdOutSrc = g_ssrVkImg;
	g_ssrVkOutState = 1;
	logLine("SSR侦察: [2d] 出向图就绪: " + std::to_string(od.Width) + "x" +
	        std::to_string(od.Height) + " RGBA16F TRANSFER_DST → 导入 handle=" + hexOf(g_ssrOutHC) +
	        " (矩阵 " + std::to_string(ntry) + " 次) 命令已录 — 本帧只建不提, 下帧起每帧出向");
}

// 每帧 (pocbInject 尾部, 紧跟 ssrInVkFrame): 入向镜像 → 出向镜像 拷一次 + CPU fence 等完,
// 置 g_ssrOutReady 让 D3D11 在**下一帧**特征B 拷进 585。失败只关出向。
void ssrOutVkFrame(PocbCtx& c)
{
	if (!g_ssrVkOutOn.load(std::memory_order_relaxed) ||
	    !g_ssrSharedOn.load(std::memory_order_relaxed))
		return;
	if (g_ssrVkOutState == 2 || g_ssrVkState == 2)
		return;
	if (c.state.load() != 2 || !c.vdev || !c.queue || !c.fence)
		return;
	if (!g_ssrOutTexC || !g_ssrOutHC)
		return;
	if (g_ssrVkImg == VK_NULL_HANDLE)
		return; // 入向还没建 —— 让 ssrInVkFrame 先把入向建起来
	if (g_ssrVkImgOut == VK_NULL_HANDLE || g_ssrVkCmdOutSrc != g_ssrVkImg)
	{
		ssrOutVkBuild(c); // 成功 → 本帧不提 (与 2c-β "只建不比" 同节奏); 失败 → 内部已关闸
		return;
	}
	c.fns.vkResetFences(c.vdev, 1, &c.fence);
	VkSubmitInfo si{};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &g_ssrVkCmdOut;
	const auto t0 = std::chrono::steady_clock::now();
	VkResult vr = c.fns.vkQueueSubmit(c.queue, 1, &si, c.fence);
	if (vr == VK_SUCCESS)
		vr = c.fns.vkWaitForFences(c.vdev, 1, &c.fence, VK_TRUE, 5000000000ULL);
	const double ms = std::chrono::duration<double, std::milli>(
	    std::chrono::steady_clock::now() - t0).count();
	if (vr != VK_SUCCESS)
	{
		if (vr == VK_ERROR_DEVICE_LOST)
			logLine("SSR侦察: [异常] [2d] 出向提交 DEVICE_LOST (-4) → 出向回写停用 "
			        "(PoC-B 可能已被连坐关掉)");
		else
			logLine("SSR侦察: [2d] 出向提交失败 " + pocbCode(vr) + " → 出向回写停用");
		g_ssrVkOutState = 2;
		g_ssrOutReady = false; // 不就绪 ⇒ D3D11 侧不回写 ⇒ 画面保持游戏原样 (最稳的降级)
		return;
	}
	g_ssrOutReady = true; // 1 帧延迟: D3D11 下一帧特征B 才读, 不存在跨 API GPU 栅栏
	const long k = ++g_ssrVkOutN;
	if (k <= 8 || (k % 128) == 0)
		logLine("SSR侦察: [2d] VK出向#" + std::to_string(k) + " 入向→出向 " +
		        std::to_string(g_ssrVkW) + "x" + std::to_string(g_ssrVkH) +
		        " RGBA16F fence=" + std::to_string(ms).substr(0, 5) +
		        "ms 帧=" + std::to_string(g_presentCount.load(std::memory_order_relaxed)) +
		        " [下帧特征B 回写585]");
}

// ---- 2d-4 路线 1′ 探测 (v0.18.3, docs/05 D2a-4 末「路线 1′」/ docs/02 §14.19) ----
// 问题: D24 家族走**单独 SHARED (不带 NTHANDLE)** 能拿老式 handle (2d-3 第 1 格已证),
// 那 VK 侧用 VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT 能不能把它导成 VkImage?
// 能 ⇒ 深度原样进 VK, 省掉路线 1 那每帧一次全屏 PS; 不能 ⇒ 回到路线 1 (R32_FLOAT 全屏 PS)。
// **纯发现**: 建出来的立刻 destroy, 不写 g_ssrIn* 与 g_ssrOut* 任何状态、不改本轮回写行为;
// 由 main.cpp 的静态 flag 控制整轮只跑一次。老式 handle 归 DXGI/D3D11 所有 ⇒ 这里**不 CloseHandle**。
// 两问分开答: #3 = 查支持度 (vkGetPhysicalDeviceImageFormatProperties2, 1.1+ 才有这个函数),
// #4 = 实测 建图→导入→绑定 (查询缺席/说不行时照样实测 D24 那格, 不因查询缺席就下结论)。
bool ssrKmtProbeVk(HANDLE h, const D3D11_TEXTURE2D_DESC& sd)
{
	// 早退也把 #3/#4 两行都补齐 (判读按"行数 = 4"数, 少一行会被当成探测没跑完)
	auto fail = [](const char* step, long code) -> bool {
		logLine("SSR侦察: [2d-4]   KMT探测#3 VK 支持度查询跳过 @" + std::string(step));
		logLine("SSR侦察: [2d-4]   KMT探测#4 VK 导入实测跳过 @" + std::string(step) + " " +
		        pocbCode(code));
		return false;
	};
	if (!h)
		return fail("D3D11 没给出老式 handle", 0);
	PocbCtx& c = g_pocb;
	if (c.state.load() != 2 || !c.inst || !c.phys || !c.vdev || !c.queue)
		return fail("g_pocb 未就绪 (VK 还没 init 完)", 0);
	if (sd.SampleDesc.Count != 1)
		return fail("源是多重采样, 本探测没做 msaa 映射", 0);

	// 源是 Format=44 (R24G8_TYPELESS) ⇒ VK 侧只可能落在 D24/S8 这一族
	const VkFormat cand[2] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};

	// --- ① 支持度查询 ---
	std::string qres;
	bool needTry[2] = {true, false}; // D24 那格无论如何都实测; D32S8 只有查询说可导才试
	for (int i = 0; i < 2; ++i)
	{
		if (i)
			qres += "; ";
		qres += (i == 0 ? "D24/S8" : "D32S8");
		if (!c.fns.vkGetPhysicalDeviceImageFormatProperties2)
		{
			qres += "=查询跳过(函数缺席)";
			continue;
		}
		VkPhysicalDeviceExternalImageFormatInfo ei{};
		ei.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
		ei.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
		VkPhysicalDeviceImageFormatInfo2 ifi{};
		ifi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
		ifi.pNext = &ei;
		ifi.format = cand[i];
		ifi.type = VK_IMAGE_TYPE_2D;
		ifi.tiling = VK_IMAGE_TILING_OPTIMAL;
		ifi.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		VkExternalImageFormatProperties ep{};
		ep.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
		VkImageFormatProperties2 fp{};
		fp.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
		fp.pNext = &ep;
		const VkResult qr = c.fns.vkGetPhysicalDeviceImageFormatProperties2(c.phys, &ifi, &fp);
		if (qr != VK_SUCCESS)
			qres += "=" + pocbCode(qr);
		else
		{
			const uint32_t f = ep.externalMemoryProperties.externalMemoryFeatures;
			qres += std::string(" features=0x") + uhex64(f).substr(12) +
			        (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ? " 可导入" : " 不可导入");
			if (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)
				needTry[i] = true;
		}
	}
	logLine("SSR侦察: [2d-4]   KMT探测#3 VK 支持度查询 handleType=D3D11_TEXTURE_KMT_BIT: " + qres);

	// --- ② 实测: 建图 → 导入老式 handle → 绑定 (全成才算数) ---
	VkPhysicalDeviceMemoryProperties mp{};
	c.fns.vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
	std::string ires;
	bool ok = false;
	for (int i = 0; i < 2 && !ok; ++i)
	{
		if (!needTry[i])
			continue;
		const char* tag = (i == 0) ? "D24/S8" : "D32S8";
		VkExternalMemoryImageCreateInfo pem{};
		pem.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		pem.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
		VkImageCreateInfo ici{};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.pNext = &pem;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = cand[i];
		ici.extent = VkExtent3D{sd.Width, sd.Height, 1};
		ici.mipLevels = sd.MipLevels ? sd.MipLevels : 1u;
		ici.arrayLayers = sd.ArraySize ? sd.ArraySize : 1u;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkImage img = VK_NULL_HANDLE;
		const VkResult cr = c.fns.vkCreateImage(c.vdev, &ici, nullptr, &img);
		std::string sub;
		if (cr != VK_SUCCESS || img == VK_NULL_HANDLE)
			sub = "createImage=" + pocbCode(cr);
		else
		{
			VkMemoryRequirements req{};
			c.fns.vkGetImageMemoryRequirements(c.vdev, img, &req);
			const int tDev = pocbMemType(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			const int tAny = pocbMemType(mp, req.memoryTypeBits, 0);
			VkDeviceMemory mem = VK_NULL_HANDLE;
			VkResult ar = VK_ERROR_OUT_OF_HOST_MEMORY;
			int tries = 0;
			for (int ti = 0; ti < 2 && ar != VK_SUCCESS; ++ti)
			{
				const int t = (ti == 0) ? tDev : tAny;
				if (t < 0 || (ti == 1 && tAny == tDev) || !req.size)
					continue;
				++tries;
				VkImportMemoryWin32HandleInfoKHR imp{};
				imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
				imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
				imp.handle = h;
				VkMemoryAllocateInfo mai{};
				mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
				mai.pNext = &imp;
				mai.allocationSize = req.size;
				mai.memoryTypeIndex = static_cast<uint32_t>(t);
				ar = c.fns.vkAllocateMemory(c.vdev, &mai, nullptr, &mem);
			}
			sub = std::string("alloc=") +
			      (tries ? pocbCode(ar) : std::string("跳过(无可用内存类型)")) +
			      " VK报size=" + std::to_string((long long)req.size);
			if (ar == VK_SUCCESS)
			{
				const VkResult br = c.fns.vkBindImageMemory(c.vdev, img, mem, 0);
				sub += " bind=" + pocbCode(br);
				ok = (br == VK_SUCCESS);
			}
			if (mem)
				c.fns.vkFreeMemory(c.vdev, mem, nullptr);
			c.fns.vkDestroyImage(c.vdev, img, nullptr);
		}
		ires += (ires.empty() ? "" : "; ");
		ires += std::string(tag) + " " + sub;
	}
	logLine("SSR侦察: [2d-4]   KMT探测#4 VK 导入实测(建图→导入→绑定, 建完立刻销毁) → " +
	        (ires.empty() ? std::string("无候选格式可试") : ires));
	return ok;
}

// ===========================================================================
// ---- 路线1′ 正式落地 (v0.18.5, docs/05 D2a-4 定案): 深度 KMT 导入分支 ----
// ---------------------------------------------------------------------------
// 2c 的色镜像走 NT handle + VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT; 深度 (D24 家族)
// 栽在 SHARED_NTHANDLE 这一轴 (R2 真凶 = **组合** 而非格式), 而"单独 SHARED"建得出老式 handle
// (2d-3 第一格), VK 用 KMT handleType 直入也 bind=0 (2d-4 #4)。于是深度**另开一条导入分支**:
// 老式 GetSharedHandle → D3D11_TEXTURE_KMT_BIT, 与色那条分支互不相干 (另开导入分支的字面落地)。
//
// 同步原语评估 (D2a-4 留的问题② —— "NT 路径的 fence 在 KMT 资源上是否等效"):
//   · EVENT 闸 (ssrInQueue End / ssrInGateWait 等) 等的是 **D3D11 那条 CopyResource**, 与
//     handleType 无关 ⇒ KMT 分支直接继承 v0.18.4 的闸, 不需要新栅栏;
//   · 老式 handle 只在**建图时导入一次**, 之后每帧都是"D3D11 写同一块物理页 → 闸等到写完 →
//     VK 读", 不涉及按帧重新导入 (KMT 句柄归 DXGI/D3D11 所有, 不 CloseHandle);
//   · VK 侧仍用 c.fence 提交+等待 (与 2c-β/2d 出向共用, 顺序提交顺序等)。
// 降级: 任一步失败只关自己 (g_ssrKmtState=2 + 一行日志), 色通路 / 2c / 2d 出向全不受影响。
static int             g_ssrKmtState = 0;   // 0=未建 1=就绪 2=关自己 (只打一次失败日志)
static VkDevice        g_ssrKmtDev = VK_NULL_HANDLE; // 这套句柄属于哪个 VkDevice (换设备须丢弃)
static VkImage         g_ssrKmtImg = VK_NULL_HANDLE;
static VkDeviceMemory  g_ssrKmtMem = VK_NULL_HANDLE;
static VkImageView     g_ssrKmtView = VK_NULL_HANDLE;
static VkBuffer        g_ssrKmtBuf = VK_NULL_HANDLE;
static VkDeviceMemory  g_ssrKmtBufMem = VK_NULL_HANDLE;
static void*           g_ssrKmtBufPtr = nullptr;
static VkCommandBuffer g_ssrKmtCmd = VK_NULL_HANDLE;
static unsigned        g_ssrKmtW = 0, g_ssrKmtH = 0; // 建图时的深度镜像尺寸 (读回行距用)
static bool            g_ssrKmtCanRead = true;       // TRANSFER_SRC/读回 buffer 建得出吗
static long            g_ssrKmtN = 0;                // 深度跨API读回次数
static long            g_ssrKmtOk = 0;               // 至少一档候选一致的次数
static long            g_ssrKmtBad = 0;              // 三档候选全不一致的次数

// 弃置一套深度分支资源。destroy=true 且设备没换 ⇒ 正常销毁; 设备换了 ⇒ **只丢句柄**
// (旧 VkDevice 已作废, 拿它 destroy 是 UB)。老式 handle 归 D3D11 所有, 这里从不 CloseHandle。
static void ssrKmtReset(PocbCtx& c, bool destroy)
{
	if (destroy && g_ssrKmtDev && g_ssrKmtDev == c.vdev && c.vdev)
	{
		c.fns.vkDeviceWaitIdle(c.vdev); // 先确认 GPU 不再读这块内存
		if (g_ssrKmtView)
			c.fns.vkDestroyImageView(c.vdev, g_ssrKmtView, nullptr);
		if (g_ssrKmtImg)
			c.fns.vkDestroyImage(c.vdev, g_ssrKmtImg, nullptr);
		if (g_ssrKmtMem)
			c.fns.vkFreeMemory(c.vdev, g_ssrKmtMem, nullptr);
		if (g_ssrKmtBuf)
			c.fns.vkDestroyBuffer(c.vdev, g_ssrKmtBuf, nullptr);
		if (g_ssrKmtBufMem)
			c.fns.vkFreeMemory(c.vdev, g_ssrKmtBufMem, nullptr);
	}
	g_ssrKmtView = VK_NULL_HANDLE;
	g_ssrKmtImg = VK_NULL_HANDLE;
	g_ssrKmtMem = VK_NULL_HANDLE;
	g_ssrKmtBuf = VK_NULL_HANDLE;
	g_ssrKmtBufMem = VK_NULL_HANDLE;
	g_ssrKmtBufPtr = nullptr;
	g_ssrKmtCmd = VK_NULL_HANDLE; // 命令 buffer 属于旧 pool ⇒ 换设备必须重分配 (无 vkFreeCommandBuffers)
	g_ssrKmtCanRead = true;
	g_ssrKmtState = 0;
	g_ssrKmtDev = VK_NULL_HANDLE;
}

// 建图 + KMT 导入 + 绑定 + 视图 + 读回 buffer, **持久持有** (与 ssrKmtProbeVk 的"建完立刻销毁"
// 相对)。只在 D3D11 侧的读回节奏上被调到 (前3次 + 每600次), 失败只关自己。
static bool ssrKmtVkBuild(PocbCtx& c)
{
	if (g_ssrKmtState == 2)
		return false;
	if (c.state.load() != 2 || !c.vdev || !c.queue || !c.pool || !c.fence)
		return false; // PoC-B 还没就绪 → 下一帧再试
	if (!g_ssrSharedOn.load(std::memory_order_relaxed) || !g_ssrInDepthKmt ||
	    !g_ssrInHD || !g_ssrInTexD)
		return false;
	auto fail = [&](const std::string& why) {
		logLine("SSR侦察: [2d-5] 深度KMT 导入失败: " + why +
		        " → 深度不开 (色通路/2c/2d 出向不受影响; SSR 兜底 = 颜色 + 边缘 fallback)");
		ssrKmtReset(c, true); // 已建成的那一半正常销毁 (设备还在), 再关自己
		g_ssrKmtState = 2;
		return false;
	};
	// 本函数只在 state==0 进来 ⇒ 这些句柄此时必然是空的; 先置一遍是防 vkCreateImage/AllocateMemory
	// **失败时不写输出参数**, 免得把垃圾值带进后面的失败清理 (ssrKmtReset 会拿它去 destroy)。
	g_ssrKmtImg = VK_NULL_HANDLE;
	g_ssrKmtMem = VK_NULL_HANDLE;
	g_ssrKmtView = VK_NULL_HANDLE;
	g_ssrKmtBuf = VK_NULL_HANDLE;
	g_ssrKmtBufMem = VK_NULL_HANDLE;
	g_ssrKmtBufPtr = nullptr;
	D3D11_TEXTURE2D_DESC md{};
	g_ssrInTexD->GetDesc(&md);
	// VK 侧映射表本版只认 D24 家族 —— 真机源格式 = Format=44 (R24G8_TYPELESS), 与 2d-4 探测同一格;
	// 四个符号与 main.cpp 的 ssrIsD24 同表, 都是已编译过的既有 token。
	const bool d24 = (md.Format == DXGI_FORMAT_R24G8_TYPELESS ||
	                  md.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
	                  md.Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
	                  md.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT);
	if (!d24)
		return fail("深度镜像格式 " + ssrFmtName(md.Format) + " 不在 D24 家族映射表");
	if (md.ArraySize != 1 || md.MipLevels != 1)
		return fail("深度镜像 arr" + std::to_string(md.ArraySize) + " mips" +
		            std::to_string(md.MipLevels) + " (本分支只做单层单 mip)");
	if (md.SampleDesc.Count != 1)
		return fail("深度镜像多重采样 msaa" + std::to_string(md.SampleDesc.Count) +
		            " (本分支没做 msaa 映射)");
	g_ssrKmtW = md.Width;
	g_ssrKmtH = md.Height;
	g_ssrKmtDev = c.vdev;

	// --- 1) 外部内存 VkImage: handleType = KMT (与色的 D3D11_TEXTURE_BIT 分支分开) ---
	VkExternalMemoryImageCreateInfo pem{};
	pem.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	pem.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
	VkImageCreateInfo ici{};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.pNext = &pem;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_D24_UNORM_S8_UINT;
	ici.extent = VkExtent3D{md.Width, md.Height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkResult vr = c.fns.vkCreateImage(c.vdev, &ici, nullptr, &g_ssrKmtImg);
	std::string usg = "TRANS_SRC|SAMPLED";
	if (vr != VK_SUCCESS)
	{
		// 兜底档 = 2d-4 探测实测过的 TRANSFER_DST|SAMPLED (支持度查询就是用它查的)。
		// 落到这档说明 TRANSFER_SRC 不被支持 ⇒ 导入照样成立, 但读回会失能 (下面关掉 canRead)。
		ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		usg = "TRANS_DST|SAMPLED";
		g_ssrKmtCanRead = false;
		vr = c.fns.vkCreateImage(c.vdev, &ici, nullptr, &g_ssrKmtImg);
	}
	if (vr != VK_SUCCESS)
		return fail("vkCreateImage(KMT, " + usg + ") = " + pocbCode(vr));

	// --- 2) 导入老式 handle: 分配即导入 (链进 VkMemoryAllocateInfo) ---
	VkMemoryRequirements req{};
	c.fns.vkGetImageMemoryRequirements(c.vdev, g_ssrKmtImg, &req);
	VkPhysicalDeviceMemoryProperties mp{};
	c.fns.vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
	const int tDev = pocbMemType(mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	const int tAny = pocbMemType(mp, req.memoryTypeBits, 0);
	if (!req.size || (tDev < 0 && tAny < 0))
		return fail("KMT 导入没有可用内存类型 (memoryTypeBits=" +
		            std::to_string(req.memoryTypeBits) + ")");
	VkResult ar = VK_ERROR_OUT_OF_HOST_MEMORY;
	int tUsed = -1;
	bool usedDed = false;
	// 内存类型 (DEVICE_LOCAL 优先) × dedicated 两档。2d-4 支持度查询回 features=0x0005 =
	// DEDICATED_ONLY|IMPORTABLE ⇒ 按规范必须走 dedicated 分配 (探针当年没链也过了, 那是驱动
	// 宽容不是合规); 万一被拒, 再退回不带 dedicated 的老路, 两档都把码记下来便于归因。
	for (int ti = 0; ti < 2 && ar != VK_SUCCESS; ++ti)
	{
		const int t = (ti == 0) ? tDev : tAny;
		if (t < 0 || (ti == 1 && tAny == tDev))
			continue;
		for (int di = 0; di < 2 && ar != VK_SUCCESS; ++di)
		{
			VkImportMemoryWin32HandleInfoKHR imp{};
			imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
			imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
			imp.handle = g_ssrInHD;
			VkMemoryDedicatedAllocateInfo dai{};
			dai.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
			dai.pNext = &imp;
			dai.image = g_ssrKmtImg;
			VkMemoryAllocateInfo mai{};
			mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			mai.pNext = (di == 0) ? static_cast<const void*>(&dai) : static_cast<const void*>(&imp);
			mai.allocationSize = req.size;
			mai.memoryTypeIndex = static_cast<uint32_t>(t);
			ar = c.fns.vkAllocateMemory(c.vdev, &mai, nullptr, &g_ssrKmtMem);
			if (ar == VK_SUCCESS)
			{
				tUsed = t;
				usedDed = (di == 0);
			}
		}
	}
	if (ar != VK_SUCCESS)
		return fail("vkAllocateMemory(KMT 导入) = " + pocbCode(ar) +
		            " VK报size=" + std::to_string((long long)req.size));
	const VkResult br = c.fns.vkBindImageMemory(c.vdev, g_ssrKmtImg, g_ssrKmtMem, 0);
	if (br != VK_SUCCESS)
		return fail("vkBindImageMemory(KMT) = " + pocbCode(br));

	// --- 3) 深度视图 (DEPTH|STENCIL): 只为后续 SSR v1 shader 采样准备, 读回不依赖它 ---
	VkImageViewCreateInfo vci{};
	vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image = g_ssrKmtImg;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_D24_UNORM_S8_UINT;
	vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
	const VkResult vvr = c.fns.vkCreateImageView(c.vdev, &vci, nullptr, &g_ssrKmtView);
	if (vvr != VK_SUCCESS)
	{
		g_ssrKmtView = VK_NULL_HANDLE;
		logLine("SSR侦察: [2d-5] 深度KMT ImageView 失败 " + pocbCode(vvr) +
		        " (读回不受影响; SSR v1 要采样前必须修)");
	}

	// --- 4) 读回 buffer: 紧密 width*height*4 (D24 的 depth aspect ≤ 4B/px), host visible|coherent
	//      持久映射 —— 与 2c-β 同法; 建不出只是失去读回比对, 导入本身照常成立。 ---
	if (g_ssrKmtCanRead)
	{
		const VkDeviceSize sz = (VkDeviceSize)md.Width * (VkDeviceSize)md.Height * 4ULL;
		VkBufferCreateInfo bci{};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.size = sz;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		vr = c.fns.vkCreateBuffer(c.vdev, &bci, nullptr, &g_ssrKmtBuf);
		if (vr != VK_SUCCESS)
			g_ssrKmtCanRead = false;
		else
		{
			VkMemoryRequirements breq{};
			c.fns.vkGetBufferMemoryRequirements(c.vdev, g_ssrKmtBuf, &breq);
			const int bt = pocbMemType(mp, breq.memoryTypeBits,
			                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			VkMemoryAllocateInfo bmai{};
			bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			bmai.allocationSize = breq.size;
			bmai.memoryTypeIndex = (bt >= 0) ? static_cast<uint32_t>(bt) : 0;
			if (bt < 0 || c.fns.vkAllocateMemory(c.vdev, &bmai, nullptr, &g_ssrKmtBufMem) != VK_SUCCESS ||
			    c.fns.vkBindBufferMemory(c.vdev, g_ssrKmtBuf, g_ssrKmtBufMem, 0) != VK_SUCCESS ||
			    c.fns.vkMapMemory(c.vdev, g_ssrKmtBufMem, 0, VK_WHOLE_SIZE, 0, &g_ssrKmtBufPtr) != VK_SUCCESS ||
			    !g_ssrKmtBufPtr)
				g_ssrKmtCanRead = false;
		}
		if (!g_ssrKmtCanRead)
			logLine("SSR侦察: [2d-5] 深度KMT 读回 buffer 建不出 → 导入仍就绪, 但本分支无跨API比对");
	}

	// --- 5) 读回命令 (ONE_TIME 录, 提交完自动回 initial state ⇒ 之后可直接重录) ---
	if (g_ssrKmtCanRead && !g_ssrKmtCmd)
	{
		VkCommandBufferAllocateInfo cbai{};
		cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cbai.commandPool = c.pool;
		cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbai.commandBufferCount = 1;
		if (c.fns.vkAllocateCommandBuffers(c.vdev, &cbai, &g_ssrKmtCmd) != VK_SUCCESS)
		{
			g_ssrKmtCmd = VK_NULL_HANDLE;
			g_ssrKmtCanRead = false;
			logLine("SSR侦察: [2d-5] 深度KMT vkAllocateCommandBuffers 失败 → 无跨API比对");
		}
	}

	// --- 6) 布局初转 (UNDEFINED→GENERAL) 单独一条, 建好时立即提交并等完。按规范这次转换
	//      **可能丢掉** D3D11 刚拷进来的内容 ⇒ 调用方"建图帧只建不比", 下一机会才出对比值。 ---
	if (g_ssrKmtCanRead && g_ssrKmtCmd)
	{
		VkCommandBufferBeginInfo cbb{};
		cbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		cbb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (c.fns.vkBeginCommandBuffer(g_ssrKmtCmd, &cbb) != VK_SUCCESS)
			return fail("vkBeginCommandBuffer(深度初转)");
		VkImageMemoryBarrier imb{};
		imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imb.srcAccessMask = 0;
		imb.dstAccessMask = 0;
		imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imb.image = g_ssrKmtImg;
		imb.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
		c.fns.vkCmdPipelineBarrier(g_ssrKmtCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
		                           1, &imb);
		if (c.fns.vkEndCommandBuffer(g_ssrKmtCmd) != VK_SUCCESS)
			return fail("vkEndCommandBuffer(深度初转)");
		c.fns.vkResetFences(c.vdev, 1, &c.fence);
		VkSubmitInfo si{};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &g_ssrKmtCmd;
		vr = c.fns.vkQueueSubmit(c.queue, 1, &si, c.fence);
		if (vr == VK_SUCCESS)
			vr = c.fns.vkWaitForFences(c.vdev, 1, &c.fence, VK_TRUE, 5000000000ULL);
		if (vr != VK_SUCCESS)
			return fail("深度初转 vkQueueSubmit/vkWaitForFences = " + pocbCode(vr));
	}

	g_ssrKmtState = 1;
	logLine("SSR侦察: [2d-5] 深度KMT 导入OK: 老式handle → D3D11_TEXTURE_KMT_BIT 直入 " +
	        std::to_string(md.Width) + "x" + std::to_string(md.Height) + " D24/S8 usage=" + usg +
	        " alloc=" + std::string(usedDed ? "dedicated" : "plain") +
	        " 内存类型#" + std::to_string(tUsed) +
	        " view=" + std::string(g_ssrKmtView ? "OK" : "失败") +
	        " 读回=" + std::string(g_ssrKmtCanRead ? "OK" : "不可用") +
	        " — 路线1′ 分支就绪 (与色的 D3D11_TEXTURE_BIT 分支互不相干)");
	return true;
}

// 到点读回比对 (节奏完全跟随 D3D11 侧的深度读回: 前3次 + 每600次, 用一次性 flag 消费)。
// 判据故意做三档候选: VK 拷 depth aspect 时每像素字节数 (4 或 3) 与 stencil 字节是否保留
// 都是**规范内可变**的, 单一口径假设有假 FAIL 风险 ⇒ (4B全量) / (4B跳stencil=低24位) /
// (3B紧密排布) 三档都算, 命中任一即证明"D3D11 写的这帧字节 VK 能原样读到" = 同步成立。
void ssrKmtVkFrame(PocbCtx& c) // vkrenderer.h 有声明 (非 static), 定义必须同链接性
{
	const bool want = g_ssrInChkDValid;
	const unsigned long long d0 = g_ssrInChkD;
	const unsigned long long d1 = g_ssrInChkD3;
	g_ssrInChkDValid = false; // 一次性消费, 免得下一帧拿旧值去比
	if (!want || !g_ssrInDepthKmt || !g_ssrSharedOn.load(std::memory_order_relaxed))
		return;
	if (g_ssrKmtState == 2)
		return;
	if (c.state.load() != 2 || !c.vdev || !c.queue || !c.pool || !c.fence)
		return;
	// 设备换了 ⇒ 旧句柄作废 (不能拿旧 VkDevice destroy), 只丢不毁, 之后重建
	if (g_ssrKmtState && g_ssrKmtDev != c.vdev)
	{
		logLine("SSR侦察: [2d-5] 深度KMT 检测到 VkDevice 变更 → 丢弃旧句柄重建");
		ssrKmtReset(c, false);
	}
	// 分辨率变了 ⇒ 深度镜像尺寸跟着变, 正常销毁重建 (设备还在, 可以安全 destroy)
	if (g_ssrKmtState == 1 && g_ssrInTexD)
	{
		D3D11_TEXTURE2D_DESC md{};
		g_ssrInTexD->GetDesc(&md);
		if (md.Width != g_ssrKmtW || md.Height != g_ssrKmtH)
		{
			logLine("SSR侦察: [2d-5] 深度KMT 尺寸变更 " + std::to_string(g_ssrKmtW) + "x" +
			        std::to_string(g_ssrKmtH) + " → " + std::to_string(md.Width) + "x" +
			        std::to_string(md.Height) + " → 销毁重建");
			ssrKmtReset(c, true);
		}
	}
	if (g_ssrKmtImg == VK_NULL_HANDLE)
	{
		ssrKmtVkBuild(c); // 建图帧只建不比 (初转按规范可能丢内容), 下一机会出对比值
		return;
	}
	if (!g_ssrKmtCanRead || !g_ssrKmtCmd || !g_ssrKmtBufPtr || !g_ssrKmtW || !g_ssrKmtH)
		return;

	// --- 重录读回命令 ---
	VkCommandBufferBeginInfo cbb{};
	cbb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	cbb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (c.fns.vkBeginCommandBuffer(g_ssrKmtCmd, &cbb) != VK_SUCCESS)
	{
		logLine("SSR侦察: [2d-5] 深度KMT读回 vkBeginCommandBuffer 失败 → 本分支停用");
		g_ssrKmtState = 2;
		return;
	}
	auto barrier = [&](VkPipelineStageFlags ss, VkAccessFlags sa, VkPipelineStageFlags ds,
	                   VkAccessFlags da, VkImageLayout ol, VkImageLayout nl) {
		VkImageMemoryBarrier imb{};
		imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imb.srcAccessMask = sa;
		imb.dstAccessMask = da;
		imb.oldLayout = ol;
		imb.newLayout = nl;
		imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imb.image = g_ssrKmtImg;
		// 组合深度/模板格式的布局迁移必须把 DEPTH 与 STENCIL 两个位一起带上 (规范要求)
		imb.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
		c.fns.vkCmdPipelineBarrier(g_ssrKmtCmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &imb);
	};
	// 与 2c-β 同款三段: 跨 API 的这次写不归 VK 记账 ⇒ srcAccess 用 MEMORY_WRITE 把 VK 侧
	// 缓存失效掉; 本帧特征B 那条 CopyResource 已由 EVENT 闸 (ssrInGateWait 在本函数之前) 等完。
	barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy bic{};
	bic.bufferRowLength = 0; // 0 = 紧密排列 ⇒ 行距 = width × 每像素字节数
	bic.bufferImageHeight = 0;
	bic.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}; // 只拷 depth aspect (stencil 未定义)
	bic.imageOffset = VkOffset3D{0, 0, 0};
	bic.imageExtent = VkExtent3D{g_ssrKmtW, g_ssrKmtH, 1};
	c.fns.vkCmdCopyImageToBuffer(g_ssrKmtCmd, g_ssrKmtImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                             g_ssrKmtBuf, 1, &bic);
	barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT,
	        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	if (c.fns.vkEndCommandBuffer(g_ssrKmtCmd) != VK_SUCCESS)
	{
		logLine("SSR侦察: [2d-5] 深度KMT读回 vkEndCommandBuffer 失败 → 本分支停用");
		g_ssrKmtState = 2;
		return;
	}
	c.fns.vkResetFences(c.vdev, 1, &c.fence);
	VkSubmitInfo si{};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &g_ssrKmtCmd;
	const auto t0 = std::chrono::steady_clock::now();
	VkResult vr = c.fns.vkQueueSubmit(c.queue, 1, &si, c.fence);
	if (vr == VK_SUCCESS)
		vr = c.fns.vkWaitForFences(c.vdev, 1, &c.fence, VK_TRUE, 5000000000ULL);
	const double ms =
	    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (vr != VK_SUCCESS)
	{
		logLine("SSR侦察: [2d-5] 深度KMT读回 vkQueueSubmit/vkWaitForFences = " + pocbCode(vr) +
		        " → 本分支停用 (不连坐色通路)");
		g_ssrKmtState = 2;
		return;
	}

	// --- 三候选比对 (D3D11 侧同一帧的两个口径 vs VK 侧三种排布) ---
	const size_t p4 = (size_t)g_ssrKmtW * 4; // 4 字节/像素 (含 stencil 位)
	const size_t p3 = (size_t)g_ssrKmtW * 3; // 3 字节/像素 (VK 若按 24 位紧密排布)
	long nz0 = 0, nz1 = 0, nz2 = 0;
	const unsigned long long v0 =
	    ssrFnvSampleAdv(g_ssrKmtBufPtr, p4, g_ssrKmtW, g_ssrKmtH, 4, 4, &nz0);
	const unsigned long long v1 =
	    ssrFnvSampleAdv(g_ssrKmtBufPtr, p4, g_ssrKmtW, g_ssrKmtH, 4, 3, &nz1);
	const unsigned long long v2 =
	    ssrFnvSampleAdv(g_ssrKmtBufPtr, p3, g_ssrKmtW, g_ssrKmtH, 3, 3, &nz2);
	const char* verdict;
	const bool okVerdict = (v0 && v0 == d0) || (v1 && v1 == d1) || (v2 && v2 == d1);
	if (v0 && v0 == d0)
		verdict = "一致✓(全量4B)";
	else if (v1 && v1 == d1)
		verdict = "低24位一致✓(跳stencil)";
	else if (v2 && v2 == d1)
		verdict = "紧密3字节一致✓(VK按24位排布)";
	else
		verdict = "不一致✗(三档候选全不符)";
	++g_ssrKmtN;
	if (okVerdict)
		++g_ssrKmtOk;
	else
		++g_ssrKmtBad;
	char b[704];
	std::snprintf(b, sizeof(b),
	              "SSR侦察: [2d-5] 深度KMT读回#%ld 判定=%s | D3D全量=0x%llx 低3=0x%llx | "
	              "VK 4B=0x%llx(非零%ld) 跳stencil=0x%llx(非零%ld) 紧密3B=0x%llx(非零%ld) | "
	              "提交+等fence=%.2fms 帧=%llu",
	              g_ssrKmtN, verdict, d0, d1, v0, nz0, v1, nz1, v2, nz2, ms,
	              static_cast<unsigned long long>(
	                  g_presentCount.load(std::memory_order_relaxed) + 1));
	logLine(b);
}

// ---- O-1 正式修法 (v0.18.4, docs/02 §14.20 / docs/05 O-1): 2c 入向 EVENT 闸 ----
// 入向 CopyResource 是异步排队的: 只有阻塞读回帧 (前3次+每600次的 Map) 才等得到它, 非读回帧
// 走到 Present 时可能还没进 GPU ⇒ VK 读到上一帧那份入向, 12e 恒差一帧 (v0.18.3 实测 8 行里
// 3 行不一致, 且全是"出向 = 2 帧前入向"; 每帧都阻塞读回的头 3 行全一致 = 证据链)。
// main.cpp 侧在拷贝后 End 了 EVENT 查询, 这里在提交任何读入向镜像的 VK 命令之前把它等掉 ——
// 与 pocbInject 里 PoC-B 的 copyQ 同款: GetData 带 DONOTFLUSH (不替我们 Flush)、Sleep(0) 自旋、
// 2000ms 超时放行 (绝不卡死帧), 超时也把 live 消费掉免得下一帧等旧结果。
void ssrInGateWait(PocbCtx& c) // vkrenderer.h 有声明 (非 static), 定义必须同链接性
{
	if (!g_ssrInQLive || !g_ssrInQ || !c.ctx)
		return;
	g_ssrInQLive = false; // 一次性消费: 本帧的闸用掉了 (超时也放行, 免得下一帧等旧结果)
	if (c.state.load() != 2 || !c.vdev)
		return; // VK 没起来/已降级 → 没人读入向镜像, 不必白等 (但消费掉, 下一帧照常重新 End)
	const auto g0 = std::chrono::steady_clock::now();
	while (c.ctx->GetData(g_ssrInQ, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE)
	{
		if (std::chrono::steady_clock::now() - g0 > std::chrono::milliseconds(2000))
		{
			logLine("SSR侦察: [2c] 入向EVENT闸 等待超 2000ms → 放行本帧 (12e 可能不一致)");
			break;
		}
		Sleep(0);
	}
	const double ms = std::chrono::duration<double, std::milli>(
	    std::chrono::steady_clock::now() - g0).count();
	g_ssrInGateMs += ms;
	const long k = ++g_ssrInGateN;
	if (k <= 8 || (k % 128) == 0)
		logLine("SSR侦察: [2c] 入向EVENT闸#" + std::to_string(k) + " 等待=" +
		        std::to_string(ms).substr(0, 5) + "ms 累计均值=" +
		        std::to_string(g_ssrInGateMs / static_cast<double>(k)).substr(0, 5) +
		        "ms 帧=" + std::to_string(g_presentCount.load(std::memory_order_relaxed)));
}

// 每帧: 提交一次 Vulkan 命令 → fence 等待 → 读回像素 → 拷进 backbuffer
void pocbInject(PocbCtx& c, IDXGISwapChain* sc)
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

	// --- 共享路径跨 API 竞态闸 (v0.10.0): 上一帧 D3D11 拷贝没跑完之前不提交本帧 VK 写 ---
	// 读回路径无此问题 (数据到 CPU 手里才交给 D3D11); 共享路径两边都是 GPU 工作且分属
	// 两个 API, 无共享同步原语 ⇒ 用 EVENT 查询在下一帧提交前把上一帧的拷贝等掉。
	if (c.useShared && c.copyQ && c.copyQLive)
	{
		const auto g0 = std::chrono::steady_clock::now();
		while (c.ctx->GetData(c.copyQ, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE)
		{
			if (std::chrono::steady_clock::now() - g0 > std::chrono::milliseconds(2000))
			{
				logLine("PoC-B: 等上一帧共享拷贝超 2000ms → 放行本帧 (可能撕裂一帧)");
				break;
			}
			Sleep(0);
		}
		c.copyQLive = false;
		c.accGateMs += std::chrono::duration<double, std::milli>(
		    std::chrono::steady_clock::now() - g0).count();
	}

	c.fns.vkResetFences(c.vdev, 1, &c.fence);
	VkSubmitInfo si{};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = c.useShared ? &c.cmdS : &c.cmd;
	const auto f0 = std::chrono::steady_clock::now();
	VkResult vr = c.fns.vkQueueSubmit(c.queue, 1, &si, c.fence);
	if (vr == VK_SUCCESS)
		vr = c.fns.vkWaitForFences(c.vdev, 1, &c.fence, VK_TRUE, UINT64_MAX);
	c.accFenceMs += std::chrono::duration<double, std::milli>(
	    std::chrono::steady_clock::now() - f0).count();
	if (vr != VK_SUCCESS)
	{
		bb->Release();
		pocbFail("vkQueueSubmit / vkWaitForFences", vr);
		return;
	}

	if (c.useShared)
	{
		// 共享路径: stex (被 VK 灌满) → backbuffer, 纯 GPU 拷贝; End+Flush 保证 EVENT
		// 查询连同拷贝真的进了驱动队列 (GetData 带 DONOTFLUSH, 不替我们提交)。
		c.ctx->CopySubresourceRegion(bb, 0, POCB_X, POCB_Y, 0, c.stex, 0, nullptr);
		c.ctx->End(c.copyQ);
		c.ctx->Flush();
		c.copyQLive = true;
	}
	else
	{
		c.ctx->UpdateSubresource(c.tex, 0, nullptr, c.mapped, POCB_W * POCB_BPP, 0);
		D3D11_BOX box{0, 0, 0, static_cast<UINT>(POCB_W), static_cast<UINT>(POCB_H), 1};
		c.ctx->CopySubresourceRegion(bb, 0, POCB_X, POCB_Y, 0, c.tex, 0, &box);
	}
	bb->Release();

	const auto t1 = std::chrono::steady_clock::now();
	const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	c.frames++;
	c.accMs += ms;
	if (c.frames == 1)
	{
		char b[256];
		std::snprintf(b, sizeof(b),
		    "PoC-B 第 1 帧注入: 渲染+%s %.2f ms (fence %.2f / 闸 %.2f) → CopySubresourceRegion(%d,%d %dx%d) 已提交",
		    c.useShared ? "共享拷贝" : "读回+上传", ms, c.accFenceMs, c.accGateMs, POCB_X, POCB_Y,
		    POCB_W, POCB_H);
		logLine(b);
	}
	else if (c.frames % 600 == 0)
	{
		char b[256];
		std::snprintf(b, sizeof(b),
		    "PoC-B 注入 %llu 帧, 累计均值 %.2f ms/帧 (本帧 %.2f, fence %.2f, 闸 %.2f, %s)",
		    static_cast<unsigned long long>(c.frames), c.accMs / static_cast<double>(c.frames), ms,
		    c.accFenceMs / static_cast<double>(c.frames),
		    c.accGateMs / static_cast<double>(c.frames),
		    c.useShared ? "共享纹理" : "读回");
		logLine(b);
	}
	// O-1 EVENT 闸 (v0.18.4): 提交任何读入向镜像的 VK 命令之前, 先把本帧特征B 那条
	// CopyResource 等掉 —— 位置在下面两条 SSR 提交之前, 一次等待同时覆盖 2c-β 与 2d 出向。
	ssrInGateWait(c);
	// Step 2c-β 交叉校验 —— **刻意放在 PoC-B 统计之后**: 它只在 D3D11 读回过的帧干活
	// (前3次+每600次), 若算进 c.accMs 就会污染 PoC-B 那条 0.9 ms/帧 的验收基线;
	// 自己单独计时打进 [2c-β] 行。
	ssrInVkFrame(c);
	// 2d-1 出向回写 (v0.18.0) —— **紧跟在读回之后**: 两者共用 c.fence, 顺序提交+顺序等待;
	// 单独计时, 不算进 c.accMs, 免得污染 PoC-B 那条 ≈0.9ms/帧 的验收基线。
	ssrOutVkFrame(c);
	// 路线1′ 深度 KMT 分支 (v0.18.5) —— 排在最后: 前面 ssrInGateWait 已把本帧入向 CopyResource
	// 等完 (EVENT 闸与 handleType 无关, KMT 分支直接继承), 又与 2c-β/2d 出向共用 c.fence 顺序提交;
	// 它只在 D3D11 侧做过深度读回的那几帧干活 (前3次+每600次), 单独计时, 不进 c.accMs。
	ssrKmtVkFrame(c);
}
