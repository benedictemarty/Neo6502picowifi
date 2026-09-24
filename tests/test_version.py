#!/usr/bin/env python3
"""Cohérence des versions (US-W3) : python3 test_version.py

VERSION (source unique, semver) ↔ CMakeLists.txt ↔ CHANGELOG.md ↔ tag git."""
import os
import re
import subprocess
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def read(p):
    with open(os.path.join(ROOT, p), encoding='utf-8') as f:
        return f.read()


VERSION = read('VERSION').strip()


class Version(unittest.TestCase):
    def test_semver(self):
        self.assertRegex(VERSION, r'^\d+\.\d+\.\d+$')
        self.assertEqual(read('VERSION'), VERSION + '\n')

    def test_cmake_lit_version(self):
        c = read('CMakeLists.txt')
        self.assertIn('file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION PICOW_VERSION', c)
        self.assertIn('project(picow_modem VERSION ${PICOW_VERSION}', c)
        self.assertIn('PICOW_MODEM_VERSION="${PROJECT_VERSION}"', c)
        self.assertIsNone(re.search(r'PICOW_MODEM_VERSION="\d', c), 'version codée en dur')

    def test_pas_de_version_de_repli(self):
        self.assertIsNone(re.search(r'#define PICOW_MODEM_VERSION', read('src/net_pico.c')))

    def test_changelog(self):
        ch = read('CHANGELOG.md')
        heads = re.findall(r'^## (\S+)(?: — (\d{4}-\d{2}-\d{2}))?', ch, re.M)
        self.assertEqual(heads[0][0], '[Unreleased]')
        released = [h for h in heads[1:]]
        self.assertTrue(released, 'aucune version publiée')
        self.assertEqual(released[0][0], VERSION, 'CHANGELOG : la dernière version doit être celle de VERSION')
        self.assertTrue(released[0][1], 'CHANGELOG : date manquante')
        nums = [tuple(map(int, h[0].split('.'))) for h in released]
        self.assertEqual(nums, sorted(nums, reverse=True), 'versions du CHANGELOG non décroissantes')
        self.assertEqual(len(nums), len(set(nums)), 'version en double dans le CHANGELOG')

    def test_tag_de_head(self):
        r = subprocess.run(['git', 'tag', '--points-at', 'HEAD', '--list', 'v[0-9]*'],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            self.skipTest('git indisponible')
        for tag in r.stdout.split():
            self.assertEqual(tag, 'v' + VERSION, 'HEAD tagué %s mais VERSION = %s' % (tag, VERSION))


if __name__ == '__main__':
    unittest.main(verbosity=1)
