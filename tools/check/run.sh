#!/usr/bin/env bash
# ============================================================
# run.sh — все проверки репозитория (CLAUDE.md, «Проверки»).
#
#   ./tools/check/run.sh          — все проверки
#
# Вывод краткий: по строке на проверку; на красном — первые строки
# сбоя. Полный журнал — build/check/run.log. С появлением компилятора
# сюда добавляются его тесты.
# ============================================================
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LOG_DIR="$ROOT/build/check"
LOG="$LOG_DIR/run.log"
mkdir -p "$LOG_DIR"
: > "$LOG"

FAILED=0
TOTAL=0

step() {
    local name="$1"; shift
    TOTAL=$((TOTAL + 1))
    local out
    out="$("$@" 2>&1)"
    local rc=$?
    printf '== %s\n%s\n' "$name" "$out" >> "$LOG"
    if [ "$rc" -eq 0 ]; then
        echo "  ok   $(printf '%s\n' "$out" | tail -1)"
    else
        FAILED=$((FAILED + 1))
        echo "  FAIL $name"
        printf '%s\n' "$out" | head -20 | sed 's/^/       /'
    fi
}

# В корне — только точки входа (CLAUDE.md, «Где лежат не-кодовые файлы»).
check_root() {
    local bad=()
    while IFS= read -r f; do
        case "$f" in
            README.md|CLAUDE.md|TODO.md|LICENSE|.gitignore) ;;
            *) bad+=("$f") ;;
        esac
    done < <(git -C "$ROOT" ls-files | grep -v / )
    if [ "${#bad[@]}" -gt 0 ]; then
        echo "корень: лишние файлы: ${bad[*]}"
        return 1
    fi
    echo "корень: только точки входа"
}

echo "Проверки репозитория:"
step "секреты"         python3 "$ROOT/tools/check/check_secrets.py"
step "ссылки"          python3 "$ROOT/tools/check/check_links.py"
step "корень"          check_root

if [ "$FAILED" -eq 0 ]; then
    echo "Итог: PASS ($TOTAL/$TOTAL). Журнал: build/check/run.log"
    exit 0
fi
echo "Итог: FAIL ($FAILED из $TOTAL). Журнал: build/check/run.log"
exit 1
