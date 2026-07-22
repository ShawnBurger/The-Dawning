import importlib.util
import json
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
