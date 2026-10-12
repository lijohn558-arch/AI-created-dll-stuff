# rdc_pass8_sky.py -- 天空节点离线侦察 (Plan 2026-10-12 天空/大气/体积云 T1)
#
# 回答三问, 全部只读、不改帧:
#   Q1 定位  段7 (主不透明 MRT×3: RT0=1920x1080 RGBA16F + RT1=R16G16 + RT2=RGBA8 + DSV=1920x1080 D24S8)
#         里哪些 draw 是天空? -> 逐 draw 状态表 (事件/视口/深度写掩码/PS id/绑点) + 头部与尾部
#         若干 draw 的 PS 反射 (看是不是天空着色器: 无贴图 / 无法线图 / 只吃 per-frame CB)
#   Q2 硬事实一  天空像素的深度值是不是远平面? -> 在上 1/3 固定采样点回读 DSV(461) 深度,
#         三个时点对比: 深度清之后 / 段7 首 draw / 段7 末 draw; 同时回读 RT0(321) 同点颜色
#   Q3 硬事实二  "段7 结束" 能不能用运行时可见的结构字段判? -> 段7 前后每个 draw 的
#         (NumViews / RT0..RT3 资源指针 + desc / DSV 资源 + desc / 视口) 差分
#
# 段7 **不硬编码事件区间** —— 三个场景事件号差 2 万 (S1 34760 / S3 17771 / S5 29981), 且 S1 的
# RT2 是 342 而 S3/S5 是 339 ⇒ 按绑定签名自动定位 (本版 RenderDoc 只见 pass5/pass6 同款 API)。
#
# Env: RDC_SCENE (S1/S2/S3/S4/S5, 默认 S1); RDC_STRIDE (逐 draw 步长, 默认 1)
# Output: docs/analysis/<prefix>-pass8-sky.json (+ .txt 人读表)
#
# 判定口径与项目一致: errors=[] 才算本轮成功 (rdc_run.ps1 自动读 errors 数组)。
# 用法:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_pass8_sky.py -Scene S1

import json
import os
import struct
import sys
import time

import renderdoc as rd

_SCENES = {"S1": "S1-city-day", "S2": "S2-interior", "S3": "S3-forest-godrays",
           "S4": "S4-water", "S5": "S5-night-combat"}
_SCENE = os.environ.get("RDC_SCENE", "").strip() or "S1"
_FULL = _SCENES.get(_SCENE, _SCENE)
_PREFIX = _FULL.split("-")[0]
RDC = r"C:\Users\joker\skyrim-vulkan\captures\%s.rdc" % _FULL
ANALYSIS = r"C:\Users\joker\skyrim-vulkan\docs\analysis"
OUT_JSON = os.path.join(ANALYSIS, "%s-pass8-sky.json" % _PREFIX)
OUT_TXT = os.path.join(ANALYSIS, "%s-pass8-sky.txt" % _PREFIX)
try:
    STRIDE = max(1, int(os.environ.get("RDC_STRIDE", "1")))
except Exception:
    STRIDE = 1

# 段7 绑定签名 (格式名子串, 去空格大写比对 —— 本版 ResourceFormat 无 .name, 用 str() 口径)
SEG7_RT0_FMT = "R16G16B16A16_FLOAT"
SEG7_RT1_FMT = "R16G16_FLOAT"
SEG7_RT2_FMT = "R8G8B8A8_UNORM"
SEG7_DSV_FMT = "D24"
SEG7_W, SEG7_H = 1920, 1080

# 固定采样点 (屏幕坐标, RenderDoc 顶层原点 y 向下)。上 1/3 = y<360。
# 这几个点是 brief 指定的"上 1/3 中点等 3-5 点"固定点 —— 用途是**逐时点纵向对比**
# (清后 / 段7 首 draw / 段7 末 draw), 证明深度值在段7 期间被写过。
# 天空像素本身**不做硬编码假设** —— S1 的天空只在画面左上角一条 (Screenshot_CITY.png
# 核对过: 上 1/3 绝大多数是屋顶/石墙), 所以另跑一遍全屏深度统计找真·远平面像素集。
SKY_PTS = [("fixed-upper-third-mid", 960, 180),
           ("fixed-top-center", 960, 60),
           ("fixed-upper-left", 480, 270),
           ("fixed-upper-right", 1440, 270),
           ("fixed-upper-left-corner", 60, 90)]
CTRL_PTS = [("ctrl-lower-center", 960, 900),
            ("ctrl-lower-left", 200, 1000),
            ("ctrl-right-mid", 1800, 540),
            ("ctrl-top-right-corner", 1880, 40)]
ALL_PTS = SKY_PTS + CTRL_PTS
FAR_THRESHOLD = 0.9999   # D24 深度 1.0 精确值 = 0xFFFFFF; >= 此值视作"没被任何几何写过"

report = {"rdc": RDC, "scene": _SCENE, "full": _FULL, "script": "rdc_pass8_sky v1",
          "errors": [], "probes": [], "verdict": "UNKNOWN"}
LINES = []
t0 = time.time()
ctrl = None
capf = None
# 逐行日志 —— qrenderdoc 是 GUI 进程, stdout 未必能被父进程接住, 直接落盘保底
LOGP = os.path.join(ANALYSIS, "%s-pass8-sky.log" % _PREFIX)


def note(s):
    LINES.append(s)
    print(s)
    try:
        with open(LOGP, "a", encoding="utf-8", newline="\n") as lf:
            lf.write(str(s) + "\n")
    except Exception:
        pass


def note_err(sec, e):
    msg = "%s: %s: %s" % (sec, type(e).__name__, e)
    report["errors"].append(msg)
    note("[错误] " + msg)


def clean(rid):
    return str(rid).replace("ResourceId::", "")


def enum_int(name):
    try:
        return int(getattr(rd.ActionFlags, name))
    except Exception:
        return None


def fmt_name(f):
    if f is None:
        return "?"
    for attr in ("name", "Name", "typeName"):
        v = getattr(f, attr, None)
        if callable(v):
            try:
                v = v()
            except Exception:
                v = None
        if v:
            return str(v)
    return str(f)


def half_to_float(h):
    """IEEE754 binary16 -> python float (自写解码, 不依赖 struct 'e' 格式可用性)"""
    s = (h >> 15) & 1
    e = (h >> 10) & 0x1F
    m = h & 0x3FF
    if e == 0:
        v = float(m) * (2.0 ** -24)
    elif e == 31:
        v = float("inf") if m == 0 else float("nan")
    else:
        v = (1.0 + float(m) / 1024.0) * (2.0 ** (e - 15))
    return -v if s else v


def read_pixels(rid, w, h):
    """GetTextureData 全量回读 -> (bytes, row_pitch, bytes_per_px)。空 = 读不到。"""
    data = ctrl.GetTextureData(rid, rd.Subresource(0, 0, 0))
    if not data:
        return None, 0, 0
    data = bytes(data)
    pitch = len(data) // h if h else 0
    bpp = (len(data) / float(w * h)) if (w and h) else 0.0
    return data, pitch, bpp


