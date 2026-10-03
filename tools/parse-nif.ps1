<#
  parse-nif.ps1 - Skyrim SE NIF (version 20.2.0.7, BS stream 100) extractor.
  Field layout follows niftools/nifxml (nif.xml) for version 20.2.0.7.
  Extracts: header (block type table / string table), per-BSTriShape vertex format
  (BSVertexDesc -> Vulkan-ready vertex input), BSLightingShaderProperty params,
  BSShaderTextureSet textures, NiSkinInstance/BSDismemberSkinInstance bones.

  Usage:
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\parse-nif.ps1 -Path <file.nif>
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\parse-nif.ps1 -Path <dir> -First 30 -OutFile <report.json>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Path,
    [int]$First = 0,
    [string]$OutFile = ''
)

$ErrorActionPreference = 'Stop'

class NifReader {
    [byte[]]$b
    [int]$p
    NifReader([byte[]]$bytes, [int]$start) { $this.b = $bytes; $this.p = $start }
    [byte] U8() { return $this.b[$this.p++] }
    [bool] Bool() { return ($this.b[$this.p++] -ne 0) }
    [uint16] U16() { $v = [BitConverter]::ToUInt16($this.b, $this.p); $this.p += 2; return $v }
    [int16] I16() { $v = [BitConverter]::ToInt16($this.b, $this.p); $this.p += 2; return $v }
    [uint32] U32() { $v = [BitConverter]::ToUInt32($this.b, $this.p); $this.p += 4; return $v }
    [int32] I32() { $v = [BitConverter]::ToInt32($this.b, $this.p); $this.p += 4; return $v }
    [uint64] U64() { $v = [BitConverter]::ToUInt64($this.b, $this.p); $this.p += 8; return $v }
    [single] F32() { $v = [BitConverter]::ToSingle($this.b, $this.p); $this.p += 4; return $v }
    [void] Skip([int]$n) { $this.p += $n }
    [string] SizedString() {
        $len = [BitConverter]::ToInt32($this.b, $this.p); $this.p += 4
        if ($len -lt 0 -or ($this.p + $len) -gt $this.b.Length) { throw "SizedString len=$len out of range" }
        $s = [Text.Encoding]::UTF8.GetString($this.b, $this.p, $len); $this.p += $len
        return $s
    }
    [string] ExportString() {
        $len = $this.b[$this.p++]
        $s = [Text.Encoding]::UTF8.GetString($this.b, $this.p, $len); $this.p += $len
        return $s.TrimEnd([char]0)
    }
}

