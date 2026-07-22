#!/usr/bin/env python3
"""Validate and restore an authored component after Meshy retexturing.

Meshy may normalize scale and combine source objects while creating UVs. This
tool accepts only uniform normalization that preserves the authored triangle
and connected-island counts, then restores the exact authored bounds.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import time
from pathlib import Path

import bmesh
import bpy
from mathutils import Vector


class ComponentAcceptanceError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def repo_path(path: Path) -> str:
    root = Path(__file__).resolve().parents[2]
    try:
        return path.resolve().relative_to(root).as_posix()
    except ValueError:
        return path.name


def reset_scene() -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_glb(path: Path) -> list[bpy.types.Object]:
    if not path.is_file():
        raise ComponentAcceptanceError(f"GLB does not exist: {path}")
    bpy.ops.import_scene.gltf(filepath=str(path))
    objects = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    if not objects:
        raise ComponentAcceptanceError(f"GLB contains no mesh objects: {path}")
    return objects


def world_bounds(objects: list[bpy.types.Object]) -> tuple[Vector, Vector]:
    corners = [
        obj.matrix_world @ Vector(corner)
        for obj in objects
        for corner in obj.bound_box
    ]
    minimum = Vector(tuple(min(corner[axis] for corner in corners) for axis in range(3)))
    maximum = Vector(tuple(max(corner[axis] for corner in corners) for axis in range(3)))
    return minimum, maximum


def mesh_island_count(obj: bpy.types.Object) -> int:
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    if not bm.verts:
        bm.free()
        return 0
    # glTF duplicates vertices along normals and UV seams. Weld a temporary
    # validation mesh so connectivity describes geometry, not indexing.
    bmesh.ops.remove_doubles(bm, verts=list(bm.verts), dist=1.0e-6)
    remaining = set(bm.verts)
    islands = 0
    while remaining:
        islands += 1
        stack = [remaining.pop()]
        while stack:
            vertex = stack.pop()
            for edge in vertex.link_edges:
                neighbor = edge.other_vert(vertex)
                if neighbor in remaining:
                    remaining.remove(neighbor)
                    stack.append(neighbor)
    bm.free()
    return islands


def material_report(objects: list[bpy.types.Object]) -> dict:
    materials = {
        slot.material
        for obj in objects
        for slot in obj.material_slots
        if slot.material is not None
    }
    images = {
        node.image
        for material in materials
        if material.use_nodes and material.node_tree is not None
        for node in material.node_tree.nodes
        if node.type == "TEX_IMAGE" and node.image is not None
    }
    return {
        "material_count": len(materials),
        "materials": sorted(material.name for material in materials),
        "uv_layer_count": sum(len(obj.data.uv_layers) for obj in objects),
        "image_count": len(images),
        "images": sorted({
            (
                image.name,
                int(image.size[0]),
                int(image.size[1]),
                image.colorspace_settings.name,
            )
            for image in images
        }),
    }


def geometry_report(objects: list[bpy.types.Object]) -> dict:
    minimum, maximum = world_bounds(objects)
    return {
        "object_count": len(objects),
        "vertices": sum(len(obj.data.vertices) for obj in objects),
        "edges": sum(len(obj.data.edges) for obj in objects),
        "triangles": sum(
            max(0, len(polygon.vertices) - 2)
            for obj in objects
            for polygon in obj.data.polygons
        ),
        "connected_islands": sum(mesh_island_count(obj) for obj in objects),
        "bounds_blender_xyz_m": {
            "minimum": [float(value) for value in minimum],
            "maximum": [float(value) for value in maximum],
            "size": [float(value) for value in maximum - minimum],
            "center": [float(value) for value in (minimum + maximum) * 0.5],
        },
        "materials": material_report(objects),
    }


def require_pbr(report: dict) -> None:
    materials = report["materials"]
    if materials["material_count"] != 1:
        raise ComponentAcceptanceError(
            f"retexture requires one packed material, found {materials['material_count']}"
        )
    if materials["uv_layer_count"] < 1:
        raise ComponentAcceptanceError("retexture has no UV layer")
    if materials["image_count"] < 3:
        raise ComponentAcceptanceError(
            f"retexture exposes only {materials['image_count']} PBR images"
        )


def uniform_restore_scale(authored: dict, textured: dict) -> float:
    authored_size = authored["bounds_blender_xyz_m"]["size"]
    textured_size = textured["bounds_blender_xyz_m"]["size"]
    factors = []
    for target, current in zip(authored_size, textured_size):
        if target <= 0.0 or current <= 0.0:
            raise ComponentAcceptanceError("component bounds must be positive")
        factors.append(target / current)
    mean = sum(factors) / len(factors)
    if not math.isfinite(mean) or mean <= 0.0:
        raise ComponentAcceptanceError("restore scale is not finite and positive")
    if max(abs(value - mean) for value in factors) > 1.0e-5:
        raise ComponentAcceptanceError(
            "retexture changed authored proportions: restore factors "
            + ", ".join(f"{value:.8f}" for value in factors)
        )
    return mean


def restore_transform(
    objects: list[bpy.types.Object],
    scale: float,
    target_center: list[float],
) -> None:
    for obj in objects:
        obj.location *= scale
        obj.scale *= scale
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
        obj.select_set(False)

    minimum, maximum = world_bounds(objects)
    current_center = (minimum + maximum) * 0.5
    offset = Vector(target_center) - current_center
    for obj in objects:
        obj.location += offset
    bpy.context.view_layer.update()


def require_geometry_preserved(authored: dict, textured: dict) -> None:
    for key in ("triangles", "connected_islands"):
        if authored[key] != textured[key]:
            raise ComponentAcceptanceError(
                f"retexture changed {key}: authored={authored[key]}, "
                f"textured={textured[key]}"
            )


def require_restored_bounds(authored: dict, restored: dict) -> None:
    for label in ("size", "center"):
        expected = authored["bounds_blender_xyz_m"][label]
        actual = restored["bounds_blender_xyz_m"][label]
        for axis, (expected_value, actual_value) in enumerate(zip(expected, actual)):
            if abs(expected_value - actual_value) > 2.0e-6:
                raise ComponentAcceptanceError(
                    f"restored {label} axis {axis} is {actual_value:.9f}; "
                    f"expected {expected_value:.9f}"
                )


def export_glb(objects: list[bpy.types.Object], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.export_scene.gltf(
        filepath=str(output),
        export_format="GLB",
        use_selection=True,
        export_apply=True,
        export_yup=True,
        export_materials="EXPORT",
        export_texcoords=True,
        export_normals=True,
        export_tangents=True,
        export_cameras=False,
        export_lights=False,
        export_extras=True,
    )


def accept_component(
    *,
    plan_path: Path,
    component_id: str,
    authored_path: Path,
    textured_path: Path,
    meshy_manifest_path: Path,
    output_path: Path,
    report_path: Path,
    blend_path: Path | None,
) -> dict:
    started = time.time()
    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    component = next(
        (item for item in plan["components"] if item["component_id"] == component_id),
        None,
    )
    if component is None:
        raise ComponentAcceptanceError(f"component not found in plan: {component_id}")
    manifest = json.loads(meshy_manifest_path.read_text(encoding="utf-8"))
    expected_hash = manifest["files"]["model_sha256"]
    if sha256(textured_path) != expected_hash:
        raise ComponentAcceptanceError("Meshy model hash does not match its manifest")

    reset_scene()
    authored_objects = import_glb(authored_path)
    authored = geometry_report(authored_objects)

    reset_scene()
    textured_objects = import_glb(textured_path)
    textured = geometry_report(textured_objects)
    require_geometry_preserved(authored, textured)
    require_pbr(textured)
    restore_scale = uniform_restore_scale(authored, textured)
    restore_transform(
        textured_objects,
        restore_scale,
        authored["bounds_blender_xyz_m"]["center"],
    )

    for obj in textured_objects:
        obj["td_asset_id"] = plan["asset_id"]
        obj["td_component_id"] = component_id
        obj["td_module_id"] = component["module_id"]
        obj["td_interfaces"] = json.dumps(component["interfaces"], separators=(",", ":"))
    if len(textured_objects) == 1:
        textured_objects[0].name = f"HelixCarbine_{component_id.title().replace('_', '')}_LOD0"
        textured_objects[0].data.name = f"{textured_objects[0].name}_Mesh"

    restored = geometry_report(textured_objects)
    require_geometry_preserved(authored, restored)
    require_restored_bounds(authored, restored)
    export_glb(textured_objects, output_path)

    if blend_path is not None:
        blend_path.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(blend_path), check_existing=False)

    report = {
        "schema_version": 1,
        "tool": "tools/blender/accept_retextured_component.py",
        "blender_version": bpy.app.version_string,
        "asset_id": plan["asset_id"],
        "component_id": component_id,
        "module_id": component["module_id"],
        "plan": {"path": repo_path(plan_path), "sha256": sha256(plan_path)},
        "interfaces": component["interfaces"],
        "authored_input": {
            "path": repo_path(authored_path),
            "sha256": sha256(authored_path),
            "geometry": authored,
        },
        "meshy_input": {
            "path": repo_path(textured_path),
            "sha256": expected_hash,
            "manifest": repo_path(meshy_manifest_path),
            "request_hash": manifest["request_hash"],
            "task_id": manifest["tasks"]["retexture"]["id"],
            "geometry_before_restore": textured,
        },
        "acceptance": {
            "uniform_restore_scale": restore_scale,
            "triangle_count_preserved": True,
            "connected_islands_preserved": True,
            "authored_bounds_restored": True,
            "pbr_material_verified": True,
        },
        "output_geometry": restored,
        "output": {
            "path": repo_path(output_path),
            "sha256": sha256(output_path),
            "byte_count": output_path.stat().st_size,
        },
        "elapsed_seconds": round(time.time() - started, 3),
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--component", required=True)
    parser.add_argument("--authored", type=Path, required=True)
    parser.add_argument("--textured", type=Path, required=True)
    parser.add_argument("--meshy-manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--blend", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    report = accept_component(
        plan_path=args.plan.resolve(),
        component_id=args.component,
        authored_path=args.authored.resolve(),
        textured_path=args.textured.resolve(),
        meshy_manifest_path=args.meshy_manifest.resolve(),
        output_path=args.output.resolve(),
        report_path=args.report.resolve(),
        blend_path=args.blend.resolve() if args.blend else None,
    )
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        separator = sys.argv.index("--")
        raise SystemExit(main(sys.argv[separator + 1:]))
    except (ComponentAcceptanceError, KeyError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
