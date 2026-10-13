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
// v0.18.38 (视觉初验): 直接阳光项重建。原先用 sunRad·beer·ph·powder, 其中 beer 由
// "3 步 × 260m × density 0.06/m" 的光学厚度决定 ⇒ 只要光线在云里 (d>=0.1) OD 就是
// 15~47, beer = exp(-OD) ≈ 0 **到处都成立** ⇒ 直接项在数学上是死的, 云只被环境项
// 照亮, 呈现为暗蓝灰团块。现在:
//   全部以 **LUT 参考 texel 的散射 (`refC`, 背阳中等仰角那一档)** 为单位表达 —— 注意
//   **不是天顶**: 在这个单次散射模型里天顶是全场最暗的方向, 拿它当单位会把整片天空
//   标成几十倍 (见 sky_lut.frag 的 compressLum 与报告的 CPU 参考表)。于是
//   "受光面 ≈ 2×参考 (白天读起来是白的) / 云底内部 ≈ 0.5~1.6×参考 (白灰, 不发黑)"
//   这两个目标可以直接写成常数, 不依赖 sunIntensity/exposure 的绝对值。
//   kFace   = 受光面标定。**标定用的是侧向散射 (mu=0, 视线⊥太阳) 的 phN = 0.036**:
//             42 * 0.036 * 1.18 (粉末项) ≈ **1.78×参考 ≈ 2×参考**。
//             关键: 真正被限制的不是 phN 而是**乘积 kFace·phN** (见 kFaceMax)。
//   kFaceMax = **乘积上限 3.0** (spec constant_id 6, 由 vkrenderer.cpp 的 cloudData[6]
//             喂进来; 与 GLSL 默认值必须一致)。相位前向峰在 mu→1 是 1.7, 比侧向高 47 倍,
//             乘上 kFace 就是 42*1.7 ≈ 71×参考 —— 游戏自己的 tonemap 吸收不掉, 那正是
//             上一版 (钳 phN≤0.35, 乘积仍达 11.2 ⇒ 约 15×meanLum) 的过曝缺陷换个地方复发。
//             钳乘积而不是钳相位: 前向/侧向之比仍保留 3.0/1.51 ≈ 2.0 倍 (银边还在),
//             但绝对峰值被钉在 3.0*1.18 ≈ 3.5×参考 —— 落在「前向峰 2~4×参考」的设计意图里。
//   kMs     = **多次散射地板**: 厚云内部的亮光主要来自多次散射, 单次散射的 beer 在
//             深内部必然趋 0。0.03 (上一轮) ⇒ 内部只有 1.51*0.03 ≈ 0.045×参考, 剩下的
//             0.47×参考 全靠环境项 ⇒ 背光/厚处读成**暗灰** (Screenshot_108 那一坨)。
//             取 **0.20**: 内部直接项 = faceK*kMs = 1.51*0.20 = 0.30×参考, 加上环境项
//             (云底 0.45 / 云中 0.62 / 云顶 1.30) ⇒ 内部落在 0.75~1.60×参考 的白灰区间;
//             受光面 = 1.51*1.18 + 0.30 = 2.08×参考 (侧向) 与 3.0*1.18+0.30 = 3.84×参考
//             (正前, kFaceMax 生效) ⇒ 仍保留明确的方向性明暗, 不会糊成一片。
//             ⚠️ 这一项**乘 sunUp**: 夜里没有直射就没有直射项, 云只剩环境项 (见 main 里
//             skyAmb 那段) —— 暗但看得见, 不是纯黑剪影。
//   kAmb    = 环境项倍数: 云底 (hf=0) 拿 0.35×kAmb, 云顶拿 1.0×kAmb。取 1.3 ⇒
//             云底 0.45×参考、云顶 1.3×参考 —— 云顶比云底亮, 符合实际云 (上表面受光)。
//             **不乘 sunUp** (夜里的环境光不需要太阳在地平线以上)。
//   kDistFade = 空气透视: 只对**远于 4 km** 的云段衰减 (见 main 里的 far), 天顶那段
//             (tEnter 1.4~3.6 km) 一点不暗。
// 一律 float 声明 (数据块按 float 传, 与 kCubeElev/kMissFade 同款)。
layout(constant_id = 0) const float kSteps = 64.0;
layout(constant_id = 1) const float kHistory = 0.55;
layout(constant_id = 2) const float kMs = 0.20;
layout(constant_id = 3) const float kDistFade = 1.0 / 6000.0;
layout(constant_id = 4) const float kAmb = 1.3;
layout(constant_id = 5) const float kFace = 42.0;
// 受光项乘积上限 (与 kFace/kMs/kAmb/kDistFade 同款: 走 VkSpecializationInfo,
// 见 vkrenderer.cpp cloudData[6]): 设计意图是「前向峰 2~4×参考, 侧向 ~2×参考」。
// 侧向乘积天然是 42*0.036 = 1.51, 所以上限取 3.0 正好只削正向前段, 不动侧向标定。
layout(constant_id = 6) const float kFaceMax = 3.0;

