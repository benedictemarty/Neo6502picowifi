# Rapport de validation — modem simulé sur PC — 2026-10-01

**78/78 étapes réussies.** Cible : `pc/pcmodem` (cœur du firmware `src/at_modem.c`,
`http_parse.c`, `tnfs_link.c` inchangés ; TLS = mbedTLS du SDK avec la configuration du
firmware, `roots_store` et `roots_ca_cb`), sur le réseau du PC, Internet réel.
Code : branche `main` après `97ac2c2` + corrections de ce rapport. Version annoncée : 0.3.1
(VERSION non encore incrémenté).

Commande :
```
make -C pc PICO_SDK_PATH=~/pico-sdk-internal
pc/pcmodem -l /tmp/neomodem -T /tmp/neotnfs -c /tmp/pcm.cfg &
python3 validation/validate.py /tmp/neomodem --pc --tnfs-pty /tmp/neotnfs
```

## Ce que ce rapport prouve et ne prouve pas
- Prouvé sur le vrai réseau : dialogue AT (netinfo, netsetup, Prophet), TLS (4 autorités, 4 refus,
  vérification des noms), Hayes (telehack, `+++`, `ATO`, appel entrant), UDP (un `+IPD` par
  datagramme, rafale), flux HTTP(S) (redirection, chunked, 64 Ko d'un corps de 576 Ko), lecture seule
  du filtrage et journal, trames du port TNFS pendant un lien TCP.
- **Non prouvé** (propre à la carte) : cyw43/lwIP, mémoire du RP2040 (tas), `AT+TLSTEST`, reprise
  de session TLS, point d'accès (US-W6), USB composite (US-T17), dates par SNTP (sur PC : horloge
  système). La validation sur carte reste nécessaire avant la release.

## Défauts trouvés et corrigés grâce à cette validation
1. **TLS : enregistrements de 16 Ko refusés** — `MBEDTLS_SSL_IN_CONTENT_LEN` valait 8192 ; un serveur
   qui envoie des enregistrements pleins (github.com) faisait échouer la lecture (`-0x7100`,
   « requesting more data than fits »). Le défaut existe **aussi dans le firmware** (même
   configuration). Corrigé : 16384 (maximum RFC). Coût : +8 Ko de tas par connexion TLS, à mesurer
   sur carte (`ATI` : `heap:`).
2. **HTTP : en-têtes de plus de 2 Ko refusés** (github.com : politique de sécurité de contenu de
   plusieurs Ko) — `AT+HTTPGET` répondait `HTTP header too large`. Corrigé : lecture ligne par ligne,
   seuls le statut et les en-têtes utiles sont gardés (taille totale illimitée, RAM inchangée).
3. Deux attentes du script lui-même (page de 95 o pour un `Range 0-99`, limite de lecture).

## Résultats

