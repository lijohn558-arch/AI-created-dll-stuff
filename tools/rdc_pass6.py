# rdc_pass6.py — 第6轮: 逐 Draw 只读资源(SRV)绑定 + 目标资源消费者 + 关键 PS 反汇编 + Clear 真实目标
#
# 用途 (结清 docs/99 §八 遗留事项):
#   ① 352/12856 与 333/17375 角色终判 —— 提 SRV 输入 + PS 反汇编 (读什么 → 是什么)
#   ② 512²/501 身份终判 —— 提 512² 段 SRV (镜像场景读 G-buffer?) + 谁消费 501/520/324/466
#   ③ 各条 Clear 真实目标 —— ctrl.GetUsage() 反查 (D3D11 清屏走 RTV/DSV 句柄, 绑定态不可信,
#      usage 表按资源记录 "该事件清了它" → 不用 UI 手点 API Inspector)
#   ④ 顺带: 逐 Draw viewport + numIndices (512² 是否主场景子集的量化对照)
#
# 用法 A (UI):  qrenderdoc -> Window -> Python Scripting -> 打开本文件 -> Run (UI 里不要开着抓帧)
# 用法 B (无头, 推荐): powershell tools\rdc_run.ps1 -Script rdc_pass6.py -Scene S5
#   环境变量 (rdc_run.ps1 -Scene/-Targets/-PsEvents 传入; 不设则用默认):
#     RDC_SCENE     场景 ID (如 S5) —— 拼 .rdc 与输出路径; 不设则用下面字面量
#     RDC_TARGETS   逗号分隔的目标资源 ID (本抓帧内的局部 ID) —— 查消费者
#     RDC_PSEVENTS  逗号分隔的事件号 —— 在这些事件处导出 PS 反射 + 反汇编
#     RDC_STRIDE    逐 Draw 提取步长 (默认 1 = 全量; 计数类问题可放大)
# 输出: <场景>-extract-pass6.json + <场景>-pass6-ps-disasm.txt + <场景>-extract-pass6.log

import renderdoc as rd
import json
import os
import sys
import time

# ===== 配置 =====
_SCENES = {"S1": "S1-city-day", "S2": "S2-interior", "S3": "S3-forest-godrays",
           "S4": "S4-water", "S5": "S5-night-combat"}
_RDC_SCENE = os.environ.get("RDC_SCENE", "").strip()
if _RDC_SCENE:
    _FULL = _SCENES.get(_RDC_SCENE, _RDC_SCENE)
    _PREFIX = _FULL.split("-")[0]
    _CAP = r"C:\Users\joker\skyrim-vulkan\captures\%s.rdc" % _FULL
    _OUT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-extract-pass6.json" % _PREFIX
    _OUT_DISASM = r"C:\Users\joker\skyrim-vulkan\docs\analysis\%s-pass6-ps-disasm.txt" % _PREFIX
else:
    _CAP = r"C:\Users\joker\skyrim-vulkan\captures\S7-heavy-mods.rdc"
    _OUT = r"C:\Users\joker\skyrim-vulkan\docs\analysis\S7-extract-pass6.json"
    _OUT_DISASM = r"C:\Users\joker\skyrim-vulkan\docs\analysis\S7-pass6-ps-disasm.txt"
RDC = _CAP
OUT_JSON = _OUT
OUT_DISASM = _OUT_DISASM


def _ints(envname, default=""):
    raw = os.environ.get(envname, default).strip()
    out = []
    for part in raw.replace(";", ",").split(","):
        part = part.strip()
        if part:
            try:
                out.append(int(part))
            except ValueError:
                pass
    return out


TARGET_RES = _ints("RDC_TARGETS")            # 要查消费者的资源 ID 列表
PS_EVENTS = _ints("RDC_PSEVENTS")            # 明确指定: 在这些事件处导出 PS 反射 + 反汇编
PS_WHITELIST = _ints("RDC_PSWL")             # 按 PS 资源 ID 白名单: 自动找它首个绑定的 Draw 事件再导出
DRAW_STRIDE = max(1, int(os.environ.get("RDC_STRIDE", "1")))

# 日志 tee: GUI 程序 stdout 不可见 → 同步落一份 .log
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

report = {"rdc": RDC, "errors": [], "script": "rdc_pass6 v1",
          "targets": TARGET_RES, "ps_events": PS_EVENTS, "stride": DRAW_STRIDE}
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
    """纹理全表: str(ResourceId) -> 'WxH[xD] 格式 [customName]' (与 pass4/pass5 同款: 本版 API 无 GetTexture 单查)"""
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
    if rid is None:
        return "-"
    s = str(rid)
    if s in ("None", "ResourceId::0", "0"):
        return "-"
    return _TEX_MAP.get(s, "?" + s)


