#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Source-only regression checks for the Android 6.6 SCX port (needs git history).

This checks declarations and integration invariants, not compiled ABI or runtime.
"""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE = "5dcc2658d48c"


def source(name, before=False):
    if before:
        return subprocess.check_output(
            ["git", "show", f"{BASE}:{name}"], cwd=ROOT, text=True
        )
    return (ROOT / name).read_text()


def uncomment(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def block(text, marker):
    text = uncomment(text)
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[opening + 1:end - 1]


def select(text, enabled):
    """Select known config branches, preserving unknown branches verbatim."""
    out, stack = [], []
    active = True
    for line in text.splitlines():
        match = re.match(r"\s*#\s*(ifdef|ifndef|if|else|elif|endif)\b(.*)", line)
        if not match:
            if active:
                out.append(line)
            continue
        op, expr = match.groups()
        if op in ("if", "ifdef", "ifndef"):
            names = set(re.findall(r"CONFIG_\w+", expr))
            known = bool(names) and names <= enabled.keys()
            value = True
            if known:
                if op == "if":
                    expr = re.sub(r"defined\((CONFIG_\w+)\)",
                                  lambda m: str(enabled[m[1]]), expr)
                    expr = expr.replace("||", " or ").replace("&&", " and ")
                    value = bool(eval(expr, {"__builtins__": {}}, {}))
                else:
                    value = enabled[expr.strip()]
                    if op == "ifndef":
                        value = not value
            stack.append((active, known, value))
            active = active and value
            if active and not known:
                out.append(line)
        elif op == "else":
            parent, known, value = stack[-1]
            active = parent and (not value if known else True)
            if active and not known:
                out.append(line)
        elif op == "endif":
            parent, known, _ = stack.pop()
            if parent and not known:
                out.append(line)
            active = parent
        else:
            assert not stack[-1][1], "known #elif needs explicit handling"
            if active:
                out.append(line)
    assert not stack
    return "\n".join(out)


def declaration(text, name, enabled):
    text = select(block(text, f"struct {name} {{"), enabled)
    # genksyms sees the original reserve; android_kabi.h checks size/alignment.
    for macro in ("ANDROID_KABI_USE2", "ANDROID_KABI_USE"):
        while match := re.search(r"\b" + macro + r"\((\d+),", text):
            pos, depth = match.end(), 1
            while depth:
                depth += (text[pos] == "(") - (text[pos] == ")")
                pos += 1
            text = (text[:match.start()] + f"ANDROID_KABI_RESERVE({match[1]})"
                    + text[pos:])
    return re.findall(r"\w+|[^\s]", text)


for enabled in (False, True):
    config = {"CONFIG_SCHED_CLASS_EXT": enabled, "CONFIG_EXT_GROUP_SCHED": enabled}
    for file, names in {
        "include/linux/sched.h": ("task_struct", "sched_entity"),
        "kernel/sched/sched.h": ("rq", "task_group", "sched_class"),
        "include/linux/cgroup-defs.h": ("cgroup_file",),
    }.items():
        for name in names:
            assert declaration(source(file, True), name, config) == declaration(
                source(file), name, config
            ), (name, enabled, "KABI declaration changed")
print("PASS: six KABI declarations, SCX enabled and disabled")

core, ext, fork = map(source, ("kernel/sched/core.c", "kernel/sched/ext.c", "kernel/fork.c"))
for file in ("kernel/sched/core.c", "kernel/sched/fair.c", "kernel/sched/idle.c",
             "kernel/sched/topology.c", "kernel/fork.c"):
    hooks = lambda text: set(re.findall(r"\btrace_android_\w+(?=\()", text))
    assert hooks(source(file, True)) <= hooks(source(file)), file
assert "void reweight_task(struct task_struct *p, int prio)" in source("kernel/sched/fair.c")
assert "EXPORT_SYMBOL_GPL(reweight_task)" in source("kernel/sched/fair.c")
assert fork.index("tsk->scx = NULL") < fork.index("tsk->scx = kzalloc_node")
assert "sizeof(*tsk->scx), GFP_KERNEL_ACCOUNT, node" in fork
assert fork.count("kfree(tsk->scx)") == 2
assert "sched_ext_free(tsk);" in fork
assert "atomic_long_set(&p->scx->ops_state, 0)" in core
assert "set_load_weight(p, false);" in block(core, "void check_class_changing(")
assert core.count("scx_rq_online(rq, reason);") == 1
assert core.count("scx_rq_offline(rq, reason);") == 1
assert set(re.findall(r"offsetof\(struct sched_ext_entity, (\w+)\)", ext)) == {
    "slice", "dsq_vtime", "disallow"
}
for file in (ROOT / "tools/sched_ext").rglob("*.c"):
    assert "->scx." not in file.read_text(), file
assert not re.search(r"struct (?:task_struct|rq),\s*scx[.)]", ext)
print("PASS: Android hooks, lifecycle, callback API and BPF sidecar accesses")

legacy = block(core, "static struct cftype cpu_legacy_cftypes")
assert "CPU_CFTYPE_" not in legacy
assert re.findall(r'\.name\s*=\s*"([^"]+)"', legacy) == re.findall(
    r'\.name\s*=\s*"([^"]+)"',
    block(source("kernel/sched/core.c", True), "static struct cftype cpu_legacy_files")
)
header = source("kernel/sched/sched.h")
ids = block(header, "enum cpu_cftype_id {")
table = block(core, "struct cftype cpu_cftypes[")
for fair in (False, True):
    for bandwidth in (False, True):
        for uclamp in (False, True):
            for scx in (False, True):
                config = {"CONFIG_FAIR_GROUP_SCHED": fair,
                          "CONFIG_EXT_GROUP_SCHED": scx,
                          "CONFIG_CFS_BANDWIDTH": fair and bandwidth,
                          "CONFIG_UCLAMP_TASK_GROUP": uclamp}
                expected = re.findall(r"\bCPU_CFTYPE_(?!CNT\b)\w+", select(ids, config))
                actual = re.findall(r"\[(CPU_CFTYPE_\w+)\]", select(table, config))
                assert expected == actual, (config, expected, actual)
print("PASS: legacy cgroup files and 16 cgroup-v2 config combinations")

ops = block(source("include/linux/sched/ext.h"), "struct sched_ext_ops {")
callbacks = re.findall(r"\b(void|s32|bool) \(\*(\w+)\)\(([^;]*)\);", ops)
for result, name, args in callbacks:
    stub = re.search(r"static " + result + r" scx_cfi_" + name + r"\(([^)]*)\)", ext)
    assert stub and re.sub(r"\s", "", args) == re.sub(r"\s", "", stub[1]), name
    assert f".{name} = scx_cfi_{name}," in ext, name
assert ".cfi_stubs = &scx_ops_cfi_stubs," in ext
assert "jiffies_to_msecs(SCX_WATCHDOG_MAX_TIMEOUT)" in ext
assert "CONFIG_SCHED_CLASS_EXT=y" in source("arch/arm64/configs/gki_defconfig")
print(f"PASS: {len(callbacks)} typed CFI stubs, timeout units and GKI opt-in")

assert "tg->scx_weight = CGROUP_WEIGHT_DFL;" in block(core, "struct task_group *sched_create_group(")
init = block(core, "void __init sched_init(")
assert "root_task_group.scx_weight" not in block(init, "if (ptr)")
assert "root_task_group.scx_weight = CGROUP_WEIGHT_DFL;" in init
print("PASS: memory-accounted task sidecars and default cgroup weights")

online = block(ext, "int scx_tg_online(")
early_root = block(online, "if (tg == &root_task_group)")
assert "tg->scx_flags |= SCX_TG_ONLINE;" in early_root
assert "return 0;" in early_root
assert online.index("if (tg == &root_task_group)") < online.index("percpu_down_read(")
assert "percpu_" not in early_root
assert "percpu_down_read(&scx_cgroup_rwsem);" in online
assert "percpu_up_read(&scx_cgroup_rwsem);" in online
print("PASS: early root cgroup avoids per-CPU locking")

enable = block(ext, "static int scx_ops_enable(")
prepare = enable[enable.index("ret = scx_ops_prepare_task(p, task_group(p));"):]
failure = block(prepare, "if (ret)")
assert failure.index("p->comm, p->pid") < failure.index("put_task_struct(p)")
cancel = block(ext, "void scx_cancel_fork(")
assert (cancel.index("task_rq_lock(p, &rf)") <
        cancel.index("scx_ops_disable_task(p)") <
        cancel.index("task_rq_unlock(rq, p, &rf)") <
        cancel.index("percpu_up_read(&scx_fork_rwsem)"))
assert "div_s64((s64)(runnable_at - jiffies) * MSEC_PER_SEC, HZ)" in ext
print("PASS: enable failure task lifetime, fork cancellation locking and diagnostic units")

helper = block(ext, "static struct kthread_worker *scx_create_rt_helper(")
assert "if (!IS_ERR(helper))" in helper
creation = block(enable, "if (!scx_ops_helper)")
assert "ret = PTR_ERR(helper);" in block(creation, "if (IS_ERR(helper))")
assert creation.index("goto err_unlock;") < creation.index("WRITE_ONCE(scx_ops_helper, helper)")
print("PASS: helper creation rejects error pointers before publication")
