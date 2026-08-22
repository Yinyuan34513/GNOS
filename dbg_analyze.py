#!/usr/bin/env python3
"""Analyze GNOS syscall trace logs (TRC entry / TR2 return lines).

Streams the file (logs are 100 MB+), never loads it whole.  Usage:

    python3 dbg_analyze.py LOG [PID]

If PID is omitted, the most-active pid is analyzed.
"""
import re
import sys
from collections import Counter, defaultdict

TRC = re.compile(
    r"^TRC p=(\d+) nr=(\d+) a1=0x[0-9a-f]+ a2=0x[0-9a-f]+ a3=0x[0-9a-f]+ rip=(0x[0-9a-f]+)"
)
TR2 = re.compile(r"^TR2 p=(\d+) nr=(\d+) ret=(0x[0-9a-f]+)")


def main(path: str, want_pid: str | None = None) -> None:
    cnt_pid = Counter()                # pid -> syscall count
    nr_rip = defaultdict(Counter)      # pid -> (nr, rip) -> count
    nr_ret = defaultdict(Counter)      # pid -> (nr, ret) -> count
    seq = defaultdict(list)            # pid -> [(nr, rip, ret)] in order
    pending = {}                       # pid -> (nr, rip) awaiting its TR2
    tail: list[str] = []

    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            tail.append(line)
            if len(tail) > 25:
                tail.pop(0)
            m = TRC.match(line)
            if m:
                p, nr, rip = m.groups()
                cnt_pid[p] += 1
                nr_rip[p][(nr, rip)] += 1
                pending[p] = (nr, rip)
                continue
            m = TR2.match(line)
            if m:
                p, nr, ret = m.groups()
                nr_ret[p][(nr, ret)] += 1
                pend = pending.pop(p, None)
                if pend and pend[0] == nr:
                    seq[p].append((pend[0], pend[1], ret))
                continue

    print(f"=== 文件 {path}: {sum(cnt_pid.values()):,} 次系统调用 ===")
    print("--- pid 活跃度 ---")
    for p, c in cnt_pid.most_common(8):
        print(f"  pid {p:>4}: {c:,}")
    if not want_pid:
        want_pid = cnt_pid.most_common(1)[0][0]
    p = str(want_pid)
    print(f"\n=== 分析 pid {p} ===")
    print("--- 最常见的 (nr, rip) 组合 ---")
    for (nr, rip), c in nr_rip[p].most_common(12):
        print(f"  {c:>8,}x  nr={nr:<4} rip={rip}")
    print("--- 最常见的 (nr, ret) 组合 ---")
    for (nr, ret), c in nr_ret[p].most_common(10):
        print(f"  {c:>8,}x  nr={nr:<4} ret={ret}")
    print("--- 序列头部 (前 12 个调用) ---")
    for t in seq[p][:12]:
        print(f"  nr={t[0]:<4} rip={t[1]} ret={t[2]}")
    print("--- 序列尾部 (最后 15 个调用) ---")
    for t in seq[p][-15:]:
        print(f"  nr={t[0]:<4} rip={t[1]} ret={t[2]}")
    print("--- 去重后的 rip 列表 (nr: rip 集合) ---")
    by_nr: dict[str, set] = defaultdict(set)
    for (nr, rip) in nr_rip[p]:
        by_nr[nr].add(rip)
    for nr, rips in sorted(by_nr.items()):
        print(f"  nr={nr}: {sorted(rips)}")
    print("\n--- 日志最后 25 行 ---")
    print("".join(tail))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
