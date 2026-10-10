#!/usr/bin/env bash
# ============================================================
# run.sh — все проверки репозитория (CLAUDE.md, «Проверки»).
#
#   ./tools/check/run.sh          — все проверки
#
# Вывод краткий: по строке на проверку; на красном — первые строки
# сбоя. Полный журнал — build/check/run.log. Эталоны набора задач —
# bench/run_refs.py; решения на вариантах синтаксиса —
# bench/syntax/run_variants.py; компилятор и его тесты — tests/run.py
# (docs/compiler/README.md); обвязка опыта — bench/experiment.py
# selftest, после компилятора: ей нужен dpl.
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

# Скрипты разбираются без ошибок: bash -n и компиляция Python.
check_scripts() {
    local n=0 f
    while IFS= read -r f; do
        case "$f" in
            *.sh) bash -n "$ROOT/$f" || return 1 ;;
            *.py) python3 -m py_compile "$ROOT/$f" || return 1 ;;
            *) continue ;;
        esac
        n=$((n + 1))
    done < <(git -C "$ROOT" ls-files -- '*.sh' '*.py')
    echo "скрипты: разбираются ($n)"
}

echo "Проверки репозитория:"
step "секреты"         python3 "$ROOT/tools/check/check_secrets.py"
step "ссылки"          python3 "$ROOT/tools/check/check_links.py"
step "корень"          check_root
step "скрипты"         check_scripts
step "эталоны"         python3 "$ROOT/bench/run_refs.py"
step "варианты"        python3 "$ROOT/bench/syntax/run_variants.py"
step "компилятор"      python3 "$ROOT/tests/run.py"
# Обвязке нужен собранный компилятор: язык dp опыта (bench/dp_check.py).
step "обвязка"         python3 "$ROOT/bench/experiment.py" selftest

if [ "$FAILED" -eq 0 ]; then
    echo "Итог: PASS ($TOTAL/$TOTAL). Журнал: build/check/run.log"
    exit 0
fi
echo "Итог: FAIL ($FAILED из $TOTAL). Журнал: build/check/run.log"
exit 1
