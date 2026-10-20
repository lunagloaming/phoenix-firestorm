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

**Phase 3: memory footprint** (found by heap profiling; see Status)
- **Avatar bake masks:** they were uploaded as GL textures that nothing ever draws with, and one code path leaked them. The upload is skipped now, which saves about 1 GB of driver memory in a crowd, as well as GPU memory.
- **Vertex buffer CPU copies:** every vertex and index buffer used to keep a system-memory copy of its contents for its whole lifetime. The copy is now freed once a buffer has gone unchanged for 10 seconds, and is read back from the GPU if the buffer ever changes again.
- **GPU-copy uploads:** geometry is uploaded through a small staging buffer and a GPU-side copy, instead of directly. Scenes load noticeably faster. In a standalone test it also stopped the NVIDIA driver keeping its own system copy of every buffer, but in the viewer that saved only a few hundred MB.
- **Flexi prims in attachments:** a swaying flexi prim near an octree node boundary inside its attachment crossed it back and forth, and every crossing rebuilt the geometry of both nodes into new vertex buffers. One avatar's attachment rebuilt about 500 MB a minute that way. Objects in attachments, moving linksets and animesh now stay in their node until they are well outside it (`FSOctreeBridgeMoveSlack`), which cut that to about 20 MB a minute.
- **Colour changes:** every colour change (and, since a 2022 Linden fix, even a plain tint) rebuilt the object's whole group, for an attachment the whole attachment. A change that leaves a face in the same render batch (same alpha class) now updates its vertex colours in place (`FSInPlaceColorUpdates`). Hiding a face (alpha 0) keeps it in its batch with degenerate triangles, and showing it again restores it in place when the batch still fits, so scripted hide/show ("flipbook") animations no longer rebuild the whole attachment. Checked in-world with tint HUDs on mesh and flexi hair, and a test cube cycling opaque, blended and hidden.
- **Allocator page return (Linux):** the NVIDIA driver reserves heap memory the size of every vertex buffer it creates and never writes most of it. jemalloc's default of keeping freed pages for 10 seconds let those reservations land on pages that were already resident. Freed pages are now returned to the system at once (`FSHeapDecayMs`), which saves about 0.8 GB per viewer in a crowd with no measurable frame-rate cost.

**Outside the viewer: Main/Alt launchers**
- `main` and `alt` modes pass session-only `--set` overrides (not saved to `settings.xml`).
- The Alt profile trades quality for memory: a lower GPU budget, a lower avatar-complexity limit, fewer fully rendered avatars, lower LOD, shorter draw distance and a frame cap. Its whole process is pinned to the efficiency cores on hybrid CPUs.
- The launchers use a separate user-data directory (`FIRESTORM_X64_USER_DIR`), so they don't disturb a normal Firestorm install.
- **Linux wrapper (`firestorm`):**
  - `FS_JEMALLOC_LIB` selects another jemalloc, for example a system build with heap profiling.
  - `__GL_THREADED_OPTIMIZATIONS` set by the caller is respected; the default is still on.

### New settings

| Setting | Default | Purpose |
|---|---|---|
| `FSReadOnlyCacheRefreshSecs` | 15 | Minimum interval between index re-reads in a read-only texture cache (0 disables) |
| `FSLinuxMemPressureThreshold` | 2.0 | `/proc/pressure/memory` "some avg10" percentage treated as low memory (0 disables) |
| `FSVBOReleaseStagingSecs` | 10 | Free the CPU copy of a vertex/index buffer after this many seconds unchanged; read back from the GPU if it changes again (0 keeps every copy; not used on Apple GPUs) |
| `FSVBOGPUCopyUpload` | on | Upload vertex/index data through a staging buffer and a GPU copy rather than directly (not used on Apple GPUs) |
| `FSHeapDecayMs` | 0 | Linux with jemalloc: milliseconds freed pages stay with the allocator before they go back to the system (negative leaves jemalloc's own setting) |
| `FSInPlaceColorUpdates` | on | Update a face's colour or alpha in its existing vertex buffer when it stays in the same render batch, and hide or show faces in place, instead of rebuilding the object's whole group. Off restores the original behaviour |
| `FSOctreeBridgeMoveSlack` | 1 | How far, in multiples of its octree node's half size, an object in an attachment, moving linkset or animesh may move outside its node before it is moved to another one (each move rebuilds both nodes' geometry). 0 restores the original behaviour |
| `FSVBOAllocTrace` | off | Debug, not saved: log once a minute how much heap the GL driver allocates and frees inside the vertex buffer pool's GL calls (with a size histogram), jemalloc totals next to the process's RSS and swap, and the call stacks that write the most upload data (Linux with jemalloc; run with `__GL_THREADED_OPTIMIZATIONS=0`). Also logs which object groups rebuild their geometry (`RebuildTrace`): by kind of object, the groups that rebuild most, octree moves, and the call stacks that queued the rebuilds |
| `FSMultiBoxProfile` | *(empty)* | Tuning profile name set by a launcher via `--set`; shown on the login screen. Not persisted. |

