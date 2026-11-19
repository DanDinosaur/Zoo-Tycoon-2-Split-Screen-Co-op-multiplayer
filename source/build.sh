#!/bin/sh
# Builds d3d9.dll with MinGW-w64 (i686, 32-bit) - run from this folder: sh build.sh
set -e
cd "$(dirname "$0")"
mkdir -p out/obj
for f in lua/*.c; do i686-w64-mingw32-gcc -O2 -malign-double -Dl_realloc\(b,os,s\)=zt_realloc\(b,s\) -Dl_free\(b,os\)=zt_free\(b\) -Ilua -include lua/zt_alloc.h -c $f -o out/obj/$(basename $f .c).o; done
i686-w64-mingw32-gcc -O2 -malign-double -Isrc -c miniz/miniz.c -o out/obj/miniz.o
i686-w64-mingw32-g++ -O2 -malign-double -Ilua -std=c++17 -shared -static -static-libgcc -static-libstdc++ -Wall -Wno-unused-function -Wno-misleading-indentation \
  -o out/d3d9.dll src/*.cpp out/obj/*.o src/d3d9.def -lpsapi -lwinmm -Wl,--enable-stdcall-fixup -Wl,--kill-at
ls -la out/d3d9.dll
