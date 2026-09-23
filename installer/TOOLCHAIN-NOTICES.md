# C++ build tools

This optional component contains the installed MSYS2 UCRT64 GCC C/C++ toolchain
and its dependencies. `PACKAGES.txt` records exact package versions. Each package's
licences are included under `share/licenses`, with GCC runtime notices also
included by the upstream package. Full corresponding source archives are in
`sources`, alongside `toolchain-sources.json` with exact versions and SHA-256
hashes. They contain upstream source (including Git objects for VCS packages),
MSYS2 patches and PKGBUILD build recipes. The source packages were authenticated
using MSYS2's package signatures before their hashes were recorded.

GCC and binutils are GPL software; some dependencies are LGPL or permissively
licensed. Read each source's individual notices. The GCC Runtime Library
Exception applies to the files carrying it, not to distributing the compiler
program itself. Keep the sources, license files and package list together when
redistributing these tools. No upstream homepage is being used as a substitute
for the corresponding source included here.

Upstream projects and corresponding package build/source information:

- GCC: https://gcc.gnu.org/ and https://github.com/msys2/MINGW-packages/tree/master/mingw-w64-gcc
- GNU Binutils: https://www.gnu.org/software/binutils/
- MinGW-w64 headers, CRT and winpthreads: https://www.mingw-w64.org/
- MSYS2 package sources and patches: https://github.com/msys2/MINGW-packages
- MSYS2 package archive: https://repo.msys2.org/mingw/ucrt64/
- MSYS2 full source packages: https://repo.msys2.org/mingw/sources/

Keep the included notices with redistributed copies of the tools. Their licences
are separate from the workbench's source and from simulations you create.
