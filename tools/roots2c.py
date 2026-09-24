#!/usr/bin/env python3
"""roots2c.py entrée.pem sortie.c [--symbol nom] — magasin de racines de confiance
en flash (US-T13).

Chaque racine du fichier PEM est convertie en DER et concaténée dans un tableau
constant (donc en flash, lu en XIP). Un index trié par empreinte FNV-1a 32 bits
du sujet (DER brut) permet au modem de retrouver, pendant la vérification TLS,
les racines dont le sujet égale l'émetteur recherché (src/roots_store.c), puis
de ne décoder que celles-là (src/roots_ca_cb.c).

Sont exclues (et listées sur stderr) les racines dont la clé n'est pas gérée
par la configuration mbedTLS du modem : RSA < 2048 bits, courbes autres que
P-256 et P-384, autres algorithmes. Les doublons exacts sont retirés.

Sans dépendance : analyse DER minimale, suffisante pour un certificat X.509.
"""
import argparse
import base64
import hashlib
import os
import re
import sys

OID_RSA = '1.2.840.113549.1.1.1'
OID_EC = '1.2.840.10045.2.1'
CURVES = {'1.2.840.10045.3.1.7': 'P-256', '1.3.132.0.34': 'P-384'}
OID_CN, OID_OU, OID_O = '2.5.4.3', '2.5.4.11', '2.5.4.10'


def fnv1a32(data):
    h = 0x811c9dc5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def tlv(buf, pos):
    """(tag, début du contenu, fin du contenu) de l'élément DER à pos."""
    tag = buf[pos]
    n = buf[pos + 1]
    pos += 2
    if n & 0x80:
        k = n & 0x7f
        if k == 0 or k > 3:
            raise ValueError('longueur DER non gérée')
        n = int.from_bytes(buf[pos:pos + k], 'big')
        pos += k
    if pos + n > len(buf):
        raise ValueError('élément DER tronqué')
    return tag, pos, pos + n


def children(buf, start, end):
    out = []
    while start < end:
        t, s, e = tlv(buf, start)
        out.append((t, start, s, e))
        start = e
    return out


