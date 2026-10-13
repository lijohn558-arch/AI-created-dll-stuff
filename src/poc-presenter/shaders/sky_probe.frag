#version 450
// sky_probe.frag —— T6 天空探针 (1x2 RT, 全屏三角)
//
// 为什么有这个 pass (计划 T6 写的是"太阳方向取 per-frame CB 既有入向"): 本仓当前
// **没有**任何太阳方向/相机朝向的入向 —— T1 的侦察没产出太阳向量, T5 只接了两张
// 入向图 (skyIn 色 / skyDepth 深); 唯一的方向入向 g_ssrV1CamQ 走的是 ctx 槽7 cbuffer
// 读回链, 它由 g_ssrOn + ini ssr.camrot + ssr.edge>=2 三重门控, **sky=1 且 ssr=0 的
// 会话里恒为全 0** (见 main.cpp 安装回显"camVSBOnce 由 g_ssrOn 门控")。
// 而 docs/00 L109 的硬约束禁止生产通路每帧同步读回 ⇒ 不能在 sky 通路里现造一条
// cbuffer 读回链 (那既是新拦截面, 也是新的每帧读回)。
//
// 所以这里用**纯 GPU 探针**顶替那个缺失的入向, 一次提交内拿三样东西, 零读回:
//   texel(0.5,0.25) = vec4(太阳方向 xyz [伪世界系], 天空平均亮度)
//   texel(0.5,0.75) = vec4(地平线所在的屏幕 v [0..1, y 向下], 有效标志, 峰值亮度, 天空占比)
// ① 太阳方向: skyIn 上按 32x18 网格取**天空像素** (depth==1.0 精确), 以亮度三次方
//    加权平均视线方向 —— 游戏自己的天空里最亮的那团就是太阳/最亮的日光方向。
// ② 地平线: skyDepth 上按 64x64 网格, 每列自顶向下找第一条 depth<1 的行 = 该列的地平线,
//    跨列取均值。这是"相机俯仰"的结构化估计 (纯几何, 不用 event id, R2)。
// ③ 平均亮度: 天空像素的亮度均值 ⇒ 合成 pass 用它把散射结果定标到游戏自己的天空量级,
//    白天/黄昏/夜间 (S5) 都不会因为曝光常量而错档。
//
// 三个量都带**帧间平滑** (与上一帧状态 texel 1:1 比较), 平滑系数 0.35 ⇒ 云不会因为
// 探针抖动而跳, 而快速转头时 valid/地平线差值会让合成 pass 走"拒绝历史"分支 (docs/07 §6 R4)。
//
// 伪世界系: 相机在原点, +Y 上, -Z 前; 视线方向由 (屏幕 uv, 地平线 v) 反解 ——
//   ndcX = (u-0.5)*2*tanHalfFov*aspect;  ndcY = (horizon-v)*2*tanHalfFov;  dir = -(Z)
// push constant 口径 (三个 frag 共用同一布局, 见 vkrenderer.cpp SkyPcBlock):
//   cam  = (time, aspect, tan(fovY/2), exposure)
//   sz   = (cloudW, cloudH, windX, windZ)
//   ctl  = (coverage, density, sunIntensity, firstFrame)
//   tier = (cloudscale, lutscale, cloudtemporal, unused)
layout(set = 0, binding = 0) uniform sampler2D uSkyIn; // skyIn RGBA16F (LINEAR clamp)
layout(set = 0, binding = 1) uniform sampler2D uDepth; // skyDepth depth aspect (NEAREST)
layout(set = 0, binding = 2) uniform sampler2D uState; // 1x2 上一帧状态 (NEAREST)