# ---- BSVertexDesc (uint64 bitfield) decode --------------------------------
function Get-VertexFormat([uint64]$desc) {
    $strideDwords = [int]($desc -band 0xF)
    $stride = $strideDwords * 4
    $attrs = [int](($desc -shr 44) -band 0xFFF)
    $flags = @()
    $names = @('Vertex', 'UVs', 'UVs_2', 'Normals', 'Tangents', 'Vertex_Colors', 'Skinned', 'Land_Data', 'Eye_Data', 'Instance', 'Full_Precision')
    for ($i = 0; $i -lt $names.Count; $i++) { if ($attrs -band (1 -shl $i)) { $flags += $names[$i] } }

    # BSVertexDataSSE per-vertex layout (nif.xml, arg = attrs)
    $comps = @()
    $calc = 0
    if ($attrs -band 0x1) { $comps += @{ name = 'Position'; bytes = 16; format = 'R32G32B32_SFLOAT + pad4' }; $calc += 16 }
    if (($attrs -band 0x2) -ne 0) { $comps += @{ name = 'UV0'; bytes = 4; format = 'R16G16_SFLOAT (half2)' }; $calc += 4 }
    if (($attrs -band 0x4) -ne 0) { $comps += @{ name = 'UV1'; bytes = 4; format = 'R16G16_SFLOAT (half2)' }; $calc += 4 }
    if (($attrs -band 0x8) -ne 0) { $comps += @{ name = 'Normal+BitangentY'; bytes = 4; format = 'R8G8B8A8_UNORM (packed)' }; $calc += 4 }
    if (($attrs -band 0x18) -eq 0x18) { $comps += @{ name = 'Tangent+BitangentZ'; bytes = 4; format = 'R8G8B8A8_UNORM (packed)' }; $calc += 4 }
    if (($attrs -band 0x20) -ne 0) { $comps += @{ name = 'Color0'; bytes = 4; format = 'R8G8B8A8_UNORM' }; $calc += 4 }
    if (($attrs -band 0x40) -ne 0) {
        $comps += @{ name = 'BoneWeights'; bytes = 8; format = 'R16G16B16A16_SFLOAT (4x half)' }
        $comps += @{ name = 'BoneIndices'; bytes = 4; format = 'R8G8B8A8_UINT (4x u8)' }
        $calc += 12
    }
    if (($attrs -band 0x100) -ne 0) { $comps += @{ name = 'EyeData'; bytes = 4; format = 'R32_SFLOAT' }; $calc += 4 }

    return [pscustomobject]@{
        descHex    = '0x{0:X16}' -f $desc
        stride     = $stride
        strideOk   = ($calc -eq $stride)
        calcStride = $calc
        attrsHex   = '0x{0:X3}' -f $attrs
        flags      = $flags
        skinned    = (($attrs -band 0x40) -ne 0)
        fullPrec   = (($attrs -band 0x400) -ne 0)
        offsets    = [pscustomobject]@{
            uv1      = [int](($desc -shr 8) -band 0xF)
            uv2      = [int](($desc -shr 12) -band 0xF)
            normal   = [int](($desc -shr 16) -band 0xF)
            tangent  = [int](($desc -shr 20) -band 0xF)
            color    = [int](($desc -shr 24) -band 0xF)
            skinning = [int](($desc -shr 28) -band 0xF)
        }
        components = $comps
        signature  = ('{0}B {1}' -f $stride, (($flags | ForEach-Object { $_[0] }) -join ''))
    }
}

# ---- shared sub-parsers ---------------------------------------------------
function Read-NifObjectNet([NifReader]$r, [bool]$withShaderType) {
    # NiObjectNET (SSE): [Shader Type (only BSLightingShaderProperty)], Name(u32), NumExtraData(u32)+list, Controller(i32)
    $shaderType = -1
    if ($withShaderType) { $shaderType = [int]$r.U32() }
    $nameIdx = $r.I32()          # int32, -1 = empty name (0xFFFFFFFF must not cast to uint)
    $numExtra = $r.U32()
    if ($numExtra -gt 4096) { throw "NiObjectNET numExtra=$numExtra implausible" }
    for ($i = 0; $i -lt $numExtra; $i++) { [void]$r.I32() }
    $controller = $r.I32()
    return @{ shaderType = $shaderType; nameIdx = $nameIdx; numExtra = $numExtra; controller = $controller }
}

function Read-NiAVObjectBase([NifReader]$r) {
    # NiAVObject (SSE, BSVER>26): NiObjectNET + Flags(u32) + Translation(12) + Rotation(36) + Scale(4) + Collision(i32)
    $net = Read-NifObjectNet $r $false
    $flags = $r.U32()
    $r.Skip(12 + 36 + 4)
    $collision = $r.I32()
    return @{ nameIdx = $net.nameIdx; flags = $flags; collision = $collision }
}

