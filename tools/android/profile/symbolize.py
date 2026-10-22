#!/usr/bin/env python3
"""Turns a profile.txt written by the app's sampling profiler (GOW3_PROFILE, gpu/shim/gow3_profiler.cpp)
into per-thread reports.

    symbolize.py profile.txt --symbols DIR [--window N] [--thread NAME] [--top 40]

DIR holds the unstripped libraries of the build that produced the profile (CI's "symbols"
artifact: libgow3gpu.so, libgow3cpu.so, gow3-probe), matched by file name against the memory
map in the profile. Addresses in other libraries (the Vulkan driver, libc) are reported as
library+offset. Needs llvm-symbolizer (LLVM_SYMBOLIZER or on PATH).
"""
import argparse
import bisect
import collections
import os
import shutil
import struct
import subprocess
import sys


class Mapping:
    __slots__ = ("start", "end", "offset", "path")

    def __init__(self, start, end, offset, path):
        self.start, self.end, self.offset, self.path = start, end, offset, path


class Window:
    def __init__(self, index, start_ms, seconds, frames, fps):
        self.index, self.start_ms, self.seconds, self.frames, self.fps = index, start_ms, seconds, frames, fps
        self.maps = []
        self.threads = []  # (tid, name, samples, failed, hot, [(count, [addrs])])


def parse(path):
    windows = []
    window = None
    thread = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            kind = line[0]
            rest = line[2:].rstrip("\n")
            if kind == "W":
                parts = rest.split()
                window = Window(int(parts[0]), int(parts[1]), float(parts[2]), int(parts[3]), float(parts[4]))
                windows.append(window)
                thread = None
            elif kind == "M" and window is not None:
                parts = rest.split(None, 5)
                lo, hi = parts[0].split("-")
                window.maps.append(Mapping(int(lo, 16), int(hi, 16), int(parts[2], 16), parts[5] if len(parts) > 5 else ""))
            elif kind == "T" and window is not None:
                parts = rest.split()
                # The name may hold spaces (a guest thread's): the last three fields are fixed.
                thread = (int(parts[0]), " ".join(parts[1:-3]) or "?", int(parts[-3]), int(parts[-2]),
                          parts[-1] == "hot", [])
                window.threads.append(thread)
            elif kind == "S" and thread is not None:
                parts = rest.split()
                thread[5].append((int(parts[0]), [int(a, 16) for a in parts[1:]]))
    for w in windows:
        w.maps.sort(key=lambda m: m.start)
    return windows


def elf_loads(path):
    """PT_LOAD segments of a 64-bit little-endian ELF: [(p_offset, p_vaddr, p_filesz)]."""
    with open(path, "rb") as f:
        head = f.read(64)
        if head[:4] != b"\x7fELF" or head[4] != 2:
            return []
        phoff, = struct.unpack_from("<Q", head, 32)
        phentsize, phnum = struct.unpack_from("<HH", head, 54)
        f.seek(phoff)
        loads = []
        for _ in range(phnum):
            ph = f.read(phentsize)
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from("<IIQQQQ", ph, 0)
            if p_type == 1:
                loads.append((p_offset, p_vaddr, p_filesz))
        return loads


