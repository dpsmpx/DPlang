#!/usr/bin/env bash
# Пакет APK examples/triangle для android-arm64 (шаг 7 v0.2, Р64).
#
#   examples/triangle/android/build.sh [каталог]   — по умолчанию build/android-triangle
#
# Программа dpl — библиотека (`dpl build --lib`), из неё — libmain.so с
# SDL_main; SDLActivity и классы SDL3 — в classes.dex; пакет подписан
# ключом отладки $DPL_APK_KEYSTORE (по умолчанию
# ~/.android/dpl-debug.keystore; нет — создаётся). Инструменты:
# tools/setup/cross_android.sh и tools/setup/graphics_android.sh. Запуск —
# на устройстве с Vulkan:
#   adb install -r triangle.apk
# (пакет с другим ключом — из другого окружения — сперва adb uninstall
# org.dpl.triangle).
# Сборка воспроизводима (docs/policy/DETERMINISM.md, раздел 6): тот же
# коммит, инструменты и ключ дают те же байты. Время записей zip —
# SOURCE_DATE_EPOCH, по умолчанию время коммита.
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
KEYSTORE="${DPL_APK_KEYSTORE:-$HOME/.android/dpl-debug.keystore}"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || echo 315532800)}"
export TZ=UTC  # время записей zip — местное: только UTC даёт те же байты
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
mapfile -d '' java < <(find "$SDL/java" -name '*.java' -print0 | sort -z)
javac -nowarn -source 11 -target 11 -cp "$JAR" -d "$OUT/classes" "${java[@]}" 2> "$OUT/javac.log"
mapfile -d '' classes < <(find "$OUT/classes" -name '*.class' -print0 | sort -z)
"$BT/d8" --release --min-api 24 --lib "$JAR" --output "$OUT/stage" "${classes[@]}"
# 3. Пакет: манифест, dex, библиотеки; выравнивание и подпись.
"$BT/aapt2" link -o "$OUT/base.apk" -I "$JAR" --manifest "$HERE/AndroidManifest.xml" \
    --min-sdk-version 24 --target-sdk-version 35
# Записи — с одним временем и без доп. полей (-X: владелец, точное время).
find "$OUT/stage" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
(cd "$OUT/stage" && zip -q -X -0 "$OUT/base.apk" lib/arm64-v8a/*.so && zip -q -X "$OUT/base.apk" classes.dex)
"$BT/zipalign" -f -P 16 4 "$OUT/base.apk" "$OUT/aligned.apk"
if [ ! -f "$KEYSTORE" ]; then
    mkdir -p "$(dirname "$KEYSTORE")"
    keytool -genkeypair -keystore "$KEYSTORE" -alias dpl -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass android -keypass android -dname "CN=dpl debug" > /dev/null 2>&1
fi
# Подпись RSA схем v2 и v3 (minSdk 24 — без v1) от данных не зависит.
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --out "$OUT/triangle.apk" "$OUT/aligned.apk"
echo "$OUT/triangle.apk"
