# 04 · NIF 资源格式调研（顶点格式 / 材质参数 / 骨骼蒙皮）

> 对应 `docs/00` 首周行动项 **#3「资源格式调研：解析典型 NIF 文件，确定顶点格式、材质参数、骨骼数据结构」**
> 与最高风险项 **#2「NIF 格式动画 / 蒙皮在 Vulkan 下的实现复杂度」** 的评估输入。
>
> **结论口径：经验验证法**。字段权威布局取自 `niftools/nifxml`（`nif.xml`，20.2.0.7 / BS 100 与 BS 83 条件），
> 每条结论都用本机实测字节 dump 互证，三重校验：①块尺寸 `sizeOk` 全等；②`dataSize = stride×verts + tris×6` 算术精确；
> ③三角形落点/顶点值语义合理（浮点半精度解码、法线长度≈1、权重和=1）。
> **未验证项一律写在 §9 诚实边界**，不做推断式表述。
>
> 状态：**✅ 已完成（2026-10-03）**；80 文件 / 1613 shape 全量 sizeOk，0 error，0 对齐失败。

---

## §0 结论速览

| 关注点 | 结论 | 证据 |
|---|---|---|
| 顶点格式 | SSE 用 `BSVertexDesc`（u64）自描述：低 4 位=stride/4，各 4-bit 槽给组件偏移，bits44-55=属性位图。实测 **9 种格式**，20~44 B/顶点 | §4 聚合表（1613 shape） |
| Vulkan 映射 | 位置 f32×3+4B、UV **half2**、法线/切线 **packed UNORM8→[-1,1]**、颜色 UNORM8、权重 **half4（和=1.0000）**、骨骼索引 **u8×4** → 属性表可直接落到 `VkVertexInputAttributeDescription` | §4 逐字节解码 |
| 材质参数 | `BSLightingShaderProperty`（shaderType 枚举 0~20 + 条件尾）+ `BSShaderTextureSet`（**9 slot**），纹理全 `.dds`，路径需归一化（相对 3504 / 绝对 144） | §5 分布统计 |
| 骨骼蒙皮 | 三层：`NiSkinInstance/DSI`（骨骼表+部位）→ `NiSkinPartition`（全局 VB + 子网格三角形/骨骼）→ 权重/索引在**顶点流内**（4 权重/顶点） | §6（457+397 实例、854 partition、3686 子网格） |
| 风险项 #2 | 数据格式侧**不难**；真正的难点是**运行时骨骼矩阵来源**（动画求值在游戏侧）。结论：中等复杂度、可分阶段规避 | §7 |
| 解析器 | `tools/parse-nif.ps1`（单文件 / 目录批跑 / JSON 聚合）；字节级 dump `dump-nif-block.ps1` | §10 |

---

## §1 方法与工具

### 1.1 为什么用经验验证法

本机**无 Python、无原生编译器**（PowerShell 5.1 `Add-Type` 仅 C#5），无法跑 niflib/pyffi 交叉验证；
GitHub HTTPS 被墙但 `api.github.com` 可直连 → 取 `niftools/nifxml` 的 `nif.xml`（权威字段布局 + 版本条件）作为规格，
自写解析器并用**字节 dump 互证**。判定标准只有三条，全过才算“定案”：

1. **sizeOk**：解析消费字节数 == 块表声明的 `blockSize`（逐块全等）；
2. **dataSize 算术**：`vertexData + triangles == dataSize`，`tris×6 + verts×stride == dataSize` 精确成立；
3. **落点语义**：三角形字节出现在算出的偏移上、顶点浮点/半精度值在合理量纲（坐标 ≈ 场景尺度、法线长≈1、权重和=1）。

### 1.2 工具

| 工具 | 作用 | 说明 |
|---|---|---|
| `tools/parse-nif.ps1` | 解析器 + 批跑 + 聚合 | `-Path <file|dir> [-First N] [-OutFile report.json]`；输出 header/块表/geometry/shaders/textureSets/skins/errors 与聚合（`vertexFormatAgg`、`layoutVariantAgg`、`blockTypeAgg`、`shaderTypeAgg`） |
| `tools/dump-nif-block.ps1` | 字节 dump | `-File -Idx N`（单块 hex）/ `-Want <type> -List`（同类块的偏移、size、after-desc、tail16） |
| `nif.xml`（会话临时件） | 规格 | `niftools/nifxml` 20.2.0.7 / BS 100·83 条件字段序 |

