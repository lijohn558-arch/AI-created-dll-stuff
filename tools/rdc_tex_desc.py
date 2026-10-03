# rdc_tex_desc.py -- dump TextureDescription full fields for given ids / filter (v0.11 depth pairing)
#
# Env: RDC_SCENE, RDC_IDS (comma resource ids, e.g. "544,552"; empty = auto:
#      512x512 textures or anything with D24/RGBA16F in format name)
# Output: docs/analysis/<prefix>-tex-desc.txt
#
# 用途: v0.11 探针升质修复 -- 探针配对 depth (552) 的 type/arraySize/format 实测,
#       决定 main.cpp probeDepthDesc 过滤器字段; cube 544 同表对照.
# API 口径: ctrl.GetTextures() -> TextureDescription 列表 (同 cubefaces candidates);
#   format.name property/method 兜底 = pass4 fmt_name 口径; type = TextureType 枚举.
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
OUT_TXT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-tex-desc.txt" % _PREFIX

try:
    WANTED = set(x.strip() for x in os.environ.get("RDC_IDS", "").split(",") if x.strip())
except Exception:
    WANTED = set()

LINES = []
ERRS = []


def note(s):
    LINES.append(s)
    print(s)


def finish(code):
    try:
        with open(OUT_TXT, "w", encoding="utf-8") as f:
            f.write("# tex-desc scene=%s ids=%s\n" % (_SCENE, sorted(WANTED) or "auto"))
            for e in ERRS:
                f.write("# ERROR: %s\n" % e)
            for ln in LINES:
                f.write(ln + "\n")
    except Exception as e:
        print("[error] write: %s" % e)
    print("RESULT: %s" % ("SUCCESS" if code == 0 else "FAILED"))
    if os.environ.get("RDC_HEADLESS"):
        sys.exit(code)


def fmt_name(f):
    if f is None:
        return None
    for attr in ("name", "Name"):
        v = getattr(f, attr, None)
        if callable(v):
            v = v()
        if v:
            return str(v)
    return str(f)


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
        finish(1)
        raise SystemExit(1)
    res = capf.OpenCapture(rd.ReplayOptions(), None)
    ctrl = res[1] if (isinstance(res, tuple) and len(res) == 2) else res
    if ctrl is None:
        ERRS.append("no controller")
        finish(1)
        raise SystemExit(1)
    note("textures:")
    n_hit = 0
    for t in ctrl.GetTextures():
        rid = str(t.resourceId)
        dig = rid.split("::")[-1]
        fn = fmt_name(getattr(t, "format", None)) or "?"
        w, h = int(t.width), int(t.height)
        if WANTED:
            if dig not in WANTED:
                continue
        else:
            if not (w == 512 and h == 512 or "D24" in fn or "RGBA16F" in fn.upper()
                    or "R16G16B16A16" in fn.upper()):
                continue
        n_hit += 1
        ty = getattr(t, "type", "?")
        extra = []
        for attr in ("depth", "arraySize", "mips", "sampleCount", "byteSize", "customName"):
            v = getattr(t, attr, None)
            if callable(v):
                try:
                    v = v()
                except Exception:
                    v = "?"
            extra.append("%s=%s" % (attr, v))
        note("  %-22s %dx%d fmt=%s type=%s %s" % (rid, w, h, fn, ty, " ".join(extra)))
        # 全属性一览 (首6个命中): 发现 TextureDescription 上还有哪些可读字段
        if n_hit <= 6:
            pub = []
            for a in sorted(dir(t)):
                if a.startswith("_") or a in ("resourceId", "width", "height", "format", "type",
                                              "depth", "arraySize", "mips", "sampleCount",
                                              "byteSize", "customName"):
                    continue
                v = getattr(t, a, None)
                if callable(v):
                    continue
                pub.append("%s=%s" % (a, v))
            if pub:
                note("      extra-attrs: %s" % " ".join(pub))
    note("hit=%d elapsed=%.1fs" % (n_hit, time.time() - t0))
    try:
        ctrl.Shutdown()
    except Exception:
        pass
    try:
        capf.Close()
    except Exception:
        pass
    finish(0)
except SystemExit:
    raise
except Exception as e:
    ERRS.append("fatal: %s: %s" % (type(e).__name__, e))
    finish(1)
