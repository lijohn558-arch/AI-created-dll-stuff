#version 450
// sky_lut.frag —— T6 pass ①: 大气散射 LUT 预计算 (RGBA16F, lutscale x 64x32)
//
// 算法思路 (公开论文的通用物理模型, **独立实现**, 未参考任何现有实现代码):
//   指数大气 (球对称) + 单次散射积分 —— Rayleigh(分子) + Mie(气溶胶) 相位函数,
//   视线方向 沿程 N 步, 每步再向太阳方向 M 步估光学厚度 (Beer-Lambert)。
//   这就是"Precomputed Atmospheric Scattering" 一族 (Bruneton / Nishita / Hillaire)
//   的**最简形式**: 不建 3D LUT, 只建一张按 (视线天顶角, 视线-太阳相对方位角)
//   参数化的 sky-view LUT, 每像素 ~16x5 次采样, 全图 64x32 ⇒ 每帧不到 0.05ms。
//
// 低频重算 (docs/07: "按太阳方向变化阈值低频重算"):
//   本 pass **无条件被录制**, 但像素级早退 —— 拿当前探针 (uProbe) 与上一帧探针
//   (uProbePrev) 的太阳方向比夹角, 小于阈值 (spec constant kSunThreshDeg) 时直接
//   采上一帧的 LUT 原样输出 (一次纹理取), 不跑积分。阈值之上才真算。
//   ⇒ "太阳不动 ⇒ 每帧只花一次 2x2 取样" 的低频语义成立, 且**不需要任何 CPU 读回**
//     (docs/00 L109 硬约束): 判定完全在 GPU 上。
//
// push constant 口径 (三个 frag 共用同一布局, 见 vkrenderer.cpp SkyPcBlock):
//   cam  = (time, aspect, tan(fovY/2), exposure)
//   sz   = (cloudW, cloudH, windX, windZ)
//   ctl  = (coverage, density, sunIntensity, firstFrame)
//   tier = (cloudscale, lutscale, cloudtemporal, unused)
layout(set = 0, binding = 0) uniform sampler2D uProbe;     // 1x2 本帧探针 (NEAREST)
layout(set = 0, binding = 1) uniform sampler2D uProbePrev; // 1x2 上一帧探针 (NEAREST)
layout(set = 0, binding = 2) uniform sampler2D uLutPrev;   // 上一帧 LUT (LINEAR clamp)

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

// 档位烘焙 (VkSpecializationInfo, 仿 kMissFade 模式): kLutSteps 随 lutscale 档位变
// (lutscale=0.5/1 -> 16 步, 2 -> 28 步); kSunThreshDeg = 低频重算的太阳夹角阈值。
// 一律用 **float** 声明 (VkSpecializationMapEntry 的 4 字节数据块按 float 传, 与
// kCubeElev/kMissFade 同款, 免去 int/float 混写的字节序坑); 用处再取整。
layout(constant_id = 0) const float kLutSteps = 16.0;
layout(constant_id = 1) const float kSunThreshDeg = 0.35;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

const float kPi = 3.14159265358979;
const float kRg = 6360000.0;  // 地面半径 (m)
const float kRt = 6420000.0;  // 大气顶半径 (m)
const float kHr = 8000.0;     // Rayleigh 标高
const float kHm = 1200.0;     // Mie 标高
const vec3  kBetaR = vec3(5.802e-6, 13.558e-6, 33.1e-6); // Rayleigh 散射系数 (1/m)
const float kBetaM = 3.996e-6;                            // Mie 散射系数
const float kBetaMe = 4.40e-6;                            // Mie 消光系数 (含吸收)
const float kMieG = 0.80;

float raySphere(vec3 ro, vec3 rd, float R)
{
	float b = dot(ro, rd);
	float c = dot(ro, ro) - R * R;
	float d = b * b - c;
	if (d < 0.0)
		return -1.0;
	float s = sqrt(d);
	float t0 = -b - s;
	if (t0 > 0.0)
		return t0;
	float t1 = -b + s;
	return (t1 > 0.0) ? t1 : -1.0;
}

