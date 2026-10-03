# rdc_pass7_pixels.py — PoC-B 像素探针: 读抓帧 backbuffer, 判"我们注入的像素在不在最终帧里"
#
# 为什么需要它 (docs/00 首周行动项 #4 / docs/01 §8 PoC-B 验收):
#   PoC-B 在 Present 钩子里把 Vulkan 离屏渲的 512x512 图 (洋红清屏 + 三角形)
#   拷进游戏 backbuffer 的 (16,16)。rdc_compare 的 10 条锚点全是结构锚点 (计数/格式/
#   哈希), 看不出像素 —— "注入是否真的落进抓帧" 必须直接读像素才能机器判定。
#   这里读的是 **SwapchainImage** 资源 (RenderDoc 对交换链 backbuffer 的正式身份),
#   不是随便哪张同尺寸纹理。
#
# 探针点 (全局屏幕坐标, 与 src\poc-presenter\main.cpp 的 POCB_X/Y/W/H 对齐):
#   - 注入矩形四角内侧 6px  → 期望洋红 (255,0,255) —— DLL 里的 vkCmdClearColorAttachments 哨兵
#   - 三角形重心 (272,313) → 期望绿系 (顶点色平均 85,217,85; 覆盖住即说明几何也进帧了)
#   - 矩形外两点           → 期望**非**洋红 (证明没把整帧污染成哨兵色)
#
# 判定:
#   SENTINEL_FOUND   四角中 >=3 个是洋红, 且矩形外对照点不是洋红 → 注入进了抓帧
#   SENTINEL_ABSENT  四角一个都不是洋红                          → 未注入 (基线帧应得此值)
#   PARTIAL          其余情形 (角点部分命中 / 对照点也被污染)      → 需人工看 rgb 明细
#   NO_BACKBUFFER    抓帧里找不到 SwapchainImage
#   注: verdict 只描述像素; errors[] 为空才代表"脚本本身执行成功" (项目判定口径)。
#
# 用法:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 `
#       -Script rdc_pass7_pixels.py -Scene S4b
#   (Scene 未知 ID 时按原样拼 captures\<Scene>.rdc, 与 rdc_extract 同范式)
#
# 输出: docs/analysis/<prefix>-pass7-pixels.json (+ errors / elapsed_sec)

import json
import os
import sys
import time

import renderdoc as rd

# ===== 配置 =====
_SCENES = {"S1": "S1-city-day", "S2": "S2-interior", "S3": "S3-forest-godrays",
           "S4": "S4-water", "S5": "S5-night-combat"}
_RDC_SCENE = os.environ.get("RDC_SCENE", "").strip() or "S4"
_FULL = _SCENES.get(_RDC_SCENE, _RDC_SCENE)
_PREFIX = _FULL.split("-")[0]
RDC = r"C:\Users\joker\skyrim-vulkan\captures\%s.rdc" % _FULL
OUT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-pass7-pixels.json" % _PREFIX

# 与 poc-presenter 的 POCB_* 常量一致 (main.cpp)
POCB_X, POCB_Y, POCB_W, POCB_H = 16, 16, 512, 512
TRI_CX, TRI_CY = 256, 297  # 三角形重心 (相对矩形) —— NDC y 向下, v=(-0.66,.6)(.66,.6)(0,-.72)

report = {"rdc": RDC, "scene": _RDC_SCENE, "full": _FULL, "errors": [],
          "probes": [], "verdict": "NO_BACKBUFFER"}
t0 = time.time()


def _fmt_name(fmt):
    """ResourceFormat -> 可读格式名。本版 renderdoc 的 ResourceFormat 既无 .name 也无 __str__
    (str() 只给 SWIG 对象地址), 退路: 逐属性尝试 + 最后用 '名称?+组件数' 兜底。"""
    for attr in ("name", "Name", "typeName", "formatName"):
        v = getattr(fmt, attr, None)
        if callable(v):
            try:
                v = v()
            except Exception:
                v = None
        if v:
            return str(v)
    bits = []
    for attr in ("compCount", "componentCount"):
        v = getattr(fmt, attr, None)
        if v is not None:
            bits.append("%scomp" % v)
            break
    for attr in ("compType", "componentType", "Type"):
        v = getattr(fmt, attr, None)
        if v is not None:
            bits.append(str(v))
            break
    return ("ResourceFormat(" + ",".join(bits) + ")") if bits else "ResourceFormat(?)"


