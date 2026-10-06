#version 450
// SSR v1 采样 (docs/05 D4, Step 3) —— 视图空间线性 ray march + 二分收紧
//
// R4 定案 (docs/05 §6): **只用 inv(投影)** —— 行进全程在视图空间做, 只要能由
// (fov / 宽高比 / near / far) 反推出深度->视图 Z 的映射即可, 不需要 view 矩阵。
// 这几个参数由 poc-presenter.ini 的 ssr.fov / ssr.near / ssr.far 传入 (push constant),
// 与实际投影不一致时反射比例会偏 —— 观感对不上就改 ini, 不必改代码。
//
// v0.18.7 两处观感修补 (真机判读: 倒影"完全破碎" + 水面除了倒影仍几乎透明无色):
//   A 平滑: 法线改**中心差分**且邻域半径 ssr.smooth (px) 可调 —— 水面像素的深度其实是
//     水底/河床 (水体不写深度), 逐像素差分得到的是碎石法线 ⇒ 每像素反射方向不同 ⇒ 破碎;
//     半径放大到宏观尺度后压成整体坡度。再加 ssr.blur 的 5tap 空间平滑兜住命中抖动。
//   B 水色: 合成底色从 uColor (324 = 段16 **之前**, 没画水) 换成 uBase (585 段16 **之后**,
//     含水) —— fresnel 正对相机时权重只有 ~0.08, 92% 是底色, 底色没水 ⇒ 水看起来仍透明。
//     ssr.base585 关着/建不出时 p3.w=0 ⇒ 退回采 uColor (v0.18.6 行为)。
//
// 输入: uColor = 324 场景色镜像 (RGBA16F, 段16 前的快照) —— 反射源
//       uDepth = 520 深度快照  (D24, 取 depth aspect)
//       uBase  = 585 段16 后镜像 (RGBA16F, 含水画面) —— 合成底色 (可缺)
// 输出: 覆盖式写进出向镜像 (下帧特征B 整幅拷进 585)
layout(set = 0, binding = 0) uniform sampler2D uColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;
layout(set = 0, binding = 2) uniform sampler2D uBase;

