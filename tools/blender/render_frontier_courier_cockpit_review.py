#!/usr/bin/env python3
"""Render deterministic review views of the Frontier Courier cockpit LOD0."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import bpy
from mathutils import Vector


class CockpitReviewError(RuntimeError):
    pass


def engine_to_blender(value: tuple[float, float, float]) -> Vector:
    return Vector((value[0], value[2], value[1]))


def look_at(obj: bpy.types.Object, target: Vector) -> None:
    direction = target - obj.location
    if direction.length <= 1.0e-6:
        raise CockpitReviewError("camera or light cannot look at itself")
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def add_area_light(
    name: str,
    position_engine: tuple[float, float, float],
    target_engine: tuple[float, float, float],
    *,
    energy: float,
    size: float,
    color: tuple[float, float, float],
) -> None:
    data = bpy.data.lights.new(name, "AREA")
    data.energy = energy
    data.shape = "DISK"
    data.size = size
    data.color = color
    obj = bpy.data.objects.new(name, data)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = engine_to_blender(position_engine)
    look_at(obj, engine_to_blender(target_engine))


def configure_scene(model: Path) -> bpy.types.Object:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(model))
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    cockpit = next(
        (obj for obj in meshes
         if obj.name.startswith("FrontierCourier_CockpitModule_LOD0")),
        None,
    )
    if cockpit is None:
        raise CockpitReviewError("model has no cockpit LOD0 object")
    for obj in meshes:
        obj.hide_render = obj is not cockpit

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x = 1600
    scene.render.resolution_y = 900
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.render.image_settings.color_mode = "RGBA"
    scene.view_settings.look = "AgX - Medium High Contrast"
    scene.world = bpy.data.worlds.new("TD_Cockpit_Review_World")
    scene.world.color = (0.008, 0.012, 0.018)

    add_area_light(
        "TD_Review_Key",
        (-1.1, 0.92, 0.4),
        (0.0, -0.45, 1.8),
        energy=110.0,
        size=2.2,
        color=(0.70, 0.84, 1.0),
    )
    add_area_light(
        "TD_Review_Fill",
        (1.6, 0.25, -1.8),
        (0.0, -0.35, 1.2),
        energy=75.0,
        size=1.8,
        color=(1.0, 0.64, 0.42),
    )
    add_area_light(
        "TD_Review_Forward",
        (0.0, 0.75, 3.65),
        (0.0, -0.35, 1.5),
        energy=95.0,
        size=1.5,
        color=(0.55, 0.78, 1.0),
    )
    return cockpit


def render_views(model: Path, output_dir: Path) -> list[Path]:
    configure_scene(model)
    scene = bpy.context.scene
    camera_data = bpy.data.cameras.new("TD_Cockpit_Review_Camera")
    camera_data.lens = 34.0
    camera_data.sensor_width = 36.0
    camera_data.clip_start = 0.04
    camera_data.clip_end = 50.0
    camera = bpy.data.objects.new("TD_Cockpit_Review_Camera", camera_data)
    scene.collection.objects.link(camera)
    scene.camera = camera

    output_dir.mkdir(parents=True, exist_ok=True)
    views = (
        ("entry", (0.0, 0.32, -3.45), (0.0, -0.35, 2.35), 33.0),
        # An 18 mm review lens approximates the wide seated field of view used
        # by modern cockpit games. It must show the console lip and complete
        # display array instead of turning the center display into a false
        # full-screen close-up.
        ("pilot", (0.0, 0.16, 2.18), (0.0, -0.22, 4.20), 18.0),
        ("cabin_oblique", (-1.25, 0.18, -2.55), (0.15, -0.38, 2.15), 35.0),
    )
    outputs = []
    for label, position, target, lens in views:
        camera.location = engine_to_blender(position)
        camera_data.lens = lens
        look_at(camera, engine_to_blender(target))
        path = output_dir / f"frontier_courier_cockpit_{label}.png"
        scene.render.filepath = str(path)
        bpy.ops.render.render(write_still=True)
        outputs.append(path)
    return outputs


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    outputs = render_views(args.model.resolve(), args.output_dir.resolve())
    for path in outputs:
        print(path)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[sys.argv.index("--") + 1:]))
    except (CockpitReviewError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