// ---- LUT 参考 texel 的坐标 (**与 sky_lut.frag / sky_comp.frag 必须逐字一致**) ----
// 目标是吸附到**真实 texel 中心**: LUT 高度随 lutscale 变 (16/32/64), 而采样器是 LINEAR,
// 固定写 0.7568 会落在两行 (甚至两列) 之间 ⇒ 采到的是插值值, LUT 内部那个「参考点逐位
// 透传」的性质就不成立 (压缩用的 ref 与这里采到的不是同一个数, 差几个百分点)。
// 三个 pass 各算一遍同样的式子 (各自的 uLut/uLutPrev 尺寸相同) ⇒ 坐标必然一致。
vec2 refTexel()
{
	ivec2 sz = textureSize(uLut, 0);
	float x = (floor(0.5 * float(sz.x) - 0.5) + 0.5) / float(sz.x);
	float y = (floor(0.7568 * float(sz.y) - 0.5) + 0.5) / float(sz.y);
	return vec2(x, y);
}

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
	// 进/出 slab 距离: **两端都要钳**。只钳 tExit 时, 天顶方向 (rd.y≈1) 的 tExit=3600 会小于
	// 低仰角方向算出的 tEnter 上限, dt 可能为负 ⇒ 步进反向。
	// v0.18.38: 上限从 60000 收到 **12000** —— 这是 RC2 (平视整片黑云幕) 的正解。
	// 原来 60 km 的上限意味着朝地平线看时 tEnter 一路到 14~60 km, 射线在 slab 里几乎是
	// **横着**穿过整层云, 在 coverage 0.58 下必然撞满云 ⇒ trans→0, 而云自身又只有
	// 一点点环境光 ⇒ 贴着地平线的整片天空变成不透明近黑的云幕 (用户看到的「仰视白 /
	// 平视黑」倒挂)。收到 12 km 之后, **无云带 = 0.46° ~ 6.7°**:
	//   上界 6.7°: tEnter = 1400/12000 触顶, 再低就 tEnter>=tExit ⇒ dt=0 ⇒ 一步走不到;
	//   下界 0.46°: rd.y <= 0.008 的早退 (约 0.46°)。
	// 这一带退回纯散射天空, 也就是天边那圈本来最该亮的地方。
	const float kMarchMax = 12000.0;
	float tEnter = hBase / rd.y;
	float tExit = (hBase + hSpan) / rd.y;
	tEnter = min(tEnter, kMarchMax);
	tExit = min(max(tExit, tEnter), kMarchMax);
	float coverage = pc.ctl.x;
	float shape = pc.ctl.y;
	// 消光尺度 0.06 → 0.025 /m。真实的层积云消光系数就在 0.01~0.03 /m 这一档;
	// 0.06 是上一轮为了让「云看起来厚」随手写的, 代价是直接项被 exp(-47) 彻底杀死。
	// 改小之后同样 3×260 m 的向阳步进最长 OD = 0.025*780 = 19.5 (原 47), 厚处仍然
	// 变暗 (beer 仍有真实梯度), 受光面拿到真正的 beer 值而不是恒 0。
	float density = 0.025;
	int n = max(int(kSteps + 0.5), 4);
	float dt = (tExit - tEnter) / float(n);
	float mu = dot(rd, sunDir);
	// 相位: 归一化的 HG 混双叶 (原代码乘 4π 只是为了"看得见", 现在以参考值为单位,
	// 乘 4π 会让近太阳处直接爆掉)。**这里不钳相位, 钳的是乘积** (见下面 faceK):
	// 钳 phN 会把侧向标定一起削掉, 前向/侧向的层次 (银边) 就没了。
	float phN = mix(hg(mu, 0.72), hg(mu, -0.28), 0.35);
	// 受光项系数 = kFace·phN, **封顶 kFaceMax**: 侧向 (mu=0) 天然 42*0.036 = 1.51,
	// 正向 (mu→1) 原本能到 71 ⇒ 峰值被钉在 3.0 ⇒ 前向/侧向仍留 ≈2.0 倍的银边,
	// 但绝对值落在 3.0*1.18 ≈ 3.5×参考 (设计意图: 前向峰 2~4×参考)。
	float faceK = min(kFace * phN, kFaceMax);
	// 太阳低于地平线 => 不给直射 (S5 夜间场景)
	float sunUp = clamp(sunDir.y * 6.0 + 0.15, 0.0, 1.0);
	// 帧内量级基准: **LUT 的参考 texel** (与 LUT 内部做范围压缩用的是同一个 texel, 坐标由
	// refTexel() 吸附到真实 texel 中心, 三处必须逐字一致; 见 sky_lut.frag)。
	vec3 refC = texture(uLut, refTexel()).rgb;
	// **环境项不乘 sunUp** (v0.18.40): 夜里的天空环境光并不需要太阳在地平线以上 —— LUT 里
	// 的 kSunFloor 填充已经编码了"入夜"这件事 (夜间 ref 仍是真实非零值)。而 sunUp 在
	// 假太阳估到地平线以下 (sunDir.y <= -0.025, 约 1.4°) 时**恰好是 0**, 旧写法会让
	// 环境项与直接项一起归零 ⇒ 厚云 lum=0、scat→0、trans→0 ⇒ 合成输出一坨**纯黑**的
	// 云剪影 (比"偏暗"更糟)。现在夜间内部只剩环境项: 深蓝灰, 不会黑也不会发光。
	vec3 skyAmb = refC * kAmb;
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
		// 直接项 (以参考值为单位): beer·powder 是单次散射, kMs 是多次散射地板;
		// **这一项继续乘 sunUp** (物理上正确: 没有直射太阳就没有直射项), 夜里它归零,
		// 于是夜间的云只剩上面的环境项 —— 暗但看得见, 不会黑。
		vec3 lum = refC * faceK * (beer * (0.55 + 0.75 * powder) + kMs) * sunUp +
		           skyAmb * (0.35 + 0.65 * hf);
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
	// ---- 空气透视 (RC2 第二道保险) ----
	// 只对**远于 4 km** 的云段衰减: 天顶 (tEnter 1.4 km) 与 45° (2.0 km) 的头顶云一点不暗;
	// 10° 仰角 (tEnter 8.0 km) 淡到 0.51, 7° (11.5 km) 淡到 0.29。
	// **淡向中性而不是淡向黑**: trans 被拉回 1 ⇒ 合成侧 `sky = cl.rgb + scatter*cl.a`
	// 里的散射项重新全额出现 ⇒ 远处云化成普通的天空颜色, 而不是把那片天空挖成黑洞。
	// 放在时域复用**之后**, 让写进 cloud[prv] 的是已经淡过的值, 历史与本帧口径一致。
	float far = max(tEnter - 4000.0, 0.0);
	float fade = exp(-far * kDistFade);
	scat *= fade;
	trans = mix(trans, 1.0, 1.0 - fade);
	oColor = vec4(max(scat, vec3(0.0)), trans);
}
