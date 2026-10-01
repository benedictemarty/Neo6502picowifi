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
| `AT+GMR` | `AT version:…`, `SDK version:…`, `compile time:` (date du commit, UTC), `Bin version(Pico W):X.Y.Z` (fichier `VERSION`), `OK` |
| `AT+RST`, `AT+RESTORE` | `OK` puis redémarrage (RESTORE efface la configuration) |
| `AT+CWMODE?` / `=1` | `+CWMODE:1` (station seule ; `=2`/`=3` → `ERROR`) |
| `AT+CWJAP[_CUR|_DEF]="ssid","pass"` | `WIFI CONNECTED`, `WIFI GOT IP`, `OK` ou `+CWJAP:n`, `FAIL` |
| `AT+CWJAP[_CUR|_DEF]?` | `+CWJAP_CUR:"ssid","mac",canal,rssi` ou `No AP` |
| `AT+CWQAP` | `OK`, `WIFI DISCONNECT` |
| `AT+CWLAPOPT=…`, `AT+CWLAP[=…]` | `+CWLAP:(ecn,"ssid",rssi)` par SSID (dédoublonné, meilleur RSSI, trié), `OK` |
| `AT+CWDHCP[_CUR|_DEF]?` / `=mode,en` | `+CWDHCP_DEF:3` (bit 1 = station) |
| `AT+CIPSTATUS` | `STATUS:2|3|4|5` (+ ligne `+CIPSTATUS:0,"TCP"|"UDP",…` si un lien est ouvert) |
| `AT+CIFSR` | `+CIFSR:STAIP,"ip"`, `+CIFSR:STAMAC,"mac"` |
| `AT+CIPSTA[_CUR|_DEF]?` / `="ip","gw","mask"` | `+CIPSTA_CUR:ip:"…"`, `:gateway:`, `:netmask:` |
| `AT+CIPDNS[_CUR|_DEF]?` / `=1,"ip"` / `=0` | `+CIPDNS_CUR:ip` |
| `AT+CIPMUX?` / `=0`, `AT+CIPMODE?` / `=0` | connexion unique, mode normal (`=1` → `ERROR`) |
| `AT+CIPSSLCCONF?` / `=0` / `=2` | `+CIPSSLCCONF:2` (CA toujours vérifiée ; `=1`/`=3` cert client → `ERROR`) |
| `AT+CIPSTART="TCP","hôte",port` | `CONNECT`, `OK` ; `DNS Fail` ; `ALREADY CONNECTED` ; TLS si le port est dans `AT+TLSPORT` |
| `AT+CIPSTART="SSL","hôte",port` | idem en TLS ; `no time (SNTP) for TLS`, `TLS handshake failed` (certificat refusé…) → `ERROR` |
| `AT+CIPSTART="UDP","hôte",port` | lien UDP (port local éphémère, seuls les datagrammes de hôte:port sont reçus) : `CONNECT`, `OK` ; `DNS Fail` ; `ALREADY CONNECTED` |
| `AT+TLSPORT?` / `=443[,p2,p3,p4]` / `=0` | ports pour lesquels `"TCP"` est fait en TLS (persistant) ; `=0` efface |
| `AT+TLSTEST` | autotests mbedTLS (AES, GCM, SHA-256/512, CTR-DRBG, ECP, MPI) sur carte |
| `AT+CIPSEND=n` (n ≤ 2048, ≤ 1472 en UDP) | `OK`, `> `, puis après n octets `Recv n bytes`, `SEND OK` ; en UDP, un datagramme par `CIPSEND` |
| données entrantes | `+IPD,n:` suivi de n octets (segments ≤ 1460) ; `CLOSED` à la fermeture ; en UDP, **un `+IPD` par datagramme** (jamais regroupés ni coupés ; perdu si le tampon de 8 Ko est plein), retenus pendant un `CIPSEND` |
| `AT+CIPCLOSE` | `CLOSED`, `OK` |
| `AT+CIPSERVER=1,port` / `=0` | écoute entrante → `RING` (répété toutes les 3 s), `ATA` pour décrocher |
| `AT+CIPSNTPCFG?` / `=en,tz,"serveur"`, `AT+CIPSNTPTIME?` | SNTP lwIP ; `+CIPSNTPTIME:Tue Sep 15 12:00:00 2026` |
| `AT+PING="hôte"` | `+ms`, `OK` ou `+timeout`, `ERROR` |
| `AT+CIUPDATE` | `ERROR` (pas d'OTA : reflasher un UF2) |
| `ATI` | identité (`modem X.Y.Z`), ligne `build:` (`git describe` : `vX.Y.Z` pour une release, `vX.Y.Z-N-gSHA[-dirty]` sinon), SSID mémorisé, cause du dernier reset (`power-on`, `AT+RST`, `AT+BOOTSEL (UF2 flash)`, `reboot (bootloader or debugger)`, `watchdog timeout, stage N`, `lwip assert: …`), ligne `TLS:` (pile, nombre de racines, racine retenue au dernier handshake (`last root:`), tas newlib (`heap:` utilisé, pic, max), heure, durée et suite du dernier handshake, `resumed`, drapeaux de vérification, derniers messages lwIP/mbedTLS), `TLS ports:` |
| `AT+BOOTSEL` | `OK` puis passage en mode UF2 (`RPI-RP2`) sans toucher au bouton — spécifique à ce firmware |
| `AT+APSETUP=1` / `=0` / `?` | ouvre / ferme le point d'accès de configuration ; `+APSETUP:1,"Neo6502-modem-XXXX"` ou `+APSETUP:0` (voir ci-dessous) |
| `AT$TNFS="hôte",port` / `=hôte:port` / `=hôte` / `=0` / `?` | serveur TNFS du second port USB (port 16384 par défaut, persistant) ; `$TNFS:"hôte",port` ; `=0` efface ; commande commune avec PicoWiFiModemUSB |
| `AT$TNFSUSB=1` / `=0` / `?` | second port USB TNFS présent / absent (**défaut : absent**), persistant, pris en compte au prochain démarrage (`AT+RST`) |
| `AT&W` | `OK` (la configuration est déjà enregistrée à chaque commande) |
| `AT+HTTPGET="url"[,début[,fin]]` | requête GET (TLS pour `https://`, redirections suivies, `Range` si début/fin) : `+HTTPGET:<code>,<taille ou -1>,"<type>"`, `OK` ; voir « Flux HTTP(S) » |
| `AT+HTTPREAD=n` (1 ≤ n ≤ 2048) | `+HTTPREAD:<k>,<suite>:` puis k octets du corps, `OK` ; `suite` = 0 : corps terminé |
| `AT+HTTPCLOSE` | ferme la session HTTP, `OK` |
| `AT+NHOSTS?` | `+NHOSTS:<actif>,"hôte1",…` : filtrage des hôtes ; **lecture seule** (`AT+NHOSTS=…` → `ERROR`, modifier depuis la page `/hosts`) |
| `AT+NLOG?` | 16 dernières tentatives : `+NLOG:<âge s>,"TCP|SSL|UDP|DIAL|PING|TNFS|SNTP","hôte",port,allowed|refused` |
| `AT+APSETUPPWD="…"` / `?` | mot de passe du point d'accès (8 à 63 caractères ASCII imprimables, persistant) ; défaut `neo6502wifi` |

Non pris en charge (répond `ERROR`) : `CIPMUX=1`, forme UDP à 5 paramètres (port local, mode), `ATO`/`ATA` sur un lien UDP (`NO CARRIER`), mode point d'accès,
TLS 1.3, certificat client, mode transparent ESP (`CIPMODE=1` ; utiliser
`ATDT` à la place — `ATDT` fait aussi du TLS vers un port de `AT+TLSPORT`).

## Flux HTTP(S)

Le modem fait la requête HTTP et ne rend que le **corps**, que le 6502 lit à son
rythme : pas d'analyse HTTP ni de TLS côté 6502.

```
AT+HTTPGET="https://exemple.fr/fichier.bin"
+HTTPGET:200,51234,"application/octet-stream"
OK
AT+HTTPREAD=1024
+HTTPREAD:1024,1:<1024 octets>
OK
…
AT+HTTPREAD=1024
+HTTPREAD:34,0:<34 octets>          ← suite = 0 : fin du corps
OK
```

- `https://` : TLS terminé sur le Pico W (mêmes vérifications que `AT+CIPSTART="SSL"`).
- Redirections 301/302/303/307/308 suivies (5 au plus), y compris vers https.
- `AT+HTTPGET="url",100,199` envoie `Range: bytes=100-199` (réponse `206`) ; `,100` seul :
  du 100ᵉ octet à la fin.
- Corps `chunked` décodé ; taille `-1` quand le serveur ne l'annonce pas (fin = fermeture).
- `AT+HTTPREAD` attend au plus 10 s le premier octet et rend ce qui est déjà arrivé
  (`k` peut être inférieur à `n`) ; `+HTTPREAD:0,1:` = rien encore, réessayer.
- La session occupe le lien unique (comme `CIPSTART`) ; pas de `+IPD` pendant ce temps.
  Erreurs : `bad URL`, `DNS Fail`, `TLS handshake failed`, `host not allowed`,
  `timeout`, `connection closed`, `bad HTTP response`, `HTTP header too large` (> 2 Ko).
- Requête : `GET <chemin> HTTP/1.1`, `Host`, `User-Agent: Neo6502picowifi/<version>`,
  `Accept-Encoding: identity`, `Connection: close`.

## Hôtes autorisés (filtrage des connexions)

Pour qu'un programme `.neo` malveillant ne puisse pas envoyer le contenu du stockage
vers un serveur de son choix, le modem peut n'autoriser qu'une liste d'hôtes
(8 au plus : nom exact, `*.domaine` pour ses sous-domaines, ou adresse IP).

- **Modifiable seulement depuis la page web** du point d'accès (`AT+APSETUP=1`, puis
  http://192.168.4.1/hosts) : un programme 6502, maître du port série, ne peut pas
  élargir sa propre liste. En AT : lecture seule (`AT+NHOSTS?`) et journal (`AT+NLOG?`).
- **Désactivé par défaut** (netsetup, prophet, NeoNavigator inchangés).
- Filtrage actif : `CIPSTART` (TCP, SSL, UDP), `ATDT`, `AT$TNFS` et le port TNFS,
  `AT+PING`, `AT+HTTPGET` (et chaque redirection), un nouveau serveur `AT+CIPSNTPCFG` sont refusés (`host not allowed` /
  `NO CARRIER`) pour un hôte absent de la liste — le nom est contrôlé **avant** toute
  requête DNS (un nom peut à lui seul transporter des données) ; les **appels entrants**
  sont refusés (`CIPSERVER=1`, `ATA`, réponse automatique) ; les réglages qui permettraient
  de détourner un hôte autorisé sont **verrouillés en AT** : réseau Wi-Fi (`CWJAP`), DNS
  (`CIPDNS`), IP/passerelle (`CIPSTA`, `CWDHCP`) et mot de passe du point d'accès
  (`APSETUPPWD`) — la page web reste utilisable pour le Wi-Fi.
- Une connexion déjà ouverte n'est pas coupée quand la liste change.

Limite : la page est protégée par le mot de passe du point d'accès, **public par
défaut** ; changez-le (`AT+APSETUPPWD`) **avant** d'activer le filtrage.

## Second port USB : TNFS

**Désactivé par défaut** : le modem reste alors un seul port série USB, identique à la
0.3.x. `AT$TNFSUSB=1` puis `AT+RST` en fait un périphérique USB composite à **deux ports
série** (CDC-ACM, VID:PID `2E8A:000A`, produit « Pico W Wi-Fi modem », fabricant
« Neo6502drive ») :

| Interfaces | Nom | Rôle | Linux |
|---|---|---|---|
| 0-1 | `Modem AT` | modem AT/Hayes (inchangé) | `/dev/ttyACM0` |
| 2-3 | `TNFS` | relais TNFS (UDP) | `/dev/ttyACM1` |

Sur le port TNFS, dans les deux sens, une trame = **longueur sur 2 octets
petit-boutiste** puis le **datagramme** (1 à 1472 octets) ; une longueur invalide vide
le tampon d'entrée (resynchronisation). Chaque trame part en un datagramme UDP vers le
serveur `AT$TNFS` (DNS résolu sans bloquer au premier datagramme), chaque réponse revient
en une trame. Ce lien UDP est **indépendant du lien AT** : une session Minitel/Telnet
(`ATDT`, `CIPSTART`) et TNFS fonctionnent en même temps. Sans Wi-Fi, sans serveur, pendant
la résolution DNS, ou port TNFS fermé (DTR) : rien n'est renvoyé, le client TNFS gère ses
délais. Format convenu avec reload-emulator et Neo6502TeleStrat (client
`src/devices/neo_tnfs.h` de reload). Pas de TNFS sur l'UART.

Limite côté Neo6502 : l'hôte USB du RP2040 n'a que **15 points de terminaison pour tous
les appareils** ; le second port en ajoute 3 (7 au lieu de 4). Avec un hub, deux
périphériques HID et une clé USB (10 points relevés sur carte), le port TNFS ne tiendrait
pas et serait ignoré (le port AT, interface 0, reste monté). Analyse de Trinity 0.16.68,
non essayée sur carte.

## Configuration du Wi-Fi depuis un téléphone

Sans PC ni `netsetup` : le modem ouvre un **point d'accès de configuration**

- automatiquement au démarrage s'il n'a **aucun réseau mémorisé** ;
- automatiquement si le réseau mémorisé reste **injoignable 60 s** après le démarrage ;
- sur commande `AT+APSETUP=1`.

1. Sur le téléphone, rejoindre le réseau **`Neo6502-modem-XXXX`** (XXXX = fin de
   l'adresse MAC), mot de passe **`neo6502wifi`** (modifiable par `AT+APSETUPPWD`).
2. La page s'ouvre d'elle-même (portail captif) ; sinon ouvrir **http://192.168.4.1/**.
3. Choisir le réseau dans la liste (ou saisir son nom), entrer son mot de passe,
   **Enregistrer et se connecter**. Le réseau est mémorisé comme par `AT+CWJAP_DEF`.
4. En cas de succès, la page affiche l'adresse IP obtenue et le point d'accès se
   ferme 15 s plus tard ; en cas d'échec (mot de passe refusé, réseau introuvable),
   la page l'indique et reste disponible.

Le point d'accès se ferme aussi après **10 min sans requête** ; pour le rouvrir :
redémarrer le modem ou `AT+APSETUP=1`. Les commandes AT restent utilisables
pendant ce temps ; `ATI` affiche la ligne `setup AP:` (SSID, mot de passe, adresse).

Sécurité : le mot de passe par défaut est public (il est écrit ici) ; toute personne
à portée peut se connecter au point d'accès tant qu'il est ouvert. Le mot de passe du
réseau mémorisé n'est jamais affiché par la page. Le serveur DHCP, le DNS captif et la
page web n'écoutent que sur l'interface du point d'accès (pas sur le réseau domestique).

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
  compilation, contrôle de reproductibilité par une seconde compilation, `dist/picow_modem-vX.Y.Z.uf2`
  + SHA-256) ; `--publish` pousse `main` et le tag sur tous
  les remotes et crée la release GitHub avec l'UF2.
- `make -C tests` contrôle la cohérence `VERSION` ↔ CMake ↔ CHANGELOG ↔ tag (`tests/test_version.py`).

## Tests

```
make -C tests      # cœur du modem (test_at_modem) + dates TLS (test_tls_date) sur PC, gcc + ASan/UBSan
                   # + point d'accès de configuration : DHCP et DNS captif (test_dhcp_server), page (test_web_setup)
                   # + trames du port USB TNFS (test_tnfs_link), analyse HTTP (test_http_parse)
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
src/net_pico.[ch]     Wi-Fi (cyw43), TCP/TLS (altcp + mbedTLS), UDP, DNS/SNTP/ping, flash, watchdog/diagnostic
src/ap_pico.[ch]      point d'accès de configuration (cyw43 + lwIP) : ouverture, fermeture, HTTP
src/dhcp_server.[ch]  serveur DHCP du point d'accès (portable, testé sur PC)
src/dns_catchall.[ch] DNS captif du point d'accès (portable, testé sur PC)
src/web_setup.[ch]    page de configuration : HTTP, formulaire, portail captif (portable, testé sur PC)
src/tnfs_link.[ch]    trames du port USB TNFS et file des réponses (portable, testé sur PC)
src/tnfs_pico.[ch]    port USB TNFS : lien UDP indépendant, DNS non bloquant
src/http_parse.[ch]   flux HTTP : URL, redirections, en-têtes, « chunked » (portable, testé sur PC)
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

## Licence

EUPL 1.2 (European Union Public Licence) : voir [LICENSE](LICENSE).

## Avertissement

⚠️ Avertissement : ce programme est un programme généré par Claude Code sous la supervision d'un être humain : il a été utilisé pour améliorer, développer, rendre compatible ou traduire ce logiciel.
