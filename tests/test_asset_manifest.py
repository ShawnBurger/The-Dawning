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

    def validate(self, document: dict) -> list[str]:
        return VALIDATOR.validate_document(document)

    def test_reference_ship_satisfies_production_contract(self) -> None:
        self.assertEqual(self.validate(copy.deepcopy(self.reference)), [])

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


if __name__ == "__main__":
    unittest.main()