### Status
Builds cleanly with GCC 14 on Ubuntu 24.04 (`ReleaseFS_open`). Linux-only testing so far, on an i7-14700K with 32 GB of RAM (zram swap) and an RTX 4090 (driver 615).

**Working (runtime-tested with Main and Alt in a crowded region):**
- instance slots, and Alt on the efficiency cores
- the read-only instance picking up textures the other instance cached
- the second instance writing its own object cache and reusing it on a return visit
- the memory-pressure trigger (with zram it rarely passes 10%, hence the 2% default)
- an evening of dogfooding with various alts and locations, with no crashes

**Memory, Main in a crowded region** (jemalloc heap profiles):

| | Start (unmodified) | Now |
|---|---|---|
| Live heap after about 12 minutes | about 18 GB | about 8.4 GB |
| Viewer's CPU copies of vertex buffers | 3.4–4.1 GB | under 0.1 GB |
| Avatar bake mask textures (driver memory) | about 1 GB | about 1 MB |
| NVIDIA driver memory, other | 4–6 GB | about 3.7 GB |
| Mesh/prim geometry (`LLVolumeFace`) | 2.7–4.5 GB | about 2.8 GB |

The runs differ in crowd size and settings, so the totals are indicative. The per-item figures are the reliable ones.

Returning freed pages at once (`FSHeapDecayMs` 0) saves a further 0.8 GB or so. That was measured with two 13-minute runs at the same spot (~75 avatars) that differed only in this setting.

**Still open:**
- **Most of the NVIDIA driver's memory remains:** about 3.5 GB that it reserves when a buffer is created. Object rebuilds keep creating new buffers, so the reservations keep churning. In a settled crowd about 1–1.5 GB of geometry a minute is still rebuilt, about 90% of it other avatars' attachments. Before the flexi and colour fixes, most of it was flexi prims crossing octree nodes and rigged attachments whose scripts hide and show faces; the effect of the hide/show change in a crowd is still to be measured. `glBufferStorage` is still untried.
- **Avatar body meshes:** for the first ten minutes or so after arriving in a crowd, the system body meshes are rewritten about 9–10 GB a minute, probably even when they're hidden under mesh bodies.
- **Mesh geometry** is now the largest item in the viewer's own memory.
- **An unattributed "operator delete[]" entry**, about 0.8 GB.
- **GPU budgets:** both instances still exceed their texture GPU-memory budgets by several GB, because the budget covers only textures.

**Known issues not caused by this branch:**
- **Expected once:** on first launch against caches written by an official build, the texture cache is purged, because this build decodes with OpenJPEG instead of KDU.
- **Avatars missing all attachments until a scene reload** (for example by dropping and restoring draw distance) is the long-standing SL object-update problem.
- **A mesh door renders broken from behind in unmodified `master` builds as well**, but not in the 7.2.4 release. It's either an upstream change or the OpenJPEG decoder.

### Building this branch on Linux
`doc/building_linux.md` still describes Ubuntu 22.04 with GCC 11, but current code needs **GCC 14**, the toolchain the official builds now use on Ubuntu 24.04. With an Ubuntu 24.04 build environment, `gcc-14`/`g++-14` installed and `CC=gcc-14 CXX=g++-14`:

```
autobuild configure -A 64 -c ReleaseFS_open -- --ninja --jobs 10 --chan=MultiBox -DLL_TESTS:BOOL=FALSE
autobuild build     -A 64 -c ReleaseFS_open --no-configure -- --ninja --jobs 10 --chan=MultiBox -DLL_TESTS:BOOL=FALSE
```

When running from the packaged tree (`build-linux-x86_64/newview/packaged/`) or a copy of it, check that `bin/dullahan_host`, `bin/chrome-sandbox` and `bin/SLVoice` are executable (`chmod 755`). A build in a rootless chroot left them at 644, which silently breaks the web login page, profiles, media and voice.

### Not done (possible future work)
- Fewer buffer creations on object rebuilds, and skipping updates to hidden avatar body meshes.
- Reducing mesh geometry held for avatars that aren't fully rendered.
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
