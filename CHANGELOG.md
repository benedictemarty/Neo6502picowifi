# Changelog — Neo6502picowifi

## [Unreleased]
- 2026-09-24 : release **v0.3.1 publiée** (https://github.com/benedictemarty/Neo6502picowifi/releases/tag/v0.3.1),
  UF2 SHA-256 `805d2ada4145bff676c0ee2ae1aa851beb4df52897e8579d3c688ddf07e6cb95` : **identique** à l'UF2 flashé
  et validé sur carte (58/58) — première release reproductible. Incident : le push vers GitHub a échoué
  pendant `release.sh --publish` (le script s'est arrêté avant de créer la release, comme prévu) ; push
  relancé à la main, puis release créée avec `gh release create` à partir des fichiers de `dist/`.

## 0.3.1 — 2026-09-24
Version corrective.
- **US-W4 — cause du dernier reset.** `ATI` affichait `last reset: watchdog, stage 0` après un simple
  `AT+RST` ou un reflashage. Causes : `watchdog_caused_reboot()` est vrai pour tout reset passant par le
  watchdog (bootrom, `watchdog_reboot`), et le numéro d'étape était écrit dans `scratch[4]`, registre où
  `watchdog_enable()` pose la marque qui distingue un vrai dépassement — marque donc effacée à chaque étape.
  Correctif : classification portable `src/reset_cause.[ch]`, marqueurs du modem dans `scratch[0..2]`
  (4 à 7 réservés au SDK et au bootrom), `watchdog_enable_caused_reboot()` pour les dépassements, marqueurs
  posés par `AT+RST` et `AT+BOOTSEL`. Valeurs : `power-on`, `AT+RST`, `AT+BOOTSEL (UF2 flash)`,
  `reboot (bootloader or debugger)`, `watchdog timeout, stage N`, `lwip assert: …`. Test PC
  `tests/test_reset_cause.c` (14 vérifications). Sur carte : les quatre premières constatées ; le vrai
  dépassement n'a pas pu être provoqué sur carte (couvert par le test PC).
- **US-W5 — compilation reproductible.** `AT+GMR` `compile time:` = date du commit en UTC (`build_id.h`,
  `LC_ALL=C`) au lieu de `__DATE__`/`__TIME__` ; deux compilations dans deux répertoires donnent le même
  UF2 ; `tools/release.sh` compile deux fois et refuse de publier si les UF2 diffèrent. `test_at_modem` :
  +2 vérifications (`compile time`).
- Bannière UART `ready (…)` : tampon agrandi à la taille de `boot_info` (avertissement `-Wformat-truncation`).
- Validation sur carte avant release : `validate.py` 58/58.
- 2026-09-24 : release **v0.3.0 publiée** (https://github.com/benedictemarty/Neo6502picowifi/releases/tag/v0.3.0,
  UF2 SHA-256 `94c77ba54be21bf3203d58ec7f9a860f46274b7193b852030556c9c19037595a`) ; tag poussé sur GitHub et
  Framagit. L'UF2 téléchargé depuis la release a été flashé : `ATI` → `modem 0.3.0` / `build: v0.3.0`,
  `AT+GMR` → `Bin version(Pico W):0.3.0`, `validate.py` **58/58**. Backlog : US-W4 (cause de reset
  « watchdog » après un reflashage) et US-W5 (compilation non reproductible : `__DATE__`/`__TIME__`).

## 0.3.0 — 2026-09-24
Première version publiée depuis ce dépôt : tag `v0.3.0`, UF2 en release GitHub.
- 2026-09-24 : **US-W3 — versions.** Fichier `VERSION` (semver) = source unique : lu par
  `CMakeLists.txt` (`project(... VERSION)`, `PICOW_MODEM_VERSION`) ; plus de version codée en dur ni de
  repli `0.1.0` dans `net_pico.c` (`#error` si absente). Identifiant de build `git describe` recalculé à
  chaque compilation (`cmake/build_id.cmake` → `build_id.h`) et affiché par `ATI` (ligne `build: v0.3.0`,
  ou `v0.3.0-N-gSHA[-dirty]` hors release) ; `AT+GMR` garde le format ESP (`Bin version(Pico W):0.3.0`).
  `tools/release.sh` : vérifie (main, arbre propre, section CHANGELOG), lance les tests, tague, compile,
  contrôle l'identifiant, produit `dist/picow_modem-vX.Y.Z.uf2` + SHA-256 ; `--publish` pousse sur tous
  les remotes et crée la release GitHub. Tests : `test_at_modem` (+10 : GMR, ATI avec/sans build),
  `tests/test_version.py` (VERSION ↔ CMake ↔ CHANGELOG ↔ tag).
- 2026-09-24 : **US-T13 validée sur carte** (Pico W flashé depuis `fd9f879`) : `validation/validate.py`
  **58/58** — rapport `validation/RAPPORT-validation-2026-09-24.md`. Handshakes vers ISRG Root X1,
  DigiCert Global Root G2 et Sectigo E46 (`ATI` : `last root:` conforme) ; refus racine inconnue, nom
  faux, expiré (désormais par la date seule), IP. Tas : pic 32 096 o sur 171 360 o, inchangé d'une
  autorité à l'autre ; handshake complet 2,0 s, repris 130 ms.
- 2026-09-24 : **US-T13 implémentée — magasin de racines TLS complet en flash, consulté à la demande.**
  - `certs/roots.pem` : 150 racines Mozilla (paquet Ubuntu `ca-certificates` 20250419, SHA-256
    `693f7690…68ad47`) au lieu de la seule ISRG Root X1 ; provenance dans `certs/README.md`.
  - `tools/roots2c.py` (remplace `tools/pem2c.py`) : DER concaténés en flash (159 591 o) + index trié
    par FNV-1a du sujet ; exclut les clés non gérées (RSA < 2048, courbes ≠ P-256/P-384) et les doublons.
  - `src/roots_store.[ch]` (recherche portable), `src/roots_ca_cb.[ch]` (rappel
    `mbedtls_ssl_conf_ca_cb`, décodage `parse_der_nocopy`) ; `MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK` ;
    `net_pico.c` : config TLS sans chaîne de CA, rappel installé avec `tls_verify_cb` (authmode, SNI
    et dates inchangés). `ATI` : `roots: 150 in flash (on demand)`, `last root:`, `heap:` (utilisé, pic, max).
  - Tests : `tests/test_roots2c.py` (générateur, recoupé avec `cryptography`), `tests/test_roots_store.c`
    (1 069 vérifications), `tests/test_roots_ca_cb.c` (mbedTLS du SDK compilé sur PC : chaînes locales
    `tests/fixtures/gen.sh`, chaînes réelles DigiCert / github.com / mimuma.pl, refus racine inconnue et
    nom faux, racines jumelles, échec d'allocation, absence de fuite ; 337 vérifications). Mesure PC
    64 bits : pic de 6–12,5 Ko pendant une vérification, contre 406 Ko pour décoder les 150 racines d'avance.
    Suite complète : 164 + 10 804 + 1 069 + 337 vérifications et 8 tests Python, 0 échec.
  - Firmware : image 518 896 → 690 292 o (2 Mio de flash), BSS 84 920 o (+100). Compilé sans avertissement
    avec `~/pico-sdk-internal` ; `~/pico-sdk` (2.2.0) n'a pas ses sous-modules initialisés.
  - `validation/validate.py` + `PROTOCOLE.md` : étapes `www.digicert.com`, `github.com`, contrôle de
    `last root:` (58 étapes) ; `expired.badssl.com` n'est plus refusé que par les dates (racine COMODO
    désormais présente). **Non validé sur carte** : aucun Pico W branché ce jour.
- 2026-09-24 : `docs/BACKLOG.md` : story **US-T13** (P2) ajoutée — magasin de racines TLS complet en flash,
  consulté à la demande (`mbedtls_ssl_conf_ca_cb`), pour que la RAM consommée ne dépende plus du nombre de
  racines. Mesure du jour (compilation avec le SDK 2.2.0 complet) : image 518 896 o sur 2 Mio de flash,
  ~1,5 Mio libres ; `.bss` 84 820 o sur 256 Kio de RAM. Aucun changement de code.
- 2026-09-19 : **validé sur Neo6502 réel** (Trinity 0.1.0, TinyUSB 0.21) : modem reconnu (`USB serial modem found
  2E8A 000A`), `netinfo.neo` (neo-networking) affiche l'IP obtenue — la chaîne hub USB → CDC hôte → routage
  10,19 → modem → Wi-Fi fonctionne sans PC.
- 2026-09-19 : remote `framagit` ajouté (Framagit `benedictemarty/Neo6502picowifi`).
- 2026-09-19 : documentation bilingue — `README.en.md` (anglais) ajouté, `README.md` corrigé pour la
  racine du dépôt (`cmake -S . -B build`, `make -C tests`, câblage UEXT renvoyé vers
  `Neo6502drive/hardware/PICOW_UEXT.md`, CDC reconnu sous Trinity, section « Consommateurs ») ;
  `docs/BACKLOG.md` : en-tête de tableau réparé, lien README ; `validation/PROTOCOLE.md` : commandes
  de flash/test alignées. Tests PC : `make -C tests` → 164 + 10 804 vérifications, 0 échec. Dépôt
  distant créé (GitHub `benedictemarty/Neo6502picowifi`, remote `origin`).
- 2026-09-18 : projet créé par extraction de `Neo6502drive/firmware/picow-modem` (historique conservé,
  `git subtree split`), contenu remonté à la racine ; backlog (US-T0..T3, T9, T11, T12 repris ; US-W1..W3).
  Validation du jour : modem 0.2.0 reconnu par le Neo6502 sous Trinity 0.0.1
  (`USB serial modem found 2E8A 000A`) ; `AT`/`AT+GMR` OK sur PC. Neo6502drive n'a pas été modifié
  (arbre de travail occupé par une autre session) : y remplacer `firmware/picow-modem` par un renvoi.

## 0.2.0 — 2026-09-16 (dans Neo6502drive)
- Validation matérielle rejouable 55/55, reconnexion Wi-Fi de fond, TLS terminé sur le modem (US-T9),
  watchdog, corrections lwIP/Hayes, `AT+BOOTSEL`.
