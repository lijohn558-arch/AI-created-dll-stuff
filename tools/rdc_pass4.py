# rdc_pass4.py — 第4轮: 全量 Dispatch CB (float+uint 双解析) + 全部在用 CS 反射/资源/反汇编（场景见配置区）
#
# 用法: qrenderdoc -> Window -> Python Scripting -> 打开本文件 -> Run (约10秒; UI 里不要开着抓帧)
#
# 从 S1 第4轮 v2 继承的修复:
#   1) TextureDescription / BufferDescription 没有 .id 属性 -> 属性名自动发现, 失败时打印 dir()
#   2) CB 只按 float 解析会漏 uint 位型字段 -> 同时解析 uint32 并按 uint 精确 diff
#   3) 资源索引取值笔误 (rid, _ = pick_attr) -> 已改 (_, rid = ...)
# 本轮产出:
#   a) 全部在用 CS 的线程组尺寸 + 反射 + 反汇编 (不按 CS ID 白名单过滤) -> S7-post-cs-disasm.txt
#   b) 全部绑定资源尺寸/格式/customName
#   c) uint CB diff -> 验证 CB 是否按事件返回内容 + 变化字段序列
#
# 输出: S7-extract-pass4.json + S7-post-cs-disasm.txt + 控制台摘要

import renderdoc as rd
import json
import time
import struct
import os
import sys

# ===== 配置 =====
# 与 rdc_pass5/pass6 同范式: RDC_SCENE 环境变量选场景 (rdc_run.ps1 -Scene 传入);
# 不设则回落到 S7 占位路径 (S7 场景不存在, 直接手跑会打不开文件)
_SCENES = {"S1": "S1-city-day", "S2": "S2-interior", "S3": "S3-forest-godrays",
           "S4": "S4-water", "S5": "S5-night-combat"}
_RDC_SCENE = os.environ.get("RDC_SCENE", "").strip()
if _RDC_SCENE:
    _FULL = _SCENES.get(_RDC_SCENE, _RDC_SCENE)
    _PREFIX = _FULL.split("-")[0]
    RDC = r"C:\Users\joker\skyrim-vulkan\captures\%s.rdc" % _FULL
    OUT_JSON = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-extract-pass4.json" % _PREFIX
    OUT_DISASM = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-post-cs-disasm.txt" % _PREFIX
else:
    RDC = r"C:\Users\joker\skyrim-vulkan\captures\S7-heavy-mods.rdc"
    OUT_JSON = r"C:\Users\joker\skyrim-vulkan\docs\analysis\S7-extract-pass4.json"
    OUT_DISASM = r"C:\Users\joker\skyrim-vulkan\docs\analysis\S7-post-cs-disasm.txt"

report = {"rdc": RDC, "version": "v2", "errors": []}
t0 = time.time()


def note_err(section, e):
    msg = "%s: %s: %s" % (section, type(e).__name__, e)
    report["errors"].append(msg)
    print("[错误]", msg)


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


def pick_attr(obj, cands):
    """按候选名依次取非 None 且非方法的属性 -> (属性名, 值)"""
    for a in cands:
        v = getattr(obj, a, None)
        if v is not None and not callable(v):
            return a, v
    return None, None


def fmt_name(f):
    if f is None:
        return None
    for attr in ("name", "Name"):
        v = getattr(f, attr, None)
        if callable(v):
            v = v()
        if v:
            return str(v)
    return str(f)


RID_CANDS = ("resourceId", "id", "resource", "rid")

# ===== 资源索引 (v1 错误修复: 属性名自动发现) =====
discovery = {}
tex_map = {}
try:
    texs = ctrl.GetTextures()
    if texs:
        attr, _ = pick_attr(texs[0], RID_CANDS)
        if attr is None:
            discovery["texture_dir"] = [x for x in dir(texs[0])
                                        if not x.startswith("_")]
            print("!! TextureDescription id 属性未识别, dir =", discovery["texture_dir"])
        else:
            print("TextureDescription id 属性 =", attr)
    for t in texs:
        _, rid = pick_attr(t, RID_CANDS)
        if rid is None:
            continue
        tex_map[str(rid)] = {
            "type": "texture",
            "w": int(getattr(t, "width", 0)),
            "h": int(getattr(t, "height", 0)),
            "depth": int(getattr(t, "depth", 0)),
            "arraysize": int(getattr(t, "arraySize", 0)),
            "mips": int(getattr(t, "mips", 0)),
            "dimension": str(getattr(t, "dimension", "")),
            "format": fmt_name(getattr(t, "format", None)),
            "customName": str(getattr(t, "customName", "") or ""),
        }
    print("纹理索引: %d" % len(tex_map))
