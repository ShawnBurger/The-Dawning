#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import base64
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


def write_concept_manifest(root: Path) -> Path:
    params = MESHY.build_concept_params(
        "realistic modular frontier courier spacecraft",
        image_model="gpt-image-2",
        pose_mode=None,
        asset_id="ship.frontier.courier_mk1",
        module_id="exterior_hull",
    )
    images = []
    hashes = {}
    for index in range(3):
        image = root / f"view_{index}.png"
        image.write_bytes(f"view-{index}".encode("ascii"))
        images.append(image.name)
        hashes[image.name] = MESHY.file_sha256(image)
    manifest = {
        "schema_version": 2,
        "provider": "Meshy",
        "endpoint": "/openapi/v1/text-to-image",
        "request_hash": MESHY.request_hash(params),
        "stage": "concept",
        "parameters": params,
        "tasks": {
            "concept": {
                "id": "concept-task-id",
                "status": "SUCCEEDED",
                "consumed_credits": 9,
            }
        },
        "files": {"images": images, "image_sha256": hashes},
        "art_review": {"status": "approved_for_reconstruction"},
        "consumed_credits": 9,
    }
    path = root / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


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

    def test_concept_request_is_three_view_content_addressed_and_cost_bounded(self) -> None:
        params = MESHY.build_concept_params(
            "realistic modular frontier courier spacecraft",
            image_model="gpt-image-2",
            pose_mode=None,
            asset_id="ship.frontier.courier_mk1",
            module_id="exterior_hull",
        )
        request = MESHY.build_concept_request(params)
        self.assertTrue(request["generate_multi_view"])
        self.assertNotIn("aspect_ratio", request)
        self.assertNotIn("pose_mode", request)
        self.assertEqual(MESHY.estimated_concept_credits("gpt-image-2"), 9)
        self.assertEqual(MESHY.request_hash(params), MESHY.request_hash(dict(params)))

    def test_multiview_request_preserves_approved_concept_and_pbr_master(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = write_concept_manifest(Path(temporary))
            concept = MESHY.load_concept_manifest(path)
            params = MESHY.build_multiview_params(
                concept,
                ai_model="meshy-6",
                polycount=120000,
                should_remesh=True,
                decimation_mode=None,
                enable_pbr=True,
                hd_texture=True,
                remove_lighting=True,
                texture_prompt="de-lit aerospace alloy",
                auto_size=False,
                origin_at="bottom",
            )
            request = MESHY.build_multiview_request(params)
            self.assertEqual(request["input_task_id"], "concept-task-id")
            self.assertFalse(request["image_enhancement"])
            self.assertTrue(request["enable_pbr"])
            self.assertTrue(request["remove_lighting"])
            self.assertTrue(request["save_pre_remeshed_model"])
            self.assertEqual(request["target_polycount"], 120000)
            self.assertEqual(request["target_formats"], ["glb"])
            self.assertEqual(MESHY.estimated_multiview_credits("meshy-6"), 30)

    def test_multiview_can_use_exact_hashed_local_images_without_exposing_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = write_concept_manifest(root)
            concept = MESHY.load_concept_manifest(path)
            local_images = []
            for index in range(3):
                image = root / f"clean_{index}.png"
                image.write_bytes(f"clean-view-{index}".encode("ascii"))
                local_images.append(image)
            descriptions = MESHY.describe_input_images(local_images)
            params = MESHY.build_multiview_params(
                concept,
                input_images=descriptions,
                ai_model="meshy-6",
                polycount=120000,
                should_remesh=True,
                decimation_mode=None,
                enable_pbr=True,
                hd_texture=True,
                remove_lighting=True,
                texture_prompt="de-lit aerospace alloy",
                auto_size=False,
                origin_at="bottom",
            )
            data_uris = MESHY.input_image_data_uris(local_images)
            request = MESHY.build_multiview_request(params, data_uris)
            self.assertNotIn("input_task_id", request)
            self.assertEqual(len(request["image_urls"]), 3)
            header, encoded = request["image_urls"][0].split(",", 1)
            self.assertEqual(header, "data:image/png;base64")
            self.assertEqual(base64.b64decode(encoded), b"clean-view-0")
            redacted = MESHY.redact_multiview_request(request, params)
            self.assertNotIn(encoded, json.dumps(redacted))
            self.assertIn(descriptions[0]["sha256"], redacted["image_urls"][0])

    def test_unapproved_concept_is_rejected_before_reconstruction(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = write_concept_manifest(root)
            manifest = json.loads(path.read_text(encoding="utf-8"))
            manifest["art_review"]["status"] = "rejected_for_production"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(MESHY.MeshyError, "approved_for_reconstruction"):
                MESHY.load_concept_manifest(path)

    def test_concept_and_multiview_dry_runs_need_no_key_and_write_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            concept_out = root / "concepts"
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                asset_dir = MESHY.generate_concept(
                    None,
                    concept_out,
                    "realistic modular frontier courier spacecraft",
                    name="frontier_courier_concept",
                    image_model="gpt-image-2",
                    pose_mode=None,
                    asset_id="ship.frontier.courier_mk1",
                    module_id="exterior_hull",
                    dry_run=True,
                    max_credits=9,
                    force=False,
                )
            plan = json.loads(output.getvalue())
            self.assertEqual(plan["projected_credits"], 9)
            self.assertFalse(plan["credential_required"])
            self.assertFalse(asset_dir.exists())

            concept_manifest = write_concept_manifest(root)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                model_dir = MESHY.generate_multiview(
                    None,
                    root / "models",
                    concept_manifest,
                    name="frontier_courier_hull",
                    ai_model="meshy-6",
                    polycount=120000,
                    should_remesh=True,
                    decimation_mode=None,
                    enable_pbr=True,
                    hd_texture=True,
                    remove_lighting=True,
                    texture_prompt="de-lit aerospace alloy",
                    auto_size=False,
                    origin_at="bottom",
                    dry_run=True,
                    max_credits=30,
                    force=False,
                )
            plan = json.loads(output.getvalue())
            self.assertEqual(plan["projected_credits"], 30)
            self.assertFalse(plan["credential_required"])
            self.assertFalse(model_dir.exists())

    def test_concept_manifest_rejects_missing_or_tampered_images(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = write_concept_manifest(root)
            (root / "view_1.png").write_bytes(b"tampered")
            with self.assertRaisesRegex(MESHY.MeshyError, "hash mismatch"):
                MESHY.load_concept_manifest(path)

    def test_concept_manifest_rejects_non_object_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "manifest.json"
            path.write_text("[]", encoding="utf-8")
            with self.assertRaisesRegex(MESHY.MeshyError, "root must be an object"):
                MESHY.load_concept_manifest(path)

    def test_concept_cache_requires_matching_request_and_image_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output_root = Path(temporary)
            params = MESHY.build_concept_params(
                "realistic modular frontier courier spacecraft",
                image_model="gpt-image-2",
                pose_mode=None,
                asset_id="ship.frontier.courier_mk1",
                module_id="exterior_hull",
            )
            asset_dir = output_root / (
                f"frontier_courier_concept_{MESHY.cache_key(params)}")
            asset_dir.mkdir()
            images = []
            hashes = {}
            for index in range(3):
                image = asset_dir / f"view_{index}.png"
                image.write_bytes(f"view-{index}".encode("ascii"))
                images.append(image.name)
                hashes[image.name] = MESHY.file_sha256(image)
            hashes["view_1.png"] = "0" * 64
            (asset_dir / "manifest.json").write_text(json.dumps({
                "request_hash": MESHY.request_hash(params),
                "parameters": params,
                "tasks": {"concept": {"id": "task", "status": "SUCCEEDED"}},
                "files": {"images": images, "image_sha256": hashes},
            }), encoding="utf-8")

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                MESHY.generate_concept(
                    None,
                    output_root,
                    params["prompt"],
                    name="frontier_courier_concept",
                    image_model="gpt-image-2",
                    pose_mode=None,
                    asset_id="ship.frontier.courier_mk1",
                    module_id="exterior_hull",
                    dry_run=True,
                    max_credits=9,
                    force=False,
                )
            plan = json.loads(output.getvalue())
            self.assertEqual(plan["cache_state"], "task")
            self.assertEqual(plan["projected_credits"], 0)

            manifest = json.loads((asset_dir / "manifest.json").read_text(encoding="utf-8"))
            manifest["request_hash"] = "f" * 64
            (asset_dir / "manifest.json").write_text(
                json.dumps(manifest), encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                MESHY.generate_concept(
                    None,
                    output_root,
                    params["prompt"],
                    name="frontier_courier_concept",
                    image_model="gpt-image-2",
                    pose_mode=None,
                    asset_id="ship.frontier.courier_mk1",
                    module_id="exterior_hull",
                    dry_run=True,
                    max_credits=9,
                    force=False,
                )
            plan = json.loads(output.getvalue())
            self.assertEqual(plan["cache_state"], "miss")
            self.assertEqual(plan["projected_credits"], 9)

    def test_multiview_cache_requires_authenticated_model(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            concept_path = write_concept_manifest(root)
            concept = MESHY.load_concept_manifest(concept_path)
            params = MESHY.build_multiview_params(
                concept,
                ai_model="meshy-6",
                polycount=120000,
                should_remesh=True,
                decimation_mode=None,
                enable_pbr=True,
                hd_texture=True,
                remove_lighting=True,
                texture_prompt="de-lit aerospace alloy",
                auto_size=False,
                origin_at="bottom",
            )
            output_root = root / "models"
            asset_dir = output_root / f"courier_{MESHY.cache_key(params)}"
            asset_dir.mkdir(parents=True)
            model = asset_dir / "model.glb"
            model.write_bytes(b"tampered-model")
            (asset_dir / "manifest.json").write_text(json.dumps({
                "request_hash": MESHY.request_hash(params),
                "parameters": params,
                "tasks": {"generation": {"id": "task", "status": "SUCCEEDED"}},
                "files": {"model": model.name, "model_sha256": "0" * 64},
            }), encoding="utf-8")

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                MESHY.generate_multiview(
                    None,
                    output_root,
                    concept_path,
                    name="courier",
                    ai_model="meshy-6",
                    polycount=120000,
                    should_remesh=True,
                    decimation_mode=None,
                    enable_pbr=True,
                    hd_texture=True,
                    remove_lighting=True,
                    texture_prompt="de-lit aerospace alloy",
                    auto_size=False,
                    origin_at="bottom",
                    dry_run=True,
                    max_credits=30,
                    force=False,
                )
            plan = json.loads(output.getvalue())
            self.assertEqual(plan["cache_state"], "task")
            self.assertEqual(plan["projected_credits"], 0)

    def test_retexture_request_uses_hashed_model_and_redacts_binary_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "accepted.glb"
            source.write_bytes(b"accepted-production-geometry")
            description = MESHY.describe_input_model(source)
            params = MESHY.build_retexture_params(
                description,
                text_style_prompt="de-lit aerospace composite hull panels",
                ai_model="meshy-6",
                enable_original_uv=False,
                enable_pbr=True,
                hd_texture=True,
                remove_lighting=True,
                asset_id="ship.frontier.courier_mk1",
                module_id="exterior_hull",
            )
            uri = MESHY.input_model_data_uri(source)
            request = MESHY.build_retexture_request(params, uri)
            self.assertFalse(request["enable_original_uv"])
            self.assertTrue(request["enable_pbr"])
            self.assertTrue(request["hd_texture"])
            self.assertTrue(request["remove_lighting"])
            self.assertEqual(request["target_formats"], ["glb"])
            header, encoded = request["model_url"].split(",", 1)
            self.assertEqual(header, "data:application/octet-stream;base64")
            self.assertEqual(base64.b64decode(encoded), source.read_bytes())
            redacted = MESHY.redact_retexture_request(request, params)
            self.assertNotIn(encoded, json.dumps(redacted))
            self.assertIn(description["sha256"], redacted["model_url"])

    def test_retexture_dry_run_costs_zero_and_writes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "accepted.glb"
            source.write_bytes(b"accepted-production-geometry")
            output_root = root / "generated"
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                asset_dir = MESHY.generate_retexture(
                    None,
                    output_root,
                    source,
                    name="frontier_courier_hull_retexture",
                    text_style_prompt="de-lit aerospace composite hull panels",
                    ai_model="meshy-6",
                    enable_original_uv=False,
                    enable_pbr=True,
                    hd_texture=True,
                    remove_lighting=True,
                    asset_id="ship.frontier.courier_mk1",
                    module_id="exterior_hull",
                    dry_run=True,
                    max_credits=10,
                    force=False,
                )
            plan = json.loads(output.getvalue())
            self.assertEqual(plan["projected_credits"], 10)
            self.assertFalse(plan["credential_required"])
            self.assertNotIn(
                base64.b64encode(source.read_bytes()).decode("ascii"),
                output.getvalue(),
            )
            self.assertFalse(asset_dir.exists())
            self.assertFalse(output_root.exists())

    def test_text_cache_does_not_trust_unhashed_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output_root = Path(temporary)
            params = parameters()
            asset_dir = output_root / f"pressure_door_{MESHY.cache_key(params)}"
            asset_dir.mkdir()
            preview = asset_dir / "preview.glb"
            model = asset_dir / "model.glb"
            preview.write_bytes(b"preview")
            model.write_bytes(b"model")
            (asset_dir / "manifest.json").write_text(json.dumps({
                "request_hash": MESHY.request_hash(params),
                "parameters": params,
                "tasks": {
                    "preview": {"id": "preview", "status": "SUCCEEDED"},
                    "refine": {"id": "refine", "status": "SUCCEEDED"},
                },
                "files": {
                    "preview": preview.name,
                    "preview_sha256": MESHY.file_sha256(preview),
                    "model": model.name,
                    "model_sha256": "0" * 64,
                },
            }), encoding="utf-8")

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                MESHY.generate(
                    None,
                    output_root,
                    params["prompt"],
                    name="pressure_door",
                    polycount=50000,
                    enable_pbr=True,
                    texture_prompt="de-lit aerospace alloy",
                    ai_model="latest",
                    should_remesh=False,
                    decimation_mode=None,
                    hd_texture=False,
                    remove_lighting=True,
                    auto_size=False,
                    origin_at="bottom",
                    asset_id="ship.reference.fighter",
                    module_id="airlock_module",
                    preview_only=False,
                    dry_run=True,
                    max_credits=40,
                    force=False,
                )
            plan = json.loads(output.getvalue())
            self.assertEqual(plan["cache_state"], "preview")
            self.assertEqual(plan["projected_credits"], 10)

    def test_malformed_cache_shapes_become_a_clean_cache_miss(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output_root = Path(temporary)
            params = parameters()
            asset_dir = output_root / f"pressure_door_{MESHY.cache_key(params)}"
            asset_dir.mkdir()
            (asset_dir / "manifest.json").write_text(json.dumps({
                "request_hash": MESHY.request_hash(params),
                "parameters": params,
                "tasks": [],
                "files": [],
            }), encoding="utf-8")

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                MESHY.generate(
                    None,
                    output_root,
                    params["prompt"],
                    name="pressure_door",
                    polycount=50000,
                    enable_pbr=True,
                    texture_prompt="de-lit aerospace alloy",
                    ai_model="latest",
                    should_remesh=False,
                    decimation_mode=None,
                    hd_texture=False,
                    remove_lighting=True,
                    auto_size=False,
                    origin_at="bottom",
                    asset_id="ship.reference.fighter",
                    module_id="airlock_module",
                    preview_only=False,
                    dry_run=True,
                    max_credits=40,
                    force=False,
                )
            plan = json.loads(output.getvalue())
            self.assertEqual(plan["cache_state"], "miss")
            self.assertEqual(plan["projected_credits"], 30)


if __name__ == "__main__":
    unittest.main()
