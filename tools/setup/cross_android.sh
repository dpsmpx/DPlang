#!/usr/bin/env bash
# Инструменты цели android-arm64 (docs/policy/PLATFORMS.md): Android NDK
# (clang, API 24) и qemu-aarch64 для тестов. В поставку dpl они не входят
# (docs/policy/LICENSING.md, раздел 3); скачивание NDK принимает лицензию
# Android SDK — разрешено автором 10 октября 2026 (история DECISIONS.md).
# NDK кладётся в /opt/android, ссылка /opt/android-ndk — путь по умолчанию
# для `dpl build --target android-arm64` (иначе $ANDROID_NDK_HOME).
set -euo pipefail
NDK_VERSION=r30
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq qemu-user unzip curl
if [ ! -d "/opt/android/android-ndk-$NDK_VERSION" ]; then
    mkdir -p /opt/android
    curl -sSL -o /opt/android/ndk.zip "https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-linux.zip"
    unzip -q /opt/android/ndk.zip -d /opt/android
    rm /opt/android/ndk.zip
fi
ln -sfn "/opt/android/android-ndk-$NDK_VERSION" /opt/android-ndk
grep Pkg.Revision /opt/android-ndk/source.properties
qemu-aarch64 --version | head -1
