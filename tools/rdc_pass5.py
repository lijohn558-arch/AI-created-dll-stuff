# rdc_pass5.py — 第5轮: 全帧 RT 组织结构 (v4) — Draw 分段(同目标 Clear 处拆批) + Clear 目标&方位 + Copy 源/目标 + 逐 Draw viewport
#
# 用途: 判定场景渲染的 RT 组织 —— 是否存在独立反射 pass、水面折射拷贝的源/目标、各段资源身份
# v3: ① 每条 Clear 提取方位 (ActionFlags ClearColor/ClearDepthStencil 位, 含 raw flags 兜底)
#     ② 分段遇同目标 Clear 强制拆批 (新段标 after_clear=true) → 拆开段内多批渲染
# v4: ③ 逐 Draw 提取 viewport/scissor 尺寸直方图 (段内字段 viewports/scissors)
#     —— 解决 A/B 批身份: 绑定态被 S2 ev5507 否证后, "批A/批B 视口尺寸不同 = 写不同 mip/slice 的直证"
#     ④ 场景路径可用环境变量 RDC_SCENE 指定 (值 = 场景 ID, 如 S4/S5/S1), 不设则用下面两行字面量
# 用法: qrenderdoc -> Window -> Python Scripting -> 打开本文件 -> Run (UI 里不要开着抓帧)
#       无头运行: powershell tools\rdc_run.ps1 -Script rdc_pass5.py -Scene S5  (qrenderdoc --py)
# 说明: 场景无关; 各段独立 try/except, 错误记入 errors
# 输出: <场景>-extract-pass5.json + 控制台分段表 + 同名 .log (无头运行时 stdout 不可见, 日志落盘)

import renderdoc as rd
import json
import os
import sys
import time

# ===== 配置 =====
# 环境变量 RDC_SCENE 指定场景时按其拼路径 (headless 批跑用); 否则用字面量 (UI 手跑用)
# RDC_SCENE 接受短 ID (S1..S5) 或完整名 (S5-night-combat); 输出名一律取短 ID 前缀 (与历史文件一致)
_SCENES = {"S1": "S1-city-day", "S2": "S2-interior", "S3": "S3-forest-godrays",
           "S4": "S4-water", "S5": "S5-night-combat"}
_RDC_SCENE = os.environ.get("RDC_SCENE", "").strip()
if _RDC_SCENE:
    _FULL = _SCENES.get(_RDC_SCENE, _RDC_SCENE)
    _PREFIX = _FULL.split("-")[0]
    _CAP = r"C:\Users\joker\skyrim-vulkan\captures\%s.rdc" % _FULL
    _OUT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-extract-pass5.json" % _PREFIX
else:
    _CAP = r"C:\Users\joker\skyrim-vulkan\captures\S7-heavy-mods.rdc"
    _OUT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\S7-extract-pass5.json"
RDC = _CAP
OUT_JSON = _OUT

# 日志 tee: GUI 程序 stdout 不可见 → 同步落一份 .log (无头批跑排查用)
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

report = {"rdc": RDC, "errors": [], "script": "rdc_pass5 v4"}
t0 = time.time()


def note_err(section, e):
    msg = "%s: %s: %s" % (section, type(e).__name__, e)
    report["errors"].append(msg)
    print("[错误]", msg)


def enum_int(name):
    f = getattr(rd.ActionFlags, name, None)
    return int(f) if f is not None else None


# ===== 1. 打开抓帧 =====
print("=== 打开:", RDC)
capf = rd.OpenCaptureFile()
st = None
for ud in ("", b""):
    try:
        st = capf.OpenFile(RDC, ud, None)
        print("OpenFile 返回: %r" % (str(st),))
        break
    except Exception as e:
        print("OpenFile(userData=%r) 异常: %s" % (ud, e))
        st = None
if st is None:
    raise SystemExit(2)
low = str(st).lower()
if "success" not in low and any(x in low for x in ("fail", "error", "unsupported", "corrupt")):
    print(">>> 打开失败:", st)
    raise SystemExit(3)