layout(push_constant) uniform PC
{
	vec4 p0; // x=tanHalfY  y=aspect  z=near  w=far
	vec4 p1; // x=mode(0 透传 / 1 SSR)  y=steps  z=strength  w=march 最大距离 (**单位 = near**)
	vec4 p2; // x=width  y=height  z=rev(反向深度开关)  w 备用
	vec4 p3; // x=smooth(法线差分邻域 px)  y=blur(0/1)  z=debug(0/1/2/3)  w=用底色(1=采 uBase)
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

// ssr.rev=1 (游戏用反向深度: 近平面->1 远平面->0) => 先翻回标准口径再算
float linD(float d)
{
	return pc.p2.z > 0.5 ? 1.0 - d : d;
}

// 深度 [0,1] -> 视图空间 Z (负值, 相机看 -Z; D3D 正投影: 近平面->0 远平面->1)
float viewZ(float d)
{
	d = linD(d);
	float n = pc.p0.z;
	float f = pc.p0.w;
	float A = f / (n - f);
	float B = f * n / (n - f);
	return B / -(d + A);
}

vec3 viewPos(vec2 uv, float d)
{
	float z = viewZ(d);
	float tx = pc.p0.x * pc.p0.y; // tanHalfY * aspect = tanHalfX
	float ty = pc.p0.x;
	// 屏幕顶部 (v=0) 对应视图 +Y (D3D 口径 y 向上), 所以 v 要翻一次
	return vec3((2.0 * uv.x - 1.0) * tx * -z, (1.0 - 2.0 * uv.y) * ty * -z, z);
}

// 采一格深度并反推视图空间位置 (uv 先 clamp 到屏内 => 大邻域取样不会出界)
vec3 PAt(vec2 uv)
{
	return viewPos(uv, texture(uDepth, clamp(uv, vec2(0.0), vec2(1.0))).r);
}

vec2 projectUV(vec3 q)
{
	float w = -q.z;
	float tx = pc.p0.x * pc.p0.y;
	float ty = pc.p0.x;
	float u = (q.x / (tx * w)) * 0.5 + 0.5;
	float v = 1.0 - ((q.y / (ty * w)) * 0.5 + 0.5);
	return vec2(u, v);
}

void main()
{
	vec2 uv = vUV;
	vec4 base = texture(uColor, uv);
	float d = linD(texture(uDepth, uv).r); // 反向深度先翻回标准口径

	// debug=3: 深度可视化 (灰度, 越白越近) —— 一眼看清水面像素读到的到底是河床还是水面
	if (pc.p3.z > 2.5)
	{
		oColor = vec4(vec3(d), 1.0);
		return;
	}

	// mode=0 (或深度缺失: 深度会整片是 0/1 之外的垃圾) => 纯透传, 与 2d 出向拷贝等价
	if (pc.p1.x < 0.5 || d <= 0.0 || d >= 1.0)
	{
		if (pc.p3.z > 1.5)
		{
			oColor = vec4(0.5); // debug=2: 这格没有可用深度 = 灰
			return;
		}
		oColor = base;
		return;
	}

	vec2 px = 1.0 / pc.p2.xy;
	float R = clamp(pc.p3.x, 1.0, 16.0); // ssr.smooth 邻域半径 (px)
	vec2 ox = vec2(px.x * R, 0.0);
	vec2 oy = vec2(0.0, px.y * R);
	vec3 P = viewPos(uv, d);
	// 中心差分还原法线 (半径 R 可调, 见文件头 A): 水面像素深度 = 水底 ⇒ 小半径会拿到碎石法线
	vec3 Pl = PAt(uv - ox);
	vec3 Pr = PAt(uv + ox);
	vec3 Pu = PAt(uv - oy);
	vec3 Pd = PAt(uv + oy);
	vec3 cx = cross(Pr - Pl, Pd - Pu);
	// 平坦/退化时叉积接近 0 => 兜底一个朝相机的法线 (视图空间 +Z), 免得 normalize 出 NaN
	vec3 N = (dot(cx, cx) > 1e-20) ? normalize(cx) : vec3(0.0, 0.0, 1.0);
	if (dot(N, P) > 0.0)
		N = -N;

	vec3 V = normalize(P); // 相机 -> 表面
	vec3 Rf = reflect(V, N); // 反射方向 (离开表面)

	// 线性步进 + 末段三次二分收紧 (docs/05 D4: 固定步数, 不做 Hi-Z)
	int steps = int(pc.p1.y);
	float maxT = pc.p1.w;
	float stepLen = maxT / float(steps);
	float t = stepLen * 0.5;
	vec2 lastUV = uv;
	bool hit = false;
	vec2 hitUV = uv;

	for (int i = 0; i < steps; ++i)
	{
		float w = -0.0;
		vec3 Q = P + Rf * t;
		w = -Q.z;
		if (w <= pc.p0.z) // 穿回近平面之前就该停
			break;
		vec2 uq = projectUV(Q);
		if (uq.x < 0.0 || uq.x > 1.0 || uq.y < 0.0 || uq.y > 1.0)
			break; // 出屏 => 射线这一跳已在屏外, 用 lastUV 做边缘延展
		lastUV = uq;
		float sz = viewZ(texture(uDepth, uq).r);
		if (Q.z < sz)
		{
			// 已经走到场景后面 => 在 [t-stepLen, t] 里二分三次收紧
			float lo = t - stepLen;
			float hi = t;
			for (int k = 0; k < 3; ++k)
			{
				float mid = (lo + hi) * 0.5;
				vec3 M = P + Rf * mid;
				vec2 um = projectUV(M);
				if (M.z < viewZ(texture(uDepth, um).r))
					hi = mid;
				else
					lo = mid;
			}
			hitUV = projectUV(P + Rf * hi);
			hit = true;
			break;
		}
		t += stepLen;
	}

	// ssr.debug 诊断画面 (1=重建法线, 2=命中绿/未命中红) —— 判读"倒影破碎"是法线问题还是命中问题
	if (pc.p3.z > 0.5)
	{
		if (pc.p3.z < 1.5)
		{
			oColor = vec4(N * 0.5 + 0.5, 1.0);
			return;
		}
		oColor = hit ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);
		return;
	}

	// 合成底色: 有 uBase (585 段16 后 = 含水画面) 就用它, 否则退回 uColor (v0.18.6 行为)
	vec3 baseRGB = pc.p3.w > 0.5 ? texture(uBase, uv).rgb : base.rgb;

	// 命中 => 采那一处的场景色; 未命中 => 屏幕边缘延展 (射线最后一个在屏位置)
	vec2 ruv = clamp(hit ? hitUV : lastUV, vec2(0.0), vec2(1.0));
	vec3 refl;
	if (pc.p3.y > 0.5)
	{
		// ssr.blur: 按与法线同一个半径 R 做 5tap 十字平均 —— 命中判定在深度跳变处会一跳一断,
		// 平均后碎片连成片 (只糊反射色, 不糊底色, 全局清晰度不受影响)
		vec2 bx = vec2(px.x * R, 0.0);
		vec2 by = vec2(0.0, px.y * R);
		vec3 acc = texture(uColor, ruv).rgb * 4.0;
		acc += texture(uColor, clamp(ruv + bx, vec2(0.0), vec2(1.0))).rgb;
		acc += texture(uColor, clamp(ruv - bx, vec2(0.0), vec2(1.0))).rgb;
		acc += texture(uColor, clamp(ruv + by, vec2(0.0), vec2(1.0))).rgb;
		acc += texture(uColor, clamp(ruv - by, vec2(0.0), vec2(1.0))).rgb;
		refl = acc * 0.125;
	}
	else
		refl = texture(uColor, ruv).rgb;
	if (!hit && lastUV == uv)
		refl = baseRGB; // 一跳都没进屏 => 保持底色

	// 合成: Schlick fresnel (F0=0.02) x strength —— 正对相机的面反射弱, 掠射角的水面反射强
	float ndv = max(dot(N, -V), 0.0);
	float F = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
	float wgt = clamp(F * pc.p1.z * 4.0, 0.0, 1.0);
	if (!hit)
		wgt *= 0.5; // 未命中只给一半强度, 让"没打中"看得出来 (v1 诊断用)

	oColor = vec4(mix(baseRGB, refl, wgt), base.a);
}