> 解析器条目均带 `blockIdx` + `offset`，可直接回到 dump 工具做字节级回查。

### 1.3 样本

| 批 | 范围 | 取样 | 结果 |
|---|---|---|---|
| batch1 | `D:\lod\TKV6 GAMEPLAY 1.03+\NATKV\meshes`（单 mod，武器/建筑/生物/特效） | 按文件大小降序 40 | 771 shape，0 error |
| batch2 | `D:\Skyrim_MOD\mods`（跨 mod：服装/生物/动画物件…） | 按文件大小降序 40 | 842 shape，0 error |
| 锚点 | `body_0.nif`（蒙皮空壳）、`dawnbreaker.nif`（非空 shape 逐字段）、`emptypassthru_lod.nif`（单块）、`lumbermill01waterwheel01.nif`（变体发现） | — | 见 §3/§6 |

合计 **80 文件 / 1613 BSTriShape / 1305 纹理集 / 854 蒙皮块**，`errors=0`、`blockDataAligned=false 计 0`（8 字节尾已计入，见 §2）。
`bsVersion` 分布：**bs100=66、bs83=14**（旧流 14 个文件需走旧布局，见 §6.4）。

> 取样偏差：按体积降序取 Top-N，偏向大型/复杂 mod；小体积 vanilla 风格网格覆盖较少（§9）。

---

## §2 文件容器（header / 块表 / 8 字节尾）

实测布局（magic 到块数据，字段序与 `nif.xml` 一致）：

```
"Gamebryo File Format, Version 20.2.0.7"  (16+18 byte + 0x0A)
u32 version   = 0x14020007
u8  endian    = 1          (little)
u32 userVersion = 12
u32 numBlocks
u32 bsVersion = 100 | 83
ExportString × 2          (Process Script，条件 BS<131；Max Filepath，条件 BS≥103)
                          ExportString = u8 len + 字节 + NUL → 消费 1+len
u16 numBlockTypes
SizedString × numBlockTypes      (块类型名表，u32 len + 字节)
u16 typeIdx[numBlocks]           (每块指向类型表)
u32 blockSize[numBlocks]
u32 numStrings / u32 maxStringLength / 字符串表
u32 numGroups        (实测恒 0；此后直接是块数据，块前无类型前缀)
[块数据 …]
[文件尾 8 字节: 01 00 00 00 00 00 00 00]   ← footer（三锚点样本 + 80 批样本一致）
```

三锚点走查闭合（`blockStart + ΣblockSize + 8 == fileLen`）：

| 样本 | numBlocks | blockStart | ΣblockSize | fileLen | 尾 |
|---|---|---|---|---|---|
| `body_0.nif` | 162 | 3031 | 988411 | 991450 | 8 |
| `dawnbreaker.nif` | 87 | 1408 | 54734 | 56150 | 8 |
| `emptypassthru_lod.nif` | 1 | 218 | 80 | 306 | 8 |

> **口径**：`fileFooter=8` 记进 JSON；对齐判定把它计入（`blockDataAligned`），80 批样本无一失败。
> footer 的语义未定（见 §9）。

---

## §3 几何容器：BSTriShape

### 3.1 标准布局（1613 shape 中 1611 个）

```
NiAVObject base: nameIdx(i32, -1=空) + numExtra(u32) + extra[i32×numExtra] + controller(i32) + flags(u32)
                 → 72 + 4×numExtra 字节
NiBound    : center f32×3 + radius f32                       (16B)
skinRef / shaderRef / alphaRef : i32 × 3                     (12B)
vertexDesc : u64                                              (8B) → §4
numTris    : u16
numVerts   : u16
dataSize   : u32   = numVerts×stride + numTris×6             ← 算术必须精确
vertexData : stride×numVerts
triangles  : u16×3 × numTris                                 (索引 16-bit)
尾部       : u32 (=0)                                         (4B)
```

**逐字段字节坐实**（`dawnbreaker.nif` block[9]，size=1188，numExtra=1 → base=76）：

