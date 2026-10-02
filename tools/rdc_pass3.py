# rdc_pass3.py — 第3轮提取 (场景见配置区): 识别全部在用计算着色器的用途
#
# 用法: qrenderdoc -> Window -> Python Scripting -> 打开本文件 -> Run (约30秒~1分钟)
#
# 第2轮遗留:
#   a) cs_dispatch_groups 为空 (字段名错, 应为 dispatchDimension)
#   b) 反射全失败 (正确 API: GetShaderEntryPoints -> GetShader)
# 本轮补齐:
#   - 每个在用 CS 的工作组数 dispatchDimension (抽 first/mid/last 各验一次)
#   - 线程组尺寸 refl.dispatchThreadsDimension
#   - 常量缓冲/只读资源/UAV/采样器 绑定名
#   - DXBC 反汇编全文 -> S7-cs-disasm.txt (助手解读)

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
    P1 = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-extract.json" % _PREFIX
    P2 = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-extract-pass2.json" % _PREFIX
    OUT_JSON = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-extract-pass3.json" % _PREFIX
    OUT_DISASM = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-cs-disasm.txt" % _PREFIX
else:
    RDC = r"C:\Users\joker\skyrim-vulkan\captures\S7-heavy-mods.rdc"
    P1 = r"C:\Users\joker\skyrim-vulkan\docs\analysis\S7-extract.json"
    P2 = r"C:\Users\joker\skyrim-vulkan\docs\analysis\S7-extract-pass2.json"
    OUT_JSON = r"C:\Users\joker\skyrim-vulkan\docs\analysis\S7-extract-pass3.json"
    OUT_DISASM = r"C:\Users\joker\skyrim-vulkan\docs\analysis\S7-cs-disasm.txt"

report = {"rdc": RDC, "errors": []}
t0 = time.time()


def note_err(section, e):
    msg = "%s: %s: %s" % (section, type(e).__name__, e)
    report["errors"].append(msg)
    print("[错误]", msg)


# ===== 读第1/2轮结果 =====
try:
    with open(P1, encoding="utf-8") as f:
        p1 = json.load(f)
    with open(P2, encoding="utf-8") as f:
        p2 = json.load(f)
    print("已读 pass1/pass2 JSON")
except Exception as e:
    note_err("读JSON", e)
    raise

compute_keys = p1.get("shader_ids_by_stage", {}).get("compute", [])
positions = p2.get("cs_positions_sampled", {})


def key_to_int(k):
    # "ResourceId::17607:" 或 "ResourceId::17607" -> 17607
    try:
        return int(str(k).split("::")[-1].strip(": "))
    except Exception:
        return None


# ===== 打开抓帧 =====
print("=== 打开 ===")
capf = rd.OpenCaptureFile()
st = capf.OpenFile(RDC, "", None)
print("OpenFile:", repr(str(st)))
if "success" not in str(st).lower():
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
    raise SystemExit(4)


def shader_key(sh):
    if sh is None:
        return None
    res = sh.resource if hasattr(sh, "resource") else sh
    s = str(res)
    if s in ("ResourceId::0", "0", "None", ""):
        return None
    return s


# ===== 动作树: eventId -> ActionDescription (取 dispatch 组数) =====
flat = []


def walk(actions):
    for a in actions:
        flat.append(a)
        try:
            if a.children:
                walk(a.children)
        except Exception:
            pass


try:
    walk(ctrl.GetRootActions())
except Exception as e:
    note_err("动作树", e)

disp_by_eid = {}
for a in flat:
    try:
        if int(a.flags) & int(rd.ActionFlags.Dispatch):
            disp_by_eid[a.eventId] = a
    except Exception:
        pass
print("Dispatch 动作:", len(disp_by_eid))


def vec3(v):
    try:
        return [int(v.x), int(v.y), int(v.z)]
    except Exception:
        pass
    try:
        return [int(x) for x in v]
    except Exception:
        return str(v)


