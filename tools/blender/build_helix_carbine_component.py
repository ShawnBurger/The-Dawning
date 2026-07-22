#!/usr/bin/env python3
"""Build authored modular Helix carbine source geometry in Blender.

Meshy supplies reviewed PBR surface treatment after this step. The Dawning owns
the component envelope, interfaces, axes, pivots, and geometry boundaries.
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


class ComponentBuildError(RuntimeError):
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


def make_material(
    name: str,
    color: tuple[float, float, float, float],
    *,
    metallic: float,
    roughness: float,
) -> bpy.types.Material:
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    principled = material.node_tree.nodes.get("Principled BSDF")
    if principled is None:
        raise ComponentBuildError(f"material {name} has no Principled BSDF")
    principled.inputs["Base Color"].default_value = color
    principled.inputs["Metallic"].default_value = metallic
    principled.inputs["Roughness"].default_value = roughness
    return material


def apply_bevel(obj: bpy.types.Object, width: float, segments: int = 3) -> None:
    if width <= 0.0:
        return
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    modifier = obj.modifiers.new("TD_Edge_Bevel", "BEVEL")
    modifier.width = width
    modifier.segments = segments
    modifier.limit_method = "ANGLE"
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    obj.select_set(False)


def beveled_box(
    name: str,
    dimensions: tuple[float, float, float],
    location: tuple[float, float, float],
    material: bpy.types.Material,
    *,
    bevel: float,
) -> bpy.types.Object:
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.data.name = f"{name}_Mesh"
    obj.dimensions = dimensions
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    obj.data.materials.append(material)
    apply_bevel(obj, bevel)
    return obj


def chamfered_prism(
    name: str,
    *,
    width: float,
    length: float,
    height: float,
    chamfer: float,
    location: tuple[float, float, float],
    material: bpy.types.Material,
    bevel: float,
) -> bpy.types.Object:
    half_width = width * 0.5
    half_height = height * 0.5
    half_length = length * 0.5
    if chamfer <= 0.0 or chamfer >= min(half_width, half_height):
        raise ComponentBuildError("prism chamfer is outside its valid range")
    section = (
        (-half_width, -half_height + chamfer),
        (-half_width + chamfer, -half_height),
        (half_width - chamfer, -half_height),
        (half_width, -half_height + chamfer),
        (half_width, half_height - chamfer),
        (half_width - chamfer, half_height),
        (-half_width + chamfer, half_height),
        (-half_width, half_height - chamfer),
    )
    vertices = []
    for y in (-half_length, half_length):
        vertices.extend((x, y, z) for x, z in section)
    faces = []
    for index in range(8):
        next_index = (index + 1) % 8
        faces.append((index, next_index, 8 + next_index, 8 + index))
    faces.append(tuple(reversed(range(8))))
    faces.append(tuple(range(8, 16)))
    mesh = bpy.data.meshes.new(f"{name}_Mesh")
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = location
    obj.data.materials.append(material)
    apply_bevel(obj, bevel)
    return obj


def ring_y(
    name: str,
    *,
    outer_radius: float,
    inner_radius: float,
    depth: float,
    location: tuple[float, float, float],
    material: bpy.types.Material,
    segments: int = 40,
) -> bpy.types.Object:
    if not 0.0 < inner_radius < outer_radius:
        raise ComponentBuildError("ring radii are invalid")
    half = depth * 0.5
    vertices = []
    for y in (-half, half):
        for radius in (outer_radius, inner_radius):
            for index in range(segments):
                angle = 2.0 * math.pi * index / segments
                vertices.append((radius * math.cos(angle), y, radius * math.sin(angle)))

    def vertex(side: int, radius: int, index: int) -> int:
        return side * segments * 2 + radius * segments + index % segments

    faces = []
    for index in range(segments):
        next_index = (index + 1) % segments
        faces.append((
            vertex(0, 0, index), vertex(0, 0, next_index),
            vertex(1, 0, next_index), vertex(1, 0, index),
        ))
        faces.append((
            vertex(0, 1, next_index), vertex(0, 1, index),
            vertex(1, 1, index), vertex(1, 1, next_index),
        ))
        faces.append((
            vertex(0, 0, next_index), vertex(0, 0, index),
            vertex(0, 1, index), vertex(0, 1, next_index),
        ))
        faces.append((
            vertex(1, 0, index), vertex(1, 0, next_index),
            vertex(1, 1, next_index), vertex(1, 1, index),
        ))
    mesh = bpy.data.meshes.new(f"{name}_Mesh")
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.location = location
    obj.data.materials.append(material)
    apply_bevel(obj, min(depth * 0.08, 0.0012), segments=2)
    return obj


def cylinder_axis(
    name: str,
    *,
    radius: float,
    depth: float,
    axis: str,
    location: tuple[float, float, float],
    material: bpy.types.Material,
    vertices: int = 24,
) -> bpy.types.Object:
    rotations = {
        "X": (0.0, math.pi * 0.5, 0.0),
        "Y": (math.pi * 0.5, 0.0, 0.0),
        "Z": (0.0, 0.0, 0.0),
    }
    bpy.ops.mesh.primitive_cylinder_add(
        vertices=vertices,
        radius=radius,
        depth=depth,
        end_fill_type="NGON",
        location=location,
        rotation=rotations[axis],
    )
    obj = bpy.context.active_object
    obj.name = name
    obj.data.name = f"{name}_Mesh"
    obj.data.materials.append(material)
    apply_bevel(obj, min(radius * 0.12, 0.0007), segments=2)
    return obj


def build_upper_receiver(materials: dict[str, bpy.types.Material]) -> list[bpy.types.Object]:
    parts = [
        chamfered_prism(
            "UpperReceiver_StructuralShell",
            width=0.072,
            length=0.212,
            height=0.050,
            chamfer=0.008,
            location=(0.0, -0.002, 0.0),
            material=materials["receiver"],
            bevel=0.0015,
        ),
        chamfered_prism(
            "UpperReceiver_RearAdapter",
            width=0.068,
            length=0.026,
            height=0.048,
            chamfer=0.007,
            location=(0.0, -0.111, 0.0),
            material=materials["receiver"],
            bevel=0.0012,
        ),
        ring_y(
            "UpperReceiver_StockSocket",
            outer_radius=0.033,
            inner_radius=0.018,
            depth=0.035,
            location=(0.0, -0.1325, 0.0),
            material=materials["steel"],
        ),
        chamfered_prism(
            "UpperReceiver_ForwardAdapter",
            width=0.070,
            length=0.026,
            height=0.050,
            chamfer=0.007,
            location=(0.0, 0.108, 0.0),
            material=materials["receiver"],
            bevel=0.0012,
        ),
        ring_y(
            "UpperReceiver_BarrelSocket",
            outer_radius=0.032,
            inner_radius=0.0155,
            depth=0.040,
            location=(0.0, 0.130, 0.0),
            material=materials["steel"],
        ),
        beveled_box(
            "UpperReceiver_OpticDatum",
            (0.044, 0.102, 0.008),
            (0.0, -0.006, 0.029),
            materials["steel"],
            bevel=0.001,
        ),
        beveled_box(
            "UpperReceiver_LowerMount",
            (0.032, 0.046, 0.016),
            (0.0, -0.035, -0.027),
            materials["steel"],
            bevel=0.0015,
        ),
        cylinder_axis(
            "UpperReceiver_LowerMountPin",
            radius=0.0042,
            depth=0.036,
            axis="X",
            location=(0.0, -0.035, -0.027),
            material=materials["amber"],
        ),
    ]

    for side, suffix in ((-1.0, "Port"), (1.0, "Starboard")):
        parts.append(beveled_box(
            f"UpperReceiver_{suffix}_ServicePanel",
            (0.003, 0.088, 0.026),
            (side * 0.0368, -0.012, 0.0),
            materials["ceramic"],
            bevel=0.0006,
        ))
        parts.append(beveled_box(
            f"UpperReceiver_{suffix}_ThermalInsert",
            (0.0026, 0.050, 0.011),
            (side * 0.0370, 0.058, 0.0),
            materials["steel"],
            bevel=0.0004,
        ))
        for index, y in enumerate((-0.074, -0.022, 0.035, 0.083)):
            for z in (-0.015, 0.015):
                parts.append(cylinder_axis(
                    f"UpperReceiver_{suffix}_Fastener_{index}_{'Low' if z < 0 else 'High'}",
                    radius=0.0021,
                    depth=0.0028,
                    axis="X",
                    location=(side * 0.0370, y, z),
                    material=materials["steel"],
                    vertices=16,
                ))

    parts.extend((
        beveled_box(
            "UpperReceiver_ChargingSlider",
            (0.0048, 0.034, 0.012),
            (0.0360, -0.050, 0.001),
            materials["amber"],
            bevel=0.0007,
        ),
        cylinder_axis(
            "UpperReceiver_ReleaseButton",
            radius=0.004,
            depth=0.003,
            axis="X",
            location=(-0.0370, -0.048, 0.001),
            material=materials["amber"],
            vertices=20,
        ),
        chamfered_prism(
            "UpperReceiver_AftSeamBand",
            width=0.074,
            length=0.006,
            height=0.052,
            chamfer=0.008,
            location=(0.0, -0.080, 0.0),
            material=materials["steel"],
            bevel=0.0005,
        ),
        chamfered_prism(
            "UpperReceiver_ForwardSeamBand",
            width=0.074,
            length=0.006,
            height=0.052,
            chamfer=0.008,
            location=(0.0, 0.080, 0.0),
            material=materials["steel"],
            bevel=0.0005,
        ),
    ))
    return parts


def world_bounds(objects: list[bpy.types.Object]) -> tuple[Vector, Vector]:
    corners = [obj.matrix_world @ Vector(corner) for obj in objects for corner in obj.bound_box]
    minimum = Vector(tuple(min(corner[axis] for corner in corners) for axis in range(3)))
    maximum = Vector(tuple(max(corner[axis] for corner in corners) for axis in range(3)))
    return minimum, maximum


def geometry_report(objects: list[bpy.types.Object]) -> dict:
    minimum, maximum = world_bounds(objects)
    return {
        "object_count": len(objects),
        "vertices": sum(len(obj.data.vertices) for obj in objects),
        "edges": sum(len(obj.data.edges) for obj in objects),
        "polygons": sum(len(obj.data.polygons) for obj in objects),
        "bounds_blender_xyz_m": {
            "minimum": [float(value) for value in minimum],
            "maximum": [float(value) for value in maximum],
            "size": [float(value) for value in maximum - minimum],
        },
        "objects": [obj.name for obj in sorted(objects, key=lambda item: item.name)],
        "materials": sorted({
            slot.material.name
            for obj in objects
            for slot in obj.material_slots
            if slot.material is not None
        }),
    }


def validate_bounds(report: dict, envelope_engine_xyz: list[float]) -> None:
    expected_blender = [
        envelope_engine_xyz[0],
        envelope_engine_xyz[2],
        envelope_engine_xyz[1],
    ]
    actual = report["bounds_blender_xyz_m"]["size"]
    failures = []
    for axis, (size, limit) in enumerate(zip(actual, expected_blender)):
        if size > limit + 1.0e-5:
            failures.append(f"axis {axis} exceeds {limit:.6f} m with {size:.6f} m")
        if size < limit * 0.85:
            failures.append(f"axis {axis} fills only {size / limit:.1%} of its envelope")
    if failures:
        raise ComponentBuildError("component bounds rejected: " + "; ".join(failures))


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
        export_tangents=False,
        export_cameras=False,
        export_lights=False,
        export_extras=False,
    )


def build_component(
    plan_path: Path,
    component_id: str,
    output: Path,
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
        raise ComponentBuildError(f"component not found in plan: {component_id}")
    if component_id != "upper_receiver":
        raise ComponentBuildError(
            f"component builder is not implemented yet: {component_id}"
        )

    reset_scene()
    materials = {
        "receiver": make_material(
            "TD_Graphite_Receiver", (0.075, 0.085, 0.10, 1.0),
            metallic=0.78, roughness=0.32,
        ),
        "steel": make_material(
            "TD_Phosphate_Steel", (0.025, 0.03, 0.036, 1.0),
            metallic=0.9, roughness=0.4,
        ),
        "ceramic": make_material(
            "TD_Thermal_Ceramic", (0.035, 0.04, 0.047, 1.0),
            metallic=0.12, roughness=0.58,
        ),
        "amber": make_material(
            "TD_Amber_Witness", (0.52, 0.19, 0.025, 1.0),
            metallic=0.55, roughness=0.3,
        ),
    }
    objects = build_upper_receiver(materials)
    for obj in objects:
        obj["td_asset_id"] = plan["asset_id"]
        obj["td_component_id"] = component_id
        obj["td_module_id"] = component["module_id"]
    report_geometry = geometry_report(objects)
    validate_bounds(report_geometry, component["envelope"])
    export_glb(objects, output)

    if blend_path is not None:
        blend_path.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(blend_path), check_existing=False)

    report = {
        "schema_version": 1,
        "tool": "tools/blender/build_helix_carbine_component.py",
        "blender_version": bpy.app.version_string,
        "asset_id": plan["asset_id"],
        "component_id": component_id,
        "module_id": component["module_id"],
        "plan": {
            "path": repo_path(plan_path),
            "sha256": sha256(plan_path),
        },
        "authored_envelope_engine_xyz_m": component["envelope"],
        "assembly_position_engine_xyz_m": component["assembly_position"],
        "interfaces": component["interfaces"],
        "geometry": report_geometry,
        "output": {
            "path": repo_path(output),
            "sha256": sha256(output),
            "byte_count": output.stat().st_size,
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
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--blend", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    report = build_component(
        args.plan.resolve(),
        args.component,
        args.output.resolve(),
        args.report.resolve(),
        args.blend.resolve() if args.blend else None,
    )
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        separator = sys.argv.index("--")
        raise SystemExit(main(sys.argv[separator + 1:]))
    except (ComponentBuildError, KeyError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
