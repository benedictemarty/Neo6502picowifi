#!/usr/bin/env python3
"""Validation matérielle du modem Wi-Fi Neo6502drive sur Pico W (picow-modem).

Déroule PROTOCOLE.md sur /dev/ttyACM0 (USB CDC) et imprime un rapport
(à coller dans RAPPORT-validation-<date>.md). Sans argument : toutes les
étapes ; `--quick` : sans les cibles Internet lentes.

Pré-requis : Pico W flashé (make flash ou AT+BOOTSEL), Wi-Fi provisionné
(AT+CWJAP_DEF saisi par le PO), port série libre (fermer screen), PC sur le
même réseau pour l'appel entrant (RING/ATA).

Protocole série : commandes terminées par CRLF (comme netsetup.pas), 115200.
Usage : python3 validate.py [/dev/ttyACM0] [--quick]
"""
import socket, sys, threading, time
import serial

PORT = next((a for a in sys.argv[1:] if a.startswith('/dev/')), '/dev/ttyACM0')
QUICK = '--quick' in sys.argv
results = []          # (étape, ok, détail)

def hd(m): print(f'\n== {m} ==')
def rec(step, ok, detail=''):
    results.append((step, ok, detail))
    print(f"  {'✓' if ok else '✗'} {step}{(' — ' + detail) if detail else ''}")

s = serial.Serial(PORT, 115200, timeout=0.3)
time.sleep(0.3); s.reset_input_buffer()

def cmd(c, wait=3, stop=(b'\r\nOK\r\n', b'ERROR', b'FAIL', b'NO CARRIER')):
    s.write(c if isinstance(c, bytes) else (c + '\r\n').encode())
    t0 = time.time(); out = b''
    while time.time() - t0 < wait:
        out += s.read(8192)
        if any(x in out for x in stop):
            time.sleep(0.3); out += s.read(8192); break
    return out.decode(errors='replace'), time.time() - t0

def listen(t):
    t0 = time.time(); buf = b''
    while time.time() - t0 < t: buf += s.read(8192)
    return buf

def escape():
    time.sleep(1.2); s.write(b'+++'); time.sleep(1.3); return listen(0.5)

# ------------------------------------------------------------ 0. base
hd('0. Redémarrage (état connu : pas de session TLS en mémoire) et reconnexion Wi-Fi')
o, _ = cmd('ATE0')
try: o, _ = cmd('AT+RST', 2)
except serial.SerialException: o = 'OK'          # le port disparaît pendant le redémarrage
rec('AT+RST → OK', 'OK' in o)
try: s.close()
except serial.SerialException: pass
time.sleep(3)
t0 = time.time()
while time.time() - t0 < 30:
    try:
        s = serial.Serial(PORT, 115200, timeout=0.3); break
    except serial.SerialException: time.sleep(1)
else:
    print('port série absent après AT+RST'); sys.exit(2)
time.sleep(1); s.reset_input_buffer()
st = '?'
while time.time() - t0 < 60:
    o, _ = cmd('AT+CIPSTATUS', 2)
    if 'STATUS:' in o: st = o.split('STATUS:')[1][0]
    if st in '234': break
    time.sleep(2)
rec('reconnexion Wi-Fi automatique au boot', st in '234', f'STATUS:{st} après {time.time() - t0:.0f}s')
t0 = time.time()
while time.time() - t0 < 30:
    o, _ = cmd('AT+CIPSNTPTIME?', 2)
    if '1970' not in o and '+CIPSNTPTIME:' in o: break
    time.sleep(2)