// 沿 dir 从 p 到大气顶的光学厚度 (各自标高指数密度, 固定 5 步中值)
void opticalDepth(vec3 p, vec3 dir, out float odR, out float odM)
{
	float t = raySphere(p, dir, kRt);
	odR = 0.0;
	odM = 0.0;
	if (t <= 0.0)
		return;
	const int kSteps = 5;
	float dt = t / float(kSteps);
	for (int i = 0; i < kSteps; ++i)
	{
		vec3 q = p + dir * (dt * (float(i) + 0.5));
		float h = max(length(q) - kRg, 0.0);
		odR += exp(-h / kHr) * dt;
		odM += exp(-h / kHm) * dt;
	}
}

float phaseR(float mu)
{
	return (3.0 / (16.0 * kPi)) * (1.0 + mu * mu);
}

float phaseM(float mu)
{
	float g = kMieG;
	float g2 = g * g;
	float d = 1.0 + g2 - 2.0 * g * mu;
	return (1.0 - g2) / (4.0 * kPi * max(d * sqrt(max(d, 1e-6)), 1e-6));
}

// v 参数 → 视线高度分量 sin(zenith); v=0 略在地平线下, v=1 天顶; v^2 分布把采样压向地平线
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
	vec4 probe = texture(uProbe, vec2(0.5, 0.25));       // (sunDir.xyz, meanLum)
	vec4 probeH = texture(uProbe, vec2(0.5, 0.75));      // (horizon, valid, peak, frac)
	vec4 pprobe = texture(uProbePrev, vec2(0.5, 0.25));
	vec3 sunDir = (dot(probe.xyz, probe.xyz) > 0.25) ? normalize(probe.xyz) : vec3(0.0, 1.0, 0.0);
	// ---- 低频重算闸: 太阳夹角小于阈值 ⇒ 直接沿用上一帧 LUT ----
	if (probeH.y > 0.5 && dot(pprobe.xyz, pprobe.xyz) > 0.25)
	{
		vec3 pSun = normalize(pprobe.xyz);
		float cs = clamp(dot(sunDir, pSun), -1.0, 1.0);
		float ang = degrees(acos(cs));
		if (ang < kSunThreshDeg)
		{
			oColor = texture(uLutPrev, vUV);
			return;
		}
	}
	// ---- uv → 视线方向 (以太阳的方位角为参考) ----
	float phi = vUV.x * 2.0 * kPi;
	float s = vToSin(vUV.y);
	float cz = sqrt(max(1.0 - s * s, 0.0));
	vec3 sunH = normalize(vec3(sunDir.x, 0.0, sunDir.z) + vec3(1e-5, 0.0, 0.0));
	vec3 side = vec3(sunH.z, 0.0, -sunH.x); // 水平面内垂直于太阳方位
	vec3 rd = normalize(sunH * (cz * cos(phi)) + side * (cz * sin(phi)) + vec3(0.0, 1.0, 0.0) * s);
	// ---- 视线积分 ----
	vec3 ro = vec3(0.0, kRg + 2.0, 0.0); // 相机贴地 (相对大气可忽略高度差)
	float tTop = raySphere(ro, rd, kRt);
	float tGnd = raySphere(ro, rd, kRg);
	if (tTop <= 0.0)
	{
		oColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	float tMax = (tGnd > 0.0) ? tGnd : tTop;
	float mu = dot(rd, sunDir);
	float pR = phaseR(mu);
	float pM = phaseM(mu);
	vec3 sumR = vec3(0.0);
	vec3 sumM = vec3(0.0);
	vec3 trans = vec3(1.0);
	int n = max(int(kLutSteps + 0.5), 4);
	float dt = tMax / float(n);
	for (int i = 0; i < n; ++i)
	{
		vec3 p = ro + rd * (dt * (float(i) + 0.5));
		float h = max(length(p) - kRg, 0.0);
		float dR = exp(-h / kHr) * dt;
		float dM = exp(-h / kHm) * dt;
		float lR, lM;
		opticalDepth(p, sunDir, lR, lM);
		vec3 tau = kBetaR * (dR + lR) + kBetaMe * (dM + lM);
		vec3 att = exp(-tau);
		sumR += trans * att * dR;
		sumM += trans * att * dM;
		trans *= att;
	}
	vec3 inscatter = (sumR * kBetaR * pR + sumM * kBetaM * pM) * pc.ctl.z;
	// 地平线以下淡出到近黑 (掩码理论上不会命中, 兜底防探针地平线估偏时糊成一片)
	float below = smoothstep(-0.10, 0.02, s);
	inscatter *= below;
	oColor = vec4(max(inscatter, vec3(0.0)), clamp(dot(trans, vec3(0.3333)), 0.0, 1.0));
}