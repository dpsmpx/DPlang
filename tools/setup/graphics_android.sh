#!/usr/bin/env bash
# Графика для цели android-arm64 (шаг 7 v0.2, Р64): платформа Android SDK
# и build-tools для пакета APK, SDL3 той же версии, что заголовки привязки
# (bindings/sdl3/), — сборкой NDK, и его классы Java (SDLActivity). Всё —
# в /opt/android. Скачивание принимает лицензию Android SDK — разрешено
# автором (история DECISIONS.md, 10 и 12 октября 2026). Запускать после
# cross_android.sh; нужен JDK (javac, keytool).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
SDK=/opt/android/sdk
NDK=/opt/android-ndk
REPO=https://dl.google.com/android/repository
PLATFORM=platform-35_r02.zip
PLATFORM_SHA1=0bb560a90a7a2cbd0dd8348224d518b638fe7949
TOOLS=build-tools_r35_linux.zip
TOOLS_SHA1=2cfaa0bbb2336e9ec18ed3ecea84fa2e2af607bc
SDL_VERSION=3.4.18
SDL_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
SDL_DIR=/opt/android/sdl3-$SDL_VERSION
apt-get install -y -qq cmake ninja-build curl unzip zip > /dev/null
command -v javac > /dev/null || apt-get install -y -qq openjdk-21-jdk-headless > /dev/null
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fetch() {  # архив, sha1, каталог назначения
    curl -sSL -o "$work/$1" "$REPO/$1"
    echo "$2  $work/$1" | sha1sum -c --quiet
    mkdir -p "$work/x"
    unzip -q "$work/$1" -d "$work/x"
    mkdir -p "$(dirname "$3")"
    rm -rf "$3"
    mv "$work/x/"* "$3"
    rmdir "$work/x"
}
[ -f "$SDK/platforms/android-35/android.jar" ] || fetch "$PLATFORM" "$PLATFORM_SHA1" "$SDK/platforms/android-35"
[ -x "$SDK/build-tools/35.0.0/aapt2" ] || fetch "$TOOLS" "$TOOLS_SHA1" "$SDK/build-tools/35.0.0"
if [ ! -f "$SDL_DIR/lib/libSDL3.so" ]; then
    curl -sSL -o "$work/sdl.tar.gz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
    echo "$SDL_SHA256  $work/sdl.tar.gz" | sha256sum -c --quiet
    tar -xzf "$work/sdl.tar.gz" -C "$work"
    cmake -S "$work/SDL3-$SDL_VERSION" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=24 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF > "$work/cmake.log"
    cmake --build "$work/build" > "$work/build.log"
    mkdir -p "$SDL_DIR/lib" "$SDL_DIR/java"
    cp "$work/build/libSDL3.so" "$SDL_DIR/lib/"
    cp -r "$work/SDL3-$SDL_VERSION/android-project/app/src/main/java/org" "$SDL_DIR/java/"
fi
echo "android-arm64: Android SDK 35 ($SDK), SDL3 $SDL_VERSION ($SDL_DIR)"
