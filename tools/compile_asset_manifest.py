#!/usr/bin/env python3
"""Compile a validated boardable-asset manifest into runtime-only bytes."""

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

from validate_asset_manifest import validate_document


MAGIC = b"TDASMB\0\0"
FORMAT_VERSION = 1
HEADER_STRUCT = struct.Struct("<8sIIQQ32sQ")
HEADER_BYTES = HEADER_STRUCT.size
OUTSIDE_INDEX = 0xFFFFFFFF
NO_INDEX = 0xFFFFFFFF
MAX_SOURCE_BYTES = 16 * 1024 * 1024
MAX_OUTPUT_BYTES = 64 * 1024 * 1024
MAX_STRING_BYTES = 1024 * 1024

ASSET_KIND = {"ship": 1, "structure": 2}
MODULE_ROLE = {"exterior": 1, "interior": 2}
COLLISION_TYPE = {"convex": 1, "compound": 2, "mesh": 3}
SOCKET_TYPE = {"portal": 1, "interaction": 2, "attachment": 3, "spawn": 4}
PRESSURE_CLASS = {"pressurized": 1, "unpressurized": 2, "airlock": 3}
INTERACTION_TYPE = {
    "airlock": 1,
    "console": 2,
    "door": 3,
    "elevator": 4,
    "hatch": 5,
    "ladder": 6,
    "seat": 7,
}
MOTION_TYPE = {"linear": 1, "rotational": 2}
LIGHT_TYPE = {"point": 1, "spot": 2}
LIGHT_SHADOW_POLICY = {"none": 1, "static": 2, "dynamic": 3}
LIGHT_EMERGENCY_BEHAVIOR = {
    "unchanged": 1,
    "off": 2,
    "emergency_only": 3,
    "override": 4,
}


class AssemblyCompileError(RuntimeError):
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
            raise AssemblyCompileError("integer does not fit in uint32")
        self.data.extend(struct.pack("<I", value))

    def f64(self, value: float) -> None:
        self.data.extend(struct.pack("<d", float(value)))

    def bytes(self, value: bytes) -> None:
        self.data.extend(value)

    def string(self, value: str) -> None:
        encoded = value.encode("utf-8")
        if len(encoded) > MAX_STRING_BYTES:
            raise AssemblyCompileError("UTF-8 string exceeds the runtime limit")
        self.u32(len(encoded))
        self.bytes(encoded)

    def vector3(self, value: list[int | float]) -> None:
        for component in value:
            self.f64(component)


