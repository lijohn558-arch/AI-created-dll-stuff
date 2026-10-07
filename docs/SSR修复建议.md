结论先说：现在 SSR 的主要问题已经不是“某一个参数没调好”，而是 SSR 的核心算法里有 3 个结构性问题。
其中最关键的是：

ssr.edge=0 之所以“稍微正常”，本质上是在主动隐藏 SSR 的 miss；真正的问题是目前 SSR 的射线命中率非常低，而且投影/水面波纹模型本身也不够正确。

你现在的配置确实是 edge=0 / steps=32 / dist=500 / fov=65 / near=10 / far=100000 / ripple=1 / ripplesz=4 / ripplemode=1。

1. 三张图实际上已经把问题定位得非常清楚

你的三张图：

debug=2

这里代码定义的是：

绿色 = SSR ray hit
红色 = SSR ray miss

代码明确这样输出。

而你的图表现为：

远处水面大面积绿色
近处水面大面积红色
中间存在非常明显的 hit/miss 分界

这与仓库自己的 v0.18.10 实跑分析完全一致：近水区域因为反射射线很快离开屏幕，所以大量 miss。仓库文档也明确记录了这个现象。

所以：

SSR 并不是“整个水面都没有正确工作”。

而是：

远处水面
    ↓
反射射线还能留在屏幕里
    ↓
SSR hit
    ↓
能得到真正的 screen-space reflection


近处水面
    ↓
反射方向很陡
    ↓
很快射出屏幕
    ↓
SSR miss
    ↓
只能 fallback

这就是为什么你设置：

ssr.edge=0

以后画面突然“正常一些”。

因为 edge=0 的代码会：

if (!hit)
    refl = baseRGB;

也就是：

SSR 找不到反射，就直接交还给 Skyrim 原来的 585 水面反射。

这个逻辑本身是对的。

2. edge=1 的问题已经确定：它实际上是在“贴屏幕边缘的原图”

这也是为什么第三张图明显不正常。

当前代码：

vec2 ruv = clamp(hit ? hitUV : lastUV, ...);

当 ray miss 后，lastUV 只是：

射线最后一次还在屏幕内的位置。

它不是反射坐标。

然后：

refl = texture(uColor, ruv)

就等于：

把屏幕顶部/边缘看到的树、岩石，直接原方向贴进水里。

这当然不会产生正确镜像。

仓库文档自己已经把这个问题定位出来了：edge=1 的错位倒影就是这个原因。

所以：

edge=1 不应该继续作为画质方案。

它最多只能作为 debug A/B：

ssr.edge=1

用来证明 miss fallback 有问题。

正常情况下应该：

ssr.edge=0

这一点你现在已经验证出来了。

3. 真正的大问题：32 steps + dist=500 实际上太粗了

这个是我认为目前 SSR 画质差的第一核心原因之一。

你当前：

ssr.steps=32
ssr.dist=500
ssr.near=10

而代码：

pc.p1[3] = g_ssrV1Dist * g_ssrV1Near;

也就是说实际最大 ray distance：

500 × 10
= 5000 Skyrim units

然后 shader：

float stepLen = maxT / float(steps);

所以：

5000 / 32
≈ 156 units / step

也就是说：

你的 SSR 射线每一步直接跨 156 个世界单位。

这对于水面反射来说太粗了。

尤其是：

树
人
岩石
窄桥
树枝
NPC
建筑边缘

这些屏幕空间几何，很容易直接被跨过去。

代码只有：

if (Q.z < sz)

才认为 hit。

也就是说：

P
|
| 156 units
|
| 156 units
|
| 156 units
|
Q

中间如果有一个很薄的物体：

      █
      █  ← 可能完全跳过去
      █

SSR 根本不知道它存在。

4. 更严重的是：目前没有 Hi-Z / depth hierarchy

你的代码注释自己也写了：

线性步进 + 末段三次二分收紧
不做 Hi-Z

所以目前实际上是：

32 次固定步进
        ↓
第一次穿透
        ↓
3 次 binary search

这能做出 PoC，但离高质量 SSR 还差很远。

正确方向应该是：

Depth
 ↓
建立 Hi-Z / Depth Pyramid
 ↓
粗粒度快速寻找可能碰撞
 ↓
进入局部区域
 ↓
细粒度 ray march
 ↓
binary refinement

这样可以同时做到：

更高命中率
更远反射距离
更少步数
更少漏掉细小物体

所以我不建议简单粗暴地把：

ssr.steps=32

直接改成：

ssr.steps=256

虽然画面可能改善，但是性能会非常差，而且仍然不是正确的 SSR 算法。

5. 还有一个非常关键的问题：你的投影矩阵是“猜”的

这是我认为第二个核心问题。

现在 shader 需要：

