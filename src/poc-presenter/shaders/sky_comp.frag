#version 450
// sky_comp.frag —— T6 pass ③: 合成 (全屏三角, 输出到出向共享镜像 skyOut)
//
// 契约 (docs/07 §3.4 + 计划 T6「Pass 序」第三条):
//   天空色 = 散射 LUT(视线) 上叠体积云, **掩码内替换 skyIn 原色**;
//   掩码外逐字节透传 skyIn (early-out, 一步数学都不做) —— 这条同时就是室内早退。
//
// 色调映射口径: 游戏走自己的 tonemap, 我方**不做** tonemap, 只把 radiance 线性
// 叠进去; 强度标定靠两处:
//   ① pc.cam.w (exposure) 常数;
//   ② 探针给的 meanLum (游戏自己天空的平均亮度) —— 按 ratio 线性对齐,
//      于是白天/黄昏/夜间 (S1/S3/S5) 三档都不用改代码就对得上量级。
// 另外按 rays 与原 skyIn 的 luminance 做**混合** (kMix 原版比), 避免"接缝"过生硬;
// kMix=0.85 是本轮定的视觉档位常量 (经 spec constant 烘焙, 改它不用重编档位表)。
//
// push constant 口径 (三个 frag 共用同一布局, 见 vkrenderer.cpp SkyPcBlock):
//   cam  = (time, aspect, tan(fovY/2), exposure)
//   sz   = (cloudW, cloudH, windX, windZ)
//   ctl  = (coverage, density, sunIntensity, firstFrame)
//   tier = (cloudscale, lutscale, cloudtemporal, unused)
layout(set = 0, binding = 0) uniform sampler2D uSkyIn;   // skyIn RGBA16F (LINEAR clamp)
layout(set = 0, binding = 1) uniform sampler2D uMask;    // skyDepth depth aspect (NEAREST)
layout(set = 0, binding = 2) uniform sampler2D uLut;     // 散射 LUT (LINEAR clamp)
layout(set = 0, binding = 3) uniform sampler2D uCloud;   // 云 RT (LINEAR clamp)
layout(set = 0, binding = 4) uniform sampler2D uProbe;   // 1x2 探针 (NEAREST)

layout(push_constant) uniform SkyPC
{
	vec4 cam;
	vec4 sz;
	vec4 ctl;
	vec4 tier;
	vec4 pad0;
	vec4 pad1;
	vec4 pad2;
	vec4 pad3;
} pc;

// 档位烘焙 (VkSpecializationInfo, 仿 kMissFade 模式):
//   kMix  = 与原 skyIn 的混合比 (0 = 纯替换, 1 = 原版); 0.85 是本轮视觉档位
//          (计划 T6「掩码内替换 skyIn 原色」; 留 0.15 给原版是**接缝兜底**: 掩码
//          在物体轮廓处是硬边, 纯替换会让地形边缘出现 1px 硬接缝)
//   kFade = 掩码边缘 (depth 与远平面的差值) 的过渡宽度, 让接缝再柔一点
layout(constant_id = 0) const float kMix = 0.85;
layout(constant_id = 1) const float kFade = 0.0016;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

const float kPi = 3.14159265358979;

float vToSin(float v)
{
	float t = clamp(v, 0.0, 1.0);
	return -0.10 + 1.10 * (t * t);
}
float sinToV(float s)
{
	return sqrt(clamp((s + 0.10) / 1.10, 0.0, 1.0));
}

void main()
{
	vec4 orig = texture(uSkyIn, vUV);
	float d = texture(uMask, vUV).x;
	// ---- 掩码外: 原样透传 (室内整帧走这里 ⇒ 合成零工作) ----
	if (d < 0.9999999 - kFade)
	{
		oColor = orig;
		return;
	}
	vec4 probe = texture(uProbe, vec2(0.5, 0.25));
	vec4 probeH = texture(uProbe, vec2(0.5, 0.75));
	if (probeH.y < 0.5)
	{
		oColor = orig;
		return;
	}
	vec3 sunDir = (dot(probe.xyz, probe.xyz) > 0.25) ? normalize(probe.xyz) : vec3(0.0, 1.0, 0.0);
	float horizon = probeH.x;
	float ndcX = (vUV.x - 0.5) * 2.0 * pc.cam.z * pc.cam.y;
	float ndcY = (horizon - vUV.y) * 2.0 * pc.cam.z;
	vec3 rd = normalize(vec3(ndcX, ndcY, -1.0));
	// ---- LUT 取样: v = 视线高度, u = 与太阳方位角的夹角 (与 sky_lut 的反解同构) ----
	vec3 sunH = normalize(vec3(sunDir.x, 0.0, sunDir.z) + vec3(1e-5, 0.0, 0.0));
	vec3 side = vec3(sunH.z, 0.0, -sunH.x);
	vec3 rdH = vec3(rd.x, 0.0, rd.z);
	float hlen = length(rdH);
	float u = 0.5;
	if (hlen > 1e-5)
	{
		vec3 d3 = rdH / hlen;
		float cphi = dot(d3, sunH);
		float sphi = dot(d3, side);
		u = fract(atan(sphi, cphi) / (2.0 * kPi));
	}
	float v = sinToV(rd.y);
	vec3 scatter = texture(uLut, vec2(clamp(u, 0.001, 0.999), clamp(v, 0.001, 0.999))).rgb;
	// ---- 云: 云 RT 是 (散射色, 透射率); alpha 复合 = cloud.rgb + scatter*(1-trans) ----
	vec4 cl = texture(uCloud, vUV);
	vec3 sky = cl.rgb + scatter * cl.a;
	// ---- 强度标定: 与游戏自己天空的平均亮度对齐 ----
	float meanLum = max(probe.w, 1e-4);
	float skyLum = dot(sky, vec3(0.2126, 0.7152, 0.0722));
	float gain = pc.cam.w * clamp(meanLum / max(skyLum, 1e-4), 0.15, 8.0);
	sky *= gain;
	// ---- 与原版混合 + 掩码边缘软过渡 ----
	vec3 outc = mix(orig.rgb, sky, kMix);
	float edge = (d >= 0.9999999 - kFade) ? clamp((d - (0.9999999 - kFade)) / kFade, 0.0, 1.0) : 1.0;
	outc = mix(orig.rgb, outc, mix(kMix, 1.0, edge));
	// ---- 输出: alpha 保持 skyIn 原值 (HDR 目标的 alpha 语义不由我方决定) ----
	oColor = vec4(max(outc, vec3(0.0)), orig.a);
}