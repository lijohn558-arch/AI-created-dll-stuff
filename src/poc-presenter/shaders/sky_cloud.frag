#version 450
// sky_cloud.frag —— T6 pass ②: 体积云 raymarch (分辨率 = 原尺寸 x sky.cloudscale)
//
// 算法思路 (公开思路, 独立实现): 分层 slab (云底/云顶之间) 沿视线线性步进, 每步取
// 3D 值噪声 (Perlin-Worley 风格的 FBM) 当密度, 再向太阳方向做 3 步光线行进估光学厚度
// (Beer-Lambert + Henyey-Greenstein 相位函数), 逐段 front-to-back 合成 alpha。
// 与 SSR v1 同款的"逐像素步进"结构, 只是维度从 2D 屏幕空间升到 3D 世界。
//
// 工作域 = 掩码像素集 (docs/07 §3.4): depth 非远平面 ⇒ 立刻输出零并 return,
// 这一支让**室内 (掩码全空) 的云 pass 逐像素零工作** —— 早退不是"跳过整 pass"
// (那需要读回包围盒 = 预案 A, 本轮不做), 而是"掩码外像素一步不进march"。
//
// 时域复用 (sky.cloudtemporal=1): ping-pong 两张 RT, 采样上一帧同 uv 的结果,
// 系数 kHistory (spec constant)。**不做重投影** —— 本仓没有相机矩阵入向
// (见 sky_probe.frag 顶部说明), 所以历史权重压得很低 (0.55) 且带亮度比拒绝,
// 快速转头时由拒绝项自然衰减 (docs/07 §6 R4 的"历史拒绝"兜底)。
//
// push constant 口径 (三个 frag 共用同一布局, 见 vkrenderer.cpp SkyPcBlock):
//   cam  = (time, aspect, tan(fovY/2), exposure)
//   sz   = (cloudW, cloudH, windX, windZ)
//   ctl  = (coverage, density, sunIntensity, firstFrame)
//   tier = (cloudscale, lutscale, cloudtemporal, mode<本 frag 恒 0>)
layout(set = 0, binding = 0) uniform sampler2D uProbe;    // 1x2 本帧探针 (NEAREST)
layout(set = 0, binding = 1) uniform sampler2D uMask;     // skyDepth depth aspect (NEAREST)
layout(set = 0, binding = 2) uniform sampler2D uLut;      // 散射 LUT (LINEAR clamp)
layout(set = 0, binding = 3) uniform sampler2D uHistory;  // 上一帧云 RT (LINEAR clamp)
layout(set = 0, binding = 4) uniform sampler3D uNoise;    // 3D 噪声 (REPEAT, 手工生成)

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
//   kSteps = sky.cloudsteps (T4 解析出的档位值, 32/48/64/128) —— 每像素步数
//   kHistory = cloudtemporal=1 时的历史权重 (0 = 关), 烘焙而不是每帧推
// 一律 float 声明 (数据块按 float 传, 与 kCubeElev/kMissFade 同款)。
layout(constant_id = 0) const float kSteps = 64.0;
layout(constant_id = 1) const float kHistory = 0.55;

// ---- 掩码判据 (**与 sky_comp.frag 里的同名常量必须逐字一致**) ----
// skyDepth 是 D24 归一化值, 天空 = 精确满值 0xFFFFFF ⇒ 1.0。留一点点余量吸收驱动端的
// 24bit→float 舍入。云 pass 与合成 pass 用**同一条**判据, 两边不允许各写各的阈值。
const float kSkyEps = 0.9999999;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

const float kPi = 3.14159265358979;

float hg(float mu, float g)
{
	float g2 = g * g;
	float d = 1.0 + g2 - 2.0 * g * mu;
	return (1.0 - g2) / (4.0 * kPi * max(d * sqrt(max(d, 1e-6)), 1e-6));
}

