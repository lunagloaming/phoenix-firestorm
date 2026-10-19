# Working on the `multibox-memory` branch

Notes for coding agents and contributors working on this fork of Firestorm. Read `README.md` first: its top
section describes the branch (intent, changes, settings, status). This file covers how to work on it: conventions,
build and test procedure, platform notes, and what is still open.

## Conventions

- **Change markers.** Mark every change to existing source the Firestorm way, with this branch's tag:
  - `// <FS:Luna Witch> why` ... `// </FS:Luna Witch>` around a block.
  - `// <FS:Luna Witch/>` at the end of a single changed line.
  - Comment out the original code inside the block instead of deleting it, so upstream merges stay easy to read.
- **Git identity.** Set it per repository, never globally:
  `git config user.name "Luna Gloaming"` and `git config user.email "43835573+lunagloaming@users.noreply.github.com"`.
  - Never put a real name, a personal email address, a local username or a machine name in commits, branch names,
    PR text or committed files.
  - The fork's remote is conventionally named `lunagloaming`; `origin` is upstream Firestorm.
- **Platforms.** This branch targets Linux first. Breaking Windows, macOS or OpenSim builds is acceptable when
  needed, but stay portable when it costs nothing:
  - put Linux-only code under `#if LL_LINUX`
  - keep Apple-GPU code paths (`gGLManager.mIsApple`) untouched
- **Upstream files.** Prefer notes in `README.md` or here over rewriting upstream docs such as
  `doc/building_*.md`, which would conflict on every merge from Firestorm.
- **New settings** go in `indra/newview/app_settings/settings.xml` with an `FS` prefix and a comment explaining the
  default. Pass values to `llrender`/`llcommon` code through a static that is set from `LLAppViewer::idle()`; the
  `FSVBO*` settings show the pattern.

## Working with the person testing

The tester runs the viewer in-world, often alongside other viewers or games. These rules come from things that
went wrong:

- **Ask before launching any viewer.** The tester may be in-world with another viewer, and memory measurements are
  only comparable when nothing else heavy is running. Ask before closing one, too.
- **Never touch the tester's normal Firestorm install or its user-data folder.** Test builds go in their own
  install folder, with their own user-data folder (see the platform sections).
- **Never edit a launcher or wrapper script in place while a viewer started from it is running.** bash reads
  scripts incrementally, and an in-place edit once re-ran the viewer launch line and started a phantom extra
  instance. Write to a temporary file and rename it over the original, or wait.
- **Any move or delete of cache or user-data folders must hard-abort if a viewer is running.** Use, for example,
  `if pgrep -x do-not-directly >/dev/null; then exit 1; fi` with `set -e`, not a check chained with `;`. Swapping
  a cache under a live viewer once produced garbage textures and a corrupted texture cache.
- **A second instance deletes its own log on a clean exit** (`logs/dump_logs/<uuid>.log`). Copy it before closing
  that instance if you need it.
- **Keep theory and side discussions out of plans and committed docs** unless asked to record them.

## Building and testing on Linux

- **Toolchain.** Current code needs **GCC 14** (Ubuntu 24.04). `doc/building_linux.md` still says 22.04 and
  GCC 11, which fails. A rootless Ubuntu 24.04 chroot works well on non-Ubuntu hosts.
- **Build:**
  ```
  autobuild configure -A 64 -c ReleaseFS_open -- --ninja --jobs 10 --chan=MultiBox -DLL_TESTS:BOOL=FALSE
  autobuild build     -A 64 -c ReleaseFS_open --no-configure -- --ninja --jobs 10 --chan=MultiBox -DLL_TESTS:BOOL=FALSE
  ```
  The output goes to `build-linux-x86_64/newview/packaged/`.
- **Install for testing:** copy the packaged tree into a separate folder outside `/opt`, e.g.
  `rsync -rptgoDLK --exclude='bin/.debug' build-linux-x86_64/newview/packaged/ <test-install>/`.
  - Then `chmod 755 <test-install>/bin/{dullahan_host,chrome-sandbox,SLVoice}`. The package can leave them at 644,
    which silently breaks the web login page, profiles, media and voice.
- **Launchers.** A small `multibox.sh {main|alt}` beside the install:
  - sets `FIRESTORM_X64_USER_DIR` to a separate user-data folder
  - passes session-only `--set` overrides (Alt: lower budgets and LOD, frame cap)
  - pins Alt to the efficiency cores with `taskset` when `/sys/devices/cpu_atom/cpus` exists
  - optionally (`FS_MEMPROF=1`) points `FS_JEMALLOC_LIB` at a jemalloc built with profiling, and sets `MALLOC_CONF`
    for heap snapshots
- **Memory measurement.**
  - **Per-process RSS and swap:** `/proc/<pid>/smaps_rollup`, sampled every 30 s.
  - **GPU memory:** `nvidia-smi`.
  - **Memory pressure:** `/proc/pressure/memory`.
  - **Where the heap goes:** jemalloc heap profiles, read with `jeprof --text --show_bytes [--cum --focus=<fn>] <binary> <snapshot>`.
  - **NVIDIA's threaded optimisation** (on by default in the wrapper) makes the driver allocate on its own threads,
    which hides callers. Run with `__GL_THREADED_OPTIMIZATIONS=0` to attribute driver allocations.
  - **Heap profiles count allocated bytes, not resident pages.** Always cross-check against RSS.