# ===== 1. 在用 CS: 工作组数验证 (每 CS 抽 first/mid/last) =====
groups_by_key = {}
try:
    for key, pos in positions.items():
        if not pos:
            continue
        picks = sorted(set([pos[0], pos[len(pos) // 2], pos[-1]]))
        groups_by_key[key] = []
        for eid in picks:
            ctrl.SetFrameEvent(eid, False)
            ps = ctrl.GetPipelineState()
            cur = shader_key(ps.GetShader(rd.ShaderStage.Compute))
            act = disp_by_eid.get(eid)
            rec = {
                "eventId": eid,
                "key_at_event": cur,
                "dispatchDimension": None,
                "dispatchBase": None,
                "customName": "",
            }
            if act is not None:
                try:
                    rec["dispatchDimension"] = list(act.dispatchDimension)
                except Exception as e:
                    note_err("dispatchDimension@%d" % eid, e)
                try:
                    rec["dispatchBase"] = list(act.dispatchBase)
                except Exception:
                    pass
                try:
                    rec["customName"] = str(act.customName)
                except Exception:
                    pass
            groups_by_key[key].append(rec)
    report["groups_by_key"] = groups_by_key
    for k, v in groups_by_key.items():
        print("组数 %s: %s" % (k, [r["dispatchDimension"] for r in v]))
except Exception as e:
    note_err("工作组数", e)


# ===== 2. 反射 + 反汇编 =====
def extract_reflection(refl, info):
    try:
        info["threadDim"] = vec3(refl.dispatchThreadsDimension)
    except Exception as e:
        note_err("threadDim", e)
    for attr in ("stage", "encoding", "entryPoint"):
        try:
            info[attr] = str(getattr(refl, attr))
        except Exception:
            pass
    try:
        info["bytecodeSize"] = len(refl.rawBytes) if refl.rawBytes else 0
    except Exception:
        pass
    # 常量缓冲
    try:
        for cb in refl.constantBlocks:
            d = {"name": str(getattr(cb, "name", "?")),
                 "byteSize": int(getattr(cb, "byteSize", -1))}
            try:
                d["bindPoint"] = int(cb.bindPoint)
            except Exception:
                pass
            try:
                names = []
                for v in (getattr(cb, "variables", None) or [])[:40]:
                    try:
                        names.append(str(getattr(v, "name", "")))
                    except Exception:
                        pass
                if any(names):
                    d["variables"] = [n for n in names if n]
            except Exception:
                pass
            info.setdefault("cbuffers", []).append(d)
    except Exception as e:
        note_err("cbuffers", e)
    # 只读资源 / UAV / 采样器
    def res_list(lst, limit=40):
        out = []
        for r in (lst or [])[:limit]:
            d = {"name": str(getattr(r, "name", "?"))}
            for f in ("bindPoint", "bindArraySize"):
                try:
                    d[f] = int(getattr(r, f))
                except Exception:
                    pass
            out.append(d)
        return out

    try:
        info["readOnly"] = res_list(refl.readOnlyResources)
    except Exception as e:
        note_err("readOnly", e)
    try:
        info["readWrite"] = res_list(refl.readWriteResources)
    except Exception as e:
        note_err("readWrite", e)
    try:
        info["samplers"] = [str(getattr(s, "name", s)) for s in (refl.samplers or [])][:40]
    except Exception:
        pass
    # 调试信息(如有源文件名则是直接证据)
    try:
        di = refl.debugInfo
        if di is not None:
            for attr in ("files", "sourceFile"):
                v = getattr(di, attr, None)
                if v:
                    info["debugInfo"] = str(v)
                    break
    except Exception:
        pass
    return info


def get_reflection(res):
    entries = []
    try:
        entries = list(ctrl.GetShaderEntryPoints(res))
    except Exception as e:
        note_err("GetShaderEntryPoints", e)
    attempts = []
    if entries:
        attempts.append(lambda: ctrl.GetShader(rd.ResourceId(), res, entries[0]))
    attempts.append(lambda: ctrl.GetShader(rd.ResourceId(), res, None))
    attempts.append(lambda: ctrl.GetShader(rd.ResourceId(), res, ""))
    last = None
    for fn in attempts:
        try:
            r = fn()
            if r is not None:
                return r
        except Exception as e:
            last = e
    if last is not None:
        note_err("GetShader", last)
    return None


# 收集要反射的 shader ResourceId: 4 个在用的从管线状态取
used_events = {}
for key, recs in groups_by_key.items():
    if recs:
        used_events[key] = recs[0]["eventId"]

refl_results = {}
disasm_parts = []

try:
    for key, eid in used_events.items():
        ctrl.SetFrameEvent(eid, False)
        ps = ctrl.GetPipelineState()
        sh = ps.GetShader(rd.ShaderStage.Compute)
        res = sh.resource if hasattr(sh, "resource") else sh
        refl = get_reflection(res)
        if refl is None:
            refl_results[key] = {"key": key, "note": "反射失败"}
            continue
        info = extract_reflection(refl, {"key": key, "boundAtEvent": eid})
        # 反汇编
        try:
            dis = ctrl.DisassembleShader(rd.ResourceId(), refl, "")
            info["disasmLines"] = dis.count("\n") + 1 if dis else 0
            disasm_parts.append("=== %s (bound at event %d) threadDim=%s groups=%s ===\n%s\n" % (
                key, eid, info.get("threadDim"),
                [r["dispatchDimension"] for r in groups_by_key.get(key, [])],
                dis or "<empty>"))
        except Exception as e:
            note_err("DisassembleShader %s" % key, e)
            disasm_parts.append("=== %s === <反汇编失败: %s>\n" % (key, e))
        refl_results[key] = info
        print("反射 %s: threadDim=%s cb=%d ro=%d rw=%d" % (
            key, info.get("threadDim"), len(info.get("cbuffers", [])),
            len(info.get("readOnly", [])), len(info.get("readWrite", []))))
except Exception as e:
    note_err("反射主循环", e)

# 尝试反射本帧未用的 CS (需要 ResourceId(int) 构造, 失败就跳过)
unused_tried = []
for k in compute_keys:
    ki = key_to_int(k)
    if ki is None:
        continue
    tag = "ResourceId::%d" % ki
    if tag in refl_results or tag in used_events:
        continue
    try:
        rid = rd.ResourceId(ki)
    except Exception:
        unused_tried.append("%s(构造失败)" % tag)
        continue
    refl = get_reflection(rid)
    if refl is None:
        unused_tried.append("%s(反射失败)" % tag)
        continue
    info = extract_reflection(refl, {"key": tag, "usedThisFrame": False})
    refl_results[tag] = info
    try:
        dis = ctrl.DisassembleShader(rd.ResourceId(), refl, "")
        disasm_parts.append("=== %s (本帧未用) threadDim=%s ===\n%s\n" % (
            tag, info.get("threadDim"), dis or "<empty>"))
    except Exception as e:
        note_err("DisassembleShader %s" % tag, e)
report["unused_cs_attempts"] = unused_tried
report["reflections"] = refl_results

# ===== 3. 导出 =====
try:
    with open(OUT_DISASM, "w", encoding="utf-8") as f:
        f.write("\n".join(disasm_parts))
    print("已导出反汇编:", OUT_DISASM)
except Exception as e:
    note_err("写反汇编", e)

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

# 无头批跑 (--py 方式, rdc_run.ps1 置 RDC_HEADLESS=1) 才退出进程; UI 里 Run 不能退出
if os.environ.get("RDC_HEADLESS") == "1":
    sys.exit(0)
