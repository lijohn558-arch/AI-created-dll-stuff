# rdc_dump_backbuffer.py —— 把抓帧末态 backbuffer 整张导出为 PNG (无 PIL, 手写 PNG)
#
# 用法 (与 rdc_pass7_pixels 同范式, 走 rdc_run.ps1):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_dump_backbuffer.py -Scene S5b
# 输出:
#   docs\analysis\<prefix>-backbuffer.png   (prefix = 场景 ID/全名的 '-' 前段)
#
# 目的:
#   1) 给 PoC-B 留"注入进了呈现帧"的整帧图证 (左上角洋红块+三角形);
#   2) 把基线帧与候选帧并排看, 判断两帧场景状态是否可比 (结构配对的噪声来源);
#   3) 不需要游戏、不需要人工截图, 复抓一次即出图。
#
# 实测口径 (与 rdc_pass7_pixels 一致):
#   身份: ResourceType.SwapchainImage (权威) → 找不到才退 1920x1080 RGBA8 弱口径;
#   数据: ctrl.GetTextureData(resId, rd.Subresource(0,0,0)) 末态整图, 行距 = len/height (紧凑);
#   读点: 顶层原点 (RenderDoc 口径), RGBA8 → PNG 丢 A 留 RGB。
import renderdoc as rd
import json
import os
import struct
import sys
import time
import zlib

# ===== 配置 (与 rdc_pass7_pixels / rdc_extract 同范式) =====
_SCENES = {"S1": "S1-city-day", "S2": "S2-interior", "S3": "S3-forest-godrays",
           "S4": "S4-water", "S5": "S5-night-combat"}
_RDC_SCENE = os.environ.get("RDC_SCENE", "").strip() or "S4"
_FULL = _SCENES.get(_RDC_SCENE, _RDC_SCENE)
_PREFIX = _FULL.split("-")[0]
RDC = r"C:\Users\joker\skyrim-vulkan\captures\%s.rdc" % _FULL
OUT_PNG = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-backbuffer.png" % _PREFIX
OUT_JSON = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-backbuffer.json" % _PREFIX

report = {"rdc": RDC, "scene": _RDC_SCENE, "full": _FULL, "png": OUT_PNG,
          "errors": [], "identified_by": None}
t0 = time.time()


def note_err(section, e):
    msg = "%s: %s: %s" % (section, type(e).__name__, e)
    report["errors"].append(msg)
    print("[error]", msg)


def write_png(path, w, h, rows):
    """rows: 长度 h 的 bytes 列表, 每行 w*3 字节 (RGB) → 8-bit RGB PNG。"""
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


print("=== backbuffer 整帧导出 ===")
print("路径:", RDC)
try:
    print("文件存在:", os.path.isfile(RDC), "| 大小: %.1f MB" % (os.path.getsize(RDC) / 1048576.0))
except Exception as e:
    note_err("os.path.getsize", e)

# ===== 打开抓帧 (与 rdc_extract 同范式) =====
capf = rd.OpenCaptureFile()
st = None
for ud in ("", b""):
    try:
        st = capf.OpenFile(RDC, ud, None)
        print("OpenFile 返回: %r" % (st,))
        break
    except Exception as e:
        print("OpenFile 抛异常: %s: %s" % (type(e).__name__, e))
        st = None
if st is None:
    note_err("OpenFile", "所有 userData 尝试均抛异常")
    raise SystemExit(2)
if ("fail" in str(st).lower()) or ("error" in str(st).lower()):
    note_err("OpenFile", "打开失败: %r" % (st,))
    raise SystemExit(3)

ctrl = None
try:
    res = capf.OpenCapture(rd.ReplayOptions(), None)
    ctrl = res[1] if (isinstance(res, tuple) and len(res) == 2) else res
except Exception as e:
    note_err("OpenCapture", e)
if ctrl is None:
    note_err("OpenCapture", "未能创建 replay controller")
    raise SystemExit(4)

