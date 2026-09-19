# Firestorm viewer — working notes

Fork of [FirestormViewer/phoenix-firestorm](https://github.com/FirestormViewer/phoenix-firestorm). `origin` is this fork, `upstream` is the real project. Check `git rev-list --left-right --count upstream/master...master` before assuming local history has diverged — it may not have.

## Contributing (per CONTRIBUTING.md)

Firestorm's PR guidelines impose requirements beyond normal good practice — follow these when writing commits/PRs in this repo, not just the general CLAUDE.md/system conventions:

- **Commit/PR title**: reference the JIRA issue in the header, e.g. `[FIRE-12345] prevent click detaching head`. If there's no JIRA issue (e.g. this is exploratory/local-only work like the podman branch), it's fine to omit — the requirement is for real contributions headed upstream.
- **Preserve upstream (LL) code in a comment when modifying it.** This is the one that's easy to forget and not standard practice elsewhere: when changing code that Linden Lab maintains upstream (i.e. not FS-only files, not RLV code, not OpenSim-only code), comment out the original LL lines rather than deleting them, wrapped in an `<FS:LG>` block so future upstream merges can diff LL's changes against what FS did on top:
  ```c++
  // <FS:LG> [FIRE-12345] prevent click detaching head
  // int buggy_code = TRUE;
  // LL_WARN() << "This code is buggy" << LL_ENDL;
  bool fixed_code = true;
  LL_DEBUG() << "I fixed this" << LL_ENDL;
  // </FS:LG>
  ```
  For a single-line change, the shorthand trailing-comment form is acceptable:
  ```c++
  bool break_stuff = false; // </FS:LG> [FIRE-23456] don't break stuff.
  ```
  If the line being changed is already inside an existing `<FS>` block, don't nest a new one — just keep the existing block's comments aligned with the update.
- **No `<FS>` tags needed** for FS-created files, RLV-specific code, or OpenSim-only code — the preservation requirement is specifically about being able to diff against upstream LL changes.
- **PR description**: explain why the change is needed (not just what), and include testing steps/environment. If a JIRA ticket already covers the "why," a short summary is enough — no need to duplicate it.
- **New features**: suggest what should be added to the FS Wiki so users understand the feature.

## Linux build environment

`doc/building_linux.md` is stale: it describes Ubuntu 22.04 LTS + GCC 11. The **actual** current target (per `.github/workflows/build_viewer.yml` and https://www.firestormviewer.org/linux-build-environment-update/, July 2026) is **Ubuntu 24.04.4 LTS + GCC 14 + glibc 2.39**, following GitHub's runner deprecation. Don't trust the doc's package list or compiler version over the workflow file — `build_viewer.yml`'s "Install required Ubuntu packages" and "Install GCC 14.1" steps are the source of truth.

### Podman build environment

`podman/Containerfile`, `create_podman.sh`, and `build_podman.sh` reproduce that CI environment locally instead of installing GCC 14 etc. directly on the host:

- `./create_podman.sh` — builds the image (if needed) and drops you into a shell, or runs an arbitrary command (`./create_podman.sh -- <cmd>`).
- `./build_podman.sh [config] [extra autobuild switches]` — thin wrapper that just calls `create_podman.sh -- autobuild build -A 64 -c <config> -- --no-package --chan=$FS_CHANNEL ...`. Defaults to `ReleaseFS_open` and channel `todo` (override with `FS_CHANNEL=whatever` or a trailing `--chan=whatever`). Use this one day-to-day; you don't need to remember the autobuild invocation.
- `--no-package` skips the slow tar.xz packaging step (`viewer_manifest.py` compressing the whole app tree). The runnable app is still staged unpacked at `build-linux-x86_64/newview/packaged/firestorm` regardless — run that directly instead of unpacking a tarball. Pass `--package` as an extra arg to build the installer tarball when actually needed.
- The repo and a sibling `~/src/fs-build-variables` checkout (auto-cloned on first run) are bind-mounted in, so `build-linux-x86_64/` appears directly in the working tree — no copying artifacts out of the container.
- **Do not add `--userns=keep-id`** to the `podman run` invocation. The image's container process runs as root (no `USER` in the Containerfile), and rootless Podman's default user namespace already maps container root → your host UID, so bind-mounted files come out correctly owned. Adding `--userns=keep-id` anyway forces Podman to create a slow ID-mapped copy of every image layer on first run (multi-minute stall on a ~1GB image) for no benefit — this cost real debugging time to trace back to that flag.
- `autobuild build` actually reconfigures from scratch on *every* invocation (not just when unconfigured — checked the autobuild source, `autobuild_tool_build.py` calls `_configure_a_configuration` unconditionally unless `--no-configure` is passed). This is why channel naming (`--chan`) has to be passed on every build rather than set once: each podman container also gets a fresh random hostname, so `configure_firestorm.sh`'s hostname-based default channel changes on every single build unless overridden.
- Incremental builds genuinely work: rerunning only recompiles `llversioninfo.cpp.o` and `llappviewerlinux.cpp.o` (they embed the build ID, so they always touch) plus relink/repackage — verified by object-file mtimes and the `CONFIG: false` line in `build-linux-x86_64/logs/build_linux.log`.

### Branching: keep podman tooling out of upstream PRs

This `podman` branch (this file included) must never reach `upstream` — it's local-only dev tooling, not a contribution. Feature branches still need it for local builds though, so:

- Branch feature work off `podman`, not `master`: `git checkout -b feature/<name> podman`. This gives the working tree the build scripts immediately.
- Commit feature work on top as normal. Do **not** push `feature/<name>` itself to `origin`/PR it upstream — it carries the podman-tooling commits in its history.
- Before opening a PR, cut a clean branch that replays only the feature commits onto current `master`, dropping the podman commits entirely:
  ```
  git rebase --onto master podman feature/<name> -b feature/<name>-upstream
  git push origin feature/<name>-upstream
  ```
  Open the PR from `feature/<name>-upstream`. Keep developing on `feature/<name>` locally; only the rebased branch ever faces upstream.
- If `podman` itself gains new commits after a feature branch was cut, `git rebase --onto master podman feature/<name>` still works the same way — it only cares about the commit range, not which specific commits are in `podman` at rebase time.

### Expected build noise (not bugs)

- `#pragma message "Compiling with libresolv stubs"` in `indra/newview/llappviewerlinux.cpp` fires whenever glibc ≥ 2.34 (true for the Ubuntu 24.04 environment). It's an informational compiler note, not a warning — the code defines `__res_nquery`/`__dn_expand` shims because glibc 2.34 stopped exporting those internal aliases that some bundled third-party lib expects.
- `strip: ... file format not recognized` for `ca-bundle.crt`, `vk_swiftshader_icd.json`, `build_data.json` during the packaging step (`strip_binaries()` in `indra/newview/viewer_manifest.py`) is harmless — that function's exclusion list was never extended to cover non-binary files added to `bin`/`lib` later. `find -exec` doesn't abort on it; packaging completes fine.

## Fetching firestormviewer.org

The blog (firestormviewer.org) sits behind Cloudflare Turnstile, which blocks not just plain `curl` but also real headless Chromium (via Playwright) — it fingerprints automation at the browser-engine level (CDP, `navigator.webdriver`), not just TLS/headers. Don't burn time escalating (stealth patches, patched CDP, etc.) — it's not worth defeating deliberately. Ask the user to save the page's content and share that instead; `Linux build environment update - Firestorm Viewer - The Phoenix Firestorm Project Inc..txt` is a text extract of the "Linux build environment update" post obtained this way.
