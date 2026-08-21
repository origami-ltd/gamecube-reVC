#!/usr/bin/env python3
"""Validate every GameCube cheat sequence and generate the offline reference."""

from __future__ import annotations

import argparse
import html
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
TABLE = ROOT / "src/core/GameCubeCheats.inc"
PAD_CPP = ROOT / "src/core/Pad.cpp"
OUTPUT = ROOT / "tools/gamecube/gamecube_cheats.html"

# Original PS2 sequences from the 54-code list.  Symbols are intentionally
# still PS2-shaped here; the display conversion below is the one authoritative
# PS2 -> GameCube substitution table.
EXPECTED_CODES = set("""
3414LDRULDRU 3414LDRULDDL 3414LDRULDDD 341XLDRULDRU 341CLDRULDRU
R2D3LL3121 33C4LRLRLR 33C4UDUDUD CX114XXCT RRLU12LUDR
1234D142 C2LX31X1 C4D3LR31X2 R1U21R31X3 3C41R31X4 D1D2LX31XX
3241R4LXS1 R3U41R31RC C1C2LX31XX 41U1R3RUCT CC1CCC123TCT
D3C22X31LL URR1RUS2 3C4R12XXS3 41CR13RUC4 D4D32L31LR 4U2LL31CR
C3C3LL31CR R2D22X31CL C1U32X31CX 423124STCT21 4C32L3142
C2U3LX31LC C1D2LX31RC R3U22L3133 R4C32S34 R4C32D13 T1T4S11
3XTR4SUDS T33L3141 C1D2LX31RX 4X11222D 4X11222T 4X11222S
4X11222C 4X11222X R1C2LX311X TURD21S TURDS43 CC1S1SSS1TCT
43XTXTUD DUUUX4322 DLULX4321 4CU1R3RUST
""".split())

TOKEN = {
    "X": "A",
    "C": "B",
    "S": "X",
    "T": "Y",
    "U": "↑",
    "D": "↓",
    "L": "←",
    "R": "→",
    "1": "L click",
    "2": "L pull",
    "3": "R click",
    "4": "R pull",
}

ENTRY_RE = re.compile(
    r'^GC_CHEAT\("([1234XCTSLRUD]+)",\s*([A-Za-z0-9_]+),\s*"([^"]+)"\)$'
)


def entries():
    result = []
    for number, raw in enumerate(TABLE.read_text().splitlines(), 1):
        line = raw.strip()
        if not line.startswith("GC_CHEAT"):
            continue
        match = ENTRY_RE.fullmatch(line)
        if not match:
            raise ValueError(f"{TABLE}:{number}: malformed GC_CHEAT entry")
        result.append(match.groups())
    return result


def validate(items):
    errors = []
    codes = [code for code, _, _ in items]
    if len(items) != 54:
        errors.append(f"expected 54 cheats, found {len(items)}")
    if len(codes) != len(set(codes)):
        errors.append("duplicate cheat sequences")
    if set(codes) != EXPECTED_CODES:
        errors.append(
            "PS2 list mismatch: missing=%s extra=%s"
            % (sorted(EXPECTED_CODES - set(codes)), sorted(set(codes) - EXPECTED_CODES))
        )
    if any(len(code) > 12 for code in codes):
        errors.append("a sequence is longer than CPad::CheatString")
    if any(symbol not in TOKEN for code in codes for symbol in code):
        errors.append("a sequence uses a symbol without a fixed GameCube mapping")

    pad_source = PAD_CPP.read_text()
    for _, callback, _ in items:
        if not re.search(r"\b%s\s*\(" % re.escape(callback), pad_source):
            errors.append(f"missing callback {callback}")

    # Exercise the exact rolling, reverse-order buffer convention used by
    # CPad::AddToCheatString.  Each complete sequence must select itself once,
    # and no shorter suffix is allowed to fire while it is being entered.
    for expected, _, _ in items:
        buffer = [" "] * 12
        fired = []
        for symbol in expected:
            buffer[1:] = buffer[:-1]
            buffer[0] = symbol
            for code, callback, _ in items:
                if list(reversed(code)) == buffer[:len(code)]:
                    fired.append((code, callback))
                    buffer[0] = " "
                    break
        if len(fired) != 1 or fired[0][0] != expected:
            errors.append(f"dispatcher test failed for {expected}: {fired}")

    if errors:
        raise ValueError("\n".join(errors))


