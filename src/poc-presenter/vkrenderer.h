#pragma once
// ===========================================================================
// vkrenderer.h —— 自写 Vulkan 渲染器模块 (方向 A / 第一步 B, docs/00 §1.1.1)
// ---------------------------------------------------------------------------
// v0.17.0 (2026-10-05) 从 main.cpp 抽出 —— **纯代码搬迁, 行为不变**:
//   · PoC-B 全套: 实例/设备/队列/管线/fence/共享纹理导入 (pocbInit / pocbInject)
//   · Step 2c-β: 场景色导入 + 读回交叉校验 (ssrInVkBuild / ssrVkFree / ssrInVkFrame)
// 2d 起新增的出向回写、descriptor/sampler、深度改道 (VK 自建 VK_FORMAT_D24_SFLOAT 图)
// **全部写进 vkrenderer.cpp**, 不再往 main.cpp 里堆。
// main.cpp 只保留: Present/vtable hook 面 + D3D11 interop (侦察/共享纹理/哈希) + 日志/ini。
//
// 依赖方向是单向的: renderer → hook 面, 通过本文件末尾 `namespace pocmain` 的声明。
// main.cpp 原来是匿名 namespace, 为了让 renderer 能引用, v0.17.0 起改具名 `pocmain`
// (main.cpp 末尾加 `using namespace pocmain;`, 之后 DllMain/SKSEPlugin_Load 照常可见)。
// 这一段依赖是过渡态 —— 2d 收敛成"入参结构体"后整段删掉。
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <algorithm>
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
#include <vulkan/vulkan.h>

// PoC-B 的 512x512 三角 + 函数表 (原 main.cpp PoC-B 段, 现随类型一起公开给 hook 面)
#define POCB_W 512
#define POCB_H 512
#define POCB_X 16
#define POCB_Y 16
#define POCB_BPP 4

// Vulkan 函数表分三层加载: 全局 (vkCreateInstance) → 实例 → 设备。
// 本机与 CI 都没有 Vulkan SDK ⇒ 不链 vulkan-1.lib, 全部 GetProcAddress 自装。
// 实例级必查的 7 个 (vkGetPhysicalDeviceProperties2 是可选项: 1.0 实例没有它 = 跳过 LUID 匹配)
#define POCB_INST_REQ_FNS(X) \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
	X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceMemoryProperties)

// 可选项 (不进 REQ): vkGetPhysicalDeviceProperties2 = 1.0 实例没有;vkGetPhysicalDeviceImageFormatProperties2
// = 1.1 才有, v0.18.3 的 2d-4 KMT 探测用它查"这个 handle type 配这个格式能不能导入", 查不到就只看导入实测。
#define POCB_INST_FNS(X)                                                                 \
	POCB_INST_REQ_FNS(X) X(vkGetPhysicalDeviceProperties2)                            \
	X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceImageFormatProperties2)

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
	X(vkCmdCopyImage) X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier) \
	X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) X(vkQueueSubmit) \
	X(vkCreateSampler) X(vkDestroySampler) \
	X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
	X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
	X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) \
	X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) // P1-1: 能分配也能释放单 set (按材质索引换绑定时不留泄漏点) \
	                                                   // v0.18.6: SSR v1 相机参数走 push constant (免掉一张 UBO)

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
	// 共享纹理通路 (v0.10.0): D3D11 NT handle → VK 导入, 替掉读回
	ID3D11Texture2D* stex = nullptr;    // D3D11 共享纹理 (渲染结果的落点)
	HANDLE           shandle = nullptr; // stex 的 NT 句柄 (VK 导入用, 进程存活期不关)
	ID3D11Query*     copyQ = nullptr;   // EVENT 查询: 等上一帧 D3D11 拷贝跑完 (跨 API 竞态闸)
	bool             copyQLive = false; // copyQ 已 End、还没等到
	bool             useShared = false; // 本局走共享路径 (任一步失败即回退读回)
	VkImage          simg = VK_NULL_HANDLE;    // 从 stex 导入的 VkImage
	VkDeviceMemory   simgMem = VK_NULL_HANDLE;
	VkCommandBuffer  cmdS = VK_NULL_HANDLE;    // 共享路径命令 (renderpass + 拷进共享图)
	// 统计
	uint64_t frames = 0;
	double   accMs = 0.0;
	double   accFenceMs = 0.0; // vkQueueSubmit+vkWaitForFences 耗时累计 (与读回路径可比)
	double   accGateMs = 0.0;  // event 闸等待累计 (共享路径专有)
};

