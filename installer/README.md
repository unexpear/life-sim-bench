# Installer and saved simulations

The installer ships one workbench and dedicated runner. Template files describe
which starter entries appear in the library; all model implementations remain in
the shared engine, so an empty install can open a template received later. Library
discovery never constructs a model. Selecting another simulation releases the
previous world before creating the next one.

## Build

Use the same MSYS2 UCRT64 compiler as the app, CMake, and Inno Setup 7. The build
script copies the compiler and dependencies using their installed package
manifests. Installed versions must match `toolchain-sources.json`. It downloads
and hash-checks the corresponding full MSYS2 source archives, then includes them
with the optional tools. The first download is about 330 MB; subsequent builds
verify and reuse the cache. Source archives include the upstream source and MSYS2
patches/build recipes, not just links. It exports template metadata from the
freshly built runner, then compiles the component installer.

Use a clean Git checkout with all distribution source committed. Every installer
contains `source/life-sim-workbench-source.zip`, created from that exact commit,
and `source/REVISION.txt`. Licenses and runtime notices are always installed,
including when the C++ tools are deselected. Do not distribute older builds that
lack these materials. The compiler package/source versions must be reviewed
together before updating the lock; package license labels alone are not a review.

```powershell
./installer/build-installer.ps1 -InnoCompiler 'C:\path\to\ISCC.exe'
```

The default build directory is `native/build-runner`; configure it with CMake
first. `-SourceCache` selects another archive cache. `-SkipBuild` is accepted for
compatibility, but an incremental build always checks freshness before packaging.
The configured source must be this checkout and the compiler must be in the
reviewed MSYS2 UCRT64 prefix. Output: `dist/LifeSimWorkbench-Setup.exe`.

The C++ component includes the matching compiler so the plugin's C++ ABI agrees
with the host. Build passes the relocated toolchain as GCC's `--sysroot` and
prepends its `bin` folder only in the child process's PATH. Compilation runs in
the source folder, avoiding absolute Unicode parent paths in linker arguments.

## Storage

An installed layout is identified by `native/workbench.install`. Starter files
live in `{app}/templates`. Saved files, imported code and configuration instead
live under `%LOCALAPPDATA%\LifeSimWorkbench`. Uninstall never removes that folder.
`LIFESIM_USER_DATA` overrides this location for isolated verification.

`.benchsim` version 1 stores a model ID or custom source/DLL paths, control values,
switches, a rulestring and an optional creature body. Custom code is copied beside
the document using relative asset paths. Save commits the document through a
temporary sibling and an atomic replacement. A failed replacement leaves the
previous document intact. Named saves outside the default folder are indexed in
`saved-locations.txt`. Reopening creates a new run; this is not a universal state
or trained-brain checkpoint. Additional source headers/resources must accompany
the saved primary source separately.

## Verification

`bench_projects` checks document validation, atomic replacement, settings,
portable assets and Unicode paths. The real hidden-window app can also exercise
Create, Build, Save, normal close and fresh-process reload:

```powershell
$env:LIFESIM_USER_DATA = "$PWD\native\build-runner\persistence-qa"
./native/build-runner/bench_ui_shot.exe --verify-saved-write
./native/build-runner/bench_ui_shot.exe --verify-saved-read
```

Use a dedicated empty folder for each write/read pair. For installed-layout
testing, copy the current `bench_ui_shot.exe` beside the installed executable;
`--verify-library 0`, `--verify-library 2` or `--verify-library 38` checks that
exactly the requested number of templates is available and no simulation was
created during startup. The test executable is not part of the installer.

`./installer/verify-installer.ps1` checks core-only, empty, selected and full installs,
license notices, the complete source ZIP and hashes of installed compiler sources,
custom compilation and restart with the system compiler removed from PATH,
template removal, and byte-for-byte preservation of user files on uninstall.
It uses a fresh folder under the build directory and refuses to replace an
already registered user installation. Keep the resulting QA logs as evidence.

## References checked

- [Inno components](https://jrsoftware.org/ishelp/topic_componentssection.htm)
- [Inno setup types](https://jrsoftware.org/ishelp/topic_typessection.htm)
- [Setup command-line options](https://jrsoftware.org/ishelp/topic_setupcmdline.htm)
- [Uninstaller options](https://jrsoftware.org/ishelp/topic_uninstcmdline.htm)
- [Windows atomic replacement](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
- [Child process environment blocks](https://learn.microsoft.com/en-us/windows/win32/procthread/changing-environment-variables)
- [GCC directory and sysroot options](https://gcc.gnu.org/onlinedocs/gcc/Directory-Options.html)

## Particle Collision Lab and third-party physics

Box2D 3.1.1 is vendored in the source tree and linked into `workbench.exe` /
`bench_run.exe`. It is not an Inno component. Staging a release must include
`native/third_party/box2d` in the corresponding-source archive so the MIT notices
and buildable sources travel with the GPL workbench. See `native/PACKS.md`.

