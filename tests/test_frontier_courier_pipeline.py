import importlib.util
import hashlib
import json
import math
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DIMENSIONS = ROOT / "assets/design/frontier_courier_mk1/dimensions.json"
MANIFEST = ROOT / "assets/manifests/frontier_courier_mk1.design.tdasset.json"
INTERIOR_REPORT = (
    ROOT / "assets/source/frontier_courier_mk1/frontier_courier_interior_lods.report.json"
)
CONTENT = ROOT / "assets/runtime/frontier_courier_mk1.tdcontent"

ACCEPTED_COCKPIT_FIXTURES = {
    "pilot_seat": {
        "directory": "frontier_courier_cockpit_pilot_seat_b1f6b68b36fb14e1",
        "request_hash": "b1f6b68b36fb14e15154daaa89012c86538935e5a70c43ce71710b9d638951c1",
        "preview_task": "019f8944-4292-7694-817b-83e7c72fccf8",
        "refine_task": "019f894c-854f-7475-97a3-2d2b200e3fc1",
        "source": "frontier_courier_pilot_seat_source.glb",
        "source_sha256": "c73501d4e678b15386da7c27261baa2237677835effc99b0478df927a94836f6",
        "instance_count": 1,
    },
    "flight_station": {
        "directory": "frontier_courier_cockpit_flight_station_0eb8bdcb89c3dd37",
        "request_hash": "0eb8bdcb89c3dd3753744b9b44ae6e15b65ed0b30771d8885604e67a50d41497",
        "preview_task": "019f8968-dd30-7f17-90c8-e537a9bf1f92",
        "refine_task": "019f8972-ccba-71b7-8138-909d3a4d84ed",
        "source": "frontier_courier_flight_station_source.glb",
        "source_sha256": "a7d8ef497d56a6ace415433d2b163f8e0f3308306b53c38a1163225ff4b166cc",
        "instance_count": 1,
    },
    "cockpit_wall_panel": {
        "directory": "frontier_courier_cockpit_sidewall_slab_cf8dd773d5d65659",
        "request_hash": "cf8dd773d5d656592bd80942c1ed6554d78565af2387b53a45a6983eb8230682",
        "preview_task": "019f89a3-1123-7ed8-8ede-f97dd9c8b030",
        "refine_task": "019f89a6-9870-74dc-8b9b-e174f7c74c81",
        "source": "frontier_courier_cockpit_wall_panel_source.glb",
        "source_sha256": "a7d7e5febba3c72be24c8a183ca534e99a22fa9aa19b7659a90a3c429af78ae5",
        "instance_count": 29,
    },
    "cockpit_deck_panel": {
        "directory": "frontier_courier_cockpit_deck_bay_08c94c3825dcd328",
        "request_hash": "08c94c3825dcd328455fd9577d3c3a085821d53bf5748d30f3ff8a4513b62dd9",
        "preview_task": "019f899e-97be-7c80-b06f-07048ff43a81",
        "refine_task": "019f89a6-9871-7da9-8b5e-07b74271b9f5",
        "source": "frontier_courier_cockpit_deck_panel_source.glb",
        "source_sha256": "7ace54dda437f6dbf8019a98a5362aeedb8516a1f516fbb2831551bdf4175fd5",
        "instance_count": 16,
    },
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_tool(name: str, relative_path: str):
    path = ROOT / relative_path
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


COLLISION_TOOL = load_tool(
    "generate_frontier_courier_collision",
    "tools/generate_frontier_courier_collision.py",
)
CONTENT_TOOL = load_tool(
    "generate_frontier_courier_content",
    "tools/generate_frontier_courier_content.py",
)


class FrontierCourierPipelineTests(unittest.TestCase):
    def test_longitudinal_proportion_contract(self):
        document = json.loads(DIMENSIONS.read_text(encoding="utf-8"))
        envelope = document["envelope_m"]
        length = envelope["length_z"]
        width = envelope["width_x"]
        height = envelope["height_y"]
        contract = document["proportion_contract"]

        self.assertGreater(length, width)
        self.assertGreater(width, height)
        self.assertGreaterEqual(length / width, contract["minimum_length_to_width"])
        self.assertGreaterEqual(height / width, contract["allowed_height_to_width"][0])
        self.assertLessEqual(height / width, contract["allowed_height_to_width"][1])

    def test_required_route_includes_unpressurized_vestibule(self):
        dimensions = json.loads(DIMENSIONS.read_text(encoding="utf-8"))
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(
            dimensions["required_zone_path"][:4],
            ["outside", "boarding_vestibule", "airlock", "corridor"],
        )
        zones = {zone["id"]: zone for zone in manifest["zones"]}
        self.assertEqual(zones["boarding_vestibule"]["pressure"], "unpressurized")
        self.assertEqual(zones["airlock"]["pressure"], "airlock")
        self.assertEqual(manifest["navigation"]["entry_zone"], "boarding_vestibule")

    def test_pilot_exit_spawn_is_clear_and_within_reentry_range(self):
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        sockets = {socket["id"]: socket for socket in manifest["sockets"]}
        spawn = sockets["pilot_exit_spawn"]
        seat = sockets["pilot_seat_anchor"]
        self.assertEqual(spawn["module"], seat["module"])
        self.assertEqual(spawn["forward"], [0.0, 0.0, 1.0])

        # StagePilotEntry measures from the on-foot eye to the seat anchor.
        # These values mirror the shipped default possession configuration.
        capsule_center_y = spawn["position_m"][1] + 0.35 + 0.45 + 0.02
        eye = [spawn["position_m"][0], capsule_center_y + 0.65,
               spawn["position_m"][2]]
        offset = [seat["position_m"][axis] - eye[axis] for axis in range(3)]
        distance = sum(value * value for value in offset) ** 0.5
        self.assertLessEqual(distance, 2.5)
        facing = offset[2] / distance
        self.assertGreaterEqual(facing, 0.25)

    def test_generated_cockpit_fixture_provenance_and_closed_lods(self):
        dimensions = json.loads(DIMENSIONS.read_text(encoding="utf-8"))
        report = json.loads(INTERIOR_REPORT.read_text(encoding="utf-8"))
        configured = dimensions["production_fixtures"]
        dressing = report["production_dressing"]
        self.assertEqual(set(dressing), set(ACCEPTED_COCKPIT_FIXTURES))

        for fixture_id, expected in ACCEPTED_COCKPIT_FIXTURES.items():
            fixture = configured[fixture_id]
            manifest_path = (
                ROOT / "assets/generated" / expected["directory"] / "manifest.json"
            )
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            source = (
                ROOT / "assets/source/frontier_courier_mk1" / expected["source"]
            )
            fixture_report = dressing[fixture_id]

            self.assertEqual(fixture["source_request_hash"], expected["request_hash"])
            self.assertEqual(manifest["request_hash"], expected["request_hash"])
            self.assertEqual(manifest["tasks"]["preview"]["id"], expected["preview_task"])
            self.assertEqual(manifest["tasks"]["refine"]["id"], expected["refine_task"])
            self.assertEqual(manifest["consumed_credits"], 30)
            self.assertTrue(manifest["art_review"]["status"].startswith("accepted"))
            self.assertEqual(manifest["files"]["model_sha256"], expected["source_sha256"])
            self.assertEqual(sha256(source), expected["source_sha256"])
            self.assertEqual(
                fixture_report["source"]["sha256"], expected["source_sha256"]
            )

            # Meshy source topology is retained for provenance, but it is not
            # allowed into runtime until the deterministic repair has closed it.
            self.assertGreater(
                fixture_report["source"]["geometry"]["boundary_edges"], 0
            )
            for geometry in [
                fixture_report["repaired_geometry"],
                *(lod["geometry"] for lod in fixture_report["lods"]),
            ]:
                self.assertEqual(geometry["boundary_edges"], 0)
                self.assertEqual(geometry["non_manifold_edges"], 0)
                self.assertEqual(geometry["wire_edges"], 0)
                self.assertEqual(geometry["loose_vertices"], 0)

            targets = fixture["lod_triangle_targets"]
            self.assertEqual(
                [lod["target_triangles"] for lod in fixture_report["lods"]],
                targets,
            )
            self.assertEqual(
                {lod["instance_count"] for lod in fixture_report["lods"]},
                {expected["instance_count"]},
            )
            master_triangles = [
                lod["geometry"]["triangles"] for lod in fixture_report["lods"]
            ]
            self.assertEqual(master_triangles, sorted(master_triangles, reverse=True))
            for triangles, target in zip(master_triangles, targets):
                self.assertGreaterEqual(triangles, target * 0.95)
                self.assertLessEqual(triangles, target * 1.01)

    def test_generated_cockpit_room_uses_complete_right_handed_coverage(self):
        fixtures = json.loads(DIMENSIONS.read_text(encoding="utf-8"))[
            "production_fixtures"
        ]
        wall_instances = fixtures["cockpit_wall_panel"]["instances"]
        deck_instances = fixtures["cockpit_deck_panel"]["instances"]
        self.assertEqual(len(wall_instances), 29)
        self.assertEqual(len(deck_instances), 16)
        self.assertEqual(
            sum(item["id"].startswith("deck_") for item in deck_instances), 8
        )
        self.assertEqual(
            sum(item["id"].startswith("overhead_") for item in deck_instances), 8
        )
        self.assertEqual(
            sum("wall_" in item["id"] for item in wall_instances), 8
        )
        self.assertEqual(
            sum("chamfer_" in item["id"] for item in wall_instances), 16
        )
        self.assertEqual(
            sum("bulkhead_" in item["id"] or item["id"] == "aft_door_lintel"
                for item in wall_instances),
            5,
        )

        instances = wall_instances + deck_instances
        ids = [item["id"] for item in instances]
        self.assertEqual(len(ids), len(set(ids)))
        for item in instances:
            basis = item["basis_engine_xyz"]
            width = basis["width_axis"]
            height = basis["height_axis"]
            depth = basis["depth_axis"]
            for axis in (width, height, depth):
                self.assertAlmostEqual(math.sqrt(sum(value * value for value in axis)), 1.0)
            self.assertAlmostEqual(sum(a * b for a, b in zip(width, height)), 0.0)
            self.assertAlmostEqual(sum(a * b for a, b in zip(width, depth)), 0.0)
            self.assertAlmostEqual(sum(a * b for a, b in zip(height, depth)), 0.0)
            cross = [
                width[1] * height[2] - width[2] * height[1],
                width[2] * height[0] - width[0] * height[2],
                width[0] * height[1] - width[1] * height[0],
            ]
            for actual, expected in zip(cross, depth):
                self.assertAlmostEqual(actual, expected)
            self.assertTrue(all(value > 0.0 for value in item["scale"]))

    def test_cockpit_module_is_closed_and_keeps_runtime_primitive_contract(self):
        report = json.loads(INTERIOR_REPORT.read_text(encoding="utf-8"))
        self.assertEqual(len(report["primitive_order"]), 28)
        self.assertEqual(len(set(report["primitive_order"])), 28)
        cockpit = [
            item for item in report["geometry"]
            if item.get("module") == "cockpit_module"
        ]
        self.assertEqual([item["primitive_index"] for item in cockpit], [9, 10, 11])
        self.assertEqual([item["lod"] for item in cockpit], [0, 1, 2])
        triangles = [item["geometry"]["triangles"] for item in cockpit]
        self.assertEqual(triangles, sorted(triangles, reverse=True))
        self.assertLessEqual(triangles[0], 500000)
        for item in cockpit:
            geometry = item["geometry"]
            self.assertEqual(geometry["boundary_edges"], 0)
            self.assertEqual(geometry["non_manifold_edges"], 0)
            self.assertEqual(geometry["wire_edges"], 0)
            self.assertEqual(geometry["loose_vertices"], 0)

    def test_rejected_cockpit_generations_cannot_become_runtime_sources(self):
        rejected_manifests = {
            "frontier_courier_cockpit_pressure_frame_277b5de409f7b90f": 30,
            "frontier_courier_cockpit_sidewall_bay_81662bece7e9f0e3": 20,
            "frontier_courier_cockpit_overhead_bay_a0104cba35dce4bf": 20,
            "frontier_courier_cockpit_overhead_bay_9396982959e284d1": 30,
        }
        rejected_failures = {
            "frontier_courier_cockpit_room_liner_3736d972aed1c248": 10,
            "frontier_courier_cockpit_room_shell_b9c8f30e0b267fd2": 0,
            "frontier_courier_cockpit_overhead_slab_b01353ccabe9b9c4": 0,
        }
        recorded_credits = 0
        for directory, credits in rejected_manifests.items():
            path = ROOT / "assets/generated" / directory / "manifest.json"
            document = json.loads(path.read_text(encoding="utf-8"))
            self.assertTrue(document["art_review"]["status"].startswith("rejected"))
            self.assertNotIn("promoted_source", document["art_review"])
            self.assertEqual(document["consumed_credits"], credits)
            recorded_credits += credits
        for directory, credits in rejected_failures.items():
            path = ROOT / "assets/generated" / directory / "rejection.json"
            document = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(document["status"], "rejected_provider_failure")
            self.assertFalse(document["artifact_created"])
            self.assertEqual(document["observed_credit_delta"], credits)
            recorded_credits += credits
        self.assertEqual(recorded_credits, 110)

    def test_collision_sources_regenerate_deterministically(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            report = COLLISION_TOOL.generate(DIMENSIONS, MANIFEST, output)
            self.assertEqual(len(report["outputs"]), 8)
            self.assertEqual(report["total_boxes"], 88)
            for item in report["outputs"]:
                generated = Path(item["path"])
                canonical = ROOT / "assets/collision_sources" / generated.name
                self.assertEqual(generated.read_bytes(), canonical.read_bytes())
                document = json.loads(generated.read_text(encoding="utf-8"))
                box_ids = [box["id"] for box in document["boxes"]]
                self.assertEqual(box_ids, sorted(box_ids))
                self.assertEqual(len(box_ids), len(set(box_ids)))
                if item["module"] != "exterior_hull":
                    self.assertEqual(
                        sum(box["walkable"] for box in document["boxes"]), 1)

    def test_fixture_collision_rejects_malformed_records(self):
        valid = {
            "fixture": {
                "module": "cockpit_module",
                "collision_boxes": [{
                    "id": "fixture_box",
                    "center_m": [0.0, 0.0, 0.0],
                    "size_m": [1.0, 1.0, 1.0],
                    "walkable": False,
                }],
            },
        }
        self.assertEqual(
            COLLISION_TOOL.fixture_collision_boxes("cockpit_module", valid)[0]["id"],
            "fixture_box",
        )

        malformed = (
            {"fixture": None},
            {"fixture": {"module": "cockpit_module", "collision_boxes": [{}]}},
            {"fixture": {
                "module": "cockpit_module",
                "collision_boxes": [{
                    "id": "bad_center",
                    "center_m": [0.0, 0.0],
                    "size_m": [1.0, 1.0, 1.0],
                }],
            }},
            {"fixture": {
                "module": "cockpit_module",
                "collision_boxes": [{
                    "id": "bad_boolean",
                    "center_m": [0.0, 0.0, 0.0],
                    "size_m": [1.0, 1.0, 1.0],
                    "walkable": "false",
                }],
            }},
        )
        for fixtures in malformed:
            with self.subTest(fixtures=fixtures):
                with self.assertRaises(COLLISION_TOOL.CollisionGenerationError):
                    COLLISION_TOOL.fixture_collision_boxes(
                        "cockpit_module", fixtures)

    def test_runtime_content_regenerates_with_exact_coverage(self):
        with tempfile.TemporaryDirectory() as temporary:
            generated = Path(temporary) / "frontier_courier_mk1.tdcontent"
            report = CONTENT_TOOL.generate(MANIFEST, INTERIOR_REPORT, generated)
            self.assertEqual(generated.read_bytes(), CONTENT.read_bytes())
            self.assertEqual(report["bindings"], 61)
            self.assertEqual(report["visual_bindings"], 39)
            self.assertEqual(report["collision_bindings"], 8)
            self.assertEqual(report["navigation_bindings"], 7)
            self.assertEqual(report["walkable_bindings"], 7)

            for line in generated.read_text(encoding="ascii").splitlines():
                if not line.startswith("visual "):
                    continue
                tokens = line.split()
                model = tokens[-2].strip('"')
                primitive = int(tokens[-1])
                if model == "frontier_courier_hull_lods.tdmodel":
                    self.assertIn(primitive, range(3))
                else:
                    self.assertEqual(model, "frontier_courier_interior_lods.tdmodel")
                    self.assertIn(primitive, range(28))


if __name__ == "__main__":
    unittest.main()