# ---- per-block parsers ----------------------------------------------------
function Parse-BSTriShape([byte[]]$bytes, [int]$start, [int]$size) {
    $r = [NifReader]::new($bytes, $start)
    $base = Read-NiAVObjectBase $r
    $r.Skip(16)                              # NiBound (center 12 + radius 4)
    $skinRef = $r.I32()
    $shaderRef = $r.I32()
    $alphaRef = $r.I32()
    $desc = $r.U64()
    $fmt = Get-VertexFormat $desc
    $posAfterDesc = $r.p - $start

    # SSE layout (byte-verified 2026-10-03 on body_0.nif / dawnbreaker.nif):
    #   [u16 numTris][u16 numVerts][u32 dataSize][vertexData][triangles][u32 tail]
    #   dataSize must equal numVerts*stride + numTris*6; trailing u32 observed = 0.
    # Fallbacks (other exporters): u32 triangle count / dataSize not stored.
    $specs = @(
        @{ name = 'u16 tris + stored dataSize'; trisBytes = 2; storedDs = $true },
        @{ name = 'u32 tris + stored dataSize'; trisBytes = 4; storedDs = $true },
        @{ name = 'u16 tris, dataSize computed'; trisBytes = 2; storedDs = $false },
        @{ name = 'u32 tris, dataSize computed'; trisBytes = 4; storedDs = $false }
    )
    $found = $null
    foreach ($sp in $specs) {
        $q = [NifReader]::new($bytes, $start + $posAfterDesc)
        $nt = 0
        if ($sp.trisBytes -eq 4) { $nt = [int]$q.U32() } else { $nt = [int]$q.U16() }
        $nv = [int]$q.U16()
        if ($nt -gt 400000 -or $nv -gt 400000) { continue }
        $ds = 0
        if ($sp.storedDs) { $ds = [int]$q.U32() } else { $ds = ($fmt.stride * $nv) + ($nt * 6) }
        if ($ds -ne (($fmt.stride * $nv) + ($nt * 6))) { continue }
        if ($ds -gt $size) { continue }
        $after = ($q.p - $start) + $ds
        $tailBytes = $size - $after
        $variantName = $sp.name
        if ($tailBytes -eq 0 -or $tailBytes -eq 4) {
            # standard SSE shape (4-byte trailer observed = 0)
        }
        elseif ($tailBytes -ge 4) {
            # observed variant (waterwheel): [u32 u16Count][half3 pos x nv][half3 normal x nv][triangles identical to main]
            $secCount = [BitConverter]::ToUInt32($bytes, $start + $after)
            if (($secCount * 2 + 4) -eq $tailBytes -and $secCount -eq ((6 * $nv) + (3 * $nt))) {
                $variantName = $sp.name + ' + compact sec2'
            }
            else { continue }
        }
        else { continue }
        $found = [pscustomobject]@{ variant = $variantName; numTris = $nt; numVerts = $nv; dataSize = $ds; tailBytes = $tailBytes }
        break
    }
    if ($null -eq $found) {
        throw "BSTriShape: no layout matches size=$size (posAfterDesc=$posAfterDesc stride=$($fmt.stride))"
    }
    $numTris = $found.numTris
    $numVerts = $found.numVerts

    return [pscustomobject]@{
        type       = 'BSTriShape'
        nameIdx    = $base.nameIdx
        flags      = '0x{0:X}' -f $base.flags
        skinRef    = $skinRef
        shaderRef  = $shaderRef
        alphaRef   = $alphaRef
        numTris    = $numTris
        numVerts   = $numVerts
        dataSize   = $found.dataSize
        tailBytes  = $found.tailBytes
        format     = $fmt
        variant    = $found.variant
        blockSize  = $size
        consumed   = $size
        sizeOk     = $true
    }
}

function Parse-BSShaderTextureSet([byte[]]$bytes, [int]$start, [int]$size) {
    $r = [NifReader]::new($bytes, $start)
    $n = $r.U32()
    if ($n -gt 64) { throw "texture count $n implausible" }
    $tex = @()
    for ($i = 0; $i -lt $n; $i++) { $tex += $r.SizedString() }
    return [pscustomobject]@{
        type = 'BSShaderTextureSet'; numTextures = $n; textures = $tex
        blockSize = $size; consumed = ($r.p - $start); sizeOk = (($r.p - $start) -eq $size)
    }
}

