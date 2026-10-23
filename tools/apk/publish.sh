#!/usr/bin/env bash
# ============================================================
# Пакет APK — в ветку apk-builds (CLAUDE.md, «Сборки APK»).
#
#   tools/apk/publish.sh <программа> [аргументы git commit...]
#
# Собирает examples/<программа>/android/build.sh из коммита HEAD — он
# должен быть на GitHub — и кладёт пакет в ветку-сироту apk-builds под
# именем <программа>-<коммит>.apk; строка — в оглавление README.md
# ветки; затем пуш. Сборка идёт в новой копии дерева этого коммита
# (build/apk-src) компилятором, собранным в ней же, — локальные правки и
# прежние сборки в пакет не попадают. Запуски в одном дереве идут по
# одному. Аргументы после имени программы уходят в git commit
# (например, --trailer).
# ============================================================
set -euo pipefail
# Переменные Git и dpl снаружи увели бы сборку или коммит в другое дерево.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_COMMON_DIR
unset DPL DPL_RUNTIME DPL_STDLIB DPL_BINDINGS DPL_CROSS_CC DPL_AR DPL_RUNNER
ROOT="$(git rev-parse --show-toplevel)"
[ -n "$ROOT" ]
BRANCH=apk-builds
REF="refs/heads/$BRANCH"
NAME="${1:?программа: tools/apk/publish.sh <программа>}"
shift
[[ "$NAME" =~ ^[a-z0-9_-]+$ ]] || { echo "имя программы: $NAME" >&2; exit 1; }
SRC="$ROOT/build/apk-src"
WT="$ROOT/build/$BRANCH"
OUT="$ROOT/build/apk/$NAME"
mkdir -p "$ROOT/build/apk"
exec 9> "$ROOT/build/apk/.lock"
flock -n 9 || { echo "publish.sh уже идёт в этом дереве" >&2; exit 1; }

drop() {  # рабочая копия: снять блокировку прерванного запуска и удалить
    git -C "$ROOT" worktree unlock "$1" 2>/dev/null || true
    git -C "$ROOT" worktree remove -f -f "$1" 2>/dev/null || rm -rf "$1"
    git -C "$ROOT" worktree prune
}
trap 'drop "$WT"; drop "$SRC"' EXIT

# Ветки GitHub — как они есть сейчас: в свой каталог ссылок, с удалением
# исчезнувших. Коммит в имени должен быть на одной из них.
FULL="$(git -C "$ROOT" rev-parse HEAD)"
SHA="${FULL:0:7}"
SUBJECT="$(git -C "$ROOT" log -1 --format=%s HEAD)"
FILE="$NAME-$SHA.apk"
git -C "$ROOT" fetch -q --prune origin "+refs/heads/*:refs/apk-check/*"
on="$(git -C "$ROOT" for-each-ref --contains "$FULL" --format=x refs/apk-check/)"
[ -n "$on" ] || { echo "коммита $SHA нет на ветках GitHub: сначала запушить" >&2; exit 1; }
st="$(git -C "$ROOT" status --porcelain)"
[ -z "$st" ] || echo "локальные правки в пакет не входят: собирается $SHA" >&2

# Копия ветки — без своей локальной ветки. Нет ветки на GitHub — новая
# сирота, но только если её следов нет и здесь: иначе прежние сборки
# потерялись бы.
drop "$WT"
if git -C "$ROOT" rev-parse -q --verify "refs/apk-check/$BRANCH" > /dev/null; then
    git -C "$ROOT" worktree add -q --detach "$WT" "refs/apk-check/$BRANCH"
else
    if git -C "$ROOT" rev-parse -q --verify "refs/remotes/origin/$BRANCH" > /dev/null \
        || git -C "$ROOT" rev-parse -q --verify "$REF" > /dev/null; then
        echo "$BRANCH нет на GitHub, но есть здесь (refs/remotes/origin/$BRANCH или $REF):" \
            "проверить, не пропали ли сборки, и удалить ссылку вручную" >&2
        exit 1
    fi
    git -C "$ROOT" worktree add -q --detach "$WT"
    git -C "$WT" checkout -q --orphan "apk-new-$$"
    git -C "$WT" rm -rfq .
