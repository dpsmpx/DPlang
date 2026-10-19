#!/usr/bin/env bash
# ============================================================
# Пакет APK — в ветку apk-builds (CLAUDE.md, «Сборки APK»).
#
#   tools/apk/publish.sh <программа> [аргументы git commit...]
#
# Собирает examples/<программа>/android/build.sh из коммита HEAD — он
# должен быть на GitHub — и кладёт пакет в ветку-сироту apk-builds под
# именем <программа>-<коммит>.apk; строка — в оглавление README.md
# ветки; затем пуш. Сборка идёт в отдельной копии дерева этого коммита
# (build/apk-src) своим компилятором (build/apk-compiler) — локальные
# правки и прежние сборки в пакет не попадают. Аргументы после имени
# программы уходят в git commit (например, --trailer).
# ============================================================
set -euo pipefail
# Переменные Git и dpl снаружи увели бы сборку или коммит в другое дерево.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_COMMON_DIR
unset DPL DPL_RUNTIME DPL_STDLIB DPL_BINDINGS
ROOT="$(git rev-parse --show-toplevel)"
[ -n "$ROOT" ]
BRANCH=apk-builds
REF="refs/heads/$BRANCH"
NAME="${1:?программа: tools/apk/publish.sh <программа>}"
shift
[[ "$NAME" =~ ^[a-z0-9_-]+$ ]] || { echo "имя программы: $NAME" >&2; exit 1; }
[ -f "$ROOT/examples/$NAME/android/build.sh" ] || { echo "нет examples/$NAME/android/build.sh" >&2; exit 1; }
SRC="$ROOT/build/apk-src"
CB="$ROOT/build/apk-compiler"
WT="$ROOT/build/$BRANCH"
OUT="$ROOT/build/apk/$NAME"
mkdir -p "$ROOT/build/apk"

# Коммит в имени должен найтись на GitHub и через сессию.
FULL="$(git -C "$ROOT" rev-parse HEAD)"
SHA="${FULL:0:7}"
SUBJECT="$(git -C "$ROOT" log -1 --format=%s HEAD)"
FILE="$NAME-$SHA.apk"
git -C "$ROOT" fetch -q origin
remote="$(git -C "$ROOT" branch -r --contains HEAD)"
[ -n "$remote" ] || { echo "коммита $SHA нет на GitHub: сначала запушить" >&2; exit 1; }
st="$(git -C "$ROOT" status --porcelain)"
[ -z "$st" ] || echo "локальные правки в пакет не входят: собирается $SHA" >&2

# Рабочая копия ветки — с GitHub; нет ветки (код 2) — новая сирота, но
# только если её следов нет и здесь: иначе прежние сборки потерялись бы.
git -C "$ROOT" worktree remove --force "$WT" 2>/dev/null || rm -rf "$WT"
git -C "$ROOT" worktree prune
rc=0
git -C "$ROOT" ls-remote --exit-code origin "$REF" > /dev/null || rc=$?
if [ "$rc" -eq 0 ]; then
    git -C "$ROOT" fetch -q origin "$REF"
    git -C "$ROOT" worktree add -q -B "$BRANCH" "$WT" FETCH_HEAD
elif [ "$rc" -eq 2 ]; then
    if git -C "$ROOT" rev-parse -q --verify "refs/remotes/origin/$BRANCH" > /dev/null \
        || git -C "$ROOT" rev-parse -q --verify "$REF" > /dev/null; then
        echo "$BRANCH нет на GitHub, но есть здесь — разобраться вручную" >&2
        exit 1
    fi
    git -C "$ROOT" worktree add -q --detach "$WT"
    git -C "$WT" checkout -q --orphan "$BRANCH"
    git -C "$WT" rm -rfq .
else
    echo "git ls-remote: код $rc" >&2
    exit 1
fi
[ ! -e "$WT/$FILE" ] || { echo "$FILE уже в $BRANCH" >&2; exit 1; }

# Копия дерева коммита и компилятор из неё; runtime, stdlib и привязки —
# оттуда же (без переменных dpl ищет их рядом с собой).
if [ "$(git -C "$SRC" rev-parse --show-toplevel 2>/dev/null)" = "$SRC" ]; then
    git -C "$SRC" checkout -q --force --detach "$FULL"
else
    rm -rf "$SRC"
    git -C "$ROOT" worktree prune
    git -C "$ROOT" worktree add -q --detach "$SRC" "$FULL"
fi
git -C "$SRC" clean -qffdx
if ! { cmake -S "$SRC/compiler" -B "$CB" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDPL_SANITIZE=OFF \
           -DCMAKE_CXX_COMPILER="${CXX:-g++}" && cmake --build "$CB"; } > "$ROOT/build/apk/compiler.log" 2>&1; then
    tail -20 "$ROOT/build/apk/compiler.log" >&2
    exit 1
fi
APK="$(DPL="$CB/dpl" DPL_RUNTIME="$SRC/runtime" DPL_STDLIB="$SRC/stdlib" DPL_BINDINGS="$SRC/bindings" \
    bash "$SRC/examples/$NAME/android/build.sh" "$OUT" | tail -1)"
cp "$APK" "$WT/$FILE"
SUM="$(sha256sum "$WT/$FILE" | cut -d' ' -f1)"

# Оглавление: шапка — всегда эта, строки прежних сборок сохраняются.
rows="$(grep '^| \[' "$WT/README.md" 2>/dev/null || true)"
cat > "$WT/README.md" <<'EOF'
# Сборки APK

Хранилище пакетов APK (слово автора, 19 октября 2026; `CLAUDE.md`,
«Сборки APK»). Ветка-сирота: в `main` не вливается, PR из неё нет.

- Имя — `<программа>-<коммит>.apk`: пакет собран из этого коммита — в
  отдельной копии его дерева, компилятором из неё же.
- Кладёт `tools/apk/publish.sh <программа>`; коммит должен быть на
  GitHub.
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

# Пуш: сбой сети — повтор; отказ (ветку обновили) — заново весь запуск.
for delay in 2 4 8 16 0; do
    if out="$(git -C "$WT" push -q -u origin "$REF:$REF" 2>&1)"; then
        break
    fi
    printf '%s\n' "$out" >&2
    if [ "$delay" -eq 0 ] || grep -q 'rejected' <<< "$out"; then
        echo "пуш $BRANCH не удался" >&2
        exit 1
    fi
    sleep "$delay"
done
[ "$(git -C "$ROOT" ls-remote origin "$REF" | cut -f1)" = "$(git -C "$WT" rev-parse HEAD)" ] \
    || { echo "$BRANCH на GitHub не совпала с отправленной" >&2; exit 1; }
echo "$BRANCH/$FILE $SUM"