function Parse-BSLightingShaderProperty([byte[]]$bytes, [int]$start, [int]$size) {
    $r = [NifReader]::new($bytes, $start)
    $net = Read-NifObjectNet $r $true
    $shaderType = $net.shaderType
    $flags1 = $r.U32()
    $flags2 = $r.U32()
    $uvOff = @($r.F32(), $r.F32())
    $uvScl = @($r.F32(), $r.F32())
    $texSetRef = $r.I32()
    $emissive = @($r.F32(), $r.F32(), $r.F32())
    $emissiveMul = $r.F32()
    $texClamp = $r.U32()
    $alpha = $r.F32()
    $refraction = $r.F32()
    $gloss = $r.F32()
    $specColor = @($r.F32(), $r.F32(), $r.F32())
    $specStrength = $r.F32()
    $light1 = $r.F32()
    $light2 = $r.F32()
    # conditional tail (SSE, BS stream 100)
    switch ($shaderType) {
        1 { [void]$r.F32() }                 # Environment Map Scale
        5 { $r.Skip(12) }                    # Skin Tint Color
        6 { $r.Skip(12) }                    # Hair Tint Color
        7 { $r.Skip(8) }                     # Max Passes + Scale
        11 { $r.Skip(4 + 4 + 8 + 4) }        # parallax block
        14 { $r.Skip(16) }                   # Sparkle Vector4
        16 { $r.Skip(4 + 12 + 12) }          # Eye cubemap
    }
    $consumed = $r.p - $start
    return [pscustomobject]@{
        type = 'BSLightingShaderProperty'
        shaderType = $shaderType
        nameIdx = $net.nameIdx
        flags1 = '0x{0:X8}' -f $flags1
        flags2 = '0x{0:X4}' -f $flags2
        uvOffset = $uvOff; uvScale = $uvScl
        textureSetRef = $texSetRef
        alpha = [math]::Round($alpha, 4)
        glossiness = [math]::Round($gloss, 4)
        specular = [pscustomobject]@{ color = $specColor; strength = [math]::Round($specStrength, 4) }
        lightingEffects = @([math]::Round($light1, 4), [math]::Round($light2, 4))
        emissive = [pscustomobject]@{ color = $emissive; multiple = [math]::Round($emissiveMul, 4) }
        texClampMode = $texClamp
        blockSize = $size
        consumed = $consumed
        sizeOk = ($consumed -eq $size)
    }
}

function Parse-SkinInstance([byte[]]$bytes, [int]$start, [int]$size, [string]$type) {
    $r = [NifReader]::new($bytes, $start)
    $dataRef = $r.I32()
    $partitionRef = $r.I32()
    $skeletonRoot = $r.I32()
    $numBones = $r.U32()
    if ($numBones -gt 10000) { throw "numBones=$numBones implausible" }
    $bones = @()
    for ($i = 0; $i -lt $numBones; $i++) { $bones += $r.I32() }
    $numPart = 0; $partTypes = @()
    if ($type -eq 'BSDismemberSkinInstance') {
        $numPart = $r.U32()
        if ($numPart -gt 1000) { throw "numPartitions=$numPart implausible" }
        for ($i = 0; $i -lt $numPart; $i++) { $partTypes += [int]$r.U32() }   # BodyPartList elem = 4 bytes
    }
    $consumed = $r.p - $start
    return [pscustomobject]@{
        type = $type; dataRef = $dataRef; partitionRef = $partitionRef
        skeletonRoot = $skeletonRoot; numBones = $numBones; boneRefs = $bones
        numPartitions = $numPart; partitionTypes = $partTypes
        blockSize = $size; consumed = $consumed; sizeOk = ($consumed -eq $size)
    }
}

function Parse-OldSkinPartition([NifReader]$r, [int]$numPart, [bool]$hasFacesStored, [bool]$hasTail) {
    # pre-SSE (bsVersion < 100): no Vertex Desc / Triangles Copy; LOD Level + Global VB when BS_GT_FO3
    $parts = @()
    for ($i = 0; $i -lt $numPart; $i++) {
        $p0 = $r.p
        $nv = [int]$r.U16(); $nt = [int]$r.U16(); $nb = [int]$r.U16(); $ns = [int]$r.U16(); $nwpv = [int]$r.U16()
        if ($nb -gt 256 -or $nwpv -gt 16 -or $nt -gt 400000) { throw "old partition[$i] implausible nb=$nb nt=$nt nwpv=$nwpv" }
        for ($k = 0; $k -lt $nb; $k++) { [void]$r.U16() }
        if ($r.Bool()) { for ($k = 0; $k -lt $nv; $k++) { [void]$r.U16() } }
        if ($r.Bool()) { $r.Skip(4 * $nv * $nwpv) }
        for ($k = 0; $k -lt $ns; $k++) { [void]$r.U16() }
        $hasFaces = $true
        if ($hasFacesStored) { $hasFaces = $r.Bool() }
        if ($ns -ne 0) {
            for ($k = 0; $k -lt $ns; $k++) { $sl = [int]$r.U16(); $r.Skip(2 * $sl) }
        }
        elseif ($hasFaces -and $nt -gt 0) { $r.Skip(6 * $nt) }
        if ($r.Bool()) { $r.Skip($nv * $nwpv) }
        $lodLevel = 0; $globalVB = $false
        if ($hasTail) { $lodLevel = $r.U8(); $globalVB = $r.Bool() }
        $parts += [pscustomobject]@{
            vertices = $nv; triangles = $nt; bones = $nb; strips = $ns; weightsPerVertex = $nwpv
            lodLevel = $lodLevel; globalVB = $globalVB; bytes = ($r.p - $p0)
        }
    }
    return $parts
}