hd('1. Identité et état')
o, _ = cmd('ATE0'); rec('ATE0 → OK', 'OK' in o)
o, _ = cmd('ATI'); rec('ATI', 'Neo6502drive Pico W modem' in o, o.strip().splitlines()[0] if o.strip() else '')
ver = o.split('modem ')[1].split()[0] if 'modem ' in o else '?'
o, _ = cmd('AT+GMR'); rec('AT+GMR', 'AT version:' in o and 'OK' in o)
o, _ = cmd('AT+CWMODE?'); rec('AT+CWMODE? → +CWMODE:1', '+CWMODE:1' in o)
o, _ = cmd('AT+CIPSTATUS'); st = o.split('STATUS:')[1][0] if 'STATUS:' in o else '?'
rec('Wi-Fi associé (STATUS:2..4)', st in '234', f'STATUS:{st}')
o, _ = cmd('AT+CIFSR'); rec('AT+CIFSR (IP + MAC)', '+CIFSR:STAIP,"' in o and '0.0.0.0' not in o, o.strip().splitlines()[0] if o.strip() else '')
o, _ = cmd('AT+CIPSNTPTIME?'); rec('Heure SNTP acquise', '1970' not in o and '+CIPSNTPTIME:' in o, o.strip().splitlines()[0] if o.strip() else '')
o, _ = cmd('AT+CWJAP_CUR?'); rec('AT+CWJAP_CUR? (ssid, bssid, canal, rssi)', '+CWJAP_CUR:"' in o)
o, _ = cmd('AT+CIPSTA_CUR?'); rec('AT+CIPSTA_CUR? ip/gateway/netmask', ':ip:"' in o and ':gateway:"' in o and ':netmask:"' in o)
o, _ = cmd('AT+CIPDNS_CUR?'); rec('AT+CIPDNS_CUR?', '+CIPDNS_CUR:' in o)
o, _ = cmd('AT+CWDHCP_DEF?'); rec('AT+CWDHCP_DEF?', '+CWDHCP_DEF:' in o)
o, _ = cmd('AT+CIPSSLCCONF?'); rec('AT+CIPSSLCCONF? → 2 (CA vérifiée)', '+CIPSSLCCONF:2' in o)
o, dt = cmd('AT+CWLAP', 20); n = o.count('+CWLAP:(')
rec('AT+CWLAP liste des réseaux', n >= 1 and 'OK' in o, f'{n} réseaux en {dt:.1f}s')
o, dt = cmd('AT+PING="mimuma.pl"', 8); rec('AT+PING', '+' in o and 'OK' in o, o.strip().splitlines()[0] if o.strip() else '')
o, dt = cmd('AT+TLSTEST', 60); rec('AT+TLSTEST autotests mbedTLS', 'gcm=0' in o and 'aes=0' in o and 'ecp=0' in o, o.strip().splitlines()[0] if o.strip() else '')

# ------------------------------------------------------------ 1. Prophet en clair
hd('2. Séquence Prophet en clair (mimuma.pl:80)')
o, dt = cmd('AT+CIPSTART="TCP","mimuma.pl",80', 15); rec('CIPSTART TCP → CONNECT', 'CONNECT' in o and 'OK' in o, f'{dt:.1f}s')
o, _ = cmd('AT+CIPSTATUS'); rec('STATUS:3 (TCP ouvert)', 'STATUS:3' in o)
req = b'GET / HTTP/1.0\r\nHost: mimuma.pl\r\n\r\n'
o, _ = cmd(f'AT+CIPSEND={len(req)}', 2, (b'> ',)); rec('CIPSEND → OK >', '> ' in o)
o, dt = cmd(req + b'\r\n', 15, (b'CLOSED',))
rec('SEND OK, +IPD, CLOSED', 'SEND OK' in o and '+IPD,' in o and 'HTTP/1.1 ' in o and 'CLOSED' in o, f'{dt:.1f}s')
o, _ = cmd('AT+CIPSTATUS'); rec('STATUS:4 (TCP fermé)', 'STATUS:4' in o)

