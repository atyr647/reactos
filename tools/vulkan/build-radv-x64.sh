#!/bin/bash
# Cross-build RADV (Mesa's Vulkan driver for AMD GPUs) for ReactOS x64 with its real amdgpu winsys: it talks to the
# Linux amdgpu driver that the LinuxKPI host runs in the kernel, through libdrm-ros (libdrm over \\.\LKPIDRM<n>).
# Everything is built in $WORK (default ./radv-work); nothing third-party is added to the repo.
# Needs: x86_64-w64-mingw32-gcc/g++, meson, ninja, pkg-config, python3 with mako and pyyaml, glslangValidator,
#        git, curl. No LLVM: RADV compiles shaders with ACO.
# Result: $WORK/out/{vulkan_radeon.dll, radeon_icd.x86_64.json, amdgpu.ids, radvtest.exe, libdrmtest.exe,
#         sharetest.exe}. The loader
#         (vulkan-1.dll, with loader-win64.patch) comes from build-vulkan-x64.sh; copy it next to these.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$PWD/radv-work}; mkdir -p "$WORK/out"; WORK=$(cd "$WORK" && pwd); cd "$WORK"
MESA_VER=26.2.6
MESA_SHA=b9436b3e1a8be76fbd4823dc884f90596ab7c6ac5e1867a305e66ffccfb31a92
VK_TAG=v1.4.309
DRM=$WORK/drm

# libdrm for ReactOS (static libraries, headers, pkg-config files)
"$HERE/libdrm-ros/build.sh" "$DRM"

if [ ! -d mesa ]; then
  curl -fsSL --retry 3 -o mesa.tar.xz "https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz"
  echo "$MESA_SHA  mesa.tar.xz" | sha256sum -c --quiet -
  mkdir mesa && tar -C mesa --strip-components=1 -xJf mesa.tar.xz
fi
python3 "$HERE/patch-mesa-radv.py" mesa

# libdrm-ros's POSIX compat header goes into every Mesa file (O_CLOEXEC, fcntl, ...); -static keeps winpthread
# (libdrm_amdgpu's mutexes, clock_gettime) out of a DLL that ReactOS does not have
cat > radv-cross.ini <<EOF
[binaries]
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-I$DRM/include/libdrm', '-include', '$DRM/include/libdrm/ros_posix.h']
cpp_args = ['-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-I$DRM/include/libdrm', '-include', '$DRM/include/libdrm/ros_posix.h']
c_link_args = ['-static-libgcc', '-static']
cpp_link_args = ['-static-libgcc', '-static-libstdc++', '-static']
EOF
# only the libdrm-ros pkg-config files: the build machine's own (e.g. its SPIRV-Tools) must not be found
export PKG_CONFIG_LIBDIR=$DRM/lib/pkgconfig
[ -d build-radv ] || meson setup build-radv mesa --cross-file "$HERE/mesa-cross.ini" --cross-file radv-cross.ini \
  --buildtype=release -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms=windows -Dllvm=disabled \
  -Damd-use-llvm=false -Dwindows-libdrm=true -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled \
  -Dgbm=disabled -Dglx=disabled -Dbuild-tests=false -Dvideo-codecs= -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dzstd=disabled -Dzlib=disabled -Dxmlconfig=disabled -Dvulkan-layers= -Dtools=
ninja -C build-radv
D=build-radv/src/amd/vulkan
x86_64-w64-mingw32-strip -o out/vulkan_radeon.dll $D/vulkan_radeon.dll
sed 's|[^"]*vulkan_radeon.dll|vulkan_radeon.dll|' $D/radeon_icd.x86_64.json > out/radeon_icd.x86_64.json
cp "$DRM/lib/amdgpu.ids" out/      # GPU marketing names, read from next to vulkan_radeon.dll
if x86_64-w64-mingw32-objdump -p out/vulkan_radeon.dll | grep -qi "DLL Name: libwinpthread"; then
  echo "vulkan_radeon.dll imports libwinpthread-1.dll (not linked statically)"; exit 1
fi

# tests
[ -d headers ] || GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 --branch $VK_TAG https://github.com/KhronosGroup/Vulkan-Headers headers
x86_64-w64-mingw32-gcc -O1 -Wall -I headers/include "$HERE/radvtest.c" -o out/radvtest.exe
x86_64-w64-mingw32-gcc -O1 -Wall -static -include ros_posix.h -I"$DRM/include/libdrm" "$HERE/libdrmtest.c" \
  -L"$DRM/lib" -ldrm_amdgpu -ldrm -lpthread -o out/libdrmtest.exe
x86_64-w64-mingw32-gcc -O1 -Wall -static -include ros_posix.h -I"$DRM/include/libdrm" "$HERE/sharetest.c" \
  -L"$DRM/lib" -ldrm -lpthread -o out/sharetest.exe
echo "done: $WORK/out (add vulkan-1.dll from build-vulkan-x64.sh)"
echo "check imports against a ReactOS build:  $HERE/rosimp.py <reactos-build-dir> $WORK/out/*.dll $WORK/out/*.exe"
