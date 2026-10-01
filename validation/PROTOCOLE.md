# Protocole de validation matérielle — picow-modem

Objectif : prouver sur **Pico W réel** que le firmware `build/picow_modem.uf2`
tient les stories US-T0/T1/T2 (modem AT ESP8266 + Hayes) et US-T9 (TLS),
avec les **refus** de sécurité, puis (ajouts 2026-10-01) US-T14 (UDP), US-T11
(flux HTTP(S)), US-T12 (hôtes autorisés), US-W6 (point d'accès de configuration)
et US-T17 (second port USB TNFS). Modèle : `~/picowifi/validation` (protocole +
script + rapport, versionnés avec le code).

## Prérequis
- Pico W flashé : BOOTSEL ou `AT+BOOTSEL`, puis copie de `build/picow_modem.uf2` sur `RPI-RP2`.
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
| `mimuma.pl:443` (Let's Encrypt → ISRG Root X1) | `CONNECT`, puis `resumed` ; `ATI` : `last root: ISRG Root X1` | chaîne, SNI, dates, reprise ; racine trouvée dans le magasin en flash (US-T13) |
| `badssl.com:443` | `CONNECT` | 2e serveur Let's Encrypt |
| `www.digicert.com:443` | `CONNECT` ; `ATI` : `last root: DigiCert Global Root G2` | autorité RSA hors Let's Encrypt (US-T13) |
| `github.com:443` | `CONNECT` ; `ATI` : `last root: Sectigo Public Server Authentication Root E46` | autorité ECDSA P-384 hors Let's Encrypt (US-T13) |
| `untrusted-root.badssl.com` | `TLS handshake failed` | racine absente refusée |
| `wrong.host.badssl.com` | `TLS handshake failed` | nom d'hôte vérifié |
| `expired.badssl.com` | `TLS handshake failed` | expiré : la racine COMODO RSA est dans le magasin depuis US-T13, seul le contrôle des dates refuse |
| `178.219.142.145:443` (IP) | `TLS handshake failed` | nom non vérifiable refusé |
| `telehack.com:23` | `CONNECT`, dialogue, `+++`, `ATO`, `ATH` | modem Hayes |
| PC → Pico `:6502` | `RING`, `ATA`, `NO CARRIER` | appel entrant |
| PC, serveur d'écho UDP (port libre choisi par le script) | un `+IPD` par datagramme (1, 532, 1472 o ; rafale de 12) | UDP ESP8266 (US-T14) |
| `--tnfsd hôte[:port]` (facultatif) | réponse au MOUNT, statut 0 | TNFS réel par l'UDP AT (US-T14) ; vérifié contre tnfsd 23.0207.1 le 2026-10-01 |
| `--nfs hôte` (facultatif) | montage, écriture 812 o, relecture identique, déplacement, liste, renommage, suppression | fichiers TNFS en `AT+N…` (US-T16) ; crée puis efface `/validate-<horodatage>.bin` sur le serveur |
| `http://mimuma.pl/`, `https://mimuma.pl/` (+ `Range 0-99`) | `200`, corps complet ; `206`, 100 o | flux HTTP(S) (US-T11) |
| `http://github.com/` | redirection vers https, `200` | redirections, corps chunked éventuel (US-T11) |
| `AT+APSETUP=1` | SSID `Neo6502-modem-XXXX`, station toujours associée, `CIPSTART` sortant fonctionne | point d'accès sans détourner la route par défaut (US-W6) |
| `--tnfs-usb` (facultatif) | `/dev/ttyACM1` présent, trames aller-retour pendant un lien TCP AT | second port USB (US-T17) ; le réglage d'origine est remis à la fin |

## Exécution
```
python3 validation/validate.py [/dev/ttyACM0] [--quick] [--tnfs-usb] [--tnfsd hôte[:port]] [--nfs hôte]
```
Le pare-feu du PC doit laisser entrer l'UDP sur un port quelconque (serveur d'écho de
l'étape 6) et le TCP 6502 (appel entrant).
Le script imprime ✓/✗ par étape et un tableau Markdown à coller dans
`RAPPORT-validation-<date>.md` avec le commit, la version (`ATI`) et le
réseau utilisé. Code de retour 0 = tout passe.

## Critères de réussite
- Toutes les étapes ✓ ; en particulier **les quatre refus TLS** (sinon la
  vérification est cassée : voir `ALTCP_MBEDTLS_AUTHMODE` dans `lwipopts.h`)
  et `AT+TLSTEST` avec `gcm=0` (sinon `-O3` a été réintroduit).
- Ordres de grandeur : handshake complet 2–6 s, repris < 1 s, connexions
  Prophet suivantes < 2 s.
- Mémoire (US-T13) : relever dans le rapport la valeur `heap:` (utilisé, pic,
  max) donnée par `ATI` après chaque handshake ; le pic ne doit pas croître
  avec le nombre de racines du magasin.

## Étapes manuelles avec un téléphone (US-W6, US-T12)
1. `AT+APSETUP=1` ; sur le téléphone, rejoindre `Neo6502-modem-XXXX` (mot de passe :
   `AT+APSETUPPWD?`). Attendu : la page s'ouvre seule (portail captif) sur Android et
   iOS, sinon http://192.168.4.1/ ; noter le comportement de chaque téléphone.
2. La page liste les réseaux ; choisir un réseau, mot de passe **faux** → « Mot de passe
   refusé », l'AP reste ouvert ; puis le bon → adresse IP affichée, AP fermé 15 s après.
   Vérifier que le téléphone ne perd pas l'AP pendant l'association (changement de canal :
   non vérifié à ce jour).
3. Modem sans réseau mémorisé (`AT+CWQAP` ne l'efface pas : flasher une config vierge ou
   saisir un SSID inexistant puis attendre 60 s) → l'AP s'ouvre seul.
4. Page `/hosts` : `AT+APSETUPPWD="…"` d'abord (mot de passe non public), puis cocher le
   filtrage avec `mimuma.pl` seul. En AT : `AT+NHOSTS?` → `+NHOSTS:1,"mimuma.pl"` ;
   `AT+CIPSTART="TCP","telehack.com",23` → `host not allowed` ; `AT+PING="x.example"` →
   refusé ; `AT+CWJAP_DEF=…` → `locked by host filter` ; `AT+CIPSERVER=1,23` → refusé ;
   `AT+HTTPGET="http://mimuma.pl/"` → `200` ; `AT+NLOG?` montre les refus. Décocher à la fin.

## Non couvert par le script (manuel)
- Refus sans heure SNTP (`no time (SNTP) for TLS`) : couper le réseau avant
  la synchro, ou tester sur PC (`make -C tests`).
- Transport UART GP0/GP1 (adaptateur USB-série ou UEXT) ; Neo6502 réel avec
  `netsetup.neo` / `prophet.neo`.
