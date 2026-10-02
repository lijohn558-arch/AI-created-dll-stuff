# rdc_compare.py — 配对判定 harness（docs/03 §3 锚点表 → 可执行判定）
#
# 用途: 比对两份提取产出（基线 DX11 vs 复跑/候选帧），按锚点逐项给 PASS / DIFF / SKIP。
#       配对门槛（docs/03 §7.1）第③条“锚点比对走脚本而非人工目视”的落地。
#
# 设计口径（docs/03 §5）:
#   - ResourceId 是抓帧局部量 → 所有锚点均为 **id-free 签名**（尺寸/格式串/计数/字节码哈希），
#     不比较 "ResourceId::501" 这类句柄。
#   - 两侧缺哪个文件，对应锚点 SKIP（不硬比）；errors[] 非空 = 脚本执行失败（不是 DIFF）。
#
# 用法 A (无头, 推荐): powershell tools\rdc_compare.ps1 -Base S4 -Cand S2
# 用法 B (任意 Python): RDC_CMP_BASE=S4 RDC_CMP_CAND=S2 python tools\rdc_compare.py
# 用法 C (UI): qrenderdoc -> Python Scripting -> 打开本文件 -> Run（不设 RDC_HEADLESS，不退出进程）
#
# 环境变量:
#   RDC_CMP_BASE / RDC_CMP_CAND  基线/候选：场景 ID (S4)、前缀 (S4-water)、或 pass1 JSON 绝对路径
#   RDC_CMP_OUT                  报告输出前缀（默认 <base>~<cand>-compare，落在 docs/analysis）
#   RDC_HEADLESS=1               结尾 sys.exit(0)（rdc_run/rdc_compare 的 ps1 会设；UI 手跑不设）
#
# 输出: docs/analysis/<base>~<cand>-compare.json + 同名 .log
# 判定: report["summary"]["match"] == true（无 DIFF 且至少 1 项通过）= 配对通过

import json
import os
import re
import sys
import time
import hashlib
from collections import Counter

# ===== 配置 =====
REPO = r"C:\Users\joker\skyrim-vulkan"
ANA = os.path.join(REPO, "docs", "analysis")

_CLEAR_ASPECT_BY_FLAGS = {1048577: "ClearColor", 2097153: "ClearDepthStencil"}

_MRT_GBUFFER = ["1920x1080 R16G16B16A16_FLOAT", "1920x1080 R16G16_FLOAT", "1920x1080 R8G8B8A8_UNORM"]
_MRT_TRANSPARENT = ["1920x1080 R16G16B16A16_FLOAT", "1920x1080 R8G8_UNORM", "1920x1080 R16G16B16A16_FLOAT"]


class Tee(object):
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


def _resolve(val, default):
    """RDC_CMP_* → {"dir","prefix"}：支持场景 ID / 前缀 / pass1 JSON 路径。"""
    v = (val or "").strip() or default
    if v.lower().endswith(".json") or os.sep in v or "/" in v:
        p = os.path.abspath(v)
        d, name = os.path.split(p)
        m = re.match(r"(.+?)-extract(-pass\d+)?\.json$", name, re.I)
        prefix = m.group(1) if m else os.path.splitext(name)[0]
        return {"dir": d, "prefix": prefix}
    return {"dir": ANA, "prefix": v.split("-")[0]}


def _path(side, kind):
    return os.path.join(side["dir"], side["prefix"] + kind)


def _load_json(path, errors):
    """读 JSON；文件缺失 → None；errors[] 非空 → 记 error 并作废该文件。"""
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            obj = json.load(f)
    except Exception as e:
        errors.append("parse failed: %s (%s)" % (os.path.basename(path), e))
        return None
    errs = obj.get("errors") or []
    if errs:
        errors.append("%s errors[] not empty (%d): %s" % (
            os.path.basename(path), len(errs), str(errs[0])[:120]))
        return None
    return obj


def _load_text(path):
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except Exception:
        return None


