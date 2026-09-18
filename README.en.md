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
  embedded roots `certs/roots.pem`, host name/SNI, dates from SNTP time —
  refused if the time has not been acquired). Session resumption (ticket) for
  the same host:port. Details: § TLS.
- Board LED: on = Wi-Fi associated, blinking = TCP connection open.
- 8 s watchdog: a hang reboots the board; `ATI` reports the cause and the
  stage (`net_pico_stage`) or the lwIP assertion message.

## Supported AT commands

| Command | Response (ESP8266 AT 1.x format) |
|---|---|
| `AT`, `ATE0`, `ATE1` | `OK` |
| `AT+GMR` | `AT version:…`, `SDK version:…`, `OK` |
| `AT+RST`, `AT+RESTORE` | `OK` then reboot (RESTORE erases the configuration) |
| `AT+CWMODE?` / `=1` | `+CWMODE:1` (station only; `=2`/`=3` → `ERROR`) |
| `AT+CWJAP[_CUR|_DEF]="ssid","pass"` | `WIFI CONNECTED`, `WIFI GOT IP`, `OK` or `+CWJAP:n`, `FAIL` |
| `AT+CWJAP[_CUR|_DEF]?` | `+CWJAP_CUR:"ssid","mac",channel,rssi` or `No AP` |
| `AT+CWQAP` | `OK`, `WIFI DISCONNECT` |
| `AT+CWLAPOPT=…`, `AT+CWLAP[=…]` | `+CWLAP:(ecn,"ssid",rssi)` per SSID (deduplicated, best RSSI, sorted), `OK` |
| `AT+CWDHCP[_CUR|_DEF]?` / `=mode,en` | `+CWDHCP_DEF:3` (bit 1 = station) |
| `AT+CIPSTATUS` | `STATUS:2|3|4|5` (+ a `+CIPSTATUS:` line if TCP is open) |
| `AT+CIFSR` | `+CIFSR:STAIP,"ip"`, `+CIFSR:STAMAC,"mac"` |
| `AT+CIPSTA[_CUR|_DEF]?` / `="ip","gw","mask"` | `+CIPSTA_CUR:ip:"…"`, `:gateway:`, `:netmask:` |
| `AT+CIPDNS[_CUR|_DEF]?` / `=1,"ip"` / `=0` | `+CIPDNS_CUR:ip` |
| `AT+CIPMUX?` / `=0`, `AT+CIPMODE?` / `=0` | single connection, normal mode (`=1` → `ERROR`) |
| `AT+CIPSSLCCONF?` / `=0` / `=2` | `+CIPSSLCCONF:2` (CA always verified; `=1`/`=3` client cert → `ERROR`) |
| `AT+CIPSTART="TCP","host",port` | `CONNECT`, `OK`; `DNS Fail`; `ALREADY CONNECTED`; TLS if the port is listed in `AT+TLSPORT` |
| `AT+CIPSTART="SSL","host",port` | same over TLS; `no time (SNTP) for TLS`, `TLS handshake failed` (certificate refused…) → `ERROR` |
| `AT+TLSPORT?` / `=443[,p2,p3,p4]` / `=0` | ports for which `"TCP"` is done over TLS (persistent); `=0` clears |
| `AT+TLSTEST` | mbedTLS self-tests (AES, GCM, SHA-256/512, CTR-DRBG, ECP, MPI) on the board |
| `AT+CIPSEND=n` (n ≤ 2048) | `OK`, `> `, then after n bytes `Recv n bytes`, `SEND OK` |
| incoming data | `+IPD,n:` followed by n bytes (segments ≤ 1460); `CLOSED` when the peer closes |
| `AT+CIPCLOSE` | `CLOSED`, `OK` |
| `AT+CIPSERVER=1,port` / `=0` | incoming listener → `RING` (repeated every 3 s), `ATA` to answer |
| `AT+CIPSNTPCFG?` / `=en,tz,"server"`, `AT+CIPSNTPTIME?` | lwIP SNTP; `+CIPSNTPTIME:Tue Sep 15 12:00:00 2026` |
| `AT+PING="host"` | `+ms`, `OK` or `+timeout`, `ERROR` |
| `AT+CIUPDATE` | `ERROR` (no OTA: reflash a UF2) |
| `ATI` | identity, saved SSID, last reset cause, `TLS:` line (stack, roots, time, duration and cipher suite of the last handshake, `resumed`, verification flags, last lwIP/mbedTLS messages), `TLS ports:` |
| `AT+BOOTSEL` | `OK` then switch to UF2 mode (`RPI-RP2`) without touching the button — specific to this firmware |

