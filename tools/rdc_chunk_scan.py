# rdc_chunk_scan.py — whole-file structured-data chunk census (NO replay needed)
#
# Headless usage (via rdc_run.ps1, env inherited):
#   $env:RDC_FILTER='CreateRenderTargetView,CreateDepthStencilView,CreateTexture2D,CreateShaderResourceView'
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_chunk_scan.py -Scene S4d
# Env:
#   RDC_SCENE  scene id
#   RDC_FILTER comma-separated chunk-name substrings to dump in detail (default Create set)
#   RDC_DUMP   max number of detailed dumps (default 80)
#
# Output: docs/analysis/<prefix>-chunk-scan.txt
#   # header + census (top names by count) + per-matching-chunk: idx <name> k=v ...
#
# Purpose (pipe0 attribution, 2026-10-03):
#   rdc_api_scan 只能看帧内 action 树引用的 chunk; 视图/纹理创建若发生在帧外 (init 流),
#   action 树里没有 → 永远扫不到。本脚本直接遍历 CaptureFile.GetStructuredData().chunks
#   全量普查: Create* 到底在不在文件里 (在 = 回放侧应可解析; 不在 = 视图创建不在 rdc,
#   pipe state 全 0 归因取证口径)。不需要 OpenCapture/replay → 快、省内存。
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
OUT_TXT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-chunk-scan.txt" % _PREFIX

_FILTER = [s.strip() for s in os.environ.get(
    "RDC_FILTER",
    "CreateTexture2D,CreateRenderTargetView,CreateDepthStencilView,CreateShaderResourceView").split(",")
    if s.strip()]
try:
    DUMP_MAX = int(os.environ.get("RDC_DUMP", "80"))
except Exception:
    DUMP_MAX = 80

report = {"rdc": RDC, "scene": _SCENE, "filter": _FILTER,
          "errors": [], "chunks": 0, "matched": 0}
t0 = time.time()
OUT_LINES = []


def note_err(section, e):
    msg = "%s: %s: %s" % (section, type(e).__name__, e)
    report["errors"].append(msg)
    print("[error]", msg)


def finish(code):
    report["elapsed_sec"] = round(time.time() - t0, 1)
    try:
        with open(OUT_TXT, "w", encoding="utf-8") as f:
            f.write("# chunk-scan scene=%s filter=%s dump_max=%d\n"
                    % (_SCENE, ",".join(_FILTER), DUMP_MAX))
            for k in ("chunks", "matched", "elapsed_sec"):
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
    """Stringify one SDObject (same pattern as rdc_api_scan.sd_val)."""
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
        if depth >= 4:
            return "{%d children}" % n
        is_arr = bt == int(rd.SDBasic.Array)
        parts = []
        for i in range(min(n, 32)):
            c = o.GetChild(i)
            if c is None:
                continue
            if is_arr:
                parts.append(sd_val(c, depth + 1))
            else:
                parts.append("%s=%s" % (c.name, sd_val(c, depth + 1)))
        if n > 32:
            parts.append("...+%d" % (n - 32))
        return "{" + ",".join(parts) + "}"
    except Exception as e:
        return "err:%s" % type(e).__name__


print("=== chunk scan (whole file, no replay) ===")
print("path:", RDC)

# ===== 1. open capture (file only — no OpenCapture) =====
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

# ===== 2. structured data =====
sf = None
try:
    sf = capf.GetStructuredData()
except Exception as e:
    note_err("capf.GetStructuredData", e)
if sf is None:
    finish(3)

chunks = sf.chunks
report["chunks"] = len(chunks)
print("chunks=%d" % len(chunks))

# ===== 3. census + detailed dumps =====
try:
    from collections import Counter
    census = Counter()
    hits = []
    for idx, ch in enumerate(chunks):
        try:
            cname = str(ch.name)
        except Exception:
            try:
                cname = str(ch.type.name)
            except Exception:
                cname = "?"
        census[cname] += 1
        if any(f in cname for f in _FILTER):
            hits.append((idx, cname, ch))
    report["matched"] = len(hits)

    # census: top 60 names
    OUT_LINES.append("== census: total %d chunks, %d distinct names ==" %
                     (len(chunks), len(census)))
    for name, cnt in census.most_common(60):
        OUT_LINES.append("census %-60s %d" % (name, cnt))
    # any name containing 'View' or 'Texture' beyond the filter (context)
    OUT_LINES.append("== names containing 'Create' (all) ==")
    for name, cnt in sorted(census.items()):
        if "Create" in name:
            OUT_LINES.append("create %-60s %d" % (name, cnt))

    # detailed dumps (capped)
    OUT_LINES.append("== detailed dumps: %d matching chunks, showing up to %d =="
                     % (len(hits), DUMP_MAX))
    for idx, cname, ch in hits[:DUMP_MAX]:
        try:
            n = ch.NumChildren()
            parts = []
            for i in range(min(n, 24)):
                c = ch.GetChild(i)
                if c is None:
                    continue
                try:
                    if int(c.type.flags) & int(rd.SDTypeFlags.Hidden):
                        continue
                except Exception:
                    pass
                parts.append("%s=%s" % (c.name, sd_val(c)))
            if n > 24:
                parts.append("...+%d" % (n - 24))
            body = " ".join(parts)
        except Exception as e:
            body = "(params err: %s: %s)" % (type(e).__name__, e)
        OUT_LINES.append("chunk#%-8d %s  %s" % (idx, cname, body))
    if len(hits) > DUMP_MAX:
        OUT_LINES.append("... +%d more matching chunks not dumped" %
                         (len(hits) - DUMP_MAX))
    print("census done: %d names, %d Create* hits" % (len(census), len(hits)))
except SystemExit:
    raise
except Exception as e:
    note_err("scan", e)

finish(1 if report["errors"] else 0)
