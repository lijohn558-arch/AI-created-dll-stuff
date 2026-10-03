# rdc_state_probe.py -- minimal pipe-state check at given events (seek=True vs False)
#
# Env: RDC_SCENE, RDC_EVENTS (comma eids, default "118,200,440,900")
# Output: docs/analysis/<prefix>-state-probe.txt
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
OUT_TXT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-state-probe.txt" % _PREFIX

try:
    EVENTS = [int(x) for x in os.environ.get("RDC_EVENTS", "118,200,440,900").split(",") if x.strip()]
except Exception:
    EVENTS = [118, 200, 440, 900]

LINES = []
ERRS = []


def note(s):
    LINES.append(s)
    print(s)


def finish(code):
    try:
        with open(OUT_TXT, "w", encoding="utf-8") as f:
            f.write("# state-probe scene=%s events=%s\n" % (_SCENE, EVENTS))
            for e in ERRS:
                f.write("# ERROR: %s\n" % e)
            for ln in LINES:
                f.write(ln + "\n")
    except Exception as e:
        print("[error] write: %s" % e)
    print("RESULT: %s" % ("SUCCESS" if code == 0 else "FAILED"))
    if os.environ.get("RDC_HEADLESS"):
        sys.exit(code)


def desc_targets(ctrl, tag):
    try:
        ps = ctrl.GetPipelineState()
        outs = []
        for t in list(ps.GetOutputTargets()):
            try:
                outs.append("res=%s slice=%s" % (str(getattr(t, "resource", "?")),
                                                 str(getattr(t, "firstSlice", "?"))))
            except Exception as e:
                outs.append("err:%s" % type(e).__name__)
        d = None
        try:
            dt = ps.GetDepthTarget()
            d = "None" if dt is None else "res=%s" % str(getattr(dt, "resource", "?"))
        except Exception as e:
            d = "err:%s" % type(e).__name__
        vp = "?"
        try:
            v = ps.GetViewport(0)
            vp = "none" if v is None else "%gx%g" % (v.width, v.height)
        except Exception:
            pass
        note("%-28s RT=[%s] DS=%s vp=%s" % (tag, ", ".join(outs), d, vp))
    except Exception as e:
        ERRS.append("desc_targets %s: %s: %s" % (tag, type(e).__name__, e))


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

    note("== %s (%s) ==" % (_SCENE, os.path.basename(RDC)))
    for eid in EVENTS:
        try:
            ctrl.SetFrameEvent(eid, True)
            desc_targets(ctrl, "ev%d seek=True " % eid)
            ctrl.SetFrameEvent(eid, False)
            desc_targets(ctrl, "ev%d seek=False" % eid)
        except Exception as e:
            ERRS.append("SetFrameEvent(%d): %s: %s" % (eid, type(e).__name__, e))
except SystemExit:
    raise
except Exception as e:
    ERRS.append("main: %s: %s" % (type(e).__name__, e))

note("elapsed %.1fs errors=%d" % (round(time.time() - t0, 1), len(ERRS)))
finish(1 if ERRS else 0)