FOV
aspect
near
far

然后自己重建 projection。

代码：

float viewZ(float d)

以及：

vec3 viewPos(...)

全部依赖这些参数。

而你的 ini：

ssr.fov=65
ssr.near=10
ssr.far=100000

这些实际上都是人工填写的。

代码自己也承认：

与实际投影不一致时反射比例会偏。

这对于 SSR 是一个非常大的问题。

因为：

Depth
 ↓
view-space position
 ↓
reflection ray
 ↓
project back to screen

任何一个 projection 参数不正确：

真实位置
    ↓
错误 view-space position
    ↓
错误 reflection vector
    ↓
错误 hitUV

最后就是：

倒影比例不对
倒影位置不对
水面不同区域出现不同程度偏移
物体轮廓附近出现 halo
ray 更容易错误地出屏
hit/miss 边界错误
6. 所以我认为目前应该停止继续调 fov=65

应该直接把：

ssr.fov=65
ssr.near=10
ssr.far=100000

这个设计废掉。

真正正确的方法是：

从 Skyrim 的实际 projection matrix 获取：
Projection Matrix
View Matrix

甚至最好直接得到：

ViewProjection
InverseViewProjection

然后 SSR：

world/view position
↓
真实 projection
↓
screen UV

而不是：

猜 FOV
猜 near
猜 far

目前项目的设计明确选择了：

只通过 FOV/aspect/near/far 反推 inverse projection，不使用 view matrix。

这个方案作为 PoC 可以，但如果你的目标是：

真正替代 Skyrim 水面 SSR / 做高质量 SSR

我认为这里已经到了应该换架构的阶段。

7. 你说的“人物周围一圈不明物体”，我重点看了

这里有一个很有意思的现象。

我把你的 edge=0 人物区域放大后看：

人物轮廓确实有一圈比较明显的亮/暗边。

但我认为它不是 edge=1 的那个“错位倒影”问题。

更值得怀疑的是目前的 ripple displacement。

代码现在不是直接使用水面法线，而是：

uBase
 ↓
取当前像素周围亮度梯度
 ↓
1px gradient
 ↓
4px gradient
 ↓
相减
 ↓
×10
 ↓
最多 6px UV displacement

具体代码就是这里。

问题在于：

uBase 是“已经经过水面反射后的最终颜色”，不是水面法线。

所以它里面同时包含：

岩石轮廓
树
天空
NPC
光照
原版水面反射
水波
高光

你现在实际上是在：

最终颜色图
    ↓
猜测哪里是水波
    ↓
把颜色梯度当成水波位移

这不是可靠的水面法线重建方法。

8. 这会直接解释“水波比较小”

当前：

ssr.ripple=1
ssr.ripplesz=4
ssr.ripplemode=1

代码最大位移：

* (6.0 * px) * ripA

也就是最多大约：

6 像素

而且实际位移大小由：

(g1 - g2)

决定。

所以它并不是：

原版水波强度 × 1

而是：

从原版反射颜色猜一个 displacement

因此很容易出现你现在看到的：

有波纹，但是比原版小很多。

这不是简单把：

ssr.ripple=1

改成：

ssr.ripple=2

就能解决的。

因为 CPU 代码会把它限制到：

0..1

9. 更正确的波纹实现：直接拿 Skyrim 水面使用的 Normal

这才是我认为下一版应该做的事情。

现在：

585
 ↓
最终水面颜色
 ↓
推断 ripple

应该改成：

Skyrim Water Pass
        ↓
找到水面 normal map / normal texture
        ↓
共享到 Vulkan
        ↓
SSR shader
        ↓
真实 water normal
        ↓
reflect()

这样：

Rf = reflect(V, Nwater);

里面的 Nwater 才是真正的 Skyrim 水面法线。

而不是现在这种：

最终反射颜色 → gradient → 猜法线/位移

这是一个非常大的质量提升点。

10. 另外一个问题：现在的 SSR 没有 thickness

当前 hit：

if (Q.z < sz)

非常绝对。

没有：

thickness
bias
depth tolerance

也就是说：

Q.z = sz - 0.001

也可能被认为 hit。

这非常容易产生：

自相交
物体轮廓 halo
边缘脏线
反射粘在几何体表面
小物体附近异常

尤其是：

水面
    ↘
      人物 / 岩石

这种地方。

建议下一版增加：

ssr.thickness
ssr.bias

例如：

hit = Q.z < sceneZ - thickness

然后根据：

ray distance
view depth
screen edge
normal difference

计算 hit confidence。

11. 现在的 smooth=4 也不是根治

你当前：

ssr.smooth=4
ssr.blur=1

这确实能减少破碎。

代码是：

Pl
Pr
Pu
Pd

cross(...)

然后得到法线。

但是：

smooth=4

