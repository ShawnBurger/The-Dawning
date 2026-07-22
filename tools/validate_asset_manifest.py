#!/usr/bin/env python3
"""Validate The Dawning's production ship/structure assembly contract.

Meshy output is source art, not authoritative gameplay topology. This validator
keeps the engine-owned facts needed by a continuous, interactive interior
explicit and reviewable before an asset can be promoted into runtime content.
It intentionally uses only the Python standard library so CI and artists can run
the exact same gate without installing a schema package.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections import defaultdict, deque
from pathlib import Path
from typing import Any


SUPPORTED_SCHEMA_VERSIONS = {1, 2}
ID_PATTERN = re.compile(r"^[a-z0-9]+(?:[._-][a-z0-9]+)*$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
ASSET_KINDS = {"ship", "structure"}
MODULE_ROLES = {"exterior", "interior"}
PRESSURE_CLASSES = {"pressurized", "unpressurized", "airlock"}
SOCKET_TYPES = {"portal", "interaction", "attachment", "spawn"}
INTERACTION_TYPES = {
    "airlock", "console", "door", "elevator", "hatch", "ladder", "seat"
}
MOVING_INTERACTIONS = {"airlock", "door", "elevator", "hatch"}
COLLISION_TYPES = {"convex", "compound", "mesh"}
MOTION_TYPES = {"linear", "rotational"}
LIGHT_TYPES = {"point", "spot"}
LIGHT_SHADOW_POLICIES = {"none", "static", "dynamic"}
LIGHT_EMERGENCY_BEHAVIORS = {
    "unchanged", "off", "emergency_only", "override"
}
MIN_COLOR_TEMPERATURE_K = 1000.0
MAX_COLOR_TEMPERATURE_K = 20000.0
MAX_LIGHT_INTENSITY = 1.0e9
MAX_LIGHT_RANGE_M = 10000.0
MAX_EMERGENCY_INTENSITY_SCALE = 8.0
MAX_INTERIOR_LIGHT_FIXTURES = 4096
MAX_INTERIOR_LIGHT_GROUPS = 1024
MAX_INTERIOR_LIGHT_CIRCUITS = 1024
MAX_INTERIOR_LIGHT_ID_BYTES = 256


class ManifestValidator:
    def __init__(self, document: Any) -> None:
        self.document = document
        self.errors: list[str] = []

    def error(self, path: str, message: str) -> None:
        self.errors.append(f"{path}: {message}")

    def require_object(self, value: Any, path: str) -> dict[str, Any] | None:
        if not isinstance(value, dict):
            self.error(path, "must be an object")
            return None
        return value

    def require_list(self, value: Any, path: str, *, nonempty: bool = True) -> list[Any] | None:
        if not isinstance(value, list):
            self.error(path, "must be an array")
            return None
        if nonempty and not value:
            self.error(path, "must not be empty")
        return value

    def require_string(self, value: Any, path: str) -> str | None:
        if not isinstance(value, str) or not value.strip():
            self.error(path, "must be a nonempty string")
            return None
        return value

    def require_id(self, value: Any, path: str) -> str | None:
        result = self.require_string(value, path)
        if result is not None and not ID_PATTERN.fullmatch(result):
            self.error(path, "must use lowercase letters, digits, '.', '_' or '-'")
            return None
        return result

    def require_number(self, value: Any, path: str, *, positive: bool = False) -> float | None:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            self.error(path, "must be a finite number")
            return None
        number = float(value)
        if not math.isfinite(number):
            self.error(path, "must be a finite number")
            return None
        if positive and number <= 0.0:
            self.error(path, "must be greater than zero")
            return None
        return number

    def require_vector3(self, value: Any, path: str, *, nonzero: bool = False) -> list[float] | None:
        if not isinstance(value, list) or len(value) != 3:
            self.error(path, "must be a three-number array")
            return None
        vector: list[float] = []
        for index, component in enumerate(value):
            number = self.require_number(component, f"{path}[{index}]")
            if number is None:
                return None
            vector.append(number)
        if nonzero and math.sqrt(sum(component * component for component in vector)) < 1.0e-6:
            self.error(path, "must not be the zero vector")
            return None
        return vector

    def collect_objects(self, key: str) -> tuple[list[dict[str, Any]], dict[str, dict[str, Any]]]:
        values = self.require_list(self.document.get(key), f"$.{key}") or []
        objects: list[dict[str, Any]] = []
        by_id: dict[str, dict[str, Any]] = {}
        for index, value in enumerate(values):
            path = f"$.{key}[{index}]"
            obj = self.require_object(value, path)
            if obj is None:
                continue
            identifier = self.require_id(obj.get("id"), f"{path}.id")
            if identifier is not None:
                if identifier in by_id:
                    self.error(f"{path}.id", f"duplicate id '{identifier}'")
                else:
                    by_id[identifier] = obj
            objects.append(obj)
        return objects, by_id

    def validate_transform(self, value: Any, path: str) -> None:
        transform = self.require_object(value, path)
        if transform is None:
            return
        self.require_vector3(transform.get("position_m"), f"{path}.position_m")
        self.require_vector3(
            transform.get("rotation_euler_degrees"),
            f"{path}.rotation_euler_degrees")
        scale = self.require_vector3(transform.get("scale"), f"{path}.scale")
        if scale is not None and any(component <= 0.0 for component in scale):
            self.error(f"{path}.scale", "all components must be greater than zero")

    def validate(self) -> list[str]:
        root = self.require_object(self.document, "$")
        if root is None:
            return self.errors
        schema_version = root.get("schema_version")
        if (isinstance(schema_version, bool) or
                not isinstance(schema_version, int) or
                schema_version not in SUPPORTED_SCHEMA_VERSIONS):
            self.error(
                "$.schema_version",
                f"must be one of {sorted(SUPPORTED_SCHEMA_VERSIONS)}")
        self.require_id(root.get("asset_id"), "$.asset_id")
        if root.get("asset_kind") not in ASSET_KINDS:
            self.error("$.asset_kind", f"must be one of {sorted(ASSET_KINDS)}")
        if root.get("units") != "meters":
            self.error("$.units", "must be 'meters'")

        coordinates = self.require_object(root.get("coordinate_system"), "$.coordinate_system")
        if coordinates is not None:
            expected = {"handedness": "left", "up": "+Y", "forward": "+Z"}
            for key, value in expected.items():
                if coordinates.get(key) != value:
                    self.error(f"$.coordinate_system.{key}", f"must be '{value}'")

        contract = self.require_object(root.get("interior_contract"), "$.interior_contract")
        if contract is not None:
            for key in ("continuous_world", "loading_screen_free", "fully_interactive"):
                if contract.get(key) is not True:
                    self.error(f"$.interior_contract.{key}", "must be true")
            self.require_number(
                contract.get("minimum_clearance_m"),
                "$.interior_contract.minimum_clearance_m", positive=True)
            self.require_number(
                contract.get("minimum_door_width_m"),
                "$.interior_contract.minimum_door_width_m", positive=True)

        source_ids = self.validate_provenance(root.get("provenance"))
        modules, module_by_id = self.collect_objects("modules")
        sockets, socket_by_id = self.collect_objects("sockets")
        zones, zone_by_id = self.collect_objects("zones")
        portals, portal_by_id = self.collect_objects("portals")
        interactions, interaction_by_id = self.collect_objects("interactions")
        moving_parts, moving_part_by_id = self.collect_objects("moving_parts")
        light_fixtures: list[dict[str, Any]] = []
        if schema_version == 2:
            light_fixtures, _ = self.collect_objects("light_fixtures")
        elif "light_fixtures" in root:
            self.error(
                "$.light_fixtures",
                "requires schema_version 2")

        self.validate_modules(modules, module_by_id, source_ids)
        self.validate_sockets(sockets, module_by_id)
        self.validate_zones(zones, zone_by_id, module_by_id)
        self.validate_moving_parts(moving_parts, module_by_id, interaction_by_id)
        self.validate_interactions(
            interactions, module_by_id, socket_by_id, portal_by_id, moving_part_by_id)
        self.validate_portals(
            portals, zone_by_id, socket_by_id, module_by_id, interaction_by_id)
        self.validate_light_fixtures(light_fixtures, module_by_id)
        self.validate_navigation(root.get("navigation"), zone_by_id, portals)
        self.validate_interior_coverage(modules, zones, portals)
        return self.errors

    def validate_provenance(self, value: Any) -> set[str]:
        provenance = self.require_object(value, "$.provenance")
        if provenance is None:
            return set()
        sources = self.require_list(provenance.get("sources"), "$.provenance.sources") or []
        source_ids: set[str] = set()
        for index, value in enumerate(sources):
            path = f"$.provenance.sources[{index}]"
            source = self.require_object(value, path)
            if source is None:
                continue
            identifier = self.require_id(source.get("id"), f"{path}.id")
            if identifier is not None:
                if identifier in source_ids:
                    self.error(f"{path}.id", f"duplicate id '{identifier}'")
                source_ids.add(identifier)
            self.require_string(source.get("provider"), f"{path}.provider")
            self.require_string(source.get("manifest"), f"{path}.manifest")
            digest = self.require_string(source.get("request_hash"), f"{path}.request_hash")
            if digest is not None and not SHA256_PATTERN.fullmatch(digest):
                self.error(f"{path}.request_hash", "must be a lowercase SHA-256 digest")
        self.require_string(provenance.get("assembly_revision"), "$.provenance.assembly_revision")
        return source_ids

    def validate_modules(
        self,
        modules: list[dict[str, Any]],
        module_by_id: dict[str, dict[str, Any]],
        source_ids: set[str],
    ) -> None:
        roles: set[str] = set()
        for index, module in enumerate(modules):
            path = f"$.modules[{index}]"
            role = module.get("role")
            if role not in MODULE_ROLES:
                self.error(f"{path}.role", f"must be one of {sorted(MODULE_ROLES)}")
            else:
                roles.add(role)
            source_ref = self.require_id(module.get("provenance_ref"), f"{path}.provenance_ref")
            if source_ref is not None and source_ref not in source_ids:
                self.error(f"{path}.provenance_ref", f"unknown source '{source_ref}'")
            self.require_string(module.get("visual_source"), f"{path}.visual_source")
            self.validate_transform(module.get("transform"), f"{path}.transform")
            collision = self.require_object(module.get("collision"), f"{path}.collision")
            if collision is not None:
                if collision.get("type") not in COLLISION_TYPES:
                    self.error(
                        f"{path}.collision.type",
                        f"must be one of {sorted(COLLISION_TYPES)}")
                self.require_string(collision.get("source"), f"{path}.collision.source")
            lods = self.require_list(module.get("lods"), f"{path}.lods") or []
            if len(lods) < 3:
                self.error(f"{path}.lods", "production modules require at least LOD0, LOD1 and LOD2")
            levels: list[int] = []
            distances: list[float] = []
            for lod_index, value in enumerate(lods):
                lod_path = f"{path}.lods[{lod_index}]"
                lod = self.require_object(value, lod_path)
                if lod is None:
                    continue
                level = lod.get("level")
                if not isinstance(level, int) or isinstance(level, bool) or level < 0:
                    self.error(f"{lod_path}.level", "must be a nonnegative integer")
                else:
                    levels.append(level)
                self.require_string(lod.get("source"), f"{lod_path}.source")
                distance = self.require_number(
                    lod.get("max_distance_m"), f"{lod_path}.max_distance_m", positive=True)
                if distance is not None:
                    distances.append(distance)
            if levels and levels != list(range(len(levels))):
                self.error(f"{path}.lods", "levels must be contiguous and start at zero")
            if distances and distances != sorted(distances):
                self.error(f"{path}.lods", "max distances must increase with LOD level")
        if module_by_id and roles != MODULE_ROLES:
            self.error("$.modules", "boardable assets require exterior and interior modules")

    def validate_sockets(
        self,
        sockets: list[dict[str, Any]],
        module_by_id: dict[str, dict[str, Any]],
    ) -> None:
        for index, socket in enumerate(sockets):
            path = f"$.sockets[{index}]"
            module = self.require_id(socket.get("module"), f"{path}.module")
            if module is not None and module not in module_by_id:
                self.error(f"{path}.module", f"unknown module '{module}'")
            if socket.get("type") not in SOCKET_TYPES:
                self.error(f"{path}.type", f"must be one of {sorted(SOCKET_TYPES)}")
            self.require_vector3(socket.get("position_m"), f"{path}.position_m")
            forward = self.require_vector3(socket.get("forward"), f"{path}.forward", nonzero=True)
            up = self.require_vector3(socket.get("up"), f"{path}.up", nonzero=True)
            if forward is not None and up is not None:
                forward_length = math.sqrt(sum(value * value for value in forward))
                up_length = math.sqrt(sum(value * value for value in up))
                cosine = sum(a * b for a, b in zip(forward, up)) / (forward_length * up_length)
                if abs(cosine) > 0.01:
                    self.error(path, "forward and up must be approximately orthogonal")

    def validate_zones(
        self,
        zones: list[dict[str, Any]],
        zone_by_id: dict[str, dict[str, Any]],
        module_by_id: dict[str, dict[str, Any]],
    ) -> None:
        for index, zone in enumerate(zones):
            path = f"$.zones[{index}]"
            module_id = self.require_id(zone.get("module"), f"{path}.module")
            module = module_by_id.get(module_id or "")
            if module_id is not None and module is None:
                self.error(f"{path}.module", f"unknown module '{module_id}'")
            elif module is not None and module.get("role") != "interior":
                self.error(f"{path}.module", "zones must belong to interior modules")
            if zone.get("pressure") not in PRESSURE_CLASSES:
                self.error(f"{path}.pressure", f"must be one of {sorted(PRESSURE_CLASSES)}")
            self.require_string(zone.get("navmesh_source"), f"{path}.navmesh_source")
            self.require_string(zone.get("walkable_surface"), f"{path}.walkable_surface")
        if not zone_by_id:
            self.error("$.zones", "boardable assets require at least one interior zone")

    def validate_portals(
        self,
        portals: list[dict[str, Any]],
        zone_by_id: dict[str, dict[str, Any]],
        socket_by_id: dict[str, dict[str, Any]],
        module_by_id: dict[str, dict[str, Any]],
        interaction_by_id: dict[str, dict[str, Any]],
    ) -> None:
        for index, portal in enumerate(portals):
            path = f"$.portals[{index}]"
            endpoints: list[str] = []
            for key in ("a", "b"):
                endpoint = self.require_id(portal.get(key), f"{path}.{key}")
                if endpoint is not None:
                    endpoints.append(endpoint)
                    if endpoint != "outside" and endpoint not in zone_by_id:
                        self.error(f"{path}.{key}", f"unknown zone '{endpoint}'")
            if len(endpoints) == 2 and endpoints[0] == endpoints[1]:
                self.error(path, "portal endpoints must differ")
            for endpoint_key, socket_key in (("a", "socket_a"), ("b", "socket_b")):
                socket_id = self.require_id(portal.get(socket_key), f"{path}.{socket_key}")
                socket = socket_by_id.get(socket_id or "")
                if socket_id is not None and socket is None:
                    self.error(f"{path}.{socket_key}", f"unknown socket '{socket_id}'")
                    continue
                endpoint = portal.get(endpoint_key)
                if socket is None or not isinstance(endpoint, str):
                    continue
                socket_module = socket.get("module")
                if endpoint == "outside":
                    module = module_by_id.get(socket_module)
                    if module is None or module.get("role") != "exterior":
                        self.error(
                            f"{path}.{socket_key}",
                            "the outside-side socket must belong to an exterior module")
                else:
                    zone = zone_by_id.get(endpoint)
                    if zone is not None and socket_module != zone.get("module"):
                        self.error(
                            f"{path}.{socket_key}",
                            f"socket module must match zone '{endpoint}'")
            closure = self.require_id(
                portal.get("closure_interaction"), f"{path}.closure_interaction")
            closure_interaction = interaction_by_id.get(closure or "")
            if closure is not None and closure_interaction is None:
                self.error(
                    f"{path}.closure_interaction", f"unknown interaction '{closure}'")
            elif closure_interaction is not None and closure_interaction.get("portal") != portal.get("id"):
                self.error(
                    f"{path}.closure_interaction",
                    "interaction must point back to this portal")
            if portal.get("sealable") is not True:
                self.error(f"{path}.sealable", "must be true for pressure continuity")
            if portal.get("nav_link") is not True:
                self.error(f"{path}.nav_link", "must be true")

    def validate_interactions(
        self,
        interactions: list[dict[str, Any]],
        module_by_id: dict[str, dict[str, Any]],
        socket_by_id: dict[str, dict[str, Any]],
        portal_by_id: dict[str, dict[str, Any]],
        moving_part_by_id: dict[str, dict[str, Any]],
    ) -> None:
        for index, interaction in enumerate(interactions):
            path = f"$.interactions[{index}]"
            interaction_type = interaction.get("type")
            if interaction_type not in INTERACTION_TYPES:
                self.error(f"{path}.type", f"must be one of {sorted(INTERACTION_TYPES)}")
            module = self.require_id(interaction.get("module"), f"{path}.module")
            module_object = module_by_id.get(module or "")
            if module is not None and module_object is None:
                self.error(f"{path}.module", f"unknown module '{module}'")
            socket = self.require_id(interaction.get("socket"), f"{path}.socket")
            socket_object = socket_by_id.get(socket or "")
            if socket is not None and socket_object is None:
                self.error(f"{path}.socket", f"unknown socket '{socket}'")
            elif socket_object is not None and module is not None:
                if socket_object.get("module") != module:
                    self.error(
                        f"{path}.socket",
                        "socket must belong to the interaction module")
            states = self.require_list(interaction.get("states"), f"{path}.states") or []
            if not all(isinstance(state, str) and state for state in states):
                self.error(f"{path}.states", "must contain only nonempty strings")
            elif len(states) != len(set(states)):
                self.error(f"{path}.states", "must not contain duplicates")
            initial = self.require_string(interaction.get("initial_state"), f"{path}.initial_state")
            if initial is not None and initial not in states:
                self.error(f"{path}.initial_state", "must be present in states")
            if interaction_type in MOVING_INTERACTIONS:
                if not {"closed", "open"}.issubset(set(states)):
                    self.error(f"{path}.states", "moving closures require 'closed' and 'open'")
                part = self.require_id(interaction.get("moving_part"), f"{path}.moving_part")
                moving_part = moving_part_by_id.get(part or "")
                if part is not None and moving_part is None:
                    self.error(f"{path}.moving_part", f"unknown moving part '{part}'")
                elif moving_part is not None and moving_part.get("interaction") != interaction.get("id"):
                    self.error(
                        f"{path}.moving_part",
                        "moving part must point back to this interaction")
                elif moving_part is not None and moving_part.get("module") != module:
                    self.error(
                        f"{path}.moving_part",
                        "moving part must belong to the interaction module")
                portal = self.require_id(interaction.get("portal"), f"{path}.portal")
                if portal is not None and portal not in portal_by_id:
                    self.error(f"{path}.portal", f"unknown portal '{portal}'")

    def validate_moving_parts(
        self,
        moving_parts: list[dict[str, Any]],
        module_by_id: dict[str, dict[str, Any]],
        interaction_by_id: dict[str, dict[str, Any]],
    ) -> None:
        for index, part in enumerate(moving_parts):
            path = f"$.moving_parts[{index}]"
            module = self.require_id(part.get("module"), f"{path}.module")
            if module is not None and module not in module_by_id:
                self.error(f"{path}.module", f"unknown module '{module}'")
            interaction = self.require_id(part.get("interaction"), f"{path}.interaction")
            interaction_object = interaction_by_id.get(interaction or "")
            if interaction is not None and interaction_object is None:
                self.error(f"{path}.interaction", f"unknown interaction '{interaction}'")
            elif interaction_object is not None and module is not None:
                if interaction_object.get("module") != module:
                    self.error(
                        f"{path}.interaction",
                        "interaction must belong to the moving-part module")
            self.require_string(part.get("visual_source"), f"{path}.visual_source")
            self.require_vector3(part.get("pivot_m"), f"{path}.pivot_m")
            motion = self.require_object(part.get("motion"), f"{path}.motion")
            if motion is not None:
                if motion.get("type") not in MOTION_TYPES:
                    self.error(f"{path}.motion.type", f"must be one of {sorted(MOTION_TYPES)}")
                self.require_vector3(motion.get("axis"), f"{path}.motion.axis", nonzero=True)
                self.require_number(motion.get("travel"), f"{path}.motion.travel", positive=True)

    def validate_light_fixtures(
        self,
        fixtures: list[dict[str, Any]],
        module_by_id: dict[str, dict[str, Any]],
    ) -> None:
        if len(fixtures) > MAX_INTERIOR_LIGHT_FIXTURES:
            self.error(
                "$.light_fixtures",
                f"must contain at most {MAX_INTERIOR_LIGHT_FIXTURES} fixtures")
        group_ids: set[str] = set()
        circuit_ids: set[str] = set()
        for index, fixture in enumerate(fixtures):
            path = f"$.light_fixtures[{index}]"
            module_id = self.require_id(fixture.get("module"), f"{path}.module")
            module = module_by_id.get(module_id or "")
            if module_id is not None and module is None:
                self.error(f"{path}.module", f"unknown module '{module_id}'")
            elif module is not None and module.get("role") != "interior":
                self.error(f"{path}.module", "interior lights require an interior module")

            light_type = fixture.get("type")
            if light_type not in LIGHT_TYPES:
                self.error(f"{path}.type", f"must be one of {sorted(LIGHT_TYPES)}")
            self.require_vector3(fixture.get("position_m"), f"{path}.position_m")
            direction = self.require_vector3(
                fixture.get("direction"), f"{path}.direction", nonzero=True)
            if direction is not None:
                length = math.sqrt(sum(value * value for value in direction))
                if abs(length - 1.0) > 1.0e-4:
                    self.error(f"{path}.direction", "must be normalized")

            temperature = self.require_number(
                fixture.get("color_temperature_k"),
                f"{path}.color_temperature_k",
                positive=True)
            if (temperature is not None and
                    not MIN_COLOR_TEMPERATURE_K <= temperature <= MAX_COLOR_TEMPERATURE_K):
                self.error(
                    f"{path}.color_temperature_k",
                    f"must be in [{MIN_COLOR_TEMPERATURE_K}, {MAX_COLOR_TEMPERATURE_K}]")

            intensity = self.require_number(
                fixture.get("intensity_lm_or_cd"),
                f"{path}.intensity_lm_or_cd",
                positive=True)
            if intensity is not None and intensity > MAX_LIGHT_INTENSITY:
                self.error(
                    f"{path}.intensity_lm_or_cd",
                    f"must not exceed {MAX_LIGHT_INTENSITY}")

            range_m = self.require_number(
                fixture.get("range_m"), f"{path}.range_m", positive=True)
            if range_m is not None and range_m > MAX_LIGHT_RANGE_M:
                self.error(
                    f"{path}.range_m",
                    f"must not exceed {MAX_LIGHT_RANGE_M}")

            inner = self.require_number(
                fixture.get("inner_cone_degrees"),
                f"{path}.inner_cone_degrees")
            outer = self.require_number(
                fixture.get("outer_cone_degrees"),
                f"{path}.outer_cone_degrees")
            if inner is not None and outer is not None:
                if light_type == "point":
                    if inner != 180.0 or outer != 180.0:
                        self.error(path, "point lights require 180-degree cone values")
                elif light_type == "spot" and not (0.0 <= inner < outer <= 89.9):
                    self.error(
                        path,
                        "spot cones must satisfy 0 <= inner < outer <= 89.9 degrees")

            fixture_id = fixture.get("id")
            if (isinstance(fixture_id, str) and
                    len(fixture_id.encode("utf-8")) > MAX_INTERIOR_LIGHT_ID_BYTES):
                self.error(
                    f"{path}.id",
                    f"must be at most {MAX_INTERIOR_LIGHT_ID_BYTES} bytes")
            group_id = self.require_id(fixture.get("group"), f"{path}.group")
            circuit_id = self.require_id(
                fixture.get("circuit"), f"{path}.circuit")
            if group_id is not None:
                group_ids.add(group_id)
                if len(group_id.encode("utf-8")) > MAX_INTERIOR_LIGHT_ID_BYTES:
                    self.error(
                        f"{path}.group",
                        f"must be at most {MAX_INTERIOR_LIGHT_ID_BYTES} bytes")
            if circuit_id is not None:
                circuit_ids.add(circuit_id)
                if len(circuit_id.encode("utf-8")) > MAX_INTERIOR_LIGHT_ID_BYTES:
                    self.error(
                        f"{path}.circuit",
                        f"must be at most {MAX_INTERIOR_LIGHT_ID_BYTES} bytes")
            importance = self.require_number(
                fixture.get("importance"), f"{path}.importance")
            if importance is not None and not 0.0 <= importance <= 1.0:
                self.error(f"{path}.importance", "must be in [0, 1]")
            if fixture.get("shadow_policy") not in LIGHT_SHADOW_POLICIES:
                self.error(
                    f"{path}.shadow_policy",
                    f"must be one of {sorted(LIGHT_SHADOW_POLICIES)}")

            emergency_behavior = fixture.get("emergency_behavior")
            if emergency_behavior not in LIGHT_EMERGENCY_BEHAVIORS:
                self.error(
                    f"{path}.emergency_behavior",
                    f"must be one of {sorted(LIGHT_EMERGENCY_BEHAVIORS)}")
            emergency_temperature = self.require_number(
                fixture.get("emergency_color_temperature_k"),
                f"{path}.emergency_color_temperature_k",
                positive=True)
            if (emergency_temperature is not None and
                    not MIN_COLOR_TEMPERATURE_K <= emergency_temperature <=
                    MAX_COLOR_TEMPERATURE_K):
                self.error(
                    f"{path}.emergency_color_temperature_k",
                    f"must be in [{MIN_COLOR_TEMPERATURE_K}, {MAX_COLOR_TEMPERATURE_K}]")
            emergency_scale = self.require_number(
                fixture.get("emergency_intensity_scale"),
                f"{path}.emergency_intensity_scale")
            if (emergency_scale is not None and
                    not 0.0 <= emergency_scale <= MAX_EMERGENCY_INTENSITY_SCALE):
                self.error(
                    f"{path}.emergency_intensity_scale",
                    f"must be in [0, {MAX_EMERGENCY_INTENSITY_SCALE}]")
            if (emergency_behavior in {"emergency_only", "override"} and
                    emergency_scale == 0.0):
                self.error(
                    f"{path}.emergency_intensity_scale",
                    "active emergency behaviors require a positive emergency scale")
        if len(group_ids) > MAX_INTERIOR_LIGHT_GROUPS:
            self.error(
                "$.light_fixtures",
                f"must reference at most {MAX_INTERIOR_LIGHT_GROUPS} groups")
        if len(circuit_ids) > MAX_INTERIOR_LIGHT_CIRCUITS:
            self.error(
                "$.light_fixtures",
                f"must reference at most {MAX_INTERIOR_LIGHT_CIRCUITS} circuits")

    def validate_navigation(
        self,
        value: Any,
        zone_by_id: dict[str, dict[str, Any]],
        portals: list[dict[str, Any]],
    ) -> None:
        navigation = self.require_object(value, "$.navigation")
        if navigation is None:
            return
        entry = self.require_id(navigation.get("entry_zone"), "$.navigation.entry_zone")
        if entry is not None and entry not in zone_by_id:
            self.error("$.navigation.entry_zone", f"unknown zone '{entry}'")
        required = self.require_list(
            navigation.get("required_reachable_zones"),
            "$.navigation.required_reachable_zones") or []
        if not all(isinstance(zone, str) for zone in required):
            self.error("$.navigation.required_reachable_zones", "must contain zone ids")
            return
        if len(required) != len(set(required)):
            self.error("$.navigation.required_reachable_zones", "must not contain duplicates")
        unknown = set(required) - set(zone_by_id)
        if unknown:
            self.error(
                "$.navigation.required_reachable_zones",
                f"contains unknown zones {sorted(unknown)}")
        missing = set(zone_by_id) - set(required)
        if missing:
            self.error(
                "$.navigation.required_reachable_zones",
                f"must include every interior zone; missing {sorted(missing)}")

        graph: dict[str, set[str]] = defaultdict(set)
        for portal in portals:
            a = portal.get("a")
            b = portal.get("b")
            if isinstance(a, str) and isinstance(b, str):
                graph[a].add(b)
                graph[b].add(a)
        reached = {"outside"}
        queue: deque[str] = deque(["outside"])
        while queue:
            current = queue.popleft()
            for neighbor in graph[current]:
                if neighbor not in reached:
                    reached.add(neighbor)
                    queue.append(neighbor)
        disconnected = set(zone_by_id) - reached
        if disconnected:
            self.error(
                "$.portals",
                f"interior zones are not reachable from outside: {sorted(disconnected)}")
        if entry is not None and entry not in graph["outside"]:
            self.error(
                "$.navigation.entry_zone",
                "must be directly connected to outside by an entry portal")

    def validate_interior_coverage(
        self,
        modules: list[dict[str, Any]],
        zones: list[dict[str, Any]],
        portals: list[dict[str, Any]],
    ) -> None:
        zoned_modules = {zone.get("module") for zone in zones}
        missing = {
            module.get("id") for module in modules
            if module.get("role") == "interior" and module.get("id") not in zoned_modules
        }
        if missing:
            self.error("$.zones", f"interior modules lack gameplay zones: {sorted(missing)}")
        if not any(
            portal.get("a") == "outside" or portal.get("b") == "outside"
            for portal in portals
        ):
            self.error("$.portals", "boardable assets require a portal connected to outside")


def validate_document(document: Any) -> list[str]:
    return ManifestValidator(document).validate()


def validate_file(path: Path) -> list[str]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return [f"$: file not found: {path}"]
    except (OSError, UnicodeError) as exc:
        return [f"$: could not read {path}: {exc}"]
    except json.JSONDecodeError as exc:
        return [f"$: invalid JSON at line {exc.lineno}, column {exc.colno}: {exc.msg}"]
    return validate_document(document)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Validate production ship/structure assembly manifests.")
    parser.add_argument("manifests", nargs="+", type=Path)
    args = parser.parse_args(argv)

    failed = False
    for path in args.manifests:
        errors = validate_file(path)
        if errors:
            failed = True
            print(f"FAIL {path}", file=sys.stderr)
            for error in errors:
                print(f"  {error}", file=sys.stderr)
        else:
            print(f"PASS {path}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