- **Standalone GL experiments** are much faster than viewer runs for driver questions. A small C program with a
  headless EGL or GLX context, plus `/proc/self/status` and jemalloc's `thread.allocatedp`/`thread.deallocatedp`,
  answered most of them.
- **Comparisons.** The process is named `do-not-directly-run-firestorm-bin`. For baseline-vs-branch runs, restore
  the same starting cache before each run, use the same region and duration, and note crowd size and settings.

## macOS: what to expect

Nothing on this branch has been built or run on macOS yet. Things to check:

| Change | macOS status |
|---|---|
| Texture-cache read-only fixes and refresh, atomic disk-cache writes, instance slots (APR file locks), per-slot object cache, settings-on-exit | Portable; should work as on Linux. |
| Login-screen profile notice | Portable (skins and XUI). |
| Linux memory pressure (PSI), thread naming, jemalloc tracing | `LL_LINUX` only. |
| Freeing VBO CPU copies (`FSVBOReleaseStagingSecs`), GPU-copy uploads (`FSVBOGPUCopyUpload`) | Skipped on Apple GPUs (`mIsApple`). **Active on Intel Macs with AMD or Intel GPUs**, where macOS GL is 4.1. Both use GL 3.1 calls, but are untested there. |
| Avatar bake-mask texture skip | Portable. |
| Launchers, wrapper, `taskset` pinning | Linux only. macOS has no CPU affinity. |

- **User-data folder: check before the first launch.** macOS has no environment override like Linux's
  `FIRESTORM_X64_USER_DIR`. The viewer always uses fixed folders under `~/Library/Application Support` and
  `~/Library/Caches` (see `indra/llfilesystem/lldir_mac.cpp`). A self-built viewer can therefore share settings
  and caches with an official Firestorm install on the same Mac. Either keep the official install's folders backed
  up and out of the way, or add an override to `lldir_mac.cpp` first, mirroring `lldir_linux.cpp`.
- **Running two copies.** macOS won't start a second instance of the same application. The tester's existing
  workaround is two copies of the app bundle under different names (e.g. `Firestorm Main.app` and
  `Firestorm Alt.app`).
  - `open -n -a <app>` forces a new instance and may be worth trying as a lighter alternative.
  - Either way, both copies use the same user-data and cache folders (see above). The branch's instance slots are
    what keep a shared cache safe: the first copy owns it, and the second uses it read-only with its own object cache.
  - Mac versions of the Main/Alt launchers would need to pass the `--set` overrides as app arguments (`open -n -a <app> --args ...`).
- **Build:** follow `doc/building_macos.md`.
- **Memory tools:** `footprint`, `vmmap --summary`, `heap`, `leaks`, and Instruments (Allocations,
  VM Tracker). Set `MallocStackLogging=1` for allocation call stacks. On Apple Silicon, GL runs on a translation
  layer over Metal, so the NVIDIA findings below don't carry over.

## Findings so far (Linux, NVIDIA)

The details and numbers are in the README Status section. In short:

- **Where a crowded region's memory goes:** geometry held several times over.
  - the viewer's mesh/prim data (`LLVolumeFace`)
  - the viewer's CPU copy of every vertex buffer (now freed after 10 s unchanged)
  - NVIDIA driver memory
  - the GPU copy itself
- **Textures and the allocator are not the problem.**
- **Uploads:** NVIDIA keeps a system-memory copy of buffer data uploaded with `glBufferSubData` or `glBufferData(data)`,
  but not of data written by `glCopyBufferSubData`. That's why uploads now go through a staging buffer, which is
  also 2–4× faster.
- **Buffer creation:** `glBufferData(NULL)` makes the driver *malloc* about the buffer's size as a reservation. It
  is untouched, so it isn't resident in a fresh process, but heap profiles count it.
  - **Open question:** whether those reservations become resident in the long-running viewer, through the allocator
    recycling resident pages. The `FSVBOAllocTrace` debug setting logs driver bytes per GL call, to answer that
    alongside RSS.
- **Not from this branch:**
  - Avatars missing all attachments until a scene reload is SL's object-update/interest-list problem.
  - A mesh door that renders broken from behind in `master` but not in the 7.2.4 release is an upstream change or
    the OpenJPEG decoder.

## Other ideas (separate from this branch, not scheduled)

- **"Fix Moblins":** a World/Advanced menu command that finds avatars with zero attachment objects (after a grace
  period, ignoring impostors and jelly dolls) and re-requests only their attachments. Never automatic. Starting
  points: `LLViewerRegion::addCacheMiss`/`requestCacheMisses` (`RequestMultipleObjects`), the orphan handling in
  `LLViewerObjectList`, and what changing draw distance triggers.
- **Pie menu layout:** move common actions such as "Render Fully" up from the fourth level.
- **Hybrid CPUs:** per-thread-pool placement on performance or efficiency cores, and idle scheduling priority for
  housekeeping threads.
