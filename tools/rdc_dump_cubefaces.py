# rdc_dump_cubefaces.py —— 把 cubemap 探针纹理 (RGBA16F 方形) 逐面导出为 PNG (无 PIL, 手写 PNG)
#
# 用法 (与 rdc_dump_backbuffer 同范式, 走 rdc_run.ps1):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_dump_cubefaces.py -Scene S4d
# 输出:
#   docs\analysis\<prefix>-cube-<rid>-face<i>.png  (逐面)
#   docs\analysis\<prefix>-cubefaces.json          (候选表 + 面统计 + md5 + API 自探)
#   docs\analysis\<prefix>-cubefaces.log           (逐阶段进度 —— 超时强杀时按尾部定位挂点)
#
# 目的:
#   1) 探针升质客观分辨率证据: S4b/S4c 基线 512x512 vs S4d 候选 1024x1024 (PNG 像素尺寸直接可读);
#   2) replay 内容诊断: 若 probe draw 的 OM 绑定在回放里解析不到 (pipe state 全 0),
#      导出的面内容应只有清屏纯色 —— 与有内容的基线面对比即可归因;
#   3) 面数据 md5 多样性: 6 面 md5 互不相同 = Subresource 切片参数正确 (否则全同 = 取错面)。
#
# API 口径 (2026-10-03 自探确认):
#   rd.Subresource(mip: int = 0, slice: int = 0, sample: int = 0)  ← slice 在第 2 位
#   取面 = rd.Subresource(0, face, 0); 另一序 (0,0,f) 留作 fallback, md5 多样性定序。
#   json 内字符串一律 ASCII (rdc_run.ps1 的 ConvertFrom-Json 按 ANSI 读, 中文会炸)。
#
# v2 (同日): 逐阶段进度日志 + 分阶段落盘 —— v1 曾在 900s 超时无产物, 挂点无法定位;
#   GetDebugMessages 移到全部产物写完之后 (头号挂点嫌疑, 纯诊断, 挂了也不损失主产物)。
import renderdoc as rd
import hashlib
import json
import os
import struct
import sys
import time
import zlib

# ===== 配置 (与其余 pass 同范式) =====
_SCENES = {"S1": "S1-city-day", "S2": "S2-interior", "S3": "S3-forest-godrays",
           "S4": "S4-water", "S5": "S5-night-combat"}
_RDC_SCENE = os.environ.get("RDC_SCENE", "").strip() or "S4"
_FULL = _SCENES.get(_RDC_SCENE, _RDC_SCENE)
_PREFIX = _FULL.split("-")[0]
RDC = r"C:\Users\joker\skyrim-vulkan\captures\%s.rdc" % _FULL
OUT_JSON = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-cubefaces.json" % _PREFIX
OUT_LOG = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-cubefaces.log" % _PREFIX

report = {"rdc": RDC, "scene": _RDC_SCENE, "full": _FULL,
          "errors": [], "api_probe": {}, "candidates": [],
          "fetch_notes": [], "face_stats": []}
t0 = time.time()
_logf = open(OUT_LOG, "w", encoding="ascii", errors="replace")
# 立即落标记: 若后续任何一步死掉, 日志至少有这一行 -> 能区分 "open 后立即挂" vs "压根没起来"
_logf.write("[   0.0s] start: script entered (open ok)\n")
_logf.flush()


def log(msg):
    # 先写文件后 print —— stdout 在某些批跑环境下可能阻塞/异常, 文件是可靠落点
    line = "[%7.1fs] %s" % (time.time() - t0, msg)
    try:
        _logf.write(line + "\n")
        _logf.flush()
    except Exception:
        pass
    try:
        print(line, flush=True)
    except Exception:
        pass


def save_json():
    try:
        with open(OUT_JSON, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=True, indent=1)
    except Exception as e:
        log("json.dump FAILED: %s: %s" % (type(e).__name__, e))


def note_err(section, e):
    msg = "%s: %s: %s" % (section, type(e).__name__, e)
    report["errors"].append(msg)
    log("[error] " + msg)


def write_png(path, w, h, rows):
    """rows: 长度 h 的 bytes 列表, 每行 w*3 字节 (RGB) -> 8-bit RGB PNG."""
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8bit, colorType=2 (RGB)
    blob = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob)


