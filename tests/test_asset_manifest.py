#!/usr/bin/env python3

from __future__ import annotations

import copy
import importlib.util
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "validate_asset_manifest.py"
SPEC = importlib.util.spec_from_file_location("validate_asset_manifest", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


class AssetManifestContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.reference = json.loads(
            (ROOT / "assets" / "manifests" / "reference_ship.tdasset.json")
            .read_text(encoding="utf-8")
        )
        cls.frontier = json.loads(
            (ROOT / "assets" / "manifests" /
             "frontier_courier_mk1.design.tdasset.json")
            .read_text(encoding="utf-8")
        )

    def validate(self, document: dict) -> list[str]:
        return VALIDATOR.validate_document(document)

    def test_reference_ship_satisfies_production_contract(self) -> None:
        self.assertEqual(self.validate(copy.deepcopy(self.reference)), [])

    def test_frontier_courier_schema_v2_light_contract_is_valid(self) -> None:
        self.assertEqual(self.validate(copy.deepcopy(self.frontier)), [])
        self.assertEqual(len(self.frontier["light_fixtures"]), 12)

    def test_disconnected_interior_is_rejected(self) -> None:
        document = copy.deepcopy(self.reference)
        document["portals"] = document["portals"][:1]
        errors = self.validate(document)
        self.assertTrue(any("not reachable from outside" in error for error in errors), errors)

    def test_missing_pressure_seal_and_nav_link_are_rejected(self) -> None:
        document = copy.deepcopy(self.reference)
        document["portals"][0]["sealable"] = False
        document["portals"][0]["nav_link"] = False
        errors = self.validate(document)
        self.assertTrue(any("sealable" in error for error in errors), errors)
        self.assertTrue(any("nav_link" in error for error in errors), errors)

    def test_missing_collision_and_lods_are_rejected(self) -> None:
        document = copy.deepcopy(self.reference)
        del document["modules"][1]["collision"]
        document["modules"][1]["lods"] = document["modules"][1]["lods"][:1]
        errors = self.validate(document)
        self.assertTrue(any("collision" in error for error in errors), errors)
        self.assertTrue(any("at least LOD0" in error for error in errors), errors)

    def test_moving_door_without_pivot_contract_is_rejected(self) -> None:
        document = copy.deepcopy(self.reference)
        del document["moving_parts"][0]["pivot_m"]
        errors = self.validate(document)
        self.assertTrue(any("pivot_m" in error for error in errors), errors)

    def test_moving_door_requires_addressable_visual_and_module_ownership(self) -> None:
        document = copy.deepcopy(self.reference)
        del document["moving_parts"][0]["visual_source"]
        document["moving_parts"][1]["module"] = "cockpit_module"
        errors = self.validate(document)
        self.assertTrue(any("visual_source" in error for error in errors), errors)
        self.assertTrue(any("moving-part module" in error for error in errors), errors)

    def test_interaction_socket_must_belong_to_its_module(self) -> None:
        document = copy.deepcopy(self.reference)
        document["interactions"][2]["socket"] = "airlock_inner"
        errors = self.validate(document)
        self.assertTrue(any("interaction module" in error for error in errors), errors)

    def test_unknown_module_and_provenance_references_are_rejected(self) -> None:
        document = copy.deepcopy(self.reference)
        document["zones"][0]["module"] = "missing_module"
        document["modules"][0]["provenance_ref"] = "missing_source"
        errors = self.validate(document)
        self.assertTrue(any("unknown module" in error for error in errors), errors)
        self.assertTrue(any("unknown source" in error for error in errors), errors)

    def test_portal_socket_must_belong_to_its_zone_side(self) -> None:
        document = copy.deepcopy(self.reference)
        document["portals"][1]["socket_b"] = "airlock_outer"
        errors = self.validate(document)
        self.assertTrue(any("must match zone 'cockpit'" in error for error in errors), errors)

    def test_schema_v1_cannot_smuggle_light_fixture_records(self) -> None:
        document = copy.deepcopy(self.reference)
        document["light_fixtures"] = copy.deepcopy(
            self.frontier["light_fixtures"][:1]
        )
        errors = self.validate(document)
        self.assertTrue(any("requires schema_version 2" in error for error in errors), errors)

    def test_schema_v2_requires_nonempty_authored_fixtures(self) -> None:
        document = copy.deepcopy(self.frontier)
        document["light_fixtures"] = []
        errors = self.validate(document)
        self.assertTrue(any("must not be empty" in error for error in errors), errors)

    def test_light_fixture_physical_and_ownership_guards(self) -> None:
        mutations = (
            ("module", "exterior_hull", "interior module"),
            ("direction", [0.0, -2.0, 0.0], "normalized"),
            ("color_temperature_k", 500.0, "must be in"),
            ("intensity_lm_or_cd", 0.0, "greater than zero"),
            ("range_m", float("nan"), "finite number"),
            ("importance", 1.1, "must be in [0, 1]"),
            ("shadow_policy", "cinematic", "must be one of"),
            ("emergency_behavior", "flicker", "must be one of"),
        )
        for field, value, expected in mutations:
            with self.subTest(field=field):
                document = copy.deepcopy(self.frontier)
                document["light_fixtures"][0][field] = value
                errors = self.validate(document)
                self.assertTrue(any(expected in error for error in errors), errors)

    def test_light_type_controls_cone_contract(self) -> None:
        point = copy.deepcopy(self.frontier)
        point["light_fixtures"][1]["inner_cone_degrees"] = 45.0
        errors = self.validate(point)
        self.assertTrue(any("point lights require" in error for error in errors), errors)

        spot = copy.deepcopy(self.frontier)
        spot["light_fixtures"][0]["inner_cone_degrees"] = 80.0
        spot["light_fixtures"][0]["outer_cone_degrees"] = 40.0
        errors = self.validate(spot)
        self.assertTrue(any("spot cones must satisfy" in error for error in errors), errors)

    def test_active_emergency_behavior_requires_positive_override_scale(self) -> None:
        document = copy.deepcopy(self.frontier)
        override = next(
            fixture for fixture in document["light_fixtures"]
            if fixture["emergency_behavior"] == "override"
        )
        override["emergency_intensity_scale"] = 0.0
        errors = self.validate(document)
        self.assertTrue(
            any("active emergency behaviors" in error for error in errors),
            errors,
        )

    def test_light_fixture_runtime_budgets_are_enforced_at_authoring(self) -> None:
        document = copy.deepcopy(self.frontier)
        source = document["light_fixtures"][0]
        document["light_fixtures"] = []
        for index in range(VALIDATOR.MAX_INTERIOR_LIGHT_GROUPS + 1):
            fixture = copy.deepcopy(source)
            fixture["id"] = f"bulk_{index}"
            fixture["group"] = f"group_{index}"
            document["light_fixtures"].append(fixture)
        errors = self.validate(document)
        self.assertTrue(any("at most 1024 groups" in error for error in errors), errors)

        document = copy.deepcopy(self.frontier)
        document["light_fixtures"][0]["id"] = "a" * 257
        errors = self.validate(document)
        self.assertTrue(any("at most 256 bytes" in error for error in errors), errors)


if __name__ == "__main__":
    unittest.main()
