#!/usr/bin/env python3
"""Generate exact runtime content coverage for the Frontier Courier."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


class ContentGenerationError(RuntimeError):
    pass


def quoted(value: str) -> str:
    if not value or any(ord(char) < 32 or ord(char) > 126 for char in value):
        raise ContentGenerationError(f"content string is not printable ASCII: {value!r}")
    if '"' in value or "\\" in value:
        raise ContentGenerationError(f"content string cannot contain quotes or slashes: {value!r}")
    return f'"{value}"'


def add_unique(seen: set[tuple[str, str]], kind: str, locator: str) -> None:
    key = (kind, locator)
    if key in seen:
        raise ContentGenerationError(f"duplicate {kind} binding for {locator}")
    seen.add(key)


def generate(manifest_path: Path, interior_report_path: Path, output: Path) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    interior_report = json.loads(interior_report_path.read_text(encoding="utf-8"))
    interior_bindings = {
        str(locator): int(index)
        for locator, index in interior_report["bindings"].items()
    }
    if interior_report["asset_id"] != manifest["asset_id"]:
        raise ContentGenerationError("interior report asset id does not match assembly")

    lines = [
        "tdcontent 2",
        f"scene {quoted('ship.frontier.courier_mk1.runtime')}",
        f"assembly {quoted('frontier_courier_mk1.tdassembly')}",
        "root 0 0 0 0 0 0 1 1 1",
    ]
    seen: set[tuple[str, str]] = set()
    expected_interior_locators: set[str] = set()

    for module in manifest["modules"]:
        locators = [module["visual_source"]]
        locators.extend(lod["source"] for lod in module["lods"])
        for lod_index, locator in enumerate(locators):
            add_unique(seen, "visual", locator)
            if module["id"] == "exterior_hull":
                primitive = 0 if lod_index == 0 else lod_index - 1
                model = "frontier_courier_hull_lods.tdmodel"
            else:
                expected_interior_locators.add(locator)
                if locator not in interior_bindings:
                    raise ContentGenerationError(
                        f"interior report does not bind {locator}")
                primitive = interior_bindings[locator]
                model = "frontier_courier_interior_lods.tdmodel"
            lines.append(
                f"visual {quoted(locator)} {quoted(model)} {primitive}")

    for part in manifest["moving_parts"]:
        locator = part["visual_source"]
        add_unique(seen, "visual", locator)
        expected_interior_locators.add(locator)
        if locator not in interior_bindings:
            raise ContentGenerationError(f"interior report does not bind {locator}")
        lines.append(
            f"visual {quoted(locator)} "
            f"{quoted('frontier_courier_interior_lods.tdmodel')} "
            f"{interior_bindings[locator]}")

    extra_interior = set(interior_bindings) - expected_interior_locators
    if extra_interior:
        raise ContentGenerationError(
            "interior report has unauthored bindings: " + ", ".join(sorted(extra_interior)))

    for module in manifest["modules"]:
        locator = module["collision"]["source"]
        add_unique(seen, "collision", locator)
        package = f"frontier_courier_{module['id']}.tdcollision"
        lines.append(f"collision {quoted(locator)} {quoted(package)}")

    for zone in manifest["zones"]:
        locator = zone["navmesh_source"]
        add_unique(seen, "navigation", locator)
        lines.append(f"navigation {quoted(locator)}")
        locator = zone["walkable_surface"]
        add_unique(seen, "walkable", locator)
        lines.append(f"walkable {quoted(locator)}")

    lines.append("end")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
    return {
        "path": str(output),
        "bindings": len(seen),
        "visual_bindings": sum(kind == "visual" for kind, _ in seen),
        "collision_bindings": sum(kind == "collision" for kind, _ in seen),
        "navigation_bindings": sum(kind == "navigation" for kind, _ in seen),
        "walkable_bindings": sum(kind == "walkable" for kind, _ in seen),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--interior-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = generate(
        args.manifest.resolve(), args.interior_report.resolve(), args.output.resolve())
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ContentGenerationError, KeyError, ValueError) as exc:
        print(f"error: {exc}")
        raise SystemExit(2)
