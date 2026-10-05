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

#define POCB_INST_FNS(X) \
	POCB_INST_REQ_FNS(X) X(vkGetPhysicalDeviceProperties2) X(vkEnumerateDeviceExtensionProperties)

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
	X(vkCmdCopyImage) X(vkCmdPipelineBarrier) \
	X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) X(vkQueueSubmit) \
	X(vkCreateSampler) X(vkDestroySampler) \
	X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
	X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
	X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBindDescriptorSets)

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
	unsigned long long ssrInFnv(ID3D11DeviceContext* ctx, ID3D11Texture2D* mir,
	                            ID3D11Texture2D* stg, const char* nm, long* nz,
	                            size_t* pitch = nullptr); // v0.16.3: 实际 RowPitch 要传出去
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
	// ---- 2d-1 出向回写 (v0.18.0) ----
	extern std::atomic<bool>       g_ssrVkOutOn;      // ini ssr.vkout (2d 出向独立逃生门)
	extern ID3D11Texture2D*        g_ssrOutTexC;      // 出向 SHARED 镜像 (desc 照抄 324 ≡ 585)
	extern HANDLE                  g_ssrOutHC;        // 出向镜像 NT handle (VK 导入源)
	extern bool                    g_ssrOutReady;     // VK 已把结果填进出向镜像
	extern long                    g_ssrOutN;         // 已回写 585 的累计次数
}