function Parse-NiSkinPartition([byte[]]$bytes, [int]$start, [int]$size, [uint32]$bsVersion) {
    # SSE (bsVersion >= 100, byte-verified): [u32 numPartitions][u32 dataSize][u32 vertexSize]
    #   [u64 vertexDesc][vertexData dataSize][SkinPartition x numPartitions]
    # older stream (bsVersion < 100): [u32 numPartitions][SkinPartition x numPartitions] with no VB/vecdesc tail
    $r = [NifReader]::new($bytes, $start)
    $numPart = $r.U32()
    if ($numPart -gt 1000) { throw "numPartitions=$numPart implausible" }

    if ($bsVersion -ge 100) {
        $dataSize = $r.U32()
        $vertexSize = $r.U32()
        $desc = $r.U64()
        $fmt = Get-VertexFormat $desc
        if ($dataSize -gt $size) { throw "dataSize=$dataSize exceeds block $size" }
        if ($fmt.stride -le 0 -or ($dataSize % $fmt.stride) -ne 0) {
            throw "dataSize=$dataSize not a multiple of stride $($fmt.stride)"
        }
        $r.Skip($dataSize)
        $globalVerts = [int]($dataSize / $fmt.stride)

        $parts = @()
        for ($i = 0; $i -lt $numPart; $i++) {
            $p0 = $r.p
            $nv = [int]$r.U16(); $nt = [int]$r.U16(); $nb = [int]$r.U16(); $ns = [int]$r.U16(); $nwpv = [int]$r.U16()
            if ($nb -gt 256 -or $nwpv -gt 16 -or $nt -gt 400000) { throw "partition[$i] implausible nb=$nb nt=$nt nwpv=$nwpv" }
            for ($k = 0; $k -lt $nb; $k++) { [void]$r.U16() }          # Bones
            $hasVMap = $r.Bool()
            if ($hasVMap) { for ($k = 0; $k -lt $nv; $k++) { [void]$r.U16() } }   # Vertex Map
            $hasVW = $r.Bool()
            if ($hasVW) { $r.Skip(4 * $nv * $nwpv) }                    # Vertex Weights (float32)
            for ($k = 0; $k -lt $ns; $k++) { [void]$r.U16() }           # Strip Lengths
            $hasFaces = $r.Bool()                                       # nif.xml marks calc; SSE files store it (byte-verified)
            if ($ns -ne 0) {                                            # Strips (width = StripLengths)
                for ($k = 0; $k -lt $ns; $k++) { $sl = [int]$r.U16(); $r.Skip(2 * $sl) }
            }
            elseif ($hasFaces -and $nt -gt 0) { $r.Skip(6 * $nt) }      # Triangles
            $hasBI = $r.Bool()
            if ($hasBI) { $r.Skip($nv * $nwpv) }                        # Bone Indices (byte)
            $lodLevel = $r.U8()                                         # 20.2.0.7 only
            $globalVB = $r.Bool()                                       # 20.2.0.7 only
            [void]$r.U64()                                              # per-partition Vertex Desc
            $r.Skip(6 * $nt)                                            # Triangles Copy
            $parts += [pscustomobject]@{
                vertices = $nv; triangles = $nt; bones = $nb; strips = $ns; weightsPerVertex = $nwpv
                vertexMap = $hasVMap; vertexWeights = $hasVW; boneIndices = $hasBI
                lodLevel = $lodLevel; globalVB = $globalVB; bytes = ($r.p - $p0)
            }
        }
        $consumed = $r.p - $start
        if ($consumed -ne $size) { throw "SSE partition layout consumed=$consumed size=$size" }
        return [pscustomobject]@{
            type = 'NiSkinPartition'; layout = 'SSE (bs>=100, global VB)'; numPartitions = $numPart
            dataSize = $dataSize; vertexSize = $vertexSize; stride = $fmt.stride; globalVerts = $globalVerts
            format = $fmt; partitions = $parts
            blockSize = $size; consumed = $consumed; sizeOk = $true
        }
    }

    # old stream: probe layout combos
    foreach ($hf in @($true, $false)) {
        foreach ($ht in @($true, $false)) {
            $r2 = [NifReader]::new($bytes, $start + 4)
            try { $parts = Parse-OldSkinPartition $r2 $numPart $hf $ht } catch { continue }
            if (($r2.p - $start) -eq $size) {
                return [pscustomobject]@{
                    type = 'NiSkinPartition'; layout = 'old (bs<100)'; numPartitions = $numPart
                    dataSize = 0; vertexSize = 0; stride = 0; globalVerts = 0
                    format = $null; partitions = $parts
                    hasFacesStored = $hf; tailFields = $ht
                    blockSize = $size; consumed = ($r2.p - $start); sizeOk = $true
                }
            }
        }
    }
    throw "old (bs<100) partition layout: no combo consumed $size bytes"
}

