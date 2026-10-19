#!/usr/bin/env bash
# Пакет APK examples/triangle для android-arm64 (шаг 7 v0.2, Р64).
#
#   examples/triangle/android/build.sh [каталог]   — по умолчанию build/android-triangle
#
# Программа dpl — библиотека (`dpl build --lib`), из неё — libmain.so с
# SDL_main; SDLActivity и классы SDL3 — в classes.dex; пакет подписан
# ключом отладки. Инструменты: tools/setup/cross_android.sh и
# tools/setup/graphics_android.sh. Запуск — на устройстве с Vulkan:
#   adb install -r triangle.apk
# (ключ у каждой сборки новый: прежнюю — adb uninstall org.dpl.triangle).
# Сборки для автора — в ветку apk-builds: tools/apk/publish.sh triangle.
# Сверка первого кадра — сообщением на экране.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="${1:-$ROOT/build/android-triangle}"
DPL="${DPL:-$ROOT/build/compiler/dpl}"
NDK="${ANDROID_NDK_HOME:-/opt/android-ndk}"
SDK=/opt/android/sdk
BT="$SDK/build-tools/35.0.0"
JAR="$SDK/platforms/android-35/android.jar"
SDL=/opt/android/sdl3-3.4.18
LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
rm -rf "$OUT"
mkdir -p "$OUT/stage/lib/arm64-v8a" "$OUT/classes"
# 1. Программа dpl — статическая библиотека для цели, затем libmain.so.
(cd "$HERE/.." && "$DPL" build --lib --target android-arm64 -o "$OUT/libtriangle.a" main.dpm)
libs="$(sed -n 's|^// Компоновка хоста (android-arm64): libtriangle.a ||p' "$OUT/triangle.h")"
"$LLVM/clang" --target=aarch64-linux-android24 -shared -o "$OUT/stage/lib/arm64-v8a/libmain.so" \
    -Wl,--whole-archive "$OUT/libtriangle.a" -Wl,--no-whole-archive -Wl,--no-undefined \
    -Wl,-z,max-page-size=16384 -L"$SDL/lib" $libs
"$LLVM/llvm-strip" --strip-unneeded -o "$OUT/stage/lib/arm64-v8a/libSDL3.so" "$SDL/lib/libSDL3.so"
# 2. Классы Java SDL3 → classes.dex.
javac -nowarn -source 11 -target 11 -cp "$JAR" -d "$OUT/classes" $(find "$SDL/java" -name '*.java') 2> "$OUT/javac.log"
"$BT/d8" --release --min-api 24 --lib "$JAR" --output "$OUT/stage" $(find "$OUT/classes" -name '*.class')
# 3. Пакет: манифест, dex, библиотеки; выравнивание и подпись.
"$BT/aapt2" link -o "$OUT/base.apk" -I "$JAR" --manifest "$HERE/AndroidManifest.xml" \
    --min-sdk-version 24 --target-sdk-version 35
(cd "$OUT/stage" && zip -q -0 "$OUT/base.apk" lib/arm64-v8a/*.so && zip -q "$OUT/base.apk" classes.dex)
"$BT/zipalign" -f -P 16 4 "$OUT/base.apk" "$OUT/aligned.apk"
[ -f "$OUT/debug.keystore" ] || keytool -genkeypair -keystore "$OUT/debug.keystore" -alias dpl -keyalg RSA \
    -keysize 2048 -validity 10000 -storepass android -keypass android -dname "CN=dpl debug" > /dev/null 2>&1
"$BT/apksigner" sign --ks "$OUT/debug.keystore" --ks-pass pass:android --out "$OUT/triangle.apk" "$OUT/aligned.apk"
echo "$OUT/triangle.apk"