extern PocbCtx g_pocb; // 全局唯一, v0.17.0 起定义在 vkrenderer.cpp (原来在 main.cpp)

// ---- renderer 对外 API (hook 面调用; 判读脚本看它们打的日志) ----
bool  pocbEnabled();                                 // ini vulkan=0 关闸
void  pocbFail(const char* what, long code);         // 关闸 + 记日志 (v0.9.0 语义)
void  pocbFail(const char* what);
bool  pocbInit(IDXGISwapChain* sc);                  // 首帧: 建实例/设备/管线 + 场景色导入
void  pocbInject(PocbCtx& c, IDXGISwapChain* sc);    // 每帧: 提交+fence+写回 backbuffer+交叉校验
bool  ssrInVkBuild(PocbCtx& c, int slot);            // 2c-β: 建图+导入+初转 (失败只关自己)
void  ssrVkFree(PocbCtx& c);                         // 2c-β: 换槽时 vkDeviceWaitIdle+destroy
void  ssrInVkFrame(PocbCtx& c);                      // 2c-β: 到点读回比对 (节流 前3次+每600次)
void  ssrOutVkBuild(PocbCtx& c);                     // 2d-1: 出向图建图+导入+录命令 (失败只关自己)
void  ssrOutVkFrame(PocbCtx& c);                     // 2d-1: 每帧 入向图→出向图 拷贝 + fence
bool  ssrKmtProbeVk(HANDLE h, const D3D11_TEXTURE2D_DESC& sd); // 2d-4: 路线 1′ KMT 导入探测 (纯发现)
void  ssrInGateWait(PocbCtx& c);                     // O-1 正式修法 (v0.18.4): 2c 入向 EVENT 闸
void  ssrKmtVkFrame(PocbCtx& c);                     // 路线1′ (v0.18.5): 深度 KMT 导入分支 + 跨API读回比对

// ---- renderer 依赖 hook 面的符号 (v0.17.0 过渡态, 2d 收敛成参数后删除) ----
namespace pocmain
{
	void logLine(const std::string& msg);
	std::string pluginDir();                          // ...\Data\SKSE\Plugins
	std::string hexOf(const void* p);                 // "0x00000130…" (2d 出向日志用)
	std::string hexHr(long hr);
	std::string lowerCopy(std::string s);
	bool  iniFlag(const char* key, bool def);
	std::string ssrFmtName(DXGI_FORMAT f);
	std::string uhex64(unsigned long long v);
	unsigned long long ssrFnvSample(const void* data, size_t rowPitch, unsigned width,
	                                unsigned height, int bpp, long* nz);
	unsigned long long ssrFnvSampleAdv(const void* data, size_t rowPitch, unsigned width,
	                                   unsigned height, int stride, int nB, long* nz); // 路线1′ 深度三候选
	unsigned long long ssrInFnv(ID3D11DeviceContext* ctx, ID3D11Texture2D* mir,
	                            ID3D11Texture2D* stg, const char* nm, long* nz,
	                            size_t* pitch = nullptr, // v0.16.3: 实际 RowPitch 要传出去
	                            unsigned long long* alt3 = nullptr); // v0.18.5: 深度低24位候选
	void  installProbeOn(ID3D11Device* dev);

