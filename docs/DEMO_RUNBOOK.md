# Cross-device RAM sharing for GIMP — demo runbook

Laptop A runs GIMP. Laptop B lends it RAM. The proof is Task Manager on both
machines plus a correctness check that the edit really completed.

This document is the operational half. What each piece does and why is in the
source; what follows is what to type, in what order, and what to do when it
goes wrong.

---

## Before anything else: the two things that can invalidate the plan

Run both of these on the exact laptop you will demo on, days ahead, not on the
morning.

### 1. Does the page-fault technique work on this machine?

```
build\tools\Release\win_veh_probe.exe
```

Exit code 0 and a list of `[ok  ]` lines means the vectored handler, the
protection transitions and decommit all behave. Anything else means stop.
The usual cause is security software intercepting exception dispatch; a
debugger attached to the process will also change the answer, so run it
standalone.

**If this fails there is no workaround.** Use a different machine.

### 2. Do GIMP's big buffers actually come through the C runtime?

Run GIMP under the hook in log mode — it redirects nothing, it only counts:

```
build\hook\meminfo_launch.exe --target "C:\Program Files\GIMP 2\bin\gimp-2.10.exe" --mode log
```

Open your large image, do an edit, quit. Then read `meminfo_hook.log`.

- **`large allocation #N: ... bytes` lines, with N large and sizes in the
  hundreds of MB** — good. The technique can see GIMP's buffers. Proceed.
- **`NOTE: not one allocation reached the threshold`** — GIMP is not getting
  its image buffers from the CRT heap this hook can see. Before giving up:
  - confirm `G_SLICE=always-malloc` is set (the launcher sets it unless you
    pass `--no-gslice`);
  - check which CRT lines appear as `found CRT ...`. GIMP's official Windows
    builds are MSYS2 and use `msvcrt.dll`. If only `ucrtbase.dll` was found,
    the hook never saw GIMP's allocator;
  - lower `--threshold` to something like `262144` and re-run, to see whether
    allocations are arriving but smaller than expected.
  - GEGL, GIMP's processing engine, manages image data as tiles through its
    own buffer layer and can swap them to disk itself. If the tiles never
    reach `malloc`, this interception point cannot reach them either.

**If this fails, switch to the fallback target now** (see the last section) and
do not spend the remaining time trying to make GIMP cooperate.

---

## Machine setup