# ===== half-float -> 8bit 查找表 (Reinhard v/(1+v): HDR 结构可见且确定性) =====
def _h2f(h):
    s = (h >> 15) & 1
    e = (h >> 10) & 0x1F
    m = h & 0x3FF
    if e == 0:
        v = m * (2.0 ** -24)
    elif e == 31:
        # 防御: inf/nan 夹成有限值 —— inf/(1+inf)=nan -> int(nan) 会把整脚本炸死
        v = 65504.0 if m == 0 else 1.0  # inf -> max half; nan -> 中性 1.0
    else:
        v = (1.0 + m / 1024.0) * (2.0 ** (e - 15))
    if v != v:
        v = 1.0
    if v < 0.0:
        v = 0.0
    return v / (1.0 + v)


_LUT = []
for _h in range(65536):
    try:
        _LUT.append(max(0, min(255, int(round(255.0 * _h2f(_h))))))
    except Exception:
        _LUT.append(128)  # 兜底灰, 绝不因单个半浮点值中断


def fmt_of(t):
    """format 名 (pass4 fmt_name 口径: 试 name/Name, property 与 method 都兜)。"""
    f = getattr(t, "format", None)
    if f is None:
        return None
    for attr in ("name", "Name"):
        v = getattr(f, attr, None)
        if callable(v):
            v = v()
        if v:
            return str(v)
    return str(f)


log("=== cubeface export === path: " + RDC)

# ===== open capture (same boilerplate as rdc_dump_backbuffer) =====
capf = rd.OpenCaptureFile()
st = None
for ud in ("", b""):
    try:
        st = capf.OpenFile(RDC, ud, None)
        log("OpenFile -> %r" % (st,))
        break
    except Exception as e:
        log("OpenFile raised: %s: %s" % (type(e).__name__, e))
        st = None
if st is None:
    note_err("OpenFile", "all userData attempts raised")
    raise SystemExit(2)
if ("fail" in str(st).lower()) or ("error" in str(st).lower()):
    note_err("OpenFile", "open failed: %r" % (st,))
    raise SystemExit(3)

ctrl = None
try:
    log("OpenCapture (replay) ...")
    res = capf.OpenCapture(rd.ReplayOptions(), None)
    ctrl = res[1] if (isinstance(res, tuple) and len(res) == 2) else res
    log("OpenCapture done")
except Exception as e:
    note_err("OpenCapture", e)
if ctrl is None:
    note_err("OpenCapture", "no replay controller")
    raise SystemExit(4)

# ===== Subresource API self-probe (record for audit) =====
try:
    report["api_probe"]["Subresource_doc"] = str(rd.Subresource.__doc__)
except Exception as e:
    note_err("api_probe(Subresource doc)", e)
try:
    _s0 = rd.Subresource(0, 0, 0)
    report["api_probe"]["Subresource_inst_dir"] = [x for x in dir(_s0)
                                                   if not x.startswith("__")]
except Exception as e:
    note_err("api_probe(Subresource inst)", e)
report["api_probe"]["ctrl_debug_api"] = [x for x in dir(ctrl)
                                         if ("ebug" in x or "essage" in x)
                                         and not x.startswith("_")]
log("api probe recorded; ctrl debug api = %s"
    % report["api_probe"]["ctrl_debug_api"])

# ===== seek to last event (probe face content = end-of-frame residue) =====
try:
    max_eid = 0

    def _walk(actions):
        global max_eid
        for a in actions:
            if a.eventId > max_eid:
                max_eid = a.eventId
            if a.children:
                _walk(a.children)

    log("walk GetRootActions ...")
    _walk(ctrl.GetRootActions())
    log("SetFrameEvent(%d, True) ..." % max_eid)
    ctrl.SetFrameEvent(max_eid, True)
    log("last event = %d, seek done" % max_eid)
    report["last_event"] = max_eid
except Exception as e:
    note_err("walk/SetFrameEvent", e)

# ===== candidates: RGBA16F-ish + square + >=256, or TextureType cube =====
cands = []
samples = []
try:
    log("GetTextures ...")
    for t in ctrl.GetTextures():
        rid = None
        for a in ("resourceId", "id", "resource", "rid"):
            v = getattr(t, a, None)
            if v is not None and not callable(v):
                rid = v
                break
        w, h = int(getattr(t, "width", 0)), int(getattr(t, "height", 0))
        fn = fmt_of(t)
        fns = (fn + " " + str(getattr(t, "format", ""))).replace(" ", "")
        ty = str(getattr(t, "type", ""))
        arr = 0
        for a in ("arraySize", "arraysize"):
            if hasattr(t, a):
                arr = int(getattr(t, a))
                break
        if len(samples) < 20 and w >= 256:
            samples.append({"id": str(rid), "w": w, "h": h, "fmt": fn,
                            "type": ty, "arraySize": arr})
        fmt_ok = ("R16G16B16A16" in fns) or ("RGBA16F" in fns)
        cube_ok = "Cube" in ty
        if not ((fmt_ok or cube_ok) and w == h and w >= 256):
            continue
        cands.append({"id": str(rid), "rid_obj": rid, "w": w, "h": h, "arraySize": arr,
                      "mips": int(getattr(t, "mips", 0)), "type": ty,
                      "format": fn, "res": t})
