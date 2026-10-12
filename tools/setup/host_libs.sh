#!/usr/bin/env bash
# Библиотеки хоста для прохода bindings (tests/bindings.py): загрузчик и
# заголовки Vulkan, программный драйвер lavapipe и SDL3 той же версии,
# что заголовки привязки (bindings/sdl3/, docs/THIRD_PARTY.md). В
# поставку dpl они не входят — ставятся в окружение разработки. Ubuntu
# 24.04; облачная сессия начинается с чистого контейнера, поэтому скрипт
# нужно запускать в каждой сессии, которой нужен проход bindings целиком.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
SDL_VERSION=3.4.18
SDL_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
apt-get update -qq
apt-get install -y -qq libvulkan-dev mesa-vulkan-drivers cmake ninja-build curl
if ! pkg-config --exact-version="$SDL_VERSION" sdl3 2>/dev/null; then
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT
    curl -sSL -o "$work/sdl.tar.gz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
    echo "$SDL_SHA256  $work/sdl.tar.gz" | sha256sum -c --quiet
    tar -xzf "$work/sdl.tar.gz" -C "$work"
    # Без X11 и Wayland: видео offscreen и dummy, Vulkan — через
    # VK_EXT_headless_surface.
    cmake -S "$work/SDL3-$SDL_VERSION" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF > "$work/cmake.log"
    cmake --build "$work/build" > "$work/build.log"
    cmake --install "$work/build" > "$work/install.log"
    ldconfig
fi
echo "bindings: Vulkan $(pkg-config --modversion vulkan), lavapipe, SDL3 $(pkg-config --modversion sdl3)"
