# Neo6502picowifi — modem Wi-Fi Neo6502 sur Raspberry Pi Pico W (ex-`Neo6502drive/firmware/picow-modem`)

*English version: [README.en.md](README.en.md).*

Firmware C (Pico SDK 2.x, cyw43 + lwIP, TinyUSB) qui transforme un **Pico W**
en modem Wi-Fi pour le Neo6502 (stories US-T1 et US-T2 de `docs/BACKLOG.md`).

## Ce qu'il fait

- **Dialecte AT ESP8266** : le sous-ensemble exact utilisé par `netsetup.neo`,
  `netinfo.neo`, `netconsole.neo` (gitlab.com/bocianu/neo-networking) et
  `prophet.neo` / `pget.neo` (neo-prophet), relevé dans leurs sources
  (`uart.inc`, `http.inc`, `*.pas`). Ces programmes fonctionnent **sans
  modification** en croyant parler à un MOD-WIFI-ESP8266.
- **Modem Hayes** : `ATDT hôte:port` (TCP sortant, mode transparent),
  `+++` (temps de garde 1 s, S12), `ATO`, `ATH`, `ATA`, `ATE0/1`, `ATZ`,
  `ATI`, `ATS0/S2/S12`, `RING` sur appel entrant (`AT+CIPSERVER=1,port`).