def render(items):
    rows = []
    for index, (code, _, title) in enumerate(items, 1):
        buttons = " ".join(
            f'<span class="key k-{html.escape(TOKEN[symbol].split()[0].lower())}">'
            f'{html.escape(TOKEN[symbol])}</span>'
            for symbol in code
        )
        rows.append(
            f"<tr><td>{index}</td><td>{html.escape(title)}</td>"
            f"<td class=sequence>{buttons}</td></tr>"
        )
    return """<!doctype html>
<html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>reVC GameCube — 54 cheat codes</title>
<style>
:root{color-scheme:dark;--bg:#110d1a;--panel:#21182f;--ink:#fff;--muted:#bdb2ca;--pink:#ff78bd;--purple:#7152a8}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 20% 0,#39234c 0,var(--bg) 45%);color:var(--ink);font:16px/1.45 system-ui,sans-serif}
main{max-width:1180px;margin:auto;padding:34px 22px 70px}h1{margin:0;color:var(--pink);font-size:clamp(30px,6vw,58px);letter-spacing:-.04em}.lead{max-width:850px;color:var(--muted)}
.map,.warning{padding:14px 18px;border:1px solid #564268;border-radius:12px;background:#1b1426;margin:18px 0}.warning{border-color:#9b6a39;color:#ffd7a4}
table{width:100%;border-collapse:separate;border-spacing:0 7px}th{text-align:left;color:var(--pink);padding:8px 12px}td{background:var(--panel);padding:12px}td:first-child{border-radius:10px 0 0 10px;color:var(--muted);width:42px}td:last-child{border-radius:0 10px 10px 0}
.sequence{display:flex;flex-wrap:wrap;gap:5px}.key{display:inline-grid;place-items:center;min-width:29px;height:29px;padding:0 8px;border:1px solid #85739b;border-radius:7px;background:#332540;font:700 12px ui-monospace,monospace;white-space:nowrap;box-shadow:0 2px 0 #0d0912}.k-a{background:#2b7d69}.k-b{background:#8b3343}.k-x,.k-y{background:#5a5d68}.k-l,.k-r{background:#4d3c62}
@media(max-width:700px){table,tbody,tr,td{display:block}thead{display:none}tr{margin:12px 0}td{border-radius:0!important}td:first-child{width:auto}.sequence{padding-top:5px}}
</style>
<main><h1>Cheats do reVC GameCube</h1>
<p class=lead>Todos os 54 códigos de controle da versão PlayStation 2, convertidos com uma substituição única e consistente para o controle do GameCube. Digite durante o gameplay, sem pausar.</p>
<div class=map><b>Mapeamento fixo:</b> PS2 Cross → GameCube A · Circle → B · Square → X · Triangle → Y · L1/R1 → clique de L/R · L2/R2 → curso analógico de L/R. D-pad não muda.</div>
<div class=warning><b>Gatilhos:</b> para L1/R1, dê um clique rápido até o fim. Para L2/R2, puxe sem clicar e solte. Riot e “pedestres odeiam Tommy” são persistentes; não salve por cima do seu progresso principal após usá-los.</div>
<table><thead><tr><th>#</th><th>Efeito</th><th>Sequência GameCube</th></tr></thead><tbody>
""" + "\n".join(rows) + """
</tbody></table></main></html>
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="validate and require a current HTML file")
    args = parser.parse_args()
    try:
        items = entries()
        validate(items)
        document = render(items)
        if args.check:
            if not OUTPUT.exists() or OUTPUT.read_text() != document:
                raise ValueError(f"{OUTPUT} is stale; run {pathlib.Path(__file__).name}")
        else:
            OUTPUT.write_text(document)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    print(f"PASS: {len(items)}/{len(items)} cheat sequences; {OUTPUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