layout(push_constant) uniform SkyPC
{
	vec4 cam;    // x=time(s) y=aspect z=tan(fovY/2) w=exposure
	vec4 sz;     // x=cloudW y=cloudH z=windX w=windZ
	vec4 ctl;    // x=coverage y=density z=sunIntensity w=firstFrame(1=头几帧)
	vec4 tier;   // x=cloudscale y=lutscale z=cloudtemporal w=unused
	vec4 pad0;
	vec4 pad1;
	vec4 pad2;
	vec4 pad3;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

const int kHzN = 64; // 地平线扫描: 列数
const int kHzM = 64; // 地平线扫描: 行数
const int kClN = 32; // 颜色/太阳采样: 列数
const int kClM = 18; // 颜色/太阳采样: 行数

float skyLum(vec3 c)
{
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// 屏幕 uv + 地平线 v ⇒ 伪世界视线方向 (地平线处 y 恰为 0)
vec3 skyRay(vec2 uv, float horizon)
{
	float x = (uv.x - 0.5) * 2.0 * pc.cam.z * pc.cam.y;
	float y = (horizon - uv.y) * 2.0 * pc.cam.z;
	return normalize(vec3(x, y, -1.0));
}

// ---- 三个量的估计 (两个 texel 各算一遍, 代价可忽略; 1x2 = 2 像素) ----
void skyProbe(out vec3 sunDir, out float meanLum, out float horizon, out float valid,
              out float peakLum, out float skyFrac)
{
	// ① 地平线: 每列自顶向下第一条非天空行
	float hSum = 0.0;
	float hCnt = 0.0;
	for (int c = 0; c < kHzN; ++c)
	{
		float u = (float(c) + 0.5) / float(kHzN);
		for (int r = 0; r < kHzM; ++r)
		{
			float v = (float(r) + 0.5) / float(kHzM);
			if (texture(uDepth, vec2(u, v)).x < 0.9999999)
			{
				hSum += v;
				hCnt += 1.0;
				break;
			}
		}
	}
	// ② 颜色网格: 只统计天空像素
	vec3 sunAcc = vec3(0.0);
	float lumAcc = 0.0;
	float cnt = 0.0;
	float peak = 0.0;
	for (int j = 0; j < kClM; ++j)
	{
		float v = (float(j) + 0.5) / float(kClM);
		for (int i = 0; i < kClN; ++i)
		{
			float u = (float(i) + 0.5) / float(kClN);
			vec2 uv = vec2(u, v);
			if (texture(uDepth, uv).x < 0.9999999)
				continue; // 非天空像素 (掩码外) 不参与
			vec3 c = max(texture(uSkyIn, uv).rgb, vec3(0.0));
			float l = min(skyLum(c), 64.0);
			if (l > peak)
				peak = l;
			// 方向用粗地平线 (无地平线的场景退到屏幕中线)
			float hz = hCnt > 0.5 ? (hSum / hCnt) : 0.5;
			sunAcc += skyRay(uv, hz) * (l * l * l);
			lumAcc += l;
			cnt += 1.0;
		}
	}
	valid = (cnt >= 8.0 && hCnt >= 4.0) ? 1.0 : 0.0;
	skyFrac = clamp(cnt / float(kClN * kClM), 0.0, 1.0);
	peakLum = peak;
	if (valid > 0.5)
	{
		horizon = (hCnt > 0.0) ? clamp(hSum / hCnt, 0.0, 1.0) : 0.5;
		meanLum = lumAcc / cnt;
		float sl = length(sunAcc);
		sunDir = (sl > 1e-6) ? (sunAcc / sl) : vec3(0.0, 0.35, -0.94);
	}
	else
	{
		// 全帧无天空 (室内早退场景): 给出中性缺省值, 合成 pass 会因 valid=0 走零工作分支
		horizon = 0.5;
		meanLum = 0.0;
		sunDir = vec3(0.0, 0.35, -0.94);
	}
}

void main()
{
	vec3 sunDir;
	float meanLum, horizon, valid, peakLum, skyFrac;
	skyProbe(sunDir, meanLum, horizon, valid, peakLum, skyFrac);
	// ---- 帧间平滑 (仅当上一帧也有效; 否则直接用本帧值) ----
	vec4 p0 = texture(uState, vec2(0.5, 0.25));
	vec4 p1 = texture(uState, vec2(0.5, 0.75));
	float prevValid = p1.y;
	bool canSmooth = (prevValid > 0.5 && prevValid < 1.5 && valid > 0.5 &&
	                  dot(p0.xyz, p0.xyz) > 0.25 && p0.w > 0.0);
	if (canSmooth)
	{
		const float k = 0.35;
		vec3 mixed = mix(normalize(p0.xyz), sunDir, k);
		sunDir = normalize(mixed);
		horizon = mix(p1.x, horizon, k);
		meanLum = mix(p0.w, meanLum, k);
		peakLum = mix(p1.z, peakLum, k);
	}
	int row = int(gl_FragCoord.y); // 0 = 上排 (太阳/均值), 1 = 下排 (地平线/有效)
	oColor = (row == 0) ? vec4(sunDir, meanLum) : vec4(horizon, valid, peakLum, skyFrac);
}