- **Deux transports simultanés**, réponses émises sur les deux :
  - USB CDC-ACM (`/dev/ttyACM*` sur PC ; sur le Neo6502 avec un firmware
    hôte CDC — la branche `trinity` du fork Neo6502firmware le reconnaît
    (`USB serial modem found 2E8A 000A`, 2026-09-18), **le firmware Neo6502
    officiel ignore aujourd'hui les périphériques CDC**) ;
  - UART0 GP0 (TX) / GP1 (RX), 115200 8N1 : connecteur UEXT du Neo6502,
    utilisable **dès maintenant** (câblage : `Neo6502drive/hardware/PICOW_UEXT.md`).
- Configuration persistante (dernier secteur de flash) : SSID/mot de passe
  (`AT+CWJAP_DEF`), écho, DHCP/IP statique, DNS, SNTP, port d'écoute, S0.
  Le Pico W rejoint le dernier réseau enregistré en tâche de fond (au boot et
  après une coupure : nouvelle tentative toutes les 15 s), comme l'ESP.
- **TLS terminé sur le Pico W** (mbedTLS 3.6, TLS 1.2 client) : `AT+CIPSTART="SSL",…`
  ou, pour les clients non modifiables comme `prophet.neo`, `AT+TLSPORT=443` qui
  rend TLS toute connexion `"TCP"` vers ce port. Certificat **toujours vérifié**
  (chaîne contre les 150 racines du magasin Mozilla `certs/roots.pem`, stockées
  en flash et décodées à la demande, nom d'hôte/SNI,
  dates via l'heure SNTP — refus si l'heure n'est pas acquise). Reprise de
  session (ticket) pour le même hôte:port. Détail : § TLS.
- LED de la carte : allumée = Wi-Fi associé, clignote = connexion TCP ouverte.
- Watchdog 8 s : un blocage redémarre la carte ; `ATI` indique la cause et
  l'étape (`net_pico_stage`) ou le message d'assertion lwIP.

## Commandes AT prises en charge

| Commande | Réponse (format ESP8266 AT 1.x) |
|---|---|
| `AT`, `ATE0`, `ATE1` | `OK` |
| `AT+GMR` | `AT version:…`, `SDK version:…`, `Bin version(Pico W):X.Y.Z` (fichier `VERSION`), `OK` |
| `AT+RST`, `AT+RESTORE` | `OK` puis redémarrage (RESTORE efface la configuration) |
| `AT+CWMODE?` / `=1` | `+CWMODE:1` (station seule ; `=2`/`=3` → `ERROR`) |
| `AT+CWJAP[_CUR|_DEF]="ssid","pass"` | `WIFI CONNECTED`, `WIFI GOT IP`, `OK` ou `+CWJAP:n`, `FAIL` |
| `AT+CWJAP[_CUR|_DEF]?` | `+CWJAP_CUR:"ssid","mac",canal,rssi` ou `No AP` |
| `AT+CWQAP` | `OK`, `WIFI DISCONNECT` |
| `AT+CWLAPOPT=…`, `AT+CWLAP[=…]` | `+CWLAP:(ecn,"ssid",rssi)` par SSID (dédoublonné, meilleur RSSI, trié), `OK` |
| `AT+CWDHCP[_CUR|_DEF]?` / `=mode,en` | `+CWDHCP_DEF:3` (bit 1 = station) |
| `AT+CIPSTATUS` | `STATUS:2|3|4|5` (+ ligne `+CIPSTATUS:` si TCP ouvert) |
| `AT+CIFSR` | `+CIFSR:STAIP,"ip"`, `+CIFSR:STAMAC,"mac"` |
| `AT+CIPSTA[_CUR|_DEF]?` / `="ip","gw","mask"` | `+CIPSTA_CUR:ip:"…"`, `:gateway:`, `:netmask:` |
| `AT+CIPDNS[_CUR|_DEF]?` / `=1,"ip"` / `=0` | `+CIPDNS_CUR:ip` |
| `AT+CIPMUX?` / `=0`, `AT+CIPMODE?` / `=0` | connexion unique, mode normal (`=1` → `ERROR`) |
| `AT+CIPSSLCCONF?` / `=0` / `=2` | `+CIPSSLCCONF:2` (CA toujours vérifiée ; `=1`/`=3` cert client → `ERROR`) |
| `AT+CIPSTART="TCP","hôte",port` | `CONNECT`, `OK` ; `DNS Fail` ; `ALREADY CONNECTED` ; TLS si le port est dans `AT+TLSPORT` |
| `AT+CIPSTART="SSL","hôte",port` | idem en TLS ; `no time (SNTP) for TLS`, `TLS handshake failed` (certificat refusé…) → `ERROR` |
| `AT+TLSPORT?` / `=443[,p2,p3,p4]` / `=0` | ports pour lesquels `"TCP"` est fait en TLS (persistant) ; `=0` efface |
| `AT+TLSTEST` | autotests mbedTLS (AES, GCM, SHA-256/512, CTR-DRBG, ECP, MPI) sur carte |
| `AT+CIPSEND=n` (n ≤ 2048) | `OK`, `> `, puis après n octets `Recv n bytes`, `SEND OK` |
| données entrantes | `+IPD,n:` suivi de n octets (segments ≤ 1460) ; `CLOSED` à la fermeture |
| `AT+CIPCLOSE` | `CLOSED`, `OK` |
| `AT+CIPSERVER=1,port` / `=0` | écoute entrante → `RING` (répété toutes les 3 s), `ATA` pour décrocher |
| `AT+CIPSNTPCFG?` / `=en,tz,"serveur"`, `AT+CIPSNTPTIME?` | SNTP lwIP ; `+CIPSNTPTIME:Tue Sep 15 12:00:00 2026` |
| `AT+PING="hôte"` | `+ms`, `OK` ou `+timeout`, `ERROR` |
| `AT+CIUPDATE` | `ERROR` (pas d'OTA : reflasher un UF2) |
| `ATI` | identité (`modem X.Y.Z`), ligne `build:` (`git describe` : `vX.Y.Z` pour une release, `vX.Y.Z-N-gSHA[-dirty]` sinon), SSID mémorisé, cause du dernier reset, ligne `TLS:` (pile, nombre de racines, racine retenue au dernier handshake (`last root:`), tas newlib (`heap:` utilisé, pic, max), heure, durée et suite du dernier handshake, `resumed`, drapeaux de vérification, derniers messages lwIP/mbedTLS), `TLS ports:` |
| `AT+BOOTSEL` | `OK` puis passage en mode UF2 (`RPI-RP2`) sans toucher au bouton — spécifique à ce firmware |

Non pris en charge (répond `ERROR`) : UDP, `CIPMUX=1`, mode point d'accès,
TLS 1.3, certificat client, mode transparent ESP (`CIPMODE=1` ; utiliser
`ATDT` à la place — `ATDT` fait aussi du TLS vers un port de `AT+TLSPORT`).

## TLS

- Pile : lwIP `altcp_tls` + mbedTLS 3.6.2 (`src/mbedtls_config.h`) ; TLS 1.2,
  ECDHE-ECDSA / ECDHE-RSA / RSA, AES-GCM, SHA-256/384, P-256, P-384, X25519.
- Vérification : `MBEDTLS_SSL_VERIFY_REQUIRED` (forcé dans `lwipopts.h`
  **et** dans le code — la valeur par défaut d'altcp est `OPTIONAL`, qui
  laisserait passer un certificat invalide), SNI + nom d'hôte, dates
  vérifiées dans un rappel (`tls_date.c`, sans `gmtime_r` qui bloquerait dans
  le contexte lwIP), heure SNTP exigée. Pas de mode « accepter tout ».
- Racines (US-T13) : `certs/roots.pem` = magasin Mozilla (150 racines, paquet
  Ubuntu `ca-certificates` ; provenance et mise à jour : `certs/README.md`).
  `tools/roots2c.py` les compile en DER concaténés (159 591 o, en flash) + un
  index trié par empreinte FNV-1a du sujet (`src/roots_store.c`). Aucune racine
  n'est chargée en RAM d'avance : pendant la vérification, mbedTLS appelle
  `roots_ca_cb` (`src/roots_ca_cb.c`, `MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK`)
  avec le certificat dont il cherche l'émetteur ; seules les racines de ce sujet
  sont décodées, sans copie du DER (`mbedtls_x509_crt_parse_der_nocopy`), puis
  libérées par mbedTLS. Mesure PC (64 bits) : pic de 6–12,5 Ko pendant une
  vérification, contre 406 Ko pour décoder les 150 racines d'avance. Racines
  exclues à la génération : clé RSA < 2048 ou courbe autre que P-256/P-384.
- Reprise de session : ticket mémorisé par hôte:port et réutilisé à la
  connexion suivante (`prophet.neo` ouvre une connexion par bloc `Range`).
- Mesures sur carte (2026-09-16, Apache + Let's Encrypt, ECDHE-RSA-AES256-GCM) :
  handshake complet **2,4–2,8 s** (chaîne de 4 certificats vérifiée en ~1,7 s),
  handshake repris **0,3–0,5 s** ; `AT+CIPSTART` complet : 3–4 s la première
  fois, **1,2 s** ensuite. letsencrypt.org (ECDSA, CDN) : 5 s. Flash 523 Ko,
  BSS 85 Ko, mbedTLS sur le tas newlib. Depuis US-T13 : image 690 Ko
  (mesure de compilation du 2026-09-24), BSS inchangée.
- Pièges rencontrés : (1) **`-O3` (GCC 14.2.1, armv6-m) produit un AES-GCM
  faux** dans `gcm.c` → `bad_record_mac` chez tous les serveurs ; le projet
  compile en `-O2` (`CMakeLists.txt`), `AT+TLSTEST` le vérifie ; (2) le
  handshake s'exécute dans l'IRQ lwIP et peut dépasser les 8 s du watchdog :
  un timer prioritaire le rafraîchit pendant le handshake (60 s max) ;
  (3) `MBEDTLS_ECP_WINDOW_SIZE 4` : ×4 plus rapide que la valeur par défaut.

## Compilation

```
export PICO_SDK_PATH=/chemin/pico-sdk      # SDK 2.x avec lib/cyw43-driver, lwip, tinyusb
cmake -S . -B build && cmake --build build # depuis la racine du dépôt
```

Résultat : `build/picow_modem.uf2`. Flash : brancher le Pico W en maintenant
**BOOTSEL** (ou envoyer `AT+BOOTSEL` au modem), puis copier l'UF2 sur le
volume `RPI-RP2`.

Vitesse UART : `-DUART_BAUD=…` dans `target_compile_definitions` (115200 par
défaut = valeur de `netsetup.pas`).

## Versions et releases

- Version : fichier `VERSION` (semver), seule source ; `CMakeLists.txt` la lit, `AT+GMR` et `ATI`
  l'affichent. `ATI` donne en plus l'identifiant de build (`git describe`, recalculé à chaque
  compilation) : `build: v0.3.0` = UF2 de la release, tout autre suffixe = compilation de travail.
- Release : incrémenter `VERSION`, déplacer les entrées de `[Unreleased]` du CHANGELOG sous
  `## X.Y.Z — date`, commiter, puis `PICO_SDK_PATH=… tools/release.sh` (tests, tag `vX.Y.Z`,
  compilation, `dist/picow_modem-vX.Y.Z.uf2` + SHA-256) ; `--publish` pousse `main` et le tag sur tous
  les remotes et crée la release GitHub avec l'UF2.
- `make -C tests` contrôle la cohérence `VERSION` ↔ CMake ↔ CHANGELOG ↔ tag (`tests/test_version.py`).

## Tests

```
make -C tests      # cœur du modem (test_at_modem) + dates TLS (test_tls_date) sur PC, gcc + ASan/UBSan
                   # + magasin de racines : générateur (test_roots2c.py), recherche (test_roots_store),
                   #   rappel contre mbedTLS (test_roots_ca_cb, exige PICO_SDK_PATH ou MBEDTLS_DIR)
```

`test_roots_ca_cb` compile le mbedTLS du SDK sur PC (`tests/mbedtls_host_config.h`)
et vérifie des chaînes locales (`tests/fixtures/gen.sh` : 3 niveaux, racines
jumelles, racine inconnue, mauvais nom) et réelles capturées (DigiCert, Sectigo,
Let's Encrypt ; `gen.sh --real` pour les rafraîchir). Sans mbedTLS il est
**sauté** avec un message explicite.

`src/at_modem.c` ne dépend d'aucune API Pico : la maquette
`tests/test_at_modem.c` rejoue les séquences exactes de netinfo, netsetup,
prophet (`CIPSTART` → `CIPSEND` → `+IPD` → `CLOSED`) et du modem Hayes.

Validation sur carte, automatisée : `python3 validation/validate.py`
(protocole `validation/PROTOCOLE.md`, rapports `validation/RAPPORT-*.md`) —
58 étapes : identité, Wi-Fi, SNTP, séquence Prophet en clair et en TLS,
autorités hors Let's Encrypt (DigiCert, Sectigo), refus TLS (racine inconnue,
nom faux, expiré, IP), Hayes, appel entrant.
Prérequis : Wi-Fi provisionné une fois avec `screen /dev/ttyACM0 115200` et
`AT+CWJAP_DEF="ssid","pass"`.

## Structure

```
src/at_modem.[ch]     cœur portable : parseur AT/Hayes, tampon RX, +IPD, +++
src/net_pico.[ch]     Wi-Fi (cyw43), TCP/TLS (altcp + mbedTLS), DNS/SNTP/ping, flash, watchdog/diagnostic
src/tls_date.[ch]     date civile sans gmtime_r (vérification des dates de certificats)
src/mbedtls_config.h  configuration mbedTLS (client TLS 1.2)
certs/roots.pem       racines de confiance (magasin Mozilla) ; tools/roots2c.py les compile
src/roots_store.[ch]  index des racines en flash, recherche par sujet (portable, testé sur PC)
src/roots_ca_cb.[ch]  rappel mbedTLS : racines décodées à la demande
src/main.c            transports USB CDC + UART0, boucle principale, LED
src/usb_descriptors.c, tusb_config.h, lwipopts.h
tests/                tests unitaires PC
validation/           protocole, script et rapports de validation sur carte
docs/BACKLOG.md       backlog agile ; CHANGELOG.md — versions = `AT+GMR` et tags `vX.Y.Z`
VERSION               version (semver) ; cmake/build_id.cmake : identifiant de build ; tools/release.sh
```

## Consommateurs

Neo6502drive (driver 6502, terminal), Neo6502ProphetGui, Neo6502Basic
(primitives `at`), firmware Neo6502 branche `trinity` (groupe 14 CDC, routage
10,19). Les programmes amont (`netsetup.neo`, `prophet.neo`, `pget.neo`,
ProphetGui) doivent continuer de fonctionner sans modification ; tout nouveau
comportement AT est documenté ici.