# ===== 推到末事件 (注入发生在 Present 之前, 末态即本帧呈现内容) =====
try:
    max_eid = 0

    def _walk(actions):
        global max_eid
        for a in actions:
            if a.eventId > max_eid:
                max_eid = a.eventId
            if a.children:
                _walk(a.children)

    _walk(ctrl.GetRootActions())
    ctrl.SetFrameEvent(max_eid, True)
    print("末事件 eventId =", max_eid)
except Exception as e:
    note_err("walk/SetFrameEvent", e)

# ===== 找 backbuffer: SwapchainImage 权威身份 → RGBA8 同尺寸兜底 =====
bb_res, bb_how = None, None
try:
    for r in ctrl.GetResources():
        if r.type == rd.ResourceType.SwapchainImage:
            bb_res, bb_how = r, "SwapchainImage"
            break
except Exception as e:
    note_err("GetResources", e)

if bb_res is None:
    try:
        for t in ctrl.GetTextures():
            fn = str(getattr(t.format, "name", t.format)).replace(" ", "")
            if t.width >= 1280 and t.height >= 720 and "R8G8B8A8" in fn:
                bb_res, bb_how = t, "fallback-format-size"
                break
    except Exception as e:
        note_err("GetTextures(fallback)", e)

if bb_res is None:
    report["errors"].append("backbuffer: 无 SwapchainImage 也无同尺寸 RGBA8")
    print("[error] 未找到 backbuffer")
else:
    report["identified_by"] = bb_how
    desc = None
    try:
        for t in ctrl.GetTextures():
            if t.resourceId == bb_res.resourceId:
                desc = t
                break
    except Exception as e:
        note_err("GetTextures", e)
    w = int(desc.width) if desc is not None else 0
    h = int(desc.height) if desc is not None else 0
    report["resourceId"] = str(bb_res.resourceId)
    report["width"], report["height"] = w, h
    print("backbuffer:", bb_how, report["resourceId"], "%dx%d" % (w, h))

    data = None
    try:
        data = ctrl.GetTextureData(bb_res.resourceId, rd.Subresource(0, 0, 0))
    except Exception as e:
        note_err("GetTextureData", e)

    if data and w and h:
        n = len(data)
        pitch = n // h
        report["data_bytes"] = n
        report["row_pitch"] = pitch
        print("GetTextureData: %d B, row_pitch=%d (期望 %d)" % (n, pitch, w * 4))
        if pitch < w * 4:
            note_err("row_pitch", "行距 %d < w*4=%d, 拒绝导出 (避免花屏假图)" % (pitch, w * 4))
        else:
            rows = []
            for y in range(h):
                base = y * pitch
                # RGBA → RGB (丢 A); 逐行切片, data 支持索引/切片
                rgb = bytearray(w * 3)
                for x in range(w):
                    s = base + x * 4
                    d = x * 3
                    rgb[d] = data[s]
                    rgb[d + 1] = data[s + 1]
                    rgb[d + 2] = data[s + 2]
                rows.append(bytes(rgb))
            blob = write_png(OUT_PNG, w, h, rows)
            report["png_bytes"] = blob
            print("已导出: %s (%.1f KB)" % (OUT_PNG, blob / 1024.0))

            # 顺手把探针同款 4 角点也记上, 便于与 pass7 互证
            def px(x, y):
                o = y * pitch + x * 4
                return [data[o], data[o + 1], data[o + 2]]

            report["spotcheck"] = {
                "rect-corner-1(16+6,16+6)": px(22, 22),
                "triangle-centroid(272,313)": px(272, 313),
                "control(1056,540)": px(1056, 540),
            }
            print("抽检:", report["spotcheck"])

report["elapsed_sec"] = round(time.time() - t0, 1)
try:
    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    print("已导出:", OUT_JSON)
except Exception as e:
    note_err("json.dump", e)
print("完成, 用时 %.1fs, 错误数 %d" % (report["elapsed_sec"], len(report["errors"])))

# 任意错误 → 非零退出, 让 rdc_run.ps1 报 FAILED (与 pass7 同口径)
if report["errors"] or not report.get("png_bytes"):
    print("RESULT: FAILED")
    if os.environ.get("RDC_HEADLESS"):
        sys.exit(1)
else:
    print("RESULT: SUCCESS")
    if os.environ.get("RDC_HEADLESS"):
        sys.exit(0)