def _cs_hash_profile(text):
    """pass3 反汇编文本 → ["threadDim|sha1(body)", ...]；丢弃事件号头部行（跨抓帧可变）。"""
    blocks = []
    dim, lines = None, None
    for line in text.splitlines():
        if line.startswith("==="):
            if lines is not None:
                blocks.append((dim, lines))
            m = re.search(r"threadDim=\[[^\]]*\]", line)
            dim = m.group(0) if m else "?"
            lines = []
        elif lines is not None:
            lines.append(line)
    if lines is not None:
        blocks.append((dim, lines))
    prof = []
    for d, ls in blocks:
        body = "\n".join(x.rstrip() for x in ls).strip()
        if not body:
            continue
        prof.append(d + "|" + hashlib.sha1(body.encode("utf-8")).hexdigest()[:12])
    return sorted(prof)


def _seg_signature(seg):
    vp = ",".join(sorted((seg.get("viewports") or {}).keys()))
    rts = "||".join(seg.get("rt_infos") or [])
    return (vp, rts, seg.get("dsv_info") or "-")


def _side_data(side, errors):
    """把一个侧的各 pass 文件读进来。"""
    d = {
        "p1": _load_json(_path(side, "-extract.json"), errors),
        "p4": _load_json(_path(side, "-extract-pass4.json"), errors),
        "p5": _load_json(_path(side, "-extract-pass5.json"), errors),
        "p6": _load_json(_path(side, "-extract-pass6.json"), errors),
        "disasm": None,
    }
    for kind in ("-post-cs-disasm.txt", "-cs-disasm.txt"):
        t = _load_text(_path(side, kind))
        if t:
            d["disasm"] = t
            break
    d["present"] = [k for k in ("p1", "p4", "p5", "p6") if d[k]]
    return d


# ===== 各锚点（返回 status, base_val, cand_val, detail） =====

def a_counts(b, c):
    bv = {"draws": b["draws"], "dispatches": b["dispatches"],
          "clears": (b.get("flag_counts") or {}).get("Clear"),
          "copies": (b.get("flag_counts") or {}).get("Copy"),
          "present": (b.get("flag_counts") or {}).get("Present"),
          "pso": b.get("approx_pso_count"),
          "textures": b.get("textures_total")}
    cv = {"draws": c["draws"], "dispatches": c["dispatches"],
          "clears": (c.get("flag_counts") or {}).get("Clear"),
          "copies": (c.get("flag_counts") or {}).get("Copy"),
          "present": (c.get("flag_counts") or {}).get("Present"),
          "pso": c.get("approx_pso_count"),
          "textures": c.get("textures_total")}
    diffs = [k for k in bv if bv[k] != cv[k]]
    detail = "diff keys: " + ",".join(diffs) if diffs else ""
    return ("PASS" if not diffs else "DIFF"), bv, cv, detail


def a_formats(b, c):
    bv = sorted(str(x) for x in (b.get("texture_formats") or []))
    cv = sorted(str(x) for x in (c.get("texture_formats") or []))
    if bv == cv:
        return "PASS", "%d formats" % len(bv), "%d formats" % len(cv), ""
    only_b = sorted(set(bv) - set(cv))
    only_c = sorted(set(cv) - set(bv))
    return "DIFF", "%d formats" % len(bv), "%d formats" % len(cv), \
        "base-only: %s / cand-only: %s" % (",".join(only_b) or "-", ",".join(only_c) or "-")


def _cs_profile(p4):
    disp = p4.get("dispatches") or []
    groups = Counter(",".join(str(x) for x in (d.get("groups") or [])) for d in disp)
    return {"count": len(disp),
            "grand": p4.get("grand_total_threads"),
            "groups": dict(sorted(groups.items()))}


def a_cs_dispatch(b, c):
    bv, cv = _cs_profile(b), _cs_profile(c)
    diffs = [k for k in bv if bv[k] != cv[k]]
    detail = "diff keys: " + ",".join(diffs) if diffs else ""
    return ("PASS" if not diffs else "DIFF"), bv, cv, detail


def a_cs_bytecode(b, c):
    bv, cv = _cs_hash_profile(b["disasm"]), _cs_hash_profile(c["disasm"])
    if bv == cv:
        return "PASS", "%d CS blocks" % len(bv), "%d CS blocks" % len(cv), ""
    only_b = [x for x in bv if x not in cv]
    only_c = [x for x in cv if x not in bv]
    return "DIFF", "%d CS blocks" % len(bv), "%d CS blocks" % len(cv), \
        "base-only: %s / cand-only: %s" % (
            ";".join(only_b[:4]) or "-", ";".join(only_c[:4]) or "-")


