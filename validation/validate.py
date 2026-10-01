#!/usr/bin/env python3
"""Validation matérielle du modem Wi-Fi Neo6502drive sur Pico W (picow-modem).

Déroule PROTOCOLE.md sur /dev/ttyACM0 (USB CDC) et imprime un rapport
(à coller dans RAPPORT-validation-<date>.md). Sans argument : toutes les
étapes ; `--quick` : sans les cibles Internet lentes.

Pré-requis : Pico W flashé (make flash ou AT+BOOTSEL), Wi-Fi provisionné
(AT+CWJAP_DEF saisi par le PO), port série libre (fermer screen), PC sur le
même réseau pour l'appel entrant (RING/ATA).

Protocole série : commandes terminées par CRLF (comme netsetup.pas), 115200.
Usage : python3 validate.py [/dev/ttyACM0] [--quick] [--tnfs-usb] [--tnfsd hôte[:port]]
  --tnfs-usb : active le second port USB (AT$TNFSUSB=1, redémarrage), le teste,
               puis remet le réglage d'origine (US-T17)
  --tnfsd    : MOUNT TNFS réel contre ce serveur, par l'UDP AT (US-T14)
"""
import os, socket, struct, sys, threading, time
import serial

PORT = next((a for a in sys.argv[1:] if a.startswith('/dev/')), '/dev/ttyACM0')
QUICK = '--quick' in sys.argv
TNFS_USB = '--tnfs-usb' in sys.argv
TNFSD = sys.argv[sys.argv.index('--tnfsd') + 1] if '--tnfsd' in sys.argv else None
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

def reboot():
    """AT+RST, réouverture du port, attente du Wi-Fi ; renvoie (ok_rst, STATUS, durée)."""
    global s
    cmd('ATE0')
    try: o, _ = cmd('AT+RST', 2)
    except serial.SerialException: o = 'OK'      # le port disparaît pendant le redémarrage
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
    cmd('ATE0')
    st = '?'
    while time.time() - t0 < 60:
        o2, _ = cmd('AT+CIPSTATUS', 2)
        if 'STATUS:' in o2: st = o2.split('STATUS:')[1][0]
        if st in '234': break
        time.sleep(2)
    return 'OK' in o, st, time.time() - t0

# ------------------------------------------------------------ 0. base
hd('0. Redémarrage (état connu : pas de session TLS en mémoire) et reconnexion Wi-Fi')
ok_rst, st, dt = reboot()
rec('AT+RST → OK', ok_rst)
rec('reconnexion Wi-Fi automatique au boot', st in '234', f'STATUS:{st} après {dt:.0f}s')
t0 = time.time()
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

# ------------------------------------------------------------ 6. UDP (US-T14)
o, _ = cmd('AT+CIFSR'); ip = o.split('"')[1] if '"' in o else ''
def pc_ip_towards(dest):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try: u.connect((dest, 9)); return u.getsockname()[0]
    finally: u.close()
PC_IP = pc_ip_towards(ip) if ip else ''

class UdpEcho:
    """Serveur UDP du PC : renvoie chaque datagramme ; « burst:n » → n datagrammes."""
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('0.0.0.0', 0)); self.port = self.sock.getsockname()[1]
        self.sock.settimeout(0.5); self.run = True; self.seen = []
        threading.Thread(target=self.loop, daemon=True).start()
    def loop(self):
        while self.run:
            try: d, a = self.sock.recvfrom(4096)
            except socket.timeout: continue
            self.seen.append(len(d))
            if d.startswith(b'burst:'):
                for i in range(int(d[6:])): self.sock.sendto(b'B%02d' % i + b'x' * (100 + i), a)
            else:
                self.sock.sendto(d, a)
    def stop(self): self.run = False

def ipd_list(raw):
    """Découpe une sortie en charges +IPD (longueur annoncée respectée)."""
    out, i = [], 0
    while True:
        j = raw.find(b'+IPD,', i)
        if j < 0: return out
        k = raw.find(b':', j)
        if k < 0 or not raw[j + 5:k].isdigit(): return out      # en-tête pas encore complet
        n = int(raw[j + 5:k]); out.append(raw[k + 1:k + 1 + n]); i = k + 1 + n

def cmd_raw(data, wait, stop):
    s.write(data); t0 = time.time(); out = b''
    while time.time() - t0 < wait:
        out += s.read(8192)
        if stop(out): time.sleep(0.2); out += s.read(8192); break
    return out

