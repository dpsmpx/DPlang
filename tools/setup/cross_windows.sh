#!/usr/bin/env bash
# Инструменты цели windows-x86_64 (docs/policy/PLATFORMS.md): clang + lld
# поверх MinGW-w64 и Wine для тестов. В поставку dpl они не входят
# (docs/policy/LICENSING.md, раздел 3) — ставятся в окружение разработки.
# Ubuntu 24.04; облачная сессия начинается с чистого контейнера, поэтому
# скрипт нужно запускать в каждой сессии, которой нужен проход cross.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq clang lld mingw-w64 wine64
WINEDEBUG=-all wineboot --init
echo "windows-x86_64: clang $(clang --version | head -1 | sed 's/.*version //'), $(wine --version)"
