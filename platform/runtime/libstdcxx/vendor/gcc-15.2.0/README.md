# GCC 15.2 filesystem sources

`c++17/fs_ops.cc`, `c++17/fs_dir.cc`, `filesystem/ops-common.h`, and
`filesystem/dir-common.h` are copied verbatim from the
`releases/gcc-15.2.0` tag of <https://github.com/gcc-mirror/gcc>.

The upstream files are licensed under GPLv3 with the GCC Runtime Library
Exception 3.1. The corresponding texts are in `licenses/COPYING3` and
`licenses/COPYING.RUNTIME`.

`bits/largefile-config.h` is a project-owned generated compatibility header
for the Newlib target; it is not an upstream source file.
