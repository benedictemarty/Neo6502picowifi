#!/bin/sh
# gen.sh — jeux d'essai de US-T13 (magasin de racines en flash). Rejouable ;
# les sorties sont versionnées pour que les tests ne dépendent ni d'openssl
# ni du réseau. Validité 100 ans : les chaînes locales ne périment pas.
#   ./gen.sh          chaînes de test locales (openssl)
#   ./gen.sh --real   chaînes réelles capturées (réseau, openssl s_client) ;
#                     leurs feuilles expirent, le test ignore donc les dates
set -eu
cd "$(dirname "$0")"

if [ "${1:-}" = "--real" ]; then
    for h in www.digicert.com github.com mimuma.pl; do
        openssl s_client -connect "$h:443" -servername "$h" -showcerts </dev/null 2>/dev/null \
            | sed -n '/-----BEGIN CERTIFICATE-----/,/-----END CERTIFICATE-----/p' > "real_$h.pem"
        echo "real_$h.pem : $(grep -c BEGIN "real_$h.pem") certificat(s)"
    done
    exit 0
fi

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
D=36500
RSA="-newkey rsa:2048"
P256="-newkey ec -pkeyopt ec_paramgen_curve:P-256"
P521="-newkey ec -pkeyopt ec_paramgen_curve:P-521"
WEAK="-newkey rsa:1024"

root() {    # root nom sujet clé
    # shellcheck disable=SC2086
    openssl req -x509 -new -nodes $3 -keyout "$T/$1.key" -out "$T/$1.pem" -subj "$2" -days $D \
        -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
}
signed() {  # signed nom sujet clé émetteur fichier-extensions
    # shellcheck disable=SC2086
    openssl req -new -nodes $3 -keyout "$T/$1.key" -out "$T/$1.csr" -subj "$2" 2>/dev/null
    openssl x509 -req -in "$T/$1.csr" -CA "$T/$4.pem" -CAkey "$T/$4.key" -CAcreateserial \
        -days $D -extfile "$T/$5" -out "$T/$1.pem" 2>/dev/null
}
printf 'basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n' > "$T/ca.ext"
for h in test.example twin.example x.example; do
    printf 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:%s\n' "$h" > "$T/$h.ext"
done

root   root_a  "/CN=Test Root A"        "$RSA"                       # chaîne à 3 niveaux
signed int_a   "/CN=Test Int A"         "$RSA"  root_a ca.ext
signed leaf_a  "/CN=test.example"       "$P256" int_a  test.example.ext
root   twin1   "/CN=Test Twin Root"     "$P256"                      # même sujet, 2 clés
root   twin2   "/CN=Test Twin Root"     "$P256"
signed leaf_t  "/CN=twin.example"       "$P256" twin2  twin.example.ext
root   root_x  "/CN=Test Unknown Root"  "$RSA"                       # absente du magasin
signed leaf_x  "/CN=x.example"          "$P256" root_x x.example.ext
root   p521    "/CN=Test P-521 Root"    "$P521"                      # exclues à la génération
root   weak    "/CN=Test Weak Root"     "$WEAK"

cat "$T/root_a.pem" "$T/twin1.pem" "$T/twin2.pem" "$T/p521.pem" "$T/weak.pem" "$T/root_a.pem" > store.pem
cat "$T/leaf_a.pem" "$T/int_a.pem" > chain_a.pem
cp "$T/leaf_t.pem" chain_t.pem
cp "$T/leaf_x.pem" chain_x.pem
echo "store.pem chain_a.pem chain_t.pem chain_x.pem régénérés"
