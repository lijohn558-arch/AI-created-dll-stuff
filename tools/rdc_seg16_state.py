# rdc_seg16_state.py -- D-2/D-3 取数: 段16 逐 Draw 的 blend / depth-stencil 状态 + 覆盖线索
#
# 要回答两问 (docs/05 D3b 准入前置, 见 docs/05 D3b 行):
#   D-2 段16 的 blend state —— 16 个 draw 是 GPU blend 分层合成, 还是各自画不同像素?
#   D-3 段16 对 DS=461 是否有实质深度写入 (depth test / depth write mask), 被段17/18 消费?
# 附带逐 draw 的 numIndices / numInstances / viewport ——
#   numIndices==3 (全屏三角形) + viewport 1920x1080 ⇒ 覆盖重叠, 必须靠 blend 分层;
#   数千 index 的真几何 ⇒ 各画各的像素, blend 可以是关的。
#
# Env: RDC_SCENE 默认 S4; RDC_LO / RDC_HI 默认 39270/39502 (段16 区间);
#      RDC_EXTRA 默认 "39215,39530,39585" (段15/17/18 对照)
# Output: docs/analysis/<prefix>-seg-state.txt
import os
import sys
import time

import renderdoc as rd

_SCENES = {"S1": "S1-city-day", "S2": "S2-interior", "S3": "S3-forest-godrays",
           "S4": "S4-water", "S5": "S5-night-combat"}
_SCENE = os.environ.get("RDC_SCENE", "").strip() or "S4"
_FULL = _SCENES.get(_SCENE, _SCENE)
_PREFIX = _FULL.split("-")[0]
RDC = r"C:\Users\joker\skyrim-vulkan\captures\%s.rdc" % _FULL
OUT_TXT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-seg-state.txt" % _PREFIX

LO = int(os.environ.get("RDC_LO", "39270"))
HI = int(os.environ.get("RDC_HI", "39502"))
try:
    EXTRA = [int(x) for x in os.environ.get("RDC_EXTRA", "39215,39530,39585").split(",") if x.strip()]
except Exception:
    EXTRA = [39215, 39530, 39585]

LINES = []
ERRS = []


def note(s):
    LINES.append(s)
    print(s)


def finish(code):
    try:
        with open(OUT_TXT, "w", encoding="utf-8") as f:
            f.write("# seg16-state scene=%s range=%d..%d extra=%s\n"
                    % (_SCENE, LO, HI, ",".join(str(x) for x in EXTRA)))
            for e in ERRS:
                f.write("# ERROR: %s\n" % e)
            for ln in LINES:
                f.write(ln + "\n")
    except Exception as e:
        print("[error] write: %s" % e)
    print("RESULT: %s" % ("SUCCESS" if code == 0 else "FAILED"))
    if os.environ.get("RDC_HEADLESS"):
        sys.exit(code)


def enum_int(name):
    try:
        return int(getattr(rd.ActionFlags, name))
    except Exception:
        return None


def fmt(x, depth=0, seen=None):
    """任意结构 → 单行文本。
    SWIG 结构体 (ColorBlend/DepthTestState/StencilFace/RasterState) 的 str() 只给指针地址
    ⇒ 必须逐属性取出真值, 否则汇总去重会把每个地址当成不同取值。"""
    if seen is None:
        seen = set()
    if depth > 6:
        return "..."
    if isinstance(x, (bool, int, float, str)):
        s = repr(x)
        return s.replace("ResourceId::", "")
    if isinstance(x, dict):
        return "{" + ", ".join("%s=%s" % (k, fmt(v, depth + 1, seen)) for k, v in x.items()) + "}"
    if isinstance(x, (list, tuple)):
        return "[" + ", ".join(fmt(v, depth + 1, seen) for v in x) + "]"
    if id(x) in seen:
        return "<cycle>"
    seen.add(id(x))
    try:
        items = []
        for name in dir(x):
            if name.startswith("_"):
                continue
            try:
                v = getattr(x, name)
            except Exception:
                continue
            if callable(v):
                continue
            items.append("%s=%s" % (name, fmt(v, depth + 1, seen)))
        if items:
            return "{" + ", ".join(items) + "}"
    except Exception:
        pass
    s = str(x).replace("ResourceId::", "")
    if "<Swig" in s or "0x0000" in s:
        return "<obj>"
    if len(s) > 300:
        s = s[:300] + "..."
    return s


