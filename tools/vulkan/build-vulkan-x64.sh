#!/bin/bash
# Cross-build a Vulkan stack (Khronos loader + Mesa lavapipe/llvmpipe, LLVM JIT) for ReactOS x64 with mingw-w64.
# Everything is built in $WORK (default ./vk-work); nothing third-party is added to the repo.
# Needs: x86_64-w64-mingw32-gcc/g++, cmake, ninja, meson (pip), mako, pyyaml, flex, bison, glslangValidator,
#        llvm-18 host tools (llvm-tblgen), git, curl.
# Result: $WORK/out/{vulkan-1.dll,vulkan_lvp.dll,lvp_icd.x86_64.json,vktest.exe}
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$PWD/vk-work}; mkdir -p "$WORK/out"; cd "$WORK"
LLVM_VER=18.1.3; MESA_VER=26.2.6; VK_TAG=v1.4.309
TG=$HERE/mingw-toolchain.cmake

[ -d llvm ] || { curl -sSL -O https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-$LLVM_VER.src.tar.xz \
  -O https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/cmake-$LLVM_VER.src.tar.xz
  tar xf llvm-$LLVM_VER.src.tar.xz && mv llvm-$LLVM_VER.src llvm; tar xf cmake-$LLVM_VER.src.tar.xz && mv cmake-$LLVM_VER.src cmake; }
if [ ! -d llvm-install ]; then
  cmake -G Ninja -S llvm -B llvm-build -DCMAKE_TOOLCHAIN_FILE=$TG -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/llvm-install \
    -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_HOST_TRIPLE=x86_64-w64-mingw32 -DLLVM_TABLEGEN=/usr/lib/llvm-18/bin/llvm-tblgen \
    -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_TOOLS=OFF -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_PLUGINS=OFF -DLLVM_ENABLE_RTTI=ON \
    -DCMAKE_C_FLAGS_RELEASE="-O2 -g0 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -g0 -DNDEBUG"
  ninja -C llvm-build install          # ~1.5 h on 4 cores
fi

[ -d headers ] || GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 --branch $VK_TAG https://github.com/KhronosGroup/Vulkan-Headers headers
[ -d loader ]  || GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 --branch $VK_TAG https://github.com/KhronosGroup/Vulkan-Loader loader
cmake -S headers -B headers-build -G Ninja -DCMAKE_INSTALL_PREFIX=$PWD/headers-install >/dev/null && cmake --install headers-build >/dev/null
cmake -S loader -B loader-build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TG -DCMAKE_BUILD_TYPE=Release \
  -DVULKAN_HEADERS_INSTALL_DIR=$PWD/headers-install -DUSE_MASM=OFF -DUSE_GAS=ON -DBUILD_TESTS=OFF
ninja -C loader-build
x86_64-w64-mingw32-strip -o out/vulkan-1.dll loader-build/loader/vulkan-1.dll

[ -d mesa ] || { curl -sSL https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz | tar xJ && mv mesa-$MESA_VER mesa; }
[ -d mesa-build ] || meson setup mesa-build mesa --cross-file $HERE/mesa-cross.ini --buildtype=release \
  -Dvulkan-drivers=swrast -Dgallium-drivers=llvmpipe -Dllvm=enabled -Dplatforms=windows -Dgallium-d3d12-graphics=disabled \
  -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dshared-llvm=disabled \
  -Dcmake_prefix_path=$PWD/llvm-install -Dbuild-tests=false -Dvideo-codecs= -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dzstd=disabled -Dxmlconfig=disabled -Dvulkan-layers= -Dtools=
ninja -C mesa-build
L=mesa-build/src/gallium/targets/lavapipe
x86_64-w64-mingw32-strip -o out/vulkan_lvp.dll $L/vulkan_lvp.dll
sed 's|[^"]*vulkan_lvp.dll|vulkan_lvp.dll|' $L/lvp_icd.x86_64.json > out/lvp_icd.x86_64.json
x86_64-w64-mingw32-gcc -O2 -I headers-install/include $HERE/vktest.c -o out/vktest.exe -static-libgcc
echo "done: $WORK/out"
echo "check imports against a ReactOS build:  $HERE/rosimp.py <reactos-build-dir> out/*.dll"