except Exception as e:
    note_err("GetTextures", e)

buf_map = {}
try:
    bufs = ctrl.GetBuffers()
    if bufs:
        attr, _ = pick_attr(bufs[0], RID_CANDS)
        if attr is None:
            discovery["buffer_dir"] = [x for x in dir(bufs[0])
                                       if not x.startswith("_")]
            print("!! BufferDescription id 属性未识别, dir =", discovery["buffer_dir"])
        else:
            print("BufferDescription id 属性 =", attr)
    for b in bufs:
        _, rid = pick_attr(b, RID_CANDS)
        if rid is None:
            continue
        _, blen = pick_attr(b, ("byteLength", "length", "byteSize", "size"))
        buf_map[str(rid)] = {
            "type": "buffer",
            "bytes": int(blen) if blen is not None else None,
            "customName": str(getattr(b, "customName", "") or ""),
        }
    print("缓冲索引: %d" % len(buf_map))
except Exception as e:
    note_err("GetBuffers", e)
report["attr_discovery"] = discovery


def res_info(rid):
    s = str(rid)
    if s in tex_map:
        return dict(tex_map[s])
    if s in buf_map:
        return dict(buf_map[s])
    return {"type": "unknown", "resource": s}


# ===== 动作树: dispatch 全表 =====
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

dispatches = []
for a in flat:
    try:
        if int(a.flags) & int(rd.ActionFlags.Dispatch):
            dispatches.append(a)
    except Exception:
        pass
dispatches.sort(key=lambda a: a.eventId)
print("Dispatch 动作:", len(dispatches))

FLAG_NAMES = ["Drawcall", "Dispatch", "Clear", "Copy", "Resolve", "Present",
              "PushMarker", "SetMarker", "BeginRenderPass", "EndRenderPass",
              "PassBoundary", "MultiDraw", "Indirect"]
FLAG_LIST = []
for _n in FLAG_NAMES:
    try:
        FLAG_LIST.append((_n, int(getattr(rd.ActionFlags, _n))))
    except Exception:
        pass


def flag_str(a):
    try:
        f = int(a.flags)
    except Exception:
        return "?"
    return "|".join(n for n, bit in FLAG_LIST if f & bit) or "-"


# ===== 全树命名标记 (引擎 pass 名, 如有则是实锤) =====
try:
    named = []
    for a in flat:
        nm = str(getattr(a, "customName", "") or "").strip()
        if nm:
            named.append({"eventId": a.eventId, "name": nm,
                          "flags": flag_str(a)})
    report["named_actions"] = named
    print("命名动作: %d 个" % len(named))
    for rec in named[:80]:
        print("  [%d] %s (%s)" % (rec["eventId"], rec["name"], rec["flags"]))
except Exception as e:
    note_err("命名动作", e)

# ===== dispatch 前后各 3 个事件的上下文 (只入 JSON, 控制台不打) =====
try:
    idx_by_eid = {}
    for i, a in enumerate(flat):
        idx_by_eid[a.eventId] = i
    ctx = {}
    for a in dispatches:
        i = idx_by_eid.get(a.eventId)
        if i is None:
            continue
        near = []
        for j in range(max(0, i - 3), min(len(flat), i + 4)):
            b = flat[j]
            near.append({"eventId": b.eventId, "flags": flag_str(b),
                         "name": str(getattr(b, "customName", "") or "")})
        ctx[str(a.eventId)] = near
    report["dispatch_context"] = ctx
    print("dispatch 上下文: %d 条 (仅 JSON)" % len(ctx))
except Exception as e:
    note_err("dispatch上下文", e)


def vec3(v):
    try:
        return [int(v.x), int(v.y), int(v.z)]
    except Exception:
        pass
    try:
        return [int(x) for x in v]
    except Exception:
        return str(v)


def prod3(v):
    try:
        return int(v[0]) * int(v[1]) * int(v[2])
    except Exception:
        return 0


def desc_field(d, *path):
    """UsedDescriptor -> 字段值 (兼容 descriptor.resource / resource 等不同版本路径)"""
    cur = d
    for p in path:
        if not hasattr(cur, p):
            return None
        cur = getattr(cur, p)
    return cur


# cb 声明尺寸兜底: 描述符 byteSize 缺失时默认请求 65536 (D3D11 CB 上限; GetBufferData 自动截到实际大小)
CB_SIZE_FALLBACK = {}