except Exception as e:
    note_err("GetTextures", e)

report["tex_samples"] = samples
report["candidates"] = [{k: v for k, v in c.items() if k not in ("res", "rid_obj")}
                        for c in cands]
log("candidates: %d" % len(cands))
for c in report["candidates"]:
    log("  cand " + json.dumps(c, ensure_ascii=True))
if not cands:
    report["errors"].append("cubefaces: no RGBA16F/cube square candidate >=256")
save_json()  # 阶段落盘 1: 候选表

# ===== fetch faces (order self-validated by md5 diversity) =====
def fetch_faces(resid, w, h, nf):
    orders = [("mip0_sliceF_sample0", lambda f: rd.Subresource(0, f, 0)),
              ("mip0_sample0_sliceF", lambda f: rd.Subresource(0, 0, f))]
    expect = w * h * 8  # RGBA16F compact
    notes = []
    for name, mk in orders:
        data_list, md5s = [], []
        try:
            for f in range(nf):
                tg = time.time()
                log("  GetTextureData[%s] face%d (%s) ..." % (name, f, resid))
                d = ctrl.GetTextureData(resid, mk(f))
                log("  -> %d B in %.1fs" % (len(d) if d else 0, time.time() - tg))
                if not d:
                    raise ValueError("empty data at face%d" % f)
                data_list.append(d)
                md5s.append(hashlib.md5(bytes(d)).hexdigest()[:12])
        except Exception as e:
            notes.append("%s FAILED %s: %s" % (name, type(e).__name__, e))
            log("  " + notes[-1])
            continue
        lens = [len(d) for d in data_list]
        diverse = len(set(md5s)) > 1 or nf == 1
        notes.append("%s lens=%s expect=%d md5s=%s diverse=%s"
                     % (name, lens, expect, md5s, diverse))
        log("  " + notes[-1])
        # 每面首16字节 hex: 判 "零初始化" vs "清屏色" vs "实渲内容"
        for f, d in enumerate(data_list):
            head = bytes(d[:16]).hex()
            notes.append("  face%d head16=%s" % (f, head))
            log("  face%d head16=%s" % (f, head))
        if all(l >= expect for l in lens):
            if not diverse:
                notes.append("%s: NOT-diverse (faces byte-identical) -> "
                             "回放内容真一致假设 (pipe0 生效时清屏/绘制全失效, "
                             "面停留初始值) 或 slice 未生效 —— 用 S4b 对照区分" % name)
                log("  " + notes[-1])
            return name, data_list, notes
        if lens and lens[0] == expect * nf and nf > 1:
            blob = data_list[0]
            faces = [blob[i * expect:(i + 1) * expect] for i in range(nf)]
            notes.append("%s -> whole-blob concat, split into %d" % (name, nf))
            return "concat-split", faces, notes
    return None, None, notes