| 字段 | 相对偏移 | 值 |
|---|---|---|
| nameIdx / numExtra / extra[0] / controller | 0 / 4 / 8 / 12 | 5 / 1 / ref10 / −1 |
| flags | 16 | 0x0008000F |
| translation | 20 | (−42.59, −0.687, −1.08e−5) |
| rotation 9×f32 / scale | 32 / 68 | 单位阵 / 0.99999 |
| NiBound (center+radius) | 76 | (42.59, 37.99, ~0) r=26.56 |
| skinRef / shaderRef / alphaRef | 92 / 96 / 100 | −1 / 11 / 13 |
| vertexDesc | 104 | 0x0001B00000650407（stride 28、attrs 0x1B） |
| numTris / numVerts / dataSize | 112 / 114 / 116 | **42 / 29 / 1064 = 29×28 + 42×6** ✓ |
| vertexData | 120 | 29×28 = 812 B → 起点 932 |
| triangles | 932 | 42×6 = 252 B → 1184 |
| 尾 u32 | 1184 | 0 → 1188 = size ✓ |

对照空壳：`body_0.nif` 13 个 shape 全部 120 B（base72+16+12+8+8+0+0+4=120）——**顶点不在 shape 里，而在 NiSkinPartition**（§6）。

### 3.2 变体：compact sec2（2 例，均为水车文件的粒子/特效挂载 shape）

`lumbermill01waterwheel01.nif` 的 block[21]/block[51] 在标准结构后多出一截：

```
[u32 u16Count][half3 pos × numVerts][half3 normal × numVerts][三角形 × numTris]
u16Count = 6×numVerts + 3×numTris      ← 两块均精确（8790 / 3840）
```

证据：①`u16Count×2+4 == 剩余字节` 精确；②后段三角形与主区三角形**逐字节相同**（重复索引缓冲）；
③`half3 pos` 解码值 == 主区 float3 位置（200.43/-120.61/-55.08 → 200.38/-120.63/-55.06，半精度误差内）；
④`half3 normal` 解码 (-0.25, 0.00, -0.97) 与主区 packed 法线解码 (-0.255, -0.004, -0.969) **逐值一致**。

→ 语义：**同一网格的半精度位置/法线副本 + 重复索引表**。用途未证（§9）。
解析器按“算术吻合即接受”处理，聚合里记 `u16 tris + stored dataSize + compact sec2 ×2`，其余 1611 为标准布局。

### 3.3 老变体兜底

`dataSize` 未按 u32 存储 / 三角形计数为 u32 的导出器变体已在解析器里做了 4 组探测
（`u16/u32 tris × stored/computed dataSize`），80 样本未触发——但保留在规格里以防其它导出工具。

---

## §4 顶点格式 → Vulkan 顶点输入（核心）

### 4.1 BSVertexDesc 解码（实测定案）

```
u64 desc:
  bits 0-3   : dword 数 = stride/4            （0xA=10→40B、0x7=7→28B、0x6→24B…）
  bits 4-7   : Dynamic（恒 0）
  bits 8-11  : UV0 偏移(dword)   bits12-15: UV1 偏移
  bits16-19  : Normal 偏移       bits20-23: Tangent 偏移
  bits24-27  : Color 偏移        bits28-31: Skinning 偏移
  bits32-43  : 0                bits44-55: attrs 属性位图
  attrs bit: 0 Vertex / 1 UVs / 2 UVs_2 / 3 Normals / 4 Tangents / 5 Vertex_Colors /
             6 Skinned / 7 Land_Data / 8 Eye_Data / 9 Instance / 10 Full_Precision
```

> 易错点（本仓踩过）：`color` 与 `skinning` 的 nibble 顺序是 **24-27=Color、28-31=Skinning**；
> 未启用组件的偏移 nibble 是**脏值**（例：block[21] 无 Tangent 但 tangent nibble=5），必须以 attrs 位图为准。
> `stride==4×attrs 中各分量之和` 的一致性由解析器 `strideOk/calcStride` 双算校验。

### 4.2 组件的字节形式与 Vulkan 映射

顶点内分量**按固定顺序连续排列**（只有 attrs 声明的组件才占字节）：

