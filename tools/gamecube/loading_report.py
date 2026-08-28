#!/usr/bin/env python3
"""Summarize GameCube loading/LOD telemetry from a Dolphin log."""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass, field
from pathlib import Path


STAMP = re.compile(r"(?P<m>\d+):(?P<s>\d+):(?P<ms>\d{3})")
TPT = re.compile(r"\bTPT (?P<area>\S+).*? load=(?P<load>\d+)ms")
TPR = re.compile(
    r"\bTPR (?P<area>\S+).*?max=(?P<max>\d+)us.*?>33=(?P<over33>\d+)"
    r".*?load=(?P<load>\d+)ms lod=(?P<lod>\d+) oom=(?P<geo>\d+)/(?P<tex>\d+)"
)
BUILD = re.compile(
    r"\bBUILD-AFTER-REVEAL model=(?P<model>\S+) id=(?P<id>\d+)"
    r" load=(?P<load>\d+) evict=(?P<evict>\d+)"
)
LOD = re.compile(
    r"\bLOD-LATE (?P<area>\S+) model=(?P<model>\S+) id=(?P<id>\d+)"
    r" class=(?P<class>\S+) miss=(?P<miss>\d+) hit=(?P<hit>\d+)"
    r".*?load=(?P<load>\d+) evict=(?P<evict>\d+)"
    r" min=(?P<min>\d+) max=(?P<max>\d+) txd=(?P<txd>\S+)"
)
REVEAL = re.compile(
    r"\bLOAD-REVEAL-COMMIT level=(?P<level>\d+) pending=(?P<pending>\d+)"
    r" drop=(?P<drop>\d+)"
)
FINAL_OOM = re.compile(r"\b(?:GEO-TEX-OOM|GEO-ALIGNED-OOM|RW FATAL|STACK:)")
ALLOCATOR_PROBE = re.compile(r"\bGEO-OOM\b")
FALLBACK = re.compile(r"\bA o\d+ f(?P<fallback>\d+)\b")


def millis(line: str) -> int | None:
    match = STAMP.search(line)
    if not match:
        return None
    return (
        int(match["m"]) * 60_000
        + int(match["s"]) * 1_000
        + int(match["ms"])
    )


@dataclass
class Area:
    name: str
    reveal_ms: int | None = None
    load_ms: int = 0
    pending: int = 0
    dropped: int = 0
    builds: list[tuple[int, str, int, int, int]] = field(default_factory=list)
    lod: list[dict[str, str]] = field(default_factory=list)
    lod_misses: int = 0
    max_frame_us: int = 0
    over_33ms: int = 0
    geo_oom: int = 0
    tex_oom: int = 0


def parse(lines: list[str]) -> tuple[list[Area], int, int, int]:
    areas: list[Area] = []
    current: Area | None = None
    reveal: tuple[int | None, int, int] | None = None
    final_oom = 0
    allocator_probes = 0
    max_audio_fallback = 0

    for line in lines:
        stamp = millis(line)
        if match := REVEAL.search(line):
            reveal = (stamp, int(match["pending"]), int(match["drop"]))
        elif match := TPT.search(line):
            current = Area(match["area"], load_ms=int(match["load"]))
            if reveal:
                current.reveal_ms, current.pending, current.dropped = reveal
            areas.append(current)
        elif match := BUILD.search(line):
            if current and stamp is not None:
                origin = current.reveal_ms if current.reveal_ms is not None else stamp
                current.builds.append(
                    (
                        max(0, stamp - origin),
                        match["model"],
                        int(match["id"]),
                        int(match["load"]),
                        int(match["evict"]),
                    )
                )
        elif match := TPR.search(line):
            area = next((item for item in reversed(areas) if item.name == match["area"]), None)
            if area:
                area.lod_misses = int(match["lod"])
                area.max_frame_us = int(match["max"])
                area.over_33ms = int(match["over33"])
                area.geo_oom = int(match["geo"])
                area.tex_oom = int(match["tex"])
        elif match := LOD.search(line):
            area = next((item for item in reversed(areas) if item.name == match["area"]), None)
            if area:
                area.lod.append(match.groupdict())

        final_oom += bool(FINAL_OOM.search(line))
        allocator_probes += bool(ALLOCATOR_PROBE.search(line))
        if match := FALLBACK.search(line):
            max_audio_fallback = max(max_audio_fallback, int(match["fallback"]))

    return areas, final_oom, allocator_probes, max_audio_fallback


def render(
    areas: list[Area], final_oom: int, allocator_probes: int, audio_fallback: int
) -> str:
    out = [
        "LOADING LIFECYCLE REPORT",
        "class: building=detail appeared after reveal; blink=detail disappeared after appearing; late=never appeared in sample",
        "",
        "area         load  post-build  building  blink  late  lod-miss  >33ms  max-frame  final-oom",
    ]
    for area in areas:
        counts = {name: 0 for name in ("building", "blink", "late")}
        for item in area.lod:
            counts[item["class"]] = counts.get(item["class"], 0) + 1
        out.append(
            f"{area.name:12} {area.load_ms:4}ms {len(area.builds):10}"
            f" {counts['building']:9} {counts['blink']:6} {counts['late']:5}"
            f" {area.lod_misses:9} {area.over_33ms:6}"
            f" {area.max_frame_us / 1000:8.1f}ms {area.geo_oom + area.tex_oom:9}"
        )

    out.extend(
        [
            "",
            f"final allocation failures: {final_oom}",
            f"allocator fallbacks (recovered): {allocator_probes}",
            f"audio conversion fallbacks: {audio_fallback}",
            "",
            "VISIBLE LIFECYCLE FAILURES",
        ]
    )
    for area in areas:
        if not area.lod:
            continue
        out.append(f"[{area.name}]")
        for item in sorted(area.lod, key=lambda row: int(row["miss"]), reverse=True):
            out.append(
                f"{item['class']:8} model={item['model']} id={item['id']}"
                f" miss={item['miss']} hit={item['hit']} dist={item['min']}-{item['max']}m"
                f" load/evict={item['load']}/{item['evict']} txd={item['txd']}"
            )

    out.extend(["", "POST-REVEAL BUILDS"])
    for area in areas:
        if not area.builds:
            continue
        out.append(
            f"[{area.name}] reveal_pending={area.pending} dropped={area.dropped}"
        )
        for delay, model, model_id, loads, evicts in area.builds:
            out.append(
                f"+{delay:5}ms model={model} id={model_id} load/evict={loads}/{evicts}"
            )
    return "\n".join(out) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args()
    report = render(*parse(args.log.read_text(errors="replace").splitlines()))
    if args.output:
        args.output.write_text(report)
    else:
        print(report, end="")


if __name__ == "__main__":
    main()