def oid(raw):
    first = raw[0]
    parts = [first // 40, first % 40]
    v = 0
    for b in raw[1:]:
        v = (v << 7) | (b & 0x7f)
        if not b & 0x80:
            parts.append(v)
            v = 0
    return '.'.join(map(str, parts))


def parse_cert(der):
    """Renvoie (sujet DER brut, émetteur DER brut, clé, libellé)."""
    t, s, e = tlv(der, 0)
    if t != 0x30 or e != len(der):
        raise ValueError('pas un certificat DER')
    tbs = children(der, s, e)[0]
    f = children(der, tbs[2], tbs[3])
    if f[0][0] == 0xa0:                       # [0] version
        f = f[1:]
    # serial, signature, issuer, validity, subject, spki
    issuer = der[f[2][1]:f[2][3]]
    subject_off, subject_end = f[4][1], f[4][3]
    subject = der[subject_off:subject_end]
    spki = children(der, f[5][2], f[5][3])
    alg = children(der, spki[0][2], spki[0][3])
    key_oid = oid(der[alg[0][2]:alg[0][3]])
    if key_oid == OID_RSA:
        bits = der[spki[1][2] + 1:spki[1][3]]          # BIT STRING sans l'octet « unused »
        rsa = children(bits, 0, len(bits))[0]
        mod = children(bits, rsa[2], rsa[3])[0]
        n = bits[mod[2]:mod[3]].lstrip(b'\0')
        key = 'RSA-%d' % (len(n) * 8 - (8 - n[0].bit_length()))
    elif key_oid == OID_EC and len(alg) > 1 and alg[1][0] == 0x06:
        c = oid(der[alg[1][2]:alg[1][3]])
        key = 'EC-' + CURVES.get(c, c)
    else:
        key = key_oid
    return subject_off, subject, issuer, key, label(der, f[4][2], f[4][3])


def label(der, start, end):
    """CN du sujet, sinon OU, sinon O ; ASCII imprimable (le reste → '?')."""
    found = {}
    for _, _, s, e in children(der, start, end):           # SET (RDN)
        for _, _, s2, e2 in children(der, s, e):           # SEQUENCE (type, valeur)
            av = children(der, s2, e2)
            k = oid(der[av[0][2]:av[0][3]])
            v = der[av[1][2]:av[1][3]]
            if av[1][0] == 0x1e:                            # BMPString
                v = v.decode('utf-16-be', 'replace').encode()
            found.setdefault(k, v.decode('utf-8', 'replace'))
    name = found.get(OID_CN) or found.get(OID_OU) or found.get(OID_O) or '?'
    return ''.join(ch if ' ' <= ch <= '~' else '?' for ch in name)


def supported(key):
    if key.startswith('RSA-'):
        return int(key[4:]) >= 2048
    return key in ('EC-P-256', 'EC-P-384')


def load(pem):
    ders = []
    for m in re.finditer(rb'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', pem, re.S):
        ders.append(base64.b64decode(b''.join(m.group(1).split())))
    return ders


def build(pem):
    """Entrées retenues (triées) et exclusions [(libellé, raison)]."""
    kept, excluded, seen = [], [], set()
    for der in load(pem):
        if der in seen:
            excluded.append(('?', 'doublon'))
            continue
        seen.add(der)
        subj_off, subj, _, key, name = parse_cert(der)
        if not supported(key):
            excluded.append((name, 'clé non gérée : ' + key))
            continue
        kept.append({'der': der, 'subj_off': subj_off, 'subj_len': len(subj),
                     'hash': fnv1a32(subj), 'name': name, 'key': key})
    kept.sort(key=lambda r: (r['hash'], r['der']))
    off = 0
    for r in kept:
        r['off'] = off
        off += len(r['der'])
    return kept, excluded


def c_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('pem')
    ap.add_argument('out')
    ap.add_argument('--symbol', default='roots_store')
    a = ap.parse_args()
    pem = open(a.pem, 'rb').read()
    kept, excluded = build(pem)
    total = sum(len(r['der']) for r in kept)
    sym = a.symbol
    with open(a.out, 'w') as out:
        out.write('/* Généré par tools/roots2c.py depuis %s — ne pas éditer.\n' % os.path.basename(a.pem))
        out.write('   Source SHA-256 %s ; %d racines, %d octets DER, %d exclues. */\n'
                  % (hashlib.sha256(pem).hexdigest(), len(kept), total, len(excluded)))
        out.write('#include "roots_store.h"\n\n')
        out.write('static const uint8_t %s_der[] = {\n' % sym)
        for r in kept:
            out.write('    /* %s (%s) */\n' % (r['name'].replace('*/', '*_/'), r['key']))
            d = r['der']
            for i in range(0, len(d), 16):
                out.write('    ' + ', '.join('0x%02x' % b for b in d[i:i + 16]) + ',\n')
        if not kept:
            out.write('    0\n')
        out.write('};\n\n')
        out.write('static const struct roots_entry %s_entries[] = {\n' % sym)
        for r in kept:
            out.write('    { 0x%08xu, %du, %du, %du, %du, %s },\n'
                      % (r['hash'], r['off'], len(r['der']), r['subj_off'], r['subj_len'], c_string(r['name'])))
        if not kept:
            out.write('    { 0, 0, 0, 0, 0, "" }\n')
        out.write('};\n\n')
        out.write('const struct roots_store %s = { %s_der, %du, %s_entries, %d };\n'
                  % (sym, sym, total, sym, len(kept)))
    print('roots2c: %d racines retenues (%d octets DER), %d exclues'
          % (len(kept), total, len(excluded)), file=sys.stderr)
    for name, why in excluded:
        print('roots2c:   exclue %s — %s' % (name, why), file=sys.stderr)


if __name__ == '__main__':
    main()