| Étape | Résultat | Détail |
|---|---|---|
| AT+RST → OK | OK |  |
| reconnexion Wi-Fi automatique au boot | OK | STATUS:2 après 3s |
| ATE0 → OK | OK |  |
| ATI | OK | Neo6502drive Pico W modem 0.3.1 |
| AT+GMR | OK |  |
| AT+CWMODE? → +CWMODE:1 | OK |  |
| Wi-Fi associé (STATUS:2..4) | OK | STATUS:2 |
| AT+CIFSR (IP + MAC) | OK | +CIFSR:STAIP,"10.57.1.59" |
| Heure SNTP acquise | OK | +CIPSNTPTIME:Thu Oct 01 17:47:27 2026 |
| AT+CWJAP_CUR? (ssid, bssid, canal, rssi) | OK |  |
| AT+CIPSTA_CUR? ip/gateway/netmask | OK |  |
| AT+CIPDNS_CUR? | OK |  |
| AT+CWDHCP_DEF? | OK |  |
| AT+CIPSSLCCONF? → 2 (CA vérifiée) | OK |  |
| AT+CWLAP liste des réseaux | OK | 1 réseaux en 0.9s |
| AT+PING | OK | +158 |
| CIPSTART TCP → CONNECT | OK | 0.9s |
| STATUS:3 (TCP ouvert) | OK |  |
| CIPSEND → OK > | OK |  |
| SEND OK, +IPD, CLOSED | OK | 0.9s |
| STATUS:4 (TCP fermé) | OK |  |
| SSL mimuma.pl (Let's Encrypt) → CONNECT | OK | 1.2s |
| HTTPS : réponse déchiffrée en +IPD | OK |  |
| SSL mimuma.pl 2e fois (reprise de session) | OK | 1.2s |
| SSL badssl.com (ISRG Root X1) → CONNECT | OK | 1.5s |
| SSL www.digicert.com (DigiCert Global Root G2) → CONNECT | OK | 1.2s, heap: ? |
| SSL github.com (Sectigo Public Server Authentication Root E46) → CONNECT | OK | 1.2s, heap: ? |
| REFUS racine inconnue (untrusted-root.badssl.com) | OK | 1.2s |
| REFUS nom d'hôte faux (wrong.host.badssl.com) | OK | 1.2s |
| REFUS certificat expiré (expired.badssl.com, racine pourtant présente) | OK | 1.2s |
| REFUS IP directe (nom non vérifiable) | OK | 0.9s |
| DNS Fail sur hôte inexistant | OK |  |
| AT+TLSPORT=443 | OK |  |
| AT+TLSPORT? → 443 | OK |  |
| bloc 1 : CONNECT + réponse + CLOSED | OK | connexion 1.2s, réponse 0.9s |
| bloc 2 : CONNECT + réponse + CLOSED | OK | connexion 1.2s, réponse 0.9s |
| bloc 3 : CONNECT + réponse + CLOSED | OK | connexion 1.2s, réponse 0.9s |
| AT+TLSPORT=0 (effacement) | OK |  |
| ATDT telehack.com:23 → CONNECT | OK | 1.2s |
| données transparentes reçues (bannière) | OK | 1201 octets |
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
| CIPSTART "UDP" → CONNECT | OK | PC 10.57.1.59:59345 |
| CIPSTATUS : lien "UDP" | OK |  |
| datagramme de 1 o : SEND OK puis un +IPD identique | OK | 1 +IPD |
| datagramme de 532 o : SEND OK puis un +IPD identique | OK | 1 +IPD |
| datagramme de 1472 o : SEND OK puis un +IPD identique | OK | 1 +IPD |
| 12 datagrammes en rafale → 12 +IPD, ni regroupés ni coupés, dans l'ordre | OK | 12 +IPD |
| CIPSEND=1473 en UDP → ERROR | OK |  |
| ATO sur lien UDP → NO CARRIER | OK |  |
| CIPCLOSE → CLOSED | OK |  |
| HTTPGET http://mimuma.pl/ → 200, corps complet | OK | 200, annoncé 95, lu 95 o en 1 lectures, 1.5s |
| HTTPGET https://mimuma.pl/ (TLS) → 200, corps complet | OK | 200, annoncé 95, lu 95 o, 1.2s |
| Range 0-99 → 206, au plus 100 octets, taille annoncée lue | OK | 206, annoncé 95, lu 95 o |
| redirection http://github.com → https, 200, 64 Ko de corps lus (en-têtes > 4 Ko, TLS 16 Ko) | OK | 200, annoncé -1, lu 65536 o, 2.1s |
| HTTPCLOSE → OK | OK |  |
| URL invalide → bad URL | OK |  |
| après HTTP, CIPSTART/CIPCLOSE normaux | OK |  |
| AT+NHOSTS? → filtrage inactif par défaut | OK | +NHOSTS:0 |
| AT+NHOSTS=… refusé (lecture seule) | OK |  |
| AT+NLOG? journalise les connexions de la session | OK | 16 entrées |
| /tmp/claude-1000/-home-bmarty-Neo6502picowifi/920c0b4f-095e-4927-9be0-d3dd2be3e5a1/scratchpad/neotnfs présent (interface « TNFS ») | OK |  |
| AT$TNFS=PC → OK | OK |  |
| lien AT TCP ouvert en parallèle | OK |  |
| 3 trames (5, 532, 1472 o) aller-retour par le PC, identiques | OK | 3/3 |
| longueur invalide → resynchronisation, trame suivante servie | OK |  |
| lien AT toujours vivant pendant TNFS (CIPCLOSE → CLOSED) | OK |  |
