#!/bin/sh
# release.sh — publie la version du fichier VERSION (US-W3).
#   PICO_SDK_PATH=… tools/release.sh            vérifie, teste, tague vX.Y.Z (local), compile
#                                               → dist/picow_modem-vX.Y.Z.uf2 (+ .sha256)
#   PICO_SDK_PATH=… tools/release.sh --publish  idem + pousse main et le tag sur tous les remotes
#                                               + release GitHub (gh) avec l'UF2
# Prérequis : arbre propre sur main, section « ## X.Y.Z — date » dans CHANGELOG.md.
set -eu
cd "$(dirname "$0")/.."
V=$(cat VERSION)
TAG="v$V"
PUBLISH=${1:-}
die() { echo "release: $*" >&2; exit 1; }

[ -n "${PICO_SDK_PATH:-}" ] || die "PICO_SDK_PATH non défini"
[ "$(git rev-parse --abbrev-ref HEAD)" = main ] || die "pas sur main"
[ -z "$(git status --porcelain)" ] || die "arbre de travail non propre"
grep -q "^## $V — " CHANGELOG.md || die "CHANGELOG.md : section « ## $V — date » absente"
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    [ "$(git rev-list -n1 "$TAG")" = "$(git rev-parse HEAD)" ] || die "$TAG existe déjà sur un autre commit"
fi

echo "== tests"
make -C tests clean >/dev/null
make -C tests test

echo "== tag $TAG"
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null || \
    git tag -a "$TAG" -m "Neo6502picowifi $V (Pico W modem)"

echo "== compilation"
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
cmake -S . -B "$B" >/dev/null
cmake --build "$B" -j8 >/dev/null
grep -q "\"$TAG\"" "$B/build_id.h" || die "identifiant de build inattendu : $(cat "$B/build_id.h")"
mkdir -p dist
cp "$B/picow_modem.uf2" "dist/picow_modem-$TAG.uf2"
(cd dist && sha256sum "picow_modem-$TAG.uf2" > "picow_modem-$TAG.uf2.sha256")
echo "dist/picow_modem-$TAG.uf2 ($(stat -c %s "dist/picow_modem-$TAG.uf2") o) : $(cut -d' ' -f1 "dist/picow_modem-$TAG.uf2.sha256")"

[ "$PUBLISH" = "--publish" ] || { echo "== fin (sans publication : relancer avec --publish)"; exit 0; }

echo "== publication"
for r in $(git remote); do
    git push -q "$r" main
    git push -q "$r" "$TAG"
done
notes=$(awk -v v="$V" '$0 ~ "^## "v" — " {on=1; next} /^## / {on=0} on' CHANGELOG.md)
gh release create "$TAG" "dist/picow_modem-$TAG.uf2" "dist/picow_modem-$TAG.uf2.sha256" \
    --title "Neo6502picowifi $V" --notes "$notes"
echo "== publié : $TAG"