| 顺序 | 组件 | 字节数 | 文件内形式 | 实测解码 | VkFormat（顶点输入） | Shader 侧 |
|---|---|---|---|---|---|---|
| 1 | Position | 16 | f32×3 + **4B 垃圾**（非 0） | dawnbreaker v0=(42.599, 50.682, −0.2774)，第 4 分量 −0.994 | `R32G32B32_SFLOAT` @0（或 A32 忽略 w） | `vec3` |
| 2 | UV0 | 4 | **half×2** | (0.173, −0.335) | `R16G16_SFLOAT` | `vec2` |
| 3 | Normal | 4 | packed 4×u8 | (0x73,0x80,0x01,0x86) → (−0.098, 0.004, **−0.992**) 长度 0.997 | `R8G8B8A8_UNORM` | `n = c*2-1` |
| 4 | Tangent | 4 | packed 4×u8 | xyz 单位长；**w 为杂值**（实测 −0.435，非 ±1） | `R8G8B8A8_UNORM` | xyz 同上，w 弃用/取符号 |
| 5 | Color | 4 | 4×u8 UNORM | (126,126,126,255) → (0.494,0.494,0.494,1) | `R8G8B8A8_UNORM` | `vec4` |
| 6 | Skin Weights | 8 | **half×4** | `[0.875, 0.125, 0.000, 0.000]` **sum=1.0000** | `R16G16B16A16_SFLOAT` | `vec4` |
| 7 | Skin Indices | 4 | 4×u8 | `[2, 3, 13, 1]` | `R8G8B8A8_UINT` | `uvec4` |

法线打包的**交叉验证**：`waterwheel` block[51] 主区 packed 法线 (5F 7F 04 00) 解码为 (−0.255, −0.004, −0.969)，
其 compact sec2 半精度法线解码为 (−0.25, 0.00, −0.97) —— 两条独立路径逐值一致 ⇒ 打包方式 `u8 → [−1,1]` 定案。

### 4.3 实测格式聚合（80 文件 / 1613 shape）

| 签名 | stride | 组成（字节） | skinned | 数量 |
|---|---|---|---|---|
| **VUNTS** | 40 | 16 pos + 4 uv + 4 n + 4 t + 8 w + 4 idx | ✓ | **713** |
| **VUNTV** | 32 | 16 + 4 + 4 + 4 + 4 color | ✗ | **399** |
| **VUNT** | 28 | 16 + 4 + 4 + 4 | ✗ | **372** |
| **VN** | 20 | 16 + 4 | ✗ | 71 |
| **VUS** | 32 | 16 + 4 uv + 8 w + 4 idx | ✓ | 45 |
| **VNS** | 32 | 16 + 4 n + 8 w + 4 idx | ✓ | 8 |
| **VUNTVS** | 44 | 16 + 4 + 4 + 4 + 4 + 8 + 4 | ✓ | 2 |
| **VNVS** | 36 | 16 + 4 + 4 color + 8 + 4 | ✓ | 2 |
| **VNV** | 24 | 16 + 4 n + 4 color | ✗ | 1 |

- **第二 UV 集（UV1/UV2）在 80 样本中未出现**（`offsets.uv2` 全 0）。
- 三角形索引恒 **u16** → Vulkan 用 `VK_INDEX_TYPE_UINT16`；实测单 partition 最大顶点 **54947** < 65536，16-bit 索引够用
  （但 `numVerts` 本身是 u16，超大网格只能切 shape/partition）。
- `attr 10 Full_Precision`（位置用 f32 全精度变体）样本中未出现，解析器已识别但未遇例。
- 顶点输入速率：同 stride 顶点流 = `VK_VERTEX_INPUT_RATE_VERTEX`；**没有 per-instance 数据**（实例化在场景图层，见 §9）。

---

## §5 材质参数

### 5.1 BSLightingShaderProperty

字段序（实测 sizeOk 全绿，条件尾按 shaderType 分支）：

```
u32 shaderType (BSLightingShaderType) + NiObjectNET (nameIdx i32 / numExtra / controller)
u32 flags1, u32 flags2
f32×2 UVOffset, f32×2 UVScale
i32  textureSet ref
f32×3 emissive + f32 emissiveMultiple
u32  texClampMode (实测 0/1/3 → wrap/clamp/mirror)
f32 alpha, f32 refraction, f32 glossiness
f32×3 specularColor + f32 specularStrength
f32 lighting1, lighting2 (0.3 / 2 之类)
[条件尾] type∈{1,5,6,7,11,14,16} 时追加 EnvMapScale / SkinTint / HairTint /
         MaxPasses / MultiLayer Parallax 参数组 / Sparkle / EyeEnvMapScale
```

`shaderType` 枚举与**实测分布**（`nif.xml` `BSLightingShaderType`，1441 个 shader）：

