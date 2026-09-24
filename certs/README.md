# Racines de confiance embarquées dans le modem

`roots.pem` : magasin de racines (PEM concaténés), compilé dans le firmware par
`tools/roots2c.py` → `roots_gen.c` à la construction : certificats en DER
concaténés **en flash** + index trié par empreinte FNV-1a du sujet (US-T13).
Aucune racine n'est décodée d'avance : pendant la vérification TLS, mbedTLS
appelle `roots_ca_cb` (`src/roots_ca_cb.c`), qui ne décode que les racines dont
le sujet égale l'émetteur recherché. La RAM consommée ne dépend donc pas du
nombre de racines.

Le modem **refuse** toute connexion TLS dont la chaîne n'aboutit pas à l'une de
ces racines, et vérifie le nom d'hôte (SNI) et les dates (heure SNTP obligatoire).

## Magasin actuel

| Champ | Valeur |
|---|---|
| Contenu | Magasin de racines Mozilla (confiance « serveur web »), tel que distribué par le paquet Ubuntu `ca-certificates` |
| Source | `/etc/ssl/certs/ca-certificates.crt`, paquet `ca-certificates` version **20250419**, copié le **2026-09-24** |
| SHA-256 du fichier | `693f769039db98ce5d24b50e5053f731401c4374d1f43ac1699c4bff2968ad47` |
| Racines | 150 (RSA 4096 : 65, RSA 2048 : 42, EC P-384 : 39, EC P-256 : 4), toutes retenues |
| Taille en flash | 159 591 octets DER + index |
| Exemples | ISRG Root X1 / X2 (Let's Encrypt), DigiCert Global Root G2, USERTrust ECC, Sectigo Public Server Authentication Root E46, GTS Root R1… |

`roots2c.py` exclut (et liste à la compilation) les racines que la configuration
mbedTLS du modem ne sait pas vérifier : clé RSA de moins de 2048 bits, courbe
autre que P-256 / P-384 (P-521 notamment), autre algorithme ; ainsi que les
doublons exacts. Aucune exclusion avec le magasin actuel.

## Mettre à jour le magasin

1. Remplacer `roots.pem` par le nouveau fichier (même origine de préférence :
   `ca-certificates` à jour, ou l'export Mozilla officiel).
2. Noter dans le tableau ci-dessus la version, la date et le SHA-256
   (`sha256sum roots.pem`).
3. `make -C tests` : les tests exigent notamment ISRG Root X1, ISRG Root X2,
   DigiCert Global Root G2, USERTrust ECC, et vérifient les chaînes réelles
   capturées dans `tests/fixtures/real_*.pem` (à rafraîchir avec
   `tests/fixtures/gen.sh --real`).
4. Recompiler, flasher, rejouer `validation/validate.py`, consigner dans le
   CHANGELOG.

## Ajouter une racine isolée

Concaténer son PEM à `roots.pem`, vérifier son empreinte
(`openssl x509 -in x.pem -noout -fingerprint -sha256`) contre la publication
officielle de l'autorité, l'ajouter dans ce document, recompiler.