hd('6. UDP au format ESP8266 (US-T14) — serveur d\'écho UDP sur le PC')
echo = UdpEcho()
o, dt = cmd(f'AT+CIPSTART="UDP","{PC_IP}",{echo.port}', 15)
rec('CIPSTART "UDP" → CONNECT', 'CONNECT' in o and 'OK' in o, f'PC {PC_IP}:{echo.port}')
o, _ = cmd('AT+CIPSTATUS'); rec('CIPSTATUS : lien "UDP"', '"UDP"' in o)
for size in (1, 532, 1472):
    payload = bytes((i * 7 + size) & 0xff for i in range(size))
    o, _ = cmd(f'AT+CIPSEND={size}', 2, (b'> ',))
    raw = cmd_raw(payload, 5, lambda b, n=size: b'+IPD,%d:' % n in b and len(ipd_list(b)) >= 1 and len(ipd_list(b)[0]) >= n)
    got = ipd_list(raw)
    rec(f'datagramme de {size} o : SEND OK puis un +IPD identique', b'SEND OK' in raw and len(got) == 1 and got[0] == payload,
        f'{len(got)} +IPD')
o, _ = cmd('AT+CIPSEND=8', 2, (b'> ',))
raw = cmd_raw(b'burst:12', 6, lambda b: len(ipd_list(b)) >= 12)
got = ipd_list(raw)
rec('12 datagrammes en rafale → 12 +IPD, ni regroupés ni coupés, dans l\'ordre',
    len(got) == 12 and all(g == b'B%02d' % i + b'x' * (100 + i) for i, g in enumerate(got)), f'{len(got)} +IPD')
o, _ = cmd('AT+CIPSEND=1473'); rec('CIPSEND=1473 en UDP → ERROR', 'ERROR' in o)
o, _ = cmd('ATO', 2, (b'CONNECT', b'NO CARRIER')); rec('ATO sur lien UDP → NO CARRIER', 'NO CARRIER' in o)
o, _ = cmd('AT+CIPCLOSE'); rec('CIPCLOSE → CLOSED', 'CLOSED' in o)
echo.stop()
if TNFSD:
    host, _, tport = TNFSD.partition(':'); tport = int(tport or 16384)
    o, _ = cmd(f'AT+CIPSTART="UDP","{host}",{tport}', 15)
    mount = struct.pack('<HBB', 0, 0, 0) + b'\x02\x01' + b'/\x00' + b'\x00' + b'\x00'   # MOUNT "/" anonyme
    cmd(f'AT+CIPSEND={len(mount)}', 2, (b'> ',))
    raw = cmd_raw(mount, 6, lambda b: len(ipd_list(b)) >= 1)
    got = ipd_list(raw)
    rec(f'TNFS MOUNT sur {host}:{tport} → réponse, statut 0', len(got) == 1 and len(got[0]) >= 5 and got[0][3] == 0 and got[0][4] == 0,
        got[0][:8].hex() if got else 'aucune réponse')
    cmd('AT+CIPCLOSE')

# ------------------------------------------------------------ 7. HTTP (US-T11)
def read_done(b):
    """Réponse AT+HTTPREAD complète : en-tête, k octets annoncés, puis OK (ou ERROR)."""
    j = b.find(b'+HTTPREAD:')
    if j < 0: return b'ERROR' in b
    k = b.find(b':', j + 10)
    if k < 0: return False
    cnt = b[j + 10:k].split(b',')[0]
    return cnt.isdigit() and len(b) >= k + 1 + int(cnt) + 6

def http_read_all(n=1024, limit=400000):
    """AT+HTTPREAD jusqu'à suite = 0 ; renvoie (corps, nombre de lectures, ok)."""
    body, reads = b'', 0
    while len(body) < limit:
        raw = cmd_raw(b'AT+HTTPREAD=%d\r\n' % n, 15, read_done)
        j = raw.find(b'+HTTPREAD:')
        if j < 0: return body, reads, False
        k = raw.find(b':', j + 10)
        cnt, more = raw[j + 10:k].split(b',')
        cnt = int(cnt); body += raw[k + 1:k + 1 + cnt]; reads += 1
        if raw[k + 1 + cnt:k + 1 + cnt + 6] != b'\r\nOK\r\n': return body, reads, False
        if more == b'0': return body, reads, True
    return body, reads, False

