#!/usr/bin/env python3
"""Render deterministic material review views for an arbitrary GLB in Blender.

This tool is intentionally read-only. It imports a candidate into a clean scene,
records the geometry and material bindings Blender actually sees, and renders
four neutral-lit views. It does not normalize, repair, decimate, or export the
source asset.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import time
from pathlib import Path

import bpy
from mathutils import Vector


class AssetReviewError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def reset_scene() -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_meshes(model: Path) -> list[bpy.types.Object]:
    if not model.is_file():
        raise AssetReviewError(f"model not found: {model}")
    if model.suffix.lower() != ".glb":
        raise AssetReviewError("model must be a binary .glb")

    bpy.ops.import_scene.gltf(filepath=str(model))
    meshes = sorted(
        (obj for obj in bpy.context.scene.objects if obj.type == "MESH"),
        key=lambda obj: obj.name,
    )
    if not meshes:
        raise AssetReviewError("model contains no mesh objects")
    return meshes


def world_corners(meshes: list[bpy.types.Object]) -> list[Vector]:
    return [
        obj.matrix_world @ Vector(corner)
        for obj in meshes
        for corner in obj.bound_box
    ]


def bounds(meshes: list[bpy.types.Object]) -> tuple[Vector, Vector]:
    corners = world_corners(meshes)
    minimum = Vector(tuple(min(point[axis] for point in corners) for axis in range(3)))
    maximum = Vector(tuple(max(point[axis] for point in corners) for axis in range(3)))
    return minimum, maximum


def look_at(obj: bpy.types.Object, target: Vector) -> None:
    direction = target - obj.location
    if direction.length <= 1.0e-8:
        raise AssetReviewError("camera or light cannot look at itself")
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def configure_scene() -> bpy.types.Scene:
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x = 1200
    scene.render.resolution_y = 1200
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.film_transparent = False
    scene.render.use_file_extension = True
    scene.view_settings.look = "AgX - Medium High Contrast"

    world = bpy.data.worlds.new("TD_Asset_Review_World")
    world.use_nodes = True
    background = world.node_tree.nodes.get("Background")
    if background is None:
        raise AssetReviewError("review world has no Background node")
    background.inputs["Color"].default_value = (0.025, 0.032, 0.044, 1.0)
    background.inputs["Strength"].default_value = 0.22
    scene.world = world
    return scene


def add_area_light(
    name: str,
    center: Vector,
    offset: Vector,
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
    obj.location = center + offset
    look_at(obj, center)


def configure_lighting(center: Vector, largest_dimension: float) -> None:
    scale = max(largest_dimension, 0.01)
    # Light positions and emitter sizes scale with the asset. Area-light power
    # must therefore scale with distance squared to keep review exposure
    # consistent from small weapon parts to ship hulls. The original rig was
    # authored around a 2 m asset.
    energy_scale = (scale / 2.0) ** 2
    add_area_light(
        "TD_Review_Key",
        center,
        Vector((-1.6, -1.8, 2.2)) * scale,
        energy=1050.0 * energy_scale,
        size=1.4 * scale,
        color=(0.86, 0.93, 1.0),
    )
    add_area_light(
        "TD_Review_Fill",
        center,
        Vector((1.8, -0.4, 0.7)) * scale,
        energy=680.0 * energy_scale,
        size=1.2 * scale,
        color=(1.0, 0.82, 0.68),
    )
    add_area_light(
        "TD_Review_Rim",
        center,
        Vector((0.4, 1.9, 1.4)) * scale,
        energy=850.0 * energy_scale,
        size=1.0 * scale,
        color=(0.64, 0.78, 1.0),
    )


def add_camera() -> bpy.types.Object:
    data = bpy.data.cameras.new("TD_Asset_Review_Camera")
    data.type = "ORTHO"
    data.clip_start = 0.01
    data.clip_end = 10000.0
    camera = bpy.data.objects.new("TD_Asset_Review_Camera", data)
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    return camera


def fit_camera(
    camera: bpy.types.Object,
    corners: list[Vector],
    center: Vector,
    direction: Vector,
    up_axis: str,
) -> None:
    direction.normalize()
    largest_dimension = max((max(c[axis] for c in corners) - min(c[axis] for c in corners)
                             for axis in range(3)), default=1.0)
    camera.location = center + direction * max(2.5 * largest_dimension, 1.0)
    camera.rotation_euler = (center - camera.location).to_track_quat("-Z", up_axis).to_euler()
    bpy.context.view_layer.update()

    inverse = camera.matrix_world.inverted()
    camera_points = [inverse @ point for point in corners]
    span_x = max(point.x for point in camera_points) - min(point.x for point in camera_points)
    span_y = max(point.y for point in camera_points) - min(point.y for point in camera_points)
    camera.data.ortho_scale = max(span_x, span_y, 1.0e-4) * 1.16


def material_report(material: bpy.types.Material) -> dict:
    image_names: list[str] = []
    node_types: list[str] = []
    if material.use_nodes and material.node_tree is not None:
        for node in material.node_tree.nodes:
            node_types.append(node.bl_idname)
            if node.bl_idname == "ShaderNodeTexImage" and node.image is not None:
                image_names.append(node.image.name)
    return {
        "name": material.name,
        "use_nodes": bool(material.use_nodes),
        "node_types": sorted(set(node_types)),
        "images": sorted(set(image_names)),
    }


def geometry_report(meshes: list[bpy.types.Object]) -> dict:
    minimum, maximum = bounds(meshes)
    materials = sorted(
        {slot.material for obj in meshes for slot in obj.material_slots if slot.material},
        key=lambda material: material.name,
    )
    images = sorted(
        {
            image.name
            for material in materials
            if material.use_nodes and material.node_tree is not None
            for node in material.node_tree.nodes
            if node.bl_idname == "ShaderNodeTexImage" and node.image is not None
            for image in (node.image,)
        }
    )
    return {
        "mesh_object_count": len(meshes),
        "mesh_objects": [
            {
                "name": obj.name,
                "vertices": len(obj.data.vertices),
                "edges": len(obj.data.edges),
                "polygons": len(obj.data.polygons),
                "material_slots": len(obj.material_slots),
            }
            for obj in meshes
        ],
        "vertices": sum(len(obj.data.vertices) for obj in meshes),
        "edges": sum(len(obj.data.edges) for obj in meshes),
        "polygons": sum(len(obj.data.polygons) for obj in meshes),
        "bounds_blender_xyz": {
            "minimum": [float(value) for value in minimum],
            "maximum": [float(value) for value in maximum],
            "size": [float(value) for value in maximum - minimum],
        },
        "materials": [material_report(material) for material in materials],
        "image_count": len(images),
        "images": images,
    }


def render_reviews(model: Path, output_dir: Path, label: str) -> dict:
    started = time.time()
    reset_scene()
    meshes = import_meshes(model)
    scene = configure_scene()
    corners = world_corners(meshes)
    minimum, maximum = bounds(meshes)
    center = (minimum + maximum) * 0.5
    largest_dimension = max(maximum - minimum)
    configure_lighting(center, largest_dimension)
    camera = add_camera()

    safe_label = "".join(char if char.isalnum() or char in "-_" else "_" for char in label)
    if not safe_label:
        raise AssetReviewError("label must contain at least one safe filename character")
    output_dir.mkdir(parents=True, exist_ok=True)
    views = (
        ("axis_x", Vector((1.0, 0.0, 0.0)), "Y"),
        ("axis_y", Vector((0.0, -1.0, 0.0)), "Y"),
        ("axis_z", Vector((0.0, 0.0, 1.0)), "Y"),
        ("three_quarter", Vector((1.0, -1.0, 0.72)), "Y"),
    )
    rendered = []
    for view_name, direction, up_axis in views:
        fit_camera(camera, corners, center, direction, up_axis)
        path = output_dir / f"{safe_label}_{view_name}.png"
        scene.render.filepath = str(path)
        bpy.ops.render.render(write_still=True)
        rendered.append(path.name)

    report = {
        "schema_version": 1,
        "tool": "tools/blender/render_asset_review.py",
        "blender_version": bpy.app.version_string,
        "source": {
            "path": str(model),
            "sha256": sha256(model),
            "byte_count": model.stat().st_size,
        },
        "label": safe_label,
        "geometry": geometry_report(meshes),
        "rendered_files": rendered,
        "elapsed_seconds": round(time.time() - started, 3),
    }
    report_path = output_dir / f"{safe_label}_review.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--label", required=True)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    report = render_reviews(
        args.model.resolve(),
        args.output_dir.resolve(),
        args.label,
    )
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        separator = sys.argv.index("--")
        raise SystemExit(main(sys.argv[separator + 1:]))
    except (AssetReviewError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