build_tex_map()


# ===== 2. 动作树 =====
flat = []


def walk(actions):
    for a in actions:
        flat.append(a)
        try:
            if a.children:
                walk(a.children)
        except Exception:
            pass


draws, clears = [], []
try:
    flag_names = ["Drawcall", "Clear"]
    flags = {n: enum_int(n) for n in flag_names}
    flags = {n: v for n, v in flags.items() if v is not None}
    walk(ctrl.GetRootActions())
    for a in flat:
        try:
            af = int(a.flags)
        except Exception:
            af = 0
        if af & flags.get("Drawcall", 0):
            draws.append(a)
        if af & flags.get("Clear", 0):
            clears.append(a)
    print("Draw %d | Clear %d" % (len(draws), len(clears)))
except Exception as e:
    note_err("动作树", e)


# ===== 3. Clear 真实目标 (GetUsage 反查) =====
# 依据: RenderDoc 载入时按资源记录全帧 usage 列表; 某事件的 usage 含 "Clear" = 该事件清了这个资源
# (D3D11 清屏走 RTV/DSV 句柄不经 OM → 绑定态不可信; usage 表是引擎侧事实, 与 UI API Inspector 同源)
clear_targets = []
try:
    usage_by_res = {}
    for t in ctrl.GetTextures():
        rid = None
        for attr in ("resourceId", "id", "resource", "rid"):
            v = getattr(t, attr, None)
            if v is not None and not callable(v):
                rid = v
                break
        if rid is None or not valid_id(rid):
            continue
        try:
            ul = ctrl.GetUsage(rid)
        except Exception as e:
            note_err("GetUsage(%s)" % rid, e)
            continue
        if not ul:
            continue
        # 载入未跟踪的哨兵: 仅 1 条 eventId=0 且 usage=Unused
        first = ul[0]
        if len(ul) == 1 and int(getattr(first, "eventId", 0)) == 0 and "Unused" in str(getattr(first, "usage", "")):
            continue
        for u in ul:
            usage_by_res.setdefault(int(u.eventId), []).append((str(rid), str(u.usage)))

    print("usage 索引: %d 事件" % len(usage_by_res))
    for a in clears:
        rec = {"eventId": a.eventId}
        try:
            rec["flags_raw"] = int(a.flags)
        except Exception:
            pass
        hits = []
        for rid, usg in usage_by_res.get(a.eventId, []):
            if "Clear" in usg:
                hits.append({"id": rid, "usage": usg, "info": tex_info(rid)})
        rec["cleared"] = hits
        # 绑定态仅作对照 (可能完全错误, 见 docs/analysis 方法论)
        try:
            ctrl.SetFrameEvent(a.eventId, False)
            pipe = ctrl.GetPipelineState()
            rec["bound_rts"] = [str(d.resource) for d in pipe.GetOutputTargets() if valid_id(d.resource)]
            dt = pipe.GetDepthTarget()
            rec["bound_dsv"] = str(dt.resource) if dt is not None and valid_id(dt.resource) else "-"
        except Exception as e:
            note_err("clear绑定态@%d" % a.eventId, e)
        clear_targets.append(rec)
    clear_targets.sort(key=lambda c: c["eventId"])
    report["clear_targets"] = clear_targets
    print("Clear 目标反查 %d 条:" % len(clear_targets))
    for c in clear_targets:
        hits = " | ".join("%s(%s) %s" % (h["id"].replace("ResourceId::", ""), h["usage"], h["info"])
                          for h in c.get("cleared", [])) or "<无命中>"
        print("  ev%-6d %s   [绑定态 RT=%s DS=%s]" %
              (c["eventId"], hits,
               ",".join(x.replace("ResourceId::", "") for x in c.get("bound_rts", [])) or "-",
               str(c.get("bound_dsv", "-")).replace("ResourceId::", "")))
except Exception as e:
    note_err("Clear目标", e)


# ===== 4. 逐 Draw SRV/UAV + viewport + 图元数 =====
def desc_rec(ud, stage_name):
    """UsedDescriptor -> dict (槽位 / 资源 / 尺寸格式)"""
    d = {"stage": stage_name}
    try:
        d["slot"] = int(ud.access.index)
    except Exception:
        pass
    try:
        d["type"] = str(ud.access.type).split(".")[-1]
    except Exception:
        pass
    try:
        d["arrayIdx"] = int(getattr(ud.access, "arrayIndex", 0))
    except Exception:
        pass
    res = None
    try:
        res = ud.descriptor.resource
    except Exception:
        pass
    if res is not None and valid_id(res):
        d["id"] = str(res)
        d["info"] = tex_info(res)
        try:
            fm = getattr(ud.descriptor, "firstMip", None)
            if fm is not None:
                d["firstMip"] = int(fm)
        except Exception:
            pass
    else:
        d["id"] = "-"
        d["info"] = "-"
    return d


