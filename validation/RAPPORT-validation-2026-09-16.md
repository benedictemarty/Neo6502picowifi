# Rapport de validation matérielle — picow-modem 0.2.0 — 2026-09-16

- Firmware : `picow_modem.uf2` 0.2.0, commit de travail suivant 8a8fa7f (reconnexion de fond incluse)
- Carte : Raspberry Pi Pico W, USB CDC `/dev/ttyACM0`, PC Ubuntu (GCC arm-none-eabi 14.2.1, Pico SDK 2.2.0, mbedTLS 3.6.2)
- Réseau : partage de connexion Android 2,4 GHz (SSID mémorisé en flash, saisi par le PO)
- Protocole : `PROTOCOLE.md`, script `validate.py` (complet, sans `--quick`)
- Résultat : **55/55 étapes réussies**, code de retour 0

## Sortie du script

```

== 0. Redémarrage (état connu : pas de session TLS en mémoire) et reconnexion Wi-Fi ==
  ✓ AT+RST → OK
  ✓ reconnexion Wi-Fi automatique au boot — STATUS:2 après 11s

== 1. Identité et état ==
  ✓ ATE0 → OK
  ✓ ATI — Neo6502drive Pico W modem 0.2.0
  ✓ AT+GMR
  ✓ AT+CWMODE? → +CWMODE:1
  ✓ Wi-Fi associé (STATUS:2..4) — STATUS:2
  ✓ AT+CIFSR (IP + MAC) — +CIFSR:STAIP,"10.122.53.149"
  ✓ Heure SNTP acquise — +CIPSNTPTIME:Wed Sep 16 01:26:50 2026
  ✓ AT+CWJAP_CUR? (ssid, bssid, canal, rssi)
  ✓ AT+CIPSTA_CUR? ip/gateway/netmask
  ✓ AT+CIPDNS_CUR?
  ✓ AT+CWDHCP_DEF?
  ✓ AT+CIPSSLCCONF? → 2 (CA vérifiée)
  ✓ AT+CWLAP liste des réseaux — 6 réseaux en 1.2s
  ✓ AT+PING — +406
  ✓ AT+TLSTEST autotests mbedTLS — selftest aes=0 gcm=0 sha256=0 sha512=0 ctr_drbg=0 ecp=0 mpi=0 (0 = OK)

== 2. Séquence Prophet en clair (mimuma.pl:80) ==
  ✓ CIPSTART TCP → CONNECT — 0.9s
  ✓ STATUS:3 (TCP ouvert)
  ✓ CIPSEND → OK >
  ✓ SEND OK, +IPD, CLOSED — 0.9s
  ✓ STATUS:4 (TCP fermé)

== 3. TLS ==
  ✓ SSL mimuma.pl (Let's Encrypt) → CONNECT — 3.6s
  ✓ ATI : handshake complet mesuré — 2920 ms (TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384)
  ✓ HTTPS : réponse déchiffrée en +IPD
  ✓ SSL mimuma.pl 2e fois (reprise de session) — 1.2s
  ✓ ATI : resumed — 367 ms (TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384, resumed)
  ✓ SSL badssl.com (ISRG Root X1) → CONNECT — 3.6s
  ✓ REFUS racine inconnue (untrusted-root.badssl.com) — 2.4s
  ✓ REFUS nom d'hôte faux (wrong.host.badssl.com) — 2.7s
  ✓ REFUS certificat expiré / racine absente (expired.badssl.com) — 3.3s
  ✓ REFUS IP directe (nom non vérifiable) — 3.0s
  ✓ DNS Fail sur hôte inexistant

== 4. Scénario Prophet en TLS (AT+TLSPORT=443, client "TCP" inchangé) ==
  ✓ AT+TLSPORT=443
  ✓ AT+TLSPORT? → 443
  ✓ bloc 1 : CONNECT + réponse + CLOSED — connexion 6.0s, réponse 1.2s
  ✓ bloc 2 : CONNECT + réponse + CLOSED — connexion 1.2s, réponse 1.2s
  ✓ bloc 3 : CONNECT + réponse + CLOSED — connexion 1.2s, réponse 0.9s
  ✓ reprise : connexions 2 et 3 plus rapides que la 1re — 6.0s / 1.2s / 1.2s
  ✓ AT+TLSPORT=0 (effacement)

== 5. Modem Hayes ==
  ✓ ATDT telehack.com:23 → CONNECT — 1.2s
  ✓ données transparentes reçues (bannière) — 1185 octets
  ✓ écho serveur à "date"
  ✓ +++ → OK (retour commande)
  ✓ STATUS:3 en mode commande
  ✓ ATO → CONNECT
  ✓ +++ 2e fois → OK
  ✓ ATH → OK
  ✓ STATUS:4 après ATH
  ✓ AT+CIPSERVER=1,6502
  ✓ RING sur appel entrant, sans +IPD avant ATA
  ✓ ATA → CONNECT + données du PC
  ✓ le PC reçoit la ligne entière — b'salut du Neo6502\r\n'
  ✓ NO CARRIER à la fermeture par le PC
  ✓ ATS12? → 050

== Résumé ==
55/55 étapes réussies — firmware 0.2.0, port /dev/ttyACM0, 2026-09-16 01:28

```