def _canonical_bytes(document: dict[str, Any]) -> bytes:
    normalized = copy.deepcopy(document)
    for key in (
        "modules",
        "sockets",
        "zones",
        "portals",
        "interactions",
        "moving_parts",
    ):
        normalized[key] = sorted(normalized[key], key=lambda item: item["id"])
    if normalized.get("schema_version", 0) >= 2:
        normalized["light_fixtures"] = sorted(
            normalized["light_fixtures"], key=lambda item: item["id"])
    normalized["provenance"]["sources"] = sorted(
        normalized["provenance"]["sources"], key=lambda item: item["id"]
    )
    normalized["navigation"]["required_reachable_zones"] = sorted(
        normalized["navigation"]["required_reachable_zones"]
    )
    return json.dumps(
        normalized,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _sha256_hex(value: str, label: str) -> bytes:
    try:
        digest = bytes.fromhex(value)
    except ValueError as exc:
        raise AssemblyCompileError(f"{label} is not hexadecimal") from exc
    if len(digest) != 32 or value != value.lower():
        raise AssemblyCompileError(f"{label} must be a lowercase SHA-256 digest")
    return digest


def _sorted_table(document: dict[str, Any], key: str) -> list[dict[str, Any]]:
    return sorted(document[key], key=lambda item: item["id"])


def _index_by_id(values: list[dict[str, Any]]) -> dict[str, int]:
    return {value["id"]: index for index, value in enumerate(values)}


def _validate_runtime_wiring(document: dict[str, Any]) -> None:
    def require_safe_text(value: str, label: str) -> None:
        if not value or any(ord(character) < 0x20 or ord(character) == 0x7F for character in value):
            raise AssemblyCompileError(f"{label} contains an unsafe control character")

    require_safe_text(
        document["provenance"]["assembly_revision"], "assembly_revision"
    )
    for source in document["provenance"]["sources"]:
        require_safe_text(source["provider"], f"source '{source['id']}' provider")
    for module in document["modules"]:
        require_safe_text(module["visual_source"], f"module '{module['id']}' visual_source")
        require_safe_text(
            module["collision"]["source"], f"module '{module['id']}' collision source"
        )
        for lod in module["lods"]:
            require_safe_text(lod["source"], f"module '{module['id']}' LOD source")
    for zone in document["zones"]:
        require_safe_text(zone["navmesh_source"], f"zone '{zone['id']}' navmesh_source")
        require_safe_text(
            zone["walkable_surface"], f"zone '{zone['id']}' walkable_surface"
        )
    for part in document["moving_parts"]:
        require_safe_text(part["visual_source"], f"moving part '{part['id']}' visual_source")

    sockets = {socket["id"]: socket for socket in document["sockets"]}
    interactions = {
        interaction["id"]: interaction for interaction in document["interactions"]
    }
    used_portal_sockets: set[str] = set()
    for portal in document["portals"]:
        closure_socket = interactions[portal["closure_interaction"]]["socket"]
        if closure_socket not in {portal["socket_a"], portal["socket_b"]}:
            raise AssemblyCompileError(
                f"portal '{portal['id']}' closure must use one of its endpoint sockets"
            )
        for key in ("socket_a", "socket_b"):
            socket_id = portal[key]
            if socket_id in used_portal_sockets:
                raise AssemblyCompileError(
                    f"portal socket '{socket_id}' is reused by multiple endpoints"
                )
            used_portal_sockets.add(socket_id)
            if sockets[socket_id]["type"] != "portal":
                raise AssemblyCompileError(
                    f"portal socket '{socket_id}' must have type 'portal'"
                )
    moving_types = {"airlock", "door", "elevator", "hatch"}
    for interaction in document["interactions"]:
        expected = "portal" if interaction["type"] in moving_types else "interaction"
        if sockets[interaction["socket"]]["type"] != expected:
            raise AssemblyCompileError(
                f"interaction '{interaction['id']}' requires a {expected} socket"
            )


def compile_document(document: dict[str, Any]) -> bytes:
    errors = validate_document(document)
    if errors:
        formatted = "\n".join(f"  {error}" for error in errors)
        raise AssemblyCompileError(f"manifest failed production validation:\n{formatted}")
    _validate_runtime_wiring(document)

    sources = sorted(document["provenance"]["sources"], key=lambda item: item["id"])
    modules = _sorted_table(document, "modules")
    sockets = _sorted_table(document, "sockets")
    zones = _sorted_table(document, "zones")
    portals = _sorted_table(document, "portals")
    interactions = _sorted_table(document, "interactions")
    moving_parts = _sorted_table(document, "moving_parts")
    light_fixtures = (
        _sorted_table(document, "light_fixtures")
        if document["schema_version"] >= 2 else []
    )

    source_index = _index_by_id(sources)
    module_index = _index_by_id(modules)
    socket_index = _index_by_id(sockets)
    zone_index = _index_by_id(zones)
    portal_index = _index_by_id(portals)
    interaction_index = _index_by_id(interactions)
    moving_part_index = _index_by_id(moving_parts)

    writer = Writer()
    writer.bytes(hashlib.sha256(_canonical_bytes(document)).digest())
    writer.u32(document["schema_version"])
    writer.u8(ASSET_KIND[document["asset_kind"]])
    contract = document["interior_contract"]
    flags = (
        (1 if contract["continuous_world"] else 0)
        | (2 if contract["loading_screen_free"] else 0)
        | (4 if contract["fully_interactive"] else 0)
    )
    writer.u8(flags)
    writer.u16(0)
    writer.string(document["asset_id"])
    writer.string(document["provenance"]["assembly_revision"])
    writer.f64(contract["minimum_clearance_m"])
    writer.f64(contract["minimum_door_width_m"])

    writer.u32(len(sources))
    for source in sources:
        writer.string(source["id"])
        writer.string(source["provider"])
        writer.bytes(_sha256_hex(source["request_hash"], "request_hash"))

    writer.u32(len(modules))
    for module in modules:
        writer.string(module["id"])
        writer.u8(MODULE_ROLE[module["role"]])
        writer.u8(COLLISION_TYPE[module["collision"]["type"]])
        writer.u16(0)
        writer.u32(source_index[module["provenance_ref"]])
        writer.string(module["visual_source"])
        transform = module["transform"]
        writer.vector3(transform["position_m"])
        writer.vector3(transform["rotation_euler_degrees"])
        writer.vector3(transform["scale"])
        writer.string(module["collision"]["source"])
        lods = sorted(module["lods"], key=lambda lod: lod["level"])
        writer.u32(len(lods))
        for lod in lods:
            writer.u32(lod["level"])
            writer.string(lod["source"])
            writer.f64(lod["max_distance_m"])

    writer.u32(len(sockets))
    for socket in sockets:
        writer.string(socket["id"])
        writer.u32(module_index[socket["module"]])
        writer.u8(SOCKET_TYPE[socket["type"]])
        writer.u8(0)
        writer.u16(0)
        writer.vector3(socket["position_m"])
        writer.vector3(socket["forward"])
        writer.vector3(socket["up"])

    writer.u32(len(zones))
    for zone in zones:
        writer.string(zone["id"])
        writer.u32(module_index[zone["module"]])
        writer.u8(PRESSURE_CLASS[zone["pressure"]])
        writer.u8(0)
        writer.u16(0)
        writer.string(zone["navmesh_source"])
        writer.string(zone["walkable_surface"])

    writer.u32(len(portals))
    for portal in portals:
        writer.string(portal["id"])
        writer.u32(OUTSIDE_INDEX if portal["a"] == "outside" else zone_index[portal["a"]])
        writer.u32(OUTSIDE_INDEX if portal["b"] == "outside" else zone_index[portal["b"]])
        writer.u32(socket_index[portal["socket_a"]])
        writer.u32(socket_index[portal["socket_b"]])
        writer.u32(interaction_index[portal["closure_interaction"]])
        writer.u8((1 if portal["sealable"] else 0) | (2 if portal["nav_link"] else 0))
        writer.u8(0)
        writer.u16(0)

    writer.u32(len(interactions))
    for interaction in interactions:
        writer.string(interaction["id"])
        writer.u8(INTERACTION_TYPE[interaction["type"]])
        writer.u8(0)
        writer.u16(0)
        writer.u32(module_index[interaction["module"]])
        writer.u32(socket_index[interaction["socket"]])
        states = interaction["states"]
        writer.u32(len(states))
        for state in states:
            writer.string(state)
        writer.u32(states.index(interaction["initial_state"]))
        writer.u32(
            moving_part_index[interaction["moving_part"]]
            if "moving_part" in interaction else NO_INDEX
        )
        writer.u32(
            portal_index[interaction["portal"]]
            if "portal" in interaction else NO_INDEX
        )

    writer.u32(len(moving_parts))
    for part in moving_parts:
        writer.string(part["id"])
        writer.u32(module_index[part["module"]])
        writer.u32(interaction_index[part["interaction"]])
        writer.string(part["visual_source"])
        writer.u8(MOTION_TYPE[part["motion"]["type"]])
        writer.u8(0)
        writer.u16(0)
        writer.vector3(part["pivot_m"])
        writer.vector3(part["motion"]["axis"])
        writer.f64(part["motion"]["travel"])

    navigation = document["navigation"]
    writer.u32(zone_index[navigation["entry_zone"]])
    required = sorted(
        (zone_index[zone] for zone in navigation["required_reachable_zones"])
    )
    writer.u32(len(required))
    for index in required:
        writer.u32(index)

    if document["schema_version"] >= 2:
        writer.u32(len(light_fixtures))
        for fixture in light_fixtures:
            writer.string(fixture["id"])
            writer.u32(module_index[fixture["module"]])
            writer.u8(LIGHT_TYPE[fixture["type"]])
            writer.u8(LIGHT_SHADOW_POLICY[fixture["shadow_policy"]])
            writer.u8(LIGHT_EMERGENCY_BEHAVIOR[fixture["emergency_behavior"]])
            writer.u8(0)
            writer.vector3(fixture["position_m"])
            direction = fixture["direction"]
            direction_length = math.sqrt(
                sum(float(component) * float(component) for component in direction)
            )
            writer.vector3([
                float(component) / direction_length for component in direction
            ])
            writer.f64(fixture["color_temperature_k"])
            writer.f64(fixture["intensity_lm_or_cd"])
            writer.f64(fixture["range_m"])
            writer.f64(fixture["inner_cone_degrees"])
            writer.f64(fixture["outer_cone_degrees"])
            writer.f64(fixture["importance"])
            writer.f64(fixture["emergency_color_temperature_k"])
            writer.f64(fixture["emergency_intensity_scale"])
            writer.string(fixture["group"])
            writer.string(fixture["circuit"])

    payload = bytes(writer.data)
    if len(payload) > MAX_OUTPUT_BYTES - HEADER_BYTES:
        raise AssemblyCompileError("compiled payload exceeds the runtime file limit")
    header = HEADER_STRUCT.pack(
        MAGIC,
        FORMAT_VERSION,
        HEADER_BYTES,
        HEADER_BYTES + len(payload),
        len(payload),
        hashlib.sha256(payload).digest(),
        0,
    )
    return header + payload


def compile_file(source: Path, output: Path) -> int:
    try:
        source_size = source.stat().st_size
    except OSError as exc:
        raise AssemblyCompileError(f"could not inspect source manifest: {exc}") from exc
    if source_size > MAX_SOURCE_BYTES:
        raise AssemblyCompileError("source manifest exceeds the compiler limit")
    try:
        source_bytes = source.read_bytes()
        if len(source_bytes) > MAX_SOURCE_BYTES:
            raise AssemblyCompileError("source manifest exceeds the compiler limit")
        document = json.loads(source_bytes.decode("utf-8"))
    except AssemblyCompileError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AssemblyCompileError(f"could not read source manifest: {exc}") from exc
    if not isinstance(document, dict):
        raise AssemblyCompileError("source manifest root must be an object")
    cooked = compile_document(document)

    source_resolved = source.resolve(strict=False)
    output_resolved = output.resolve(strict=False)
    aliases_source = os.path.normcase(source_resolved) == os.path.normcase(output_resolved)
    if output.exists():
        try:
            aliases_source = aliases_source or source.samefile(output)
        except OSError:
            pass
    if aliases_source:
        raise AssemblyCompileError("source and cooked output paths must differ")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            dir=output.parent,
            prefix=f".{output.name}.",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary_name = temporary.name
            temporary.write(cooked)
            temporary.flush()
            os.fsync(temporary.fileno())
        if Path(temporary_name).read_bytes() != cooked:
            raise AssemblyCompileError("temporary cooked output verification failed")
        os.replace(temporary_name, output)
    finally:
        if temporary_name is not None:
            try:
                Path(temporary_name).unlink(missing_ok=True)
            except OSError:
                pass
    return len(cooked)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Compile a validated .tdasset.json into a runtime .tdassembly file."
    )
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    args = parser.parse_args(argv)
    output = args.output
    if output is None:
        name = args.source.name
        if name.endswith(".tdasset.json"):
            name = name[: -len(".tdasset.json")] + ".tdassembly"
            output = args.source.with_name(name)
        else:
            output = args.source.with_suffix(".tdassembly")
    try:
        size = compile_file(args.source, output)
    except AssemblyCompileError as exc:
        print(f"compile failed: {exc}", file=sys.stderr)
        return 1
    print(f"cooked={output}")
    print(f"bytes={size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
