#!/usr/bin/env python3
"""pem2c.py entrée.pem sortie.c — tableau C (NUL-terminé, comme l'exige mbedTLS
pour le PEM) contenant les racines de confiance embarquées."""
import sys
pem = open(sys.argv[1], 'rb').read()
if not pem.endswith(b'\n'):
    pem += b'\n'
with open(sys.argv[2], 'w') as out:
    out.write('/* Généré par tools/pem2c.py depuis certs/roots.pem — ne pas éditer. */\n')
    out.write('#include <stddef.h>\n')
    out.write('const unsigned char roots_pem[] = {\n')
    data = pem + b'\0'
    for i in range(0, len(data), 16):
        out.write('    ' + ', '.join('0x%02x' % b for b in data[i:i+16]) + ',\n')
    out.write('};\n')
    out.write('const size_t roots_pem_len = sizeof roots_pem;\n')
    names = [l.strip() for l in pem.decode().splitlines() if l.startswith('-----BEGIN')]
    out.write('const int roots_pem_count = %d;\n' % len(names))