for c in cands:
    rid, w, h = c["id"], c["w"], c["h"]
    rid_obj = c.get("rid_obj", rid)  # GetTextureData 要 ResourceId 对象, 不是 str
    nf_try = [6] if (c.get("arraySize") or 0) == 6 else [6, 1]
    log("candidate %s %dx%d arr=%s type=%s: fetch"
        % (rid, w, h, c.get("arraySize"), c.get("type")))
    got_order, data_list = None, None
    for nf in nf_try:
        got_order, data_list, notes = fetch_faces(rid_obj, w, h, nf)
        report["fetch_notes"].extend(["%s/%d: %s" % (rid, nf, s) for s in notes])
        save_json()  # 阶段落盘 2: 取面结果
        if data_list:
            break
    if not data_list:
        note_err("faces", "resource %s: no face data" % rid)
        save_json()
        continue

    rid_tag = rid.replace("::", "").replace(":", "")
    for f, d in enumerate(data_list):
        tg = time.time()
        n = len(d)
        pitch = n // h
        if pitch < w * 8:
            note_err("row_pitch", "face%d pitch %d < w*8=%d" % (f, pitch, w * 8))
            continue
        rows = []
        stt = {"min": [65535] * 3, "max": [0] * 3, "sum": [0.0] * 3,
               "n": 0, "uniq": set()}
        for y in range(h):
            base = y * pitch
            px = struct.unpack_from("<%dH" % (w * 4), d, base)
            rgb = bytearray(w * 3)
            if y % 16 == 0:  # stats: sample every 16th row / 16th pixel
                for x in range(0, w, 16):
                    o = x * 4
                    for ch in range(3):
                        hv = px[o + ch]
                        if hv < stt["min"][ch]:
                            stt["min"][ch] = hv
                        if hv > stt["max"][ch]:
                            stt["max"][ch] = hv
                        stt["sum"][ch] += _h2f(hv)
                        stt["uniq"].add(_LUT[hv])
                    stt["n"] += 1
            for x in range(w):
                o = x * 4
                rgb[x * 3] = _LUT[px[o]]
                rgb[x * 3 + 1] = _LUT[px[o + 1]]
                rgb[x * 3 + 2] = _LUT[px[o + 2]]
            rows.append(bytes(rgb))
        png_path = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-cube-%s-face%d.png" % (
            _PREFIX, rid_tag, f)
        blob = write_png(png_path, w, h, rows)
        rec = {"resource": rid, "face": f, "w": w, "h": h,
               "order": got_order, "png": png_path, "png_bytes": blob,
               "raw_md5": hashlib.md5(bytes(d)).hexdigest(),
               "half_min": stt["min"], "half_max": stt["max"],
               "mean_float": [round(stt["sum"][ch] / max(1, stt["n"]), 4)
                              for ch in range(3)],
               "uniq_rgb8_sampled": len(stt["uniq"])}
        report["face_stats"].append(rec)
        log("face%d %dx%d md5=%s uniq8=%d png=%dB in %.1fs"
            % (f, w, h, rec["raw_md5"][:12], rec["uniq_rgb8_sampled"],
               blob, time.time() - tg))
        save_json()  # 阶段落盘 3: 逐面

report["elapsed_sec"] = round(time.time() - t0, 1)
save_json()
log("primary artifacts done, errors=%d" % len(report["errors"]))

# ===== GetDebugMessages (diagnostic, LAST — 头号挂点嫌疑, 挂了不损失主产物) =====
if os.environ.get("RDC_PROBE_DEBUG", "1").strip() not in ("0", ""):
    log("GetDebugMessages probe start ...")
    try:
        dg = getattr(ctrl, "GetDebugMessages", None)
        if dg is None:
            report["api_probe"]["GetDebugMessages"] = "absent"
        else:
            msgs = None
            for args in ((), (True,), (False,)):
                try:
                    log("  try GetDebugMessages%r" % (args,))
                    msgs = dg(*args)
                    report["api_probe"]["GetDebugMessages_args"] = str(args)
                    break
                except TypeError:
                    continue
            if msgs is None:
                report["api_probe"]["GetDebugMessages"] = "call signature not found"
            else:
                recs = []
                for m in list(msgs)[:60]:
                    # DebugMessage 字段 (renderdoc v1.47 文档): eventId/severity/category/source/description
                    try:
                        rec = "ev%-7s %-14s %-14s %-14s %s" % (
                            str(getattr(m, "eventId", "?")),
                            str(getattr(m, "severity", "?")),
                            str(getattr(m, "category", "?")),
                            str(getattr(m, "source", "?")),
                            str(getattr(m, "description", ""))[:400])
                    except Exception:
                        rec = str(m)[:300]
                    recs.append(rec)
                report["api_probe"]["debug_messages_n"] = len(recs)
                report["api_probe"]["debug_messages"] = recs
                log("GetDebugMessages -> %d msgs (first %d recorded)"
                    % (len(recs), len(recs)))
        save_json()
    except Exception as e:
        note_err("GetDebugMessages", e)
        save_json()

log("done %.1fs errors=%d" % (report["elapsed_sec"], len(report["errors"])))
if report["errors"] or not report.get("face_stats"):
    log("RESULT: FAILED")
    _logf.close()
    if os.environ.get("RDC_HEADLESS"):
        sys.exit(1)
else:
    log("RESULT: SUCCESS")
    _logf.close()
    if os.environ.get("RDC_HEADLESS"):
        sys.exit(0)