# ---- file driver ----------------------------------------------------------
function Parse-NifFile([string]$file) {
    $bytes = [IO.File]::ReadAllBytes($file)
    $r = [NifReader]::new($bytes, 0)

    # magic: ASCII up to and including 0x0A
    $magicStart = 0
    while ($r.p -lt $bytes.Length -and $bytes[$r.p] -ne 0x0A) { $r.p++ }
    $magic = [Text.Encoding]::ASCII.GetString($bytes, $magicStart, $r.p - $magicStart)
    $r.p++                                       # consume 0x0A

    $version = $r.U32()
    $endian = $r.U8()
    $userVersion = $r.U32()
    $numBlocks = $r.U32()
    if ($numBlocks -gt 200000) { throw "numBlocks=$numBlocks implausible" }
    $bsVersion = $r.U32()
    $author = $r.ExportString()
    $processScript = ''; $exportScript = ''; $maxFilepath = ''
    if ($bsVersion -lt 131) { $processScript = $r.ExportString() }
    $exportScript = $r.ExportString()
    if ($bsVersion -ge 103) { $maxFilepath = $r.ExportString() }

    $numBlockTypes = $r.U16()
    $blockTypes = @()
    for ($i = 0; $i -lt $numBlockTypes; $i++) { $blockTypes += $r.SizedString() }

    $typeIdx = @()
    for ($i = 0; $i -lt $numBlocks; $i++) { $typeIdx += [int]$r.U16() }

    $blockSizes = @()
    $hasBlockSizes = ($version -ge 0x14020005)   # since 20.2.0.5
    if ($hasBlockSizes) { for ($i = 0; $i -lt $numBlocks; $i++) { $blockSizes += [int]$r.U32() } }

    $numStrings = $r.U32()
    if ($numStrings -gt 1000000) { throw "numStrings=$numStrings implausible" }
    $maxStringLength = $r.U32()
    $strings = @()
    for ($i = 0; $i -lt $numStrings; $i++) { $strings += $r.SizedString() }

    # trailing: Num Groups (u32, since 5.0.0.6) - auto detect so block data aligns
    $sumSizes = 0
    for ($i = 0; $i -lt $blockSizes.Count; $i++) { $sumSizes += $blockSizes[$i] }
    $headerEndNoGroups = $r.p
    $headerEnd = $r.p + 4
    $groupsSize = 4
    $aligned = $false
    $footer = $bytes.Length - ($headerEnd + $sumSizes)
    if ($footer -eq 0) { $aligned = $true }
    elseif (($headerEndNoGroups + $sumSizes) -eq $bytes.Length) { $aligned = $true; $headerEnd = $headerEndNoGroups; $groupsSize = 0; $footer = 0 }
    elseif ($footer -eq 8) {
        # all inspected samples end with an 8-byte trailer (u32 1, u32 0) outside block data
        $aligned = $true
    }

    $result = [ordered]@{
        file           = $file
        sizeBytes      = $bytes.Length
        magic          = $magic.Trim()
        version        = '0x{0:X8}' -f $version
        endian         = $endian
        userVersion    = $userVersion
        bsVersion      = $bsVersion
        author         = $author
        processScript  = $processScript
        exportScript   = $exportScript
        numBlocks      = $numBlocks
        numStrings     = $numStrings
        maxStringLength = $maxStringLength
        blockSizesPresent = $hasBlockSizes
        headerEnd      = $headerEnd
        blockDataAligned = $aligned
        groupsTail     = $groupsSize
        fileFooter     = $footer
        blockTypeHistogram = @()
        interestingStrings = @()
        geometry       = @()
        shaders        = @()
        textureSets    = @()
        skins          = @()
        errors         = @()
    }

    # block type histogram
    $hist = @{}
    for ($i = 0; $i -lt $numBlocks; $i++) {
        $t = $blockTypes[$typeIdx[$i]]
        if ($hist.ContainsKey($t)) { $hist[$t]++ } else { $hist[$t] = 1 }
    }
    $result.blockTypeHistogram = @($hist.GetEnumerator() | Sort-Object -Property Value -Descending |
        ForEach-Object { [pscustomobject]@{ type = $_.Key; count = $_.Value } })

    # strings that look like paths / shader names
    $result.interestingStrings = @($strings | Where-Object { $_ -match '\.[dD][dD][sS]|\.nif|\.tga|meshes\\|textures\\|shaders\\' } |
        Select-Object -First 40)

    # walk blocks
    $offset = $headerEnd
    for ($i = 0; $i -lt $numBlocks; $i++) {
        $type = $blockTypes[$typeIdx[$i]]
        $size = 0
        if ($hasBlockSizes -and $i -lt $blockSizes.Count) { $size = $blockSizes[$i] }
        if ($size -lt 0 -or ($offset + $size) -gt $bytes.Length) {
            $result.errors += "block[$i] $type size=$size out of range"
            break
        }
        try {
            switch ($type) {
                'BSTriShape' {
                    $g = Parse-BSTriShape $bytes $offset $size
                    $g | Add-Member -NotePropertyName blockIdx -NotePropertyValue $i -Force
                    $g | Add-Member -NotePropertyName offset -NotePropertyValue $offset -Force
                    $result.geometry += $g
                }
                'BSSubIndexTriShape' {
                    $g = Parse-BSTriShape $bytes $offset $size
                    $g | Add-Member -NotePropertyName type -NotePropertyValue 'BSSubIndexTriShape' -Force
                    $g | Add-Member -NotePropertyName blockIdx -NotePropertyValue $i -Force
                    $g | Add-Member -NotePropertyName offset -NotePropertyValue $offset -Force
                    $result.geometry += $g
                }
                'BSShaderTextureSet' { $result.textureSets += (Parse-BSShaderTextureSet $bytes $offset $size) }
                'BSLightingShaderProperty' { $result.shaders += (Parse-BSLightingShaderProperty $bytes $offset $size) }
                'NiSkinInstance' { $s = Parse-SkinInstance $bytes $offset $size 'NiSkinInstance'; $s | Add-Member -NotePropertyName blockIdx -NotePropertyValue $i -Force; $s | Add-Member -NotePropertyName offset -NotePropertyValue $offset -Force; $result.skins += $s }
                'BSDismemberSkinInstance' { $s = Parse-SkinInstance $bytes $offset $size 'BSDismemberSkinInstance'; $s | Add-Member -NotePropertyName blockIdx -NotePropertyValue $i -Force; $s | Add-Member -NotePropertyName offset -NotePropertyValue $offset -Force; $result.skins += $s }
                'NiSkinPartition' { $s = Parse-NiSkinPartition $bytes $offset $size $bsVersion; $s | Add-Member -NotePropertyName blockIdx -NotePropertyValue $i -Force; $s | Add-Member -NotePropertyName offset -NotePropertyValue $offset -Force; $result.skins += $s }
            }
        }
        catch {
            $result.errors += "block[$i] ${type}: $($_.Exception.Message)"
        }
        $offset += $size
    }
    $result.blocksAfterWalk = $offset
    $result.walkEndMatchesFile = ($offset -eq $bytes.Length)
    return [pscustomobject]$result
}

