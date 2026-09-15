# picow-modem — modem Wi-Fi Neo6502drive sur Raspberry Pi Pico W

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
  - USB CDC-ACM (`/dev/ttyACM*` sur PC ; sur le Neo6502 après la story
    F-13 « hôte CDC » du dépôt Neo6502firmware — **le firmware Neo6502
    officiel ignore aujourd'hui les périphériques CDC**) ;
  - UART0 GP0 (TX) / GP1 (RX), 115200 8N1 : connecteur UEXT du Neo6502,
    utilisable **dès maintenant** (câblage dans `hardware/PICOW_UEXT.md`).
- Configuration persistante (dernier secteur de flash) : SSID/mot de passe
  (`AT+CWJAP_DEF`), écho, DHCP/IP statique, DNS, SNTP, port d'écoute, S0.
  Au démarrage le Pico W rejoint le dernier réseau enregistré, comme l'ESP.
- LED de la carte : allumée = Wi-Fi associé, clignote = connexion TCP ouverte.
- Watchdog 8 s : un blocage redémarre la carte ; `ATI` indique la cause et
  l'étape (`net_pico_stage`) ou le message d'assertion lwIP.

## Commandes AT prises en charge

| Commande | Réponse (format ESP8266 AT 1.x) |
|---|---|
| `AT`, `ATE0`, `ATE1` | `OK` |
| `AT+GMR` | `AT version:…`, `SDK version:…`, `OK` |
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
| `AT+CIPSSLCCONF?` / `=0` | `+CIPSSLCCONF:0` — **pas de TLS** (`=1..3` → `ERROR`) |
| `AT+CIPSTART="TCP","hôte",port` | `CONNECT`, `OK` ; `DNS Fail` ; `ALREADY CONNECTED` |
| `AT+CIPSEND=n` (n ≤ 2048) | `OK`, `> `, puis après n octets `Recv n bytes`, `SEND OK` |
| données entrantes | `+IPD,n:` suivi de n octets (segments ≤ 1460) ; `CLOSED` à la fermeture |
| `AT+CIPCLOSE` | `CLOSED`, `OK` |
| `AT+CIPSERVER=1,port` / `=0` | écoute entrante → `RING` (répété toutes les 3 s), `ATA` pour décrocher |
| `AT+CIPSNTPCFG?` / `=en,tz,"serveur"`, `AT+CIPSNTPTIME?` | SNTP lwIP ; `+CIPSNTPTIME:Tue Sep 15 12:00:00 2026` |
| `AT+PING="hôte"` | `+ms`, `OK` ou `+timeout`, `ERROR` |
| `AT+CIUPDATE` | `ERROR` (pas d'OTA : reflasher un UF2) |
| `ATI` | identité + cause du dernier reset (`power-on`, `watchdog, stage n`, `lwip assert: …`) |
| `AT+BOOTSEL` | `OK` puis passage en mode UF2 (`RPI-RP2`) sans toucher au bouton — spécifique à ce firmware |

Non pris en charge (répond `ERROR`) : UDP, `CIPMUX=1`, mode point d'accès,
TLS, mode transparent ESP (`CIPMODE=1` ; utiliser `ATDT` à la place).

## Compilation

```
export PICO_SDK_PATH=/chemin/pico-sdk      # SDK 2.x avec lib/cyw43-driver, lwip, tinyusb
make firmware                              # depuis la racine du dépôt
# ou : cmake -S firmware/picow-modem -B firmware/picow-modem/build && cmake --build firmware/picow-modem/build
```

Résultat : `build/picow_modem.uf2`. Flash : brancher le Pico W en maintenant
**BOOTSEL**, puis `make flash` (copie sur le volume `RPI-RP2`).

Vitesse UART : `-DUART_BAUD=…` dans `target_compile_definitions` (115200 par
défaut = valeur de `netsetup.pas`).

## Tests

```
make test          # firmware/picow-modem/tests : cœur du modem sur PC (gcc, ASan/UBSan)
```

`src/at_modem.c` ne dépend d'aucune API Pico : la maquette
`tests/test_at_modem.c` rejoue les séquences exactes de netinfo, netsetup,
prophet (`CIPSTART` → `CIPSEND` → `+IPD` → `CLOSED`) et du modem Hayes.

Test sur carte : `screen /dev/ttyACM0` (ou `minicom`) puis `AT`, `AT+CWLAP`,
`AT+CWJAP_DEF="ssid","pass"`, `AT+CIPSTART="TCP","mimuma.pl",8998`… ;
résultats à consigner dans `docs/SPRINTS.md`.

## Structure

```
src/at_modem.[ch]     cœur portable : parseur AT/Hayes, tampon RX, +IPD, +++
src/net_pico.[ch]     Wi-Fi (cyw43), TCP/DNS/SNTP/ping (lwIP), flash de configuration
src/main.c            transports USB CDC + UART0, boucle principale, LED
src/usb_descriptors.c, tusb_config.h, lwipopts.h
tests/                tests unitaires PC
```
