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
//     1 = 纯 SSR; 未命中 (v0.18.11 起**一律**) 与贴屏边的命中都回 baseRGB ⇒ 再大也是原版兜底。
//   B 涟漪回注 (治②): 段16 那 16 个 draw 是按**水面涟漪法线**采 cubemap 写进 585 的 ⇒ uBase 里
//     除探针内容外还带着逐像素涟漪调制; 换掉 585 内容等于把涟漪一起换掉 ⇒ 反射区成了平面镜。
//     这里把 uBase 的**高频**亮度结构 (5tap 低频被除掉, 只剩涟漪那一档 ⇒ 不会把探针的山/天
//     重新印上来) 按比例乘回 SSR, 量由 `ssr.ripple` (p4.x) 控制, 0 = 关。
//
// v0.18.10 (v0.18.9 实跑归因: ①回注出来的波纹"非常细小, 不如原本明显" ②倒影"仍然多重堆叠") ——
//   先把段17 的权重从反汇编里逐条算死 (docs/analysis/S1-pass6-ps-disasm.txt:460-481):
//     r1.z = 扭曲UV∈[0,1] 且 588.a==1; 合法时:
//       cb2.z==0 ⇒ w = (0.95 - 0.85·mask339) · depthW[0.1..0.95] · cb2.w   (339≈1 ⇒ w ≤ 0.095)
//       cb2.z!=0 ⇒ w = 0.95 (只给 5% 我们这层 —— 真机上反射看得见 ⇒ 走的必是上一支 ⇒ w≤0.095)
//   ⇒ 最终水面 ≈ **90% 我们写的 585 + 9.5% 扭曲场景色**, 再叠段18 的水体混合。这三样是游戏自己的,
//     换不掉; 能换的只有 585 这一层的内容。段18 首笔绑定 (ev39585: t0=19520 / t1=520 / t2=18483)
//     证明它**不读 585**, `321→324` 拷贝 = ev39225 在段16 **之前** ⇒ 也没有跨帧递归 —— 所以
//     "多层堆叠"只可能出在①我们这层自己 (回注印上来的轮廓 / 5tap 叠影) 或 ②游戏那 9.5%+水体,
//     两者靠**只动一个旋钮的隔离试验**分开 (docs/02 §14.25.1): strength=0 → ripple=0 → blur=0。
//   B 回注的两个缺陷就地修 (②仍是待验证项, 隔离试验说了算):
//     ①带宽写死 2px ⇒ 只抓得住像素级噪点 (现象 = 波纹细小) ⇒ 改成 `ssr.ripplesz` (p4.y, 1..16px);
//     ②只做亮度调制 ⇒ 原版涟漪本质是**反射方向被水面法线扰动**, 亮度做得再准也不像波纹
//       ⇒ 新增 `ssr.ripplemode=1` **位移式**: 采样前拖 ruv, 梯度取「1px 梯度 − Rb 梯度」——
//       阶跃轮廓两支梯度相近 ⇒ 相减归 0 (不把探针的山/天轮廓印上来); 波纹波长 ≈ 2·Rb 时两支
//       差最大 ⇒ 拖动跟着波纹走, 且天然只对 Rb 这一档波长起效 (调 ripplesz = 调"认哪档波")。
//     debug=6 回注可视化 / debug=7 uBase 原样 (看原版涟漪长啥样、波长多少 ⇒ 定 ripplesz 的依据)。
//     debug=8 v0.18.15 单位回补量 (Reinhard 灰度) —— 直接读出「ssr.v1det 能往哪加、能加多少」,
//              不乘 det ⇒ v1det=0 也能拍, 因此不受跨图差分的机位/光照/水面动画噪音影响。
//              没深度 = 输出 0 (黑 = 没得补), 与 6/7 的「灰 0.5 = 没深度」刻意不同, 见下方分支。
//
// v0.18.11 (v0.18.10 实跑截图 docs/analysis/Screenshot_SSR debug.png: 「扇形范围内是错误的倒影,
//   屏幕两边一小部分水纹正常」) —— 把图和代码对上, 那个扇形是 **hit/miss 的结构**, 不是 bug 面积:
//   · 远处水面 (扇形上半带): 反射方向近乎水平 ⇒ 行进还没出屏就先撞到对岸悬崖 ⇒ **hit** ⇒ 是正常镜像;
//   · 近处水面 (扇形下半): 反射角 = atan(相机高 / 水距) 大 ⇒ 一两跳就飞出屏幕**顶部** ⇒ **miss**
//     ⇒ v0.18.10 拿 `lastUV` (射线最后一个在屏位置, 约等于**屏幕顶边**) 去采 uColor, 采到的是岸边/
//     树的**原位画面** —— 它没经过镜像关系, 整块贴进水里 ⇒ 就是"错位的倒影 / 同一棵树两份"的重影;
//   · 屏幕左右窄条: 第一跳就在屏外 ⇒ `lastUV == uv` ⇒ 走的本来就是原版层兜底 ⇒ 才有"两边水纹正常"
//     (这两条恰恰证明**回退到原版层是对的方向**, 只是 v0.18.10 只回退了这一种情况)。
//   ⇒ 修法 **未命中一律回 baseRGB (原版层)**: 屏幕里根本看不到的内容本来就该交给 cubemap,
//     "镜像点已出屏"时的边缘延展贴的是未镜像的原图, 没有物理意义。门 `ssr.edge` (p4.w):
//     0 = 新默认 (回原版层) / 1 = 老的边缘延展 (A/B 对照)。
//   附带 **hit 但 hitUV 贴屏幕边 (4%) ⇒ 淡回 baseRGB**: 扇形边界上的像素正是"再走一跳出屏就变 miss"
//     那批, 它们的 hitUV 就落在屏幕边附近, 淡出之后边界两边都是反射 (锐 SSR <-> 原版层), 不再出现
//     "镜像图 硬接 未镜像贴图" 那条接缝。
//
// v0.18.11 实跑三张图 (docs/analysis/Screenshot_ssr.edge=0.png / .edge=1.png / .debug=2.png)
//   **坐实了上面的归因**: debug=2 的红(未命中)/绿(命中)分界与 v0.18.10 那块扇形同构 (绿 = 贴岸那条
//   带 + 人下方一条竖带, 红 = 近处整片水面); edge=1 复现错位倒影 (整片水被贴成屏顶画面, 直边都在);
//   edge=0 明显正常。用户回: 「只有 ssr.edge=0 效果较好」, 剩三处归 v0.18.12:
//   ① **人物身体一圈区域 = 前景遮挡假命中**: 我们的射线从原点往**深处**走, 本来就碰不到站在它
//      **前面**的人/石头; 但深度图是高度场, 轮廓处会**突然变浅**, `Q.z < sz` 一看「射线已经落在场景
//      后面」就判命中 ⇒ 紧挨轮廓那一圈像素把身体颜色采进来糊在水上 (debug=2 里人物外圈那道绿边就是它)。
//      ⇒ 命中多认一道 **`sz <= P.z * 0.98` = 反射目标不许比原点浅**; 人/石头正下方那条**合法**反射带
//      不受影响 (那里的目标确实比原点深)。
//   ② **倒影质量差 (块状/阶梯)** = 二分只有 3 次, 32 步时命中点精度 = 156/8 ≈ 19.5 单位 ⇒ 一跳一跳
//      的色块。⇒ **二分 3 → 5 次** (精度 156/32 ≈ 4.9, 只多 2 次深度采样)。更细的还能用 ini
//      `ssr.steps=64` / `ssr.blur=0` 调, 不用重编。
//   ③ **倒影区域水波较小** = 位移幅度写死 `6.0 * px`, 与波长脱钩 ⇒ 改成随 ripplesz 缩放
//      (`clamp(1.5 * Rb, 6, 16)` px; 默认 ripplesz=4 ⇒ 6px, **与原来逐位一致**), ripplesz 调大时
//      波长与幅度一起长大; 亮度那一半 (sparkle 回来没) 走 mode0, 见 docs/02 §14.28。
//
// v0.18.12 实跑 (2026-10-07, 四张图 docs/analysis/Screenshot_v0.18.12*.png) ⇒ v0.18.13 归因:
//   用户原话「人物周围一圈还在」⇒ **上面①那条「前景遮挡假命中」的归因是错的**: 守卫
//   `sz <= P.z * 0.98` 没治好它 (守卫本身保留, 它治的是另一类越界)。做像素级测量把成因钉死 ——
//   判据取**与机位无关的局部量**: 「1..24px 的绿色短条 + 紧邻一侧有 >=25px 的物体剪影」,
//   扫 y=400..1060, 三张 debug=2 图 (fov65/steps32、只改 fov=58.7155、只改 steps=128) 结果:
//     环宽**中位数一律 4px**, 直方图主峰 33/46/36 全在 4px, 坐标全落在人物身上 (y=510..550)。
//   4px = `ssr.smooth` = 法线中心差分的邻域半径, 且与 fov/steps 完全无关 ⇒ 归因定死:
//   **差分的邻点跨过了物体剪影** —— 中心是水面 (uWdep), 邻点却踩在人/石头上 (那里 461 与 520
//   同源 ⇒ 判不成水), 3D 位置突跳 ⇒ 叉积出来的是乱法线 ⇒ 反射方向被甩进屏内 ⇒ **平白多出
//   一次 hit** ⇒ 采到对岸亮岩 = 默认图里那圈白亮边 (debug=2 里同一批像素 = 那圈绿边)。这一条
//   同时解释了「只在物体与水交界出现」「宽度恒等于 ssr.smooth」「fov/steps 怎么调都不动」。
//   ⇒ 修法 **轮廓守卫**: 4 个差分邻点逐个过 `nbWater` (与 useW 同款三条件), 单侧越界退成
//     **单侧差分** (水面是平的, 单侧照样给出正确平面法线 ⇒ 这圈像素会和周围一样 miss ⇒ 亮边
//     自然消失), 两侧都越界落回原有退化兜底 (叉积为 0 ⇒ N = 视图 +Z ⇒ 射线回头 ⇒ miss ⇒ 回
//     原版层)。邻点全是水面时与 v0.18.12 **逐位一致**; `!useW` 短路 ⇒ 非水面像素连 nbWater
//     都不调, 也逐位不变; **无新 ini 键**。为什么不用深度差阈值: 前景礁石只比水面高几十厘米、
//     深度差根本抓不住它, 而「461 与 520 同源 ⇒ 不是水」在人/石头/岸上一律成立 —— 那就是
//     useW 自己已经在跑的判据, 零参数零调优。判读模板 docs/02 §14.30, 归因实测 §14.28.1。
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
	vec4 p3; // x=smooth(法线差分邻域 px)  y=blur(0/1)  z=debug(0..8)  w=用底色(1=采 uBase)
	vec4 p4; // v0.18.9: x=ripple(涟漪回注量 0..1)  y=ripplesz(回注带宽 px 1..16)
	         // v0.18.10: z=ripplemode(0=亮度调制 / 1=位移扭曲)
	         // v0.18.11: w=edge(0=未命中回原版层(默认) / 1=边缘延展) (push constant 64B → 80B)
	vec4 p5; // v0.18.14: x=ripk(输入端软限幅阈值, 线性亮度, 0=关)  y=ripamp(位移幅度 px, 0=自动)
	         // v0.18.14: z=ripgain(梯度增益, 0=自动 =10)  w=保留 (push constant 80B → 96B)
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

