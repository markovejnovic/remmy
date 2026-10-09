#!/usr/bin/env python3
"""Symbolicate kernel frames in an Instruments Time Profiler trace.

Instruments on Apple silicon leaves most kernel frames as raw addresses even
with the matching Kernel Debug Kit installed: it slides kernel addresses by
the kernel's __TEXT load address, but kernel code runs from __TEXT_EXEC,
which moves by a different amount. This script exports the trace's
time-profile table, works out the real slide from the kernel frames
Instruments did name, symbolicates the rest with
`atos -textExecAddress`, and prints a text report. With --collapsed it also
writes folded stacks for flame-graph viewers (speedscope, Firefox Profiler).

usage: symbolicate_trace.py TRACE [--top N] [--out FILE] [--collapsed [FILE]]
Needs: Xcode (for xctrace) and the KDK matching the traced kernel's build.
"""

import argparse
import collections
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

KDK_GLOB = "/Library/Developer/KDKs/*.kdk/System/Library/Kernels/*.dSYM"


def find_xctrace():
    for p in (os.environ.get("XCTRACE"), "/Applications/Xcode.app/Contents/Developer/usr/bin/xctrace",
              "/Applications/Xcode-beta.app/Contents/Developer/usr/bin/xctrace", shutil.which("xctrace")):
        if p and os.access(p, os.X_OK):
            return p
    sys.exit("xctrace not found: install Xcode or set XCTRACE=/path/to/xctrace")


def export_time_profile(xctrace, trace, out):
    xpath = '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'
    subprocess.run([xctrace, "export", "--input", trace, "--xpath", xpath, "--output", out],
                   check=True, capture_output=True)


def load_rows(path):
    """Returns (rows, kernel) where rows = [(state, weight_ns, thread, frames)], frames leaf first as
    (name, addr, binary), and kernel = (binary name, UUID) of the kernel image."""
    root = ET.parse(path).getroot()
    ids = {}
    for e in root.iter():
        if e.get("id"):
            ids[e.get("id")] = e

    def r(e):
        return ids[e.get("ref")] if e is not None and e.get("ref") else e

    rows, kernel = [], None
    for row in root.iter("row"):
        st, w, th, bt = (r(row.find(t)) for t in ("thread-state", "weight", "thread", "tagged-backtrace"))
        if st is None or w is None:
            continue
        frames = []
        for f in (bt.findall("frame") if bt is not None else []):
            f = r(f)
            b = r(f.find("binary"))
            bname = (b.get("name") or "?") if b is not None else "?"
            if b is not None and bname.startswith("kernel") and b.get("UUID"):
                kernel = (bname, b.get("UUID"))
            addr = f.get("addr")
            frames.append((f.get("name") or "?", int(addr, 16) if addr else 0, bname))
        rows.append((st.text or "?", int(w.text or 0), (th.get("fmt") or "?") if th is not None else "?", frames))
    return rows, kernel


def find_dsym(binary, uuid):
    for d in glob.glob(KDK_GLOB):
        dwarf = os.path.join(d, "Contents/Resources/DWARF", os.path.basename(d)[: -len(".dSYM")])
        if os.path.basename(dwarf) != binary or not os.path.exists(dwarf):
            continue
        out = subprocess.run(["dwarfdump", "--uuid", dwarf], capture_output=True, text=True).stdout
        if uuid.upper() in out.upper():
            return dwarf
    sys.exit(f"no KDK dSYM for {binary} {uuid}; install the Kernel Debug Kit for the traced build")