ctrl = None
try:
    res = capf.OpenCapture(rd.ReplayOptions(), None)
    if isinstance(res, tuple) and len(res) == 2:
        _, ctrl = res
    else:
        ctrl = res
except Exception as e:
    note_err("OpenCapture", e)
if ctrl is None:
    raise SystemExit(4)


def valid_id(rid):
    try:
        return rid is not None and int(rid) >= 0
    except Exception:
        return str(rid) not in ("", "None", "<>")


_TEX_MAP = {}


def build_tex_map():
    """纹理全表: str(ResourceId) -> 'WxH[xD] 格式 [customName]'
    本版 API 无单查 GetTexture(rid)(会 AttributeError), 改用 GetTextures() 建全表 —— 与 pass4 同款做法"""
    try:
        for t in ctrl.GetTextures():
            rid = None
            for attr in ("resourceId", "id", "resource", "rid"):
                v = getattr(t, attr, None)
                if v is not None and not callable(v):
                    rid = v
                    break
            if rid is None:
                continue
            w = int(getattr(t, "width", 0))
            h = int(getattr(t, "height", 0))
            dep = int(getattr(t, "depth", 0))
            arr = int(getattr(t, "arraySize", 0))
            f = getattr(t, "format", None)
            fname = str(f) if f is not None else "?"
            if f is not None:
                for attr in ("name", "Name"):
                    v = getattr(f, attr, None)
                    if callable(v):
                        v = v()
                    if v:
                        fname = str(v)
                        break
            dims = "%dx%d" % (w, h)
            if dep > 1:
                dims += "x%d" % dep
            if arr > 1:
                dims += "[%darr]" % arr
            desc = "%s %s" % (dims, fname)
            cn = str(getattr(t, "customName", "") or "")
            if cn:
                desc += " [%s]" % cn
            _TEX_MAP[str(rid)] = desc
        print("纹理索引: %d" % len(_TEX_MAP))
    except Exception as e:
        note_err("GetTextures", e)


def tex_info(rid):
    """ResourceId -> 说明串 (对象/字符串均可; 取不到则 '?' + id; 无绑定返回 '-')"""
    if rid is None:
        return "-"
    s = str(rid)
    if s in ("None", "ResourceId::0", "0"):
        return "-"
    return _TEX_MAP.get(s, "?" + s)


build_tex_map()


# ===== 2. 枚举动作树 =====
try:
    flag_names = ["Drawcall", "Clear", "Copy", "Resolve"]
    flags = {n: enum_int(n) for n in flag_names}
    flags = {n: v for n, v in flags.items() if v is not None}

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

    draws, clears, copies, resolves = [], [], [], []
    for a in flat:
        try:
            af = int(a.flags)
        except Exception:
            af = 0
        if af & flags.get("Drawcall", 0):
            draws.append(a)
        if af & flags.get("Clear", 0):
            clears.append(a)
        if af & flags.get("Copy", 0):
            copies.append(a)
        if af & flags.get("Resolve", 0):
            resolves.append(a)
    print("Draw %d | Clear %d | Copy %d | Resolve %d" %
          (len(draws), len(clears), len(copies), len(resolves)))
except Exception as e:
    note_err("动作树", e)
    draws, clears, copies, resolves = [], [], [], []


def bound_targets():
    """当前事件的输出 RT 列表 + 深度目标 (ResourceId 对象)"""
    outs = []
    dsv = None
    try:
        ps = ctrl.GetPipelineState()
        for t in list(ps.GetOutputTargets()):
            rid = getattr(t, "resource", None)
            if valid_id(rid):
                outs.append(rid)
    except Exception:
        pass
    try:
        d = ps.GetDepthTarget()
        if d is not None:
            rid = getattr(d, "resource", None)
            if valid_id(rid):
                dsv = rid
    except Exception:
        pass
    return outs, dsv


def vp_key(pipe):
    """viewport 尺寸键 (v4 新增): '512x512' / 'off' / '?' —— 同目标不同视口 = 不同 mip/slice 的直证"""
    try:
        v = pipe.GetViewport(0)
        if v is None:
            return "?"
        if not getattr(v, "enabled", True):
            return "off"
        return "%dx%d" % (int(round(v.width)), int(round(v.height)))
    except Exception:
        return "?"


