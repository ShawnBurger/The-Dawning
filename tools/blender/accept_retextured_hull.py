#!/usr/bin/env python3
"""Accept a Meshy retexture and package exact-scale shared-material hull LODs."""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import struct
import sys
from pathlib import Path

import bmesh
import bpy


def load_common():
    path = Path(__file__).with_name("prepare_hull_lod0.py")
    spec = importlib.util.spec_from_file_location("td_prepare_hull_lod0", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load Blender hull helpers from {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


COMMON = load_common()
HullPreparationError = COMMON.HullPreparationError


# Engine coordinates are X=width, Y=height, Z=length. Blender imports the
# glTF ship as X=width, Y=length, Z=height. These overlapping volumes are
# deliberately a little larger than the authored clear interior so the
# architectural liners can hide the raw Boolean surface without z-fighting.
INTERIOR_CAVITY_CUTTERS = (
    {
        "id": "boarding_vestibule",
        "center_engine_xyz_m": (0.0, -0.1, -12.8),
        "size_engine_xyz_m": (1.45, 2.45, 2.7),
    },
    {
        "id": "airlock",
        "center_engine_xyz_m": (0.0, 0.0, -10.2),
        "size_engine_xyz_m": (2.5, 2.5, 3.0),
    },
    {
        "id": "corridor",
        "center_engine_xyz_m": (0.0, 0.0, -3.9),
        "size_engine_xyz_m": (1.5, 2.5, 10.0),
    },
    {
        "id": "cockpit",
        "center_engine_xyz_m": (0.0, 0.05, 5.0),
        "size_engine_xyz_m": (4.9, 2.6, 8.2),
    },
    {
        "id": "engineering",
        "center_engine_xyz_m": (2.5, 0.0, -4.5),
        "size_engine_xyz_m": (3.8, 2.6, 4.7),
    },
    {
        "id": "cargo",
        "center_engine_xyz_m": (-3.45, 0.0, -3.0),
        "size_engine_xyz_m": (5.7, 2.9, 6.2),
    },
    {
        "id": "crew",
        "center_engine_xyz_m": (2.6, 0.0, -0.8),
        "size_engine_xyz_m": (4.0, 2.5, 4.7),
    },
)


def weld_coincident_vertices(obj: bpy.types.Object, distance_m: float) -> int:
    if distance_m <= 0.0 or not math.isfinite(distance_m):
        raise HullPreparationError("weld distance must be finite and positive")
    before = len(obj.data.vertices)
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.remove_doubles(bm, verts=list(bm.verts), dist=distance_m)
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    return before - len(obj.data.vertices)


def require_textured(obj: bpy.types.Object) -> None:
    if not obj.data.uv_layers:
        raise HullPreparationError("retextured hull has no UV layer")
    materials = [material for material in obj.data.materials if material is not None]
    if len(materials) != 1:
        raise HullPreparationError(
            f"retextured hull requires one material, found {len(materials)}")
    if not materials[0].use_nodes or materials[0].node_tree is None:
        raise HullPreparationError("retextured hull material has no node graph")
    image_nodes = [
        node for node in materials[0].node_tree.nodes
        if node.type == "TEX_IMAGE" and node.image is not None
    ]
    if len(image_nodes) < 3:
        raise HullPreparationError(
            f"retextured PBR material exposes only {len(image_nodes)} image nodes")


def remove_empty_material_slots(obj: bpy.types.Object) -> int:
    removed = 0
    used = {polygon.material_index for polygon in obj.data.polygons}
    for index in range(len(obj.data.materials) - 1, -1, -1):
        if obj.data.materials[index] is None and index not in used:
            obj.data.materials.pop(index=index)
            removed += 1
    return removed


def material_report(obj: bpy.types.Object) -> dict:
    materials = [material for material in obj.data.materials if material is not None]
    image_nodes = []
    if materials and materials[0].use_nodes and materials[0].node_tree:
        for node in materials[0].node_tree.nodes:
            if node.type == "TEX_IMAGE" and node.image is not None:
                image_nodes.append({
                    "node": node.name,
                    "image": node.image.name,
                    "width": int(node.image.size[0]),
                    "height": int(node.image.size[1]),
                    "colorspace": node.image.colorspace_settings.name,
                })
    return {
        "uv_layers": [layer.name for layer in obj.data.uv_layers],
        "materials": [material.name for material in materials],
        "images": image_nodes,
    }


def create_cutter_box(spec: dict) -> bpy.types.Object:
    center_x, center_y, center_z = spec["center_engine_xyz_m"]
    size_x, size_y, size_z = spec["size_engine_xyz_m"]
    bpy.ops.mesh.primitive_cube_add(location=(center_x, center_z, center_y))
    cutter = bpy.context.object
    cutter.name = f"TD_Cavity_{spec['id']}"
    cutter.dimensions = (size_x, size_z, size_y)
    bpy.context.view_layer.objects.active = cutter
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    return cutter


def build_cavity_cutter() -> bpy.types.Object:
    cutter = create_cutter_box(INTERIOR_CAVITY_CUTTERS[0])
    cutter.name = "TD_InteriorCavity_Cutter"
    for spec in INTERIOR_CAVITY_CUTTERS[1:]:
        operand = create_cutter_box(spec)
        modifier = cutter.modifiers.new(name="TD_Cavity_Union", type="BOOLEAN")
        modifier.operation = "UNION"
        modifier.solver = "EXACT"
        modifier.object = operand
        bpy.context.view_layer.objects.active = cutter
        cutter.select_set(True)
        bpy.ops.object.modifier_apply(modifier=modifier.name)
        bpy.data.objects.remove(operand, do_unlink=True)

    cutter_report = COMMON.mesh_report(cutter)
    COMMON.require_closed_connected_mesh(cutter_report, "interior cavity cutter")
    return cutter


def cut_interior_cavity(
    obj: bpy.types.Object,
    *,
    width_m: float,
    height_m: float,
    length_m: float,
) -> dict:
    before = COMMON.mesh_report(obj)
    COMMON.require_closed_connected_mesh(before, "sealed textured hull")
    cutter = build_cavity_cutter()
    cutter_report = COMMON.mesh_report(cutter)

    modifier = obj.modifiers.new(name="TD_Boardable_Interior", type="BOOLEAN")
    modifier.operation = "DIFFERENCE"
    modifier.solver = "MANIFOLD"
    modifier.object = cutter
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    bpy.data.objects.remove(cutter, do_unlink=True)
    obj.data.update()
    empty_material_slots_removed = remove_empty_material_slots(obj)

    after = COMMON.mesh_report(obj)
    COMMON.require_closed_connected_mesh(after, "open boardable hull")
    removed_volume = before["signed_volume_m3"] - after["signed_volume_m3"]
    if removed_volume <= 1.0:
        raise HullPreparationError(
            f"interior cavity removed only {removed_volume:.3f} cubic meters")

    actual_width, actual_length, actual_height = obj.dimensions
    if abs(actual_width - width_m) > 0.025 or abs(actual_height - height_m) > 0.025:
        raise HullPreparationError("interior cut changed the authored width or height")
    # The open aperture removes the generated shell's single aft-most point.
    # The separately authored closure/frame restores the full 28 m assembly
    # envelope, while the open hull must remain within 25 cm of that contract.
    if not length_m - 0.25 <= actual_length <= length_m + 0.025:
        raise HullPreparationError(
            f"open hull length {actual_length:.3f} m is outside the aperture tolerance")

    return {
        "solver": "MANIFOLD",
        "cutters_engine_xyz_m": list(INTERIOR_CAVITY_CUTTERS),
        "cutter_geometry": cutter_report,
        "sealed_hull_geometry": before,
        "open_hull_geometry": after,
        "removed_volume_m3": removed_volume,
        "empty_material_slots_removed": empty_material_slots_removed,
        "aft_opening_plane_engine_z_m": -length_m / 2.0,
        "closure_required": "outer_hatch_panel",
    }


def create_lod(
    source: bpy.types.Object,
    *,
    level: int,
    target_faces: int,
    asset_id: str,
    module_id: str,
) -> bpy.types.Object:
    lod = source.copy()
    lod.data = source.data.copy()
    lod.name = f"FrontierCourier_Hull_LOD{level}"
    lod.data.name = f"FrontierCourier_Hull_LOD{level}_Mesh"
    bpy.context.scene.collection.objects.link(lod)
    COMMON.decimate_to_faces(lod, target_faces)
    report = COMMON.mesh_report(lod)
    COMMON.require_closed_connected_mesh(report, f"LOD{level}")
    if not lod.data.uv_layers:
        raise HullPreparationError(f"LOD{level} lost its UV layer")
    lod["td_asset_id"] = asset_id
    lod["td_module_id"] = module_id
    lod["td_lod"] = level
    return lod


def glb_mesh_order(path: Path) -> list[str]:
    data = path.read_bytes()
    if len(data) < 20:
        raise HullPreparationError("exported GLB is too small")
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2 or length != len(data):
        raise HullPreparationError("exported GLB header is invalid")
    document = None
    offset = 12
    while offset + 8 <= len(data):
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk = data[offset:offset + chunk_length]
        offset += chunk_length
        if chunk_type == 0x4E4F534A:
            document = json.loads(chunk.rstrip(b" \t\r\n\0").decode("utf-8"))
    if document is None:
        raise HullPreparationError("exported GLB has no JSON chunk")
    meshes = document.get("meshes") or []
    if any(len(mesh.get("primitives") or []) != 1 for mesh in meshes):
        raise HullPreparationError("each exported LOD must contain one primitive")
    return [str(mesh.get("name") or "") for mesh in meshes]


def accept_retexture(
    source: Path,
    output: Path,
    report_path: Path,
    *,
    asset_id: str,
    module_id: str,
    width_m: float,
    height_m: float,
    length_m: float,
    lod1_faces: int,
    lod2_faces: int,
    weld_distance_m: float = 1.0e-6,
    blend_path: Path | None = None,
) -> dict:
    if not source.is_file() or source.suffix.lower() != ".glb":
        raise HullPreparationError(f"retexture source must be an existing GLB: {source}")
    if not (length_m > width_m > height_m > 0.0):
        raise HullPreparationError(
            "ship proportions must satisfy length > width > height")
    if length_m / width_m < 1.5:
        raise HullPreparationError("ship length-to-width ratio must be at least 1.5")
    height_to_width = height_m / width_m
    if not 0.35 <= height_to_width <= 0.55:
        raise HullPreparationError(
            "frontier courier height-to-width ratio must be in [0.35, 0.55]")

    COMMON.reset_scene()
    lod0 = COMMON.import_single_mesh(source)
    raw_dimensions = [float(value) for value in lod0.dimensions]
    raw_vertices = len(lod0.data.vertices)
    raw_faces = len(lod0.data.polygons)
    require_textured(lod0)
    COMMON.normalize_dimensions(
        lod0,
        width_m=width_m,
        height_m=height_m,
        length_m=length_m,
    )
    welded_vertices = weld_coincident_vertices(lod0, weld_distance_m)
    lod0.name = "FrontierCourier_Hull_LOD0"
    lod0.data.name = "FrontierCourier_Hull_LOD0_Mesh"
    lod0_report = COMMON.mesh_report(lod0)
    COMMON.require_closed_connected_mesh(lod0_report, "textured LOD0")
    require_textured(lod0)
    cavity_report = cut_interior_cavity(
        lod0,
        width_m=width_m,
        height_m=height_m,
        length_m=length_m,
    )
    lod0_report = COMMON.mesh_report(lod0)
    require_textured(lod0)
    lod0["td_asset_id"] = asset_id
    lod0["td_module_id"] = module_id
    lod0["td_lod"] = 0
    lod0["td_dimensions_engine_xyz_m"] = [width_m, height_m, length_m]
    lod0["td_open_hull_dimensions_engine_xyz_m"] = [
        float(lod0.dimensions.x),
        float(lod0.dimensions.z),
        float(lod0.dimensions.y),
    ]
    lod0["td_source_sha256"] = COMMON.sha256(source)

    if not 100 <= lod2_faces < lod1_faces < raw_faces:
        raise HullPreparationError(
            "LOD targets must satisfy 100 <= LOD2 < LOD1 < LOD0 faces")
    lod1 = create_lod(
        lod0,
        level=1,
        target_faces=lod1_faces,
        asset_id=asset_id,
        module_id=module_id,
    )
    lod2 = create_lod(
        lod0,
        level=2,
        target_faces=lod2_faces,
        asset_id=asset_id,
        module_id=module_id,
    )
    lods = [lod0, lod1, lod2]
    open_hull_dimensions = tuple(float(value) for value in lod0.dimensions)
    for lod in lods:
        for actual, expected in zip(lod.dimensions, open_hull_dimensions):
            if abs(actual - expected) > 0.025:
                raise HullPreparationError(
                    f"{lod.name} dimension drifted from {expected:.3f} m "
                    f"to {actual:.3f} m")

    export_collection = bpy.data.collections.new("TD_FrontierCourier_Export")
    bpy.context.scene.collection.children.link(export_collection)
    for lod in lods:
        for collection in list(lod.users_collection):
            collection.objects.unlink(lod)
        export_collection.objects.link(lod)

    # One GLB keeps the shared PBR images single-copy. Runtime content locators
    # select primitive 0/1/2 for LOD0/1/2 after deterministic cooking.
    COMMON.export_glb_objects(lods, output)
    actual_mesh_order = glb_mesh_order(output)
    expected_mesh_order = [lod.data.name for lod in lods]
    if actual_mesh_order != expected_mesh_order:
        raise HullPreparationError(
            "exported GLB mesh order is not deterministic LOD0/LOD1/LOD2: "
            f"expected {expected_mesh_order}, got {actual_mesh_order}")
    if blend_path is not None:
        blend_path.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(
            filepath=str(blend_path), check_existing=False, compress=True)

    report = {
        "schema_version": 2,
        "asset_id": asset_id,
        "module_id": module_id,
        "tool": "tools/blender/accept_retextured_hull.py",
        "blender_version": bpy.app.version_string,
        "source": {
            "path": COMMON.report_path(source),
            "sha256": COMMON.sha256(source),
            "byte_count": source.stat().st_size,
            "raw_dimensions_blender_xyz": raw_dimensions,
            "raw_vertices": raw_vertices,
            "raw_faces": raw_faces,
        },
        "authored_dimensions_engine_xyz_m": [width_m, height_m, length_m],
        "proportions": {
            "length_to_width": length_m / width_m,
            "height_to_width": height_to_width,
        },
        "weld_distance_m": weld_distance_m,
        "welded_seam_vertices": welded_vertices,
        "interior_cavity": cavity_report,
        "material": material_report(lod0),
        "lods": [
            {"level": level, "geometry": COMMON.mesh_report(lod)}
            for level, lod in enumerate(lods)
        ],
        "output": {
            "path": COMMON.report_path(output),
            "sha256": COMMON.sha256(output),
            "byte_count": output.stat().st_size,
            "primitive_order": [lod.name for lod in lods],
            "mesh_order": actual_mesh_order,
        },
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
    parser.add_argument("--lod1-faces", type=int, default=80000)
    parser.add_argument("--lod2-faces", type=int, default=18000)
    parser.add_argument("--weld-distance", type=float, default=1.0e-6)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    report = accept_retexture(
        args.source.resolve(),
        args.output.resolve(),
        args.report.resolve(),
        asset_id=args.asset_id,
        module_id=args.module_id,
        width_m=args.width,
        height_m=args.height,
        length_m=args.length,
        lod1_faces=args.lod1_faces,
        lod2_faces=args.lod2_faces,
        weld_distance_m=args.weld_distance,
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