def read_color_blends(pipe):
    """GetColorBlends() 返回 SWIG ColorBlend 数组 —— 泛型 dir() 扫不出字段,
    必须逐下标逐字段取 (enable/srcBlend/destBlend/blendOp/...), 否则只能拿到指针地址。"""
    cbs = pipe.GetColorBlends()
    seq = None
    try:
        seq = list(cbs)
    except Exception:
        seq = None
    if seq is None:
        seq = []
        for i in range(8):
            try:
                seq.append(cbs[i])
            except Exception:
                break
    rows = []
    _dump = [True]
    for i, cb in enumerate(seq[:8]):
        if _dump[0]:
            _dump[0] = False
            try:
                note("-- API 自省 colorBlend --")
                note("   type=%s" % type(cb))
                note("   dir=" + ", ".join(d for d in dir(cb) if not d.startswith("__")))
            except Exception:
                pass
        row = {}
        for f in ("enabled", "colorBlend", "alphaBlend", "logicOperationEnabled",
                  "logicOperation", "writeMask"):
            try:
                v = getattr(cb, f)
            except Exception:
                v = "?"
            if isinstance(v, (bool, int, float, str)):
                row[f] = v
            else:
                row[f] = fmt(v)  # 嵌套 BlendStruct → 逐字段 (src/dest/operation)
        rows.append(row)
    return rows


def get_first(obj, names):
    """按顺序试若干方法名, 返回第一个能调用成功的"""
    for n in names:
        fn = getattr(obj, n, None)
        if fn is None:
            continue
        try:
            return fn()
        except Exception:
            continue
    return None


