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
//   本 pass **无条件被录制**, 但像素级早退 —— 拿当前探针 (uProbe) 与 1x1 状态图
//   (uLutState, 存的是**上一次真算时用的太阳方向**) 比夹角, 小于阈值 (spec constant
//   kSunThreshDeg) 时直接采上一帧的 LUT 原样输出 (一次纹理取), 不跑积分。
//   ⇒ 基准是"真算时的太阳"而不是"上一帧的太阳": 探针输出带帧间平滑 (k=0.35), 拿平滑值
//     当基准会让阈值判定在平滑运动下被推迟很久, 而合成侧按**当前**太阳采这张 LUT。
//   ⇒ "太阳不动 ⇒ 每帧只花一次取样" 的低频语义成立, 且**不需要任何 CPU 读回**
//     (docs/00 L109 硬约束): 判定与状态更新完全在 GPU 上。
//   ⇒ 状态更新用**同一条管线再渲一次 1x1** (push constant tier.w = 1 走状态分支),
//     不需要 MRT、不需要新管线、不需要新 shader。
//
// push constant 口径 (四个 frag 共用同一布局, 见 vkrenderer.cpp skyPassRecord 的 pc):
//   cam  = (time, aspect, tan(fovY/2), exposure)
//   sz   = (cloudW, cloudH, windX, windZ)
//   ctl  = (coverage, density, sunIntensity, firstFrame)
//   tier = (cloudscale, lutscale, cloudtemporal, mode<仅本 frag: 0=算 LUT / 1=写状态>)
layout(set = 0, binding = 0) uniform sampler2D uProbe;     // 1x2 本帧探针 (NEAREST)
// binding 1 空着 (原 uProbePrev 已不用, 见 main() 里的注释); 描述符布局是四个 pass 共用的
// 5 binding 表, 空位照填不误 —— 未被着色器引用的 binding 在 Vulkan 里合法。
layout(set = 0, binding = 2) uniform sampler2D uLutPrev;   // 上一帧 LUT (LINEAR clamp)
layout(set = 0, binding = 3) uniform sampler2D uLutState;  // 1x1 上次**真算**用的太阳 (NEAREST)

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
	vec4 state = texture(uLutState, vec2(0.5, 0.5));     // (上次真算用的太阳方向, 有效位)
	vec3 sunDir = (dot(probe.xyz, probe.xyz) > 0.25) ? normalize(probe.xyz) : vec3(0.0, 1.0, 0.0);
	// ---- 低频重算闸: 跟「上一次**真算**时用的太阳方向」比, 不是跟上一帧比 ----
	// 探针输出是帧间平滑过的 (系数 0.35), 平滑运动时每帧增量只有原始增量的 ~0.35 倍;
	// 拿它当基准会把"太阳已走过阈值"这件事推迟很久 (理论上可冻结数分钟), 而合成侧又按
	// **当前** sunDir 去采这张 LUT ⇒ 散射与太阳系统性错位。state 存的是「生成 lut[cur]
	// 时用的那个太阳」, 于是两次真算之间的漂移恒 ≤ kSunThreshDeg, 与平滑系数无关。
	const bool haveRef = (state.w > 0.5) && (dot(state.xyz, state.xyz) > 0.25);
	bool doCompute;
	if (probeH.y < 0.5)
		doCompute = false;               // 全帧无天空: 本 LUT 没人用 (云/合成各自早退), 不跑积分
	else if (!haveRef)
		doCompute = true;                // 首帧 / 基准还没建立
	else
	{
		vec3 refSun = normalize(state.xyz);
		float cs = clamp(dot(sunDir, refSun), -1.0, 1.0);
		doCompute = (degrees(acos(cs)) >= kSunThreshDeg);
	}
	// ---- 状态写回 (mode=1): 渲进 1x1 小图, 与主 LUT **同一条管线 / 同一套描述符**,
	//      所以上面那段判据逐字复用即可 (输入相同 ⇒ 结论必然相同)。 ----
	if (pc.tier.w > 0.5)
	{
		vec3 newSun = doCompute ? sunDir : (haveRef ? normalize(state.xyz) : vec3(0.0, 0.0, 0.0));
		float ok = (doCompute || haveRef) ? 1.0 : 0.0;
		oColor = vec4(newSun, ok);
		return;
	}
	if (!doCompute)
	{
		oColor = texture(uLutPrev, vUV);  // 一次取样沿用上一帧, 连积分都不跑
		return;
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
