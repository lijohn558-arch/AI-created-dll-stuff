# rdc_cbuf_scan.py : dump VS/PS constant blocks (first N bytes, float view) for sampled draws
#                    and hunt for the REAL projection matrix -- R4 evidence.
#
# Why: rdc_pass6.py:491 only unpacks the first 64 bytes of each constant block
#      (struct "<16f"), so a projection matrix sitting deeper in a 320-byte cbuffer
#      (typical layout World / WorldView / WorldViewProj / ...) is invisible.
#      Scan of S1~S5 head16f found 18 matrices, 13 non-identity, ALL rotations of
#      ResourceId::1463 -> no projection. docs/02 S14.29 records that result.
#
# Usage (headless): powershell tools\rdc_run.ps1 -Script rdc_cbuf_scan.py -Scene S4 -Stride 25
#   RDC_SCENE     scene id (S1..S5) or full .rdc path
#   RDC_STRIDE    sample every Nth draw (default 25)
#   RDC_EVENTS    explicit event ids to sample in addition (comma separated)
#   RDC_CB_BYTES  bytes to read per constant block (default 512)
# Output: docs/analysis/<prefix>-cbuf-scan.json + .log

import renderdoc as rd
import json
import math
import os
import struct
import sys
import time

_SCENES = {"S1": "S1-city-day", "S2": "S2-interior", "S3": "S3-forest-godrays",
           "S4": "S4-water", "S5": "S5-night-combat"}
_RDC_SCENE = os.environ.get("RDC_SCENE", "S4").strip() or "S4"
_FULL = _SCENES.get(_RDC_SCENE, _RDC_SCENE)
_PREFIX = _FULL.split("-")[0]
if os.path.isfile(_RDC_SCENE):
    RDC = _RDC_SCENE
    _PREFIX = os.path.splitext(os.path.basename(_RDC_SCENE))[0].split("-")[0]
else:
    RDC = r"C:\Users\joker\skyrim-vulkan\captures\%s.rdc" % _FULL
OUT_JSON = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-cbuf-scan.json" % _PREFIX

DRAW_STRIDE = max(1, int(os.environ.get("RDC_STRIDE", "25")))
CB_BYTES = max(64, int(os.environ.get("RDC_CB_BYTES", "512")))
EXTRA_EVENTS = []
for part in os.environ.get("RDC_EVENTS", "").replace(";", ",").split(","):
    part = part.strip()
    if part:
        try:
            EXTRA_EVENTS.append(int(part))
        except ValueError:
            pass


class _Tee(object):
    def __init__(self, *streams):
        self.streams = streams

    def write(self, s):
        for st in self.streams:
            try:
                st.write(s)
                st.flush()
            except Exception:
                pass

    def flush(self):
        for st in self.streams:
            try:
                st.flush()
            except Exception:
                pass


try:
    _logf = open(os.path.splitext(OUT_JSON)[0] + ".log", "w", encoding="utf-8")
    sys.stdout = _Tee(sys.stdout, _logf)
except Exception:
    pass

report = {"rdc": RDC, "errors": [], "script": "rdc_cbuf_scan v1",
          "stride": DRAW_STRIDE, "cb_bytes": CB_BYTES, "extra_events": EXTRA_EVENTS,
          "blocks": [], "proj_hits": []}
t0 = time.time()


def note_err(section, e):
    msg = "%s: %s: %s" % (section, type(e).__name__, e)
    report["errors"].append(msg)
    print("[err] " + msg)


def transpose4(m):
    return [m[c * 4 + r] for r in range(4) for c in range(4)]


def try_proj(w):
    """Return derived dict if the 16 floats look like a D3D perspective projection, else None.

    Row-major pattern (HLSL mul(v, M)): m00=a m11=b m22=c m23=1 m32=d, rest ~0
      a = 1/(aspect*tanHalfFov), b = 1/tanHalfFov, c = f/(f-n), d = -n*f/(f-n)
    Column-major convention = the transpose, so test both.
    """
    if len(w) != 16:
        return None
    for tag, M in (("row", list(w)), ("col", transpose4(list(w)))):
        if abs(M[11] - 1.0) > 0.02:
            continue
        if not (M[14] < -0.05 and M[14] > -1e7):
            continue
        if not (M[0] > 1e-4 and M[5] > 1e-4 and M[10] > 0.5):
            continue
        # every index except {0,5,10,11,14} must be ~0 (11 entries)
        bad = 0
        for i in range(16):
            if i in (0, 5, 10, 11, 14):
                continue
            if abs(M[i]) > 1e-3:
                bad += 1
        if bad > 1:
            continue
        b = M[5]
        a = M[0]
        c = M[10]
        d = M[14]
        tan_half = 1.0 / b
        fov_deg = 2.0 * math.atan(tan_half) * 180.0 / math.pi
        aspect = b / a
        near = -d / c
        far = near * c / (c - 1.0) if abs(c - 1.0) > 1e-9 else float("inf")
        if not (0.1 < near < 10000.0 and far > near * 5.0 and 15.0 < fov_deg < 150.0
                and 0.4 < aspect < 4.0):
            continue
        return {"layout": tag, "m00": round(a, 6), "m11": round(b, 6),
                "m22": round(c, 6), "m23": round(M[11], 6), "m32": round(d, 6),
                "fov_deg": round(fov_deg, 4), "aspect": round(aspect, 4),
                "near": round(near, 4), "far": round(far, 2),
                "tanHalfFov": round(tan_half, 6)}
    return None