	extern std::atomic<uint64_t>   g_presentCount;    // Present 计数 (日志里"帧=F")
	extern ID3D11Texture2D*        g_ssrInTexC;       // 324 的 SHARED 镜像 (色)
	extern ID3D11Texture2D*        g_ssrInStgC;       // 色镜像 STAGING (算校验和用)
	extern HANDLE                  g_ssrInHC;         // 色镜像 NT handle (VK 导入源)
	extern std::atomic<bool>       g_ssrSharedOn;     // ini ssr.shared
	extern unsigned long long      g_ssrInChkC;       // 本帧 D3D11 算出的色校验和
	extern bool                    g_ssrInChkCValid;
	extern unsigned long long      g_ssrInChkCPrev;   // 上一帧 (差一帧归因)
	extern size_t                  g_ssrInChkPitch;   // D3D11 STAGING 实际 RowPitch (行距归因)
	// ---- 路线1′ (v0.18.5): 深度 KMT 导入分支 (docs/05 D2a-4 定案) ----
	extern ID3D11Texture2D*        g_ssrInTexD;       // 520 的 SHARED 镜像 (深度, 老式 SHARED 建成)
	extern HANDLE                  g_ssrInHD;         // 深度镜像老式 handle (KMT), VK 侧导入源
	extern bool                    g_ssrInDepthKmt;   // 深度走路线1′ ⇒ VK 用 KMT handleType 另开分支
	extern unsigned long long      g_ssrInChkD;       // 本帧 D3D11 深度校验和 (每像素4字节全量)
	extern unsigned long long      g_ssrInChkD3;      // 同上, 每像素前3字节 (跳过 stencil 字节)
	extern bool                    g_ssrInChkDValid;  // 上两行可用, 一次性消费
	// ---- O-1 正式修法 (v0.18.4): 2c 入向的跨 API EVENT 闸 ----
	extern ID3D11Query*            g_ssrInQ;          // D3D11_QUERY_EVENT (main 侧 End / renderer 侧等)
	extern bool                    g_ssrInQLive;      // 已 End、还没等到 (一次性消费)
	extern long                    g_ssrInGateN;      // 闸等到的累计次数
	extern double                  g_ssrInGateMs;     // 闸累计等待 ms
	// ---- v0.18.8 正解B: 段后水深镜像 (第5张) —— 法线/反射原点输入, 520 只当行进层级 ----
	extern std::atomic<bool>       g_ssrWDepOn;       // ini ssr.wdep (默认 1, 只关自己)
	extern ID3D11Texture2D*        g_ssrWDepTex;      // 段后水深镜像 (源 461, 特征B 后首次换绑拷)
	extern HANDLE                  g_ssrWDepH;        // 镜像 handle (KMT 或 NT, 与深度镜像同轴)
	extern bool                    g_ssrWDepKmt;      // true = 老式 SHARED → VK 走 KMT handleType
	extern ID3D11Query*            g_ssrWDepQ;        // 第二条 EVENT (排在 2c 闸之后, 2c 等不到它)
	extern bool                    g_ssrWDepQLive;    // 已 End、还没等到 (一次性消费)
	// ---- 2d-1 出向回写 (v0.18.0) ----
	extern std::atomic<bool>       g_ssrVkOutOn;      // ini ssr.vkout (2d 出向独立逃生门)
	extern ID3D11Texture2D*        g_ssrOutTexC;      // 出向 SHARED 镜像 (desc 照抄 324 ≡ 585)
	extern HANDLE                  g_ssrOutHC;        // 出向镜像 NT handle (VK 导入源)
	extern bool                    g_ssrOutReady;     // VK 已把结果填进出向镜像
	extern long                    g_ssrOutN;         // 已回写 585 的累计次数
	// ---- SSR v1 shader 采样 (v0.18.6, docs/05 D4 / Step3 / R4) ----
	extern std::atomic<bool>       g_ssrV1On;         // ini ssr.v1 (默认 0 = 只上代码不开跑)
	extern int                     g_ssrV1Mode;       // ini ssr.mode  0=透传 1=SSR (R4 之外唯一的观感开关)
	extern float                   g_ssrV1Fov;        // ini ssr.fov   垂直视场角 (度) —— R4 反推用
	extern float                   g_ssrV1Near;       // ini ssr.near
	extern float                   g_ssrV1Far;        // ini ssr.far
	extern float                   g_ssrV1Steps;      // ini ssr.steps ray march 步数
	extern float                   g_ssrV1Strength;   // ini ssr.strength **SSR 替换比 0..1** (v0.18.9 起; 0 = 原版 cubemap)
	extern float                   g_ssrV1Dist;       // ini ssr.dist  ray march 最大距离 (以 near 为单位)
	extern float                   g_ssrV1Rev;        // ini ssr.rev   反向深度开关 (近->1 时置 1)
	// ---- v0.18.7: A 平滑批 (倒影破碎) + B 水色保留 (第4张底色镜像) ----
	extern int                     g_ssrV1Smooth;     // ini ssr.smooth 法线差分邻域 (px, 1..16)
	extern int                     g_ssrV1Blur;       // ini ssr.blur   反射 5tap 空间平滑 0/1
	extern int                     g_ssrV1Debug;      // ini ssr.debug  0 正常/1 法线/2 命中/3 深度/4 段后水深/5 水面像素/6 回注可视化/7 uBase 原样/8 v0.18.15 单位回补量
	extern float                   g_ssrV1Ripple;     // ini ssr.ripple 涟漪回注量 0..1 (v0.18.9, 默认 1)
	extern int                     g_ssrV1RippleSz;   // ini ssr.ripplesz 回注带宽 (px, 1..16; v0.18.10, 默认 4)
	extern int                     g_ssrV1RippleMode; // ini ssr.ripplemode 0=亮度调制 1=位移扭曲 (v0.18.10, 默认 1)
	extern int                     g_ssrV1Edge;       // ini ssr.edge 0=未命中回原版层(默认) 1=屏幕边缘延展 (v0.18.11) 2=真 cubemap 兜底 (v0.18.16)
	extern float                   g_ssrV1RipK;    // ini ssr.ripk 输入端软限幅阈值 (线性亮度, 0=关; v0.18.14, 默认 0)
	extern float                   g_ssrV1RipAmp;  // ini ssr.ripamp 位移幅度 (px, 0=自动 clamp(ripplesz*1.5,6,16); v0.18.14, 默认 0)
	extern float                   g_ssrV1RipGain; // ini ssr.ripgain 梯度增益 (0=自动=10; v0.18.14, 默认 0)
	extern float                   g_ssrV1Det;     // ini ssr.v1det 原版高光回补量 0..1 (0=关; v0.18.15, 默认 0)
	// ---- v0.18.16 (issue B): 探针 cube 从 D3D11 读回后交给 VK 上传 ----
	// D3D11 侧 (main.cpp 的 P1 探测) 负责写这四个, VK 侧 (vkrenderer.cpp) 只读;
	// tick 变 = 内容换过一次 ⇒ ssrV1Sig 变 ⇒ 重录 + 重填描述符 (与其它入向图同一口径)。
	extern unsigned char*          g_probeCpu;        // 6 面紧凑排列 (layer = w*h*8B), 无数据 = nullptr
	extern int                     g_probeW;          // 面宽 (= 升质后的 1024)
	extern int                     g_probeH;          // 面高
	extern int                     g_probeLayers;     // 面数 (cube 恒 6)
	extern unsigned long long      g_probeCpuTick;    // 内容代数 (变了才重传)
	extern std::atomic<bool>       g_ssrBaseOn;       // ini ssr.base585 底色镜像独立门 (默认 1)
	extern ID3D11Texture2D*        g_ssrBaseTex;      // 底色 SHARED 镜像 (585 段16 后 = 含水画面)
	extern HANDLE                  g_ssrBaseH;        // 底色镜像 NT handle (VK 导入源)
}