def sc_key(pipe):
    """scissor 尺寸键 (v4 新增)"""
    try:
        s = pipe.GetScissor(0)
        if s is None:
            return "?"
        if not getattr(s, "enabled", True):
            return "off"
        return "%dx%d@%d,%d" % (int(s.width), int(s.height), int(s.x), int(s.y))
    except Exception:
        return "?"


# ===== 3. Clear 目标（先行提取：分段拆批要用）=====
ASPECT_CANDS = ["ClearColor", "ClearColour", "ClearDepthStencil",
                "ClearDepth", "ClearStencil", "ClearBuffer", "ClearUAV"]
aspect_bits = {}
for _n in ASPECT_CANDS:
    _b = enum_int(_n)
    if _b is not None:
        aspect_bits[_n] = _b
if not aspect_bits:
    note_err("Clear方位", Exception("ActionFlags 无 Clear* 方位成员(尝试: %s)" % ",".join(ASPECT_CANDS)))

cl = []
try:
    for a in clears:
        try:
            ctrl.SetFrameEvent(a.eventId, False)
            outs, dsv = bound_targets()
            try:
                af = int(a.flags)
            except Exception:
                af = 0
            aspects = [n for n, b in aspect_bits.items() if af & b]
            cl.append({
                "eventId": a.eventId,
                "flags_raw": af,
                "aspects": aspects,
                "rts": [str(x) for x in outs],
                "rt_infos": [tex_info(x) for x in outs],
                "dsv": str(dsv) if dsv is not None else "-",
                "dsv_info": tex_info(dsv) if dsv is not None else "-",
            })
        except Exception as e:
            note_err("clear%d" % getattr(a, "eventId", "?"), e)
    cl.sort(key=lambda c: c["eventId"])
    report["clears"] = cl
    print("Clear 目标 %d 条:" % len(cl))
    for c in cl:
        print("  ev%d [%s] RTs=[%s] DS=%s" %
              (c["eventId"], "+".join(c["aspects"]) or "?",
               " | ".join(c["rt_infos"]) if c["rt_infos"] else "-", c["dsv_info"]))
except Exception as e:
    note_err("Clear", e)

# 清屏事件索引 (eventId, (rts元组, dsv)) 供分段在同目标 Clear 处强制拆批
# 过滤: 无绑定的 Clear(目标不可知, 疑 UAV/交换链) 不参与拆批, 但仍留在 report["clears"] 里
cl_list = [(c["eventId"], (tuple(c["rts"]), c["dsv"]))
           for c in cl if c["rts"] or c["dsv"] != "-"]


