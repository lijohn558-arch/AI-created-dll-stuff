# rdc_extract.py — 从 RenderDoc 抓帧中提取结构化分析数据
#
# 用法（qrenderdoc 内，注意菜单项叫 "Python Scripting"）:
#   1. 不需要在 UI 里预先打开抓帧（若开着请先 File -> Close Capture，避免文件锁）
#   2. Window -> Python Scripting -> 打开本文件 -> Run
#   3. 输出: JSON 报告写入 docs/analysis/<场景>-extract.json
#
# 说明: 各小节独立 try/except —— 某段 API 对不上时错误记入 errors，
#       已完成的数据照样导出，便于迭代修复脚本。

import renderdoc as rd
import json
import os
import sys
import time

# ===== 配置 =====
# 与 pass5/pass6 同范式: RDC_SCENE 环境变量选场景 (rdc_run.ps1 -Scene 传入);
# 不设则回落 S7 占位路径 (S7 场景不存在, 直接手跑会打不开文件)
_SCENES = {"S1": "S1-city-day", "S2": "S2-interior", "S3": "S3-forest-godrays",
           "S4": "S4-water", "S5": "S5-night-combat"}
_RDC_SCENE = os.environ.get("RDC_SCENE", "").strip()
if _RDC_SCENE:
    _FULL = _SCENES.get(_RDC_SCENE, _RDC_SCENE)
    _PREFIX = _FULL.split("-")[0]
    RDC = r"C:\Users\joker\skyrim-vulkan\captures\%s.rdc" % _FULL
    OUT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-extract.json" % _PREFIX
else:
    RDC = r"C:\Users\joker\skyrim-vulkan\captures\S7-heavy-mods.rdc"
    OUT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\S7-extract.json"

report = {"rdc": RDC, "errors": []}
t0 = time.time()


def note_err(section, e):
    msg = "%s: %s: %s" % (section, type(e).__name__, e)
    report["errors"].append(msg)
    print("[错误]", msg)


def enum_int(name):
    """按名字取 ActionFlags 位，取不到返回 None（兼容不同版本的标志名差异）"""
    f = getattr(rd.ActionFlags, name, None)
    return int(f) if f is not None else None


# ===== 1. 打开抓帧（自诊断版）=====
print("=== 打开诊断 ===")
print("路径:", RDC)
try:
    print("文件存在:", os.path.isfile(RDC), "| 大小: %.1f MB" % (os.path.getsize(RDC) / 1048576.0))
except Exception as e:
    print("读取文件信息失败:", e)

capf = rd.OpenCaptureFile()
st = None
for ud in ("", b""):
    try:
        st = capf.OpenFile(RDC, ud, None)
        print("OpenFile(userData=%r) 返回: %r (type=%s)" % (ud, st, type(st).__name__))
        break
    except Exception as e:
        print("OpenFile(userData=%r) 抛异常: %s: %s" % (ud, type(e).__name__, e))
        st = None
if st is None:
    print(">>> OpenFile 所有尝试都抛异常 —— 把本段输出发助手")
    raise SystemExit(2)

s_str = str(st)
print("状态字符串:", repr(s_str))
ok = None
low = s_str.lower()
# RenderDoc 1.46 返回 Result 类型: 成功形如 <Result: 'Success'>
if "success" in low:
    ok = True
elif any(x in low for x in ("fail", "error", "unrecognised", "unsupported", "corrupted",
                            "notfound", "access", "denied", "sharing", "ioerror", "fileio")):
    ok = False
else:
    try:
        ok = (int(st) == 0)
    except Exception:
        ok = None
print("判定 ok =", ok)

if ok is False:
    print(">>> 打开失败。常见原因: UI 里还开着同一个抓帧（File -> Close Capture 后重试）。")
    print(">>> 请把本段全部输出发助手。")
    raise SystemExit(3)

ctrl = None
try:
    res = capf.OpenCapture(rd.ReplayOptions(), None)
    if isinstance(res, tuple) and len(res) == 2:
        rs, ctrl = res
        print("Replay:", rs)
    else:
        ctrl = res
except Exception as e:
    note_err("OpenCapture", e)
if ctrl is None:
    print(">>> 无法创建 replay controller —— 把 Replay: 行和错误信息发助手")
    raise SystemExit(4)


def valid_id(rid):
    try:
        return rid is not None and int(rid) >= 0
    except Exception:
        s = str(rid)
        return s not in ("", "None", "<>")


def fmt_of(rid):
    """ResourceId -> 格式名"""
    try:
        tex = ctrl.GetTexture(rid)
        f = tex.format
        for attr in ("name", "Name"):
            v = getattr(f, attr, None)
            if callable(v):
                v = v()
            if v:
                return str(v)
        return str(f)
    except Exception:
        return None


# ===== 2. 动作树统计 =====
try:
    flag_names = ["Drawcall", "Dispatch", "Clear", "Copy", "Resolve", "Present",
                  "PassBoundary", "PushMarker", "SetMarker", "BeginRenderPass",
                  "EndRenderPass", "MultiDraw", "Indirect"]
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

    counts = {n: 0 for n in flags}
    draws, dispatches = [], []
    for a in flat:
        try:
            af = int(a.flags)
        except Exception:
            af = 0
        for n, bit in flags.items():
            if af & bit:
                counts[n] += 1
                if n == "Drawcall":
                    draws.append(a)
                elif n == "Dispatch":
                    dispatches.append(a)

    report["actions_total"] = len(flat)
    report["flag_counts"] = counts
    report["draws"] = len(draws)
    report["dispatches"] = len(dispatches)
    eids = [a.eventId for a in flat]
    report["event_id_range"] = [min(eids), max(eids)] if eids else []
    print("动作总数 %d | Draw %d | Dispatch %d | Present %d" %
          (len(flat), len(draws), len(dispatches), counts.get("Present", 0)))
