# rdc_api_scan.py -- scan serialised API calls (structured data) by event range
#
# Headless usage (via rdc_run.ps1, env inherited):
#   $env:RDC_RANGE='0-1300'
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_api_scan.py -Scene S4d
# Env:
#   RDC_SCENE  scene id (same convention as other passes)
#   RDC_RANGE  event range "lo-hi" (default 0-999999 = whole frame)
#   RDC_FILTER comma-separated name substrings to keep (default OM/view/clear/copy set)
#
# Output: docs/analysis/<prefix>-api-scan.txt   lines: ev<id> <call> k=v ...
# Purpose: ground truth for *state-setting* calls (OMSetRenderTargets etc.) which are
#          NOT actions -- invisible to pass5/pass6 binding & segment extraction.
# Method:  APIEvent.chunkIndex (per action tree) -> SDFile.chunks[ci] (serialised call).
#          SDFile via CaptureFile.GetStructuredData() (docs: capture_access).
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
OUT_TXT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-api-scan.txt" % _PREFIX

_rng = os.environ.get("RDC_RANGE", "").strip()
try:
    RLO, RHI = (int(x) for x in _rng.split("-")) if _rng else (0, 999999)
except Exception:
    RLO, RHI = 0, 999999

_FILTER = [s.strip() for s in os.environ.get(
    "RDC_FILTER",
    "OMSetRenderTargets,CreateRenderTargetView,CreateDepthStencilView,CreateShaderResourceView,"
    "CreateTexture2D,ClearState,ClearRenderTargetView,ClearDepthStencilView,"
    "CopySubresourceRegion,UpdateSubresource,RSSetViewports").split(",") if s.strip()]

report = {"rdc": RDC, "scene": _SCENE, "range": [RLO, RHI], "filter": _FILTER,
          "errors": [], "mapped_events": 0, "matched": 0}
t0 = time.time()
OUT_LINES = []


def note_err(section, e):
    msg = "%s: %s: %s" % (section, type(e).__name__, e)
    report["errors"].append(msg)
    print("[error]", msg)


def finish(code):
    """Always write OUT_TXT before exiting (runner judges by files, not exit code)."""
    report["elapsed_sec"] = round(time.time() - t0, 1)
    try:
        with open(OUT_TXT, "w", encoding="utf-8") as f:
            f.write("# api-scan scene=%s range=%d-%d filter=%s\n"
                    % (_SCENE, RLO, RHI, ",".join(_FILTER)))
            for k in ("mapped_events", "matched", "elapsed_sec"):
                if k in report:
                    f.write("# %s=%s\n" % (k, report[k]))
            for e in report["errors"]:
                f.write("# ERROR: %s\n" % e)
            for ln in OUT_LINES:
                f.write(ln + "\n")
        print("written:", OUT_TXT, "(%d lines)" % len(OUT_LINES))
    except Exception as e:
        print("[error] finish.write: %s" % e)
    print("elapsed %.1fs errors=%d" % (report["elapsed_sec"], len(report["errors"])))
    print("RESULT: %s" % ("SUCCESS" if code == 0 else "FAILED"))
    if os.environ.get("RDC_HEADLESS"):
        sys.exit(code)


def sd_val(o, depth=0):
    """Stringify one SDObject (leaf value, or struct/array children depth-capped)."""
    try:
        bt = int(o.type.basetype)
        if bt == int(rd.SDBasic.Null):
            return "null"
        if bt == int(rd.SDBasic.String):
            return repr(o.AsString())
        if bt == int(rd.SDBasic.Boolean):
            return str(o.AsBool())
        if bt == int(rd.SDBasic.Float):
            return str(o.AsFloat())
        if bt == int(rd.SDBasic.Character):
            return repr(o.data.basic.c)
        if bt == int(rd.SDBasic.Resource):
            return "res:%s" % str(o.data.basic.id)
        if bt == int(rd.SDBasic.GPUAddress):
            return "0x%x" % int(o.data.basic.u)
        if bt in (int(rd.SDBasic.UnsignedInteger), int(rd.SDBasic.SignedInteger),
                  int(rd.SDBasic.Enum)):
            v = o.data.basic.i
            try:
                if int(o.type.flags) & int(rd.SDTypeFlags.HasCustomString):
                    return "%d(%s)" % (v, o.data.string)
            except Exception:
                pass
            return str(v)
        n = o.NumChildren()
        if depth >= 3:
            return "{%d children}" % n
        is_arr = bt == int(rd.SDBasic.Array)
        parts = []
        for i in range(min(n, 24)):
            c = o.GetChild(i)
            if c is None:
                continue
            if is_arr:
                parts.append(sd_val(c, depth + 1))
            else:
                parts.append("%s=%s" % (c.name, sd_val(c, depth + 1)))
        if n > 24:
            parts.append("...+%d" % (n - 24))
        return "{" + ",".join(parts) + "}"
    except Exception as e:
        return "err:%s" % type(e).__name__