# ===== 4. Draw 分段 (按输出绑定变化点; 同目标 Clear 处强制拆批) =====
segments = []
try:
    print("开始逐 Draw 提取输出绑定并分段 (%d 个; 同目标 Clear 处拆批)..." % len(draws))
    ci = 0
    prev_eid = -1
    for i, a in enumerate(draws):
        try:
            ctrl.SetFrameEvent(a.eventId, False)
            outs, dsv = bound_targets()
            key = (tuple(str(x) for x in outs), str(dsv) if dsv is not None else "-")
            while ci < len(cl_list) and cl_list[ci][0] <= prev_eid:
                ci += 1
            split = False
            j = ci
            while j < len(cl_list) and cl_list[j][0] < a.eventId:
                if cl_list[j][1] == key:
                    split = True
                j += 1
            pipe = ctrl.GetPipelineState()
            psid = "-"
            try:
                psid = str(pipe.GetShader(rd.ShaderStage.Pixel))
            except Exception as e:
                psid = "?%s" % type(e).__name__
            vk = vp_key(pipe)   # v4 新增
            sk = sc_key(pipe)   # v4 新增
            if (not split) and segments and segments[-1]["key"] == key:
                seg = segments[-1]
                seg["end"] = a.eventId
                seg["draws"] += 1
                seg["ps"][psid] = seg["ps"].get(psid, 0) + 1
                seg["viewports"][vk] = seg["viewports"].get(vk, 0) + 1
                seg["scissors"][sk] = seg["scissors"].get(sk, 0) + 1
            else:
                segments.append({
                    "key": key,
                    "start": a.eventId,
                    "end": a.eventId,
                    "draws": 1,
                    "rts": [str(x) for x in outs],
                    "rt0": str(outs[0]) if outs else "-",
                    "dsv": key[1],
                    "ps": {psid: 1},
                    "viewports": {vk: 1},
                    "scissors": {sk: 1},
                    "after_clear": bool(split),
                    "rt_infos": [tex_info(x) for x in outs],
                    "dsv_info": tex_info(dsv) if dsv is not None else "-",
                })
            prev_eid = a.eventId
            if (i + 1) % 500 == 0:
                print("  进度 %d/%d (%.0fs)" % (i + 1, len(draws), time.time() - t0))
        except Exception as e:
            note_err("draw%d" % getattr(a, "eventId", "?"), e)

    for seg in segments:
        seg.pop("key", None)
    report["draw_segments"] = segments
    print("分段数: %d" % len(segments))
    for s in segments:
        top = sorted(s["ps"].items(), key=lambda kv: -kv[1])
        pss = " ".join("%s*%d" % (k.replace("ResourceId::", ""), v)
                       for k, v in top[:6])
        print("  ev %5d-%5d draws=%4d%s RTs=[%s] DS=%s VP={%s} PS={%s}" %
              (s["start"], s["end"], s["draws"],
               " (Clear后)" if s.get("after_clear") else "",
               " | ".join(s["rt_infos"]) if s["rt_infos"] else "-",
               s["dsv_info"],
               " ".join("%s*%d" % (k, v) for k, v in sorted(s["viewports"].items(), key=lambda kv: -kv[1])),
               pss))
except Exception as e:
    note_err("Draw分段", e)


# ===== 5. Copy / Resolve 资源 =====
try:
    cp = []
    miss = 0
    for a in copies:
        rec = {"eventId": a.eventId, "name": getattr(a, "name", "")}
        src = getattr(a, "copySource", None)
        dst = getattr(a, "copyDestination", None)
        if src is None and dst is None:
            miss += 1
        rec["src"] = str(src) if src is not None else None
        rec["dst"] = str(dst) if dst is not None else None
        rec["src_info"] = tex_info(src) if valid_id(src) else "-"
        rec["dst_info"] = tex_info(dst) if valid_id(dst) else "-"
        cp.append(rec)
    report["copies"] = cp
    if miss:
        note_err("Copy属性", Exception("%d/%d 条取不到 copySource/copyDestination" % (miss, len(cp))))
    for r in cp:
        print("Copy ev%d: %s(%s) -> %s(%s)" %
              (r["eventId"], r["src"], r["src_info"], r["dst"], r["dst_info"]))
except Exception as e:
    note_err("Copy", e)

try:
    rs = []
    for a in resolves:
        rec = {"eventId": a.eventId}
        for attr in ("resolveSource", "resolveDestination", "resolveMip", "resolveSlice"):
            v = getattr(a, attr, None)
            if v is not None:
                rec[attr] = str(v)
        rs.append(rec)
    report["resolves"] = rs
    print("Resolve %d 条" % len(rs))
except Exception as e:
    note_err("Resolve", e)


# ===== 6. 导出 =====
report["elapsed_sec"] = round(time.time() - t0, 1)
try:
    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False, default=str)
    print("已导出:", OUT_JSON)
except Exception as e:
    note_err("写JSON", e)

try:
    ctrl.Shutdown()
    capf.Close()
except Exception:
    pass

print("完成，用时 %.1fs，错误数 %d" % (report["elapsed_sec"], len(report["errors"])))
try:
    sys.stdout.flush()
except Exception:
    pass
# 无头批跑 (--py 方式, rdc_run.ps1 置 RDC_HEADLESS=1) 才退出进程; UI 里 Run 不能退出, 否则关掉整个 qrenderdoc
if os.environ.get("RDC_HEADLESS") == "1":
    sys.exit(0)