| 值 | 名称 | 数量 | 触发的条件尾 |
|---|---|---|---|
| 0 | Default | 648 | 无 |
| 1 | Environment Map | 614 | +EnvMapScale(f32)；抽样纹理集 slot4 指向 `cubemaps\*_e.dds` ✓ |
| 2 | Glow | 107 | 无 |
| 5 | Skin Tint | 45 | +SkinTint 12B |
| 11 | MultiLayer Parallax | 20 | +多层视差参数组 |
| 6 | Hair Tint | 5 | +HairTint 12B |
| 16 | Eye Envmap | 2 | +EyeEnvMapScale |

> 条件尾的字节数即“sizeOk 能全绿”的前提 ⇒ 枚举→尾部映射是**被尺寸校验反证过**的，不是照抄文档。

### 5.2 BSShaderTextureSet

`u32 numTextures + SizedString×N`（SSE 常见 N=9；实测 9=1281、1=20、3=4）。

| slot | 用途 | 非空数（1305 集） | 典型路径 |
|---|---|---|---|
| 0 | Diffuse | 1289 | `...\*_d.dds` / `*_basecolor.dds` |
| 1 | Normal(+Gloss) | 1245 | `...\*_n.dds` / `*normal*` |
| 2 | Glow | 176 | `...\*_g.dds` |
| 3 | Height（Parallax） | 12 | |
| 4 | Environment（Cube） | 689 | `textures\cubemaps\*_e.dds` |
| 5 | Environment Mask | 166 | |
| 6 | Subsurface | 20 | |
| 7 | BackLighting | 51 | |
| 8 | （未用） | 0 | |

- 纹理扩展名 **100% `.dds`**（3648 非空槽）。
- **路径两种形态**：相对 `textures\...`（3504）与绝对 `c:\program files\skyrim special edition\data\...`（144）→
  资源加载器必须做归一化（剥盘符/大小写折叠到 Data 目录）。
- Vulkan 映射建议：9 slot → 描述符集按“实际用到的 slot”惰性分配（slot3/8 极少用），采样器地址模式由 `texClampMode` 驱动。

---

## §6 骨骼蒙皮结构

### 6.1 三层结构

```
NiSkinInstance / BSDismemberSkinInstance     ← 谁被蒙皮（骨骼表 + 部位）
    ├ i32 dataRef      (NiSkinData: bind pose 等)
    ├ i32 partitionRef (NiSkinPartition)
    ├ i32 skeletonRoot, u32 numBones, i32 bones[numBones]
    └ BSDismember 追加: u32 numPartitions + BodyPartList(4B×N)
NiSkinPartition                               ← 权重数据 + 子网格
    └ 全局顶点流 + 每子网格的三角形/骨骼/映射
顶点流（在 partition 或 shape 内）             ← 权重与索引就在这里
    └ weights half4 + indices u84（每顶点 4 影响）
```

实测（80 文件）：**NiSkinInstance 457、BSDismemberSkinInstance 397、NiSkinPartition 块 854（旧布局 84）、子网格 3686、骨骼引用合计 19975**。

### 6.2 SSE 布局（bsVersion ≥ 100）

```
u32 numPartitions
u32 dataSize          (= 全局顶点字节)
u32 vertexSize        (= stride，实测与 shape 的 vertexDesc 完全一致)
u64 vertexDesc        (同 §4 解码)
u8  vertexData[dataSize]                 → globalVerts = dataSize / stride
SkinPartition × numPartitions:
    u16 numVerts, u16 numTris, u16 numBones, u16 numStrips, u16 numWeightsPerVertex
    u16 bones[numBones]
    bool hasVertexMap     → u16 vertexMap[numVerts]?
    bool hasVertexWeights → f32 weights[numVerts×numWeightsPerVertex]?
    u16  stripLengths[numStrips]
    bool hasFaces         ← nifxml 标 calc，实测**在流中存储 1 字节**（字节差 1 的算术定位）
    strips(u16 宽=stripLengths) | triangles(u16×3×numTris)
    bool hasBoneIndices   → u8 boneIndices[numVerts×numWeightsPerVertex]?
    u8  lodLevel, bool globalVB          (20.2.0.7 尾)
    u64 vertexDesc, triangles 复制区       (SSE 尾，字节数 = numTris×6)
```

**锚点 `body_0.nif`**：`numPart=1, dataSize=49280, vertexSize=40, desc=0x0005B0007065040A`
（= VUNTS/40B，与 shape 侧一致），子网格 `nv=1232, nt=2186, nb=16, nwpv=4, vertexMap/weights/boneIndices=true`，
`LOD=0, globalVB=0`，消费 102692 == blockSize ✓。