def depth_raw_u32(data, pitch, x, y):
    """D24S8 深度面回读 = 每 texel 4 字节, 低 24 位小端为 D24 深度值 (高 8 位是 stencil,
    本项目不关心)。探针实测: 按 float32 解出来是 2.3e-38 量级 (典型的 uint32 位型被当
    浮点读), 按 D24 解出来是 0.94~1.0 的合理深度 ⇒ 一律走 D24。"""
    off = y * pitch + x * 4
    if off + 4 > len(data):
        return None
    u = struct.unpack_from("<I", data, off)[0]
    return (u & 0xFFFFFF, (u >> 24) & 0xFF)


def depth_at(data, pitch, bpp, x, y):
    """深度纹理取样。bpp==4 -> D24 打包 (低 24 位, 高 8 位 stencil); bpp==3 -> D24 三字节。"""
    if bpp == 4.0:
        r = depth_raw_u32(data, pitch, x, y)
        if r is None:
            return None, "越界"
        return r[0] / float(1 << 24), "d24-u32"
    if bpp == 3.0:
        off = y * pitch + x * 3
        if off + 3 > len(data):
            return None, "越界"
        u = data[off] | (data[off + 1] << 8) | (data[off + 2] << 16)
        return u / float(1 << 24), "d24-p24"
    return None, "未支持 bpp=%.3f" % bpp


def color_at(data, pitch, bpp, x, y):
    """颜色取样。stride 必须按 bpp 算: RGBA16F = 8 B/texel (half x4), RGBA8 = 4 B/texel。
    (初版写成 x*4 恒定, 对 RGBA16F 会每像素偏移半个 texel —— 已修, 别改回去。)"""
    if bpp == 8.0:
        off = y * pitch + x * 8
        if off + 8 > len(data):
            return None
        hs = struct.unpack_from("<4H", data, off)
        return [round(half_to_float(v), 5) for v in hs]
    if bpp == 4.0:
        off = y * pitch + x * 4
        if off + 4 > len(data):
            return None
        return [round(v / 255.0, 5) for v in struct.unpack_from("<4B", data, off)]
    return None


note("=== 天空节点离线侦察 ===  %s" % time.strftime("%H:%M:%S"))
note("路径: " + RDC)
report["scene_info"] = {
    "stride": STRIDE,
    "sky_points": [{"label": a, "x": b, "y": c} for a, b, c in SKY_PTS],
    "ctrl_points": [{"label": a, "x": b, "y": c} for a, b, c in CTRL_PTS],
    "seg7_signature": {"rt0": SEG7_RT0_FMT, "rt1": SEG7_RT1_FMT, "rt2": SEG7_RT2_FMT,
                       "dsv": SEG7_DSV_FMT, "wh": [SEG7_W, SEG7_H]},
}

# ===== 1. 开抓帧 (与 rdc_pass7_pixels 同范式) =====
try:
    capf = rd.OpenCaptureFile()
    st = None
    for ud in ("", b""):
        try:
            st = capf.OpenFile(RDC, ud, None)
            break
        except Exception:
            st = None
    if st is None or "fail" in str(st).lower() or "error" in str(st).lower():
        note_err("OpenFile", "st=%r" % (st,))
        raise SystemExit(2)
    res = capf.OpenCapture(rd.ReplayOptions(), None)
    ctrl = res[1] if (isinstance(res, tuple) and len(res) == 2) else res
except SystemExit:
    raise
except Exception as e:
    note_err("OpenCapture", e)
if ctrl is None:
    note_err("OpenCapture", "未能创建 replay controller")
    raise SystemExit(3)

# ===== 2. 纹理全表 (格式/尺寸索引, 与 pass6 build_tex_map 同口径) =====
TEX = {}
try:
    for t in ctrl.GetTextures():
        rid = getattr(t, "resourceId", None)
        if rid is None:
            continue
        TEX[clean(rid)] = {"w": int(getattr(t, "width", 0)), "h": int(getattr(t, "height", 0)),
                           "fmt": fmt_name(getattr(t, "format", None)).replace(" ", ""),
                           "type": str(getattr(t, "type", "?")),
                           "array": int(getattr(t, "arraySize", 1) or 1),
                           "mips": int(getattr(t, "mips", 1) or 1)}
    note("纹理索引: %d 条" % len(TEX))
except Exception as e:
    note_err("GetTextures", e)


def tinfo(rid):
    d = TEX.get(clean(rid))
    if not d:
        return "?"
    return "%dx%d %s" % (d["w"], d["h"], d["fmt"])


# ===== 3. 动作树 -> 全部 draw / clear 事件 =====
flat = []
draws = []
clears = []
try:
    def walk(actions):
        for a in actions:
            flat.append(a)
            try:
                if a.children:
                    walk(a.children)
            except Exception:
                pass

    walk(ctrl.GetRootActions())
    f_draw = enum_int("Drawcall")
    for a in flat:
        try:
            af = int(a.flags)
        except Exception:
            af = 0
        if f_draw and (af & f_draw):
            draws.append(a)
        if enum_int("Clear") and (af & enum_int("Clear")):
            clears.append(a)
    note("事件总数 %d | Draw %d | Clear %d" % (len(flat), len(draws), len(clears)))
except Exception as e:
    note_err("动作树", e)

f_clear_flag = enum_int("Clear")
act_by_id = {int(a.eventId): a for a in flat}