# ---- main -----------------------------------------------------------------
$files = @()
if (Test-Path $Path -PathType Container) {
    $files = @(Get-ChildItem -Path $Path -Recurse -Filter *.nif -File | Sort-Object Length -Descending)
    if ($First -gt 0) { $files = $files | Select-Object -First $First }
}
elseif (Test-Path $Path) { $files = @(Get-Item $Path) }
else { throw "path not found: $Path" }

$reports = @()
$fmtAgg = @{}; $typeAgg = @{}; $shaderTypeAgg = @{}; $variantAgg = @{}; $errAgg = @()
foreach ($f in $files) {
    try {
        $rep = Parse-NifFile $f.FullName
        $reports += $rep
        foreach ($g in $rep.geometry) {
            $sig = '{0}|{1}|skinned={2}' -f $g.format.signature, $g.format.stride, $g.format.skinned
            if ($fmtAgg.ContainsKey($sig)) { $fmtAgg[$sig]++ } else { $fmtAgg[$sig] = 1 }
            $vn = [string]$g.variant
            if ($variantAgg.ContainsKey($vn)) { $variantAgg[$vn]++ } else { $variantAgg[$vn] = 1 }
        }
        foreach ($kv in $rep.blockTypeHistogram) {
            if ($typeAgg.ContainsKey($kv.type)) { $typeAgg[$kv.type] += $kv.count } else { $typeAgg[$kv.type] = $kv.count }
        }
        foreach ($s in $rep.shaders) {
            $k = [string]$s.shaderType
            if ($shaderTypeAgg.ContainsKey($k)) { $shaderTypeAgg[$k]++ } else { $shaderTypeAgg[$k] = 1 }
        }
        if ($rep.errors.Count -gt 0) { $errAgg += ($f.Name + ': ' + ($rep.errors -join '; ')) }
        $bad = @($rep.geometry | Where-Object { -not $_.sizeOk }).Count
        $badS = @($rep.shaders | Where-Object { -not $_.sizeOk }).Count
        $badT = @($rep.textureSets | Where-Object { -not $_.sizeOk }).Count
        $badK = @($rep.skins | Where-Object { -not $_.sizeOk }).Count
        Write-Output ("{0,-46} blocks={1,-4} geo={2,-2}(OK {3}) shd={4,-2}(OK {5}) tex={6,-2}(OK {7}) skin={8,-2}(OK {9}) align={10} err={11}" -f `
            $f.Name, $rep.numBlocks, $rep.geometry.Count, ($rep.geometry.Count - $bad), `
            $rep.shaders.Count, ($rep.shaders.Count - $badS), $rep.textureSets.Count, ($rep.textureSets.Count - $badT), `
            $rep.skins.Count, ($rep.skins.Count - $badK), $rep.blockDataAligned, $rep.errors.Count)
    }
    catch {
        $errAgg += ($f.Name + ': FATAL ' + $_.Exception.Message)
        Write-Output ("{0,-46} FATAL: {1}" -f $f.Name, $_.Exception.Message)
    }
}

$report = [pscustomobject]@{
    generated           = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
    tool                = 'tools/parse-nif.ps1 (layout ref: niftools/nifxml, NIF 20.2.0.7 / BS 100)'
    fileCount           = $reports.Count
    vertexFormatAgg     = @($fmtAgg.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { [pscustomobject]@{ signature = $_.Key; count = $_.Value } })
    layoutVariantAgg    = @($variantAgg.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { [pscustomobject]@{ variant = $_.Key; count = $_.Value } })
    blockTypeAgg        = @($typeAgg.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 40 | ForEach-Object { [pscustomobject]@{ type = $_.Key; count = $_.Value } })
    shaderTypeAgg       = @($shaderTypeAgg.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { [pscustomobject]@{ shaderType = $_.Key; count = $_.Value } })
    errors              = $errAgg
    files               = $reports
}

$json = $report | ConvertTo-Json -Depth 8
if ($OutFile) {
    $dir = Split-Path -Parent $OutFile
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    [IO.File]::WriteAllText($OutFile, $json, (New-Object Text.UTF8Encoding($false)))
    Write-Output "wrote $OutFile"
}
else { Write-Output $json }