def httpget(url, extra=''):
    o, dt = cmd(f'AT+HTTPGET="{url}"{extra}', 60)
    if '+HTTPGET:' not in o: return None, None, o, dt
    code, size = o.split('+HTTPGET:')[1].split(',')[:2]
    return int(code), int(size), o, dt

hd('7. Flux HTTP(S) (US-T11)')
code, size, o, dt = httpget('http://mimuma.pl/')
body, reads, okr = http_read_all() if code else (b'', 0, False)
rec('HTTPGET http://mimuma.pl/ → 200, corps complet', code == 200 and okr and (size < 0 or len(body) == size),
    f'{code}, annoncé {size}, lu {len(body)} o en {reads} lectures, {dt:.1f}s')
code, size, o, dt = httpget('https://mimuma.pl/')
body, reads, okr = http_read_all() if code else (b'', 0, False)
rec('HTTPGET https://mimuma.pl/ (TLS) → 200, corps complet', code == 200 and okr and (size < 0 or len(body) == size),
    f'{code}, annoncé {size}, lu {len(body)} o, {dt:.1f}s')
code, size, o, dt = httpget('https://mimuma.pl/', ',0,99')
body, reads, okr = http_read_all() if code else (b'', 0, False)
rec('Range 0-99 → 206 et 100 octets', code == 206 and okr and len(body) == 100, f'{code}, {len(body)} o')
if not QUICK:
    code, size, o, dt = httpget('http://github.com/')
    body, reads, okr = http_read_all() if code else (b'', 0, False)
    rec('redirection http://github.com → https, 200, corps lu (chunked ou non)', code == 200 and okr and len(body) > 1000,
        f'{code}, annoncé {size}, lu {len(body)} o, {dt:.1f}s')
o, _ = cmd('AT+HTTPCLOSE'); rec('HTTPCLOSE → OK', 'OK' in o)
o, _ = cmd('AT+HTTPGET="ftp://x"'); rec('URL invalide → bad URL', 'bad URL' in o and 'ERROR' in o)
o, _ = cmd('AT+CIPSTART="TCP","mimuma.pl",80', 15)
raw = cmd_raw(b'AT+CIPCLOSE\r\n', 3, lambda b: b'OK' in b)
rec('après HTTP, CIPSTART/CIPCLOSE normaux', 'CONNECT' in o and b'CLOSED' in raw)

# ------------------------------------------------------------ 8. filtrage (US-T12), partie automatisable
hd('8. Hôtes autorisés (US-T12) — lecture seule en AT, journal')
o, _ = cmd('AT+NHOSTS?'); rec('AT+NHOSTS? → filtrage inactif par défaut', '+NHOSTS:0' in o, o.strip().splitlines()[0] if o.strip() else '')
o, _ = cmd('AT+NHOSTS=1,"x"'); rec('AT+NHOSTS=… refusé (lecture seule)', 'read-only' in o and 'ERROR' in o)
o, _ = cmd('AT+NLOG?'); rec('AT+NLOG? journalise les connexions de la session', '+NLOG:' in o and '"mimuma.pl"' in o and 'allowed' in o,
                            f"{o.count('+NLOG:')} entrées")

# ------------------------------------------------------------ 9. point d'accès (US-W6), partie automatisable
hd('9. Point d\'accès de configuration (US-W6) — sans téléphone')
o, _ = cmd('AT+APSETUP?'); rec('AT+APSETUP? → 0 (Wi-Fi mémorisé et joint)', '+APSETUP:0' in o)
o, dt = cmd('AT+APSETUP=1', 10); rec('AT+APSETUP=1 → OK', 'OK' in o, f'{dt:.1f}s')
o, _ = cmd('AT+APSETUP?'); ap = o.split('"')[1] if '+APSETUP:1,"' in o else ''
rec('AT+APSETUP? → 1, SSID Neo6502-modem-XXXX', ap.startswith('Neo6502-modem-'), ap)
o, _ = cmd('ATI'); rec('ATI : ligne setup AP', 'setup AP: "Neo6502-modem-' in o)
o, _ = cmd('AT+CIPSTATUS'); rec('station toujours associée (STATUS:2..4)', any(f'STATUS:{c}' in o for c in '234'))
o, dt = cmd('AT+CIPSTART="TCP","mimuma.pl",80', 15)
rec('connexion sortante pendant que l\'AP est ouvert (route par défaut = station)', 'CONNECT' in o, f'{dt:.1f}s')
cmd('AT+CIPCLOSE')
time.sleep(5)
o, dt = cmd('AT+CWLAP', 20); rec('AT+CWLAP pendant que l\'AP est ouvert', 'OK' in o, f"{o.count('+CWLAP:(')} réseaux")
if ap:
    print(f'  (manuel, si un téléphone est là : rejoindre « {ap} », mot de passe AT+APSETUPPWD?, ouvrir http://192.168.4.1/)')