Both laptops need the build. On the demo machines:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j8
```

### Laptop B — the one lending RAM

`memoryd.toml`:

```toml
[memory]
listen_address = "0.0.0.0"
port = 9200
# How much RAM B is lending. Size this to what B can actually spare.
total_reserved_bytes = 8589934592   # 8 GiB
page_size_bytes = 4096
control_socket = "meminfo_memory_ctl"
```

`discoveryd.toml`:

```toml
[discovery]
listen_address = "0.0.0.0"
memory_port = 9200
gpu_port = 0
control_socket = "meminfo_discovery_ctl"
memory_control_socket = "meminfo_memory_ctl"   # must match memoryd.toml
multicast_group = "239.255.73.77"
multicast_port = 9100
announce_interval_ms = 1000
peer_ttl_seconds = 10
```

`memory_control_socket` is not optional for a good demo. Without it discoveryd
announces the OS free-RAM figure, which has nothing to do with how much of the
pool is left, and a full node keeps advertising capacity it does not have.

Start, in this order:

```
build\memoryd\Release\memoryd.exe --config memoryd.toml
build\discoveryd\Release\discoveryd.exe --config discoveryd.toml
```

### Laptop A — the one running GIMP

Same two config files. Set `total_reserved_bytes` small (say 64 MiB): A is not
lending anything, and a small local pool keeps A from choosing itself.

Start `discoveryd` on A as well — that is what gives the client its peer list.

Check they can see each other:

```
build\memclient\Release\memclient_cli.exe --socket meminfo_discovery_ctl --max-local 2048
```

It should log a peer at B's address on port 9200.

---

## Venue network

**Test multicast at the venue, on the day, before your slot.** Conference and
hotel networks routinely block multicast between client devices, and this is
the single most common way a demo like this dies.

The check: start `discoveryd` on both laptops on the venue Wi-Fi, then on A run
`memclient_cli` and look for B in the peer list. If B never appears, multicast
is not getting through.

**Have all three of these ready:**

1. **An Ethernet cable between the two laptops**, with static IPs. The most
   reliable option by far, and also the fastest link, which matters for a
   demo timed in seconds.
2. **A phone hotspot** the two laptops share. Often passes multicast where
   venue Wi-Fi does not.
3. **Bypass discovery entirely.** Every tool takes `--peer <ip> --port 9200`
   and talks straight to `memoryd`, no multicast involved:
   ```
   build\hook\meminfo_launch.exe --target "...gimp-2.10.exe" --mode remote ^
       --peer 192.168.1.42 --port 9200
   ```
   Know B's IP before you start. Option 3 turns a dead demo into a working one
   in about fifteen seconds, so have the command already typed.

---

## Sizing, so the overflow happens while people are watching

Two numbers decide the pacing.

**`--budget`** is where laptop A's memory plateaus. Set it low enough that the
GIMP operation exceeds it within a few seconds. For a multi-GB image, 512 MiB
is a reasonable starting point; drop to 256 MiB if the plateau is slow to
appear.

**`--page`** is the transfer granularity, and it decides throughput. Each fault
is one synchronous request to B, so the round-trip is paid per page:

| Page size | Round-trips per GB | Rough time per GB, 1 GbE |
|-----------|--------------------|--------------------------|
| 64 KiB    | 16384              | ~30–60 s                 |
| 256 KiB   | 4096               | ~15–25 s                 |
| 1 MiB     | 1024               | ~10–15 s                 |

Measured locally: 512 MiB written through a 64 MiB budget took 4.3 s over
loopback, about 120 MiB/s. A real 1 GbE link caps around 110 MiB/s before
per-page latency, so treat loopback as the optimistic bound.

**Be realistic about the image size.** A 4 GB edit is minutes of waiting, not
seconds. Pick something in the 1–2 GB range so the whole thing finishes inside
your slot, and say out loud that the transfer is bounded by the LAN, because
someone will ask.

---

## The demo

1. **Task Manager open on both laptops before you start talking.** Details tab,
   sorted by Memory. On A, find the GIMP process. On B, find `memoryd.exe`.
   Get these on screen and sorted while you are still introducing the problem;
   fumbling with Task Manager mid-demo costs you the room.

2. **Show the starting state.** A's GIMP at its normal footprint, B's `memoryd`
   near zero.

3. **Launch GIMP hooked:**

   ```
   build\hook\meminfo_launch.exe ^
       --target "C:\Program Files\GIMP 2\bin\gimp-2.10.exe" ^
       --mode remote ^
       --peer <B's IP> --port 9200 ^
       --heap-size 8589934592 ^
       --budget 536870912 ^
       --page 262144 ^
       --log gimp_hook.log
   ```

   Using `--peer` rather than discovery on the day is deliberate: one less
   thing between you and a working demo.

4. **Open the large image and perform a real edit** — a crop, a filter, a layer
   merge. Something visibly a GIMP operation, not a synthetic benchmark.

5. **Point at the two numbers.** A's GIMP process climbs to roughly your
   budget and flattens. B's `memoryd` climbs by the size of the working set.
   That divergence is the whole demo.

6. **Prove it is correct, not just quiet.** Finish the edit and show the result
   is right — the filter applied, the image intact. Save it and reopen it if
   there is time. A process that is quietly corrupting data would also produce
   a nice flat memory graph.

7. `gimp_hook.log` has the allocation counts, redirected bytes, and fetch /
   flush / eviction totals if anyone wants the numbers.

---

## When it goes wrong

**GIMP crashes on startup.** Almost always a bitness mismatch: GIMP's Windows
builds are 64-bit, so `meminfo_hook.dll` and `meminfo_launch.exe` must be too.
Rebuild for x64. Failing that, run `--mode log` — if that is stable and
`--mode remote` is not, the problem is in the redirection path, and the
fallback below is your answer.

**GIMP is running but B's memory never moves.** Check `gimp_hook.log`. If
`init FAILED` appears, A could not reach B — wrong IP, firewall, or `memoryd`
not running. Windows Firewall prompting for `memoryd.exe` on first run is easy
to miss; allow it on the private network.

**Everything works but crawls.** Raise `--page` to `1048576` and raise
`--budget`. Confirm you are on Ethernet rather than Wi-Fi.

**Allocations are redirected but Task Manager on A still climbs.** The budget
is larger than you think, or the arena is fragmented and falling back to the
real heap. The log's `redirected` versus `passed straight through` counts tell
you which.

---

## The fallback, which you should rehearse

If GIMP misbehaves, switch instantly to a workload entirely within your
control. It is the same hook and the same DLL against a program whose
allocation behaviour is not in question:

```
build\hook\meminfo_launch.exe --target "C:\Python311\python.exe" ^
    --mode remote --peer <B's IP> --port 9200 ^
    --budget 268435456 --page 262144 ^
    -- scripts\fallback_workload.py --total 4G --block 256M
```

Set `PYTHONMALLOC=malloc` first so CPython's own arena allocator does not hide
the allocations behind `mmap`:

```
set PYTHONMALLOC=malloc
```

The script allocates several GB in large blocks, writes a verifiable pattern
across every page, reads it back and checks it, printing its own working set as
it goes. Same two Task Manager windows, same story, and the verification step
makes the honesty of the claim explicit.

**Rehearse this.** Run it end to end at least once on the demo machines so that
switching to it is a matter of pressing up-arrow, not improvising.

---

## What this does not do

Say this yourself before you are asked.

- It is scoped to one application on one operating system. It is not a general
  Windows memory extender.
- Allocations below the threshold are untouched. Only large buffers are
  offloaded, because small short-lived ones are not worth a network fault.
- There is no redundancy. If laptop B disappears mid-edit, the pages on it are
  gone and GIMP will fail on the next access to them.
- Throughput is bounded by the LAN. This buys capacity, not speed; it is worth
  it when the alternative is not being able to open the file at all.
