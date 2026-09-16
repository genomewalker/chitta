#!/usr/bin/env python3
"""Compatibility entrypoint for the recursive Markdown/HTML link checker.

Keep one implementation for root-relative URLs, nested pages and fragments.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

if __name__ == '__main__':
    checker = Path(__file__).with_suffix('.sh')
    sys.exit(subprocess.call(['bash', str(checker), *sys.argv[1:]]))
