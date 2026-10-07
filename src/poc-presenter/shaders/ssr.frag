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
// v0.18.8 正解B (v0.18.7 实跑归因: A 平滑只治一半, 倒影仍碎成"不规则多边形拼图"):
//   根因 = 水面像素在 uDepth(520, 段16 **之前** 的快照) 里读到的是**河床三角面** ⇒ 差分出的
//   法线 = 河床的面法线, 一整块三角形一个法线 ⇒ 反射方向按三角形跳变 ⇒ 拼图。半径调大只是
//   把碎石糊成大石, 面间跳变永远在 —— 换数据源才治本。
//   uWdep (第5张镜像 = 461 段17 之后) 里是**真·水面深度** (段17 那笔 depth 只写不测的水体并回
//   写进去的), 水面像素法线/反射原点改用它: 判据 = 水深比河床更近 (zW > zPre) ⇒ 只在水面上切
//   换, 非水面自动保持原样; 461 被 UI 中途 ClearDS 清掉时判据不成立 ⇒ 整帧退回 520 = v0.18.7
//   行为, 天生自愈。**ray march 的层级仍用 uDepth** —— 射线从水面出发, 用水面深度当层级会
//   一出门就打在自己脚下的水面上, 一个也命中不了。
//
// v0.18.9 (v0.18.8 实跑归因: ①倒影"很多层堆叠" ②有倒影处水面变平面/流动波纹消失) ——
//   **段17 PS 17586 反汇编 + 逐 draw 绑定表把 585 的契约钉死了** (docs/analysis/
//   S1-pass6-ps-disasm.txt:418 + S4-extract-pass6.json:131390):
//     t0 = 585(反射层)  t1 = 588(场景色)  t2 = 349(R16G16 偏移)  t3 = 461(深度)  t4 = 339(遮罩)
//     `discard if 339 < 1e-4` ⇒ 段17 只画有水像素; out = **mix(585, 588@扭曲UV, w)**,
//     w = (339*-0.85+0.95) * depthW[0.1..0.95] * cb2.w ⇒ 水面像素 w≈0.1 ⇒ **585 占 ~90%**。
//   ⇒ **585 的契约 = 「一层反射色」, 层叠是段17 的活**。据此修两件事:
//   A 契约修正 (治①): 585 里只放**一种**反射 —— 不再把 cubemap(uBase) 和 SSR 用 fresnel 掺在
//     一起 (那样 585 同时躺着两层反射, 段17 一合成 = 多层堆叠), 也不再自算 fresnel (权重归段17)。
//     `ssr.strength` 语义改为 **SSR 替换比 0..1**: 0 = 585 原样 (游戏 cubemap = 原版, 一号对照),
//     1 = 纯 SSR; 没命中也没边缘延展时 refl = baseRGB ⇒ 再大也是原版兜底。
//   B 涟漪回注 (治②): 段16 那 16 个 draw 是按**水面涟漪法线**采 cubemap 写进 585 的 ⇒ uBase 里
//     除探针内容外还带着逐像素涟漪调制; 换掉 585 内容等于把涟漪一起换掉 ⇒ 反射区成了平面镜。
//     这里把 uBase 的**高频**亮度结构 (5tap 低频被除掉, 只剩涟漪那一档 ⇒ 不会把探针的山/天
//     重新印上来) 按比例乘回 SSR, 量由 `ssr.ripple` (p4.x) 控制, 0 = 关。
//
// 输入: uColor = 324 场景色镜像 (RGBA16F, 段16 前的快照) —— 反射源
//       uDepth = 520 深度快照  (D24, 取 depth aspect) —— 行进层级 + 非水面像素的法线源
//       uBase  = 585 段16 后镜像 (RGBA16F, 含水画面) —— 合成底色 (可缺)
//       uWdep  = 461 段17 后镜像 (D24, 真·水面深度) —— 水面像素的法线/原点 (可缺)
// 输出: 覆盖式写进出向镜像 (下帧特征B 整幅拷进 585)
layout(set = 0, binding = 0) uniform sampler2D uColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;
layout(set = 0, binding = 2) uniform sampler2D uBase;
layout(set = 0, binding = 3) uniform sampler2D uWdep; // v0.18.8: 缺时填色 view + p2.w=0

