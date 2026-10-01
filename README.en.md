# Neo6502picowifi — Wi-Fi modem for the Neo6502 on a Raspberry Pi Pico W (formerly `Neo6502drive/firmware/picow-modem`)

*Version française : [README.md](README.md) (reference document; French is the
project's working language — backlog, changelog and commit messages).*

C firmware (Pico SDK 2.x, cyw43 + lwIP, TinyUSB) that turns a **Pico W** into a
Wi-Fi modem for the Neo6502 (stories US-T1 and US-T2 in `docs/BACKLOG.md`).

## What it does

- **ESP8266 AT dialect**: the exact subset used by `netsetup.neo`,
  `netinfo.neo`, `netconsole.neo` (gitlab.com/bocianu/neo-networking) and
  `prophet.neo` / `pget.neo` (neo-prophet), taken from their sources
  (`uart.inc`, `http.inc`, `*.pas`). These programs run **unmodified**,
  believing they talk to a MOD-WIFI-ESP8266.
- **Hayes modem**: `ATDT host:port` (outgoing TCP, transparent mode),
  `+++` (1 s guard time, S12), `ATO`, `ATH`, `ATA`, `ATE0/1`, `ATZ`,
  `ATI`, `ATS0/S2/S12`, `RING` on incoming call (`AT+CIPSERVER=1,port`).
- **Two simultaneous transports**, responses sent on both:
  - USB CDC-ACM (`/dev/ttyACM*` on a PC; on the Neo6502 with a firmware that
    hosts CDC devices — the `trinity` branch of the Neo6502 firmware fork
    recognises it, **the official Neo6502 firmware currently ignores CDC
    devices**);
  - UART0 GP0 (TX) / GP1 (RX), 115200 8N1: the Neo6502 UEXT connector,
    usable **right now** (wiring: `Neo6502drive/hardware/PICOW_UEXT.md`).
- Persistent configuration (last flash sector): SSID/password
  (`AT+CWJAP_DEF`), echo, DHCP/static IP, DNS, SNTP, listening port, S0.
  The Pico W joins the last saved network in the background (at boot and after
  a drop: retry every 15 s), like the ESP.
- **TLS terminated on the Pico W** (mbedTLS 3.6, TLS 1.2 client):
  `AT+CIPSTART="SSL",…` or, for clients that cannot be changed such as
  `prophet.neo`, `AT+TLSPORT=443`, which makes every `"TCP"` connection to that
  port a TLS one. The certificate is **always verified** (chain against the
  the 150 Mozilla roots `certs/roots.pem`, kept in flash and decoded on demand,
  host name/SNI, dates from SNTP time —
  refused if the time has not been acquired). Session resumption (ticket) for
  the same host:port. Details: § TLS.
- Board LED: on = Wi-Fi associated, blinking = TCP connection open.
- 8 s watchdog: a hang reboots the board; `ATI` reports the cause and the
  stage (`net_pico_stage`) or the lwIP assertion message.

## Supported AT commands

| Command | Response (ESP8266 AT 1.x format) |
|---|---|
| `AT`, `ATE0`, `ATE1` | `OK` |
| `AT+GMR` | `AT version:…`, `SDK version:…`, `compile time:` (commit date, UTC), `Bin version(Pico W):X.Y.Z` (`VERSION` file), `OK` |
| `AT+RST`, `AT+RESTORE` | `OK` then reboot (RESTORE erases the configuration) |
| `AT+CWMODE?` / `=1` | `+CWMODE:1` (station only; `=2`/`=3` → `ERROR`) |
| `AT+CWJAP[_CUR|_DEF]="ssid","pass"` | `WIFI CONNECTED`, `WIFI GOT IP`, `OK` or `+CWJAP:n`, `FAIL` |
| `AT+CWJAP[_CUR|_DEF]?` | `+CWJAP_CUR:"ssid","mac",channel,rssi` or `No AP` |
| `AT+CWQAP` | `OK`, `WIFI DISCONNECT` |
| `AT+CWLAPOPT=…`, `AT+CWLAP[=…]` | `+CWLAP:(ecn,"ssid",rssi)` per SSID (deduplicated, best RSSI, sorted), `OK` |
| `AT+CWDHCP[_CUR|_DEF]?` / `=mode,en` | `+CWDHCP_DEF:3` (bit 1 = station) |
| `AT+CIPSTATUS` | `STATUS:2|3|4|5` (+ a `+CIPSTATUS:0,"TCP"|"UDP",…` line if a link is open) |
| `AT+CIFSR` | `+CIFSR:STAIP,"ip"`, `+CIFSR:STAMAC,"mac"` |
| `AT+CIPSTA[_CUR|_DEF]?` / `="ip","gw","mask"` | `+CIPSTA_CUR:ip:"…"`, `:gateway:`, `:netmask:` |
| `AT+CIPDNS[_CUR|_DEF]?` / `=1,"ip"` / `=0` | `+CIPDNS_CUR:ip` |
| `AT+CIPMUX?` / `=0`, `AT+CIPMODE?` / `=0` | single connection, normal mode (`=1` → `ERROR`) |
| `AT+CIPSSLCCONF?` / `=0` / `=2` | `+CIPSSLCCONF:2` (CA always verified; `=1`/`=3` client cert → `ERROR`) |
| `AT+CIPSTART="TCP","host",port` | `CONNECT`, `OK`; `DNS Fail`; `ALREADY CONNECTED`; TLS if the port is listed in `AT+TLSPORT` |
| `AT+CIPSTART="SSL","host",port` | same over TLS; `no time (SNTP) for TLS`, `TLS handshake failed` (certificate refused…) → `ERROR` |
| `AT+CIPSTART="UDP","host",port` | UDP link (ephemeral local port, only datagrams from host:port are received): `CONNECT`, `OK`; `DNS Fail`; `ALREADY CONNECTED` |
| `AT+TLSPORT?` / `=443[,p2,p3,p4]` / `=0` | ports for which `"TCP"` is done over TLS (persistent); `=0` clears |
| `AT+TLSTEST` | mbedTLS self-tests (AES, GCM, SHA-256/512, CTR-DRBG, ECP, MPI) on the board |
| `AT+CIPSEND=n` (n ≤ 2048, ≤ 1472 over UDP) | `OK`, `> `, then after n bytes `Recv n bytes`, `SEND OK`; over UDP, one datagram per `CIPSEND` |
| incoming data | `+IPD,n:` followed by n bytes (segments ≤ 1460); `CLOSED` when the peer closes; over UDP, **one `+IPD` per datagram** (never merged nor split; dropped if the 8 KB buffer is full), held back during a `CIPSEND` |
| `AT+CIPCLOSE` | `CLOSED`, `OK` |
| `AT+CIPSERVER=1,port` / `=0` | incoming listener → `RING` (repeated every 3 s), `ATA` to answer |
| `AT+CIPSNTPCFG?` / `=en,tz,"server"`, `AT+CIPSNTPTIME?` | lwIP SNTP; `+CIPSNTPTIME:Tue Sep 15 12:00:00 2026` |
| `AT+PING="host"` | `+ms`, `OK` or `+timeout`, `ERROR` |
| `AT+CIUPDATE` | `ERROR` (no OTA: reflash a UF2) |
| `ATI` | identity (`modem X.Y.Z`), `build:` line (`git describe`: `vX.Y.Z` for a release, `vX.Y.Z-N-gSHA[-dirty]` otherwise), saved SSID, last reset cause (`power-on`, `AT+RST`, `AT+BOOTSEL (UF2 flash)`, `reboot (bootloader or debugger)`, `watchdog timeout, stage N`, `lwip assert: …`), `TLS:` line (stack, number of roots, root used by the last handshake (`last root:`), newlib heap (`heap:` used, peak, max), time, duration and cipher suite of the last handshake, `resumed`, verification flags, last lwIP/mbedTLS messages), `TLS ports:` |
| `AT+BOOTSEL` | `OK` then switch to UF2 mode (`RPI-RP2`) without touching the button — specific to this firmware |
| `AT+APSETUP=1` / `=0` / `?` | opens / closes the setup access point; `+APSETUP:1,"Neo6502-modem-XXXX"` or `+APSETUP:0` (see below) |
| `AT$TNFS="host",port` / `=host:port` / `=host` / `=0` / `?` | TNFS server of the second USB port (port 16384 by default, persistent); `$TNFS:"host",port`; `=0` clears; same command as PicoWiFiModemUSB |
| `AT$TNFSUSB=1` / `=0` / `?` | second TNFS USB port present / absent (**default: absent**), persistent, applied at the next boot (`AT+RST`) |
| `AT&W` | `OK` (the configuration is already saved by every command) |
| `AT+HTTPGET="url"[,start[,end]]` | GET request (TLS for `https://`, redirects followed, `Range` if start/end): `+HTTPGET:<code>,<size or -1>,"<type>"`, `OK`; see "HTTP(S) streaming" |
| `AT+HTTPREAD=n` (1 ≤ n ≤ 2048) | `+HTTPREAD:<k>,<more>:` then k body bytes, `OK`; `more` = 0: body finished |
| `AT+HTTPCLOSE` | closes the HTTP session, `OK` |
| `AT+NMOUNT="host"[,port[,"/path"[,"user","pass"]]]` / `?` | mounts a TNFS server (port 16384 by default): `+NMOUNT:<version>`; see "Remote files (TNFS)" |
| `AT+NOPEN="path"[,mode]` | mode 0 read (default), 1 write (create, truncate), 2 append, 3 read/write: `+NOPEN:<h>` |
| `AT+NREAD=h,n` (n ≤ 512) | `+NREAD:<k>:` then k bytes, `OK`; k = 0: end of file |
| `AT+NWRITE=h,n` (n ≤ 512) | `OK`, `> `, then after n bytes `+NWRITE:<k>`, `OK` |
| `AT+NCLOSE=h`, `AT+NSEEK=h,pos[,0|1|2]` | close; seek (start, current, end): `+NSEEK:<pos>` |
| `AT+NSTAT="path"`, `AT+NDIR="dir"` | `+NSTAT:<size>,<dir 0|1>,<mtime>`; one `+NDIR:"name",<size>,<0|1>` line per entry |
| `AT+NDEL`, `AT+NMKDIR`, `AT+NRMDIR="path"`, `AT+NREN="from","to"`, `AT+NUMOUNT` | delete, directories, rename, unmount |
| `AT+NHOSTS?` | `+NHOSTS:<on>,"host1",…`: host filter; **read-only** (`AT+NHOSTS=…` → `ERROR`, change it from the `/hosts` page) |
| `AT+NLOG?` | last 16 attempts: `+NLOG:<age s>,"TCP|SSL|UDP|DIAL|PING|TNFS|SNTP","host",port,allowed|refused` |
| `AT+APSETUPPWD="…"` / `?` | access point password (8 to 63 printable ASCII characters, persistent); default `neo6502wifi` |

Not supported (answers `ERROR`): `CIPMUX=1`, 5-parameter UDP form (local port, mode), `ATO`/`ATA` on a UDP link (`NO CARRIER`), access-point mode, TLS 1.3,
client certificate, ESP transparent mode (`CIPMODE=1`; use `ATDT` instead —
`ATDT` also does TLS towards a port listed in `AT+TLSPORT`).

## HTTP(S) streaming

The modem makes the HTTP request and only returns the **body**, which the 6502 reads
at its own pace: no HTTP parsing nor TLS on the 6502 side.

```
AT+HTTPGET="https://example.com/file.bin"
+HTTPGET:200,51234,"application/octet-stream"
OK
AT+HTTPREAD=1024
+HTTPREAD:1024,1:<1024 bytes>
OK
…
AT+HTTPREAD=1024
+HTTPREAD:34,0:<34 bytes>           ← more = 0: end of body
OK
```

- `https://`: TLS terminated on the Pico W (same checks as `AT+CIPSTART="SSL"`).
- 301/302/303/307/308 redirects followed (5 at most), including towards https.
- `AT+HTTPGET="url",100,199` sends `Range: bytes=100-199` (`206` reply); `,100` alone:
  from byte 100 to the end.
- `chunked` bodies decoded; size `-1` when the server does not announce it (end = close).
- `AT+HTTPREAD` waits up to 10 s for the first byte and returns what has already arrived
  (`k` may be less than `n`); `+HTTPREAD:0,1:` = nothing yet, try again.
- The session uses the single link (like `CIPSTART`); no `+IPD` meanwhile. Errors:
  `bad URL`, `DNS Fail`, `TLS handshake failed`, `host not allowed`, `timeout`,
  `connection closed`, `bad HTTP response`, `HTTP header too large` (> 2 KB).
- Request: `GET <path> HTTP/1.1`, `Host`, `User-Agent: Neo6502picowifi/<version>`,
  `Accept-Encoding: identity`, `Connection: close`.

## Remote files (TNFS)

Any Neo6502 program can read and write files on a **TNFS** server (FujiNet/Spectranet
`tnfsd`) with plain AT commands: the modem runs the protocol (sessions, sequence numbers,
retries, `EAGAIN`); this is the basis of the `N:` device (Neo6502Prophet memo).

```
AT+NMOUNT="server.local"             → +NMOUNT:1.2
AT+NOPEN="/games/tetris.neo"         → +NOPEN:3
AT+NREAD=3,512                       → +NREAD:512:<512 bytes>  …  +NREAD:0:  (end)
AT+NCLOSE=3
AT+NOPEN="/save.dat",1  AT+NWRITE=4,100  > <100 bytes>  → +NWRITE:100
AT+NDIR="/games"                     → +NDIR:"tetris.neo",20480,0 …
```

- Errors: `+NERR:<code>,"<name>"` then `ERROR`; TNFS server codes (2 `ENOENT`, 6 `EBADF`,
  9 `EACCES`, 0x21 `EOF`…); 256 `TIMEOUT` (no reply after 4 tries of 1.5 s), 257
  `BADREPLY`, 258 `NOTMOUNTED`, 259 `BADARG`.
- Own UDP link, independent from `CIPSTART`/`ATDT` and the TNFS USB port; the host filter
  (US-T12) applies to `AT+NMOUNT`.
- In `+NDIR`, a `"` in a name is shown as `'`; sorted list, directories first, hidden files
  omitted (default `OPENDIRX` options).
- Protocol: TNFS specification (spectranet/tnfs/tnfs-protocol.md) and `tnfsd` constants.

## Allowed hosts (connection filter)

So that a malicious `.neo` program cannot send the storage contents to a server of its
choice, the modem can allow only a list of hosts (8 at most: exact name, `*.domain` for
its subdomains, or IP address).

- **Only changeable from the web page** of the access point (`AT+APSETUP=1`, then
  http://192.168.4.1/hosts): a 6502 program, which owns the serial port, cannot widen
  its own list. Over AT: read-only (`AT+NHOSTS?`) and log (`AT+NLOG?`).
- **Disabled by default** (netsetup, prophet, NeoNavigator unchanged).
- When active: `CIPSTART` (TCP, SSL, UDP), `ATDT`, `AT$TNFS` and the TNFS port,
  `AT+PING`, `AT+HTTPGET` (and each redirect), `AT+NMOUNT`, a new `AT+CIPSNTPCFG` server are refused (`host not allowed` / `NO CARRIER`)
  for a host not in the list — the name is checked **before** any DNS query (a name alone
  can carry data); **incoming calls** are refused (`CIPSERVER=1`, `ATA`, auto answer);
  settings that would allow hijacking an allowed host are **locked over AT**: Wi-Fi
  network (`CWJAP`), DNS (`CIPDNS`), IP/gateway (`CIPSTA`, `CWDHCP`) and the access point
  password (`APSETUPPWD`) — the web page still works for Wi-Fi.
- A connection already open is not cut when the list changes.

Limit: the page is protected by the access point password, **public by default**;
change it (`AT+APSETUPPWD`) **before** turning the filter on.

## Second USB port: TNFS

**Disabled by default**: the modem then stays a single USB serial port, identical to
0.3.x. `AT$TNFSUSB=1` then `AT+RST` turn it into a composite USB device with **two serial
ports** (CDC-ACM, VID:PID `2E8A:000A`, product "Pico W Wi-Fi modem", manufacturer
"Neo6502drive"):

| Interfaces | Name | Role | Linux |
|---|---|---|---|
| 0-1 | `Modem AT` | AT/Hayes modem (unchanged) | `/dev/ttyACM0` |
| 2-3 | `TNFS` | TNFS relay (UDP) | `/dev/ttyACM1` |

On the TNFS port, in both directions, one frame = **2-byte little-endian length**
followed by the **datagram** (1 to 1472 bytes); an invalid length flushes the input
buffer (resynchronisation). Each frame goes out as one UDP datagram to the `AT$TNFS`
server (DNS resolved without blocking on the first datagram); each reply comes back as
one frame. This UDP link is **independent from the AT link**: a Minitel/Telnet session
(`ATDT`, `CIPSTART`) and TNFS work at the same time. Without Wi-Fi, without a server,
during DNS resolution, or with the TNFS port closed (DTR): nothing is sent back, the TNFS
client handles its own timeouts. Format agreed with reload-emulator and Neo6502TeleStrat
(reload's `src/devices/neo_tnfs.h` client). No TNFS over the UART.

Neo6502 limit: the RP2040 USB host only has **15 endpoints for all devices**; the
second port adds 3 (7 instead of 4). With a hub, two HID devices and a USB stick (10
endpoints measured on the board), the TNFS port would not fit and would be skipped (the
AT port, interface 0, stays mounted). Analysis of Trinity 0.16.68, not tried on the board.

## Setting up Wi-Fi from a phone

No PC nor `netsetup` needed: the modem opens a **setup access point**

- automatically at boot when **no network is saved**;
- automatically when the saved network is still **unreachable 60 s** after boot;
- on request with `AT+APSETUP=1`.

1. On the phone, join the **`Neo6502-modem-XXXX`** network (XXXX = end of the MAC
   address), password **`neo6502wifi`** (can be changed with `AT+APSETUPPWD`).
2. The page opens by itself (captive portal); otherwise browse to **http://192.168.4.1/**.
3. Pick the network from the list (or type its name), enter its password, then
   **Enregistrer et se connecter** (save and connect; the page is in French). The
   network is saved as with `AT+CWJAP_DEF`.
4. On success the page shows the IP address obtained and the access point closes
   15 s later; on failure (password refused, network not found) the page says so and
   stays available.

The access point also closes after **10 min without any request**; to reopen it,
restart the modem or send `AT+APSETUP=1`. AT commands keep working meanwhile; `ATI`
shows a `setup AP:` line (SSID, password, address).

Security: the default password is public (it is written here); anyone in range can
join the access point while it is open. The page never shows the saved network's
password. The DHCP server, captive DNS and web page only listen on the access point
interface (not on the home network).

## TLS

- Stack: lwIP `altcp_tls` + mbedTLS 3.6.2 (`src/mbedtls_config.h`); TLS 1.2,
  ECDHE-ECDSA / ECDHE-RSA / RSA, AES-GCM, SHA-256/384, P-256, P-384, X25519.
- Verification: `MBEDTLS_SSL_VERIFY_REQUIRED` (forced in `lwipopts.h` **and**
  in the code — altcp's default is `OPTIONAL`, which would let an invalid
  certificate through), SNI + host name, dates checked in a callback
  (`tls_date.c`, without `gmtime_r`, which would block in the lwIP context),
  SNTP time required. No "accept anything" mode.
- Roots (US-T13): `certs/roots.pem` = Mozilla store (150 roots, Ubuntu
  `ca-certificates` package; provenance and updates: `certs/README.md`).
  `tools/roots2c.py` compiles them into concatenated DER (159,591 B, in flash)
  plus an index sorted by the FNV-1a hash of the subject (`src/roots_store.c`).
  No root is loaded into RAM up front: during verification mbedTLS calls
  `roots_ca_cb` (`src/roots_ca_cb.c`, `MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK`)
  with the certificate whose issuer it is looking for; only the roots with that
  subject are decoded, without copying the DER
  (`mbedtls_x509_crt_parse_der_nocopy`), then freed by mbedTLS. PC measurement
  (64-bit): 6–12.5 KB peak during a verification, versus 406 KB to decode all
  150 roots up front. Excluded at generation time: RSA keys < 2048 bits and
  curves other than P-256/P-384.
- Session resumption: ticket kept per host:port and reused on the next
  connection (`prophet.neo` opens one connection per `Range` block).
- Measured on the board (2026-09-16, Apache + Let's Encrypt,
  ECDHE-RSA-AES256-GCM): full handshake **2.4–2.8 s** (4-certificate chain
  verified in ~1.7 s), resumed handshake **0.3–0.5 s**; complete
  `AT+CIPSTART`: 3–4 s the first time, **1.2 s** afterwards. letsencrypt.org
  (ECDSA, CDN): 5 s. Flash 523 KB, BSS 85 KB, mbedTLS on the newlib heap.
  Since US-T13: 690 KB image (build measurement, 2026-09-24), BSS unchanged.
- Pitfalls met: (1) **`-O3` (GCC 14.2.1, armv6-m) produces a wrong AES-GCM**
  in `gcm.c` → `bad_record_mac` from every server; the project builds with
  `-O2` (`CMakeLists.txt`), `AT+TLSTEST` checks it; (2) the handshake runs in
  the lwIP IRQ and may exceed the 8 s watchdog: a high-priority timer feeds it
  during the handshake (60 s max); (3) `MBEDTLS_ECP_WINDOW_SIZE 4`: 4× faster
  than the default.

## Building

```
export PICO_SDK_PATH=/path/to/pico-sdk     # SDK 2.x with lib/cyw43-driver, lwip, tinyusb
cmake -S . -B build && cmake --build build # from the repository root
```

Output: `build/picow_modem.uf2`. Flashing: plug the Pico W in while holding
**BOOTSEL** (or send `AT+BOOTSEL` to the modem), then copy the UF2 onto the
`RPI-RP2` volume.

UART speed: `-DUART_BAUD=…` in `target_compile_definitions` (default 115200 =
the value in `netsetup.pas`).

## Versions and releases

- Version: the `VERSION` file (semver) is the single source; `CMakeLists.txt` reads it, `AT+GMR` and
  `ATI` show it. `ATI` also shows the build id (`git describe`, recomputed on every build):
  `build: v0.3.0` = the release UF2, any other suffix = a work build.
- Release: bump `VERSION`, move the CHANGELOG `[Unreleased]` entries under `## X.Y.Z — date`,
  commit, then `PICO_SDK_PATH=… tools/release.sh` (tests, `vX.Y.Z` tag, build, reproducibility check
  through a second build,
  `dist/picow_modem-vX.Y.Z.uf2` + SHA-256); `--publish` pushes `main` and the tag to every remote
  and creates the GitHub release with the UF2.
- `make -C tests` checks `VERSION` ↔ CMake ↔ CHANGELOG ↔ tag consistency (`tests/test_version.py`).

## Tests

```
make -C tests      # modem core (test_at_modem) + TLS dates (test_tls_date) on the PC, gcc + ASan/UBSan
                   # + setup access point: DHCP and captive DNS (test_dhcp_server), page (test_web_setup)
                   # + TNFS USB port frames (test_tnfs_link), HTTP parsing (test_http_parse)
                   # + TNFS client (test_tnfs_client; TNFSD=<tnfsd binary>: real session)
                   # + root store: generator (test_roots2c.py), lookup (test_roots_store),
                   #   callback against mbedTLS (test_roots_ca_cb, needs PICO_SDK_PATH or MBEDTLS_DIR);
                   #   without mbedTLS that last test is SKIPPED with an explicit message
```

`src/at_modem.c` depends on no Pico API: the harness `tests/test_at_modem.c`
replays the exact sequences of netinfo, netsetup, prophet
(`CIPSTART` → `CIPSEND` → `+IPD` → `CLOSED`) and of the Hayes modem.

Automated on-board validation: `python3 validation/validate.py` (protocol
`validation/PROTOCOLE.md`, reports `validation/RAPPORT-*.md`, in French) — 58
steps: identity, Wi-Fi, SNTP, Prophet sequence in clear and over TLS,
non-Let's Encrypt authorities (DigiCert, Sectigo), TLS refusals (unknown root, wrong host name, expired, bare IP), Hayes, incoming
call. Prerequisite: Wi-Fi provisioned once with `screen /dev/ttyACM0 115200`
and `AT+CWJAP_DEF="ssid","pass"`.

Anything not measured on a real board is marked "non validé" (not validated)
in the French documents.

## Modem simulated on the PC

`pc/pcmodem` runs the firmware core (`src/at_modem.c`, `http_parse.c`, `tnfs_link.c`,
unchanged) on the PC network, exposed as a virtual serial port: validation without the
board, and a reference modem for emulators (reload, Phosphoneo…).

```
make -C pc PICO_SDK_PATH=~/pico-sdk-internal    # TLS: SDK mbedTLS, firmware config
pc/pcmodem -l /tmp/neomodem [-T /tmp/neotnfs] [-c pcmodem.cfg]
python3 validation/validate.py /tmp/neomodem --pc [--tnfs-pty /tmp/neotnfs]
```

- `-l`: link to the AT port; `-T`: second "TNFS" port (length + datagram frames);
  `-c`: persistent configuration (file).
- TLS with the same cipher suites, root store and `roots_ca_cb` callback as the firmware;
  without `PICO_SDK_PATH`, built without TLS (reported).
- Differences: simulated Wi-Fi (the PC network; `AT+CWJAP` accepts any SSID), no access
  point, no TLS session resumption, certificate dates checked with the PC clock (no SNTP
  required), `AT+PING` through the `ping` command.
- Report: `validation/RAPPORT-validation-pc-2026-10-01.md` (92/92 with `--nfs` and `--tnfsd`
  against a local `tnfsd`).

## Layout

```
src/at_modem.[ch]     portable core: AT/Hayes parser, RX buffer, +IPD, +++
src/net_pico.[ch]     Wi-Fi (cyw43), TCP/TLS (altcp + mbedTLS), UDP, DNS/SNTP/ping, flash, watchdog/diagnostics
src/ap_pico.[ch]      setup access point (cyw43 + lwIP): opening, closing, HTTP
src/dhcp_server.[ch]  access point DHCP server (portable, tested on the PC)
src/dns_catchall.[ch] access point captive DNS (portable, tested on the PC)
src/web_setup.[ch]    setup page: HTTP, form, captive portal (portable, tested on the PC)
src/tnfs_link.[ch]    TNFS USB port frames and reply queue (portable, tested on the PC)
src/tnfs_pico.[ch]    TNFS USB port: independent UDP link, non-blocking DNS
src/http_parse.[ch]   HTTP streaming: URL, redirects, headers, chunked (portable, tested on the PC)
src/tnfs_client.[ch]  TNFS client of the AT+N… commands (portable, tested on the PC and against tnfsd)
src/tls_date.[ch]     civil date without gmtime_r (certificate date checks)
src/mbedtls_config.h  mbedTLS configuration (TLS 1.2 client)
certs/roots.pem       trust roots (Mozilla store); tools/roots2c.py compiles them
src/roots_store.[ch]  flash root index, lookup by subject (portable, PC-tested)
src/roots_ca_cb.[ch]  mbedTLS callback: roots decoded on demand
src/main.c            USB CDC + UART0 transports, main loop, LED
src/usb_descriptors.c, tusb_config.h, lwipopts.h
tests/                PC unit tests
pc/                   modem simulated on the PC (pcmodem.c, PC mbedTLS config)
validation/           on-board validation protocol, script and reports
docs/BACKLOG.md       agile backlog (French); CHANGELOG.md — versions match `AT+GMR` and tags `vX.Y.Z`
VERSION               version (semver); cmake/build_id.cmake: build id; tools/release.sh
```

## Consumers

Neo6502drive (6502 driver, terminal), Neo6502ProphetGui, Neo6502Basic (`at`
primitives), Neo6502 firmware `trinity` branch (CDC group 14, routing 10,19).
Upstream programs (`netsetup.neo`, `prophet.neo`, `pget.neo`, ProphetGui) must
keep working unmodified; any new AT behaviour is documented in `README.md`.

## Licence

EUPL 1.2 (European Union Public Licence): see [LICENSE](LICENSE).
