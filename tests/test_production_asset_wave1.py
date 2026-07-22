#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLAN_PATH = ROOT / "assets" / "design" / "production_wave1" / "wave1_plan.json"
MESHY_PATH = ROOT / "tools" / "meshy" / "meshy_client.py"
SPEC = importlib.util.spec_from_file_location("meshy_client", MESHY_PATH)
assert SPEC is not None and SPEC.loader is not None
MESHY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MESHY)


class ProductionAssetWaveOneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.plan = json.loads(PLAN_PATH.read_text(encoding="utf-8"))

    def test_request_inventory_and_budget_are_exact(self) -> None:
        requests = self.plan["requests"]
        concepts = [request for request in requests if request["phase"] == "concept"]
        previews = [request for request in requests if request["phase"] == "preview"]

        self.assertEqual(len(concepts), 5)
        self.assertEqual(len(previews), 11)
        self.assertEqual(sum(request["expected_credits"] for request in concepts), 45)
        self.assertEqual(sum(request["expected_credits"] for request in previews), 220)

        policy = self.plan["credit_policy"]
        self.assertEqual(policy["planned_concept_credits"], 45)
        self.assertEqual(policy["planned_preview_credits"], 220)
        self.assertEqual(
            policy["planned_concept_credits"]
            + policy["planned_preview_credits"]
            + policy["conditional_multiview_credits"]
            + policy["conditional_refine_credits"]
            + policy["contingency_credits"],
            policy["wave_ceiling"],
        )
        self.assertLess(policy["wave_ceiling"], policy["verified_balance_at_claim"])

    def test_request_identity_and_prompts_are_production_safe(self) -> None:
        requests = self.plan["requests"]
        request_ids = [request["request_id"] for request in requests]
        names = [request["name"] for request in requests]
        self.assertEqual(len(request_ids), len(set(request_ids)))
        self.assertEqual(len(names), len(set(names)))

        banned = re.compile(r"\b(aegis|anvil|drake|origin|roberts space|star citizen)\b", re.I)
        for request in requests:
            with self.subTest(request=request["request_id"]):
                self.assertRegex(request["name"], MESHY.SLUG_PATTERN)
                self.assertRegex(request["asset_id"], MESHY.SLUG_PATTERN)
                self.assertRegex(request["module_id"], MESHY.SLUG_PATTERN)
                self.assertLessEqual(len(request["prompt"]), MESHY.MAX_PROMPT_CHARS)
                self.assertIsNone(banned.search(request["prompt"]))
                if request["phase"] == "preview":
                    self.assertLessEqual(
                        len(request["texture_prompt"]), MESHY.MAX_PROMPT_CHARS
                    )

    def test_ship_proportions_and_interior_clearances_are_locked(self) -> None:
        designs = self.plan["design_authority"]
        ship = designs["ship.longreach_expedition_cutter_mk1"]
        envelope = ship["envelope"]
        proportions = ship["proportion_contract"]
        interior = ship["interior_contract"]

        self.assertGreater(envelope["length_z"], envelope["width_x"])
        self.assertGreater(envelope["width_x"], envelope["height_y"])
        self.assertGreaterEqual(
            envelope["length_z"] / envelope["width_x"],
            proportions["minimum_length_to_width"],
        )
        height_ratio = envelope["height_y"] / envelope["width_x"]
        self.assertGreaterEqual(height_ratio, proportions["allowed_height_to_width"][0])
        self.assertLessEqual(height_ratio, proportions["allowed_height_to_width"][1])
        self.assertGreaterEqual(interior["minimum_standing_clearance"], 2.1)
        self.assertGreaterEqual(interior["minimum_door_clear_width"], 0.95)
        self.assertEqual(len(interior["required_zones"]), len(set(interior["required_zones"])))

    def test_station_and_suit_preserve_human_factors(self) -> None:
        designs = self.plan["design_authority"]
        station = designs["station.meridian_exchange_mk1"]["interior_contract"]
        suit = designs["equipment.argent_eva_mk1"]

        self.assertGreaterEqual(station["nominal_clear_height"], 2.4)
        self.assertGreaterEqual(station["minimum_door_clear_width"], 1.05)
        self.assertGreaterEqual(station["main_corridor_clear_width"], 1.5)
        self.assertEqual(len(suit["required_modules"]), len(set(suit["required_modules"])))
        self.assertIn("clear neck rotation", suit["mobility_contract"])
        self.assertIn("hip crouch clearance", suit["mobility_contract"])

    def test_plan_matches_current_meshy_request_and_credit_contract(self) -> None:
        for request in self.plan["requests"]:
            with self.subTest(request=request["request_id"]):
                if request["phase"] == "concept":
                    reconstruction = request["reconstruction"]
                    params = MESHY.build_concept_params(
                        request["prompt"],
                        image_model=request["image_model"],
                        pose_mode=request["pose_mode"],
                        asset_id=request["asset_id"],
                        module_id=request["module_id"],
                    )
                    body = MESHY.build_concept_request(params)
                    self.assertTrue(body["generate_multi_view"])
                    self.assertEqual(
                        MESHY.estimated_concept_credits(request["image_model"]),
                        request["expected_credits"],
                    )
                    self.assertTrue(reconstruction["remesh"])
                    self.assertTrue(reconstruction["hd_texture"])
                    self.assertGreaterEqual(reconstruction["polycount"], 100000)
                    self.assertLessEqual(reconstruction["polycount"], 300000)
                    self.assertLessEqual(
                        len(reconstruction["texture_prompt"]), MESHY.MAX_PROMPT_CHARS
                    )
                    self.assertEqual(
                        MESHY.estimated_multiview_credits("meshy-6"),
                        reconstruction["expected_credits"],
                    )
                    continue

                params = MESHY.build_generation_params(
                    request["prompt"],
                    texture_prompt=request["texture_prompt"],
                    ai_model="meshy-6",
                    polycount=request["polycount"],
                    should_remesh=True,
                    decimation_mode=None,
                    enable_pbr=True,
                    hd_texture=True,
                    remove_lighting=True,
                    auto_size=False,
                    origin_at="bottom",
                    asset_id=request["asset_id"],
                    module_id=request["module_id"],
                )
                preview = MESHY.build_preview_request(params)
                refine = MESHY.build_refine_request(params, "preview-task")
                self.assertEqual(preview["target_formats"], ["glb"])
                self.assertEqual(preview["target_polycount"], request["polycount"])
                self.assertTrue(refine["enable_pbr"])
                self.assertTrue(refine["hd_texture"])
                self.assertTrue(refine["remove_lighting"])
                self.assertEqual(
                    MESHY.estimated_credits("meshy-6", preview_only=True),
                    request["expected_credits"],
                )


if __name__ == "__main__":
    unittest.main()
