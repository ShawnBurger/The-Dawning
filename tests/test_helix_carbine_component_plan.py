#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import math
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLAN_PATH = ROOT / "assets/design/helix_carbine_mk1/component_plan.json"
MESHY_PATH = ROOT / "tools/meshy/meshy_client.py"
SPEC = importlib.util.spec_from_file_location("meshy_client", MESHY_PATH)
assert SPEC is not None and SPEC.loader is not None
MESHY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MESHY)


class HelixCarbineComponentPlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.plan = json.loads(PLAN_PATH.read_text(encoding="utf-8"))

    def test_campaign_is_strictly_serial_and_credit_bounded(self) -> None:
        policy = self.plan["generation_policy"]
        credits = self.plan["credit_policy"]
        self.assertTrue(policy["strict_serial_generation"])
        self.assertEqual(policy["asset_families_in_parallel"], 1)
        self.assertEqual(policy["active_components_at_once"], 1)
        self.assertFalse(policy["whole_asset_generation_allowed"])
        self.assertFalse(policy["generated_collision_allowed"])
        self.assertTrue(policy["authored_geometry_required_before_retexture"])
        self.assertFalse(policy["text_generated_component_geometry_allowed"])
        self.assertEqual(credits["component_count"], 8)
        self.assertEqual(
            credits["planned_component_generation_credits"],
            credits["component_count"]
            * credits["retexture_credits_each"],
        )
        self.assertEqual(
            credits["accepted_component_credits"]
            + credits["remaining_planned_component_credits"],
            credits["planned_component_generation_credits"],
        )
        self.assertEqual(
            credits["planned_component_generation_credits"]
            + credits["spent_exploration_credits"]
            + credits["contingency_credits"],
            credits["campaign_ceiling"],
        )

    def test_component_inventory_and_interfaces_are_complete(self) -> None:
        components = self.plan["components"]
        expected = {
            "upper_receiver", "lower_receiver", "accelerator_barrel",
            "thermal_handguard", "muzzle_device", "stock", "magazine", "optic",
        }
        self.assertEqual({item["component_id"] for item in components}, expected)
        self.assertEqual([item["order"] for item in components], list(range(1, 9)))

        interfaces = {
            f"{item['component_id']}.{interface['id']}": interface
            for item in components
            for interface in item["interfaces"]
        }
        for interface_id, interface in interfaces.items():
            with self.subTest(interface=interface_id):
                mate = interface["mates_to"]
                self.assertIn(mate, interfaces)
                self.assertEqual(interfaces[mate]["mates_to"], interface_id)
                self.assertAlmostEqual(
                    math.sqrt(sum(value * value for value in interface["normal"])),
                    1.0,
                )

    def test_authored_component_bounds_fit_assembly_envelope(self) -> None:
        assembly = self.plan["assembly_envelope"]
        minimum = assembly["bounds_min"]
        maximum = assembly["bounds_max"]
        size = [maximum[axis] - minimum[axis] for axis in range(3)]
        self.assertEqual(
            size,
            [assembly["width_x"], assembly["height_y"], assembly["length_z"]],
        )

        for component in self.plan["components"]:
            for axis in range(3):
                half = component["envelope"][axis] * 0.5
                low = component["assembly_position"][axis] - half
                high = component["assembly_position"][axis] + half
                with self.subTest(component=component["component_id"], axis=axis):
                    self.assertGreaterEqual(low + 1.0e-9, minimum[axis])
                    self.assertLessEqual(high - 1.0e-9, maximum[axis])

    def test_component_requests_are_original_and_api_valid(self) -> None:
        banned = re.compile(r"\b(aegis|anvil|drake|roberts space|star citizen)\b", re.I)
        names = []
        for component in self.plan["components"]:
            concept = component["concept"]
            reconstruction = component["reconstruction"]
            names.append(concept["name"])
            with self.subTest(component=component["component_id"]):
                self.assertRegex(concept["name"], MESHY.SLUG_PATTERN)
                self.assertRegex(component["module_id"], MESHY.SLUG_PATTERN)
                self.assertLessEqual(len(concept["prompt"]), MESHY.MAX_PROMPT_CHARS)
                self.assertLessEqual(
                    len(reconstruction["texture_prompt"]), MESHY.MAX_PROMPT_CHARS
                )
                self.assertIsNone(banned.search(concept["prompt"]))
                self.assertEqual(
                    MESHY.estimated_concept_credits(concept["image_model"]), 9
                )
                self.assertEqual(MESHY.estimated_multiview_credits("meshy-6"), 30)
                self.assertGreaterEqual(reconstruction["polycount"], 100000)
                self.assertLessEqual(reconstruction["polycount"], 200000)
        self.assertEqual(len(names), len(set(names)))

    def test_active_upper_receiver_uses_authored_geometry_for_retexture(self) -> None:
        component = self.plan["components"][0]
        generation = component["generation"]
        self.assertEqual(component["component_id"], "upper_receiver")
        self.assertEqual(
            component["status"], "source_promoted"
        )
        self.assertEqual(generation["method"], "blender_authored_meshy_retexture")
        self.assertEqual(
            generation["builder"],
            "tools/blender/build_helix_carbine_component.py",
        )
        input_model = ROOT / generation["input_model"]
        self.assertTrue(input_model.is_file())
        self.assertEqual(input_model.suffix, ".glb")
        for key in ("authored_source", "authored_report", "authored_blend"):
            self.assertTrue((ROOT / component["evidence"][key]).is_file())
        for key in (
            "accepted_vendor_source", "production_source",
            "production_report", "production_blend",
        ):
            self.assertTrue((ROOT / component["evidence"][key]).is_file())
        self.assertLessEqual(
            len(generation["texture_prompt"]), MESHY.MAX_PROMPT_CHARS
        )
        self.assertEqual(generation["expected_retexture_credits"], 10)
        self.assertTrue(generation["enable_pbr"])
        self.assertTrue(generation["hd_texture"])
        self.assertTrue(generation["remove_lighting"])
        self.assertFalse(generation["enable_original_uv"])


if __name__ == "__main__":
    unittest.main()
