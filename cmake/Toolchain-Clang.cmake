# Optional LLVM/Clang toolchain file.
#
# Usage:
#   cmake -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Clang.cmake
#
# SPDX-License-Identifier: GPL-3.0-or-later

set(CMAKE_C_COMPILER clang CACHE FILEPATH "C compiler")