def read_pipe(eid):
    """SetFrameEvent + 取管线状态快照 (RTs / DSV / 视口 / 深度态 / blend态 / PS / VS / SRVs)。"""
    ctrl.SetFrameEvent(eid, True)
    pipe = ctrl.GetPipelineState()
    rec = {"eventId": int(eid)}
    rec["rts"] = [clean(d.resource) for d in pipe.GetOutputTargets()]
    dt = pipe.GetDepthTarget()
    rec["dsv"] = clean(dt.resource) if dt is not None else "0"
    rec["rt_fmt"] = [tinfo(d.resource) for d in pipe.GetOutputTargets()[:3]]
    rec["dsv_fmt"] = tinfo(dt.resource) if dt is not None else "-"
    rec["numViews"] = sum(1 for x in rec["rts"] if x != "0")
    try:
        v = pipe.GetViewport(0)
        rec["viewport"] = "none" if v is None else ("%gx%g@%g,%g" %
                                                   (v.width, v.height, v.x, v.y))
    except Exception:
        rec["viewport"] = "?"
    try:
        s = pipe.GetScissor(0)
        rec["scissor"] = "none" if s is None else ("%d,%d %dx%d" % (s.x, s.y, s.width, s.height))
    except Exception:
        rec["scissor"] = "?"
    try:
        dts = pipe.GetDepthTestState()
        rec["depthEnable"] = bool(getattr(dts, "depthEnable", False))
        rec["depthWrites"] = bool(getattr(dts, "depthWrites", False))
        rec["depthCompare"] = str(getattr(dts, "depthCompare", "?")).split(".")[-1]
    except Exception as e:
        note_err("depth@%d" % eid, e)
        rec["depthEnable"] = rec["depthWrites"] = None
        rec["depthCompare"] = "?"
    try:
        cbs = pipe.GetColorBlends()
        seq = list(cbs) if cbs is not None else []
        if seq:
            cb = seq[0]
            rec["blendEnabled"] = bool(getattr(cb, "enabled", False))
            rec["blendWriteMask"] = int(getattr(cb, "writeMask", -1))
            rec["blendSlots"] = len(seq)
        else:
            rec["blendEnabled"] = None
            rec["blendWriteMask"] = None
            rec["blendSlots"] = 0
    except Exception as e:
        note_err("blend@%d" % eid, e)
        rec["blendEnabled"] = None
        rec["blendWriteMask"] = None
        rec["blendSlots"] = None
    try:
        rec["ps"] = clean(pipe.GetShader(rd.ShaderStage.Pixel))
    except Exception:
        rec["ps"] = "0"
    try:
        rec["vs"] = clean(pipe.GetShader(rd.ShaderStage.Vertex))
    except Exception:
        rec["vs"] = "0"
    srvs = []
    try:
        # UsedDescriptor 口径 = pass6 desc_rec: 槽位走 .access.index, 资源走 .descriptor.resource
        for x in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel):
            slot = -1
            try:
                slot = int(x.access.index)
            except Exception:
                pass
            res = None
            try:
                res = x.descriptor.resource
            except Exception:
                pass
            if res is None:
                continue
            srvs.append({"slot": slot, "id": clean(res), "info": tinfo(res),
                         "type": str(getattr(getattr(x, "access", None), "type", "?")).split(".")[-1]})
    except Exception as e:
        note_err("srv@%d" % eid, e)
    rec["srvPS"] = srvs
    return rec


# 全帧 draw 的 RT/DSV 绑定签名扫描。SetFrameEvent 是本脚本最贵的一步 (S1 全帧 4518 draw),
# 所以粗->细两遍: 先按步长 COARSE 扫 (force=False, 不强制重放), 命中段7 的 draw 索引附近
# 再按步长 1 细扫 ±COARSE, 补齐段边界。逐 draw 的完整管线状态留到定位之后再取。
COARSE = 16


def is_seg7(r):
    """段7 绑定签名: RT0=1920x1080 RGBA16F + RT1=R16G16 + RT2=RGBA8 + DSV=1920x1080 D24。"""
    return (r["rt0f"] == SEG7_RT0_FMT and r["rt1f"] == SEG7_RT1_FMT and
            r["rt2f"] == SEG7_RT2_FMT and SEG7_DSV_FMT in r["dsvf"] and
            TEX.get(r["rt0"], {}).get("w") == SEG7_W and
            TEX.get(r["rt0"], {}).get("h") == SEG7_H and
            TEX.get(r["dsv"], {}).get("w") == SEG7_W and
            TEX.get(r["dsv"], {}).get("h") == SEG7_H)


def sig_at(idx):
    """第 idx 个 draw 的绑定签名。"""
    a = draws[idx]
    eid = int(a.eventId)
    ctrl.SetFrameEvent(eid, False)
    pipe = ctrl.GetPipelineState()
    rts = [clean(d.resource) for d in pipe.GetOutputTargets()]
    dt = pipe.GetDepthTarget()
    dsv = clean(dt.resource) if dt is not None else "0"
    return {"i": idx, "ev": eid, "rts": rts[:4],
            "rt0": rts[0] if rts else "0",
            "rt1": rts[1] if len(rts) > 1 else "0",
            "rt2": rts[2] if len(rts) > 2 else "0",
            "dsv": dsv,
            "rt0f": TEX.get(rts[0], {}).get("fmt", "?") if rts else "?",
            "rt1f": TEX.get(rts[1], {}).get("fmt", "?") if len(rts) > 1 else "?",
            "rt2f": TEX.get(rts[2], {}).get("fmt", "?") if len(rts) > 2 else "?",
            "dsvf": TEX.get(dsv, {}).get("fmt", "?"),
            "ps": clean(pipe.GetShader(rd.ShaderStage.Pixel)),
            "vs": clean(pipe.GetShader(rd.ShaderStage.Vertex))}


note("粗扫全帧 draw 绑定签名 (draws=%d, stride=%d) ..." % (len(draws), COARSE))
sig_by_idx = {}
try:
    for i in range(0, len(draws), COARSE):
        try:
            sig_by_idx[i] = sig_at(i)
        except Exception as e:
            note_err("sig-coarse@%d" % i, e)
except Exception as e:
    note_err("sig 粗扫", e)

note("细扫命中段附近的 draw ...")
hits = [i for i, r in sig_by_idx.items() if is_seg7(r)]
report["seg7_coarse_hits"] = len(hits)
note("粗扫命中段7 采样点: %d 个 (首=%s 末=%s)"
     % (len(hits), hits[0] if hits else "-", hits[-1] if hits else "-"))
fine_set = set()
if hits:
    # 向前回溯: 从段7 首 draw 往前扫, 直到 DSV 不再是段7 的那个 DSV 为止 —— 那段连续区间
    # 就是**深度预通道**(段5)。不这么做的话 sig_rows 里没有预通道的 draw, 后面
    # "某 VS 是否也走过预通道" 的对照会得到假的 False。
    back = hits[0] - 1
    back_limit = max(0, hits[0] - 2500)
    while back >= back_limit:
        try:
            r = sig_by_idx.get(back) or sig_at(back)
            sig_by_idx[back] = r
        except Exception as e:
            note_err("sig-back@%d" % back, e)
            break
        if sig_by_idx[back]["dsv"] != sig_by_idx[hits[0]]["dsv"]:
            back += 1
            break
        back -= 1
    lo = max(0, min(back, hits[0] - COARSE))
    hi = min(len(draws) - 1, hits[-1] + COARSE)
    for i in range(lo, hi + 1):
        if i in sig_by_idx:
            fine_set.add(i)
        else:
            try:
                sig_by_idx[i] = sig_at(i)
                fine_set.add(i)
            except Exception as e:
                note_err("sig-fine@%d" % i, e)
    note("细扫区间 draw 索引 %d..%d (%d 个, 含回溯到的预通道)" % (lo, hi, len(fine_set)))
else:
    note_err("seg7", "粗扫未命中段7 签名")
sig_rows = [sig_by_idx[i] for i in sorted(sig_by_idx)]

# ===== 4. 段7 定位: 最长连续满足签名的 draw 段 =====
runs = []
cur = []
for r in sig_rows:
    if is_seg7(r):
        cur.append(r)
    else:
        if cur and (not runs or len(cur) > len(runs[0])):
            runs.append(cur)
        cur = []
if cur and (not runs or len(cur) > len(runs[0])):
    runs.append(cur)
report["seg7_runs_found"] = len(runs)
note("段7 签名命中段数: %d" % len(runs))
for k, rn in enumerate(runs):
    note("  候选%d: ev%d..ev%d draws=%d" % (k + 1, rn[0]["ev"], rn[-1]["ev"], len(rn)))

