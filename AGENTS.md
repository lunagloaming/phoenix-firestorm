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
  - Main sets `BackgroundYieldTime 0` and `TextureDiscardBackgroundedTime 0`, so a run behaves the same whether or
    not the window has focus
- **Unattended runs** (only with the tester's go-ahead; see above):
  - `multibox.sh main --autologin --set QuitAfterSeconds 780` logs in with the password saved in the test profile
    and lands at the last location.
  - After 13 minutes of rendering, `QuitAfterSeconds` logs out properly. `SIGTERM` also exits gracefully, but sends
    no logout to the sim.
  - Wait about 90 s between runs.
  - Copy the log after each run, because the next launch replaces it.
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
- **Agent test copy.** When the tester wants their own viewer uninterrupted, test builds go into a second copy of the
  install with its own user-data folder (and its own copy of the cache), launched by its own `multibox.sh`. Only
  install there; update the tester's install only when asked. Unattended runs log in an account the tester stays
  off during tests. To tell the test viewer from theirs, check `FIRESTORM_X64_USER_DIR` in `/proc/<pid>/environ`,
  not the newest process.
- **Builds.** Run them at normal priority. A change to a widely included header (`llface.h`,
  `llspatialpartition.h`, which `pipeline.h` pulls in) recompiles ~180 files; a `.cpp`-only change takes a few.
  Packaging adds several minutes to every build.
- **Reading `RebuildTrace`:** each minute logs rebuilt bytes by kind of object and what the groups hold, octree
  moves and their reasons, colour changes by alpha class (and how many were in place), the 20 groups that rebuilt
  most, and the call stacks that queued group and drawable rebuilds. Stacks are module offsets:
  - `addr2line -f -C -e <installed bin/do-not-directly-run-firestorm-bin> <offset - 1>` gives function names
    quickly.
  - The build tree's unstripped `build-linux-x86_64/newview/firestorm-bin` (same build ID) also gives
    `-i` file:line, but takes minutes per lookup.
  - A group queued from `LLVOVolume::updateGeometry` only passes on a drawable request; the drawable stacks show
    the real cause.
- **Visual check for colour/alpha changes:** a box with this script, on the ground and worn. Each step says in
  local chat what should be visible. Compare with `FSInPlaceColorUpdates` off.
  ```lsl
  list STEPS = ["opaque red", <1,0,0>, 1.0, "HIDDEN", <1,0,0>, 0.0, "opaque red again", <1,0,0>, 1.0,
      "HIDDEN", <1,0,0>, 0.0, "opaque GREEN", <0,1,0>, 1.0, "half see-through blue", <0,0,1>, 0.5,
      "HIDDEN", <0,0,1>, 0.0, "half see-through blue again", <0,0,1>, 0.5,
      "HIDDEN (turning yellow unseen)", <1,1,0>, 0.0, "opaque YELLOW", <1,1,0>, 1.0];
  integer step;
  default
  {
      state_entry() { step = 0; llSetTimerEvent(2.0); }
      attach(key id) { step = 0; }
      timer()
      {
          llSetLinkPrimitiveParamsFast(LINK_SET, [PRIM_COLOR, ALL_SIDES,
              llList2Vector(STEPS, step * 3 + 1), llList2Float(STEPS, step * 3 + 2)]);
          llOwnerSay((string)(step + 1) + ": " + llList2String(STEPS, step * 3));
          step = (step + 1) % (llGetListLength(STEPS) / 3);
      }
  }
  ```
- **Visual check for a show after a group rebuild:** a face hidden in place, a rebuild of its group while it is
  hidden (here forced by making another face half see-through), then the hidden face shown again. Before
  `33b5b80ab7` the shown face stayed a hole (first seen as holes in a card-game HUD). Use a plain cube; face 0 is
  the top and face 1 a side.
  ```lsl
  list STEPS = ["all opaque red", 1.0, 1.0, "top HIDDEN", 0.0, 1.0,
      "side half see-through (forces a rebuild), top still hidden", 0.0, 0.5,
      "top shown again: red top should be VISIBLE", 1.0, 0.5, "side opaque again", 1.0, 1.0];
  integer step;
  default
  {
      state_entry() { step = 0; llSetTimerEvent(3.0); }
      timer()
      {
          llSetLinkPrimitiveParamsFast(LINK_THIS, [PRIM_COLOR, ALL_SIDES, <1,0,0>, 1.0,
              PRIM_COLOR, 0, <1,0,0>, llList2Float(STEPS, step * 3 + 1),
              PRIM_COLOR, 1, <1,0,0>, llList2Float(STEPS, step * 3 + 2)]);
          llOwnerSay((string)(step + 1) + ": " + llList2String(STEPS, step * 3));
          step = (step + 1) % (llGetListLength(STEPS) / 3);
      }
  }
  ```
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
| Allocator page return (`FSHeapDecayMs`), `FSVBOAllocTrace` | Linux with jemalloc only (`LL_LINUX`); no-ops on macOS. `RebuildTrace` counts work everywhere, call stacks are Linux only. |
| Octree move slack for attachments (`FSOctreeBridgeMoveSlack`), in-place colour updates (`FSInPlaceColorUpdates`) | Portable. |
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
- **Textures are not the problem.**
- **Uploads:** standalone, NVIDIA keeps a system-memory copy of buffer data uploaded with `glBufferSubData` or
  `glBufferData(data)`, but not of data written by `glCopyBufferSubData`. Uploads now go through a staging buffer,
  which is 2–4× faster, but in the viewer it only cut driver memory from 4.06 to 3.65 GB.
- **Buffer creation:** `glBufferData(NULL)` makes the driver *malloc* about 1.2× the buffer's size as a reservation,
  about 3.5 GB live in a crowd.
  - About 0.8 GB of it is never written and only became resident through jemalloc recycling recently freed pages.
    `FSHeapDecayMs` (default 0) returns freed pages at once and saves that, at no measurable fps cost.
  - The rest costs real memory. The way to shrink it is fewer buffer creations, or `glBufferStorage`.
- **Who writes upload data** (`FSVBOAllocTrace` call stacks, ~75 avatars):
  - Avatar system body meshes (`LLVOAvatar::updateMeshData`, from the shadow pass): ~9–10 GB/min of rewrites of
    existing buffers in the first minutes after arrival, fading out after about 10 minutes.
  - Object rebuilds (`rebuildGeom` → `genDrawInfo` → `getGeometryVolume`): 3–5 GB/min, always into new buffers.
    This is the create/delete churn behind the driver reservations, and it doesn't stop once the scene settles.
  - Particles: about 0.6 GB/min.
- **Which objects rebuild** (`RebuildTrace`, same setting; ~70 avatars). A whole spatial group (for an attachment,
  usually the whole attachment) is rebuilt into new buffers whenever one of its objects changes, so one busy prim
  costs its whole attachment. About 90% of it is other avatars' attachments. In a settled scene it was 0.8–2 GB/min;
  with the fixes below it is 0.3–0.7 GB/min.
  - **Flexi prims crossing octree nodes** inside their attachment's spatial bridge: every crossing removed the prim
    from one group and added it to another, rebuilding both. Fixed with `FSOctreeBridgeMoveSlack`
    (`LLSpatialGroup::updateInGroup`). Culling is unaffected because group bounds come from the objects
    (`LLViewerOctreeGroup::rebound`), not the node box.
  - **Colour changes** (`LLVOVolume::setTEColor`): every change took the full-rebuild path, alpha changes as an LOD
    change and, since SL-16861 (2022, before the PBR renderer), plain tints too. Faces now stay in place when their
    alpha class (`LLFace::fsAlphaClass`: invisible, blended, nearly opaque, opaque) doesn't change.
  - **Scripted hide/show (flipbooks):** a hidden face keeps its batch with degenerate indices
    (`LLFace::getGeometryVolume`) and is shown again in place when its batched class (`mFSBatchedAlphaClass`)
    matches. A group rebuild within 10 s of a hide keeps the face batched in its pre-hide pool
    (`LLFace::fsKeepHiddenInBatch`); without that, sibling faces of a flipbook kept dropping each other out. Debug
    views that draw invisible faces (show transparent, highlights with scripted beacons) fall back to full rebuilds
    (`LLFace::fsCanHideInPlace`). About 60% of shows now happen in place.
  - **Still rebuilding:** the other ~40% of shows; a sculpty loop (`LLVOVolume::updateTextureVirtualSize` called
    from `rebuildGeom` queues another rebuild, bursts of 11–14k requests/min); material/texture and LOD changes.
- **Measuring:** with `FSVBOAllocTrace`, each minute's log has jemalloc's live/active/dirty totals next to the
  process's anon RSS and swap. Active minus anon+swap is heap that isn't really in memory.
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
