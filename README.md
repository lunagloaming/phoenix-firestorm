# MultiBox branch (`multibox-memory`)

**Intent:** make running two or more Firestorm instances at the same time behave well on Linux. The typical case is a main avatar and an alt in the same crowded region, and **memory comes first**.

What prompted it:
- On a 32 GB machine, each viewer reached about 10 GB within a minute of arriving in a crowded region.
- Together they pushed about 25 GB of viewer memory into zram swap, and neither viewer noticed: free memory still *looked* fine.
- GPU memory was nearly full, because each instance budgets as if it owns the whole GPU.
- The second instance (which opens the texture cache read-only) re-downloaded everything the first had already cached.

A single-process, multi-session viewer was considered and rejected. Session state is global throughout the code (hundreds of singletons and globals), so it would mean rewriting most of the viewer. These changes get the practical benefits (shared caches, coordinated memory use) across separate processes instead.

### Changes (high level)

**Phase 1: safety and memory awareness**
- **Texture cache, read-only (second) instance:**
  - It no longer shows the wrong texture when the owning instance has reused a cache slot. The same check covers the fast cache.
  - It never deletes texture files from a cache it doesn't own.
  - It re-reads the cache index after a miss, so it can use textures the owning instance cached after it started.
- **Linux memory pressure:** the kernel's memory-pressure figure (`/proc/pressure/memory`) now triggers texture downscaling. With compressed swap, free memory alone can look fine while the system thrashes.
- **Disk cache:** whole asset files are written to a temporary name and renamed into place, so an instance sharing the cache never reads a half-written file.
- **Diagnostics:** viewer and thread-pool threads are named on Linux, so `ps -L` / `top -H` show which thread is which.
- **Login screen:** a small "MultiBox: *profile* profile" notice appears when the viewer is launched with a tuning profile. Its tooltip lists the session-only `--set` overrides.

**Phase 2: instance slots**
- **Instance slots:** each viewer locks the lowest free `instance_<n>.lock` in the cache directory it uses. Slot 0 owns the shared caches; other slots use them read-only. Ownership follows the cache directory rather than the user directory, so:
  - Instances with separate user directories but one cache location no longer both write it.
  - Instances with separate cache locations each own theirs.
- **Object cache:** each secondary instance gets its own writable `objectcache_<n>`, instead of a read-only shared one that forced full object updates in every region.
- **Instance-aware defaults:** the autodetected texture GPU-memory budget and the automatic image-decode thread count are divided by the number of running instances. Explicit settings are left alone.
- **Settings on exit:** a second instance sharing the user directory no longer overwrites `settings.xml` when it exits. Changes made in Preferences are still saved when it is closed with OK.

**Outside the viewer: Main/Alt launchers**
- `main` and `alt` modes pass session-only `--set` overrides (not saved to `settings.xml`).
- The Alt profile trades quality for memory: a lower GPU budget, a lower avatar-complexity limit, fewer fully rendered avatars, lower LOD, shorter draw distance and a frame cap. Its whole process is pinned to the efficiency cores on hybrid CPUs.
- The launchers use a separate user-data directory (`FIRESTORM_X64_USER_DIR`), so they don't disturb a normal Firestorm install.

### New settings

| Setting | Default | Purpose |
|---|---|---|
| `FSReadOnlyCacheRefreshSecs` | 15 | Minimum interval between index re-reads in a read-only texture cache (0 disables) |
| `FSLinuxMemPressureThreshold` | 2.0 | `/proc/pressure/memory` "some avg10" percentage treated as low memory (0 disables) |
| `FSMultiBoxProfile` | *(empty)* | Tuning profile name set by a launcher via `--set`; shown on the login screen. Not persisted. |

### Status
- Both phases build cleanly with GCC 14 on Ubuntu 24.04 (`ReleaseFS_open`).
- Cross-process lock behaviour was checked against the bundled APR.
- **First runtime test** (Main + Alt in one crowded region, about 20 minutes, against an unmodified build from the same starting cache):
  - **Working:**
    - instance slots, and Alt on the efficiency cores
    - the read-only instance picking up textures the other instance cached
    - the second instance writing its own object cache and reusing it on a return visit
  - **Memory: no improvement yet.** Main reached about 15–17 GB and Alt about 12–13 GB (RAM plus swap) in both builds, and both exceeded their GPU budgets by several GB. Finding what that memory actually is comes next.
  - **Memory pressure:** it stayed under 10% even with about 26 GB in compressed swap, so the threshold default was lowered to 2%.