if not runs:
    report["verdict"] = "SEG7_NOT_FOUND"
    note_err("seg7", "全帧未找到满足 MRT3+DSV461 签名的连续 draw 段")
else:
    seg = runs[0]
    seg7 = {"ev_start": seg[0]["ev"], "ev_end": seg[-1]["ev"], "draws": len(seg),
            "rt0": seg[0]["rt0"], "rt1": seg[0]["rt1"], "rt2": seg[0]["rt2"],
            "dsv": seg[0]["dsv"],
            "rt_infos": [TEX.get(seg[0][k], {}).get("fmt", "?") for k in ("rt0", "rt1", "rt2")],
            "dsv_info": TEX.get(seg[0]["dsv"], {}).get("fmt", "?")}
    report["seg7"] = seg7
    report["seg7"]["ps_list"] = sorted(set(x["ps"] for x in seg), key=lambda v: int(v) if v.isdigit() else 0)
    report["seg7"]["vs_list"] = sorted(set(x["vs"] for x in seg), key=lambda v: int(v) if v.isdigit() else 0)
    note("段7 终判: ev%d..ev%d  draws=%d  RT=[%s] DS=%s"
         % (seg7["ev_start"], seg7["ev_end"], seg7["draws"],
            "|".join(seg7["rt_infos"]), seg7["dsv_info"]))
    note("段7 PS 去重 %d 个 / VS 去重 %d 个"
         % (len(report["seg7"]["ps_list"]), len(report["seg7"]["vs_list"])))

    # ---- 4a. 逐 draw 完整状态表 (按 STRIDE) ----
    print("逐 draw 状态表 (%d draws, stride=%d) ..." % (len(seg), STRIDE))
    rows = []
    act_by_ev = {int(a.eventId): a for a in draws}
    for j, r in enumerate(seg):
        if j % STRIDE != 0:
            continue
        try:
            rec = read_pipe(r["ev"])
            act = act_by_ev.get(r["ev"])
            rec["numIndices"] = int(getattr(act, "numIndices", 0) or 0)
            rec["numInstances"] = int(getattr(act, "numInstances", 0) or 0)
            rows.append(rec)
        except Exception as e:
            note_err("row@%d" % r["ev"], e)
    report["seg7_draw_rows"] = rows
    note("逐 draw 行数: %d" % len(rows))

    # ---- 4b. 签名聚类 + 头尾样本 + 候选打分 (天空定位证据) ----
    def rowkey(x):
        return "|".join([
            "ps=" + str(x["ps"]), "vs=" + str(x["vs"]),
            "idx=" + str(x.get("numIndices")), "inst=" + str(x.get("numInstances")),
            "dw=" + str(x.get("depthWrites")), "de=" + str(x.get("depthEnable")),
            "dc=" + str(x.get("depthCompare")),
            "bl=" + str(x.get("blendEnabled")), "wm=" + str(x.get("blendWriteMask")),
            "vp=" + str(x.get("viewport")),
            "srv=" + ",".join("%s:%s" % (s["slot"], s["id"]) for s in x.get("srvPS", []))])

    clusters = {}
    for x in rows:
        clusters.setdefault(rowkey(x), []).append(int(x["eventId"]))
    cl_sorted = sorted(clusters.items(), key=lambda kv: -len(kv[1]))
    report["seg7_clusters"] = [{"count": len(v), "ev_first": v[0], "ev_last": v[-1],
                                "events": v, "key": k} for k, v in cl_sorted]
    note("段7 绑定+状态签名聚类: %d 种" % len(cl_sorted))
    for k, v in cl_sorted[:20]:
        note("  x%-4d ev[%d..%d] %s" % (len(v), v[0], v[-1], k))

    # 深度不写的 draw = 天空最常见签名 (画了颜色但不动深度)
    no_depth_writes = [x for x in rows if x.get("depthWrites") is False]
    depth_off = [x for x in rows if x.get("depthEnable") is False]
    report["seg7_no_depth_writes"] = [{"eventId": x["eventId"], "ps": x["ps"], "vs": x["vs"],
                                      "numIndices": x.get("numIndices"),
                                      "numInstances": x.get("numInstances"),
                                      "viewport": x.get("viewport")} for x in no_depth_writes]
    report["seg7_depth_test_off"] = [{"eventId": x["eventId"], "ps": x["ps"], "vs": x["vs"],
                                      "numIndices": x.get("numIndices")} for x in depth_off]
    note("段7 深度不写(depthWrites=False) draw: %d 个" % len(no_depth_writes))
    note("段7 深度测试关(depthEnable=False) draw: %d 个" % len(depth_off))

    # 无纹理 SRV 的 draw (纯几何/纯 CB 着色 = 天空着色器候选)
    no_tex = [x for x in rows if not x.get("srvPS")]
    report["seg7_no_ps_texture"] = [{"eventId": x["eventId"], "ps": x["ps"], "vs": x["vs"],
                                     "numIndices": x.get("numIndices"),
                                     "depthWrites": x.get("depthWrites")} for x in no_tex]
    note("段7 PS 零纹理 SRV 的 draw: %d 个" % len(no_tex))

    # ---- 4c. 头部 / 尾部 PS 反射 (判是不是天空着色器) ----
    refl_targets = []
    for x in rows[:8] + rows[-4:]:
        if x["eventId"] not in refl_targets:
            refl_targets.append(x["eventId"])
    refl_out = []
    for eid in refl_targets:
        try:
            ctrl.SetFrameEvent(eid, True)
            pipe = ctrl.GetPipelineState()
            for stage, tag in ((rd.ShaderStage.Pixel, "PS"), (rd.ShaderStage.Vertex, "VS")):
                sh = pipe.GetShader(stage)
                r = pipe.GetShaderReflection(stage)
                info = {"eventId": eid, "stage": tag, "shader": clean(sh)}
                if r is None:
                    info["note"] = "无反射"
                else:
                    info["entry"] = str(getattr(r, "entryPoint", ""))
                    ro = []
                    for x in (getattr(r, "readOnlyResources", None) or []):
                        ro.append("%s@%s:%s" % (getattr(x, "name", "?"),
                                                getattr(x, "bindPoint", -1),
                                                str(getattr(x, "type", "?")).split(".")[-1]))
                    info["readOnly"] = ro
                    info["samplers"] = [str(getattr(s, "name", s))
                                        for s in (getattr(r, "samplers", None) or [])]
                    cbs = []
                    for c in (getattr(r, "constantBlocks", None) or []):
                        cbs.append("%s@%s:%dB" % (getattr(c, "name", "?"),
                                                  getattr(c, "bindPoint", -1),
                                                  getattr(c, "byteSize", -1)))
                    info["cbuffers"] = cbs
                refl_out.append(info)
        except Exception as e:
            note_err("refl@%d" % eid, e)
    report["seg7_edge_reflection"] = refl_out

    # ---- 4d. 清事件里指向段7 目标资源的 (段7 起手清 / 段7-end 清) ----
    clears_info = []
    for a in clears:
        eid = int(a.eventId)
        try:
            ctrl.SetFrameEvent(eid, True)
            pipe = ctrl.GetPipelineState()
            clears_info.append({"ev": eid,
                                "rts": [clean(d.resource) for d in pipe.GetOutputTargets()[:4]],
                                "dsv": clean(pipe.GetDepthTarget().resource)
                                if pipe.GetDepthTarget() else "0"})
        except Exception:
            pass
    report["frame_clears_binding"] = clears_info

    # ===== 5. Q2 硬事实一: 461 深度 + 321 颜色, 三时点固定点回读 =====
    # 本版 RenderDoc 的 rd.ResourceId(int) 构造抛 TypeError (docs/analysis/S4-water.md L64
    # 已记为已知限制) ⇒ 必须从管线状态里取**活的** ResourceId 对象, 不能按整数重建。
    depth_rid = None
    rt0_rid = None
    try:
        ctrl.SetFrameEvent(seg7["ev_end"], True)
        _p = ctrl.GetPipelineState()
        _dt = _p.GetDepthTarget()
        if _dt is not None:
            depth_rid = _dt.resource
        _rts = list(_p.GetOutputTargets())
        if _rts:
            rt0_rid = _rts[0].resource
        note("活 ResourceId 取到: dsv=%s rt0=%s" % (clean(depth_rid), clean(rt0_rid)))
    except Exception as e:
        note_err("取活 ResourceId", e)
    # 深度清事件 = **真·清 461 深度** 的那个 Clear。绑定态不可信 (D3D11 清屏走 DSV 句柄、
    # 不经 OM 绑定; S4-water.md L118 已把这条列为方法论), 所以走 RenderDoc 的 usage 表
    # (pass6 clear_targets 同款口径): 该事件 usage 含 461 且 usage 串含 "Clear"。
    # GetUsage(rid) 必须传**活的** ResourceId 对象 (本版 rd.ResourceId(int) 抛 TypeError)。
    depth_clear_ev = None
    clear_hits = []
    if depth_rid is not None:
        try:
            for u in ctrl.GetUsage(depth_rid):
                usg = str(getattr(u, "usage", ""))
                if "Clear" not in usg:
                    continue
                eid = int(getattr(u, "eventId", 0))
                clear_hits.append({"ev": eid, "usage": usg,
                                   "before_seg7": eid < seg7["ev_start"]})
            clear_hits.sort(key=lambda z: z["ev"])
            pre = [z for z in clear_hits if z["before_seg7"]]
            if pre:
                depth_clear_ev = pre[-1]["ev"]
        except Exception as e:
            note_err("GetUsage", e)
    report["depth_clear_events"] = clear_hits
    report["depth_clear_ev"] = depth_clear_ev
    note("真·清 %s 的事件: %s | 取段7 前最后一个 = ev%s"
         % (seg7["dsv"], [z["ev"] for z in clear_hits], depth_clear_ev))

    # 四个时点: 深度清刚过 (清出来的底) / 深度预通道末 (段7 之前 461 已被预通道写满) /
    # 段7 首 draw / 段7 末 draw。预通道末 = 段7 之前最后一个带 461 的 draw 事件。
    prepass_last = None
    for r in sig_rows:
        if r["ev"] < seg7["ev_start"] and r["dsv"] == seg7["dsv"]:
            prepass_last = r["ev"]
    report["prepass_last_draw_before_seg7"] = prepass_last
    read_points = [("depth-clear-after", depth_clear_ev),
                   ("prepass-last-draw", prepass_last),
                   ("seg7-first-draw", seg7["ev_start"]),
                   ("seg7-last-draw", seg7["ev_end"])]
    probe_report = []
    for label, eid in read_points:
        if eid is None:
            continue
        try:
            ctrl.SetFrameEvent(eid, True)
        except Exception as e:
            note_err("SetFrameEvent@%d" % eid, e)
            continue
        entry = {"label": label, "eventId": eid, "depth": {}, "color": {}}
        ddata = dpitch = 0
        dbpp = 0.0
        if depth_rid is not None:
            ddata, dpitch, dbpp = read_pixels(depth_rid, SEG7_W, SEG7_H)
        entry["depth_bytes"] = len(ddata) if ddata else 0
        entry["depth_bpp"] = round(dbpp, 3)
        if ddata:
            for lbl, x, y in ALL_PTS:
                v, how = depth_at(ddata, dpitch, dbpp, x, y)
                entry["depth"][lbl] = {"x": x, "y": y, "v": round(v, 7) if v is not None else None,
                                       "how": how}
        cdata = cpitch = 0
        cbpp = 0.0
        if rt0_rid is not None:
            cdata, cpitch, cbpp = read_pixels(rt0_rid, SEG7_W, SEG7_H)
        entry["color_bytes"] = len(cdata) if cdata else 0
        entry["color_bpp"] = round(cbpp, 3)
        if cdata:
            for lbl, x, y in ALL_PTS:
                entry["color"][lbl] = {"x": x, "y": y, "rgba": color_at(cdata, cpitch, cbpp, x, y)}
        probe_report.append(entry)
        note("回读 %s @ev%d: depth %d B (bpp=%s) | color %d B (bpp=%s)"
             % (label, eid, entry["depth_bytes"], entry["depth_bpp"],
                entry["color_bytes"], entry["color_bpp"]))
    report["probes"] = probe_report

    # ---- 5b. 全屏深度扫描: 找"真·远平面像素集" (段7 末时点) ----
    # 天空在哪不能硬编码 (S1 天空只在左上角一条), 所以直接数: 段7 结束时深度仍 = D24 最大值
    # (0xFFFFFF => 1.0) 的像素 = **段7 全程没有任何几何写过深度**的像素。天空若画在段7 里
    # 且不写深度, 这些像素就是它。附带 RT0 同点颜色 (蓝移 + 亮度高 = 天空的典型 HDR 色)。
    far_stats = {}
    for label, eid in (("prepass-last-draw", prepass_last),
                       ("seg7-first-draw", seg7["ev_start"]),
                       ("seg7-last-draw", seg7["ev_end"])):
        if depth_rid is None:
            break
        try:
            ctrl.SetFrameEvent(eid, True)
            ddata, dpitch, dbpp = read_pixels(depth_rid, SEG7_W, SEG7_H)
        except Exception as e:
            note_err("全屏扫描 %s" % label, e)
            continue
        if not ddata or dbpp != 4.0:
            note_err("全屏扫描 %s" % label, "深度回读不可用 (%s B)" % (len(ddata or b"")))
            continue
        cdata = cpitch = 0
        cbpp = 0.0
        if rt0_rid is not None:
            try:
                cdata, cpitch, cbpp = read_pixels(rt0_rid, SEG7_W, SEG7_H)
            except Exception:
                pass
        n_far = 0
        n_up_far = 0
        samples = []
        rows_far = {}
        far_cols = []
        # 每个 60 行带里**实测**的第一个远平面像素坐标 —— 天空探针点必须落在真远平面上,
        # 不能按比例猜 (S1 的天空只占画面左上角, 按比例猜会全落到屋顶上)。
        band_first = {}
        for y in range(SEG7_H):
            base = y * dpitch
            cnt = 0
            bandkey = (y // 60) * 60
            for x in range(SEG7_W):
                u = struct.unpack_from("<I", ddata, base + x * 4)[0]
                if (u & 0xFFFFFF) >= int(FAR_THRESHOLD * (1 << 24)):
                    cnt += 1
                    if bandkey not in band_first:
                        band_first[bandkey] = (x, y)
                    rgba = color_at(cdata, cpitch, cbpp, x, y) if cdata else None
                    if rgba:
                        far_cols.append(rgba)
                    if len(samples) < 16:
                        samples.append({"x": x, "y": y,
                                        "depth_d24": (u & 0xFFFFFF) / float(1 << 24),
                                        "stencil": (u >> 24) & 0xFF,
                                        "rgba": rgba})
            rows_far[y] = cnt
            n_far += cnt
            if y < SEG7_H // 3:
                n_up_far += cnt
        colstat = None
        if far_cols:
            colstat = {"n": len(far_cols)}
            for ci, cn in enumerate("rgba"):
                vals = sorted(c[ci] for c in far_cols)
                colstat[cn + "_mean"] = round(sum(vals) / len(vals), 5)
                colstat[cn + "_p10"] = vals[len(vals) // 10]
                colstat[cn + "_p50"] = vals[len(vals) // 2]
                colstat[cn + "_p90"] = vals[len(vals) * 9 // 10]
        band = []
        for y0 in range(0, SEG7_H, 60):
            band.append({"y0": y0, "y1": min(y0 + 59, SEG7_H - 1),
                         "far_px": sum(rows_far[y] for y in range(y0, min(y0 + 60, SEG7_H)))})
        far_stats[label] = {
            "eventId": eid,
            "far_total": n_far,
            "far_frac": round(n_far / float(SEG7_W * SEG7_H), 6),
            "far_upper_third": n_up_far,
            "far_upper_third_frac": round(n_up_far / float(SEG7_W * (SEG7_H // 3)), 6),
            "far_by_band": band,
            "far_samples": samples,
            "far_color_stats": colstat,
            "far_first_in_band": {"%d" % k: list(v) for k, v in sorted(band_first.items())},
        }
        note("全屏远平面像素 %s @ev%d: 共 %d (%.4f%%) | 上1/3 %d (%.4f%%)"
             % (label, eid, n_far, 100.0 * n_far / (SEG7_W * SEG7_H),
                n_up_far, 100.0 * n_up_far / float(SEG7_W * (SEG7_H // 3))))
    report["far_plane_scan"] = far_stats

    # ---- 5c. 天空像素"最后被谁写的": 沿段7 追踪远平面像素的 RT0 颜色, 二分找最后写入者 ----
    # 关键: 段7 的 788 个 draw 全部 depthWrites=False + blend 关闭 (本轮实测 700/788 显式
    # False, 其余继承同态) ⇒ **后画覆盖前画**, 远平面像素的最终颜色来自**最后一个**覆盖它的
    # draw。所以判据是"最后一次变色", 不是第一次 —— 第一次覆盖它的是路过的场景几何
    # (实测 ev45370 PS 12140 / 21813 index 的建筑网格只改了 1/4 个探针点, 且写出的颜色是
    # 灰绿 0.46/0.49/0.47 而非天空蓝, 已被证伪)。
    # 末色 = 段7 末 draw 采到的颜色; 若中途某 draw 写下末色, 它就是最后写入者。
    # 二分成立前提: 一旦某 draw 写下末色, 后续 draw 不再改这些像素 (无 blend / 无 alpha 覆盖)。
    # RT0 回读一次 16 MB, 逐 draw 全扫太贵 ⇒ 先按步长 SKYW 粗扫定位, 再二分细定位。
    SKYW = 16
    sky_writer = {}
    if rt0_rid is not None and far_stats.get("seg7-last-draw", {}).get("far_samples"):
        # 探针点取自全屏扫描记录的**实测**远平面像素 (每带第一个), 保证每个探针点
        # 深度确实 = 1.0。far_samples 只有前 16 个 (都在 y=0 一行), 不能用来布点。
        probe_pts = []
        for k in sorted(far_stats["seg7-last-draw"].get("far_first_in_band", {})):
            v = far_stats["seg7-last-draw"]["far_first_in_band"][k]
            if all(abs(v[0] - p[0]) >= 32 or abs(v[1] - p[1]) >= 24 for p in probe_pts):
                probe_pts.append((int(v[0]), int(v[1])))
        report["sky_probe_points"] = [{"x": a, "y": b} for a, b in probe_pts]
        # 段7 起点的 RT0 颜色 = 清屏底色; 段7 末 draw 的颜色 = 末色 (判据基准)
        def snapshot(ev):
            try:
                ctrl.SetFrameEvent(ev, True)
                d2, p2, b2 = read_pixels(rt0_rid, SEG7_W, SEG7_H)
                return [color_at(d2, p2, b2, a, b) if d2 else None for (a, b) in probe_pts]
            except Exception:
                return [None] * len(probe_pts)

        base_cols = snapshot(seg7["ev_start"])
        final_cols = snapshot(seg7["ev_end"])
        note("追踪 %d 个远平面像素: %s" % (len(probe_pts), probe_pts))
        note("段7 起点底色: %s" % (base_cols,))
        note("段7 末 末色: %s" % (final_cols,))

        def nmatch(cols, target):
            """多少个探针点已等于 target 的颜色。"""
            k = 0
            for c, t0 in zip(cols, target):
                if c is None or t0 is None:
                    continue
                if all(abs((c[i] if c[i] is not None else 0) - (t0[i] or 0)) <= 0.002
                       for i in range(4)):
                    k += 1
            return k

        seg_evs = [r["ev"] for r in seg]
        # 粗扫: 找"匹配数达到末色"的最早 draw (首个把全部探针点写成末色的 draw = 末次写入者)
        trace = []
        first_full = None
        for j in range(0, len(seg_evs), SKYW):
            c = snapshot(seg_evs[j])
            m = nmatch(c, final_cols)
            trace.append({"idx": j, "ev": seg_evs[j], "colors": c, "match_final": m})
            if m == len(probe_pts):
                first_full = j
                break
        if first_full is None:
            # 粗扫没全中: 说明不是"某个 draw 一次性写全", 退化为"最后一个改过任一探针点的 draw"
            for j in range(0, len(seg_evs), SKYW):
                c = snapshot(seg_evs[j])
                m = sum(1 for a, b in zip(c, base_cols)
                        if a is not None and b is not None and
                        any(abs((a[i] or 0) - (b[i] or 0)) > 0.002 for i in range(4)))
                trace[-1] = trace[-1] if trace and trace[-1]["idx"] == j else trace[-1]
                if m > 0:
                    first_full = j
                    break
        if first_full is not None:
            for j in range(max(0, first_full - SKYW), min(len(seg_evs), first_full + 1)):
                c = snapshot(seg_evs[j])
                m = nmatch(c, final_cols)
                m2 = sum(1 for a, b in zip(c, base_cols)
                         if a is not None and b is not None and
                         any(abs((a[i] or 0) - (b[i] or 0)) > 0.002 for i in range(4)))
                trace.append({"idx": j, "ev": seg_evs[j], "colors": c,
                              "match_final": m, "changed_vs_base": m2})
        trace.sort(key=lambda z: z["idx"])
        # 去重 (粗扫与细扫可能重复同一 idx)
        ded = []
        for t in trace:
            if not ded or ded[-1]["idx"] != t["idx"]:
                ded.append(t)
        sky_writer = {"points": [{"x": a, "y": b} for a, b in probe_pts],
                      "base_colors_at_seg7_start": base_cols,
                      "final_colors_at_seg7_end": final_cols,
                      "coarse_stride": SKYW, "trace": ded}
        hit_idx = None
        for t in ded:
            if t.get("match_final") == len(probe_pts):
                hit_idx = t["idx"]
                break
        if hit_idx is None:
            for t in ded:
                if (t.get("changed_vs_base") or 0) > 0:
                    hit_idx = t["idx"]
                    break
        sky_writer["last_writer_idx"] = hit_idx
        if hit_idx is not None:
            hit_ev = seg_evs[hit_idx]
            sky_writer["last_writer_ev"] = hit_ev
            sky_writer["match_final_at_hit"] = next(
                (t.get("match_final") for t in ded if t["idx"] == hit_idx), None)
            row = next((x for x in rows if x["eventId"] == hit_ev), None)
            if row:
                sky_writer["writer_draw"] = {
                    "eventId": row["eventId"], "ps": row["ps"], "vs": row["vs"],
                    "numIndices": row.get("numIndices"), "numInstances": row.get("numInstances"),
                    "viewport": row.get("viewport"), "depthEnable": row.get("depthEnable"),
                    "depthWrites": row.get("depthWrites"),
                    "blendEnabled": row.get("blendEnabled"),
                    "srvPS": row.get("srvPS"),
                }
            note("★ 远平面像素末色由段7 第 %d 个 draw = ev%d 写入 (命中末色 %s/%d) "
                 "(PS=%s VS=%s idx=%s inst=%s dw=%s)"
                 % (hit_idx, hit_ev, sky_writer["match_final_at_hit"], len(probe_pts),
                    sky_writer.get("writer_draw", {}).get("ps", "?"),
                    sky_writer.get("writer_draw", {}).get("vs", "?"),
                    sky_writer.get("writer_draw", {}).get("numIndices", "?"),
                    sky_writer.get("writer_draw", {}).get("numInstances", "?"),
                    sky_writer.get("writer_draw", {}).get("depthWrites", "?")))
        else:
            note("远平面像素在段7 全程颜色未变 (仍是清屏底色) ⇒ 段7 内无 draw 画它")
        report["sky_pixel_writer"] = sky_writer

        # ---- 5d. 写入者的 PS 反射 + 该几何是否也出现在深度预通道 ----
        if sky_writer.get("last_writer_ev") is not None:
            wev = sky_writer["last_writer_ev"]
            try:
                ctrl.SetFrameEvent(wev, True)
                wp = ctrl.GetPipelineState()
                wrow = {"eventId": wev}
                for stage, tag in ((rd.ShaderStage.Pixel, "PS"), (rd.ShaderStage.Vertex, "VS")):
                    sh = wp.GetShader(stage)
                    rf = wp.GetShaderReflection(stage)
                    d = {"shader": clean(sh)}
                    if rf is not None:
                        d["entry"] = str(getattr(rf, "entryPoint", ""))
                        d["readOnly"] = ["%s@%s:%s" % (getattr(x, "name", "?"),
                                                        getattr(x, "bindPoint", -1),
                                                        str(getattr(x, "type", "?")).split(".")[-1])
                                        for x in (getattr(rf, "readOnlyResources", None) or [])]
                        d["samplers"] = [str(getattr(x, "name", x))
                                         for x in (getattr(rf, "samplers", None) or [])]
                        d["cbuffers"] = ["%s@%s:%dB" % (getattr(x, "name", "?"),
                                                        getattr(x, "bindPoint", -1),
                                                        getattr(x, "byteSize", -1))
                                         for x in (getattr(rf, "constantBlocks", None) or [])]
                    wrow[tag] = d
                wrow["bound_srvPS"] = [{"slot": x.get("slot"), "id": x.get("id"),
                                        "info": x.get("info")}
                                       for x in (sky_writer.get("writer_draw") or {}).get("srvPS", [])]
                report["sky_writer_reflection"] = wrow
                note("写入者反射: PS ro=%s cb=%s | VS ro=%s"
                     % (wrow.get("PS", {}).get("readOnly"), wrow.get("PS", {}).get("cbuffers"),
                        wrow.get("VS", {}).get("readOnly")))
            except Exception as e:
                note_err("sky writer refl", e)

    # ---- 5e. 段7 首 draw 的 VS 是否在深度预通道 (段5) 里也出现 ----
    # 天空在 z-prepass 里通常也要走一遍 (同几何同 VS)。若段7 首 draw 的 VS 在预通道里
    # 找不到, 说明那几笔不是天空 (或者是 z-prepass 豁免对象, 如 UI/粒子)。
    try:
        head_vs = sorted(set(x["vs"] for x in rows[:8]))
        prepass_vs = set(r["vs"] for r in sig_rows
                         if prepass_last and r["ev"] <= prepass_last and r["dsv"] == seg7["dsv"])
        report["seg7_head_vs_in_prepass"] = {v: (v in prepass_vs) for v in head_vs}
        note("段7 前 8 draw 的 VS=%s 在深度预通道中出现: %s"
             % (head_vs, {v: (v in prepass_vs) for v in head_vs}))
        writer_vs = (sky_writer.get("writer_draw") or {}).get("vs")
        if writer_vs:
            report["sky_writer_vs_in_prepass"] = writer_vs in prepass_vs
            note("写入者 VS=%s 在深度预通道中出现: %s" % (writer_vs, writer_vs in prepass_vs))
    except Exception as e:
        note_err("prepass vs 对照", e)

    # ===== 6. Q3 硬事实二: 段7 前后绑点差分 (运行时可见字段) =====
    try:
        before = [r for r in sig_rows if r["ev"] < seg7["ev_start"]]
        after = [r for r in sig_rows if r["ev"] > seg7["ev_end"]]
        report["seg7_boundary"] = {
            "before_last": before[-1] if before else None,
            "after_first": after[0] if after else None,
            "seg7_signature_used": seg7["rt_infos"] + [seg7["dsv_info"]],
        }
        for tag, r in (("段7 前最后一个 draw", before[-1] if before else None),
                       ("段7 后第一个 draw", after[0] if after else None)):
            if r:
                note("%s: ev%d RT=[%s|%s|%s] DS=%s ps=%s"
                     % (tag, r["ev"], r["rt0f"], r["rt1f"], r["rt2f"], r["dsvf"], r["ps"]))
    except Exception as e:
        note_err("边界差分", e)

# ===== verdict: 两个硬事实的机器判定 (人读结论见 docs/analysis/Sky-recon.md) =====
try:
    if "seg7" in report:
        fps = report.get("far_plane_scan") or {}
        a = fps.get("prepass-last-draw")
        b = fps.get("seg7-last-draw")
        sw = report.get("sky_pixel_writer") or {}
        v = {"seg7_located": True}
        if a and b:
            v["depth_frozen_during_seg7"] = (a["far_total"] == b["far_total"])
            v["far_px"] = b["far_total"]
            v["far_all_in_upper_band"] = (b["far_upper_third"] == b["far_total"])
            cs = b.get("far_color_stats") or {}
            v["far_color_b_gt_g_gt_r"] = bool(cs and (cs.get("b_p50", 0) > cs.get("g_p50", 0) >
                                                     cs.get("r_p50", 0)))
        v["sky_writer_found"] = sw.get("last_writer_ev") is not None
        if sw.get("writer_draw"):
            v["sky_draw"] = sw["writer_draw"]
        v["seg7_all_depth_writes_off"] = all(
            x.get("depthWrites") is False for x in (report.get("seg7_draw_rows") or []))
        rws = report.get("seg7_draw_rows") or []
        report["seg7_state_hist"] = {
            "total": len(rws),
            "depthWrites_false": sum(1 for x in rws if x.get("depthWrites") is False),
            "depthWrites_true": sum(1 for x in rws if x.get("depthWrites") is True),
            "depthWrites_unknown": sum(1 for x in rws if x.get("depthWrites") is None),
            "blendEnabled_true": sum(1 for x in rws if x.get("blendEnabled") is True),
            "blendEnabled_false": sum(1 for x in rws if x.get("blendEnabled") is False),
            "distinct_binding_state_signatures": len(report.get("seg7_clusters") or []),
            "ps_zero_texture_draws": len(report.get("seg7_no_ps_texture") or []),
        }
        report["verdict"] = v
except Exception as e:
    note_err("verdict", e)

report["elapsed_sec"] = round(time.time() - t0, 1)

# ===== 7. 落盘 =====
try:
    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1, default=str)
    print("写出:", OUT_JSON)
except Exception as e:
    note_err("写 JSON", e)
try:
    with open(OUT_TXT, "w", encoding="utf-8", newline="\n") as f:
        f.write("# pass8-sky scene=%s stride=%d elapsed=%s errors=%d\n"
                % (_SCENE, STRIDE, report["elapsed_sec"], len(report["errors"])))
        for e in report["errors"]:
            f.write("# ERROR: %s\n" % e)
        f.write("## seg7\n")
        if "seg7" in report:
            f.write("ev %d..%d draws=%d RT=[%s] DS=%s\n"
                    % (report["seg7"]["ev_start"], report["seg7"]["ev_end"],
                       report["seg7"]["draws"], "|".join(report["seg7"]["rt_infos"]),
                       report["seg7"]["dsv_info"]))
            f.write("PS(%d): %s\n" % (len(report["seg7"]["ps_list"]),
                                      ",".join(report["seg7"]["ps_list"])))
        f.write("\n## 段7 逐 draw (eventId ps vs idx inst depthE depthW depthC blendE wm vp srv)\n")
        for x in report.get("seg7_draw_rows", []):
            f.write("ev%-7d ps=%-7s vs=%-7s idx=%-6s inst=%-3s de=%-5s dw=%-5s dc=%-9s "
                    "bl=%-5s wm=%-4s vp=%-18s srv=%s\n"
                    % (x["eventId"], x["ps"], x["vs"], x.get("numIndices"),
                       x.get("numInstances"), x.get("depthEnable"), x.get("depthWrites"),
                       x.get("depthCompare"), x.get("blendEnabled"), x.get("blendWriteMask"),
                       x.get("viewport"),
                       ",".join("%s:%s" % (s["slot"], s["id"]) for s in x.get("srvPS", []))))
        f.write("\n## 固定点回读 (461 深度 D24 / RT0 颜色 RGBA16F)\n")
        for p in report.get("probes", []):
            f.write("== %s @ev%s ==\n" % (p["label"], p["eventId"]))
            for lbl, _x, _y in ALL_PTS:
                d = p["depth"].get(lbl, {})
                c = p["color"].get(lbl, {})
                f.write("   %-24s (%4d,%4d) depth=%-12s (%s)  rgba=%s\n"
                        % (lbl, _x, _y, d.get("v"), d.get("how"), c.get("rgba")))
        f.write("\n## 全屏远平面 (D24 >= %s) 像素统计\n" % FAR_THRESHOLD)
        for k, fs_ in (report.get("far_plane_scan") or {}).items():
            f.write("== %s @ev%s ==  far_total=%d (%.4f%%)  上1/3=%d  色统计=%s\n"
                    % (k, fs_["eventId"], fs_["far_total"], 100.0 * fs_["far_frac"],
                       fs_["far_upper_third"], fs_.get("far_color_stats")))
            f.write("   按 60 行带: %s\n"
                    % " ".join("%d:%d" % (b["y0"], b["far_px"]) for b in fs_["far_by_band"]))
        sw = report.get("sky_pixel_writer") or {}
        if sw:
            f.write("\n## 天空像素写入者追踪 (段7 内, 判据=末次写入者)\n")
            f.write("探针点: %s\n" % sw.get("points"))
            f.write("段7 起点底色: %s\n" % sw.get("base_colors_at_seg7_start"))
            f.write("段7 末 末色  : %s\n" % sw.get("final_colors_at_seg7_end"))
            f.write("末次写入: idx=%s ev=%s 命中末色=%s/%d\n"
                    % (sw.get("last_writer_idx"), sw.get("last_writer_ev"),
                       sw.get("match_final_at_hit"), len(sw.get("points") or [])))
            f.write("写入 draw: %s\n" % json.dumps(sw.get("writer_draw"), ensure_ascii=False))
            f.write("-- trace (只打变化前后的窗口) --\n")
            tr = sw.get("trace") or []
            prev = None
            for t in tr:
                if prev is not None and t["idx"] - prev > 1:
                    f.write("   ... (idx %d..%d 略)\n" % (prev + 1, t["idx"] - 1))
                f.write("   idx=%-4d ev=%-7d matchFinal=%s colors=%s\n"
                        % (t["idx"], t["ev"], t.get("match_final"), t["colors"]))
                prev = t["idx"]
        f.write("\n## 段7 边界\n")
        f.write("%s\n" % json.dumps(report.get("seg7_boundary"), ensure_ascii=False))
    print("写出:", OUT_TXT)
except Exception as e:
    note_err("写 TXT", e)

print("RESULT: %s" % ("SUCCESS" if not report["errors"] else "FAILED"))
print("完成 errors=%d elapsed=%.1fs" % (len(report["errors"]), report["elapsed_sec"]))
if os.environ.get("RDC_HEADLESS"):
    sys.exit(0 if not report["errors"] else 1)