fi
[ ! -e "$WT/$FILE" ] || { echo "$FILE уже в $BRANCH" >&2; exit 1; }

# Новая копия дерева коммита и компилятор в ней (build/compiler): dpl
# берёт runtime, stdlib и привязки рядом с собой — из той же копии.
drop "$SRC"
git -C "$ROOT" worktree add -q --detach "$SRC" "$FULL"
[ -f "$SRC/examples/$NAME/android/build.sh" ] || { echo "в $SHA нет examples/$NAME/android/build.sh" >&2; exit 1; }
if ! { cmake -S "$SRC/compiler" -B "$SRC/build/compiler" -G Ninja -DCMAKE_BUILD_TYPE=Release \
           -DDPL_SANITIZE=OFF -DCMAKE_CXX_COMPILER="${CXX:-g++}" && cmake --build "$SRC/build/compiler"; } \
        > "$ROOT/build/apk/compiler.log" 2>&1; then
    tail -20 "$ROOT/build/apk/compiler.log" >&2
    exit 1
fi
APK="$(bash "$SRC/examples/$NAME/android/build.sh" "$OUT" | tail -1)"
cp "$APK" "$WT/$FILE"
SUM="$(sha256sum "$WT/$FILE" | cut -d' ' -f1)"

# Оглавление: шапка — всегда эта, строки прежних сборок сохраняются.
rows="$(grep '^| \[' "$WT/README.md" 2>/dev/null || true)"
cat > "$WT/README.md" <<'EOF'
# Сборки APK

Хранилище пакетов APK (слово автора, 19 октября 2026; `CLAUDE.md`,
«Сборки APK»). Ветка-сирота: в `main` не вливается, PR из неё нет.

- Имя — `<программа>-<коммит>.apk`: пакет собран из этого коммита — в
  новой копии его дерева, компилятором из неё же.
- Кладёт `tools/apk/publish.sh <программа>`; коммит в момент сборки —
  на одной из веток GitHub.
- Подпись — ключ отладки, у каждой сборки новый: поверх прежней
  установки пакет не встанет, сначала `adb uninstall <пакет>`
  (у triangle — `org.dpl.triangle`).

| файл | коммит | собран (UTC) | SHA-256 |
|---|---|---|---|
EOF
[ -z "$rows" ] || printf '%s\n' "$rows" >> "$WT/README.md"
printf '| [%s](%s) | `%s` %s | %s | `%s` |\n' "$FILE" "$FILE" "$SHA" "${SUBJECT//|/\\|}" \
    "$(date -u '+%Y-%m-%d %H:%M')" "$SUM" >> "$WT/README.md"
git -C "$WT" add "$FILE" README.md
git -C "$WT" commit -q -m "APK: $FILE" -m "Сборка $NAME из коммита $SHA ($SUBJECT)." "$@"
git -C "$WT" checkout -q --detach
git -C "$ROOT" branch -D "apk-new-$$" > /dev/null 2>&1 || true
HEAD_WT="$(git -C "$WT" rev-parse HEAD)"

# Пуш: сбой сети — повтор; отказ (ветку обновили) — заново весь запуск.
for delay in 2 4 8 16 0; do
    if out="$(git -C "$WT" push -q origin "HEAD:$REF" 2>&1)"; then
        break
    fi
    printf '%s\n' "$out" >&2
    if [ "$delay" -eq 0 ] || grep -q 'rejected' <<< "$out"; then
        echo "пуш $BRANCH не удался" >&2
        exit 1
    fi
    sleep "$delay"
done
# Сверка с GitHub; сбой сети здесь — не «не совпала».
for delay in 2 4 8 0; do
    if got="$(git -C "$ROOT" ls-remote origin "$REF")"; then
        [ "${got%%[[:space:]]*}" = "$HEAD_WT" ] && break
        echo "$BRANCH на GitHub: ${got%%[[:space:]]*}, отправлено: $HEAD_WT" >&2
        exit 1
    fi
    [ "$delay" -gt 0 ] || { echo "пуш прошёл, но сверить $BRANCH с GitHub не удалось" >&2; exit 1; }
    sleep "$delay"
done
echo "$BRANCH/$FILE $SUM"