只是把采样范围扩大。

它解决的是：

深度噪声 / 水底三角形导致的法线碎裂。

而不是：

SSR ray hit 本身不正确。

所以如果现在把：

smooth=4

改成：

smooth=8

可能会更糊，但不会从根本上解决当前问题。

12. 当前代码实际上已经完成了一个重要阶段

这一点我反而认为做得不错。

目前已经把几个完全不同的问题拆开了：

324
 ↓
SSR reflection source


520
 ↓
ray marching depth


585
 ↓
原版水面 reflection/base


461
 ↓
真正水面 depth

shader 对这四张输入的职责划分已经比较清晰。

而且 Vulkan 侧也已经做好了：

color shared image
depth KMT image
base 585 image
water-depth 461 image

这些通路。

所以我不建议再继续“修补 CopyResource / shared handle”。

现在瓶颈已经从 API interop 转移到了 SSR 算法本身。

13. 我建议下一版不要再继续 v0.18.12 式的小修

我建议直接做一个 SSR v2，分四步。

第一阶段：先把 projection 修正确

从 Skyrim 真实渲染常量里获取：

Projection
View
或 ViewProjection

然后：

viewPos()
projectUV()

全部使用真实矩阵。

这是优先级最高的。

第二阶段：改 ray marcher

从：

32 linear steps
5000 units

改成：

Hi-Z / depth pyramid
↓
coarse march
↓
fine march
↓
binary refinement

如果暂时不想做 Hi-Z，至少先测试：

ssr.steps=128
ssr.dist=100

注意这里：

100 × 10 = 1000 units
1000 / 128 ≈ 7.8 units/step

这和现在：

5000 / 32 = 156 units/step

完全不是一个级别。

这个测试非常值得做。

如果 128/100 的反射突然明显改善，就直接证明目前最大的算法问题之一就是 ray sampling 太粗。

第三阶段：增加 thickness / bias / confidence

例如：

ssr.thickness=2.0
ssr.bias=0.5

hit：

if (Q.z < sceneZ - thickness)

然后：

confidence =
    distance fade
  × edge fade
  × depth confidence
  × normal confidence

最终：

SSR
   ↕
vanilla 585

平滑混合。

第四阶段：真正解决水波

不要再从：

585 luminance

推断水波。

而是直接找：

Water Normal Texture

然后：

Nwater
 ↓
reflect()

这一步完成之后，你的：

“倒影区域水波比较小”

这个问题才算真正解决。

14. 你现在可以立即做一个非常有价值的 A/B

不用改代码。

把现在：

ssr.steps=32
ssr.dist=500

改成：

ssr.steps=128
ssr.dist=100

其他全部不动：

ssr.edge=0
ssr.smooth=4
ssr.blur=1
ssr.ripple=1
ssr.ripplesz=4
ssr.ripplemode=1
ssr.wdep=1
ssr.base585=1

然后再：

ssr.debug=2

拍一张同位置。

我最想看两个东西：

① 红色区域有没有明显缩小

如果：

32 / 500
████████████

变成：

128 / 100
████

说明 ray marching 是主要问题。

② 绿色区域里面的倒影有没有明显变得更稳定

如果明显改善：

下一步直接做 Hi-Z，而不是继续调 smooth。

如果几乎没变化：

那么 projection matrix 才是第一优先级。

15. 最终问题树

我现在对这个版本的判断是：

问题	严重程度	当前状态
edge=1 错位倒影	🔴	已定位，不能作为正常方案
大量 SSR miss	🔴	核心问题
32 / 500 ray marching 太粗	🔴	非常值得立即验证
FOV/near/far 人工猜测	🔴	结构性问题
无 Hi-Z	🔴	SSR 质量上限明显
无 thickness/bias	🟠	容易产生边缘/自交问题
ripple 从最终颜色反推	🔴	波纹质量的核心问题
ripplesz=4	🟡	参数不是根本原因
smooth=4	🟡	只能缓解碎裂
blur=1	🟡	只能掩盖部分抖动
585/520/461 通路	🟢	目前设计已经比较合理
D3D11↔VK shared handle	🟢	暂时不是主要画质瓶颈

所以我现在不会再优先修改 edge、smooth 或 blur。

最合理的路线是：

先做 128 steps / dist=100 的 A/B → 如果命中明显改善，就直接进入 Hi-Z；同时开始抓 Skyrim 实际 Projection Matrix。之后再把水面 Normal Texture 接进来，彻底替换目前“从 585 颜色猜波纹”的方案。

尤其是你这张 debug=2 图非常有价值——它已经证明现在的红色区域不是“颜色合成坏了”，而是SSR ray 根本没有找到屏幕内的反射目标。而 edge=0 正好把这些 miss 交还给原版水面，所以才会看起来稍微正常。