Not supported (answers `ERROR`): UDP, `CIPMUX=1`, access-point mode, TLS 1.3,
client certificate, ESP transparent mode (`CIPMODE=1`; use `ATDT` instead —
`ATDT` also does TLS towards a port listed in `AT+TLSPORT`).

## TLS

- Stack: lwIP `altcp_tls` + mbedTLS 3.6.2 (`src/mbedtls_config.h`); TLS 1.2,
  ECDHE-ECDSA / ECDHE-RSA / RSA, AES-GCM, SHA-256/384, P-256, P-384, X25519.
- Verification: `MBEDTLS_SSL_VERIFY_REQUIRED` (forced in `lwipopts.h` **and**
  in the code — altcp's default is `OPTIONAL`, which would let an invalid
  certificate through), SNI + host name, dates checked in a callback
  (`tls_date.c`, without `gmtime_r`, which would block in the lwIP context),
  SNTP time required. No "accept anything" mode.
- Roots: `certs/roots.pem` (ISRG Root X1; see `certs/README.md` to add one),
  compiled by `tools/pem2c.py`.
- Session resumption: ticket kept per host:port and reused on the next
  connection (`prophet.neo` opens one connection per `Range` block).
- Measured on the board (2026-09-16, Apache + Let's Encrypt,
  ECDHE-RSA-AES256-GCM): full handshake **2.4–2.8 s** (4-certificate chain
  verified in ~1.7 s), resumed handshake **0.3–0.5 s**; complete
  `AT+CIPSTART`: 3–4 s the first time, **1.2 s** afterwards. letsencrypt.org
  (ECDSA, CDN): 5 s. Flash 523 KB, BSS 85 KB, mbedTLS on the newlib heap.
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

## Tests

```
make -C tests      # modem core (test_at_modem) + TLS dates (test_tls_date) on the PC, gcc + ASan/UBSan
```

`src/at_modem.c` depends on no Pico API: the harness `tests/test_at_modem.c`
replays the exact sequences of netinfo, netsetup, prophet
(`CIPSTART` → `CIPSEND` → `+IPD` → `CLOSED`) and of the Hayes modem.

Automated on-board validation: `python3 validation/validate.py` (protocol
`validation/PROTOCOLE.md`, reports `validation/RAPPORT-*.md`, in French) — 55
steps: identity, Wi-Fi, SNTP, Prophet sequence in clear and over TLS, TLS
refusals (unknown root, wrong host name, expired, bare IP), Hayes, incoming
call. Prerequisite: Wi-Fi provisioned once with `screen /dev/ttyACM0 115200`
and `AT+CWJAP_DEF="ssid","pass"`.

Anything not measured on a real board is marked "non validé" (not validated)
in the French documents.

## Layout

```
src/at_modem.[ch]     portable core: AT/Hayes parser, RX buffer, +IPD, +++
src/net_pico.[ch]     Wi-Fi (cyw43), TCP/TLS (altcp + mbedTLS), DNS/SNTP/ping, flash, watchdog/diagnostics
src/tls_date.[ch]     civil date without gmtime_r (certificate date checks)
src/mbedtls_config.h  mbedTLS configuration (TLS 1.2 client)
certs/roots.pem       embedded trust roots; tools/pem2c.py compiles them
src/main.c            USB CDC + UART0 transports, main loop, LED
src/usb_descriptors.c, tusb_config.h, lwipopts.h
tests/                PC unit tests
validation/           on-board validation protocol, script and reports
docs/BACKLOG.md       agile backlog (French); CHANGELOG.md — versions match `AT+GMR` and tags `vX.Y.Z`
```

## Consumers

Neo6502drive (6502 driver, terminal), Neo6502ProphetGui, Neo6502Basic (`at`
primitives), Neo6502 firmware `trinity` branch (CDC group 14, routing 10,19).
Upstream programs (`netsetup.neo`, `prophet.neo`, `pget.neo`, ProphetGui) must
keep working unmodified; any new AT behaviour is documented in `README.md`.