print("=== api scan ===")
print("path:", RDC, "| range: %d-%d" % (RLO, RHI))

# ===== 1. open capture =====
try:
    capf = rd.OpenCaptureFile()
    st = None
    for ud in ("", b""):
        try:
            st = capf.OpenFile(RDC, ud, None)
            print("OpenFile: %r" % (st,))
            break
        except Exception as e:
            print("OpenFile raised: %s: %s" % (type(e).__name__, e))
            st = None
    if st is None or "fail" in str(st).lower() or "error" in str(st).lower():
        note_err("OpenFile", "open failed: %r" % (st,))
        finish(2)
except SystemExit:
    raise
except Exception as e:
    note_err("open-capture", e)
    finish(2)

# ===== 2. structured data (serialised calls; no replay needed for this part) =====
sf, sf_how = None, None
try:
    sf = capf.GetStructuredData()
    sf_how = "capf.GetStructuredData"
except Exception as e:
    note_err("capf.GetStructuredData", e)
if sf is None:
    finish(3)
print("structured data via %s | chunks=%d" % (sf_how, len(sf.chunks)))

# ===== 3. replay controller -> action tree (eventId -> chunkIndex) =====
ctrl = None
try:
    res = capf.OpenCapture(rd.ReplayOptions(), None)
    ctrl = res[1] if (isinstance(res, tuple) and len(res) == 2) else res
except Exception as e:
    note_err("OpenCapture", e)
if ctrl is None:
    finish(4)

ev2ci = {}
try:
    flat = []

    def walk(actions):
        for a in actions:
            flat.append(a)
            if a.children:
                walk(a.children)

    walk(ctrl.GetRootActions())
    n_ev = 0
    no_chunk = getattr(rd.APIEvent, "NoChunk", -1)
    for a in flat:
        evs = a.events
        if not evs:
            continue
        for e in evs:
            n_ev += 1
            try:
                ci = int(e.chunkIndex)
            except Exception:
                continue
            if ci == no_chunk or ci < 0 or ci >= len(sf.chunks):
                continue
            ev2ci[int(e.eventId)] = ci
    report["mapped_events"] = len(ev2ci)
    print("actions=%d api-events=%d mapped=%d" % (len(flat), n_ev, len(ev2ci)))
except SystemExit:
    raise
except Exception as e:
    note_err("action-tree", e)

# ===== 4. filter chunks in range =====
try:
    for eid in sorted(ev2ci):
        if eid < RLO or eid > RHI:
            continue
        chunk = sf.chunks[ev2ci[eid]]
        try:
            cname = str(chunk.name)
        except Exception:
            cname = str(chunk.type.name)
        if not any(f in cname for f in _FILTER):
            continue
        try:
            n = chunk.NumChildren()
            parts = []
            for i in range(min(n, 24)):
                c = chunk.GetChild(i)
                if c is None:
                    continue
                if int(c.type.flags) & int(rd.SDTypeFlags.Hidden):
                    continue
                parts.append("%s=%s" % (c.name, sd_val(c)))
            if n > 24:
                parts.append("...+%d" % (n - 24))
            body = " ".join(parts)
        except Exception as e:
            body = "(params err: %s: %s)" % (type(e).__name__, e)
        OUT_LINES.append("ev%-7d %s  %s" % (eid, cname, body))
    report["matched"] = len(OUT_LINES)
    print("matched %d calls in range %d-%d" % (len(OUT_LINES), RLO, RHI))
except SystemExit:
    raise
except Exception as e:
    note_err("scan", e)

finish(1 if report["errors"] else 0)