o, _ = cmd('AT+APSETUP=0', 5); rec('AT+APSETUP=0 → OK', 'OK' in o)
o, _ = cmd('AT+APSETUP?'); rec('AT+APSETUP? → 0', '+APSETUP:0' in o)

# ------------------------------------------------------------ 10. second port USB (US-T17), en option
if TNFS_USB:
    hd('10. Second port USB TNFS (US-T17)')
    o, _ = cmd('AT$TNFSUSB?'); was = '1' if '$TNFSUSB:1' in o else '0'
    rec('AT$TNFSUSB? lisible', '$TNFSUSB:' in o, f'valeur d\'origine {was}')
    if was == '0':
        o, _ = cmd('AT$TNFSUSB=1'); rec('AT$TNFSUSB=1 → OK', 'OK' in o)
        ok_rst, st, dt = reboot(); rec('redémarrage, Wi-Fi rejoint', ok_rst and st in '234', f'{dt:.0f}s')
    tnfs_dev = PORT[:-1] + str(int(PORT[-1]) + 1)
    t0 = time.time()
    while not os.path.exists(tnfs_dev) and time.time() - t0 < 10: time.sleep(0.5)
    rec(f'{tnfs_dev} présent (interface « TNFS »)', os.path.exists(tnfs_dev))
    echo = UdpEcho()
    o, _ = cmd(f'AT$TNFS="{PC_IP}",{echo.port}'); rec('AT$TNFS=PC → OK', 'OK' in o)
    o, _ = cmd('ATI'); rec('ATI : ligne TNFS', f'TNFS (USB port 2): {PC_IP}:{echo.port}' in o)
    o, _ = cmd('AT+CIPSTART="TCP","mimuma.pl",80', 15); rec('lien AT TCP ouvert en parallèle', 'CONNECT' in o)
    try:
        t = serial.Serial(tnfs_dev, 115200, timeout=0.3); time.sleep(0.5); t.reset_input_buffer()
        frames_ok = 0
        for size in (5, 532, 1472):
            d = bytes((i * 3 + size) & 0xff for i in range(size))
            t.write(struct.pack('<H', size) + d)
            t0 = time.time(); buf = b''
            while time.time() - t0 < 5 and len(buf) < 2 + size: buf += t.read(4096)
            frames_ok += buf == struct.pack('<H', size) + d
        rec('3 trames (5, 532, 1472 o) aller-retour par le PC, identiques', frames_ok == 3, f'{frames_ok}/3')
        t.write(b'\x00\x00' + struct.pack('<H', 3) + b'abc')        # longueur 0 : resynchronisation
        time.sleep(1); t.write(struct.pack('<H', 2) + b'ok')
        t0 = time.time(); buf = b''
        while time.time() - t0 < 5 and b'ok' not in buf: buf += t.read(4096)
        rec('longueur invalide → resynchronisation, trame suivante servie', buf.endswith(struct.pack('<H', 2) + b'ok'))
        t.close()
    except serial.SerialException as e:
        rec('ouverture du port TNFS', False, str(e))
    raw = cmd_raw(b'AT+CIPCLOSE\r\n', 3, lambda b: b'OK' in b)
    rec('lien AT toujours vivant pendant TNFS (CIPCLOSE → CLOSED)', b'CLOSED' in raw)
    echo.stop()
    cmd('AT$TNFS=0')
    if was == '0':
        cmd('AT$TNFSUSB=0'); ok_rst, st, dt = reboot()
        rec('réglage d\'origine remis (AT$TNFSUSB=0), un seul port USB', ok_rst and not os.path.exists(tnfs_dev))

# ------------------------------------------------------------ rapport
hd('Résumé')
nok = sum(1 for _, ok, _ in results if ok); tot = len(results)
print(f'{nok}/{tot} étapes réussies — firmware {ver}, port {PORT}, {time.strftime("%Y-%m-%d %H:%M")}')
print('\n| Étape | Résultat | Détail |\n|---|---|---|')
for step, ok, detail in results: print(f"| {step} | {'OK' if ok else '**ÉCHEC**'} | {detail} |")
sys.exit(0 if nok == tot else 1)
