#!/usr/bin/env bash
# Графика для цели windows-x86_64 (шаг 7 v0.2, Р64): SDL3 той же версии,
# что заголовки привязки (bindings/sdl3/), — кросс-сборкой MinGW-w64, и
# библиотека импорта vulkan-1.dll по функциям привязки Vulkan. Ставятся в
# sysroot MinGW: программа dpl находит их по @link("SDL3") и
# @link("vulkan"). Vulkan под Wine передаётся хосту (winevulkan), так что
# нужен и tools/setup/host_libs.sh; окну Wine — виртуальный экран Xvfb.
# Запускать после cross_windows.sh.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SYSROOT=/usr/x86_64-w64-mingw32
SDL_VERSION=3.4.18
SDL_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
apt-get install -y -qq cmake ninja-build curl llvm xvfb xauth > /dev/null
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
if [ ! -f "$SYSROOT/lib/libSDL3.dll.a" ] || ! grep -q "$SDL_VERSION" "$SYSROOT/share/dpl-sdl3-version" 2>/dev/null; then
    curl -sSL -o "$work/sdl.tar.gz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
    echo "$SDL_SHA256  $work/sdl.tar.gz" | sha256sum -c --quiet
    tar -xzf "$work/sdl.tar.gz" -C "$work"
    cmake -S "$work/SDL3-$SDL_VERSION" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
        -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
        -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF > "$work/cmake.log"
    cmake --build "$work/build" > "$work/build.log"
    cmake --install "$work/build" > "$work/install.log"
    mkdir -p "$SYSROOT/share"
    echo "$SDL_VERSION" > "$SYSROOT/share/dpl-sdl3-version"
fi
# Библиотека импорта vulkan-1.dll: функции привязки — из bindings/vulkan.
{
    echo "LIBRARY vulkan-1.dll"
    echo "EXPORTS"
    grep -oE '^pub extern fn vk[A-Za-z0-9]+' "$ROOT/bindings/vulkan/vulkan.dpm" | sed 's/^pub extern fn //'
} > "$work/vulkan-1.def"
llvm-dlltool -m i386:x86-64 -d "$work/vulkan-1.def" -l "$SYSROOT/lib/libvulkan.a"
echo "windows-x86_64: SDL3 $SDL_VERSION ($SYSROOT/bin/SDL3.dll), libvulkan.a → vulkan-1.dll"