## Tableau

| Étape | Résultat | Détail |
|---|---|---|
| AT+RST → OK | OK |  |
| reconnexion Wi-Fi automatique au boot | OK | STATUS:2 après 11s |
| ATE0 → OK | OK |  |
| ATI | OK | Neo6502drive Pico W modem 0.2.0 |
| AT+GMR | OK |  |
| AT+CWMODE? → +CWMODE:1 | OK |  |
| Wi-Fi associé (STATUS:2..4) | OK | STATUS:2 |
| AT+CIFSR (IP + MAC) | OK | +CIFSR:STAIP,"10.122.53.149" |
| Heure SNTP acquise | OK | +CIPSNTPTIME:Wed Sep 16 01:26:50 2026 |
| AT+CWJAP_CUR? (ssid, bssid, canal, rssi) | OK |  |
| AT+CIPSTA_CUR? ip/gateway/netmask | OK |  |
| AT+CIPDNS_CUR? | OK |  |
| AT+CWDHCP_DEF? | OK |  |
| AT+CIPSSLCCONF? → 2 (CA vérifiée) | OK |  |
| AT+CWLAP liste des réseaux | OK | 6 réseaux en 1.2s |
| AT+PING | OK | +406 |
| AT+TLSTEST autotests mbedTLS | OK | selftest aes=0 gcm=0 sha256=0 sha512=0 ctr_drbg=0 ecp=0 mpi=0 (0 = OK) |
| CIPSTART TCP → CONNECT | OK | 0.9s |
| STATUS:3 (TCP ouvert) | OK |  |
| CIPSEND → OK > | OK |  |
| SEND OK, +IPD, CLOSED | OK | 0.9s |
| STATUS:4 (TCP fermé) | OK |  |
| SSL mimuma.pl (Let's Encrypt) → CONNECT | OK | 3.6s |
| ATI : handshake complet mesuré | OK | 2920 ms (TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384) |
| HTTPS : réponse déchiffrée en +IPD | OK |  |
| SSL mimuma.pl 2e fois (reprise de session) | OK | 1.2s |
| ATI : resumed | OK | 367 ms (TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384, resumed) |
| SSL badssl.com (ISRG Root X1) → CONNECT | OK | 3.6s |
| REFUS racine inconnue (untrusted-root.badssl.com) | OK | 2.4s |
| REFUS nom d'hôte faux (wrong.host.badssl.com) | OK | 2.7s |
| REFUS certificat expiré / racine absente (expired.badssl.com) | OK | 3.3s |
| REFUS IP directe (nom non vérifiable) | OK | 3.0s |
| DNS Fail sur hôte inexistant | OK |  |
| AT+TLSPORT=443 | OK |  |
| AT+TLSPORT? → 443 | OK |  |
| bloc 1 : CONNECT + réponse + CLOSED | OK | connexion 6.0s, réponse 1.2s |
| bloc 2 : CONNECT + réponse + CLOSED | OK | connexion 1.2s, réponse 1.2s |
| bloc 3 : CONNECT + réponse + CLOSED | OK | connexion 1.2s, réponse 0.9s |
| reprise : connexions 2 et 3 plus rapides que la 1re | OK | 6.0s / 1.2s / 1.2s |
| AT+TLSPORT=0 (effacement) | OK |  |
| ATDT telehack.com:23 → CONNECT | OK | 1.2s |
| données transparentes reçues (bannière) | OK | 1185 octets |
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

## Observations

- Exécutions précédentes du même protocole : deux faux négatifs du script corrigés (critère `200` sur le port 80 devenu `301`, session TLS encore en mémoire → ajout du `AT+RST` initial), et **un vrai défaut** : après `AT+RST` la carte pouvait rester en `STATUS:5` sans jamais retenter le Wi-Fi → reconnexion de fond toutes les 15 s ajoutée (tentative immédiate au boot, commandes AT disponibles pendant ce temps).
- Non couvert : refus sans heure SNTP (testé sur PC), transport UART, Neo6502 réel.