class Symbolizer:
    def __init__(self, symbol_dir):
        self.symbol_dir = symbol_dir
        self.files = {}  # basename -> (path, loads)
        if symbol_dir:
            for name in os.listdir(symbol_dir):
                full = os.path.join(symbol_dir, name)
                if os.path.isfile(full):
                    loads = elf_loads(full)
                    if loads:
                        self.files[name] = (full, loads)
        self.tool = os.environ.get("LLVM_SYMBOLIZER") or shutil.which("llvm-symbolizer")
        if not self.tool:
            for candidate in ("llvm-symbolizer-18", "llvm-symbolizer-17", "llvm-symbolizer-16"):
                self.tool = shutil.which(candidate)
                if self.tool:
                    break
        self.cache = {}

    def locate(self, maps, addr):
        starts = [m.start for m in maps]
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and maps[i].start <= addr < maps[i].end:
            return maps[i]
        return None

    def resolve(self, maps, addrs):
        """addr -> 'function (file:line)' or 'lib+0xoff' or '[jit]'."""
        pending = collections.defaultdict(list)  # symbol file -> [(addr, vaddr)]
        out = {}
        for addr in addrs:
            if addr in self.cache:
                out[addr] = self.cache[addr]
                continue
            m = self.locate(maps, addr)
            if m is None or not m.path or m.path.startswith("["):
                tag = "[jit or anonymous]" if m is None or not m.path else m.path
                out[addr] = tag
                continue
            base = os.path.basename(m.path)
            file_offset = addr - m.start + m.offset
            entry = self.files.get(base)
            if entry and self.tool:
                full, loads = entry
                vaddr = None
                for p_offset, p_vaddr, p_filesz in loads:
                    if p_offset <= file_offset < p_offset + p_filesz:
                        vaddr = file_offset - p_offset + p_vaddr
                        break
                if vaddr is not None:
                    pending[full].append((addr, vaddr))
                    continue
            out[addr] = "%s+0x%x" % (base, file_offset)
        for full, items in pending.items():
            proc = subprocess.run(
                [self.tool, "--obj=" + full, "--functions=short", "--inlining=false", "--demangle", "--basenames"],
                input="".join("0x%x\n" % v for _, v in items), capture_output=True, text=True)
            lines = proc.stdout.split("\n")
            # Each address: function line, file:line:col line, blank line.
            pos = 0
            for addr, vaddr in items:
                func = lines[pos] if pos < len(lines) else "??"
                where = lines[pos + 1] if pos + 1 < len(lines) else ""
                pos += 3
                if func in ("", "??"):
                    text = "%s+0x%x" % (os.path.basename(full), vaddr)
                else:
                    where = where.split(":")
                    text = func + (" (%s:%s)" % (where[0], where[1]) if len(where) >= 2 and where[0] not in ("", "??") else "")
                out[addr] = text
        self.cache.update(out)
        return out


def report(window, thread, symbolizer, top):
    tid, name, samples, failed, hot, stacks = thread
    if samples == 0:
        return
    addrs = set()
    for _, frames in stacks:
        addrs.update(frames)
    names = symbolizer.resolve(window.maps, addrs)
    self_counts = collections.Counter()
    incl_counts = collections.Counter()
    caller_counts = collections.Counter()
    for count, frames in stacks:
        if not frames:
            continue
        self_counts[names[frames[0]]] += count
        seen = set()
        for f in frames:
            n = names[f]
            if n not in seen:
                seen.add(n)
                incl_counts[n] += count
        if len(frames) > 1:
            caller_counts[(names[frames[0]], names[frames[1]])] += count
    print("== window %d (t=%.0fs, %.1f FPS)  thread %d %s: %d samples, %d failed, %s"
          % (window.index, window.start_ms / 1000.0, window.fps, tid, name, samples, failed, "hot" if hot else "cold"))
    print("-- self (where the thread was)")
    for n, c in self_counts.most_common(top):
        print("  %5.1f%%  %s" % (100.0 * c / samples, n))
    print("-- inclusive (on the stack)")
    for n, c in incl_counts.most_common(top):
        print("  %5.1f%%  %s" % (100.0 * c / samples, n))
    print("-- leaf <- caller")
    for (leaf, caller), c in caller_counts.most_common(top // 2):
        print("  %5.1f%%  %s <- %s" % (100.0 * c / samples, leaf, caller))
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile")
    ap.add_argument("--symbols", help="directory with the unstripped libgow3gpu.so, libgow3cpu.so, gow3-probe")
    ap.add_argument("--window", type=int, help="only this window index")
    ap.add_argument("--thread", help="only threads whose name contains this")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--min-samples", type=int, default=20, help="skip threads with fewer samples")
    args = ap.parse_args()
    windows = parse(args.profile)
    if not windows:
        print("no windows in", args.profile, file=sys.stderr)
        return 1
    symbolizer = Symbolizer(args.symbols)
    if args.symbols and not symbolizer.tool:
        print("llvm-symbolizer not found: set LLVM_SYMBOLIZER", file=sys.stderr)
    print("windows:")
    for w in windows:
        total = sum(t[2] for t in w.threads)
        print("  %d: t=%.0fs, %.1fs, %d frames, %.1f FPS, %d samples, %d threads"
              % (w.index, w.start_ms / 1000.0, w.seconds, w.frames, w.fps, total, len(w.threads)))
    print()
    for w in windows:
        if args.window is not None and w.index != args.window:
            continue
        for t in sorted(w.threads, key=lambda t: -t[2]):
            if args.thread and args.thread not in t[1]:
                continue
            if t[2] < args.min_samples:
                continue
            report(w, t, symbolizer, args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