def a_pass5_structure(b, c, p1b, p1c):
    sb, sc = b.get("draw_segments") or [], c.get("draw_segments") or []
    cb = Counter(_seg_signature(s) for s in sb)
    cc = Counter(_seg_signature(s) for s in sc)
    cov_b = sum(s.get("draws") or 0 for s in sb)
    cov_c = sum(s.get("draws") or 0 for s in sc)
    bv = {"segments": len(sb), "draws_sum": cov_b}
    cv = {"segments": len(sc), "draws_sum": cov_c}
    notes = []
    if p1b and cov_b != p1b.get("draws"):
        notes.append("base coverage %d != pass1 %s" % (cov_b, p1b.get("draws")))
    if p1c and cov_c != p1c.get("draws"):
        notes.append("cand coverage %d != pass1 %s" % (cov_c, p1c.get("draws")))
    if cb == cc:
        return "PASS", bv, cv, "; ".join(notes)
    missing = [k for k in cb if cc.get(k, 0) < cb[k]]
    extra = [k for k in cc if cb.get(k, 0) < cc[k]]
    notes.append("base-only segs x%d: %s" % (
        len(missing), " ;; ".join("%s|%s" % (m[1], m[2]) for m in missing[:3]) or "-"))
    notes.append("cand-only segs x%d: %s" % (
        len(extra), " ;; ".join("%s|%s" % (m[1], m[2]) for m in extra[:3]) or "-"))
    return "DIFF", bv, cv, "; ".join(notes)


def a_clear_bound(p5):
    """p5 clears：绑定态视角的 Clear 画像（aspects + 绑定目标格式）。"""
    prof = Counter()
    for cl in p5.get("clears") or []:
        rts = "||".join(x for x in (cl.get("rt_infos") or []) if x and x != "-")
        prof[("+".join(cl.get("aspects") or []), rts or "-", cl.get("dsv_info") or "-")] += 1
    return prof


def a_clear_target(p6):
    """p6 clear_targets：GetUsage 反查的真实清屏目标画像（id-free：format 串）。"""
    prof = Counter()
    for ct in p6.get("clear_targets") or []:
        aspect = _CLEAR_ASPECT_BY_FLAGS.get(ct.get("flags_raw"), "flags:%s" % ct.get("flags_raw"))
        for cl in ct.get("cleared") or []:
            info = cl.get("info") if isinstance(cl, dict) else (cl or {}).get("info", "?")
            prof[(aspect, str(info))] += 1
    return prof


def _counter_anchor(name, pf_b, pf_c, fmt):
    bv, cv = dict(pf_b), dict(pf_c)
    if pf_b == pf_c:
        return "PASS", fmt(bv), fmt(cv), ""
    only_b = sorted(set(pf_b) - set(pf_c), key=str)
    only_c = sorted(set(pf_c) - set(pf_b), key=str)
    return "DIFF", fmt(bv), fmt(cv), "base-only: %s / cand-only: %s" % (
        " ;; ".join(str(fmt({k: pf_b[k]})) for k in only_b[:3]) or "-",
        " ;; ".join(str(fmt({k: pf_c[k]})) for k in only_c[:3]) or "-")


def a_copy_seq(p5):
    seq_b = ["%s -> %s" % (cp.get("src_info"), cp.get("dst_info"))
             for cp in (p5.get("copies") or [])]
    seq_c = ["%s -> %s" % (cp.get("src_info"), cp.get("dst_info"))
             for cp in (p5.get("copies") or [])]
    return seq_b, seq_c