# ===== 逐 dispatch: CB0 (float + uint 双解析) =====
cb_records = []
first_eid_by_key = {}
for a in dispatches:
    eid = a.eventId
    try:
        ctrl.SetFrameEvent(eid, False)
        ps = ctrl.GetPipelineState()
        key = shader_key(ps.GetShader(rd.ShaderStage.Compute))
        rec = {"eventId": eid, "cs": key,
               "groups": vec3(a.dispatchDimension), "base": None}
        try:
            rec["base"] = vec3(a.dispatchBase)
        except Exception:
            pass
        if key is not None and key not in first_eid_by_key:
            first_eid_by_key[key] = eid
        try:
            cbs = ps.GetConstantBlocks(rd.ShaderStage.Compute) or []
        except Exception as e:
            note_err("GetConstantBlocks@%d" % eid, e)
            cbs = []
        rec["cbCount"] = len(cbs)
        if cbs:
            d0 = cbs[0]
            r = (desc_field(d0, "descriptor", "resource")
                 or desc_field(d0, "resource"))
            size = (desc_field(d0, "descriptor", "byteSize")
                    or desc_field(d0, "byteSize"))
            if not size:
                size = CB_SIZE_FALLBACK.get(key, 65536)
            rec["cbBytes"] = int(size)
            if r is not None and str(r) not in ("ResourceId::0", "0", "None"):
                try:
                    data = ctrl.GetBufferData(r, 0, size)
                    n = len(data) // 4
                    raw = data[:n * 4]
                    rec["cb0f"] = [round(f, 6)
                                   for f in struct.unpack("<%df" % n, raw)]
                    rec["cb0u"] = list(struct.unpack("<%dI" % n, raw))
                    rec["cbActualBytes"] = len(data)
                except Exception as e:
                    note_err("GetBufferData@%d" % eid, e)
        cb_records.append(rec)
    except Exception as e:
        note_err("dispatch#%d" % eid, e)

report["dispatches"] = cb_records
print("CB 提取: %d/%d" % (sum(1 for r in cb_records if r.get("cb0u")),
                        len(cb_records)))

# ===== CB 按 CS 分组: uint 精确 diff + float 摘要 =====
by_key = {}
for r in cb_records:
    by_key.setdefault(r["cs"], []).append(r)

cb_diff = {}
for key, recs in sorted(by_key.items(), key=lambda kv: str(kv[0])):
    withcb = [r for r in recs if r.get("cb0u")]
    if not withcb:
        continue
    n = min(len(r["cb0u"]) for r in withcb)
    varying = [i for i in range(n)
               if len(set(r["cb0u"][i] for r in withcb)) > 1]
    headf = withcb[0].get("cb0f", [])[:24]
    tailf = withcb[0].get("cb0f", [])[80:96]
    cb_diff[key] = {
        "dispatchCount": len(recs),
        "cbBytes": withcb[0].get("cbBytes"),
        "varyingUints": varying,
        "cb0First24f": headf,
        "cb0_80_95f": tailf,
    }
    print("%s: dispatch %d 次, cb=%sB, uint 变化下标 %s"
          % (key, len(recs), withcb[0].get("cbBytes"), varying))
    print("  vec4[0..5] =", headf)
    print("  vec4[20..23] =", tailf)
    for i in varying:
        seq = [r["cb0u"][i] for r in withcb]
        if len(seq) <= 20:
            print("  u32[%d] 按事件: %s" % (i, seq))
        else:
            mono = all(seq[k + 1] >= seq[k] for k in range(len(seq) - 1))
            print("  u32[%d] 序列(%d 次): 头10=%s 尾5=%s 单调不减=%s"
                  % (i, len(seq), seq[:10], seq[-5:], mono))
    if len(withcb) > 20 and not varying:
        print("!! %s 的 CB 在 %d 次 dispatch 间无任何位变化 -> 疑 GetBufferData 返回终态快照而非按事件内容"
              % (key, len(withcb)))

report["cb_diff_by_key"] = cb_diff

# ===== 反射 (全部 9 个在用 CS) =====
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
    try:
        for cb in refl.constantBlocks:
            d = {"name": str(getattr(cb, "name", "?")),
                 "byteSize": int(getattr(cb, "byteSize", -1))}
            info.setdefault("cbuffers", []).append(d)
    except Exception as e:
        note_err("cbuffers", e)

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
        info["samplers"] = [str(getattr(s, "name", s))
                            for s in (refl.samplers or [])][:40]
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


# 不按 CS ID 白名单过滤, 所有在用 CS 都反汇编

