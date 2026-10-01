#pragma once

// ============================================================
// SKSE64 最小插件 ABI 声明
//
// 结构体复制自 SKSE64 2.0.20 官方源码:
//   src/skse64/skse64/PluginAPI.h
//   来源: https://skse.silverlock.org/beta/skse64_2_00_20.7z
//
// 受 SKSE 许可约束 (上游 src/skse64/skse64_license.txt):
// 本文件不得被 Skyrim Together / Skyrim Online 团队使用。
//
// 说明: 本文件只保留插件 SKSEPlugin_Query / SKSEPlugin_Load
// 所需的 ABI 稳定结构体。该 ABI 自 SKSE64 2.0.x 起未发生变化,
// 与游戏版本 1.5.97 / SKSE 2.0.20 匹配, 也可用于其他 2.0.x。
// ============================================================

#include <cstdint>

typedef std::uint32_t UInt32; // 定义与上游 common/ITypes.h 一致 (unsigned int)
typedef UInt32 PluginHandle;  // treat this as an opaque type

struct PluginInfo
{
	enum
	{
		kInfoVersion = 1
	};

	UInt32       infoVersion;
	const char*  name;
	UInt32       version;
};

enum
{
	kPluginHandle_Invalid = 0xFFFFFFFF
};

struct SKSEInterface
{
	UInt32 skseVersion;
	UInt32 runtimeVersion;
	UInt32 editorVersion;
	UInt32 isEditor;
	void*  (*QueryInterface)(UInt32 id);

	// 在 Query/Load 期间调用一次并保存结果
	PluginHandle (*GetPluginHandle)(void);

	// 返回 SKSE build 的 release index
	UInt32 (*GetReleaseIndex)(void);

	// 最低 SKSE 2.0.18; 仅在 PostLoad 消息之后有效
	const PluginInfo* (*GetPluginInfo)(const char* name);
};

// 插件必须导出的两个入口 (C linkage, cdecl)
//   typedef bool(*_SKSEPlugin_Query)(const SKSEInterface* skse, PluginInfo* info);
//   typedef bool(*_SKSEPlugin_Load)(const SKSEInterface* skse);