def a_conditional(p5, p6):
    segs = p5.get("draw_segments") or []
    node = {
        "cube512_seg": sum(1 for s in segs
                           if (s.get("rt_infos") or [""])[0] == "512x512 R16G16B16A16_FLOAT"
                           and s.get("dsv_info") == "512x512 D24S8_TYPELESS"),
        "gbuffer_mrt_seg": sum(1 for s in segs
                               if (s.get("rt_infos") or [])[:3] == _MRT_GBUFFER),
        "transparent_mrt_seg": sum(1 for s in segs
                                   if (s.get("rt_infos") or [])[:3] == _MRT_TRANSPARENT),
        "dark_rgba8_small_seg": sum(
            1 for s in segs
            if (s.get("rt_infos") or ["", ""])[0] == "1920x1080 R8G8B8A8_UNORM"
            and len(s.get("rt_infos") or []) > 1 and s["rt_infos"][1] == "-"
            and s.get("dsv_info") == "1920x1080 D24S8_TYPELESS"
            and (s.get("draws") or 0) <= 4),
        "copies_total": len(p5.get("copies") or []),
    }
    if p6:
        node["slot2048_clears"] = sum(
            1 for (_, info) in a_clear_target(p6) if str(info) == "2048x2048 R16_TYPELESS")
    return node


def a_transparent_draws(p5):
    segs = [s for s in (p5.get("draw_segments") or [])
            if (s.get("rt_infos") or [])[:3] == _MRT_TRANSPARENT]
    return sum(s.get("draws") or 0 for s in segs)