except Exception as e:
    note_err("动作树统计", e)


# ===== 3. 资源统计 =====
try:
    texs = ctrl.GetTextures()
    report["textures_total"] = len(texs)
    fmts = set()
    for t in texs:
        f = getattr(t, "format", None)
        if f is None:
            continue
        for attr in ("name", "Name"):
            v = getattr(f, attr, None)
            if callable(v):
                v = v()
            if v:
                fmts.add(str(v))
                break
        else:
            fmts.add(str(f))
    report["texture_formats"] = sorted(fmts)
    print("纹理 %d 个 | 格式 %d 种" % (len(texs), len(fmts)))
except Exception as e:
    note_err("资源统计", e)


# ===== 4. 逐 Draw/Dispatch 提取管线状态（着色器去重 / 近似PSO / RT格式）=====
try:
    stages = [("vertex", rd.ShaderStage.Vertex), ("pixel", rd.ShaderStage.Pixel),
              ("geometry", rd.ShaderStage.Geometry), ("hull", rd.ShaderStage.Hull),
              ("domain", rd.ShaderStage.Domain), ("compute", rd.ShaderStage.Compute)]

    shaders_by_stage = {}          # stage -> set("resource:entry")
    shader_sets = set()            # 近似PSO: 一套着色器+RT组合
    pso_keys = set()
    rt_samples = []                # 前几个 draw 的 RT 描述
    rt_formats = set()
    state_errors = 0

    targets = [(a, "draw") for a in draws] + [(a, "disp") for a in dispatches]
    total = len(targets)
    print("开始逐事件提取 %d 个事件的管线状态..." % total)

    for i, (a, kind) in enumerate(targets):
        try:
            ctrl.SetFrameEvent(a.eventId, False)
            ps = ctrl.GetPipelineState()

            cur = set()
            for sname, sval in stages:
                try:
                    s = ps.GetShader(sval)
                except Exception:
                    continue
                if s is None:
                    continue
                if hasattr(s, "resource"):
                    res, entry = s.resource, getattr(s, "entryPoint", "")
                else:
                    res, entry = s, ""
                if not valid_id(res):
                    continue
                key = "%s:%s" % (res, entry)
                shaders_by_stage.setdefault(sname, set()).add(key)
                cur.add((sname, key))

            # 输出目标（取前 8 个 draw 做样本 + 去重 RT 资源）
            if kind == "draw":
                rts = []
                try:
                    rts = list(ps.GetOutputTargets())
                except Exception:
                    pass
                try:
                    d = ps.GetDepthTarget()
                    if d is not None:
                        rts.append(d)
                except Exception:
                    pass
                rt_key = []
                for t in rts:
                    rid = getattr(t, "resource", None)
                    if rid is None or not valid_id(rid):
                        continue
                    rt_key.append(str(rid))
                    fm = fmt_of(rid)
                    if fm:
                        rt_formats.add(fm)
                pso_keys.add((frozenset(cur), frozenset(rt_key)))
                if len(rt_samples) < 8 and rt_key:
                    rt_samples.append({
                        "eventId": a.eventId,
                        "name": getattr(a, "name", ""),
                        "rt": ["%s(%s)" % (r, fmt_of(r) or "?") for r in rt_key],
                    })

            if (i + 1) % 500 == 0:
                print("  进度 %d/%d (%.0fs)" % (i + 1, total, time.time() - t0))
        except Exception as e:
            state_errors += 1
            if state_errors <= 5:
                note_err("事件%d" % getattr(a, "eventId", "?"), e)

    report["unique_shaders_by_stage"] = {k: len(v) for k, v in shaders_by_stage.items()}
    report["shader_ids_by_stage"] = {k: sorted(v) for k, v in shaders_by_stage.items()}
    report["approx_pso_count"] = len(pso_keys)
    report["rt_formats"] = sorted(rt_formats)
    report["rt_samples"] = rt_samples
    report["state_errors"] = state_errors
    print("着色器(去重): %s" % {k: len(v) for k, v in shaders_by_stage.items()})
    print("近似PSO数: %d | RT格式: %s" % (len(pso_keys), sorted(rt_formats)))
except Exception as e:
    note_err("管线状态提取", e)


# ===== 5. 导出 =====
report["elapsed_sec"] = round(time.time() - t0, 1)
try:
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False, default=str)
    print("已导出:", OUT)
except Exception as e:
    note_err("写JSON", e)

try:
    ctrl.Shutdown()
    capf.Close()
except Exception:
    pass

print("完成，用时 %.1fs，错误数 %d" % (report["elapsed_sec"], len(report["errors"])))

# 无头批跑 (--py 方式, rdc_run.ps1 置 RDC_HEADLESS=1) 才退出进程; UI 里 Run 不能退出
if os.environ.get("RDC_HEADLESS") == "1":
    sys.exit(0)
