# Changelog — Neo6502picowifi

## [Unreleased]
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
