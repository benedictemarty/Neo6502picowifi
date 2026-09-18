# Racines de confiance embarquées dans le modem

`roots.pem` : certificats racines (PEM, concaténés) compilés dans le firmware
(`tools/pem2c.py` → `roots_pem.c` à la construction). Le modem **refuse** toute
connexion TLS dont la chaîne n'aboutit pas à l'une de ces racines, et vérifie
le nom d'hôte (SNI) et les dates (heure SNTP obligatoire).

| Racine | Source | Empreinte SHA-256 | Validité |
|---|---|---|---|
| ISRG Root X1 (Let's Encrypt) | `/etc/ssl/certs/ISRG_Root_X1.pem` du paquet `ca-certificates` Ubuntu, copié le 2026-09-16 | `96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6` | 2015-06-04 → 2035-06-04 |

Ajouter une racine : concaténer son PEM à `roots.pem`, vérifier son empreinte
(`openssl x509 -in x.pem -noout -fingerprint -sha256`) contre la publication
officielle de l'autorité, l'inscrire dans ce tableau, recompiler.
