#!/usr/bin/env python3
"""Prepare a reviewed Meshy hull master as a production LOD0 in Blender.

Meshy is allowed to propose render geometry. The Dawning remains authoritative
for scale, axes, topology acceptance, and the exported asset identity. Run this
script with Blender, not the system Python interpreter.
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


class HullPreparationError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def report_path(path: Path) -> str:
    repo_root = Path(__file__).resolve().parents[2]
    try:
        return path.resolve().relative_to(repo_root).as_posix()
    except ValueError:
        return path.name


def reset_scene() -> None:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for collection in list(bpy.data.collections):
        if collection.name != "Collection":
            bpy.data.collections.remove(collection)
    bpy.data.orphans_purge(
        do_local_ids=True,
        do_linked_ids=True,
        do_recursive=True,
    )


def mesh_report(obj: bpy.types.Object) -> dict:
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.verts.ensure_lookup_table()
    bm.edges.ensure_lookup_table()
    bm.faces.ensure_lookup_table()

    unseen = set(vertex.index for vertex in bm.verts)
    component_sizes: list[int] = []
    while unseen:
        root = unseen.pop()
        stack = [root]
        size = 1
        while stack:
            vertex = bm.verts[stack.pop()]
            for edge in vertex.link_edges:
                other = edge.other_vert(vertex).index
                if other in unseen:
                    unseen.remove(other)
                    stack.append(other)
                    size += 1
        component_sizes.append(size)

    report = {
        "vertices": len(bm.verts),
        "edges": len(bm.edges),
        "faces": len(bm.faces),
        "triangles": sum(max(0, len(face.verts) - 2) for face in bm.faces),
        "connected_components": len(component_sizes),
        "largest_component_vertices": max(component_sizes, default=0),
        "boundary_edges": sum(edge.is_boundary for edge in bm.edges),
        "non_manifold_edges": sum(not edge.is_manifold for edge in bm.edges),
        "wire_edges": sum(edge.is_wire for edge in bm.edges),
        "loose_vertices": sum(not vertex.link_edges for vertex in bm.verts),
        "signed_volume_m3": bm.calc_volume(signed=True),
        "dimensions_blender_xyz_m": [float(value) for value in obj.dimensions],
        "uv_layers": len(obj.data.uv_layers),
        "materials": len(obj.data.materials),
    }
    bm.free()
    return report


def require_closed_connected_mesh(report: dict, stage: str) -> None:
    failures = []
    if report["connected_components"] != 1:
        failures.append(f"{report['connected_components']} connected components")
    if report["boundary_edges"]:
        failures.append(f"{report['boundary_edges']} boundary edges")
    if report["non_manifold_edges"]:
        failures.append(f"{report['non_manifold_edges']} non-manifold edges")
    if report["wire_edges"]:
        failures.append(f"{report['wire_edges']} wire edges")
    if report["loose_vertices"]:
        failures.append(f"{report['loose_vertices']} loose vertices")
    if not math.isfinite(report["signed_volume_m3"]) or report["signed_volume_m3"] <= 0.0:
        failures.append("non-positive or non-finite signed volume")
    if failures:
        raise HullPreparationError(f"{stage} rejected: " + ", ".join(failures))


def import_single_mesh(source: Path) -> bpy.types.Object:
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(source))
    imported = [obj for obj in bpy.data.objects if obj not in before]
    meshes = [obj for obj in imported if obj.type == "MESH"]
    unsupported = [obj.name for obj in imported if obj.type not in {"MESH", "EMPTY"}]
    if unsupported:
        raise HullPreparationError(
            "source contains unsupported imported objects: " + ", ".join(unsupported)
        )
    if len(meshes) != 1:
        raise HullPreparationError(
            f"expected exactly one imported mesh, found {len(meshes)}"
        )
    return meshes[0]


def normalize_dimensions(
    obj: bpy.types.Object,
    *,
    width_m: float,
    height_m: float,
    length_m: float,
) -> None:
    # Blender imports glTF +Y-up assets into Z-up space. Therefore engine
    # X/Y/Z (width/height/length) maps to Blender X/Z/Y.
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.origin_set(type="ORIGIN_GEOMETRY", center="BOUNDS")
    obj.location = (0.0, 0.0, 0.0)
    obj.rotation_euler = (0.0, 0.0, 0.0)
    obj.dimensions = (width_m, length_m, height_m)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)


def decimate_to_faces(obj: bpy.types.Object, target_faces: int) -> None:
    source_faces = len(obj.data.polygons)
    if target_faces <= 0 or target_faces > source_faces:
        raise HullPreparationError(
            f"target face count must be in [1, {source_faces}], got {target_faces}"
        )
    modifier = obj.modifiers.new(name="TD_Production_Decimate", type="DECIMATE")
    modifier.decimate_type = "COLLAPSE"
    modifier.ratio = target_faces / source_faces
    modifier.use_collapse_triangulate = True
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    obj.data.update()


def export_glb_objects(objects: list[bpy.types.Object], output: Path) -> None:
    if not objects:
        raise HullPreparationError("at least one object is required for GLB export")
    output.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.hide_set(False)
        obj.hide_render = False
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
        export_tangents=False,
        export_cameras=False,
        export_lights=False,
        # Scene extras include mutable Blender add-on state (for example time
        # tracking counters). Asset/module identity lives in The Dawning's
        # manifest and report, so excluding extras is required for byte-stable
        # exports from an interactive authoring session.
        export_extras=False,
    )


def export_glb(obj: bpy.types.Object, output: Path) -> None:
    export_glb_objects([obj], output)


def prepare_hull(
    source: Path,
    output: Path,
    report_path: Path,
    *,
    asset_id: str,
    module_id: str,
    width_m: float,
    height_m: float,
    length_m: float,
    target_faces: int,
    blend_path: Path | None = None,
) -> dict:
    if not source.is_file():
        raise HullPreparationError(f"source GLB not found: {source}")
    if source.suffix.lower() != ".glb":
        raise HullPreparationError("source must be a binary .glb")
    if any(value <= 0.0 or not math.isfinite(value)
           for value in (width_m, height_m, length_m)):
        raise HullPreparationError("authored dimensions must be finite and positive")

    started = time.time()
    reset_scene()
    source_obj = import_single_mesh(source)
    source_obj.name = "FrontierCourier_Source_Master"
    source_obj.data.name = "FrontierCourier_Source_Master_Mesh"
    source_report = mesh_report(source_obj)
    require_closed_connected_mesh(source_report, "source master")

    normalize_dimensions(
        source_obj,
        width_m=width_m,
        height_m=height_m,
        length_m=length_m,
    )
    normalized_report = mesh_report(source_obj)
    require_closed_connected_mesh(normalized_report, "normalized master")

    lod0 = source_obj.copy()
    lod0.data = source_obj.data.copy()
    lod0.name = "FrontierCourier_Hull_LOD0"
    lod0.data.name = "FrontierCourier_Hull_LOD0_Mesh"
    bpy.context.scene.collection.objects.link(lod0)
    source_obj.hide_set(True)
    source_obj.hide_render = True

    decimate_to_faces(lod0, target_faces)
    lod0_report = mesh_report(lod0)
    require_closed_connected_mesh(lod0_report, "LOD0")
    expected_dimensions = (width_m, length_m, height_m)
    for actual, expected in zip(lod0.dimensions, expected_dimensions):
        if abs(actual - expected) > 0.01:
            raise HullPreparationError(
                f"LOD0 dimension drifted from {expected:.3f} m to {actual:.3f} m"
            )

    lod0["td_asset_id"] = asset_id
    lod0["td_module_id"] = module_id
    lod0["td_lod"] = 0
    lod0["td_dimensions_engine_xyz_m"] = [width_m, height_m, length_m]
    lod0["td_source_sha256"] = sha256(source)
    export_glb(lod0, output)

    if blend_path is not None:
        blend_path.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(blend_path), check_existing=False)

    report = {
        "schema_version": 1,
        "asset_id": asset_id,
        "module_id": module_id,
        "tool": "tools/blender/prepare_hull_lod0.py",
        "blender_version": bpy.app.version_string,
        "source": {
            "path": report_path(source),
            "sha256": sha256(source),
            "byte_count": source.stat().st_size,
            "geometry": source_report,
        },
        "authored_dimensions_engine_xyz_m": [width_m, height_m, length_m],
        "normalized_geometry": normalized_report,
        "lod0": {
            "path": report_path(output),
            "sha256": sha256(output),
            "byte_count": output.stat().st_size,
            "target_faces": target_faces,
            "geometry": lod0_report,
        },
        "elapsed_seconds": round(time.time() - started, 3),
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--blend", type=Path)
    parser.add_argument("--asset-id", required=True)
    parser.add_argument("--module-id", required=True)
    parser.add_argument("--width", type=float, required=True)
    parser.add_argument("--height", type=float, required=True)
    parser.add_argument("--length", type=float, required=True)
    parser.add_argument("--target-faces", type=int, default=250000)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    report = prepare_hull(
        args.source.resolve(),
        args.output.resolve(),
        args.report.resolve(),
        asset_id=args.asset_id,
        module_id=args.module_id,
        width_m=args.width,
        height_m=args.height,
        length_m=args.length,
        target_faces=args.target_faces,
        blend_path=args.blend.resolve() if args.blend else None,
    )
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[sys.argv.index("--") + 1:]))
    except (HullPreparationError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
