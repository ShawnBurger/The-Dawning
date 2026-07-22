#!/usr/bin/env python3
"""Build the Frontier Courier's deterministic module-local interior kit.

The generated exterior owns visual character. This script owns architecture:
clearances, portal openings, module-local origins, closure meshes, and stable
primitive order. Run it with Blender 4.5 or newer.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import struct
import sys
from pathlib import Path

import bpy


def load_common():
    path = Path(__file__).with_name("prepare_hull_lod0.py")
    spec = importlib.util.spec_from_file_location("td_prepare_hull_lod0", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load Blender helpers from {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


COMMON = load_common()
InteriorBuildError = COMMON.HullPreparationError

MODULE_ORDER = (
    "boarding_vestibule",
    "airlock_module",
    "main_corridor",
    "cockpit_module",
    "engineering_module",
    "cargo_module",
    "crew_module",
)

PALETTE = {
    "structure": (0.075, 0.09, 0.105, 1.0),
    "panel": (0.30, 0.34, 0.37, 1.0),
    "panel_light": (0.56, 0.60, 0.62, 1.0),
    "floor": (0.045, 0.052, 0.060, 1.0),
    "safety": (0.82, 0.19, 0.035, 1.0),
    "light": (0.80, 0.90, 0.96, 1.0),
    "display": (0.035, 0.42, 0.56, 1.0),
    "seat": (0.11, 0.14, 0.17, 1.0),
    "bunk": (0.22, 0.28, 0.31, 1.0),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class MeshBuilder:
    def __init__(self) -> None:
        self.vertices: list[tuple[float, float, float]] = []
        self.faces: list[tuple[int, ...]] = []
        self.colors: list[tuple[float, float, float, float]] = []

    def box(
        self,
        center_engine_xyz: tuple[float, float, float],
        size_engine_xyz: tuple[float, float, float],
        color: tuple[float, float, float, float],
    ) -> None:
        cx, cy, cz = center_engine_xyz
        sx, sy, sz = size_engine_xyz
        if min(sx, sy, sz) <= 0.0:
            raise InteriorBuildError(f"box size must be positive: {size_engine_xyz}")

        # Engine X/Y/Z maps to Blender X/Z/Y.
        bx, by, bz = cx, cz, cy
        dx, dy, dz = sx * 0.5, sz * 0.5, sy * 0.5
        start = len(self.vertices)
        self.vertices.extend((
            (bx - dx, by - dy, bz - dz),
            (bx + dx, by - dy, bz - dz),
            (bx + dx, by + dy, bz - dz),
            (bx - dx, by + dy, bz - dz),
            (bx - dx, by - dy, bz + dz),
            (bx + dx, by - dy, bz + dz),
            (bx + dx, by + dy, bz + dz),
            (bx - dx, by + dy, bz + dz),
        ))
        local_faces = (
            (0, 3, 2, 1),
            (4, 5, 6, 7),
            (0, 1, 5, 4),
            (3, 7, 6, 2),
            (0, 4, 7, 3),
            (1, 2, 6, 5),
        )
        self.faces.extend(tuple(start + index for index in face)
                          for face in local_faces)
        self.colors.extend((color,) * len(local_faces))


def create_vertex_color_material() -> bpy.types.Material:
    material = bpy.data.materials.new("TD_Interior_VertexColor_PBR")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    shader = nodes.new("ShaderNodeBsdfPrincipled")
    vertex_color = nodes.new("ShaderNodeVertexColor")
    vertex_color.layer_name = "Color"
    shader.inputs["Metallic"].default_value = 0.48
    shader.inputs["Roughness"].default_value = 0.40
    links.new(vertex_color.outputs["Color"], shader.inputs["Base Color"])
    links.new(shader.outputs["BSDF"], output.inputs["Surface"])
    return material


def finish_object(
    name: str,
    mesh_name: str,
    builder: MeshBuilder,
    material: bpy.types.Material,
    *,
    bevel_width: float,
) -> bpy.types.Object:
    if not builder.faces:
        raise InteriorBuildError(f"{name} has no geometry")
    mesh = bpy.data.meshes.new(mesh_name)
    mesh.from_pydata(builder.vertices, [], builder.faces)
    mesh.materials.append(material)
    color_layer = mesh.color_attributes.new(
        name="Color", type="BYTE_COLOR", domain="CORNER")
    for polygon, color in zip(mesh.polygons, builder.colors):
        for loop_index in polygon.loop_indices:
            color_layer.data[loop_index].color = color
    mesh.color_attributes.active_color = color_layer
    mesh.update()

    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    if bevel_width > 0.0:
        modifier = obj.modifiers.new("TD_Edge_Bevel", "BEVEL")
        modifier.width = bevel_width
        modifier.segments = 2
        modifier.limit_method = "ANGLE"
        modifier.angle_limit = math.radians(30.0)
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        bpy.ops.object.modifier_apply(modifier=modifier.name)
    for polygon in obj.data.polygons:
        polygon.use_smooth = False
    obj.data.update()
    if len(obj.data.materials) != 1 or not obj.data.color_attributes:
        raise InteriorBuildError(f"{name} lost its single-material vertex-color contract")
    return obj


def add_floor_and_ceiling(builder: MeshBuilder, w: float, h: float, length: float) -> None:
    thickness = 0.05
    builder.box((0.0, -h * 0.5 - thickness * 0.5, 0.0),
                (w + 0.1, thickness, length + 0.1), PALETTE["floor"])
    builder.box((0.0, h * 0.5 + thickness * 0.5, 0.0),
                (w + 0.1, thickness, length + 0.1), PALETTE["structure"])


def add_side_wall(
    builder: MeshBuilder,
    *,
    side: int,
    w: float,
    h: float,
    length: float,
    openings_z: tuple[float, ...] = (),
    door_width: float = 1.05,
    door_height: float = 2.2,
) -> None:
    thickness = 0.05
    x = side * (w * 0.5 + thickness * 0.5)
    lower = -length * 0.5
    intervals = sorted(
        (max(lower, center - door_width * 0.5),
         min(length * 0.5, center + door_width * 0.5))
        for center in openings_z
    )
    cursor = lower
    for start, end in intervals:
        if start > cursor:
            builder.box((x, 0.0, (cursor + start) * 0.5),
                        (thickness, h, start - cursor), PALETTE["structure"])
        lintel_height = max(0.05, h - door_height)
        builder.box((x, door_height * 0.5, (start + end) * 0.5),
                    (thickness, lintel_height, end - start), PALETTE["safety"])
        cursor = max(cursor, end)
    if cursor < length * 0.5:
        builder.box((x, 0.0, (cursor + length * 0.5) * 0.5),
                    (thickness, h, length * 0.5 - cursor), PALETTE["structure"])


def add_end_frame(
    builder: MeshBuilder,
    *,
    side: int,
    w: float,
    h: float,
    length: float,
    door_width: float = 1.05,
    door_height: float = 2.2,
) -> None:
    thickness = 0.05
    z = side * (length * 0.5 + thickness * 0.5)
    column_width = max(0.05, (w - door_width) * 0.5)
    for x_sign in (-1, 1):
        x = x_sign * (door_width * 0.5 + column_width * 0.5)
        builder.box((x, 0.0, z), (column_width, h, thickness), PALETTE["structure"])
    lintel_height = max(0.05, h - door_height)
    builder.box((0.0, door_height * 0.5, z),
                (door_width, lintel_height, thickness), PALETTE["safety"])


def add_end_wall(
    builder: MeshBuilder,
    *,
    side: int,
    w: float,
    h: float,
    length: float,
) -> None:
    thickness = 0.05
    builder.box((0.0, 0.0, side * (length * 0.5 + thickness * 0.5)),
                (w, h, thickness), PALETTE["structure"])


def add_rib(
    builder: MeshBuilder,
    w: float,
    h: float,
    z: float,
    omit_sides: set[int],
) -> None:
    for side in (-1, 1):
        if side in omit_sides:
            continue
        builder.box((side * (w * 0.5 + 0.0125), 0.0, z),
                    (0.025, h, 0.08), PALETTE["panel_light"])
    builder.box((0.0, h * 0.5 + 0.0125, z),
                (w, 0.025, 0.08), PALETTE["panel_light"])


def add_ceiling_lights(builder: MeshBuilder, h: float, positions: tuple[float, ...]) -> None:
    for z in positions:
        builder.box((0.0, h * 0.5 - 0.008, z),
                    (0.42, 0.016, 0.52), PALETTE["light"])


def module_shell(
    module_id: str,
    clear_size: tuple[float, float, float],
    detail: int,
    portal_sockets: list[dict],
) -> MeshBuilder:
    w, h, length = clear_size
    builder = MeshBuilder()
    add_floor_and_ceiling(builder, w, h, length)

    side_openings: dict[int, tuple[float, ...]] = {-1: (), 1: ()}
    end_modes = {-1: "wall", 1: "wall"}
    mutable_side_openings: dict[int, list[float]] = {-1: [], 1: []}
    for socket in portal_sockets:
        x, _, z = (float(value) for value in socket["position_m"])
        forward = tuple(float(value) for value in socket["forward"])
        if abs(abs(x) - w * 0.5) <= 0.15 and abs(forward[0]) > 0.5:
            mutable_side_openings[1 if x > 0.0 else -1].append(z)
        elif abs(abs(z) - length * 0.5) <= 0.15 and abs(forward[2]) > 0.5:
            end_modes[1 if z > 0.0 else -1] = "frame"
        else:
            raise InteriorBuildError(
                f"portal socket {socket['id']} is not on the {module_id} boundary")
    side_openings = {
        side: tuple(sorted(values))
        for side, values in mutable_side_openings.items()
    }

    for side in (-1, 1):
        add_side_wall(
            builder,
            side=side,
            w=w,
            h=h,
            length=length,
            openings_z=side_openings[side],
        )
        if end_modes[side] == "frame":
            add_end_frame(builder, side=side, w=w, h=h, length=length)
        else:
            add_end_wall(builder, side=side, w=w, h=h, length=length)

    if detail >= 1:
        spacing = 1.5 if module_id == "main_corridor" else 2.0
        rib_count = max(1, int(length / spacing))
        for index in range(1, rib_count + 1):
            z = -length * 0.5 + index * length / (rib_count + 1)
            omit_sides = {
                side
                for side, openings in side_openings.items()
                if any(abs(z - opening) < 0.65 for opening in openings)
            }
            add_rib(builder, w, h, z, omit_sides)
        light_count = max(1, int(length / 2.8))
        add_ceiling_lights(
            builder,
            h,
            tuple(-length * 0.5 + (index + 1) * length / (light_count + 1)
                  for index in range(light_count)),
        )

    if detail >= 2:
        add_module_details(builder, module_id, w, h, length)
    return builder


def add_module_details(
    builder: MeshBuilder,
    module_id: str,
    w: float,
    h: float,
    length: float,
) -> None:
    floor_y = -h * 0.5
    if module_id == "boarding_vestibule":
        builder.box((0.0, floor_y + 0.025, 0.0),
                    (0.82, 0.05, length * 0.75), PALETTE["safety"])
    elif module_id == "airlock_module":
        for x in (-0.55, 0.0, 0.55):
            builder.box((x, floor_y + 0.018, 0.0),
                        (0.18, 0.036, length * 0.72), PALETTE["panel_light"])
        builder.box((w * 0.5 - 0.025, 0.15, 0.0),
                    (0.05, 0.75, 0.52), PALETTE["display"])
    elif module_id == "main_corridor":
        for x in (-0.46, 0.46):
            builder.box((x, floor_y + 0.012, 0.0),
                        (0.07, 0.024, length * 0.92), PALETTE["panel_light"])
    elif module_id == "cockpit_module":
        # Pilot seat and controls are inside the cockpit visual; possession and
        # safe-exit ownership remain in the assembly manifest.
        builder.box((0.0, floor_y + 0.23, 2.1), (0.62, 0.46, 0.68), PALETTE["seat"])
        builder.box((0.0, floor_y + 0.78, 2.28), (0.62, 0.92, 0.22), PALETTE["seat"])
        builder.box((0.0, floor_y + 0.72, 2.82), (2.9, 0.78, 0.48), PALETTE["panel"])
        builder.box((0.0, floor_y + 0.96, 2.54), (1.65, 0.32, 0.06), PALETTE["display"])
        for x in (-1.85, 1.85):
            builder.box((x, floor_y + 0.48, 1.65), (0.58, 0.58, 2.7), PALETTE["panel"])
        for x in (-1.45, 0.0, 1.45):
            builder.box((x, 0.36, length * 0.5 - 0.035),
                        (1.18, 0.82, 0.07), PALETTE["display"])
    elif module_id == "engineering_module":
        for z in (-1.45, 0.0, 1.45):
            builder.box((w * 0.5 - 0.42, floor_y + 0.72, z),
                        (0.76, 1.42, 0.78), PALETTE["panel"])
            builder.box((w * 0.5 - 0.82, floor_y + 0.78, z),
                        (0.04, 0.62, 0.42), PALETTE["display"])
    elif module_id == "cargo_module":
        for x in (-1.8, 0.0, 1.8):
            builder.box((x, floor_y + 0.018, 0.0),
                        (0.09, 0.036, length * 0.86), PALETTE["safety"])
        for z in (-2.1, 2.1):
            builder.box((0.0, floor_y + 0.06, z),
                        (w * 0.78, 0.12, 0.12), PALETTE["panel_light"])
    elif module_id == "crew_module":
        for z in (-1.25, 1.25):
            builder.box((w * 0.5 - 0.46, floor_y + 0.42, z),
                        (0.82, 0.32, 1.72), PALETTE["bunk"])
            builder.box((w * 0.5 - 0.46, floor_y + 1.08, z),
                        (0.82, 0.20, 1.72), PALETTE["bunk"])
        builder.box((-w * 0.5 + 0.30, floor_y + 0.78, 1.45),
                    (0.56, 1.50, 0.82), PALETTE["panel"])


def build_moving_part(
    part: dict,
    interaction: dict,
    socket: dict,
    dimensions: dict,
    material: bpy.types.Material,
) -> bpy.types.Object:
    builder = MeshBuilder()
    center = tuple(float(value) for value in socket["position_m"])
    forward = tuple(float(value) for value in socket["forward"])
    width = (dimensions["pressure_structure"]["outer_hatch_clear_width"]
             if part["id"] == "outer_hatch_panel"
             else dimensions["human_factors"]["nominal_door_clear_width"])
    height = dimensions["human_factors"]["nominal_door_clear_height"]
    depth = 0.12
    if abs(forward[2]) > 0.5:
        panel_size = (width, height, depth)
        inset_size = (width * 0.62, 0.12, depth * 1.18)
        inset_center = (center[0], center[1], center[2] - forward[2] * 0.01)
    elif abs(forward[0]) > 0.5:
        panel_size = (depth, height, width)
        inset_size = (depth * 1.18, 0.12, width * 0.62)
        inset_center = (center[0] - forward[0] * 0.01, center[1], center[2])
    else:
        raise InteriorBuildError(f"unsupported closure normal for {part['id']}")
    builder.box(center, panel_size, PALETTE["structure"])
    builder.box(inset_center, inset_size, PALETTE["safety"])
    name = f"FrontierCourier_{part['id']}"
    return finish_object(
        name,
        f"{name}_Mesh",
        builder,
        material,
        bevel_width=0.018,
    )


def glb_document(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 20:
        raise InteriorBuildError("exported GLB is too small")
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2 or length != len(data):
        raise InteriorBuildError("exported GLB header is invalid")
    offset = 12
    while offset + 8 <= len(data):
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk = data[offset:offset + chunk_length]
        offset += chunk_length
        if chunk_type == 0x4E4F534A:
            return json.loads(chunk.rstrip(b" \t\r\n\0").decode("utf-8"))
    raise InteriorBuildError("exported GLB has no JSON chunk")


def validate_export(path: Path, expected_mesh_order: list[str]) -> dict:
    document = glb_document(path)
    meshes = document.get("meshes") or []
    actual_order = [str(mesh.get("name") or "") for mesh in meshes]
    if actual_order != expected_mesh_order:
        raise InteriorBuildError(
            f"mesh order mismatch: expected {expected_mesh_order}, got {actual_order}")
    for index, mesh in enumerate(meshes):
        primitives = mesh.get("primitives") or []
        if len(primitives) != 1:
            raise InteriorBuildError(f"mesh {index} must export exactly one primitive")
        attributes = primitives[0].get("attributes") or {}
        if "POSITION" not in attributes or "COLOR_0" not in attributes:
            raise InteriorBuildError(
                f"mesh {index} is missing POSITION or COLOR_0")
    return {
        "mesh_order": actual_order,
        "mesh_count": len(meshes),
        "material_count": len(document.get("materials") or []),
    }


def validate_portal_alignment(manifest: dict) -> None:
    modules = {item["id"]: item for item in manifest["modules"]}
    sockets = {item["id"]: item for item in manifest["sockets"]}

    def world_position(socket_id: str) -> tuple[float, float, float]:
        socket = sockets[socket_id]
        module = modules[socket["module"]]
        transform = module["transform"]
        if transform["rotation_euler_degrees"] != [0.0, 0.0, 0.0]:
            raise InteriorBuildError(
                f"portal alignment requires zero module rotation: {module['id']}")
        if transform["scale"] != [1.0, 1.0, 1.0]:
            raise InteriorBuildError(
                f"portal alignment requires unit module scale: {module['id']}")
        return tuple(
            float(transform["position_m"][axis]) +
            float(socket["position_m"][axis])
            for axis in range(3)
        )

    for portal in manifest["portals"]:
        a = world_position(portal["socket_a"])
        b = world_position(portal["socket_b"])
        distance = math.sqrt(sum((a[axis] - b[axis]) ** 2 for axis in range(3)))
        if distance > 1.0e-4:
            raise InteriorBuildError(
                f"portal {portal['id']} socket endpoints differ by {distance:.6f} m")


def build_interior(
    dimensions_path: Path,
    manifest_path: Path,
    output: Path,
    report_path: Path,
) -> dict:
    dimensions = json.loads(dimensions_path.read_text(encoding="utf-8"))
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    module_dimensions = {item["id"]: item for item in dimensions["modules"]}
    manifest_modules = {item["id"]: item for item in manifest["modules"]}
    if any(module_id not in module_dimensions or module_id not in manifest_modules
           for module_id in MODULE_ORDER):
        raise InteriorBuildError("interior module definitions are incomplete")
    validate_portal_alignment(manifest)

    COMMON.reset_scene()
    material = create_vertex_color_material()
    objects: list[bpy.types.Object] = []
    bindings: dict[str, int] = {}
    geometry_reports: list[dict] = []
    socket_by_module: dict[str, list[dict]] = {}
    for socket in manifest["sockets"]:
        if socket["type"] == "portal":
            socket_by_module.setdefault(socket["module"], []).append(socket)

    for module_id in MODULE_ORDER:
        size = tuple(float(value) for value
                     in module_dimensions[module_id]["clear_size_m"])
        module = manifest_modules[module_id]
        for lod in range(3):
            detail = 2 - lod
            builder = module_shell(
                module_id,
                size,
                detail,
                socket_by_module.get(module_id, []),
            )
            label = "".join(part.title() for part in module_id.split("_"))
            name = f"FrontierCourier_{label}_LOD{lod}"
            obj = finish_object(
                name,
                f"{name}_Mesh",
                builder,
                material,
                bevel_width=(0.018 if lod == 0 else 0.010 if lod == 1 else 0.0),
            )
            primitive_index = len(objects)
            objects.append(obj)
            geometry_reports.append({
                "primitive_index": primitive_index,
                "name": name,
                "module": module_id,
                "lod": lod,
                "geometry": COMMON.mesh_report(obj),
            })
            if lod == 0:
                bindings[module["visual_source"]] = primitive_index
            bindings[module["lods"][lod]["source"]] = primitive_index

    interaction_by_id = {item["id"]: item for item in manifest["interactions"]}
    socket_by_id = {item["id"]: item for item in manifest["sockets"]}
    for part in manifest["moving_parts"]:
        interaction = interaction_by_id[part["interaction"]]
        socket = socket_by_id[interaction["socket"]]
        obj = build_moving_part(part, interaction, socket, dimensions, material)
        primitive_index = len(objects)
        objects.append(obj)
        bindings[part["visual_source"]] = primitive_index
        geometry_reports.append({
            "primitive_index": primitive_index,
            "name": obj.name,
            "moving_part": part["id"],
            "geometry": COMMON.mesh_report(obj),
        })

    for item in geometry_reports:
        geometry = item["geometry"]
        if geometry["boundary_edges"] or geometry["non_manifold_edges"]:
            raise InteriorBuildError(
                f"{item['name']} is not closed and manifold: {geometry}")
        if geometry["materials"] != 1 or geometry["uv_layers"] != 0:
            raise InteriorBuildError(
                f"{item['name']} violated the one-material vertex-color contract")

    COMMON.export_glb_objects(objects, output)
    export_report = validate_export(
        output, [obj.data.name for obj in objects])
    report = {
        "schema_version": 1,
        "asset_id": dimensions["asset_id"],
        "tool": "tools/blender/build_frontier_courier_interior.py",
        "blender_version": bpy.app.version_string,
        "design_authority": {
            "path": COMMON.report_path(dimensions_path),
            "sha256": sha256(dimensions_path),
        },
        "assembly_manifest": {
            "path": COMMON.report_path(manifest_path),
            "sha256": sha256(manifest_path),
        },
        "primitive_order": [obj.name for obj in objects],
        "bindings": bindings,
        "geometry": geometry_reports,
        "output": {
            "path": COMMON.report_path(output),
            "sha256": sha256(output),
            "byte_count": output.stat().st_size,
            **export_report,
        },
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dimensions", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    report = build_interior(
        args.dimensions.resolve(),
        args.manifest.resolve(),
        args.output.resolve(),
        args.report.resolve(),
    )
    print(json.dumps(report["output"], indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[sys.argv.index("--") + 1:]))
    except (InteriorBuildError, KeyError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