def static_symbols(dwarf):
    syms = {}
    for line in subprocess.run(["nm", dwarf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            syms.setdefault(p[2].lstrip("_"), int(p[0], 16))
    return syms


def text_exec_vmaddr(dwarf):
    out = subprocess.run(["otool", "-l", dwarf], capture_output=True, text=True).stdout
    m = re.search(r"segname __TEXT_EXEC\s+vmaddr (0x[0-9a-f]+)", out)
    if not m:
        sys.exit("no __TEXT_EXEC segment in the dSYM")
    return int(m.group(1), 16)


def find_slide(rows, kernel_bin, syms):
    """The slide is the page-aligned (runtime - static) offset most named kernel frames agree on."""
    votes = collections.Counter()
    seen = {}
    for _, _, _, frames in rows:
        for name, addr, b in frames:
            if b == kernel_bin and not name.startswith("0x") and name in syms:
                seen[name] = min(seen.get(name, addr), addr)
    for name, addr in seen.items():
        if addr >= syms[name]:
            votes[(addr - syms[name]) & ~0xFFF] += 1
    if not votes:
        sys.exit("no named kernel frames to derive the slide from")
    slide, n = votes.most_common(1)[0]
    return slide, n, len(seen)


def atos(dwarf, text_exec, addrs):
    out = {}
    addrs = sorted(addrs)
    for i in range(0, len(addrs), 500):
        chunk = addrs[i : i + 500]
        res = subprocess.run(["atos", "-o", dwarf, "-arch", "arm64e", "-textExecAddress", hex(text_exec)]
                             + [hex(a) for a in chunk], capture_output=True, text=True).stdout.splitlines()
        for a, line in zip(chunk, res):
            name = (line or "").split(" (in ")[0].strip()
            out[a] = name if name and not name.startswith("0x") else None
    return out


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--out", help="write the report here (default: TRACE minus .trace + .kernel.txt)")
    ap.add_argument("--collapsed", nargs="?", const="", metavar="FILE",
                    help="also write folded stacks for flame-graph viewers (default: TRACE minus .trace + .collapsed.txt)")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        xml = os.path.join(tmp, "tp.xml")
        export_time_profile(find_xctrace(), args.trace, xml)
        rows, kernel = load_rows(xml)
    if not kernel:
        sys.exit("no kernel frames in the trace (record with kernel callstacks enabled)")
    kbin, kuuid = kernel
    dwarf = find_dsym(kbin, kuuid)
    syms = static_symbols(dwarf)
    slide, agree, named = find_slide(rows, kbin, syms)
    text_exec = text_exec_vmaddr(dwarf) + slide

    unnamed = {addr for _, _, _, fr in rows for name, addr, b in fr if b == kbin and name.startswith("0x")}
    resolved = atos(dwarf, text_exec, unnamed)

    def label(name, addr, b):
        if b == kbin and name.startswith("0x") and resolved.get(addr):
            name = resolved[addr]
        short = b.replace("com.apple.filesystems.", "").replace("kernel.release.", "kernel.")
        return f"{name} [{short}]"

    stacks = [(st, w, th, [label(*f) for f in fr]) for st, w, th, fr in rows]
    total = sum(w for _, w, _, _ in stacks)
    ms = lambda ns: f"{ns / 1e6:9.1f} ms"

    lines = [f"trace: {args.trace}",
             f"kernel: {kbin} {kuuid}",
             f"dSYM: {dwarf}",
             f"slide {hex(slide)} (agreed by {agree} of {named} named kernel functions); "
             f"__TEXT_EXEC runtime base {hex(text_exec)}",
             f"kernel frames symbolicated: {sum(1 for v in resolved.values() if v)} of {len(resolved)} "
             f"unique raw addresses",
             f"total sampled time: {ms(total).strip()}", ""]

    by_state = collections.Counter()
    by_thread = collections.Counter()
    for st, w, th, _ in stacks:
        by_state[st] += w
        by_thread[th.split(" (")[0] + " " + (re.search(r"\((0x[0-9a-f]+)\)", th) or [""])[0]] += w
    lines += ["== time by thread state"] + [f"{ms(w)}  {k}" for k, w in by_state.most_common()] + [""]
    lines += ["== time by thread"] + [f"{ms(w)}  {k}" for k, w in by_thread.most_common(12)] + [""]

    self_t = collections.Counter()
    incl = collections.Counter()
    for _, w, _, fr in stacks:
        if fr:
            self_t[fr[0]] += w
        for f in set(fr):
            incl[f] += w
    lines += [f"== top {args.top} functions by self time"]
    lines += [f"{ms(w)} {100 * w / total:5.1f}%  {f}" for f, w in self_t.most_common(args.top)] + [""]
    lines += [f"== top {args.top} functions by inclusive time"]
    lines += [f"{ms(w)} {100 * w / total:5.1f}%  {f}" for f, w in incl.most_common(args.top)] + [""]

    heavy = collections.Counter()
    for _, w, _, fr in stacks:
        heavy[tuple(fr[:12])] += w
    lines += [f"== heaviest {min(args.top, 15)} stacks (leaf first, 12 frames)"]
    for fr, w in heavy.most_common(min(args.top, 15)):
        lines += [f"{ms(w)} {100 * w / total:5.1f}%"] + [f"      {f}" for f in fr] + [""]

    base = re.sub(r"\.trace/?$", "", args.trace.rstrip("/"))
    out = args.out or base + ".kernel.txt"
    with open(out, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print("\n".join(lines[:6]))
    print(f"report: {out}")

    if args.collapsed is not None:
        # Folded stacks: "thread;root;...;leaf weight", one line per distinct stack, weight in µs.
        # speedscope.app and profiler.firefox.com both open this format.
        folded = collections.Counter()
        for _, w, th, fr in stacks:
            thread = th.split(" (")[0] + " " + ((re.search(r"\((0x[0-9a-f]+)\)", th) or [""])[0])
            folded[";".join(f.replace(";", ":") for f in [thread.strip()] + fr[::-1])] += w
        cpath = args.collapsed or base + ".collapsed.txt"
        with open(cpath, "w") as fh:
            for stack, w in folded.most_common():
                fh.write(f"{stack} {max(1, round(w / 1000))}\n")
        print(f"collapsed stacks: {cpath} (open in https://www.speedscope.app or https://profiler.firefox.com)")


if __name__ == "__main__":
    main()
