# Protocole de validation matérielle — picow-modem

Objectif : prouver sur **Pico W réel** que le firmware `build/picow_modem.uf2`
tient les stories US-T0/T1/T2 (modem AT ESP8266 + Hayes) et US-T9 (TLS),
avec les **refus** de sécurité. Modèle : `~/picowifi/validation` (protocole +
script + rapport, versionnés avec le code).

## Prérequis
- Pico W flashé : `make flash` (BOOTSEL) ou `AT+BOOTSEL` puis copie de l'UF2.
- Wi-Fi 2,4 GHz provisionné par le PO (`AT+CWJAP_DEF="ssid","mdp"` dans un
  terminal ; jamais dans le dépôt ni dans le script).
- Port `/dev/ttyACM0` libre (fermer `screen`), PC sur le même réseau que le
  Pico W (appel entrant), accès Internet (mimuma.pl, badssl.com, telehack.com).
- Protocole série : 115200 8N1, commandes terminées par **CRLF** (comme
  `netsetup.pas`) ; le modem absorbe le LF qui suit un CR même après
  `AT+CIPSEND`/`ATDT`.

## Cibles réseau
| Cible | Attendu | Prouve |
|---|---|---|
| `mimuma.pl:80` | `CONNECT`, `+IPD`, `CLOSED` | séquence Prophet en clair |
| `mimuma.pl:443` (Let's Encrypt → ISRG Root X1) | `CONNECT`, puis `resumed` | chaîne, SNI, dates, reprise |
| `badssl.com:443` | `CONNECT` | 2e serveur Let's Encrypt |
| `untrusted-root.badssl.com` | `TLS handshake failed` | racine absente refusée |
| `wrong.host.badssl.com` | `TLS handshake failed` | nom d'hôte vérifié |
| `expired.badssl.com` | `TLS handshake failed` | expiré (et racine COMODO absente) |
| `178.219.142.145:443` (IP) | `TLS handshake failed` | nom non vérifiable refusé |
| `telehack.com:23` | `CONNECT`, dialogue, `+++`, `ATO`, `ATH` | modem Hayes |
| PC → Pico `:6502` | `RING`, `ATA`, `NO CARRIER` | appel entrant |

## Exécution
```
python3 validation/validate.py [/dev/ttyACM0] [--quick]
```
Le script imprime ✓/✗ par étape et un tableau Markdown à coller dans
`RAPPORT-validation-<date>.md` avec le commit, la version (`ATI`) et le
réseau utilisé. Code de retour 0 = tout passe.

## Critères de réussite
- Toutes les étapes ✓ ; en particulier **les quatre refus TLS** (sinon la
  vérification est cassée : voir `ALTCP_MBEDTLS_AUTHMODE` dans `lwipopts.h`)
  et `AT+TLSTEST` avec `gcm=0` (sinon `-O3` a été réintroduit).
- Ordres de grandeur : handshake complet 2–6 s, repris < 1 s, connexions
  Prophet suivantes < 2 s.

## Non couvert par le script (manuel)
- Refus sans heure SNTP (`no time (SNTP) for TLS`) : couper le réseau avant
  la synchro, ou tester sur PC (`make test`).
- Transport UART GP0/GP1 (adaptateur USB-série ou UEXT) ; Neo6502 réel avec
  `netsetup.neo` / `prophet.neo`.