print("=== open: " + RDC)
capf = rd.OpenCaptureFile()
st = None
for ud in ("", b""):
    try:
        st = capf.OpenFile(RDC, ud, None)
        print("OpenFile -> %r" % (str(st),))
        break
    except Exception as e:
        print("OpenFile(userData) exception: %s" % e)
        st = None
if st is None:
    raise SystemExit(2)
low = str(st).lower()
if "success" not in low and any(x in low for x in ("fail", "error", "unsupported", "corrupt")):
    print(">>> open failed: " + str(st))
    raise SystemExit(3)
try:
    res = capf.OpenCapture(rd.ReplayOptions(), None)
    ctrl = res[1] if isinstance(res, tuple) else res
except Exception as e:
    note_err("OpenCapture", e)
    ctrl = None
if ctrl is None:
    raise SystemExit(4)


def enum_int(name):
    f = getattr(rd.ActionFlags, name, None)
    return int(f) if f is not None else None


flat = []


def walk(actions):
    for a in actions:
        flat.append(a)
        try:
            if a.children:
                walk(a.children)
        except Exception:
            pass


draws = []
try:
    dc = enum_int("Drawcall")
    walk(ctrl.GetRootActions())
    for a in flat:
        try:
            af = int(a.flags)
        except Exception:
            af = 0
        if dc and (af & dc):
            draws.append(a)
    print("Draw count = %d" % len(draws))
except Exception as e:
    note_err("enum draws", e)

sample = []
for i, a in enumerate(draws):
    if i % DRAW_STRIDE == 0:
        sample.append(int(a.eventId))
for ev in EXTRA_EVENTS:
    if ev not in sample:
        sample.append(ev)
sample.sort()
print("sampling %d events" % len(sample))

STAGES = (("VS", rd.ShaderStage.Vertex), ("PS", rd.ShaderStage.Pixel))
for ev in sample:
    try:
        ctrl.SetFrameEvent(ev, False)
        pipe = ctrl.GetPipelineState()
    except Exception as e:
        note_err("SetFrameEvent(%d)" % ev, e)
        continue
    for sname, stage in STAGES:
        try:
            cbs = pipe.GetConstantBlocks(stage)
        except Exception as e:
            note_err("GetConstantBlocks(%s)@%d" % (sname, ev), e)
            continue
        for slot, c in enumerate(list(cbs)):   # v2: scan ALL slots (main camera may sit >5)
            try:
                rid = c.descriptor.resource
                if rid is None or int(rid) < 0:
                    continue
                raw = bytes(ctrl.GetBufferData(rid, 0, CB_BYTES))
                if len(raw) < 64:
                    continue
                n = len(raw) // 4
                vals = list(struct.unpack("<%df" % n, raw[:n * 4]))
                rec = {"ev": ev, "stage": sname, "slot": slot, "id": str(rid),
                       "bytes": len(raw), "floats": vals}
                hit = None
                # stride 4 floats (16 B) windows: catches matrices at 64/128/192/256... offsets
                for off in range(0, n - 15, 4):
                    hit = try_proj(vals[off:off + 16])
                    if hit:
                        hit.update({"ev": ev, "stage": sname, "slot": slot,
                                    "id": str(rid), "floatOffset": off})
                        break
                if hit:
                    report["proj_hits"].append(hit)
                    print("PROJ ev=%d %s slot=%d res=%s off=%d fov=%.3f aspect=%.3f near=%.3f far=%.1f (%s)"
                          % (ev, sname, slot, rid, hit["floatOffset"], hit["fov_deg"],
                             hit["aspect"], hit["near"], hit["far"], hit["layout"]))
                report["blocks"].append(rec)
            except Exception as e:
                note_err("cb %s@%d slot=%d" % (sname, ev, slot), e)

report["elapsed"] = round(time.time() - t0, 1)
report["blocks_scanned"] = len(report["blocks"])
print("blocks=%d proj_hits=%d errors=%d elapsed=%ss"
      % (report["blocks_scanned"], len(report["proj_hits"]), len(report["errors"]),
         report["elapsed"]))

with open(OUT_JSON, "w", encoding="utf-8") as f:
    json.dump(report, f, ensure_ascii=False, indent=1)
print("wrote " + OUT_JSON)
sys.exit(0)
