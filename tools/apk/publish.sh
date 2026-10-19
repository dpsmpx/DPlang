#!/usr/bin/env bash
# ============================================================
# Пакет APK — в ветку apk-builds (CLAUDE.md, «Сборки APK»).
#
#   tools/apk/publish.sh <программа> [аргументы git commit...]
#
# Собирает examples/<программа>/android/build.sh из чистого дерева и
# кладёт пакет в ветку-сироту apk-builds под именем
# <программа>-<коммит>.apk; строка — в оглавление README.md ветки;
# затем пуш. Аргументы после имени программы уходят в git commit
# (например, --trailer). Исходники — дерево Git, из которого запущен
# скрипт; рабочая копия ветки — его build/apk-builds.
# ============================================================
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
BRANCH=apk-builds
NAME="${1:?программа: tools/apk/publish.sh <программа>}"
shift
BUILD="$ROOT/examples/$NAME/android/build.sh"
WT="$ROOT/build/$BRANCH"
[ -f "$BUILD" ] || { echo "нет $BUILD" >&2; exit 1; }

# Имя с коммитом верно, только если дерево совпадает с коммитом.
if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
    echo "дерево не чистое: сборка не совпала бы с коммитом" >&2
    exit 1
fi
SHA="$(git -C "$ROOT" rev-parse --short=7 HEAD)"
SUBJECT="$(git -C "$ROOT" log -1 --format=%s HEAD)"
FILE="$NAME-$SHA.apk"

# Рабочая копия ветки — с GitHub; нет ветки (код 2) — новая сирота.
git -C "$ROOT" worktree remove --force "$WT" 2>/dev/null || rm -rf "$WT"
git -C "$ROOT" worktree prune
rc=0
git -C "$ROOT" ls-remote --exit-code --heads origin "$BRANCH" > /dev/null || rc=$?
if [ "$rc" -eq 0 ]; then
    git -C "$ROOT" fetch -q origin "$BRANCH"
    git -C "$ROOT" worktree add -q -B "$BRANCH" "$WT" FETCH_HEAD
elif [ "$rc" -eq 2 ]; then
    git -C "$ROOT" branch -D "$BRANCH" > /dev/null 2>&1 || true
    git -C "$ROOT" worktree add -q --detach "$WT"
    git -C "$WT" checkout -q --orphan "$BRANCH"
    git -C "$WT" rm -rfq .
else
    echo "git ls-remote: код $rc" >&2
    exit 1
fi
[ ! -e "$WT/$FILE" ] || { echo "$FILE уже в $BRANCH" >&2; exit 1; }

OUT="$ROOT/build/apk/$NAME"
APK="$(bash "$BUILD" "$OUT" | tail -1)"
cp "$APK" "$WT/$FILE"
SUM="$(sha256sum "$WT/$FILE" | cut -d' ' -f1)"
if [ ! -f "$WT/README.md" ]; then
    cat > "$WT/README.md" <<'EOF'
# Сборки APK

Хранилище пакетов APK (слово автора, 19 октября 2026; `CLAUDE.md`,
«Сборки APK»). Ветка-сирота: в `main` не вливается, PR из неё нет.

- Имя — `<программа>-<коммит>.apk`: пакет собран из чистого дерева
  этого коммита.
- Кладёт `tools/apk/publish.sh <программа>` из ветки с исходниками.
- Подпись — ключ отладки, у каждой сборки новый: поверх прежней
  установки пакет не встанет, сначала `adb uninstall <пакет>`
  (у triangle — `org.dpl.triangle`).

| файл | коммит | собран (UTC) | SHA-256 |
|---|---|---|---|
EOF
fi
printf '| [%s](%s) | `%s` %s | %s | `%s` |\n' "$FILE" "$FILE" "$SHA" "${SUBJECT//|/\\|}" \
    "$(date -u '+%Y-%m-%d %H:%M')" "$SUM" >> "$WT/README.md"
git -C "$WT" add "$FILE" README.md
git -C "$WT" commit -q -m "APK: $FILE" -m "Сборка $NAME из коммита $SHA ($SUBJECT)." "$@"

for delay in 2 4 8 16 0; do
    git -C "$WT" push -q -u origin "$BRANCH" && break
    [ "$delay" -gt 0 ] || { echo "пуш $BRANCH не удался" >&2; exit 1; }
    sleep "$delay"
done
echo "$BRANCH/$FILE $SUM"