- **Where the memory goes** (heap profiling with jemalloc 5.4 on Main in a crowded region, about 18 GB live after 12 minutes):
  - **Geometry, held up to four times:**
    - the viewer's mesh/prim data, about 4.5 GB
    - a system-memory copy of every vertex and index buffer that the viewer keeps after upload, about 4 GB
    - an NVIDIA driver copy of the same buffers, about 4–7 GB
    - the GPU copy itself
  - **Textures are small:** decoded images are only about 0.2 GB of RAM.
  - **The allocator isn't the problem:** jemalloc overhead is under 1 GB.
  - **What made no measurable difference to the driver's copy:** the driver's threaded optimisation (`__GL_THREADED_OPTIMIZATIONS=0`), and the `GL_STATIC_DRAW` usage hint.
  - **Graphics settings barely change the totals:** Alt, with much lower settings, used about 12 GB.
- **Fixed:** avatar bake morph masks were uploaded as GL textures that nothing ever draws with, and one code path leaked them. The upload is skipped now, which saves about 1 GB of driver memory in a crowd, as well as GPU memory.
- **Linux wrapper:**
  - `FS_JEMALLOC_LIB` selects another jemalloc, for example a system build with profiling.
  - `__GL_THREADED_OPTIMIZATIONS` set by the caller is respected.
- Expected once, on first launch against caches written by an official build: the texture cache is purged, because this build decodes with OpenJPEG instead of KDU.

### Building this branch on Linux
`doc/building_linux.md` still describes Ubuntu 22.04 with GCC 11, but current code needs **GCC 14**, the toolchain the official builds now use on Ubuntu 24.04. With an Ubuntu 24.04 build environment, `gcc-14`/`g++-14` installed and `CC=gcc-14 CXX=g++-14`:

```
autobuild configure -A 64 -c ReleaseFS_open -- --ninja --jobs 10 --chan=MultiBox -DLL_TESTS:BOOL=FALSE
autobuild build     -A 64 -c ReleaseFS_open --no-configure -- --ninja --jobs 10 --chan=MultiBox -DLL_TESTS:BOOL=FALSE
```

### Not done (possible future work)
- Per-thread-pool placement on performance or efficiency cores (beyond the launcher's whole-process pinning for Alt).
- Idle scheduling priority for housekeeping threads such as cache purging.
- Multiple writers to the shared texture cache index. Its fixed-slot format is inherently single-writer.

Changes in the source are marked `<FS:Luna Witch>`.

---

<img align="left" width="100" height="100" src="doc/firestorm_256.png" alt="Logo of Firestorm viewer"/>

**[Firestorm](https://www.firestormviewer.org) is a free client for 3D virtual worlds such as Second Life and various OpenSim worlds where users can create, connect and chat with others from around the world.**

This repository contains the official source code for the Firestorm viewer.

## Open Source

Firestorm is a third party viewer derived from the official [Second Life](https://github.com/secondlife/viewer) client. The client codebase has been open source since 2007 and is available under the LGPL license.

## Download

Pre-built versions of the viewer releases for Windows, Mac and Linux can be downloaded from the [official website](https://www.firestormviewer.org/choose-your-platform/).

## Build Instructions

Build instructions for each operating system can be found using the links below and in the official [wiki](https://wiki.firestormviewer.org).

- [Windows](doc/building_windows.md)
- [Mac](doc/building_macos.md)
- [Linux](doc/building_linux.md)

> [!NOTE]
> We do not provide support for compiling the viewer or issues resulting from using a self-compiled viewer. However, there is a self-compilers group within Second Life that can be joined to ask questions related to compiling the viewer: [Firestorm Self Compilers](https://tinyurl.com/firestorm-self-compilers)

## Contribute

Help make Firestorm better! You can get involved with improvements by filing bugs and suggesting enhancements via [JIRA](https://jira.firestormviewer.org) or [creating pull requests](CONTRIBUTING.md).

## Community respect

This section is guided by the [TPV Policy](https://secondlife.com/corporate/third-party-viewers) and the [Second Life Code of Conduct](https://github.com/secondlife/viewer?tab=coc-ov-file).

Firestorm code is made available during ongoing development, with the **master** branch representing the current nightly build. Developers and self-compilers are encouraged to work on their own forks and contribute back via pull requests, as detailed in the [contributing guide](CONTRIBUTING.md).

If you intend to use our code for your own viewer beyond personal use, please only use code from official release branches (for example, `Firestorm_7.1.13`), rather than from pre-release/preview or nightly builds.
