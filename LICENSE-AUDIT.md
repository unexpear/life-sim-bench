# Distribution license audit

Reviewed 2026-09-23. Scope: the source repository, its original generated icon
and pixel art, the desktop programs, and the offline Windows installer including
its optional compiler. User saves, local backups, downloaded research datasets
and generated build directories are excluded from the public repository.

## Findings and fixes

| Material | Verified finding | Distribution action |
| --- | --- | --- |
| Original workbench source, docs and assets | No project license existed; the owner requested a free, open project and accepted GPL. | Added GPL-3.0-or-later in LICENSE and LICENSING.md. Interactive notices are accessible through Tools → License & source. |
| Hensel neighbourhood representatives | Numeric representatives match Golly's liferules.cpp. Golly is GPL-2.0-or-later, compatible with the selected GPLv3 terms. | Credit Golly and retain its full notice/license in licenses/Golly.txt. |
| Langton transition tables | All 219 six-digit canonical transitions in the native implementation match the pinned Golly rule; all are also present in the web prototype. | Retain Langton, Sayama and Bachmutsky credits and Golly's GPL notice. No rule behavior was changed. |
| Platformer pixels | The earlier red-cap/overalls artwork lacked recorded permission and resembled a commercial game character. | Replaced the entire sprite/tile set with original geometric robot, machinery and energy-cell artwork. The earlier asset is excluded from the initial Git history and distribution. |
| Compiler and dependencies | The old installer included GPL/LGPL tools with notices and homepage links but no complete matching source. A homepage alone was inadequate for this package. | Bundle exact full MSYS2 source archives, including upstream payloads, patches and build recipes, in the same installer. Preserve package notices and exact versions. |
| Linked runtime | GCC/libstdc++ and MinGW-w64 support code are linked into the programs. | Preserve the GCC runtime exception and original runtime/header notices even when optional compiler tools are omitted. Workbench source is always included. |
| Installer engine | Unmodified Inno Setup license permits redistribution with its notices retained. | Preserve embedded notices and include licenses/Inno-Setup.txt. |
| Other assets | Icon comes from make_icon.cpp; surfaces and small sorting glyphs are drawn by project code. Windows fonts and system DLLs are used from the OS. | No external texture/font pack, commercial game sprites/music/ROMs or emulator is included. |

## Compiler source evidence

`installer/toolchain-sources.json` maps **17 exact binary packages to 15 source
archives** (some are split packages). Each downloaded archive's detached signature
was checked against the installed MSYS2 public keyring in an isolated verification
keyring. The valid signing fingerprint recorded for these archives is
`5F944B027F7FE2091985AA2EFA11531AA0AA7F57`. The lock records the archive sizes,
SHA-256 hashes and official URLs. Archive listings were inspected for upstream
payloads and PKGBUILD recipes: tarballs for release packages, Git objects for
MinGW-w64 packages, and the complete generator script for windows-default-manifest.

Packaging refuses installed package/version mismatches and archive hash changes.
It uses a fresh staging directory, requires a clean committed source checkout,
checks the configured source/compiler, runs the incremental build, and archives
that commit with Git. `source/REVISION.txt` identifies the included workbench
source. The compiler sources travel with the optional compiler component.

The installer verification script checks a core-only installation for notices and
workbench source, then hashes every compiler source archive in an installation
with tools. It also checks template choices, compilation with the system compiler
removed from PATH, saved-project reopening and preservation on uninstall.

## Scope limits and future additions

This is a source and distribution review, not proof of every historical authorship
claim or a legal opinion. The initial repository contains only the reviewed current
files; it does not import old binaries, old sprite copies or unknown Git history.
Scientific formulas, published mechanics and algorithm names are distinguished
from copied expressive code or artwork. Citations remain in simulation guides.

Fly-brain models, connectomes, Box2D, Jolt and MuJoCo/FlyGym are researched but
**not included** in this distribution. Before adding a pack, record the exact
version and review its code, data, meshes, weights and dependency terms separately.
In particular, free pricing does not remove FlyWire's noncommercial restriction.
Do not label restricted datasets as GPL or assume code licensing covers them.
See [the research brief](native/RESEARCH-NEXT.md).

## Primary sources checked

- [GNU GPLv3, including corresponding source and distribution requirements](https://www.gnu.org/licenses/gpl-3.0.html)
- [GCC Runtime Library Exception 3.1](https://www.gnu.org/licenses/gcc-exception-3.1.html)
- [MSYS2 package licensing](https://www.msys2.org/dev/package-licensing/) and [full source archives](https://repo.msys2.org/mingw/sources/)
- [Golly license at reviewed revision](https://github.com/AlephAlpha/golly/blob/2619602e564b98479735e8afaa84a7842cc76355/docs/License.html)
- [Golly Hensel representatives](https://github.com/AlephAlpha/golly/blob/2619602e564b98479735e8afaa84a7842cc76355/gollybase/liferules.cpp) and [Langton table](https://github.com/AlephAlpha/golly/blob/2619602e564b98479735e8afaa84a7842cc76355/Rules/Langtons-Loops.rule)
- [Inno Setup license](https://jrsoftware.org/files/is/license.txt)
- [US Copyright Office: games, rules and expressive material](https://www.copyright.gov/register/tx-games.html)
- [FlyWire data guidelines](https://flywire.ai/guidelines)