**守恒校验（3686 子网格）**：`numVerts ≤ globalVerts` 违例 **0**；单子网格最大 **54947** 顶点；
子网格顶点合计 7,314,580 / 三角形合计 9,148,692。

### 6.3 权重与索引（与 §4.2 呼应）

`KomAnim\AOKHalloweenS3.nif` partition（stride40、`globalVerts=17805`、`numPartitions=2`、skinning 槽 dword7→字节28）前三个顶点：

```
v0: w=[0.875 0.125 0.000 0.000] sum=1.0000  idx=[2, 3, 13, 1]
v1: w=[1.000 0.000 0.000 0.000] sum=1.0000  idx=[2, 13, 1, 1]
v2: w=[1.000 0.000 0.000 0.000] sum=1.0000  idx=[2, 13, 1, 1]
```

⇒ **权重 4×half、索引 4×u8、每顶点 4 影响、和恒 1**；索引是 `bones[]` 表内下标（partition 的 `numBones` 范围内），
并非骨骼矩阵数组下标 —— 蒙皮时须经 `bones[i]` 间接。

### 6.4 旧流（bsVersion=83，14 文件）

`[u32 numPartitions][SkinPartition…]`：**无** global VB 头（dataSize/vertexSize/vertexDesc）、**无** SSE 尾
（无 per-partition vertexDesc / triangles 复制区），LOD/GlobalVB 视 BS 条件保留；顶点数据在旧式 `NiTriShapeData` 里。
例：`atronachflame.nif`（bs83，`NiTriShape/NiTriShapeData` 老对象），partition 头直接是
`nv=2684, nt=3930, nb=37, ns=0, nwpv=4`，骨骼表 = `[0,1,2,…,36]`（骨架顺序）。
解析器按 bsVersion 分流，旧流用 4 组合探测（hasFaces 存/不存 × 尾字段留/不留）取 sizeOk 命中者。

---

## §7 风险项 #2 评估：蒙皮 / 动画在 Vulkan 下的实现复杂度

**结论：数据格式侧复杂度低（已完全解明），整体“中等、可控、可分阶段规避”；真正的难点是运行时骨骼矩阵的来源，而不是 NIF 数据结构。**

### 7.1 为什么格式侧不难

1. 蒙皮所需数据**全部是静态可读**的：顶点流（位置/法线/权重/索引）+ partition（子网格三角形与骨骼表）+ 骨骼引用表；
2. 顶点格式已给出 **9 种 → Vulkan 属性表**（§4.3），权重/索引的 packing 已逐字节验证（§6.3）；
3. 索引 16-bit、单 partition ≤ 55k 顶点，**顶点/索引缓冲与 `VK_INDEX_TYPE_UINT16` 直接可用**；
4. 材质参数（§5）是普通 uniform/描述符问题，不含动态求值。

### 7.2 真正的未证明点（= 风险的实质）

- **骨骼矩阵来源**：`NiSkinData` 只有 bind pose，**运行时矩阵由游戏动画系统逐帧求值**。
  要在 Vulkan 侧蒙皮，必须从游戏内存（hook / 读 `bones` 对应的求值结果）拿到当前帧矩阵数组，
  这是逆向与同步问题（矩阵数组地址、生命周期、与 draw 的时序对齐），**本调研未验证**。
- **同步成本**：CPU 每帧蒙皮要写顶点缓冲（staging + 传输）；compute 蒙皮要描述符/管线条目 + 与游戏侧资源的屏障。
  实测样本规模 17805/54947 顶点级，未做吞吐压测。

### 7.3 与方案C 阶段的对齐（建议口径）

| 阶段 | 是否触碰蒙皮 | 说明 |
|---|---|---|
| 阶段 0/1（共享纹理、像素级合成） | **不触碰** | 蒙皮仍在游戏 D3D11 侧完成，我方只消费成品像素 → 风险项 #2 **暂不触发** |
| 阶段 2（逐 pass 原生化） | 先做**静态网格** | 建筑/武器/LOD（§4 中 skinned=false 的 843 个 shape）无蒙皮，直接原生化 |
| 阶段 3（特效/蒙皮网格） | 引入蒙皮 | 顺序：**CPU 每帧蒙皮（简单、先通）→ compute 蒙皮（性能）**；前置门槛 = 矩阵来源 hook 验证 |
| 降级 | 蒙皮网格整体留在 D3D11 | 混合方案本就允许多数 draw 仍走游戏管线 → 蒙皮做不出来也不阻塞阶段 1/2 |