float hash1(vec3 p)
{
	p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// 值噪声 (三线性插值) —— 与 3D 噪声纹理互补: 纹理给大尺度形状, 这里给高频细节
float vnoise(vec3 x)
{
	vec3 i = floor(x);
	vec3 f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	float n000 = hash1(i + vec3(0.0, 0.0, 0.0));
	float n100 = hash1(i + vec3(1.0, 0.0, 0.0));
	float n010 = hash1(i + vec3(0.0, 1.0, 0.0));
	float n110 = hash1(i + vec3(1.0, 1.0, 0.0));
	float n001 = hash1(i + vec3(0.0, 0.0, 1.0));
	float n101 = hash1(i + vec3(1.0, 0.0, 1.0));
	float n011 = hash1(i + vec3(0.0, 1.0, 1.0));
	float n111 = hash1(i + vec3(1.0, 1.0, 1.0));
	return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
	           mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

// 密度剖面: 云底以下/云顶以上为 0, 中间按高度做 smoothstep 塑形 (标准 slab 云)
float cloudDensity(vec3 p, float hBase, float hSpan, float coverage, float shape)
{
	if (p.y < hBase || p.y > hBase + hSpan)
		return 0.0;
	float hf = clamp((p.y - hBase) / hSpan, 0.0, 1.0);
	// 高度权重: 底部稍薄, 中上部最厚
	float prof = smoothstep(0.0, 0.22, hf) * (1.0 - smoothstep(0.62, 1.0, hf) * 0.85);
	vec3 q = p * 0.00035 + vec3(pc.sz.z, 0.0, pc.sz.w) * pc.cam.x;
	float lo = texture(uNoise, q).r;
	float hi = vnoise(p * 0.0021 + vec3(pc.sz.z, 0.0, pc.sz.w) * pc.cam.x * 1.7);
	float n = mix(lo, hi, 0.35);
	// coverage 越高 => 阈值越低 => 云越厚
	float d = clamp((n - (1.0 - coverage)) * 2.6, 0.0, 1.0) * prof * shape;
	return clamp(d, 0.0, 1.0);
}

void main()
{
	// ---- 掩码外: 原样透传 (早退; 但**必须写中性值**, 见下) ----
	// 中性 = vec4(0,0,0,**1**): 合成侧 `sky = cl.rgb + scatter*cl.a`, alpha 0 会让整段天空
	// 变成纯黑 (mix(orig, 0, kMix) 只剩 15% 亮度), alpha 1 才是"无云、全透射"。
	// 判据 kSkyEps 与 sky_comp.frag **逐字相同** —— 两处一旦不一致, 中间那段深度会既被判
	// "非天空"(这里写中性) 又被判 "天空"(那里照样替换), 直接吃到上面那个黑缺陷。
	if (texture(uMask, vUV).x < kSkyEps)
	{
		oColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	vec4 probe = texture(uProbe, vec2(0.5, 0.25));
	vec4 probeH = texture(uProbe, vec2(0.5, 0.75));
	if (probeH.y < 0.5)
	{
		oColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	vec3 sunDir = normalize(probe.xyz + vec3(1e-6, 0.0, 0.0));
	float horizon = probeH.x;
	// ---- 视线 (与探针/LUT 同一套伪世界反解: 地平线 v 处 y=0) ----
	float ndcX = (vUV.x - 0.5) * 2.0 * pc.cam.z * pc.cam.y;
	float ndcY = (horizon - vUV.y) * 2.0 * pc.cam.z;
	vec3 rd = normalize(vec3(ndcX, ndcY, -1.0));
	// 地平线以下没有云 (云的"底"在 y=hBase, 相机在 y=0)
	float hBase = 1400.0;
	float hSpan = 2200.0;
	if (rd.y <= 0.008)
	{
		oColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	// 进/出 slab 距离: **两端都要钳**。只钳 tExit 时, 天顶方向 (rd.y≈1) 的 tExit=3600 小于
	// 天底方向被 rd.y<=0.008 挡掉之前算出的 tEnter 上限区, dt 可能为负 ⇒ 步进反向 ⇒ 最低
	// 约 5° 的天空里云直接消失。钳成同一个上限 ⇒ 退化成"沿射线走满上限"的极限步进。
	float tEnter = hBase / rd.y;
	float tExit = (hBase + hSpan) / rd.y;
	tEnter = min(tEnter, 60000.0);
	tExit = min(max(tExit, tEnter), 60000.0);
	float coverage = pc.ctl.x;
	float shape = pc.ctl.y;
	float density = 0.06; // 每米消光尺度
	int n = max(int(kSteps + 0.5), 4);
	float dt = (tExit - tEnter) / float(n);
	float mu = dot(rd, sunDir);
	float ph = mix(hg(mu, 0.72), hg(mu, -0.28), 0.35) * 4.0 * kPi;
	// 太阳低于地平线 => 不给直射 (S5 夜间场景)
	float sunUp = clamp(sunDir.y * 6.0 + 0.15, 0.0, 1.0);
	vec3 sunRad = vec3(1.0, 0.92, 0.80) * pc.ctl.z * sunUp;
	// 天空环境光: 从 LUT 的天顶方向取 (LUT v=1 是天顶)
	vec3 skyAmb = texture(uLut, vec2(0.5, 0.98)).rgb * 6.0;
	vec3 scat = vec3(0.0);
	float trans = 1.0;
	for (int i = 0; i < n; ++i)
	{
		if (trans < 0.02)
			break;
		// 每步起点加一点 hash 抖动, 抵消步进带来的层状 banding
		float jitter = hash1(vec3(gl_FragCoord.xy, float(i)));
		vec3 p = rd * (tEnter + dt * (float(i) + jitter));
		float d = cloudDensity(p, hBase, hSpan, coverage, shape);
		if (d <= 0.001)
			continue;
		// 朝太阳 3 步光线行进 (光学厚度)
		float lt = 0.0;
		float ls = 260.0;
		vec3 lp = p;
		for (int j = 0; j < 3; ++j)
		{
			lp += sunDir * ls;
			lt += cloudDensity(lp, hBase, hSpan, coverage, shape) * ls;
		}
		float beer = exp(-lt * density);
		// 粉末项 (Powder): 视线穿厚云时边缘变亮, 廉价但很出体积感
		float powder = 1.0 - exp(-d * 6.0);
		float hf = clamp((p.y - hBase) / hSpan, 0.0, 1.0);
		vec3 lum = sunRad * beer * ph * (0.55 + 0.75 * powder) + skyAmb * (0.35 + 0.65 * hf);
		float a = 1.0 - exp(-d * density * dt);
		scat += trans * a * lum;
		trans *= (1.0 - a);
	}
	// ---- 时域复用 (cloudtemporal) ----
	if (kHistory > 0.0 && pc.tier.z > 0.5 && pc.ctl.w < 0.5)
	{
		vec3 h = texture(uHistory, vUV).rgb;
		// 亮度比拒绝: 本帧比历史暗一半以上 => 判定为"历史是上一帧的旧内容", 丢弃
		float lc = dot(scat, vec3(0.2126, 0.7152, 0.0722));
		float lh = dot(h, vec3(0.2126, 0.7152, 0.0722));
		float keep = (lc > 0.0008 || lh <= 0.0008) ? kHistory : 0.0;
		scat = mix(scat, h, keep);
	}
	oColor = vec4(max(scat, vec3(0.0)), trans);
}