layout(push_constant) uniform PC
{
	vec4 p0; // x=tanHalfY  y=aspect  z=near  w=far
	vec4 p1; // x=mode(0 透传 / 1 SSR)  y=steps  z=strength(**SSR 替换比 0..1**)  w=march 最大距离 (**单位 = near**)
	vec4 p2; // x=width  y=height  z=rev(反向深度开关)  w=用段后水深(1=采 uWdep)
	vec4 p3; // x=smooth(法线差分邻域 px)  y=blur(0/1)  z=debug(0..5)  w=用底色(1=采 uBase)
	vec4 p4; // v0.18.9: x=ripple(涟漪回注量 0..1)  y/z/w 预留 (push constant 64B → 80B)
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
// v0.18.8: wdep=true 时改从 uWdep (段后水深) 取 —— 法线差分的 4 个邻点用**与中心同一个源**,
// 免得水面/河床两种几何混在一个叉积里 (那会在水边线拉出假棱)。
float dAt(vec2 uv, bool wdep)
{
	vec2 c = clamp(uv, vec2(0.0), vec2(1.0));
	if (!wdep)
		return texture(uDepth, c).r;
	return texture(uWdep, c).r;
}

vec3 PAt(vec2 uv, bool wdep)
{
	return viewPos(uv, dAt(uv, wdep));
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
	float dPreR = texture(uDepth, uv).r; // 520 段16 前快照 (raw, 可能是反向深度)
	float d = linD(dPreR);               // 反向深度先翻回标准口径

	// ---- v0.18.8 正解B: 段后水深判据 ----
	// 三条件: p2.w 说有水深镜像 + 水深本身有效 + 水深比河床更近 (viewZ 更大 = 离相机更近)。
	// 第三条是关键: 非水面像素上 461 与 520 同源同内容 (不成立), 水面像素上 461 = 水面 (成立)
	// ⇒ 只在水面切源; 461 若被 UI 中途 ClearDS 清成全 1.0 ⇒ 更远 ⇒ 不成立 ⇒ 整帧退回 520 =
	// v0.18.7 行为 —— 不会算出比上一版更坏的结果 (自愈)。**行进层级仍固定用 uDepth**, 见文件头。
	float dWr = (pc.p2.w > 0.5) ? texture(uWdep, uv).r : -1.0;
	bool useW = (pc.p2.w > 0.5) && dWr > 0.0 && dWr < 1.0 && (viewZ(dWr) > viewZ(dPreR));

	// debug=4: 段后水深灰度 (没水深时全黑) —— 验证第5张镜像的内容是不是"平滑的水面"
	// (若与 debug=3 (520=河床) 几乎一样 ⇒ 拷的时机/源不对, 先看日志 [wdep] 判据)
	if (pc.p3.z > 3.5 && pc.p3.z < 4.5)
	{
		oColor = vec4(vec3(dWr >= 0.0 ? linD(dWr) : 0.0), 1.0);
		return;
	}
	// debug=5: 水面像素判定图 (绿 = 用了段后水深, 背景 = 520 深度灰度) —— 验证判据命中范围
	if (pc.p3.z > 4.5)
	{
		oColor = useW ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(vec3(max(d, 0.0)), 1.0);
		return;
	}

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
	// 反射原点 + 法线的深度源: 水面像素用段后水深 (真水面), 其余用 520 (行进层级也是 520)
	vec3 P = viewPos(uv, useW ? dWr : dPreR);
	// 中心差分还原法线 (半径 R 可调, 见文件头 A): v0.18.8 起水面走 uWdep ⇒ 差出来的是平的水面,
	// 河床三角面法线只影响非水面像素 (那里本来就该是地形法线)。
	vec3 Pl = PAt(uv - ox, useW);
	vec3 Pr = PAt(uv + ox, useW);
	vec3 Pu = PAt(uv - oy, useW);
	vec3 Pd = PAt(uv + oy, useW);
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

	// 合成底色: 有 uBase (585 段16 后 = 游戏自己的 cubemap 反射层) 就用它, 否则退回 uColor
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

	// ---- v0.18.9 B: 涟漪回注 (把段16 反射里的高频涟漪调制乘回 SSR) ----
	// 低频用 5tap 除掉 ⇒ 只保留几像素级的结构 (涟漪), 探针自己的山/天是低频不会被印上来。
	if (pc.p3.w > 0.5 && pc.p4.x > 0.001)
	{
		vec2 kx = vec2(px.x * 2.0, 0.0);
		vec2 ky = vec2(0.0, px.y * 2.0);
		vec3 cb = baseRGB * 0.4 + texture(uBase, uv + kx).rgb * 0.15 +
		          texture(uBase, uv - kx).rgb * 0.15 + texture(uBase, uv + ky).rgb * 0.15 +
		          texture(uBase, uv - ky).rgb * 0.15;
		float l0 = dot(baseRGB, vec3(0.2126, 0.7152, 0.0722));
		float lb = dot(cb, vec3(0.2126, 0.7152, 0.0722));
		float kf = clamp(l0 / max(lb, 1e-3), 0.4, 2.5); // ≈1 的高频比 (探针暗处也不会炸)
		refl *= mix(1.0, kf, clamp(pc.p4.x, 0.0, 1.0));
	}
	if (!hit && lastUV == uv)
		refl = baseRGB; // 一跳都没进屏 => 保持底色 (= 原版 cubemap 兜底, 覆盖掉上面的回注)

	// ---- v0.18.9 A: 585 = **纯反射层** (段17 契约, 见文件头) ----
	// 层叠归段17: 我方只给"这一层是什么", 不再自算 fresnel 也不再把 cubemap 掺进合成里。
	float k = clamp(pc.p1.z, 0.0, 1.0); // ssr.strength = SSR 替换比 (0 = 原版 cubemap)
	oColor = vec4(mix(baseRGB, refl, k), base.a);
}