refl_results = {}
disasm_parts = []
for key, eid in sorted(first_eid_by_key.items(), key=lambda kv: str(kv[0])):
    try:
        ctrl.SetFrameEvent(eid, False)
        ps = ctrl.GetPipelineState()
        sh = ps.GetShader(rd.ShaderStage.Compute)
        res = sh.resource if hasattr(sh, "resource") else sh
        refl = get_reflection(res)
        if refl is None:
            refl_results[key] = {"key": key, "note": "反射失败"}
            continue
        info = extract_reflection(refl, {"key": key, "boundAtEvent": eid})
        recs = [r for r in cb_records if r.get("cs") == key]
        td = info.get("threadDim") or [1, 1, 1]
        total = sum(prod3(r.get("groups")) for r in recs) * prod3(td)
        info["dispatchCount"] = len(recs)
        info["totalThreads"] = total
        refl_results[key] = info
        print("反射 %s @%d: threadDim=%s dispatch=%d 总线程=%d cb=%d ro=%d rw=%d" % (
            key, eid, info.get("threadDim"), len(recs), total,
            len(info.get("cbuffers", [])), len(info.get("readOnly", [])),
            len(info.get("readWrite", []))))
        try:
            dis = ctrl.DisassembleShader(rd.ResourceId(), refl, "")
            disasm_parts.append(
                "=== %s (bound at event %d) threadDim=%s groups=%s dispatch=%d 总线程=%d ===\n%s\n"
                % (key, eid, info.get("threadDim"),
                   [r["groups"] for r in recs], len(recs), total,
                   dis or "<empty>"))
        except Exception as e:
            note_err("DisassembleShader %s" % key, e)
    except Exception as e:
        note_err("反射 %s" % key, e)
report["reflections"] = refl_results

# ===== 绑定资源尺寸 (首个事件处, 9 个 CS) =====
resources = {}
for key, eid in sorted(first_eid_by_key.items(), key=lambda kv: str(kv[0])):
    try:
        ctrl.SetFrameEvent(eid, False)
        ps = ctrl.GetPipelineState()
        entry = {"boundAtEvent": eid}
        refl = refl_results.get(key, {})
        for label, fn, rname in (
                ("readonly", ps.GetReadOnlyResources, "readOnly"),
                ("readwrite", ps.GetReadWriteResources, "readWrite")):
            lst = []
            try:
                descs = fn(rd.ShaderStage.Compute) or []
            except Exception as e:
                note_err("%s@%d" % (label, eid), e)
                descs = []
            rnames = [x.get("name") for x in (refl.get(rname) or [])]
            for idx, d in enumerate(descs):
                r = (desc_field(d, "descriptor", "resource")
                     or desc_field(d, "resource"))
                if r is None or str(r) in ("ResourceId::0", "0", "None"):
                    continue
                info = res_info(r)
                info["resource"] = str(r)
                info["bind"] = rnames[idx] if idx < len(rnames) else "res%d" % idx
                lst.append(info)
            entry[label] = lst
        resources[key] = entry
        print("资源 %s @%d:" % (key, eid))
        for label in ("readonly", "readwrite"):
            for info in entry[label]:
                if info.get("type") == "texture":
                    line = "   %-9s %-9s %s %sx%s d=%s arr=%s mips=%s %s %s" % (
                        label, info["bind"], info["resource"],
                        info.get("w"), info.get("h"), info.get("depth"),
                        info.get("arraysize"), info.get("mips"),
                        info.get("dimension"), info.get("format"))
                    if info.get("customName"):
                        line += ' "%s"' % info["customName"]
                    print(line)
                else:
                    print("   %-9s %-9s %s buffer %sB%s" % (
                        label, info["bind"], info["resource"], info.get("bytes"),
                        ' "%s"' % info["customName"] if info.get("customName") else ""))
    except Exception as e:
        note_err("资源 %s" % key, e)
report["resources_by_key"] = resources

# ===== 线程量汇总 =====
print("--- 线程量汇总 ---")
grand = 0
for key in sorted(refl_results, key=lambda k: str(k)):
    info = refl_results[key]
    if "totalThreads" in info:
        print("  %s: %d 次, 总线程 %d" % (key, info["dispatchCount"],
                                       info["totalThreads"]))
        grand += info["totalThreads"]
print("  合计: %d 线程/帧" % grand)
report["grand_total_threads"] = grand

# ===== 导出 =====
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

print("完成，用时 %.1fs，错误数 %d" % (report["elapsed_sec"],
                                   len(report["errors"])))

# 无头批跑 (--py 方式, rdc_run.ps1 置 RDC_HEADLESS=1) 才退出进程; UI 里 Run 不能退出
if os.environ.get("RDC_HEADLESS") == "1":
    sys.exit(0)
