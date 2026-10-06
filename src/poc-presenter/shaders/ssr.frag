#version 450
// SSR v1 采样 (docs/05 D4, Step 3) —— 视图空间线性 ray march + 二分收紧
//
// R4 定案 (docs/05 §6): **只用 inv(投影)** —— 行进全程在视图空间做, 只要能由
// (fov / 宽高比 / near / far) 反推出深度->视图 Z 的映射即可, 不需要 view 矩阵。
// 这几个参数由 poc-presenter.ini 的 ssr.fov / ssr.near / ssr.far 传入 (push constant),
// 与实际投影不一致时反射比例会偏 —— 观感对不上就改 ini, 不必改代码。
//
// 输入: uColor = 324 场景色镜像 (RGBA16F, 段16 前的快照)
//       uDepth = 520 深度快照  (D24, 取 depth aspect)
// 输出: 覆盖式写进出向镜像 (下帧特征B 整幅拷进 585)
layout(set = 0, binding = 0) uniform sampler2D uColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;

layout(push_constant) uniform PC
{
	vec4 p0; // x=tanHalfY  y=aspect  z=near  w=far
	vec4 p1; // x=mode(0 透传 / 1 SSR)  y=steps  z=strength  w=march 最大距离 (**单位 = near**)
	vec4 p2; // x=width  y=height  z=rev(反向深度开关)  w 备用
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

	// mode=0 (或深度缺失: 深度会整片是 0/1 之外的垃圾) => 纯透传, 与 2d 出向拷贝等价
	if (pc.p1.x < 0.5 || d <= 0.0 || d >= 1.0)
	{
		oColor = base;
		return;
	}

	vec2 px = 1.0 / pc.p2.xy;
	vec3 P = viewPos(uv, d);
	// 逐像素差分还原法线 (邻域取右边与下边, 再统一翻向背离相机一侧)
	vec3 Pr = viewPos(uv + vec2(px.x, 0.0), texture(uDepth, uv + vec2(px.x, 0.0)).r);
	vec3 Pd = viewPos(uv + vec2(0.0, px.y), texture(uDepth, uv + vec2(0.0, px.y)).r);
	vec3 N = normalize(cross(Pr - P, Pd - P));
	if (dot(N, P) > 0.0)
		N = -N;

	vec3 V = normalize(P); // 相机 -> 表面
	vec3 R = reflect(V, N); // 反射方向 (离开表面)

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
		vec3 Q = P + R * t;
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
				vec3 M = P + R * mid;
				vec2 um = projectUV(M);
				if (M.z < viewZ(texture(uDepth, um).r))
					hi = mid;
				else
					lo = mid;
			}
			hitUV = projectUV(P + R * hi);
			hit = true;
			break;
		}
		t += stepLen;
	}

	// 命中 => 采那一处的场景色; 未命中 => 屏幕边缘延展 (射线最后一个在屏位置)
	vec3 refl = texture(uColor, clamp(hit ? hitUV : lastUV, vec2(0.0), vec2(1.0))).rgb;
	if (!hit && lastUV == uv)
		refl = base.rgb; // 一跳都没进屏 => 保持底色

	// 合成: Schlick fresnel (F0=0.02) x strength —— 正对相机的面反射弱, 掠射角的水面反射强
	float ndv = max(dot(N, -V), 0.0);
	float F = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
	float wgt = clamp(F * pc.p1.z * 4.0, 0.0, 1.0);
	if (!hit)
		wgt *= 0.5; // 未命中只给一半强度, 让"没打中"看得出来 (v1 诊断用)

	oColor = vec4(mix(base.rgb, refl, wgt), base.a);
}