def note_err(section, e):
    msg = "%s: %s: %s" % (section, type(e).__name__, e)
    report["errors"].append(msg)
    print("[错误]", msg)


print("=== PoC-B 像素探针 ===")
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

ok = None
low = str(st).lower()
if "success" in low:
    ok = True
elif any(x in low for x in ("fail", "error", "unrecognis", "unsupported", "corrupt",
                            "notfound", "access", "denied", "ioerror", "fileio")):
    ok = False
else:
    try:
        ok = (int(st) == 0)
    except Exception:
        ok = None
print("打开判定 ok =", ok)
if ok is False:
    note_err("OpenCaptureFile", "打开失败: %r" % (st,))
    raise SystemExit(3)

ctrl = None
try:
    res = capf.OpenCapture(rd.ReplayOptions(), None)
    if isinstance(res, tuple) and len(res) == 2:
        print("Replay:", res[0])
        ctrl = res[1]
    else:
        ctrl = res
except Exception as e:
    note_err("OpenCapture", e)
if ctrl is None:
    note_err("OpenCapture", "未能创建 replay controller")
    raise SystemExit(4)

# ===== 推到末事件 (注入发生在 Present 之前, 末态即"本帧呈现内容") =====
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
    print("末事件 eventId =", max_eid)
    try:
        ctrl.SetFrameEvent(max_eid, True)
    except Exception as e:
        note_err("SetFrameEvent", e)  # 不致命: 默认态通常已是末尾
except Exception as e:
    note_err("walk_actions", e)

# ===== 找 backbuffer: ResourceType.SwapchainImage (权威身份) =====
bb_res = None
bb_how = None
try:
    for r in ctrl.GetResources():
        if r.type == rd.ResourceType.SwapchainImage:
            bb_res, bb_how = r, "SwapchainImage"
            break
except Exception as e:
    note_err("GetResources", e)

if bb_res is None:
    # 兜底: 扫纹理表找 1920x1080 的 RGBA8 (弱证据, 会在 json 里标 fallback)
    try:
        for t in ctrl.GetTextures():
            fn = str(getattr(t.format, "name", t.format))
            if t.width >= 1280 and t.height >= 720 and "R8G8B8A8" in fn.replace(" ", ""):
                if bb_res is None:
                    bb_res, bb_how = t, "fallback-format-size"
    except Exception as e:
        note_err("GetTextures(fallback)", e)

if bb_res is None:
    report["verdict"] = "NO_BACKBUFFER"
    note_err("backbuffer", "抓帧里既无 SwapchainImage 也无同尺寸 RGBA8 纹理")