draw_recs = []
try:
    print("开始逐 Draw 提取 SRV/viewport (%d 个, stride=%d)..." % (len(draws), DRAW_STRIDE))
    for i, a in enumerate(draws):
        if i % DRAW_STRIDE != 0:
            continue
        try:
            ctrl.SetFrameEvent(a.eventId, False)
            pipe = ctrl.GetPipelineState()
            rec = {"eventId": a.eventId}
            try:
                rec["ps"] = str(pipe.GetShader(rd.ShaderStage.Pixel)).replace("ResourceId::", "")
            except Exception:
                rec["ps"] = "?"
            try:
                rec["vs"] = str(pipe.GetShader(rd.ShaderStage.Vertex)).replace("ResourceId::", "")
            except Exception:
                rec["vs"] = "?"
            try:
                v = pipe.GetViewport(0)
                rec["viewport"] = ("%dx%d" % (int(round(v.width)), int(round(v.height)))
                                   if v is not None and getattr(v, "enabled", True) else "-")
            except Exception:
                rec["viewport"] = "?"
            for attr in ("numIndices", "numInstances"):
                try:
                    val = getattr(a, attr, None)
                    if val is not None:
                        rec[attr] = int(val)
                except Exception:
                    pass
            outs = []
            try:
                outs = [str(d.resource) for d in pipe.GetOutputTargets() if valid_id(d.resource)]
            except Exception:
                pass
            rec["rts"] = outs
            try:
                dt = pipe.GetDepthTarget()
                rec["dsv"] = str(dt.resource) if dt is not None and valid_id(dt.resource) else "-"
            except Exception:
                rec["dsv"] = "-"
            for stage, sname in ((rd.ShaderStage.Pixel, "PS"), (rd.ShaderStage.Vertex, "VS")):
                try:
                    rec["srvs_" + sname] = [desc_rec(x, sname) for x in pipe.GetReadOnlyResources(stage)]
                except Exception as e:
                    note_err("SRV%s@%d" % (sname, a.eventId), e)
                try:
                    rec["uav_" + sname] = [desc_rec(x, sname) for x in pipe.GetReadWriteResources(stage)]
                except Exception:
                    pass
            draw_recs.append(rec)
            if (i + 1) % 500 == 0:
                print("  进度 %d/%d (%.0fs)" % (i + 1, len(draws), time.time() - t0))
        except Exception as e:
            note_err("draw%d" % getattr(a, "eventId", "?"), e)
    report["draws"] = draw_recs
    print("逐 Draw 提取完成: %d 条" % len(draw_recs))
except Exception as e:
    note_err("逐Draw", e)


# ===== 5. 目标资源的消费者 (谁在读 501/520/324/466/352/333 ...) =====
try:
    want = set("ResourceId::%d" % t for t in TARGET_RES)
    consumers = {}
    for rec in draw_recs:
        for key in ("srvs_PS", "srvs_VS", "uav_PS", "uav_VS"):
            for d in rec.get(key, []):
                if d.get("id") in want:
                    consumers.setdefault(d["id"], []).append({
                        "eventId": rec["eventId"], "stage": d.get("stage"),
                        "slot": d.get("slot"), "info": d.get("info"),
                        "ps": rec.get("ps"), "rts": rec.get("rts"),
                    })
    report["target_consumers"] = consumers
    print("目标消费者 (%d 个目标):" % len(TARGET_RES))
    for t in TARGET_RES:
        key = "ResourceId::%d" % t
        lst = consumers.get(key, [])
        print("  %d -> %d 处读取 %s" % (t, len(lst),
                                        ("ev:" + ",".join(str(x["eventId"]) for x in lst[:12]) +
                                         ("..." if len(lst) > 12 else "")) if lst else "(无人读取)"))
except Exception as e:
    note_err("消费者", e)


