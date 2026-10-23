#!/bin/bash
# ============================================================
# Хук начала веб-сессии Claude Code (CLAUDE.md, «Не терять работу
# между сессиями»).
#
#   * то, что нужно tools/check/run.sh: python3 и git (зависимостей
#     у проверок пока нет);
#   * ветки с работой, не влитой в main, — свежие первыми: с них
#     продолжают, а не делают заново; apk-builds — хранилище сборок
#     (CLAUDE.md, «Сборки APK»), не работа.
#
# Только в веб-сессии; повторный запуск ничего не ставит заново.
# ============================================================
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
    exit 0
fi

PROJ="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"

MISSING=()
command -v python3 >/dev/null 2>&1 || MISSING+=(python3)
command -v git >/dev/null 2>&1 || MISSING+=(git)
if [ "${#MISSING[@]}" -gt 0 ]; then
    SUDO=""
    [ "$(id -u)" -ne 0 ] && SUDO="sudo -n"
    echo "session-start: ставлю ${MISSING[*]}"
    $SUDO apt-get update -qq
    DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq "${MISSING[@]}" >/dev/null
fi

# ---- Невлитая работа прошлых сессий ----
{
    git -C "$PROJ" fetch -q origin 2>/dev/null || true
    echo "session-start: ветки с работой, не влитой в main (свежие первыми):"
    found=0
    while read -r ref; do
        n="$(git -C "$PROJ" rev-list --count "origin/main..$ref" 2>/dev/null || echo 0)"
        [ "$n" -gt 0 ] || continue
        found=1
        echo "  ${ref#origin/}: коммитов $n, последний — $(git -C "$PROJ" log -1 --format='%cr: %s' "$ref")"
    done < <(git -C "$PROJ" for-each-ref --sort=-committerdate --format='%(refname:short)' \
                 refs/remotes/origin | grep -v -e '^origin$' -e '^origin/HEAD$' -e '^origin/main$' -e '^origin/apk-builds$' | head -8)
    [ "$found" -eq 1 ] || echo "  нет"
    echo "  Перед работой сверить с ними задачу: git log origin/main..origin/<ветка>."
} || true
