#!/usr/bin/env python3
"""Offline CLI for the shipped, strictly validated boot-image parser."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime'))
from boot_image import main

if __name__ == '__main__':
    main()