def main():
    t0 = time.time()
    base = _resolve(os.environ.get("RDC_CMP_BASE"), "S1")
    cand = _resolve(os.environ.get("RDC_CMP_CAND"), "S4")
    out_prefix = (os.environ.get("RDC_CMP_OUT") or "").strip()
    if not out_prefix:
        out_prefix = os.path.join(ANA, "%s~%s-compare" % (base["prefix"], cand["prefix"]))
    out_json = out_prefix + ".json"
    out_log = out_prefix + ".log"

    sys.stdout = Tee(sys.stdout, open(out_log, "w", encoding="utf-8"))

    report = {
        "script": "rdc_compare v1",
        "base": base, "cand": cand,
        "anchors": [], "summary": {}, "match": False,
        "errors": [], "elapsed_sec": 0.0,
    }
    errors = report["errors"]

    db = _side_data(base, errors)
    dc = _side_data(cand, errors)
    anchors = report["anchors"]

    def add(aid, name, status, bv="", cv="", detail=""):
        anchors.append({"id": aid, "name": name, "status": status,
                        "base": bv, "cand": cv, "detail": detail})
        print("%-26s %-5s base=%s cand=%s%s" % (
            aid, status, _short(bv), _short(cv), (" | " + detail) if detail else ""))

    # A1 规模守恒
    if db["p1"] and dc["p1"]:
        add("counts.pass1", "Draw/Dispatch/Clear/Copy/PSO", *a_counts(db["p1"], dc["p1"]))
    else:
        add("counts.pass1", "Draw/Dispatch/Clear/Copy/PSO", "SKIP", detail="缺 pass1 JSON")

    # A2 纹理格式集合
    if db["p1"] and dc["p1"]:
        add("textures.formats", "纹理格式集合", *a_formats(db["p1"], dc["p1"]))
    else:
        add("textures.formats", "纹理格式集合", "SKIP", detail="缺 pass1 JSON")

    # A3 CS 调度画像
    if db["p4"] and dc["p4"]:
        add("cs.dispatch-profile", "CS dispatch/线程/线程组", *a_cs_dispatch(db["p4"], dc["p4"]))
    else:
        add("cs.dispatch-profile", "CS dispatch/线程/线程组", "SKIP", detail="缺 pass4 JSON")

    # A4 CS 字节码一致性（反汇编归一化哈希）
    if db["disasm"] and dc["disasm"]:
        add("cs.bytecode-hash", "CS DXBC 逐字节(反汇编)", *a_cs_bytecode(db, dc))
    else:
        add("cs.bytecode-hash", "CS DXBC 逐字节(反汇编)", "SKIP", detail="缺 cs-disasm txt")

    # A5 pass5 分段结构
    if db["p5"] and dc["p5"]:
        add("pass5.structure", "分段数/签名多重集",
            *a_pass5_structure(db["p5"], dc["p5"], db["p1"], dc["p1"]))
    else:
        add("pass5.structure", "分段数/签名多重集", "SKIP", detail="缺 pass5 JSON")

    # A6 Clear 画像（绑定态视角，p5）
    if db["p5"] and dc["p5"]:
        add("clear.bound-profile", "Clear 方位+绑定格式",
            *_counter_anchor("clear.bound", a_clear_bound(db["p5"]), a_clear_bound(dc["p5"]),
                             lambda d: "; ".join("%s@%s x%d" % (k[0], k[2], v)
                                                 for k, v in sorted(d.items(), key=str)[:4])))
    else:
        add("clear.bound-profile", "Clear 方位+绑定格式", "SKIP", detail="缺 pass5 JSON")

    # A7 Clear 真实目标画像（GetUsage 反查，p6；权威口径）
    if db["p6"] and dc["p6"]:
        add("clear.target-profile", "Clear 真实目标(格式)",
            *_counter_anchor("clear.target", a_clear_target(db["p6"]), a_clear_target(dc["p6"]),
                             lambda d: "; ".join("%s %s x%d" % (k[0], k[1], v)
                                                 for k, v in sorted(d.items(), key=str)[:4])))
    else:
        add("clear.target-profile", "Clear 真实目标(格式)", "SKIP", detail="缺 pass6 JSON")

    # A8 Copy 序列
    if db["p5"] and dc["p5"]:
        seq_b, seq_c = a_copy_seq(db["p5"]), a_copy_seq(dc["p5"])
        add("copy.sequence", "Copy 链(src/dst 格式)",
            "PASS" if seq_b == seq_c else "DIFF", seq_b, seq_c,
            "" if seq_b == seq_c else "顺序或条目不同")
    else:
        add("copy.sequence", "Copy 链(src/dst 格式)", "SKIP", detail="缺 pass5 JSON")

    # A9 条件节点
    if db["p5"] and dc["p5"]:
        nv_b = a_conditional(db["p5"], db["p6"])
        nv_c = a_conditional(dc["p5"], dc["p6"])
        diffs = [k for k in nv_b if nv_b[k] != nv_c.get(k)]
        add("conditional-nodes", "条件节点计数",
            "PASS" if not diffs else "DIFF", nv_b, nv_c,
            "diff keys: " + ",".join(diffs) if diffs else "")
    else:
        add("conditional-nodes", "条件节点计数", "SKIP", detail="缺 pass5 JSON")

    # A10 透明段规模
    if db["p5"] and dc["p5"]:
        tb, tc = a_transparent_draws(db["p5"]), a_transparent_draws(dc["p5"])
        add("transparent.draws", "透明段 Draw 数",
            "PASS" if tb == tc else "DIFF", tb, tc,
            "" if tb == tc else "delta=%+d" % (tc - tb))
    else:
        add("transparent.draws", "透明段 Draw 数", "SKIP", detail="缺 pass5 JSON")

    n_pass = sum(1 for a in anchors if a["status"] == "PASS")
    n_diff = sum(1 for a in anchors if a["status"] == "DIFF")
    n_skip = sum(1 for a in anchors if a["status"] == "SKIP")
    report["summary"] = {"pass": n_pass, "diff": n_diff, "skip": n_skip,
                         "total": len(anchors)}
    report["match"] = (n_diff == 0 and n_pass > 0)
    report["elapsed_sec"] = round(time.time() - t0, 1)

    with open(out_json, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)

    print("summary: pass=%d diff=%d skip=%d (total %d) elapsed=%ss"
          % (n_pass, n_diff, n_skip, len(anchors), report["elapsed_sec"]))
    if errors:
        print("errors: %d" % len(errors))
        for e in errors:
            print("  ! " + e)
    print("report: " + out_json)
    print("RESULT: %s" % ("MATCH" if report["match"] else "MISMATCH"))
    if os.environ.get("RDC_HEADLESS", "").strip() == "1":
        sys.exit(0)


def _short(v, n=64):
    s = json.dumps(v, ensure_ascii=False) if not isinstance(v, str) else v
    return s if len(s) <= n else s[:n - 3] + "..."


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as e:
        print("FATAL: %s: %s" % (type(e).__name__, e))
        if os.environ.get("RDC_HEADLESS", "").strip() == "1":
            sys.exit(3)
        raise