**判据建议（写进阶段 2/3 门槛前的验证项）**：先用一个 hook 点拿到“当前帧骨骼矩阵数组”的地址与数量，
与 NIF 的 `bones[]` 表做一次索引对账（数量一致 + 首帧矩阵能复现出 bind pose 附近的姿态），再决定 compute 化。

---

## §8 实测聚合数据（可复查）

| 指标 | 值 |
|---|---|
| 文件 | 80（batch1 40 + batch2 40）；bs100=66、bs83=14 |
| 解析错误 | **0**；块对齐失败 **0**（8 字节 footer 已计入） |
| BSTriShape | 1613（sizeOk 1613）；布局变体：标准 1611、compact sec2 2 |
| 顶点格式 | VUNTS713 / VUNTV399 / VUNT372 / VN71 / VUS45 / VNS8 / VUNTVS2 / VNVS2 / VNV1 |
| shaderType | Default648 / EnvMap614 / Glow107 / SkinTint45 / MultiLayerParallax20 / HairTint5 / EyeEnvmap2 |
| 纹理集 | 1305（numTextures 9=1281、1=20、3=4）；非空槽 3648，全部 `.dds`；相对路径 3504 / 绝对 144 |
| 蒙皮 | NiSkinInstance457 + DSI397；NiSkinPartition 块 854（旧布局 84）；子网格 3686；骨骼引用 19975 |
| 子网格规模 | 顶点合计 7,314,580、三角形合计 9,148,692、单子网格最大 54,947 顶点、`nv>globalVerts` 违例 0 |
| 常见块类型（Top） | NiTransformInterpolator、BSTriShape、NiNode、NiTransformData、BSLightingShaderProperty、NiFloatInterpolator、BSShaderTextureSet |

---

## §9 诚实边界（未覆盖 / 未证明 / 有偏）

1. **未解析**：`NiTriShapeData`（旧式几何顶点区）、`NiParticleSystem/NiPSysData`（粒子）、`BSEffectShaderProperty`
   完整参数、`bhk*` 碰撞、`NiControllerSequence/NiTransformData` 动画求值 —— 都是 v1 范围外，动画面本身未做。
2. **compact sec2 的用途未证**：算术与内容（半精度位置/法线 + 重复三角形）已证，是谁写的、渲染路径是否读它**未证**。
3. **8 字节 footer 语义未定**（三锚点 + 80 样本恒 `01 00 00 00 00 00 00 00`），按 footer 处理不影响块走查。
4. **`hasFaces` 与 nifxml 规格冲突**：nifxml 标 `calc`（不在流），实测 SSE/旧流文件都存 1 字节 —— 属实现与规格差异，
   解析器按实测走（差 1 字节的算法定位 + sizeOk 反证）。
5. **取样有偏**：按文件体积降序取 Top-40×2，偏向大型 mod；小体积 vanilla 网格、`NiTriShapeData` 旧式几何未覆盖。
6. **运行时侧全部未验证**：骨骼矩阵来源、蒙皮吞吐、实例化绘制（场景图变换在 `NiNode` 层，NIF 静态数据里是每节点一个变换）
   —— 见 §7.2/§7.3，属于风险项 #2 的后续验证项，不是本调研已解决项。
7. 第二 UV 集、`Full_Precision`、`Land_Data`、`Eye_Data` 属性位在 80 样本中**零出现**（识别但无实例）。

---

## §10 复现命令

```powershell
# 单文件（打印概览）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\parse-nif.ps1 -Path <file.nif>

# 目录批跑（按体积降序取前 N）→ 聚合 JSON
powershell -NoProfile -ExecutionPolicy Bypass -File tools\parse-nif.ps1 `
    -Path <dir> -First 40 -OutFile report.json

# 字节级回查（JSON 里的 blockIdx / offset 直接可喂）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dump-nif-block.ps1 -File <file.nif> -Idx 9 -MaxHex 152
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dump-nif-block.ps1 -File <file.nif> -Want BSTriShape -List
```

> 踩坑固化：`.ps1` 须 UTF-8 带 BOM（纯 ASCII 可免）；双引号串内 `$var:` 被当盘符作用域 → 用 `${var}`；
> PS 内联 `H` 是 `Get-History` 别名，别拿它当函数名；控制台中文显示为 GBK 乱码，判读文本一律用 read/grep 工具。