# ------------------------------------------------------------ 2. TLS
hd('3. TLS')
o, dt = cmd('AT+CIPSTART="SSL","mimuma.pl",443', 60); rec('SSL mimuma.pl (Let\'s Encrypt) → CONNECT', 'CONNECT' in o, f'{dt:.1f}s')
o, _ = cmd('ATI'); hs = o.split('last handshake: ')[1].split(',')[0] if 'last handshake: ' in o else '?'
rec('ATI : handshake complet mesuré', 'verify: d0=0x0' in o, hs)
heap = o.split('heap: ')[1].split(', time')[0] if 'heap: ' in o else '?'
rec('ATI : racine ISRG Root X1 trouvée dans le magasin en flash (US-T13)', 'last root: ISRG Root X1,' in o, heap)
o, _ = cmd(f'AT+CIPSEND={len(req)}', 2, (b'> ',))
o, dt = cmd(req + b'\r\n', 15, (b'CLOSED',)); rec('HTTPS : réponse déchiffrée en +IPD', 'HTTP/1.1 200' in o and 'CLOSED' in o)
o, dt = cmd('AT+CIPSTART="SSL","mimuma.pl",443', 60); rec('SSL mimuma.pl 2e fois (reprise de session)', 'CONNECT' in o, f'{dt:.1f}s')
o, _ = cmd('ATI'); rec('ATI : resumed', 'resumed' in o, o.split('last handshake: ')[1].split(')')[0] + ')' if 'last handshake: ' in o else '')
cmd('AT+CIPCLOSE')
if not QUICK:
    o, dt = cmd('AT+CIPSTART="SSL","badssl.com",443', 60); rec('SSL badssl.com (ISRG Root X1) → CONNECT', 'CONNECT' in o, f'{dt:.1f}s'); cmd('AT+CIPCLOSE')
    # US-T13 : autorités hors Let's Encrypt, racines cherchées à la demande en flash
    for host, root in (('www.digicert.com', 'DigiCert Global Root G2'),
                       ('github.com', 'Sectigo Public Server Authentication Root E46')):
        o, dt = cmd(f'AT+CIPSTART="SSL","{host}",443', 60); okc = 'CONNECT' in o; cmd('AT+CIPCLOSE')
        o, _ = cmd('ATI'); heap = o.split('heap: ')[1].split(', time')[0] if 'heap: ' in o else '?'
        rec(f'SSL {host} ({root}) → CONNECT', okc and f'last root: {root},' in o, f'{dt:.1f}s, heap: {heap}')
    o, dt = cmd('AT+CIPSTART="SSL","untrusted-root.badssl.com",443', 60); rec('REFUS racine inconnue (untrusted-root.badssl.com)', 'TLS handshake failed' in o and 'CONNECT' not in o, f'{dt:.1f}s')
    o, dt = cmd('AT+CIPSTART="SSL","wrong.host.badssl.com",443', 60); rec('REFUS nom d\'hôte faux (wrong.host.badssl.com)', 'TLS handshake failed' in o and 'CONNECT' not in o, f'{dt:.1f}s')
    o, dt = cmd('AT+CIPSTART="SSL","expired.badssl.com",443', 60); rec('REFUS certificat expiré (expired.badssl.com, racine pourtant présente)', 'TLS handshake failed' in o and 'CONNECT' not in o, f'{dt:.1f}s')
o, dt = cmd('AT+CIPSTART="SSL","178.219.142.145",443', 60); rec('REFUS IP directe (nom non vérifiable)', 'TLS handshake failed' in o and 'CONNECT' not in o, f'{dt:.1f}s')
o, _ = cmd('AT+CIPSTART="SSL","x",443'); rec('DNS Fail sur hôte inexistant', 'DNS Fail' in o and 'ERROR' in o)

# ------------------------------------------------------------ 3. Prophet en TLS via AT+TLSPORT
hd('4. Scénario Prophet en TLS (AT+TLSPORT=443, client "TCP" inchangé)')
o, _ = cmd('AT+TLSPORT=443'); rec('AT+TLSPORT=443', 'OK' in o)
o, _ = cmd('AT+TLSPORT?'); rec('AT+TLSPORT? → 443', '+TLSPORT:443' in o)
times = []
for i in range(3):
    o, dt = cmd('AT+CIPSTART="TCP","mimuma.pl",443', 60); okc = 'CONNECT' in o
    r = b'GET / HTTP/1.1 \r\nHost: mimuma.pl\r\nRange: bytes=%d-%d\r\nResponseFormat: cli\r\n\r\n' % (i * 8192, i * 8192 + 8191)
    cmd(f'AT+CIPSEND={len(r)}', 2, (b'> ',)); o2, dt2 = cmd(r + b'\r\n', 20, (b'CLOSED',))
    times.append(dt); rec(f'bloc {i + 1} : CONNECT + réponse + CLOSED', okc and '+IPD,' in o2 and 'CLOSED' in o2, f'connexion {dt:.1f}s, réponse {dt2:.1f}s')
