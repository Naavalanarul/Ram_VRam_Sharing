#!/usr/bin/env python3
"""Fallback demo workload: allocate several GB, write it, read it back, check it.

This exists so the demo does not depend on GIMP cooperating. It runs under the
same hook DLL, through the same RemoteHeap, and tells the same story on the two
Task Manager windows -- but its allocation behaviour is entirely predictable,
which GIMP's is not.

Run it hooked:

    set PYTHONMALLOC=malloc
    meminfo_launch.exe --target "C:\\Python311\\python.exe" ^
        --mode remote --peer <B's IP> --port 9200 ^
        --budget 268435456 --page 262144 ^
        -- scripts\\fallback_workload.py --total 4G --block 256M

PYTHONMALLOC=malloc matters. CPython normally serves small objects from arenas
it obtains directly from the OS, bypassing malloc entirely; this forces
everything through the C runtime, which is what the hook intercepts. Large
bytearrays go to malloc either way, so the script would still work without it --
setting it just makes the picture unambiguous.

What to expect, with Task Manager open on both machines:

  * this process climbs to roughly the hook's --budget and then flattens,
    however large --total is;
  * memoryd on the peer climbs by roughly --total;
  * the verification pass reports no mismatches, which is what distinguishes
    working from merely quiet.
"""

import argparse
import sys
import time


def parse_size(text):
    """Accepts plain byte counts and K/M/G suffixes, so --total 4G reads naturally."""
    text = text.strip()
    multipliers = {"K": 1024, "M": 1024 ** 2, "G": 1024 ** 3}
    suffix = text[-1].upper()
    if suffix in multipliers:
        return int(float(text[:-1]) * multipliers[suffix])
    return int(text)


def human(num_bytes):
    value = float(num_bytes)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024.0 or unit == "TiB":
            return "%.1f %s" % (value, unit)
        value /= 1024.0
    return "%.1f TiB" % value


def process_resident_bytes():
    """This process's working set (Windows) or RSS (Linux). 0 when unavailable."""
    if sys.platform == "win32":
        try:
            import ctypes
            from ctypes import wintypes

            class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
                _fields_ = [
                    ("cb", wintypes.DWORD),
                    ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t),
                ]

            counters = PROCESS_MEMORY_COUNTERS()
            counters.cb = ctypes.sizeof(counters)
            handle = ctypes.windll.kernel32.GetCurrentProcess()
            if ctypes.windll.psapi.GetProcessMemoryInfo(
                handle, ctypes.byref(counters), counters.cb
            ):
                return int(counters.WorkingSetSize)
        except Exception:
            pass
        return 0

    try:
        with open("/proc/self/statm") as handle:
            resident_pages = int(handle.read().split()[1])
        return resident_pages * 4096
    except Exception:
        return 0


def pattern_byte(block_index, offset):
    """Depends on both the block and the offset, so a block swapped with another
    is caught rather than matching by coincidence."""
    return ((block_index * 7919 + offset * 31 + 11) >> 3) & 0xFF


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--total", default="4G", help="total bytes to allocate, e.g. 4G")
    parser.add_argument("--block", default="256M", help="size of each allocation, e.g. 256M")
    parser.add_argument("--stride", type=int, default=4096,
                        help="bytes between touched offsets; one per OS page by default")
    parser.add_argument("--hold", action="store_true",
                        help="stay running after the check, so Task Manager can be read")
    args = parser.parse_args()

    total = parse_size(args.total)
    block = parse_size(args.block)
    stride = max(1, args.stride)
    count = max(1, total // block)

    print("Allocating %d blocks of %s (%s total)" % (count, human(block), human(count * block)))
    print("Working set at start: %s" % human(process_resident_bytes()))
    print()

    blocks = []

    # --- allocate and write ------------------------------------------------
    started = time.time()
    for i in range(count):
        # One bytearray of this size is a single large malloc, which is exactly
        # what the hook is watching for.
        buf = bytearray(block)
        for offset in range(0, block, stride):
            buf[offset] = pattern_byte(i, offset)
        blocks.append(buf)

        print("  block %2d/%d written | this process: %s"
              % (i + 1, count, human(process_resident_bytes())))

    write_seconds = time.time() - started
    print("\nWrite pass: %.1f s (%s/s)\n"
          % (write_seconds, human(int(count * block / max(write_seconds, 0.001)))))

    # --- read back and verify ---------------------------------------------
    print("Verifying...")
    started = time.time()
    mismatches = 0
    first_mismatch = None
    for i, buf in enumerate(blocks):
        for offset in range(0, block, stride):
            if buf[offset] != pattern_byte(i, offset):
                if first_mismatch is None:
                    first_mismatch = (i, offset)
                mismatches += 1

    read_seconds = time.time() - started
    print("Read pass: %.1f s" % read_seconds)
    print("Working set at end: %s" % human(process_resident_bytes()))

    if mismatches:
        print("\nFAILED: %d bytes differ, first in block %d at offset %d"
              % (mismatches, first_mismatch[0], first_mismatch[1]))
        return 1

    print("\nOK: every checked byte of %s round-tripped intact." % human(count * block))

    if args.hold:
        print("Holding. Ctrl-C when you are done reading Task Manager.")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
