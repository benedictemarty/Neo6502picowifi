# Backlog — Neo6502picowifi (firmware du modem Wi-Fi Pico W)

Stories reprises de Neo6502drive (`docs/BACKLOG.md`, épopée « périphérique réseau ») le 2026-09-18, à
l'extraction du firmware dans ce projet. Les stories 6502 (driver, terminal) restent dans Neo6502drive.
Contrainte transverse : `netsetup.neo`, `prophet.neo`, `pget.neo` et ProphetGui doivent continuer de
fonctionner sans modification.

## Stories reprises de Neo6502drive
| ID | P | User story | État |
|----|---|------------|------|
| US-T0 | P1 | En tant que PO, je veux un **firmware Pico W** (Pico SDK, cyw43/lwIP, TinyUSB) exposant le modem sur **USB CDC et UART0 GP0/GP1** à la fois, afin de brancher la carte en UEXT dès maintenant et en USB après F-13. | Terminé (S1) — validation sur carte à consigner |
| US-T1 | P1 | En tant qu'utilisateur, je veux un **modem Hayes virtuel** sur le canal série : `AT`, `ATDT hôte:port` (TCP sortant, mode transparent, `+++` pour revenir en commande), `ATH`, `ATA` (écoute entrante), registres S de base, afin d'utiliser tout programme terminal / BBS / MUD écrit pour un modem. | Terminé (S1) — tests PC ; carte à consigner |
| US-T2 | P1 | En tant qu'utilisateur, je veux que le périphérique accepte le **sous-ensemble AT ESP8266** utilisé par `netsetup`/`netinfo`/`netconsole`/`prophet` (liste exacte relevée dans les sources : voir `README.md`), afin de rester compatible avec les outils existants de la communauté sans modification. | Terminé (S1) — tests PC ; carte à consigner |
| US-T9 | P1 | En tant qu'utilisateur de `prophet.neo` (non modifié), je veux que le modem **termine le TLS** (« picowifitls », mémo Neo6502Prophet du 2026-09-16, décisions D5/D6) : `AT+CIPSTART="SSL"`, `AT+TLSPORT=443` pour les clients qui ne savent dire que `"TCP"`, certificat vérifié contre une racine embarquée (ISRG Root X1) + nom d'hôte + dates (heure SNTP exigée), reprise de session, mesures de handshake ; afin de dialoguer en HTTPS avec un serveur Prophet sur Internet. | Terminé (S1) — validé sur carte ; test de bout en bout avec le serveur Prophet HTTPS à faire |
| US-T3 | P1 | En tant que développeur 6502, je veux un **proxy de sockets** binaire (commandes `$10-$1F` : open/read/write/close, DNS, statut, 4 connexions, non bloquant) et le driver ca65 `net.s`, afin d'écrire des programmes réseau sans parser de texte AT. | À faire (S4) |
| US-T9 | P1 | En tant qu'utilisateur, je veux un **terminal série** sur le Neo (clavier → modem CDC, modem → console, Échap = sortie) pour taper `ATI`, `ATDT hôte:port`, `+++`, `ATH` et dialoguer avec un BBS. | Terminé 2026-09-15 : `driver/src/term.asm` (64tass, groupe 14 du fork), `make -C driver test` (faux modem Hayes de Phosphoneo ; `MODEM_TTY=/dev/ttyACM0` pour le vrai) |
| US-T11 | P2 | En tant que développeur, je veux un **mode « flux HTTP »** sur le proxy de sockets (US-T3) du modem : `open(url)` effectue le GET (Host, Range optionnel) et renvoie code + Content-Length, puis `read(n)` sert le corps (l'en-tête reste dans le modem) ; `HTTPS://` via le TLS embarqué. Base du périphérique réseau `N:` (mémo `docs/MEMO-PROPHET-N-DEVICE-2026-09-16.md`). | À faire — dépend de US-T3 |
| US-T12 | P1 | En tant qu'utilisateur, je veux une **liste d'hôtes autorisés / un journal** côté modem (`AT+NHOSTS=…`) pour le mode `N:`, afin qu'un `.neo` malveillant ne puisse pas exfiltrer le stockage vers un hôte arbitraire ; à croiser avec l'audit `neo-sandbox`. **Prérequis sécurité de `N:`.** | À faire — avant toute écriture réseau via `N:` |

## Ajouts 2026-09-18 (validation sur carte Neo6502 + Trinity 0.0.1)
| ID | P | User story | État |
|----|---|------------|------|
| US-W1 | P1 | En tant qu'utilisateur, je veux voir le modem **reconnu par le Neo6502** (firmware Trinity : `USB serial modem found 2E8A 000A`), afin de valider la chaîne USB hub → CDC. | **Terminé 2026-09-18** (carte Neo6502 4×USB-A, Trinity 0.0.1 ; un câble USB défectueux avait masqué le modem) |
| US-W2 | P1 | En tant qu'utilisateur, je veux **associer le Wi-Fi sans PC** : depuis NeoBASIC (`atconnect`, projet Neo6502Basic) ou `netsetup.neo` (gitlab.com/bocianu/neo-networking), la config restant persistante (`AT+CWJAP_DEF`). | TODO |
| US-W3 | P2 | En tant que PO, je veux des **versions** du modem (`AT+GMR` : `Bin version(Pico W):x.y.z`, tag `vX.Y.Z`, UF2 en release) et un CHANGELOG, afin de savoir ce qui tourne sur la carte. | TODO (0.2.0 actuelle, non taguée) |
