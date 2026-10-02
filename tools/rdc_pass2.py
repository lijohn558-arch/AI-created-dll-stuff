# rdc_pass2.py — 第2轮提取 (场景见配置区): Dispatch 抽样 + RT 格式修复 + CS 反射
#
# 用法: qrenderdoc -> Window -> Python Scripting -> 打开本文件 -> Run
#
# 第1轮遗留问题:
#   a) 8198 个 Dispatch 占全帧 66% 却只有 13 个 CS —— 本轮定位其内容:
#      每个 CS 的抽样计数 / 帧内事件位置 / Dispatch 组数(dispatchX/Y/Z) / 线程组尺寸
#   b) RT 资源格式解析失败 —— 本轮改用 GetTextures() 建立 id->格式 映射表

import renderdoc as rd
import json
import os
import sys
import time

# ===== 配置 =====
# 与 pass5/pass6 同范式: RDC_SCENE 环境变量选场景 (rdc_run.ps1 -Scene 传入); 不设则回落 S7 占位
_SCENES = {"S1": "S1-city-day", "S2": "S2-interior", "S3": "S3-forest-godrays",
           "S4": "S4-water", "S5": "S5-night-combat"}
_RDC_SCENE = os.environ.get("RDC_SCENE", "").strip()
if _RDC_SCENE:
    _FULL = _SCENES.get(_RDC_SCENE, _RDC_SCENE)
    _PREFIX = _FULL.split("-")[0]
    RDC = r"C:\Users\joker\skyrim-vulkan\captures\%s.rdc" % _FULL
    OUT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-extract-pass2.json" % _PREFIX
else:
    RDC = r"C:\Users\joker\skyrim-vulkan\captures\S7-heavy-mods.rdc"
    OUT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\S7-extract-pass2.json"
STRIDE = 10  # 抽样步长: 1=全量(慢), 10=每10个取1(约1-3分钟)

report = {"rdc": RDC, "stride": STRIDE, "errors": []}
t0 = time.time()


def note_err(section, e):
    msg = "%s: %s: %s" % (section, type(e).__name__, e)
    report["errors"].append(msg)
    print("[错误]", msg)


# ===== 1. 打开抓帧 =====
print("=== 打开 ===")
capf = rd.OpenCaptureFile()
st = capf.OpenFile(RDC, "", None)
print("OpenFile:", repr(str(st)))
if "success" not in str(st).lower():
    print(">>> 打开失败，把本行输出发助手")
    raise SystemExit(2)

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
    print(">>> 无法创建 replay controller")
    raise SystemExit(4)


def fmtname(f):
    for attr in ("name", "Name"):
        v = getattr(f, attr, None)
        if callable(v):
            v = v()
        if v:
            return str(v)
    return str(f)


# ===== 2. 纹理 id -> 格式 映射（修复RT格式提取）=====
fmt_by_id = {}
try:
    for t in ctrl.GetTextures():
        f = getattr(t, "format", None)
        fmt_by_id[str(t.resourceId)] = fmtname(f) if f is not None else "?"
    print("纹理映射条目:", len(fmt_by_id))
except Exception as e:
    note_err("纹理映射", e)


# ===== 3. 动作树 =====
try:
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
    draws, dispatches = [], []
    for a in flat:
        try:
            af = int(a.flags)
        except Exception:
            af = 0
        if af & int(rd.ActionFlags.Drawcall):
            draws.append(a)
        elif af & int(rd.ActionFlags.Dispatch):
            dispatches.append(a)
    print("Draw %d | Dispatch %d" % (len(draws), len(dispatches)))
except Exception as e:
    note_err("动作树", e)
    raise


def shader_key(sh):
    if sh is None:
        return None
    if hasattr(sh, "resource"):
        res, entry = sh.resource, getattr(sh, "entryPoint", "")
    else:
        res, entry = sh, ""
    s = str(res)
    if s in ("ResourceId::0", "0", "None", ""):
        return None
    return s + (":" + str(entry) if entry else "")


# ===== 4. CS 反射（线程组尺寸/CB/资源名 —— 用于识别CS用途）=====
def reflect_shader(key, sh):
    res = sh.resource if hasattr(sh, "resource") else sh
    refl = None
    attempts = [
        lambda: ctrl.GetShaderReflection(res),
        lambda: ctrl.GetShaderReflection(res, ""),
        lambda: sh.reflection,
    ]
    for fn in attempts:
        try:
            refl = fn()
            if refl is not None:
                break
        except Exception:
            pass
    if refl is None:
        return {"key": key, "note": "反射获取失败", "dir": [d for d in dir(refl) if not d.startswith('_')] if refl else None}

    info = {"key": key}
    # 线程组尺寸
    for attr in ("threadDimension", "numThreads", "workDim", "dispatchDimension"):
        v = getattr(refl, attr, None)
        if v is not None:
            try:
                info[attr] = [v.x, v.y, v.z]
            except Exception:
                try:
                    info[attr] = list(v)
                except Exception:
                    info[attr] = str(v)
            break
    # 常量缓冲
    try:
        cbs = getattr(refl, "constantBlocks", None)
        if cbs is None:
            cbs = getattr(refl, "cbuffers", None)
        if cbs:
            info["cbuffers"] = ["%s(%dB)" % (getattr(c, "name", "?"), getattr(c, "byteSize", -1)) for c in cbs]
    except Exception as e:
        note_err("反射cbuffers", e)
    # 绑定资源名（纹理/采样器/UAV 名）
    try:
        res_list = getattr(refl, "resources", None)
        if res_list:
            info["resources"] = [str(getattr(r, "name", r)) for r in res_list[:20]]
    except Exception as e:
        note_err("反射resources", e)
    # 首次失败时 dump dir 帮助修脚本
    if not any(k in info for k in ("threadDimension", "numThreads", "workDim", "dispatchDimension")):
        info["refl_dir"] = [d for d in dir(refl) if not d.startswith("_")][:60]
    return info


