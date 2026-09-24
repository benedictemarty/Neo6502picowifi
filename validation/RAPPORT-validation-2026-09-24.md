# Rapport de validation matérielle — picow-modem 0.2.0 + US-T13 — 2026-09-24

- Firmware : `picow_modem.uf2` compilé depuis le commit `fd9f879` (US-T13 : magasin de 150 racines
  Mozilla en flash, consulté à la demande), image 690 292 o ; `AT+GMR` indique encore 0.2.0
- Carte : Raspberry Pi Pico W, USB CDC `/dev/ttyACM0`, flashée par `AT+BOOTSEL` + copie sur `RPI-RP2` ;
  PC Ubuntu (GCC arm-none-eabi 14.2.1, Pico SDK 2.2.0 `~/pico-sdk-internal`, mbedTLS 3.6.2)
- Réseau : Wi-Fi 2,4 GHz domestique (SSID mémorisé en flash, saisi par le PO)
- Protocole : `PROTOCOLE.md`, script `validate.py` (complet, sans `--quick`)
- Résultat : **58/58 étapes réussies**, code de retour 0

## Critères d'acceptation de US-T13 sur carte

| Critère | Constat |
|---|---|
| Handshake vers ≥ 2 autorités différentes | ISRG Root X1 (mimuma.pl, badssl.com), **DigiCert Global Root G2** (www.digicert.com, RSA), **Sectigo Public Server Authentication Root E46** (github.com, ECDSA P-384) |
| `ATI` : nombre de racines et racine retenue | `roots: 150 in flash (on demand)`, `last root: …` conforme pour les trois autorités |
| Vérifications inchangées | refus racine inconnue, nom d'hôte faux, IP directe ; `expired.badssl.com` refusé **par la date seule** (sa racine COMODO RSA est désormais dans le magasin) |
| Tas libre mesuré | après le 1er handshake : 12 352 o utilisés (connexion TLS ouverte), **pic 32 096 o** sur 171 360 o de tas possible ; pic inchangé après DigiCert et github.com |

Limite de la mesure : l'ancien firmware (1 racine) n'affichait pas `heap:` ; la comparaison
« avant » n'a donc pas été faite sur carte. Côté PC (`tests/test_roots_ca_cb.c`, 64 bits) : pic de
6–12,5 Ko par vérification à la demande, contre 406 Ko pour décoder les 150 racines d'avance — ce
qui dépasserait les 171 Ko de tas du RP2040.

Temps : handshake complet mimuma.pl 2 039 ms (2,4–2,8 s le 2026-09-16 avec 1 racine), repris 130 ms ;
connexions Prophet 2,7 s puis 0,9 s. La recherche en flash n'a pas de coût visible.

## Résultats

| Étape | Résultat | Détail |
|---|---|---|
| AT+RST → OK | OK |  |
| reconnexion Wi-Fi automatique au boot | OK | STATUS:2 après 8s |
| ATE0 → OK | OK |  |
| ATI | OK | Neo6502drive Pico W modem 0.2.0 |
| AT+GMR | OK |  |
| AT+CWMODE? → +CWMODE:1 | OK |  |
| Wi-Fi associé (STATUS:2..4) | OK | STATUS:2 |
| AT+CIFSR (IP + MAC) | OK | +CIFSR:STAIP,"192.168.1.196" |
| Heure SNTP acquise | OK | +CIPSNTPTIME:Thu Sep 24 16:00:48 2026 |
| AT+CWJAP_CUR? (ssid, bssid, canal, rssi) | OK |  |
| AT+CIPSTA_CUR? ip/gateway/netmask | OK |  |
| AT+CIPDNS_CUR? | OK |  |
| AT+CWDHCP_DEF? | OK |  |
| AT+CIPSSLCCONF? → 2 (CA vérifiée) | OK |  |
| AT+CWLAP liste des réseaux | OK | 5 réseaux en 1.2s |
| AT+PING | OK | +45 |
| AT+TLSTEST autotests mbedTLS | OK | selftest aes=0 gcm=0 sha256=0 sha512=0 ctr_drbg=0 ecp=0 mpi=0 (0 = OK) |
| CIPSTART TCP → CONNECT | OK | 0.9s |
| STATUS:3 (TCP ouvert) | OK |  |
| CIPSEND → OK > | OK |  |
| SEND OK, +IPD, CLOSED | OK | 0.9s |
| STATUS:4 (TCP fermé) | OK |  |
| SSL mimuma.pl (Let's Encrypt) → CONNECT | OK | 2.7s |
| ATI : handshake complet mesuré | OK | 2039 ms (TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384) |
| ATI : racine ISRG Root X1 trouvée dans le magasin en flash (US-T13) | OK | 12352 used, 32096 peak, 171360 max |
| HTTPS : réponse déchiffrée en +IPD | OK |  |
| SSL mimuma.pl 2e fois (reprise de session) | OK | 0.9s |
| ATI : resumed | OK | 130 ms (TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384, resumed) |
| SSL badssl.com (ISRG Root X1) → CONNECT | OK | 3.0s |
| SSL www.digicert.com (DigiCert Global Root G2) → CONNECT | OK | 1.8s, heap: 464 used, 32096 peak, 171360 max |
| SSL github.com (Sectigo Public Server Authentication Root E46) → CONNECT | OK | 3.0s, heap: 264 used, 32096 peak, 171360 max |
| REFUS racine inconnue (untrusted-root.badssl.com) | OK | 1.8s |
| REFUS nom d'hôte faux (wrong.host.badssl.com) | OK | 2.4s |
| REFUS certificat expiré (expired.badssl.com, racine pourtant présente) | OK | 1.8s |
| REFUS IP directe (nom non vérifiable) | OK | 2.1s |
| DNS Fail sur hôte inexistant | OK |  |
| AT+TLSPORT=443 | OK |  |
| AT+TLSPORT? → 443 | OK |  |
| bloc 1 : CONNECT + réponse + CLOSED | OK | connexion 2.7s, réponse 0.9s |
| bloc 2 : CONNECT + réponse + CLOSED | OK | connexion 0.9s, réponse 0.9s |
| bloc 3 : CONNECT + réponse + CLOSED | OK | connexion 0.9s, réponse 0.9s |
| reprise : connexions 2 et 3 plus rapides que la 1re | OK | 2.7s / 0.9s / 0.9s |
| AT+TLSPORT=0 (effacement) | OK |  |
| ATDT telehack.com:23 → CONNECT | OK | 1.2s |
| données transparentes reçues (bannière) | OK | 1210 octets |
| écho serveur à "date" | OK |  |
| +++ → OK (retour commande) | OK |  |
| STATUS:3 en mode commande | OK |  |
| ATO → CONNECT | OK |  |
| +++ 2e fois → OK | OK |  |
| ATH → OK | OK |  |
| STATUS:4 après ATH | OK |  |
| AT+CIPSERVER=1,6502 | OK |  |
| RING sur appel entrant, sans +IPD avant ATA | OK |  |
| ATA → CONNECT + données du PC | OK |  |
| le PC reçoit la ligne entière | OK | b'salut du Neo6502\r\n' |
| NO CARRIER à la fermeture par le PC | OK |  |
| ATS12? → 050 | OK |  |