rec('reprise : connexions 2 et 3 plus rapides que la 1re', len(times) == 3 and times[1] < times[0] and times[2] < times[0], ' / '.join(f'{t:.1f}s' for t in times))
o, _ = cmd('AT+TLSPORT=0'); rec('AT+TLSPORT=0 (effacement)', 'OK' in o)

# ------------------------------------------------------------ 4. Hayes
hd('5. Modem Hayes')
if not QUICK:
    o, dt = cmd('ATDT telehack.com:23', 20, (b'CONNECT', b'NO CARRIER', b'ERROR')); rec('ATDT telehack.com:23 → CONNECT', 'CONNECT' in o, f'{dt:.1f}s')
    data = o.encode(errors='replace') + listen(4); rec('données transparentes reçues (bannière)', b'TELEHACK' in data or len(data) > 200, f'{len(data)} octets')
    s.write(b'date\r\n'); r = listen(3); rec('écho serveur à "date"', b'date' in r)
    r = escape(); rec('+++ → OK (retour commande)', b'OK' in r)
    o, _ = cmd('AT+CIPSTATUS'); rec('STATUS:3 en mode commande', 'STATUS:3' in o)
    o, _ = cmd('ATO', 3, (b'CONNECT', b'NO CARRIER')); rec('ATO → CONNECT', 'CONNECT' in o)
    r = escape(); rec('+++ 2e fois → OK', b'OK' in r)
    o, _ = cmd('ATH'); rec('ATH → OK', 'OK' in o)
    o, _ = cmd('AT+CIPSTATUS'); rec('STATUS:4 après ATH', 'STATUS:4' in o)
# appel entrant
o, _ = cmd('AT+CIPSERVER=1,6502'); rec('AT+CIPSERVER=1,6502', 'OK' in o)
o, _ = cmd('AT+CIFSR'); ip = o.split('"')[1] if '"' in o else ''
res = {}
def client():
    try:
        time.sleep(1); c = socket.create_connection((ip, 6502), timeout=15); c.sendall(b'bonjour du PC\r\n')
        c.settimeout(10); buf = b''
        while b'\n' not in buf: buf += c.recv(100)
        res['pc'] = buf; c.close()
    except Exception as e: res['pc'] = str(e).encode()
threading.Thread(target=client, daemon=True).start()
r = listen(5); rec('RING sur appel entrant, sans +IPD avant ATA', b'RING' in r and b'+IPD' not in r)
o, _ = cmd('ATA', 3, (b'CONNECT', b'NO CARRIER')); rec('ATA → CONNECT + données du PC', 'CONNECT' in o and 'bonjour du PC' in o + listen(1).decode(errors='replace'))
s.write(b'salut du Neo6502\r\n'); time.sleep(3); rec('le PC reçoit la ligne entière', res.get('pc') == b'salut du Neo6502\r\n', repr(res.get('pc')))
r = listen(3); rec('NO CARRIER à la fermeture par le PC', b'NO CARRIER' in r)
cmd('AT+CIPSERVER=0')
o, _ = cmd('ATS12?'); rec('ATS12? → 050', '050' in o)

# ------------------------------------------------------------ rapport
hd('Résumé')
nok = sum(1 for _, ok, _ in results if ok); tot = len(results)
print(f'{nok}/{tot} étapes réussies — firmware {ver}, port {PORT}, {time.strftime("%Y-%m-%d %H:%M")}')
print('\n| Étape | Résultat | Détail |\n|---|---|---|')
for step, ok, detail in results: print(f"| {step} | {'OK' if ok else '**ÉCHEC**'} | {detail} |")
sys.exit(0 if nok == tot else 1)
