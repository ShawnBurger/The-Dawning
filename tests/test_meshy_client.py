#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "meshy" / "meshy_client.py"
SPEC = importlib.util.spec_from_file_location("meshy_client", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MESHY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MESHY)


def parameters(**overrides):
    values = dict(
        prompt="modular spaceship pressure door",
        texture_prompt="de-lit aerospace alloy",
        ai_model="latest",
        polycount=50000,
        should_remesh=False,
        decimation_mode=None,
        enable_pbr=True,
        hd_texture=False,
        remove_lighting=True,
        auto_size=False,
        origin_at="bottom",
        asset_id="ship.reference.fighter",
        module_id="airlock_module",
    )
    values.update(overrides)
    return MESHY.build_generation_params(**values)


class MeshyClientContractTests(unittest.TestCase):
    def test_latest_request_preserves_master_mesh_and_removes_baked_lighting(self) -> None:
        params = parameters()
        preview = MESHY.build_preview_request(params)
        refine = MESHY.build_refine_request(params, "preview-id")
        self.assertEqual(preview["ai_model"], "latest")
        self.assertFalse(preview["should_remesh"])
        self.assertNotIn("target_polycount", preview)
        self.assertTrue(refine["enable_pbr"])
        self.assertTrue(refine["remove_lighting"])
        self.assertEqual(refine["target_formats"], ["glb"])

    def test_remesh_parameters_are_explicit_and_content_addressed(self) -> None:
        first = parameters(should_remesh=True, polycount=64000)
        second = parameters(should_remesh=True, polycount=64000)
        changed = parameters(should_remesh=True, polycount=32000)
        self.assertEqual(MESHY.request_hash(first), MESHY.request_hash(second))
        self.assertNotEqual(MESHY.request_hash(first), MESHY.request_hash(changed))
        self.assertEqual(MESHY.build_preview_request(first)["target_polycount"], 64000)

    def test_projected_credit_cap_matches_current_two_stage_cost(self) -> None:
        self.assertEqual(MESHY.estimated_credits("latest", True), 20)
        self.assertEqual(MESHY.estimated_credits("meshy-6", False), 30)
        self.assertEqual(MESHY.estimated_credits("meshy-5", False), 15)
        self.assertEqual(MESHY.estimated_credits("latest", False, True), 10)
        self.assertEqual(MESHY.estimated_credits("latest", True, True), 0)

    def test_dry_run_needs_no_key_and_writes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                asset_dir = MESHY.generate(
                    None, Path(temporary), "modular spaceship pressure door",
                    name="pressure_door", polycount=50000, enable_pbr=True,
                    texture_prompt="de-lit aerospace alloy", ai_model="latest",
                    should_remesh=False, decimation_mode=None, hd_texture=False,
                    remove_lighting=True, auto_size=False, origin_at="bottom",
                    asset_id="ship.reference.fighter", module_id="airlock_module",
                    preview_only=True, dry_run=True, max_credits=40, force=False)
            plan = json.loads(output.getvalue())
            self.assertFalse(plan["credential_required"])
            self.assertEqual(plan["projected_credits"], 20)
            self.assertFalse(asset_dir.exists())

    def test_partial_assembly_linkage_is_rejected_before_network_access(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(MESHY.MeshyError):
                MESHY.generate(
                    None, Path(temporary), "door", name="door", polycount=30000,
                    enable_pbr=True, texture_prompt=None, ai_model="latest",
                    should_remesh=False, decimation_mode=None, hd_texture=False,
                    remove_lighting=True, auto_size=False, origin_at="bottom",
                    asset_id="ship.reference.fighter", module_id=None,
                    preview_only=True, dry_run=True, max_credits=40, force=False)


if __name__ == "__main__":
    unittest.main()
