#!/usr/bin/env python3
"""Generate deterministic box collision sources for the Frontier Courier."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path


class CollisionGenerationError(RuntimeError):
    pass


MODULE_ORDER = (
    "exterior_hull",
    "boarding_vestibule",
    "airlock_module",
    "main_corridor",
    "cockpit_module",
    "engineering_module",
    "cargo_module",
    "crew_module",
)


def box_record(
    box_id: str,
    center: tuple[float, float, float],
    size: tuple[float, float, float],
    *,
    walkable: bool,
) -> dict:
    if min(size) <= 0.0 or not all(math.isfinite(value) for value in center + size):
        raise CollisionGenerationError(f"invalid box {box_id}")
    return {
        "id": box_id,
        "center_m": [round(value, 6) for value in center],
        "half_extents_m": [round(value * 0.5, 6) for value in size],
        "walkable": walkable,
    }


def portal_boundaries(
    module_id: str,
    clear_size: tuple[float, float, float],
    sockets: list[dict],
) -> tuple[dict[int, tuple[float, ...]], dict[int, bool]]:
    width, _, length = clear_size
    sides: dict[int, list[float]] = {-1: [], 1: []}
    ends = {-1: False, 1: False}
    for socket in sockets:
        x, _, z = (float(value) for value in socket["position_m"])
        forward = tuple(float(value) for value in socket["forward"])
        if abs(abs(x) - width * 0.5) <= 0.15 and abs(forward[0]) > 0.5:
            sides[1 if x > 0.0 else -1].append(z)
        elif abs(abs(z) - length * 0.5) <= 0.15 and abs(forward[2]) > 0.5:
            ends[1 if z > 0.0 else -1] = True
        else:
            raise CollisionGenerationError(
                f"portal socket {socket['id']} is not on the {module_id} boundary")
    return ({side: tuple(sorted(values)) for side, values in sides.items()}, ends)


def side_wall_boxes(
    *,
    side: int,
    width: float,
    height: float,
    length: float,
    openings_z: tuple[float, ...],
    door_width: float,
    door_height: float,
) -> list[dict]:
    thickness = 0.10
    x = side * (width * 0.5 + thickness * 0.5)
    start_limit = -length * 0.5
    intervals = sorted(
        (max(start_limit, center - door_width * 0.5),
         min(length * 0.5, center + door_width * 0.5))
        for center in openings_z
    )
    boxes: list[dict] = []
    cursor = start_limit
    side_name = "right" if side > 0 else "left"
    segment = 0
    for start, end in intervals:
        if start > cursor:
            boxes.append(box_record(
                f"wall_{side_name}_{segment:02d}",
                (x, 0.0, (cursor + start) * 0.5),
                (thickness, height, start - cursor),
                walkable=False,
            ))
            segment += 1
        lintel_height = max(0.05, height - door_height)
        boxes.append(box_record(
            f"lintel_{side_name}_{segment:02d}",
            (x, door_height * 0.5, (start + end) * 0.5),
            (thickness, lintel_height, end - start),
            walkable=False,
        ))
        segment += 1
        cursor = max(cursor, end)
    if cursor < length * 0.5:
        boxes.append(box_record(
            f"wall_{side_name}_{segment:02d}",
            (x, 0.0, (cursor + length * 0.5) * 0.5),
            (thickness, height, length * 0.5 - cursor),
            walkable=False,
        ))
    return boxes


def end_boxes(
    *,
    side: int,
    width: float,
    height: float,
    length: float,
    is_portal: bool,
    door_width: float,
    door_height: float,
) -> list[dict]:
    thickness = 0.10
    z = side * (length * 0.5 + thickness * 0.5)
    end_name = "fore" if side > 0 else "aft"
    if not is_portal:
        return [box_record(
            f"bulkhead_{end_name}", (0.0, 0.0, z),
            (width, height, thickness), walkable=False)]

    column_width = max(0.05, (width - door_width) * 0.5)
    boxes = []
    for x_side in (-1, 1):
        x = x_side * (door_width * 0.5 + column_width * 0.5)
        label = "right" if x_side > 0 else "left"
        boxes.append(box_record(
            f"frame_{end_name}_{label}", (x, 0.0, z),
            (column_width, height, thickness), walkable=False))
    boxes.append(box_record(
        f"lintel_{end_name}", (0.0, door_height * 0.5, z),
        (door_width, max(0.05, height - door_height), thickness),
        walkable=False))
    return boxes


def fixture_collision_boxes(module_id: str, fixtures: dict) -> list[dict]:
    boxes = []
    for fixture_id, fixture in fixtures.items():
        if not isinstance(fixture, dict):
            raise CollisionGenerationError(
                f"{fixture_id} fixture must be an object")
        if fixture.get("module") != module_id:
            continue
        configured = fixture.get("collision_boxes", [])
        if not isinstance(configured, list):
            raise CollisionGenerationError(
                f"{fixture_id} collision_boxes must be a list")
        for item in configured:
            if not isinstance(item, dict):
                raise CollisionGenerationError(
                    f"{fixture_id} collision box must be an object")
            box_id = item.get("id")
            center = item.get("center_m")
            size = item.get("size_m")
            walkable = item.get("walkable", False)
            if not isinstance(box_id, str) or not box_id:
                raise CollisionGenerationError(
                    f"{fixture_id} collision box id must be a non-empty string")
            if not isinstance(center, list) or len(center) != 3 or \
                    not isinstance(size, list) or len(size) != 3:
                raise CollisionGenerationError(
                    f"{fixture_id}.{box_id} collision vectors must contain three numbers")
            if not isinstance(walkable, bool):
                raise CollisionGenerationError(
                    f"{fixture_id}.{box_id} walkable must be a boolean")
            try:
                parsed_center = tuple(float(value) for value in center)
                parsed_size = tuple(float(value) for value in size)
            except (TypeError, ValueError) as exc:
                raise CollisionGenerationError(
                    f"{fixture_id}.{box_id} collision vectors contain non-numbers") from exc
            boxes.append(box_record(
                box_id,
                parsed_center,
                parsed_size,
                walkable=walkable,
            ))
    ids = [box["id"] for box in boxes]
    if len(ids) != len(set(ids)):
        raise CollisionGenerationError(
            f"{module_id} fixture collision ids are not unique")
    return boxes


def detail_boxes(
    module_id: str,
    width: float,
    height: float,
    production_fixtures: dict,
) -> list[dict]:
    floor_y = -height * 0.5
    if module_id == "cockpit_module":
        return fixture_collision_boxes(module_id, production_fixtures)
    if module_id == "engineering_module":
        return [
            box_record(f"machinery_{index}",
                       (width * 0.5 - 0.42, floor_y + 0.72, z),
                       (0.76, 1.42, 0.78), walkable=False)
            for index, z in enumerate((-1.45, 0.0, 1.45))
        ]
    if module_id == "crew_module":
        boxes = []
        for index, z in enumerate((-1.25, 1.25)):
            boxes.append(box_record(
                f"bunk_lower_{index}", (width * 0.5 - 0.46, floor_y + 0.42, z),
                (0.82, 0.32, 1.72), walkable=False))
            boxes.append(box_record(
                f"bunk_upper_{index}", (width * 0.5 - 0.46, floor_y + 1.08, z),
                (0.82, 0.20, 1.72), walkable=False))
        boxes.append(box_record(
            "locker", (-width * 0.5 + 0.30, floor_y + 0.78, 1.45),
            (0.56, 1.50, 0.82), walkable=False))
        return boxes
    return []


def boxes_overlap(a: dict, b_center: tuple[float, float, float],
                  b_half: tuple[float, float, float]) -> bool:
    return all(
        abs(float(a["center_m"][axis]) - b_center[axis]) <
        float(a["half_extents_m"][axis]) + b_half[axis] - 1.0e-5
        for axis in range(3)
    )


def validate_entry_clearance(
    module_id: str,
    clear_size: tuple[float, float, float],
    portal_sockets: list[dict],
    details: list[dict],
    door_width: float,
    door_height: float,
) -> None:
    _, height, _ = clear_size
    clearance_depth = 1.2
    clearance_y = -height * 0.5 + door_height * 0.5
    for socket in portal_sockets:
        x, _, z = (float(value) for value in socket["position_m"])
        forward = tuple(float(value) for value in socket["forward"])
        if abs(forward[0]) > 0.5:
            center = (x - forward[0] * clearance_depth * 0.5, clearance_y, z)
            half = (clearance_depth * 0.5, door_height * 0.5, door_width * 0.5)
        else:
            center = (x, clearance_y, z - forward[2] * clearance_depth * 0.5)
            half = (door_width * 0.5, door_height * 0.5, clearance_depth * 0.5)
        for detail in details:
            if boxes_overlap(detail, center, half):
                raise CollisionGenerationError(
                    f"{module_id} detail {detail['id']} obstructs socket {socket['id']}")


def interior_collision(
    module_id: str,
    clear_size: tuple[float, float, float],
    portal_sockets: list[dict],
    door_width: float,
    door_height: float,
    production_fixtures: dict,
) -> list[dict]:
    width, height, length = clear_size
    side_openings, end_portals = portal_boundaries(
        module_id, clear_size, portal_sockets)
    boxes = [
        box_record("floor", (0.0, -height * 0.5 - 0.05, 0.0),
                   (width + 0.1, 0.1, length + 0.1), walkable=True),
        box_record("ceiling", (0.0, height * 0.5 + 0.05, 0.0),
                   (width + 0.1, 0.1, length + 0.1), walkable=False),
    ]
    for side in (-1, 1):
        boxes.extend(side_wall_boxes(
            side=side, width=width, height=height, length=length,
            openings_z=side_openings[side], door_width=door_width,
            door_height=door_height))
        boxes.extend(end_boxes(
            side=side, width=width, height=height, length=length,
            is_portal=end_portals[side], door_width=door_width,
            door_height=door_height))
    details = detail_boxes(
        module_id, width, height, production_fixtures)
    validate_entry_clearance(
        module_id, clear_size, portal_sockets, details, door_width, door_height)
    boxes.extend(details)
    return sorted(boxes, key=lambda item: item["id"])


def exterior_collision() -> list[dict]:
    return sorted([
        box_record("keel", (0.0, -3.2, 0.0), (15.2, 0.6, 27.2), walkable=False),
        box_record("roof", (0.0, 3.2, 0.0), (14.0, 0.6, 26.0), walkable=False),
        box_record("hull_left", (-7.7, 0.0, 0.0), (0.6, 6.0, 26.0), walkable=False),
        box_record("hull_right", (7.7, 0.0, 0.0), (0.6, 6.0, 26.0), walkable=False),
        box_record("nose", (0.0, 0.0, 13.7), (14.0, 6.0, 0.6), walkable=False),
        box_record("aft_left", (-4.15, 0.0, -13.7), (6.3, 6.0, 0.6), walkable=False),
        box_record("aft_right", (4.15, 0.0, -13.7), (6.3, 6.0, 0.6), walkable=False),
    ], key=lambda item: item["id"])


def generate(dimensions_path: Path, manifest_path: Path, output_dir: Path) -> dict:
    dimensions = json.loads(dimensions_path.read_text(encoding="utf-8"))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    modules = {item["id"]: item for item in dimensions["modules"]}
    sockets_by_module: dict[str, list[dict]] = {}
    for socket in manifest["sockets"]:
        if socket["type"] == "portal":
            sockets_by_module.setdefault(socket["module"], []).append(socket)
    door_width = float(dimensions["human_factors"]["nominal_door_clear_width"])
    door_height = float(dimensions["human_factors"]["nominal_door_clear_height"])
    production_fixtures = dimensions.get("production_fixtures") or {}
    if not isinstance(production_fixtures, dict):
        raise CollisionGenerationError("production_fixtures must be an object")
    if door_width < float(dimensions["human_factors"]["minimum_door_clear_width"]):
        raise CollisionGenerationError("nominal door is narrower than the minimum")

    output_dir.mkdir(parents=True, exist_ok=True)
    outputs = []
    for module_id in MODULE_ORDER:
        if module_id == "exterior_hull":
            boxes = exterior_collision()
        else:
            size = tuple(float(value) for value in modules[module_id]["clear_size_m"])
            boxes = interior_collision(
                module_id, size, sockets_by_module.get(module_id, []),
                door_width, door_height, production_fixtures)
        document = {
            "schema_version": 1,
            "collision_id": f"frontier_courier.{module_id}",
            "boxes": boxes,
        }
        path = output_dir / f"frontier_courier_{module_id}.tdcollision.json"
        path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        outputs.append({"module": module_id, "path": str(path), "boxes": len(boxes)})
    return {"outputs": outputs, "total_boxes": sum(item["boxes"] for item in outputs)}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dimensions", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = generate(
        args.dimensions.resolve(), args.manifest.resolve(), args.output_dir.resolve())
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CollisionGenerationError, KeyError, ValueError) as exc:
        print(f"error: {exc}")
        raise SystemExit(2)