# ===== 5. Dispatch 分析（stride 抽样）=====
cs_counts = {}            # key -> 抽样命中次数
cs_positions = {}         # key -> [eventId...]
cs_dispatch_groups = {}   # key -> [(x,y,z)...] 去重后的组尺寸
cs_reflections = {}       # key -> 反射信息（每CS一次）
try:
    sample = dispatches[::STRIDE]
    print("Dispatch 抽样 %d/%d 个..." % (len(sample), len(dispatches)))
    for i, a in enumerate(sample):
        try:
            ctrl.SetFrameEvent(a.eventId, False)
            ps = ctrl.GetPipelineState()
            key = shader_key(ps.GetShader(rd.ShaderStage.Compute))
            if key is None:
                key = "<unbound>"
            cs_counts[key] = cs_counts.get(key, 0) + 1
            cs_positions.setdefault(key, []).append(a.eventId)
            # 组数 (Dispatch(x,y,z))
            try:
                g = (int(a.dispatchX), int(a.dispatchY), int(a.dispatchZ))
                if g not in cs_dispatch_groups.setdefault(key, []):
                    cs_dispatch_groups[key].append(g)
            except Exception:
                pass
            # 反射（每 key 只做一次）
            if key not in cs_reflections:
                try:
                    sh = ps.GetShader(rd.ShaderStage.Compute)
                    cs_reflections[key] = reflect_shader(key, sh)
                except Exception as e:
                    cs_reflections[key] = {"key": key, "err": str(e)}
            if (i + 1) % 100 == 0:
                print("  进度 %d/%d (%.0fs)" % (i + 1, len(sample), time.time() - t0))
        except Exception as e:
            note_err("dispatch事件%d" % getattr(a, "eventId", "?"), e)

    report["dispatch_sampled"] = len(sample)
    report["cs_counts_sampled"] = cs_counts
    report["cs_counts_extrapolated"] = {k: v * STRIDE for k, v in cs_counts.items()}
    report["cs_positions_sampled"] = cs_positions
    report["cs_dispatch_groups"] = cs_dispatch_groups
    report["cs_reflections"] = cs_reflections
    print("Dispatch 抽样完成: %s" % cs_counts)
except Exception as e:
    note_err("Dispatch分析", e)


# ===== 6. Draw 抽样 -> RT/深度 格式（走映射表）=====
try:
    sample = draws[::STRIDE]
    print("Draw 抽样 %d/%d 个..." % (len(sample), len(draws)))
    out_fmts, depth_fmts, rt_combos = {}, {}, {}
    for i, a in enumerate(sample):
        try:
            ctrl.SetFrameEvent(a.eventId, False)
            ps = ctrl.GetPipelineState()
            cur = []
            try:
                for t in ps.GetOutputTargets():
                    rid = getattr(t, "resource", None)
                    if rid is None:
                        continue
                    s = str(rid)
                    if s in ("ResourceId::0", "0"):
                        continue
                    fm = fmt_by_id.get(s, "未知(%s)" % s)
                    cur.append(fm)
                    out_fmts[fm] = out_fmts.get(fm, 0) + 1
            except Exception:
                pass
            try:
                d = ps.GetDepthTarget()
                if d is not None:
                    rid = getattr(d, "resource", None)
                    s = str(rid)
                    if rid is not None and s not in ("ResourceId::0", "0"):
                        fm = fmt_by_id.get(s, "未知(%s)" % s)
                        depth_fmts[fm] = depth_fmts.get(fm, 0) + 1
                        cur.append("D:" + fm)
            except Exception:
                pass
            if cur:
                ck = " + ".join(sorted(set(cur)))
                rt_combos[ck] = rt_combos.get(ck, 0) + 1
            if (i + 1) % 100 == 0:
                print("  进度 %d/%d (%.0fs)" % (i + 1, len(sample), time.time() - t0))
        except Exception as e:
            note_err("draw事件%d" % getattr(a, "eventId", "?"), e)

    report["draw_sampled"] = len(sample)
    report["output_formats"] = out_fmts
    report["depth_formats"] = depth_fmts
    report["rt_combos"] = rt_combos
    print("输出格式: %s" % out_fmts)
    print("深度格式: %s" % depth_fmts)
    print("RT组合(去重): %d 种" % len(rt_combos))
except Exception as e:
    note_err("Draw/RT分析", e)


# ===== 7. 导出 =====
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
