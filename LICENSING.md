# Licensing

Life-sim Workbench's original source code, documentation and artwork are available
under **GNU GPL version 3 or, at your option, any later version**
(SPDX: GPL-3.0-or-later). Copyright 2026 Life-sim Workbench contributors, to the
extent copyright applies. The complete terms are in [LICENSE](LICENSE).

This program is distributed without any warranty, including implied warranties
of merchantability or fitness for a particular purpose. See the license for details.

Third-party material retains its own notices and terms. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [licenses](licenses).
The project license does not relicense third-party data, dependencies or user files.
Names of external games and research projects are references, not endorsements or
grants of trademark rights.

## Source with binary distributions

The installer includes the workbench's corresponding source ZIP and its exact Git
revision under `source`, plus licenses/notices. The optional compiler component
includes the **matching MSYS2 source archives**, package list, hash manifest and
original package notices under `toolchain`. The archives retain upstream source,
patches and PKGBUILD recipes; they are included offline, not replaced by links to
upstream homepages. The build refuses package versions or source hashes that do
not match the reviewed lock file.

Keep the complete installer when redistributing it. For another binary package,
include the corresponding source and notices with it or satisfy the applicable
license's other source-distribution provisions. A source tree for a different
version is not a replacement for matching source.

The GCC Runtime Library Exception applies only to the runtime files bearing it.
It does not waive the source obligations for redistributing GCC/binutils programs.
The workbench uses an ordinary GCC compilation process without nonfree GCC plugins.

## Your simulations

User-authored files remain the user's material. The license does not claim
ownership of your input data, saved worlds or ordinary simulation output.
The example plugin and workbench headers are GPL-3.0-or-later; code incorporating
them or distributed as a combined work must respect those terms. Do not assume
that a DLL boundary automatically makes a plugin exempt.

## Future packs

Fly-brain data and additional physics engines are not bundled in this release.
Each future pack needs its own code/data/asset review. In particular, FlyWire's
CC BY-NC 4.0 data must retain its conditions even when the surrounding code is
GPL and the installer costs nothing. MaleCNS has separate CC BY terms.

References: [GPLv3](https://www.gnu.org/licenses/gpl-3.0.html),
[GCC exception](https://www.gnu.org/licenses/gcc-exception-3.1.html),
[FlyWire](https://flywire.ai/guidelines).
