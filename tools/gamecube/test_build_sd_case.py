#!/usr/bin/env python3
"""Installs whose names are not lowercase (issues #4, #11) still stage.

    python3 tools/gamecube/test_build_sd_case.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_sd import copy_lower, find_ci

with tempfile.TemporaryDirectory() as t:
    game = os.path.join(t, "GTAVC")
    os.makedirs(os.path.join(game, "MODELS", "GENERIC"))
    for name in ("WHEELS.TXD", "gta3.img"):
        open(os.path.join(game, "MODELS", "GENERIC", name), "w").close()
    assert os.path.isfile(find_ci(game, "models", "generic", "wheels.txd"))

    out = os.path.join(t, "root", "models")
    os.makedirs(os.path.join(out, "GENERIC"))   # left by a build before the fix
    copy_lower(find_ci(game, "models"), out, ("gta3.img",))
    assert os.listdir(out) == ["generic"], os.listdir(out)
    assert os.listdir(os.path.join(out, "generic")) == ["wheels.txd"]
print("test_build_sd_case passed")