t0 = time.time()
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
        ERRS.append("OpenFile failed: %r" % (st,))
        finish(2)
    res = capf.OpenCapture(rd.ReplayOptions(), None)
    ctrl = res[1] if (isinstance(res, tuple) and len(res) == 2) else res
    if ctrl is None:
        ERRS.append("no controller")
        finish(3)

    # --- 动作树 → flat draw 列表 ---
    flat = []

    def walk(actions):
        for a in actions:
            flat.append(a)
            try:
                if a.children:
                    walk(a.children)
            except Exception:
                pass

    walk(ctrl.GetRootActions())
    dc_flag = enum_int("Drawcall")
    draws = [a for a in flat if dc_flag and (int(getattr(a, "flags", 0)) & dc_flag)]
    in_range = [a for a in draws if LO <= int(a.eventId) <= HI]
    extra = [a for a in draws if int(a.eventId) in EXTRA]
    note("== %s (%s) ==" % (_SCENE, os.path.basename(RDC)))
    note("draws total=%d | in range %d..%d = %d | extra=%d"
         % (len(draws), LO, HI, len(in_range), len(extra)))
    if not in_range:
        ERRS.append("range %d..%d 内没有 draw" % (LO, HI))

    targets = in_range + extra
    blend_seen = {}
    depth_seen = {}
    topo_seen = {}
    _dumped = [False]
    _blend_err = [False]
    _depth_err = [False]

    for idx, a in enumerate(targets):
        eid = int(a.eventId)
        tag = "ev%d" % eid
        if eid in EXTRA:
            tag += "(对照)"
        try:
            ctrl.SetFrameEvent(eid, False)
            pipe = ctrl.GetPipelineState()
            if not _dumped[0]:
                _dumped[0] = True
                note("-- API 自省 pipe --")
                note("   " + ", ".join(m for m in dir(pipe)
                                       if not m.startswith("_") and
                                       any(k in m.lower() for k in
                                           ("blend", "depth", "stencil", "state", "topo", "scissor",
                                            "raster", "shader", "viewport"))))
                note("-- API 自省 action --")
                note("   " + ", ".join(m for m in dir(a)
                                       if not m.startswith("_") and
                                       any(k in m.lower() for k in
                                           ("topo", "ind", "inst", "off", "draw", "event"))))
            ps = "?"
            try:
                ps = str(pipe.GetShader(rd.ShaderStage.Pixel)).replace("ResourceId::", "")
            except Exception:
                pass
            vs = "?"
            try:
                vs = str(pipe.GetShader(rd.ShaderStage.Vertex)).replace("ResourceId::", "")
            except Exception:
                pass
            vp = "?"
            try:
                v = pipe.GetViewport(0)
                vp = "none" if v is None else ("%gx%g" % (v.width, v.height)
                                               if getattr(v, "enabled", True) else "disabled")
            except Exception:
                pass
            rts = []
            try:
                rts = [str(d.resource).replace("ResourceId::", "") for d in pipe.GetOutputTargets()]
            except Exception:
                pass
            ds = "?"
            try:
                d = pipe.GetDepthTarget()
                ds = "None" if d is None else str(d.resource).replace("ResourceId::", "")
            except Exception:
                pass
            ni = getattr(a, "numIndices", None)
            ninst = getattr(a, "numInstances", None)
            topo = getattr(a, "topology", None)

            blend = None
            depth = None
            try:
                blend = {"colorBlends": read_color_blends(pipe),
                         "independent": pipe.IsIndependentBlendingEnabled(),
                         "factor": fmt(pipe.GetBlendFactor())}
            except Exception:
                blend = None
            try:
                st = {"depthTest": pipe.GetDepthTestState()}
                try:
                    st["stencilTest"] = pipe.IsStencilTestEnabled()
                    if st["stencilTest"]:
                        st["stencilFaces"] = pipe.GetStencilFaces()
                except Exception:
                    pass
                depth = st
            except Exception:
                depth = None
            try:
                topo = pipe.GetPrimitiveTopology()
            except Exception:
                topo = getattr(a, "topology", None)
            try:
                sc = pipe.GetScissor(0)
                scissor = "none" if sc is None else ("%d,%d %dx%d" % (sc.x, sc.y, sc.width, sc.height))
            except Exception:
                scissor = "?"
            try:
                rs = pipe.GetRasterState()
            except Exception:
                rs = None
            if blend is None:
                if not _blend_err[0]:
                    _blend_err[0] = True
                    ERRS.append("GetColorBlends 失败@%d (仅记首次)" % eid)
            if depth is None:
                if not _depth_err[0]:
                    _depth_err[0] = True
                    ERRS.append("GetDepthTestState 失败@%d (仅记首次)" % eid)

            bk = fmt(blend)
            dk = fmt(depth)
            tk = fmt(topo)
            blend_seen[bk] = blend_seen.get(bk, 0) + 1
            depth_seen[dk] = depth_seen.get(dk, 0) + 1
            topo_seen[tk] = topo_seen.get(tk, 0) + 1

            note("%-14s #%02d ps=%-7s vs=%-7s idx=%s inst=%s topo=%s vp=%s RT=[%s] DS=%s"
                 % (tag, idx, ps, vs, ni, ninst, tk, vp, ",".join(rts), ds))
            note("               scissor=%s raster=%s" % (scissor, fmt(rs)))
            note("               blend = %s" % bk)
            note("               depth = %s" % dk)
        except Exception as e:
            ERRS.append("%s: %s: %s" % (tag, type(e).__name__, e))

    note("-- 汇总 --")
    note("blend 不同取值 %d 种:" % len(blend_seen))
    for k, v in blend_seen.items():
        note("   x%-3d %s" % (v, k))
    note("depth 不同取值 %d 种:" % len(depth_seen))
    for k, v in depth_seen.items():
        note("   x%-3d %s" % (v, k))
    note("topology 不同取值 %d 种:" % len(topo_seen))
    for k, v in topo_seen.items():
        note("   x%-3d %s" % (v, k))
except SystemExit:
    raise
except Exception as e:
    ERRS.append("main: %s: %s" % (type(e).__name__, e))

note("elapsed %.1fs errors=%d" % (round(time.time() - t0, 1), len(ERRS)))
finish(1 if ERRS else 0)
