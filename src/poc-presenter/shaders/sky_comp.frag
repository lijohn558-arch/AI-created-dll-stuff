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
//   tier = (cloudscale, lutscale, cloudtemporal, mode<本 frag 恒 0>)
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
//   kFade = 掩码边缘 (depth 与远平面的差值) 的过渡宽度, 让接缝再柔一点。
// **必须是像素尺度**: 深度是高度非线性的 (游戏 near=15 / far=353467.6), 归一化深度
// 差 Δd 对应的**米数**随距离变化: dw/dz = (n/z²)/(1-n/f)。kFade=1.6e-3 在 z=10km 处
// 相当于 ~9.4 km 的地形深度带 —— 那会把 9.4 km 以外直到远平面的**所有地形**都压到 kMix
// 的 15% 亮度, 根本不是"接缝柔化"而是整片远景变黑。1e-6 ≈ D24 的 ~17 个码宽, z=1km 处
// ~7cm、z=10km 处 ~6.7m, 才是像素级接缝。
layout(constant_id = 0) const float kMix = 0.85;
layout(constant_id = 1) const float kFade = 1.0e-6;

// ---- 掩码判据 (**与 sky_cloud.frag 里的同名常量必须逐字一致**) ----
// 早退判定用纯 kSkyEps, **不掺 kFade**: 掺进去会让 [kSkyEps-kFade, kSkyEps) 这一段被
// 本 pass 当"天空"、被云 pass 当"非天空", 正好落进云输出中性值(alpha=1 ⇒ 纯散射)的缝里。
const float kSkyEps = 0.9999999;

// ---- LUT 参考 texel 的坐标 (**与 sky_lut.frag / sky_cloud.frag 必须逐字一致**) ----
// 吸附到真实 texel 中心, 使 LINEAR 采样取到的是**单个 texel** —— 这样 LUT 内部
// 「参考点逐位透传」的性质才严格成立 (写死 0.7568 会落在两行/两列之间, 采到插值)。
// 三处各算一遍同样的式子, 而 uLut 与 LUT pass 的 uLutPrev 同尺寸 ⇒ 坐标必然一致。
vec2 refTexel()
{
	ivec2 sz = textureSize(uLut, 0);
	float x = (floor(0.5 * float(sz.x) - 0.5) + 0.5) / float(sz.x);
	float y = (floor(0.7568 * float(sz.y) - 0.5) + 0.5) / float(sz.y);
	return vec2(x, y);
}

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

const float kPi = 3.14159265358979;

float sinToV(float s)
{
	return sqrt(clamp((s + 0.10) / 1.10, 0.0, 1.0));
}

void main()
{
	vec4 orig = texture(uSkyIn, vUV);
	float d = texture(uMask, vUV).x;
	// ---- 掩码外: 原样透传 (室内整帧走这里 ⇒ 逐像素着色零工作) ----
	// 注意措辞: 这里省掉的是**逐像素着色**, 不是整趟 pass —— 全屏 dispatch 与出向整帧
	// CopyResource 照旧发生 (省那一趟要靠预案 A 子矩形, 本轮不做)。
	if (d < kSkyEps)
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
	// 云 pass 的三个早退写 vec4(0,0,0,**1**) ⇒ alpha=1 ⇒ 这里退化成"只有散射色", 正确。
	vec4 cl = texture(uCloud, vUV);
	vec3 sky = cl.rgb + scatter * cl.a;
	// ---- 强度标定: 与游戏自己天空的平均亮度对齐 (v0.18.38 改为**帧内**参考量) ----
	// 原先用**逐像素**的模型亮度 skyLum 做分母 (RC3): gain = meanLum/skyLum 逐像素成立,
	// 于是 sky*gain ≈ exposure*meanLum 处处相等 —— 天空只剩色相, 模型本身那条「地平线比
	// 天顶亮」的梯度被彻底抹平; 而近太阳处 Mie 前向峰 × gain 直接吹成全屏白。
	// 现在分母换成**帧常量**: LUT 的**参考 texel** —— 与 LUT 内部做范围压缩用的、以及云
	// pass 取环境光用的, 是**同一个 texel**; 坐标由 refTexel() 吸附到真实 texel 中心,
	// 三个 pass 各算一遍同样的式子 ⇒ 必然一致 (见 sky_lut.frag 的 refTexel/compressLum)。
	// 不能再用天顶 (u=0.5, v=0.98): 在这个单次散射模型里天顶是全场**最暗**的方向, 拿它
	// 当分母等于「按最暗处定标」, 于是地平线/日晕那些行 (实测是背阳中天的 5.4 / 16.6 /
	// 153 倍, 45°/10°/夜间) 会原样被推成 5~153 倍的游戏均值 ⇒ 直接进 tonemap ⇒ 炸白带。
	// 整帧只有一个标量 ⇒ 模型的梯度原样保留: 地平线变暖、天顶保持蓝、近太阳是一圈
	// 自然的白晕而不是全屏过曝。云的 RT 本来就是以这个参考为单位的, 于是云与天空同标定。
	float meanLum = max(probe.w, 1e-4);
	vec3 refC = texture(uLut, refTexel()).rgb;
	float refLum = max(dot(refC, vec3(0.2126, 0.7152, 0.0722)), 1e-5);
	// 上限 32: refLum 只在 ref 退化到极小时才会顶到这个值 (正常天空的增益≈1, 因为
	// 又能在 ref 数值异常时把整帧的放大锁在 32× 以内, 不至于把画面整体吹白。
	float gain = pc.cam.w * clamp(meanLum / refLum, 0.1, 32.0);
	sky *= gain;
	// ---- 与原版混合 + 掩码边缘软过渡 (kFade 是**像素尺度**, 见上面的推导) ----
	vec3 outc = mix(orig.rgb, sky, kMix);
	float edge = clamp((d - kSkyEps) / kFade, 0.0, 1.0);
	outc = mix(orig.rgb, outc, mix(kMix, 1.0, edge));
	// ---- 输出: alpha 保持 skyIn 原值 (HDR 目标的 alpha 语义不由我方决定) ----
	oColor = vec4(max(outc, vec3(0.0)), orig.a);
}
