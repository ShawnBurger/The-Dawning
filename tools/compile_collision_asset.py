#!/usr/bin/env python3
"""Compile an authored collision JSON document into runtime-only bytes."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import struct
import sys
import tempfile
from pathlib import Path
from typing import Any


MAGIC = b"TDCOLL\0\0"
FORMAT_VERSION = 1
SCHEMA_VERSION = 1
HEADER_STRUCT = struct.Struct("<8sIIQQ32sQ")
HEADER_BYTES = HEADER_STRUCT.size
MAX_SOURCE_BYTES = 16 * 1024 * 1024
MAX_OUTPUT_BYTES = 64 * 1024 * 1024
MAX_STRING_BYTES = 1024 * 1024
MAX_BOXES = 1_000_000
MIN_HALF_EXTENT_METERS = 1.0e-5
MAX_COORDINATE_METERS = 1.0e9


class CollisionCompileError(RuntimeError):
    pass


class Writer:
    def __init__(self) -> None:
        self.data = bytearray()

    def u8(self, value: int) -> None:
        self.data.extend(struct.pack("<B", value))

    def u16(self, value: int) -> None:
        self.data.extend(struct.pack("<H", value))

    def u32(self, value: int) -> None:
        if not 0 <= value <= 0xFFFFFFFF:
            raise CollisionCompileError("integer does not fit in uint32")
        self.data.extend(struct.pack("<I", value))

    def f64(self, value: float) -> None:
        self.data.extend(struct.pack("<d", value))

    def bytes(self, value: bytes) -> None:
        self.data.extend(value)

    def string(self, value: str) -> None:
        encoded = value.encode("utf-8")
        if len(encoded) > MAX_STRING_BYTES:
            raise CollisionCompileError("UTF-8 string exceeds the runtime limit")
        self.u32(len(encoded))
        self.bytes(encoded)

    def vec3(self, value: list[float]) -> None:
        for component in value:
            self.f64(component)


def _is_id(value: object) -> bool:
    if not isinstance(value, str) or not value:
        return False
    separators = "._-"
    previous_separator = True
    for character in value:
        alphanumeric = "a" <= character <= "z" or "0" <= character <= "9"
        current_separator = character in separators
        if not alphanumeric and not current_separator:
            return False
        if current_separator and previous_separator:
            return False
        previous_separator = current_separator
    return not previous_separator


def _vec3(value: object, label: str, *, positive: bool = False) -> list[float]:
    if not isinstance(value, list) or len(value) != 3:
        raise CollisionCompileError(f"{label} must be a three-component array")
    result: list[float] = []
    for component in value:
        if isinstance(component, bool) or not isinstance(component, (int, float)):
            raise CollisionCompileError(f"{label} components must be numbers")
        converted = float(component)
        if not math.isfinite(converted) or abs(converted) > MAX_COORDINATE_METERS:
            raise CollisionCompileError(f"{label} component is non-finite or out of range")
        if positive and converted < MIN_HALF_EXTENT_METERS:
            raise CollisionCompileError(f"{label} components must be positive")
        result.append(converted)
    return result


def validate_document(document: object) -> dict[str, Any]:
    if not isinstance(document, dict):
        raise CollisionCompileError("collision source must be a JSON object")
    if set(document) != {"schema_version", "collision_id", "boxes"}:
        raise CollisionCompileError("collision source has missing or unknown fields")
    if document["schema_version"] != SCHEMA_VERSION:
        raise CollisionCompileError("collision schema version is unsupported")
    if not _is_id(document["collision_id"]):
        raise CollisionCompileError("collision_id must be a canonical lowercase ID")
    boxes = document["boxes"]
    if not isinstance(boxes, list) or not boxes or len(boxes) > MAX_BOXES:
        raise CollisionCompileError("boxes must be a nonempty bounded array")

    normalized = copy.deepcopy(document)
    seen: set[str] = set()
    normalized_boxes: list[dict[str, Any]] = []
    for index, source in enumerate(boxes):
        if not isinstance(source, dict) or set(source) != {
            "id", "center_m", "half_extents_m", "walkable"
        }:
            raise CollisionCompileError(f"box {index} has missing or unknown fields")
        shape_id = source["id"]
        if not _is_id(shape_id) or shape_id in seen:
            raise CollisionCompileError("box IDs must be unique canonical lowercase IDs")
        seen.add(shape_id)
        if not isinstance(source["walkable"], bool):
            raise CollisionCompileError(f"box {shape_id} walkable must be boolean")
        normalized_boxes.append({
            "id": shape_id,
            "center_m": _vec3(source["center_m"], f"box {shape_id} center_m"),
            "half_extents_m": _vec3(
                source["half_extents_m"],
                f"box {shape_id} half_extents_m",
                positive=True,
            ),
            "walkable": source["walkable"],
        })
    normalized["boxes"] = sorted(normalized_boxes, key=lambda item: item["id"])
    return normalized


def _canonical_bytes(document: dict[str, Any]) -> bytes:
    return json.dumps(
        document,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def compile_document(document: object) -> bytes:
    normalized = validate_document(document)
    payload = Writer()
    payload.u32(SCHEMA_VERSION)
    payload.string(normalized["collision_id"])
    payload.bytes(hashlib.sha256(_canonical_bytes(normalized)).digest())
    payload.u32(len(normalized["boxes"]))
    for box in normalized["boxes"]:
        payload.u8(1)  # box
        payload.u8(1 if box["walkable"] else 0)
        payload.u16(0)
        payload.string(box["id"])
        payload.vec3(box["center_m"])
        payload.vec3(box["half_extents_m"])

    payload_bytes = bytes(payload.data)
    file_bytes = HEADER_BYTES + len(payload_bytes)
    if file_bytes > MAX_OUTPUT_BYTES:
        raise CollisionCompileError("compiled collision exceeds the output limit")
    header = HEADER_STRUCT.pack(
        MAGIC,
        FORMAT_VERSION,
        HEADER_BYTES,
        file_bytes,
        len(payload_bytes),
        hashlib.sha256(payload_bytes).digest(),
        0,
    )
    return header + payload_bytes


def compile_file(source: Path, output: Path) -> int:
    try:
        if source.resolve() == output.resolve():
            raise CollisionCompileError("source and output paths must differ")
        source_size = source.stat().st_size
        if source_size > MAX_SOURCE_BYTES:
            raise CollisionCompileError("collision source exceeds the input limit")
        document = json.loads(source.read_text(encoding="utf-8"))
    except CollisionCompileError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CollisionCompileError(f"could not read collision source: {exc}") from exc

    cooked = compile_document(document)
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(cooked)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
    except OSError as exc:
        temporary.unlink(missing_ok=True)
        raise CollisionCompileError(f"could not publish cooked collision: {exc}") from exc
    return len(cooked)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Compile a .tdcollision.json source into a runtime .tdcollision file."
    )
    parser.add_argument("source", type=Path)
    parser.add_argument("output", nargs="?", type=Path)
    args = parser.parse_args(argv)
    output = args.output
    if output is None:
        name = args.source.name
        if name.endswith(".tdcollision.json"):
            output = args.source.with_name(name[: -len(".json")])
        else:
            output = args.source.with_suffix(".tdcollision")
    try:
        byte_count = compile_file(args.source, output)
    except CollisionCompileError as exc:
        print(f"collision compile failed: {exc}", file=sys.stderr)
        return 1
    print(f"wrote {output} ({byte_count} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