else:
    print("backbuffer 身份识别方式 =", bb_how, "| resourceId =", bb_res.resourceId)
    # ===== 取描述 =====
    desc = None
    try:
        for t in ctrl.GetTextures():
            if t.resourceId == bb_res.resourceId:
                desc = t
                break
    except Exception as e:
        note_err("GetTextures", e)
    if desc is not None:
        report["backbuffer"] = {
            "resourceId": str(desc.resourceId),
            "name": str(getattr(desc, "name", "")),
            "width": int(desc.width), "height": int(desc.height),
            "format": _fmt_name(desc.format),
            "mips": int(desc.mips), "arraySize": int(desc.arraysize),
            "msSamp": int(getattr(desc, "msSamp", 1)),
            "identified_by": bb_how,
        }
        print("backbuffer:", report["backbuffer"])

    # ===== 读末态像素 =====
    data = None
    try:
        data = ctrl.GetTextureData(bb_res.resourceId, rd.Subresource(0, 0, 0))
    except Exception as e:
        note_err("GetTextureData", e)

    if data:
        n = len(data)
        w = int(desc.width) if desc is not None else 0
        h = int(desc.height) if desc is not None else 0
        report["data_bytes"] = n
        pitch = n // h if h else 0
        report["row_pitch"] = pitch
        if w and h:
            report["bytes_per_pixel"] = round(n / float(w * h), 3)
        print("GetTextureData: %d B, 推得 row_pitch=%d (期望 %d)" % (n, pitch, w * 4))

        def px(x, y):
            """读 (x,y) 的 RGB —— x,y 顶层原点 (RenderDoc 口径)"""
            if h and (y < 0 or y >= h or x < 0 or x >= w):
                return None
            off = y * pitch + x * 4
            if off + 3 >= n:
                return None
            return [data[off], data[off + 1], data[off + 2]]

        def is_magenta(rgb):
            if not rgb:
                return False
            r, g, b = rgb
            return r >= 250 and g <= 5 and b >= 250

        # 四角内侧 6px (在注入矩形内, 且三角形画不到角上)
        corners = [(POCB_X + 6, POCB_Y + 6), (POCB_X + POCB_W - 7, POCB_Y + 6),
                   (POCB_X + 6, POCB_Y + POCB_H - 7), (POCB_X + POCB_W - 7, POCB_Y + POCB_H - 7)]
        for i, (x, y) in enumerate(corners):
            rgb = px(x, y)
            report["probes"].append({"label": "rect-corner-%d" % (i + 1), "x": x, "y": y,
                                     "rgb": rgb, "expect": "magenta", "ok": is_magenta(rgb)})
        # 三角形重心
        rgb = px(POCB_X + TRI_CX, POCB_Y + TRI_CY)
        greenish = bool(rgb and rgb[1] >= 150 and rgb[0] <= 170 and rgb[2] <= 170 and not is_magenta(rgb))
        report["probes"].append({"label": "triangle-centroid", "x": POCB_X + TRI_CX,
                                 "y": POCB_Y + TRI_CY, "rgb": rgb,
                                 "expect": "green(~85,217,85)", "ok": greenish})
        # 矩形外对照点 (证明不是整帧污染)
        for lbl, (x, y) in (("control-outside-1", (int(w * 0.55), int(h * 0.5))),
                            ("control-outside-2", (max(0, w - 20), max(0, h - 20)))):
            rgb = px(x, y)
            report["probes"].append({"label": lbl, "x": x, "y": y, "rgb": rgb,
                                     "expect": "not-magenta", "ok": not is_magenta(rgb)})

        corners_ok = sum(1 for p in report["probes"]
                         if p["label"].startswith("rect-corner") and p["ok"])
        ctrl_ok = all(p["ok"] for p in report["probes"] if p["label"].startswith("control"))
        tri = next((p for p in report["probes"] if p["label"] == "triangle-centroid"), None)
        if corners_ok >= 3 and ctrl_ok:
            report["verdict"] = "SENTINEL_FOUND"
        elif corners_ok == 0 and ctrl_ok:
            report["verdict"] = "SENTINEL_ABSENT"
        else:
            report["verdict"] = "PARTIAL"
        report["corners_magenta"] = "%d/4" % corners_ok
        report["controls_clean"] = ctrl_ok
        report["triangle_rgb"] = tri["rgb"] if tri else None
        print("PROBE:", json.dumps({"verdict": report["verdict"],
                                    "corners": report["corners_magenta"],
                                    "triangle_rgb": report["triangle_rgb"],
                                    "controls_clean": ctrl_ok}, ensure_ascii=False))
    else:
        note_err("GetTextureData", "返回空 (backbuffer 可能不可读)")

report["elapsed_sec"] = round(time.time() - t0, 1)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8") as f:
    json.dump(report, f, ensure_ascii=False, indent=1)
print("写出:", OUT, "| errors=%d | verdict=%s | elapsed=%.1fs"
      % (len(report["errors"]), report["verdict"], report["elapsed_sec"]))

if os.environ.get("RDC_HEADLESS") == "1":
    sys.exit(0)