// v0.18.13: 这一格到底是不是**水面** (与 useW 同款三条件, 逐点现算) —— 法线差分的邻点过滤用。
// 为什么不用「深度差阈值」: 前景礁石只比水面高几十厘米, 深度几乎一样, 阈值根本抓不住;
// 而 461(段后水深) 与 520(段16前) 在**非水面**像素上同源同内容 ⇒ `viewZ(461) > viewZ(520)`
// 在人/石头/岸上一律不成立 ⇒ 这正是游戏自己的水面判据, 零参数、零调优。
bool nbWater(vec2 u)
{
	float a = texture(uWdep, u).r;
	if (!(a > 0.0 && a < 1.0))
		return false;
	return viewZ(a) > viewZ(texture(uDepth, u).r);
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
	if (pc.p3.z > 4.5 && pc.p3.z < 5.5)
	{
		oColor = useW ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(vec3(max(d, 0.0)), 1.0);
		return;
	}

	// debug=3: 深度可视化 (灰度, 越白越近) —— 一眼看清水面像素读到的到底是河床还是水面
	if (pc.p3.z > 2.5 && pc.p3.z < 3.5)
	{
		oColor = vec4(vec3(d), 1.0);
		return;
	}

	// mode=0 (或深度缺失: 深度会整片是 0/1 之外的垃圾) => 纯透传, 与 2d 出向拷贝等价
	if (pc.p1.x < 0.5 || d <= 0.0 || d >= 1.0)
	{
		if (pc.p3.z > 1.5) // 2..8 (3/4/5 已在上面返回): 没可用深度 = 灰; 1 (法线) 照旧走透传
		{
			// debug=8 输出 0 而不是 0.5: 「没深度就没得补」, 反解成 l 就该是 0。留 0.5 会让天空
			// 反解成 l=1.0, 读数器把整片天空当饱和热点 (热力图全 # / meanL 被撑爆)。
			oColor = pc.p3.z > 7.5 ? vec4(0.0) : vec4(0.5); // 2/6/7 = 灰(无深度); 8 = 黑(无可补)
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
	// ---- v0.18.13 轮廓守卫: 水面法线只许由「同样是水面」的邻点差出来 ----
	// 实测归因 (docs/02 §14.28.1): 用户报的「人物身体一圈」= 贴着剪影的 **4px 亮边**, 三张实跑图
	// (fov/steps 三种旋钮) 环宽中位数全是 4px = `ssr.smooth` 的差分半径 ⇒ 与旋钮无关, 是差分跨了轮廓。
	// 成因: 中心是水面, 邻点却落在**前景物体**上 (那里 461 与 520 同源, 不是水) ⇒ 3D 位置突跳 ⇒
	//       叉积出来的法线是乱的 ⇒ 反射方向被甩进屏内 ⇒ 平白多出一次 hit ⇒ 采到对岸的亮岩 ⇒ 白亮边;
	//       debug=2 里那圈绿边就是同一批像素的 hit 标记。
	// 守卫: 逐邻点过一遍 nbWater; 单侧越界就退成**单侧差分** (水面是平的, 单侧照样给出正确平面法线)
	//       ⇒ 这圈像素会和周围一样 miss ⇒ 亮边自然消失; 两侧都越界则落到下面原有的退化兜底
	//       (叉积为 0 ⇒ N = 视图 +Z ⇒ 射线回头穿回近平面 ⇒ miss ⇒ 回原版层)。
	// 邻点全是水面时 ⇒ 与 v0.18.12 **逐位一致**, 只有轮廓上那几排像素会变。
	bool okL = !useW || nbWater(uv - ox);
	bool okR = !useW || nbWater(uv + ox);
	bool okU = !useW || nbWater(uv - oy);
	bool okD = !useW || nbWater(uv + oy);
	vec3 vx = (okL && okR) ? (Pr - Pl) : (okR ? (Pr - P) : (okL ? (P - Pl) : vec3(0.0)));
	vec3 vy = (okU && okD) ? (Pd - Pu) : (okD ? (Pd - P) : (okU ? (P - Pu) : vec3(0.0)));
	cx = cross(vx, vy);
	// 平坦/退化时叉积接近 0 => 兜底一个朝相机的法线 (视图空间 +Z), 免得 normalize 出 NaN
	vec3 N = (dot(cx, cx) > 1e-20) ? normalize(cx) : vec3(0.0, 0.0, 1.0);
	if (dot(N, P) > 0.0)
		N = -N;

	vec3 V = normalize(P); // 相机 -> 表面
	vec3 Rf = reflect(V, N); // 反射方向 (离开表面)

	// 线性步进 + 末段五次二分收紧 (docs/05 D4: 固定步数, 不做 Hi-Z)
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
		// v0.18.12 前景遮挡守卫: 反射目标必须**不浅于射线原点** (2% 容差)。
		// 射线从 P 出发往深处走, 碰不到站在 P **前面**的东西 (站在水里的人/石头); 但深度图是高度场,
		// 轮廓处会突然变浅, 单看 `Q.z < sz` 会把"射线落在它后面"当成命中 ⇒ 身体轮廓被糊进水里
		// (用户报的「人物身体一圈区域」, debug=2 里人物外圈那道绿边就是它)。
		// 人/石头**正下方**那条合法反射带不受影响: 那里的目标确实比原点深。
		if (Q.z < sz && sz <= P.z * 0.98)
		{
			// 已经走到场景后面 => 在 [t-stepLen, t] 里二分五次收紧 (v0.18.12: 3 -> 5, 治"倒影块状")
			float lo = t - stepLen;
			float hi = t;
			for (int k = 0; k < 5; ++k)
			{
				float mid = (lo + hi) * 0.5;
				vec3 M = P + Rf * mid;
				vec2 um = projectUV(M);
				float szm = viewZ(texture(uDepth, um).r);
				// 守卫在二分里同样生效 (否则会往"轮廓变浅"那一侧收敛)
				if (M.z < szm && szm <= P.z * 0.98)
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
	// (v0.18.10: 收成 [0.5,2.5) 区间, 否则 6/7 会在这里被提前 return 掉)
	if (pc.p3.z > 0.5 && pc.p3.z < 2.5)
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

	// 命中 => 采那一处的场景色; 未命中 => 先拿 lastUV (射线最后一个在屏位置) 兜着, 到底用不用它
	// 由下面的 v0.18.11 回退按 ssr.edge 决定 (默认 0 = 回原版层, 不用这个位置)
	vec2 ruv = clamp(hit ? hitUV : lastUV, vec2(0.0), vec2(1.0));

	// ---- v0.18.9/v0.18.10 B: 涟漪回注的**场**先算 (位移式要赶在采样之前, 因为它改 ruv) ----
	float ripA = clamp(pc.p4.x, 0.0, 1.0); // ssr.ripple   回注量 0..1
	float Rb = clamp(pc.p4.y, 1.0, 16.0);  // ssr.ripplesz 带宽 (px)
	float kf = 1.0; // mode0 亮度回注系数 (debug=6 只在 ripplemode=0 时画它; mode=1 画下面的 ripd)
	vec2 ripd = vec2(0.0);                 // mode1 位移量 (uv 单位, 直接加到 ruv)
	float ripAmp = 6.0;                    // mode1 位移幅度上限 (px) — v0.18.12 起随 ripplesz 缩放
	if (pc.p3.w > 0.5 && ripA > 0.001)
	{
		vec2 kx = vec2(px.x * Rb, 0.0);
		vec2 ky = vec2(0.0, px.y * Rb);
		if (pc.p4.z < 0.5)
		{
			// mode0 = v0.18.9 老路: 中心/宽带亮度比, 低频被 5tap 除掉 ⇒ 只剩涟漪那一档
			vec3 cb = baseRGB * 0.4 + texture(uBase, uv + kx).rgb * 0.15 +
			          texture(uBase, uv - kx).rgb * 0.15 + texture(uBase, uv + ky).rgb * 0.15 +
			          texture(uBase, uv - ky).rgb * 0.15;
			float l0 = dot(baseRGB, vec3(0.2126, 0.7152, 0.0722));
			float lb = dot(cb, vec3(0.2126, 0.7152, 0.0722));
			kf = clamp(l0 / max(lb, 1e-3), 0.4, 2.5); // ≈1 的高频比 (探针暗处也不会炸)
		}
		else
		{
			// mode1 = 位移式: 原版涟漪 = 反射方向被水面法线扰动 ⇒ 拖 ruv 才像波纹。
			// 梯度取「1px 梯度 − Rb 梯度」: 阶跃轮廓两支梯度相近 ⇒ 相减归 0, 不把探针的山/天
			// 轮廓印上来; 响应在波长 ≈ 4·Rb 处最大 (§14.30.3 实测更正, 旧注释写的 2·Rb 恰是 null)。
			// 但响应随波长变化剧烈: 3~10px 细结构是 λ30 波的 1.27~2.20 倍 (§14.30.4); 又因为屏幕
			// 波长随透视从远处 4~6px 变到近处 100px+, 同一算子在不同深度相差 15 倍 (§14.30.5)。
			vec3 cL = texture(uBase, uv - vec2(px.x, 0.0)).rgb;
			vec3 cR = texture(uBase, uv + vec2(px.x, 0.0)).rgb;
			vec3 cU = texture(uBase, uv - vec2(0.0, px.y)).rgb;
			vec3 cD = texture(uBase, uv + vec2(0.0, px.y)).rgb;
			vec3 bL = texture(uBase, uv - kx).rgb;
			vec3 bR = texture(uBase, uv + kx).rgb;
			vec3 bU = texture(uBase, uv - ky).rgb;
			vec3 bD = texture(uBase, uv + ky).rgb;
			// ---- v0.18.14 输入端软限幅 (ssr.ripk, 0 = 关 ⇒ 逐位 = v0.18.13) ----
			// 治什么: 高光斑幅度比波大 ~8 倍, 冲激响应让每个斑在 ±1、±Rb 各打满幅 ⇒ 位移场被亮斑
			// 劫持成 4~8px 的饱和块 = 用户说的「倒影处波纹小而密集」(§14.30.4 / §14.30.5)。
			// 怎么治: 把每个采样点夹进「本环 4 tap 均值 ± K」, 均值由**已取的**这 4 个 tap 算出 ⇒
			// 零额外取样。夹住之后 |g1|、|g2| 都 ≤ 2K ⇒ |g1−g2| ≤ 4K ⇒ 场被硬性封顶 40·K, 不再整片
			// 削顶; 阶跃两侧同被夹住 ⇒ 相减仍归 0, 轮廓照旧不泄漏。
			// K 是**绝对**阈值, 依赖 uBase 局部亮度 (同一帧暗水与亮反射差 ~1.7 倍) ⇒ 做成旋钮让实机
			// 扫, 不写死, 见 docs/02 §14.30.5。
			float ripK = max(pc.p5.x, 0.0);
			if (ripK > 0.0)
			{
				vec3 loC = (cL + cR + cU + cD) * 0.25;
				vec3 loB = (bL + bR + bU + bD) * 0.25;
				vec3 kk = vec3(ripK);
				cL = clamp(cL, loC - kk, loC + kk);
				cR = clamp(cR, loC - kk, loC + kk);
				cU = clamp(cU, loC - kk, loC + kk);
				cD = clamp(cD, loC - kk, loC + kk);
				bL = clamp(bL, loB - kk, loB + kk);
				bR = clamp(bR, loB - kk, loB + kk);
				bU = clamp(bU, loB - kk, loB + kk);
				bD = clamp(bD, loB - kk, loB + kk);
			}
			vec2 g1 = vec2(dot(cR - cL, vec3(0.2126, 0.7152, 0.0722)),
			               dot(cD - cU, vec3(0.2126, 0.7152, 0.0722))); // 1px 梯度
			vec2 g2 = vec2(dot(bR - bL, vec3(0.2126, 0.7152, 0.0722)),
			               dot(bD - bU, vec3(0.2126, 0.7152, 0.0722))); // Rb 梯度
			// v0.18.12: 幅度与波长挂钩 (默认 ripplesz=4 ⇒ 6px, 与原来逐位一致), ripplesz 调大时
			// 波长与幅度一起长大 —— 治「倒影区域水波较小」的"幅度"这一半; "亮度"那一半 (sparkle
			// 有没有回来) 走 mode0, 见 docs/02 §14.28。
			// v0.18.14: 幅度 / 增益两旋钮解耦 (0 = 沿用自动值 ⇒ 与 v0.18.13 逐位一致), 见 §14.30.5。
			ripAmp = pc.p5.y > 0.001 ? pc.p5.y : clamp(Rb * 1.5, 6.0, 16.0);
			float ripGain = pc.p5.z > 0.001 ? pc.p5.z : 10.0;
			ripd = clamp((g1 - g2) * ripGain, vec2(-1.0), vec2(1.0)) * (ripAmp * px) * ripA;
			ruv = clamp(ruv + ripd, vec2(0.0), vec2(1.0));
		}
	}

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

	// mode0 的亮度回注放在**采样之后**乘 (位移式已在上面把 ruv 挪过了);
	// 下面的兜底行在它之后 ⇒ 未命中时回注被原样盖掉, 保证 strength=0 是逐字节原版。
	if (pc.p3.w > 0.5 && ripA > 0.001 && pc.p4.z < 0.5)
		refl *= mix(1.0, kf, ripA);

	// ---- v0.18.11: 未命中的回退源 (门 ssr.edge, 归因见文件头) ----
	// edge=0 (默认): 未命中**一律**回 baseRGB (原版层) —— 不再拿 lastUV 去采 uColor, 因为那个位置
	//                拿到的是**未镜像**的原图 (岸边/树的原位画面), 贴进水里就是截图里那块错位重影;
	// edge=1: 退回 v0.18.10 的边缘延展 (只有"一跳都没进屏"才回底色), 当 A/B 对照。
	if (!hit && (pc.p4.w < 0.5 || lastUV == uv))
		refl = baseRGB;
	// hit 但 hitUV 贴着屏幕边 (4%) => 淡回原版层: 扇形边界上的像素正是"再走一跳出屏就变 miss"那批,
	// 它们的 hitUV 就落在屏幕边附近; 淡出后边界两边都是反射 (锐 SSR <-> 原版层), 不再是硬接缝。
	// v0.18.15: bf (贴边淡出权重) 提到 if 外面 —— 下面合成处要拿它算"SSR 到底接管了多少原版层"。
	float bf = 1.0;
	if (hit)
	{
		vec2 dm = min(hitUV, vec2(1.0) - hitUV);
		bf = smoothstep(0.0, 0.04, min(dm.x, dm.y));
		refl = mix(baseRGB, refl, bf);
	}

	// debug=6: 回注可视化 —— mode0 画 kf (蓝=被提亮 / 红=被压暗 / 黑=这格没回注), mode1 画位移灰度
	// debug=7: uBase 原样 (游戏自己写的那层) —— 对照原版涟漪的形态与波长, 据此定 ssr.ripplesz
	// debug=8: v0.18.15 单位回补量 —— 把**最终会加回去的那部分**渲成灰度 (Reinhard 压缩, 可逆):
	//          l = dot(add, luma); y = l/(1+l) ⇒ 读数时 l = y/(1-y)。
	//          加数与合成式**逐项同式** (max(baseRGB-refl,0) * gb * wSsr), 只是**不乘 ssr.v1det**
	//          ⇒ 表示"det=1 时的满量程", 所以 v1det=0 也能拍。
	//          为什么要它: 跨图差分已被光照漂移吃掉 (§14.31 实测 10 分钟水面外亮斑 +16.6%,
	//          比回补量本身大), 而这是一张**直接读出"哪里能加、加多少"**的图, 不受机位/
	//          光照/水面动画影响。黑 = 那里没得补 (refl 已 >= baseRGB, 或 wSsr=0 = 非命中/贴边),
	//          白 = 那里原版比 SSR 亮很多且亮度门开满。
	if (pc.p3.z > 7.5)
	{
		float k8 = clamp(pc.p1.z, 0.0, 1.0);
		float w8 = hit ? (k8 * bf) : 0.0;
		vec3 add8 = vec3(0.0);
		if (w8 > 0.0001)
		{
			float lb8 = dot(baseRGB, vec3(0.2126, 0.7152, 0.0722));
			float gb8 = smoothstep(0.10, 0.45, lb8);
			add8 = max(baseRGB - refl, vec3(0.0)) * (gb8 * w8);
		}
		float l8 = dot(add8, vec3(0.2126, 0.7152, 0.0722));
		oColor = vec4(vec3(l8 / (1.0 + l8)), 1.0);
		return;
	}
	if (pc.p3.z > 5.5)
	{
		if (pc.p3.z > 6.5)
		{
			oColor = vec4(texture(uBase, uv).rgb, 1.0);
			return;
		}
		if (pc.p4.z < 0.5)
		{
			float dk = clamp((kf - 1.0) * 1.5, -1.0, 1.0);
			oColor = dk >= 0.0 ? vec4(0.0, 0.0, dk, 1.0) : vec4(-dk, 0.0, 0.0, 1.0);
		}
		else
			oColor = vec4(vec3(clamp(length(ripd) / (ripAmp * px.x), 0.0, 1.0)), 1.0);
		return;
	}

	// ---- v0.18.9 A: 585 = **纯反射层** (段17 契约, 见文件头) ----
	// 层叠归段17: 我方只给"这一层是什么", 不再自算 fresnel 也不再把 cubemap 掺进合成里。
	float k = clamp(pc.p1.z, 0.0, 1.0);            // ssr.strength = SSR 替换比 (0 = 原版 cubemap)
	float v1det = clamp(pc.p5.w, 0.0, 1.0);        // ssr.v1det 高光回补量, 0 = 关 (默认)

	// ---- v0.18.15 A 正解: 把原版 585 层被顶掉的亮部高光按比例补回命中区 ----
	// 起因 (§14.30.7 A 坐实): strength=1 时 mix 100% 用 refl 顶掉 baseRGB, 585 层里游戏
	// 自己算的 specular (那批白亮斑) 跟着一起没了; strength=0.7 档 30% 回流 = "有亮斑但不如正常亮"。
	// 修法: 只在 SSR 真正接管的像素上 (wSsr = strength * 贴边淡出权重), 把 baseRGB 比 refl
	//       亮出来的那部分按 det 加回去 —— 亮处取二者较亮的一侧, 暗处不动 (max 只会补不会压)。
	//       lb/gb 是绝对亮度门, 只让原版本身够亮的像素参与, 防止中等亮度整片倒回原版层削弱 SSR。
	// v0.18.15 实跑重标定 0.35/0.85 -> 0.10/0.45 (§14.31 实拍, 门限原来定在被门控层的分布之外):
	//       debug=7 实测 baseRGB 三块水面 meanL = 0.2125 / 0.2355 / 0.3258, 而 luma>=0.35 的像素
	//       只占 10.1% / 6.1% / 24.4% —— 旧门限 0.35 **高于该层亮度中位数 (0.21~0.33)**, 等于把
	//       76~94% 的水面挡在门外, debug=8 热力图因此几乎全黑。新门限把下沿放到水体典型亮度
	//       之下 (0.10), 满量程收到 0.45; 亮片仍满开, 水体部分参与。安全性不变: max(baseRGB-refl,0)
	//       只加不减 + v1det 缩放 + wSsr 命中门, 且本块 v1det=0 整块不进 = 逐位 v0.18.14。
	// 门: miss ⇒ wSsr=0 ⇒ 纯原版逐位不动; det=0 ⇒ 整块不进 ⇒ 退化成原合成式 = 逐位 v0.18.14。
	float wSsr = hit ? (k * bf) : 0.0;
	vec3 outRGB = mix(baseRGB, refl, k);
	if (v1det > 0.001 && wSsr > 0.0001)
	{
		float lb = dot(baseRGB, vec3(0.2126, 0.7152, 0.0722));
		float gb = smoothstep(0.10, 0.45, lb);
		outRGB += max(baseRGB - refl, vec3(0.0)) * (v1det * gb * wSsr);
	}
	oColor = vec4(outRGB, base.a);
}