# ===== 6. 关键 PS 反射 + 反汇编 (角色终判用) =====
# PS_EVENTS = 明确事件号; PS_WHITELIST = PS 资源 ID → 自动取 draw_recs 中首个绑定它的 Draw 事件
disasm_parts = []
try:
    ev_list = list(PS_EVENTS)
    for psid in PS_WHITELIST:
        ps_s = str(psid)
        found = None
        for rec in draw_recs:
            if rec.get("ps") == ps_s:
                found = int(rec["eventId"])
                break
        if found is None:
            print("PS 白名单 %s: 未在任何 Draw 绑定, 跳过" % ps_s)
        elif found in ev_list:
            print("PS 白名单 %s: 已在事件列表 ev%d" % (ps_s, found))
        else:
            ev_list.append(found)
            print("PS 白名单 %s: 首个绑定事件 ev%d (已加入)" % (ps_s, found))
    ev_list = sorted(set(ev_list))
    if not ev_list:
        print("(跳过: 未提供 RDC_PSEVENTS/RDC_PSWL)")
    seen = set()
    for eid in ev_list:
        try:
            ctrl.SetFrameEvent(eid, False)
            pipe = ctrl.GetPipelineState()
            sh = pipe.GetShader(rd.ShaderStage.Pixel)
            shkey = str(sh).replace("ResourceId::", "")
            refl = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
            info = {"eventId": eid, "ps": shkey}
            if refl is None:
                info["note"] = "无反射"
                disasm_parts.append("=== PS %s @ev%d === <无反射>\n" % (shkey, eid))
            else:
                info["entryPoint"] = str(getattr(refl, "entryPoint", ""))
                try:
                    info["bytecodeSize"] = len(refl.rawBytes) if refl.rawBytes else 0
                except Exception:
                    pass
                ro = []
                for r in (getattr(refl, "readOnlyResources", None) or []):
                    ro.append({"name": str(getattr(r, "name", "?")),
                               "bindPoint": int(getattr(r, "bindPoint", -1)),
                               "bindArraySize": int(getattr(r, "bindArraySize", 1)),
                               "type": str(getattr(r, "type", "?")).split(".")[-1]})
                info["readOnly"] = ro
                info["samplers"] = [str(getattr(s, "name", s)) for s in (getattr(refl, "samplers", None) or [])]
                cb = []
                for c in (getattr(refl, "constantBlocks", None) or []):
                    cb.append({"name": str(getattr(c, "name", "?")),
                               "byteSize": int(getattr(c, "byteSize", -1)),
                               "bindPoint": int(getattr(c, "bindPoint", -1))})
                info["cbuffers"] = cb
                # 实绑只读资源 (槽位→实际纹理, 与反射的声明互补)
                try:
                    info["bound_srvs"] = [desc_rec(x, "PS") for x in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel)]
                except Exception as e:
                    note_err("PS实绑@%d" % eid, e)
                # 实绑 CB 内容头部 (前 64B, float 视图) —— 帧全局/太阳方向等可辨
                try:
                    cbs = pipe.GetConstantBlocks(rd.ShaderStage.Pixel)
                    heads = []
                    for c in cbs[:4]:
                        rid = c.descriptor.resource
                        if not valid_id(rid):
                            continue
                        b = ctrl.GetBufferData(rid, 0, 64)
                        import struct
                        floats = [round(x, 5) for x in struct.unpack("<16f", bytes(b)[:64])] if len(bytes(b)) >= 64 else []
                        heads.append({"id": str(rid), "bytes": len(bytes(b)), "head16f": floats})
                    info["cb_head"] = heads
                except Exception as e:
                    note_err("CB头@%d" % eid, e)
                try:
                    dis = ctrl.DisassembleShader(rd.ResourceId(), refl, "")
                    info["disasmLines"] = (dis.count("\n") + 1) if dis else 0
                    disasm_parts.append("=== PS %s @ev%d (event %d) ===\n%s\n" % (shkey, eid, eid, dis or "<empty>"))
                except Exception as e:
                    note_err("Disassemble PS %s" % shkey, e)
                    disasm_parts.append("=== PS %s @ev%d === <反汇编失败: %s>\n" % (shkey, eid, e))
            report.setdefault("ps_disasm", {})[shkey] = info
            if shkey not in seen:
                seen.add(shkey)
                print("PS 反汇编 %s @ev%d: ro=%d cb=%d lines=%s" %
                      (shkey, eid, len(info.get("readOnly", [])), len(info.get("cbuffers", [])),
                       info.get("disasmLines", "-")))
        except Exception as e:
            note_err("ps_disasm@%d" % eid, e)
except Exception as e:
    note_err("PS反汇编主循环", e)

try:
    with open(OUT_DISASM, "w", encoding="utf-8") as f:
        f.write("\n".join(disasm_parts))
    print("已导出反汇编:", OUT_DISASM)
except Exception as e:
    note_err("写反汇编", e)


# ===== 7. 导出 =====
report["elapsed_sec"] = round(time.time() - t0, 1)
try:
    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False, default=str)
    print("已导出:", OUT_JSON)
except Exception as e:
    note_err("写JSON", e)

try:
    ctrl.Shutdown()
except Exception:
    pass

print("完成，用时 %.1fs，错误数 %d" % (report["elapsed_sec"], len(report["errors"])))
try:
    sys.stdout.flush()
except Exception:
    pass
# 无头批跑 (--py 方式, rdc_run.ps1 置 RDC_HEADLESS=1) 才退出进程; UI 里 Run 不能退出
if os.environ.get("RDC_HEADLESS") == "1":
    sys.exit(0)
