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
import bmesh
from mathutils import Matrix, Vector


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
    "display_glass": (0.012, 0.055, 0.075, 1.0),
    "indicator": (0.06, 0.72, 0.88, 1.0),
    "seat": (0.11, 0.14, 0.17, 1.0),
    "pilot_seat": (0.085, 0.105, 0.125, 1.0),
    "flight_station": (0.065, 0.085, 0.105, 1.0),
    "cockpit_wall": (0.105, 0.135, 0.16, 1.0),
    "cockpit_deck": (0.055, 0.07, 0.08, 1.0),
    "bunk": (0.22, 0.28, 0.31, 1.0),
}

SUPPORTED_FIXTURE_FORWARD_AXES = {"-Z", "+Z"}


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
        self.smooth_faces: list[bool] = []
        self.bevel_vertex_count: int | None = None

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
        self.smooth_faces.extend((False,) * len(local_faces))

    def append_mesh(
        self,
        vertices_blender_xyz: list[tuple[float, float, float]],
        faces: list[tuple[int, ...]],
        color: tuple[float, float, float, float],
        *,
        smooth: bool = True,
    ) -> None:
        if not vertices_blender_xyz or not faces:
            raise InteriorBuildError("appended mesh must contain vertices and faces")
        vertex_count = len(vertices_blender_xyz)
        if any(len(face) < 3 or any(index < 0 or index >= vertex_count
                                   for index in face)
               for face in faces):
            raise InteriorBuildError("appended mesh contains an invalid face")
        start = len(self.vertices)
        if self.bevel_vertex_count is None:
            self.bevel_vertex_count = start
        self.vertices.extend(vertices_blender_xyz)
        self.faces.extend(tuple(start + index for index in face) for face in faces)
        self.colors.extend((color,) * len(faces))
        self.smooth_faces.extend((smooth,) * len(faces))


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
    vertex_count = len(mesh.vertices)
    face_count = len(mesh.polygons)
    mesh.validate(clean_customdata=False)
    if len(mesh.vertices) != vertex_count or len(mesh.polygons) != face_count:
        raise InteriorBuildError(
            f"{name} required destructive mesh validation")
    if len(builder.colors) != len(mesh.polygons) or \
            len(builder.smooth_faces) != len(mesh.polygons):
        raise InteriorBuildError(f"{name} lost face provenance")
    mesh.materials.append(material)
    color_layer = mesh.color_attributes.new(
        name="Color", type="BYTE_COLOR", domain="CORNER")
    for polygon, color in zip(mesh.polygons, builder.colors):
        for loop_index in polygon.loop_indices:
            color_layer.data[loop_index].color = color
    for polygon, smooth in zip(mesh.polygons, builder.smooth_faces):
        polygon.use_smooth = smooth
    mesh.color_attributes.active_color = color_layer
    mesh.update()

    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    if bevel_width > 0.0:
        modifier = obj.modifiers.new("TD_Edge_Bevel", "BEVEL")
        modifier.width = bevel_width
        modifier.segments = 2
        if builder.bevel_vertex_count is None:
            modifier.limit_method = "ANGLE"
            modifier.angle_limit = math.radians(30.0)
        else:
            # Imported production dressing is already authored and repaired.
            # Restrict the procedural bevel to room-shell vertices so it cannot
            # multiply the seat's decimated topology or soften restraint detail.
            group = obj.vertex_groups.new(name="TD_Procedural_Bevel")
            group.add(range(builder.bevel_vertex_count), 1.0, "REPLACE")
            modifier.limit_method = "VGROUP"
            modifier.vertex_group = group.name
            modifier.affect = "EDGES"
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        bpy.ops.object.modifier_apply(modifier=modifier.name)
    obj.data.update()
    if len(obj.data.materials) != 1 or not obj.data.color_attributes:
        raise InteriorBuildError(f"{name} lost its single-material vertex-color contract")
    return obj


def triangle_count(obj: bpy.types.Object) -> int:
    return sum(max(0, len(polygon.vertices) - 2) for polygon in obj.data.polygons)


def mesh_coordinate_bounds(obj: bpy.types.Object) -> tuple[Vector, Vector]:
    if obj.type != "MESH" or not obj.data.vertices:
        raise InteriorBuildError(f"{obj.name} has no mesh vertices")
    xs = [vertex.co.x for vertex in obj.data.vertices]
    ys = [vertex.co.y for vertex in obj.data.vertices]
    zs = [vertex.co.z for vertex in obj.data.vertices]
    return Vector((min(xs), min(ys), min(zs))), Vector((max(xs), max(ys), max(zs)))


def activate_object(obj: bpy.types.Object) -> None:
    bpy.ops.object.select_all(action="DESELECT")
    obj.hide_viewport = False
    obj.hide_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def fixture_vector(value: object, label: str) -> Vector:
    if not isinstance(value, list) or len(value) != 3:
        raise InteriorBuildError(f"{label} must contain three numbers")
    vector = Vector(tuple(float(component) for component in value))
    if not all(math.isfinite(component) for component in vector):
        raise InteriorBuildError(f"{label} contains a non-finite value")
    return vector


def fixture_instances(
    fixture_id: str,
    fixture: dict,
    sockets: dict[str, dict],
    *,
    floor_y: float,
    required_socket: str | None,
) -> tuple[list[dict], Vector | None]:
    socket_id = str(fixture.get("mount_socket") or "")
    anchor = None
    if required_socket:
        if socket_id != required_socket or socket_id not in sockets:
            raise InteriorBuildError(f"{fixture_id} mount socket is invalid")
        anchor = fixture_vector(
            sockets[socket_id].get("position_m"),
            f"{fixture_id} mount socket position",
        )
        instances = [{
            "id": fixture_id,
            "position_m": [anchor.x, floor_y, anchor.z],
            "scale": [1.0, 1.0, 1.0],
            "basis_engine_xyz": {
                "width_axis": [1.0, 0.0, 0.0],
                "height_axis": [0.0, 1.0, 0.0],
                "depth_axis": [0.0, 0.0, 1.0],
            },
        }]
    else:
        if socket_id:
            raise InteriorBuildError(
                f"{fixture_id} declares an unexpected mount socket")
        configured_instances = fixture.get("instances")
        if configured_instances is not None:
            if not isinstance(configured_instances, list) or not configured_instances:
                raise InteriorBuildError(
                    f"{fixture_id} instances must be a non-empty list")
            instances = configured_instances
        else:
            position = fixture.get("module_local_bottom_center_m")
            if not isinstance(position, list) or len(position) != 3:
                raise InteriorBuildError(
                    f"{fixture_id} has no module-local placement")
            instances = [{
                "id": fixture_id,
                "position_m": position,
                "scale": [1.0, 1.0, 1.0],
                "basis_engine_xyz": {
                    "width_axis": [1.0, 0.0, 0.0],
                    "height_axis": [0.0, 1.0, 0.0],
                    "depth_axis": [0.0, 0.0, 1.0],
                },
            }]

    parsed: list[dict] = []
    ids: set[str] = set()
    for index, instance in enumerate(instances):
        if not isinstance(instance, dict):
            raise InteriorBuildError(
                f"{fixture_id} instance {index} must be an object")
        instance_id = str(instance.get("id") or "")
        if not instance_id or instance_id in ids:
            raise InteriorBuildError(
                f"{fixture_id} instance ids must be non-empty and unique")
        ids.add(instance_id)
        position = fixture_vector(
            instance.get("position_m"), f"{fixture_id}.{instance_id}.position_m")
        scale = fixture_vector(
            instance.get("scale"), f"{fixture_id}.{instance_id}.scale")
        if min(scale) <= 0.0:
            raise InteriorBuildError(
                f"{fixture_id}.{instance_id} scale must be positive")
        basis = instance.get("basis_engine_xyz")
        if not isinstance(basis, dict):
            raise InteriorBuildError(
                f"{fixture_id}.{instance_id} has no placement basis")
        width_axis = fixture_vector(
            basis.get("width_axis"),
            f"{fixture_id}.{instance_id}.width_axis",
        )
        height_axis = fixture_vector(
            basis.get("height_axis"),
            f"{fixture_id}.{instance_id}.height_axis",
        )
        depth_axis = fixture_vector(
            basis.get("depth_axis"),
            f"{fixture_id}.{instance_id}.depth_axis",
        )
        axes = (width_axis, height_axis, depth_axis)
        if any(abs(axis.length - 1.0) > 1.0e-5 for axis in axes) or \
                any(abs(axes[left].dot(axes[right])) > 1.0e-5
                    for left, right in ((0, 1), (0, 2), (1, 2))):
            raise InteriorBuildError(
                f"{fixture_id}.{instance_id} placement basis is not orthonormal")
        determinant = width_axis.dot(height_axis.cross(depth_axis))
        if determinant < 1.0 - 1.0e-5:
            raise InteriorBuildError(
                f"{fixture_id}.{instance_id} placement basis reverses winding")
        parsed.append({
            "id": instance_id,
            "position": position,
            "scale": scale,
            "width_axis": width_axis,
            "height_axis": height_axis,
            "depth_axis": depth_axis,
        })
    return parsed, anchor


def transformed_fixture_point(instance: dict, local: Vector) -> Vector:
    scaled = Vector((
        local.x * instance["scale"].x,
        local.y * instance["scale"].y,
        local.z * instance["scale"].z,
    ))
    return (
        instance["position"] +
        instance["width_axis"] * scaled.x +
        instance["height_axis"] * scaled.y +
        instance["depth_axis"] * scaled.z
    )


def prepare_fixture_lods(
    fixture_id: str,
    source_path: Path,
    dimensions: dict,
    manifest: dict,
    *,
    expected_module: str,
    required_socket: str | None = None,
) -> tuple[list[dict], dict]:
    fixture = (dimensions.get("production_fixtures") or {}).get(fixture_id)
    if not isinstance(fixture, dict):
        raise InteriorBuildError(
            f"design authority has no {fixture_id!r} production fixture")
    source_forward = str(fixture.get("source_forward") or "")
    authored_forward = str(fixture.get("authored_forward") or "")
    if source_forward not in SUPPORTED_FIXTURE_FORWARD_AXES or \
            authored_forward not in SUPPORTED_FIXTURE_FORWARD_AXES:
        raise InteriorBuildError(
            f"{fixture_id} has an unsupported source/authored forward axis")
    if not source_path.is_file():
        raise InteriorBuildError(
            f"{fixture_id} source does not exist: {source_path}")

    module_id = str(fixture.get("module") or "")
    module_dimensions = {
        item["id"]: item for item in dimensions.get("modules", [])
    }
    sockets = {item["id"]: item for item in manifest.get("sockets", [])}
    if module_id != expected_module or module_id not in module_dimensions:
        raise InteriorBuildError(f"{fixture_id} module is invalid")
    socket_id = str(fixture.get("mount_socket") or "")
    if required_socket and (
            socket_id != required_socket or socket_id not in sockets or
            sockets[socket_id].get("module") != module_id):
        raise InteriorBuildError(f"{fixture_id} mount socket is invalid")

    width = float(fixture["width_x_m"])
    height = float(fixture["height_y_m"])
    depth = float(fixture["depth_z_m"])
    voxel_size = float(fixture["voxel_union_size_m"])
    max_components = int(fixture["maximum_repaired_components"])
    max_cleanup_faces = int(fixture.get("maximum_lod_cleanup_faces", 16))
    lod_targets = [int(value) for value in fixture["lod_triangle_targets"]]
    if min(width, height, depth, voxel_size) <= 0.0 or \
            min(max_components, max_cleanup_faces) <= 0 or \
            len(lod_targets) != 3 or any(value <= 0 for value in lod_targets) or \
            lod_targets != sorted(lod_targets, reverse=True):
        raise InteriorBuildError(
            f"{fixture_id} dimensions or LOD limits are invalid")

    clear_width, clear_height, clear_length = (
        float(value) for value in module_dimensions[module_id]["clear_size_m"])
    floor_y = -clear_height * 0.5
    instances, anchor = fixture_instances(
        fixture_id,
        fixture,
        sockets,
        floor_y=floor_y,
        required_socket=required_socket,
    )
    if anchor is not None:
        if anchor.y < floor_y or anchor.y > floor_y + height:
            raise InteriorBuildError(
                f"{fixture_id} socket is outside the fixture envelope")
    placement_tolerance = float(fixture.get("maximum_module_overhang_m", 0.0))
    if placement_tolerance < 0.0 or not math.isfinite(placement_tolerance):
        raise InteriorBuildError(
            f"{fixture_id} maximum module overhang is invalid")
    lower_limit = Vector((
        -clear_width * 0.5 - placement_tolerance,
        -clear_height * 0.5 - placement_tolerance,
        -clear_length * 0.5 - placement_tolerance,
    ))
    upper_limit = Vector((
        clear_width * 0.5 + placement_tolerance,
        clear_height * 0.5 + placement_tolerance,
        clear_length * 0.5 + placement_tolerance,
    ))
    source_corners = [
        Vector((x, y, z))
        for x in (-width * 0.5, width * 0.5)
        for y in (0.0, height)
        for z in (-depth * 0.5, depth * 0.5)
    ]
    for instance in instances:
        corners = [transformed_fixture_point(instance, corner)
                   for corner in source_corners]
        minimum = Vector(tuple(min(point[axis] for point in corners)
                               for axis in range(3)))
        maximum = Vector(tuple(max(point[axis] for point in corners)
                               for axis in range(3)))
        instance["bounds_min"] = minimum
        instance["bounds_max"] = maximum
        if any(minimum[axis] < lower_limit[axis] - 1.0e-5 or
               maximum[axis] > upper_limit[axis] + 1.0e-5
               for axis in range(3)):
            raise InteriorBuildError(
                f"{fixture_id}.{instance['id']} does not fit {module_id}: "
                f"{tuple(minimum)} to {tuple(maximum)}")

    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(source_path))
    imported = [obj for obj in bpy.data.objects if obj not in before]
    meshes = [obj for obj in imported if obj.type == "MESH"]
    if len(meshes) != 1:
        raise InteriorBuildError(
            f"{fixture_id} source must contain one mesh, found {len(meshes)}")
    source = meshes[0]
    label = "".join(part.title() for part in fixture_id.split("_"))
    source.name = f"TD_{label}_Source"
    source.data.name = f"TD_{label}_Source_Mesh"

    # glTF import may preserve its Y-up conversion in matrix_world even when
    # rotation_euler appears zero. Bake that basis before measuring any axis.
    world_basis = source.matrix_world.copy()
    source.parent = None
    source.data.transform(world_basis)
    source.matrix_world = Matrix.Identity(4)
    raw_geometry = COMMON.mesh_report(source)

    minimum, maximum = mesh_coordinate_bounds(source)
    source_size = maximum - minimum
    if min(source_size) <= 1.0e-8:
        raise InteriorBuildError(f"{fixture_id} source has a degenerate axis")
    scale = Vector((width / source_size.x,
                    depth / source_size.y,
                    height / source_size.z))
    center_x = (minimum.x + maximum.x) * 0.5
    center_y = (minimum.y + maximum.y) * 0.5
    for vertex in source.data.vertices:
        vertex.co.x = (vertex.co.x - center_x) * scale.x
        vertex.co.y = (vertex.co.y - center_y) * scale.y
        vertex.co.z = (vertex.co.z - minimum.z) * scale.z
    source.data.update()

    # Generated render sources commonly contain UV-seam fragments or open
    # surface soup. A bounded voxel union closes every retained component
    # before deterministic decimation. Gameplay collision remains authored.
    activate_object(source)
    source.data.remesh_voxel_size = voxel_size
    source.data.remesh_voxel_adaptivity = 0.0
    if "FINISHED" not in bpy.ops.object.voxel_remesh():
        raise InteriorBuildError(f"{fixture_id} voxel union failed")
    repaired_geometry = COMMON.mesh_report(source)
    if repaired_geometry["boundary_edges"] or \
            repaired_geometry["non_manifold_edges"] or \
            repaired_geometry["wire_edges"] or repaired_geometry["loose_vertices"] or \
            repaired_geometry["connected_components"] > max_components:
        raise InteriorBuildError(
            f"{fixture_id} voxel union failed topology limits: {repaired_geometry}")

    fixture_lods: list[dict] = []
    lod_reports: list[dict] = []
    for lod, target in enumerate(lod_targets):
        obj = source.copy()
        obj.data = source.data.copy()
        obj.name = f"TD_{label}_LOD{lod}"
        obj.data.name = f"TD_{label}_LOD{lod}_Mesh"
        bpy.context.scene.collection.objects.link(obj)
        current_triangles = triangle_count(obj)
        if current_triangles > target:
            modifier = obj.modifiers.new(f"TD_{label}_Decimate", "DECIMATE")
            modifier.decimate_type = "COLLAPSE"
            modifier.ratio = target / current_triangles
            modifier.use_collapse_triangulate = True
            activate_object(obj)
            bpy.ops.object.modifier_apply(modifier=modifier.name)
        triangulate = obj.modifiers.new(f"TD_{label}_Triangulate", "TRIANGULATE")
        activate_object(obj)
        bpy.ops.object.modifier_apply(modifier=triangulate.name)

        vertices_before_validation = len(obj.data.vertices)
        faces_before_validation = len(obj.data.polygons)
        obj.data.validate(clean_customdata=False)
        removed_degenerate_faces = faces_before_validation - len(obj.data.polygons)
        if len(obj.data.vertices) != vertices_before_validation or \
                removed_degenerate_faces < 0 or \
                removed_degenerate_faces > max_cleanup_faces:
            raise InteriorBuildError(
                f"{fixture_id} LOD{lod} required destructive mesh validation")

        mesh = bmesh.new()
        mesh.from_mesh(obj.data)
        boundary_edges = [edge for edge in mesh.edges if len(edge.link_faces) == 1]
        boundary_faces = {
            face for edge in boundary_edges for face in edge.link_faces
        }
        removed_open_fragment_faces = 0
        if boundary_edges:
            isolated = (
                len(boundary_edges) <= max_cleanup_faces * 3 and
                len(boundary_faces) <= max_cleanup_faces and
                all(len(face.verts) == 3 and
                    all(len(edge.link_faces) == 1 for edge in face.edges) and
                    all(all(linked in boundary_faces for linked in vertex.link_faces)
                        for vertex in face.verts)
                    for face in boundary_faces)
            )
            if not isolated:
                mesh.free()
                raise InteriorBuildError(
                    f"{fixture_id} LOD{lod} has a non-repairable open boundary")
            fragment_vertices = {
                vertex for face in boundary_faces for vertex in face.verts
            }
            removed_open_fragment_faces = len(boundary_faces)
            bmesh.ops.delete(
                mesh,
                geom=list(fragment_vertices),
                context="VERTS",
            )
            mesh.to_mesh(obj.data)
        mesh.free()
        obj.data.update()

        geometry = COMMON.mesh_report(obj)
        actual_triangles = triangle_count(obj)
        if geometry["boundary_edges"] or geometry["non_manifold_edges"] or \
                geometry["wire_edges"] or geometry["loose_vertices"] or \
                actual_triangles > target or actual_triangles < int(target * 0.8):
            raise InteriorBuildError(
                f"{fixture_id} LOD{lod} failed geometry limits: {geometry}")

        yaw_180 = source_forward != authored_forward
        planar_sign = -1.0 if yaw_180 else 1.0
        master_faces = [tuple(int(index) for index in polygon.vertices)
                        for polygon in obj.data.polygons]
        vertices: list[tuple[float, float, float]] = []
        faces: list[tuple[int, ...]] = []
        for instance in instances:
            start = len(vertices)
            for vertex in obj.data.vertices:
                # Normalized source coordinates are Blender X/Y/Z. Convert to
                # engine-local width/height/depth, apply the reviewed heading,
                # then place the fixture using its explicit design-authority basis.
                local = Vector((
                    planar_sign * float(vertex.co.x),
                    float(vertex.co.z),
                    planar_sign * float(vertex.co.y),
                ))
                placed = transformed_fixture_point(instance, local)
                vertices.append((placed.x, placed.z, placed.y))
            faces.extend(tuple(start + index for index in face)
                         for face in master_faces)
        fixture_lods.append({"vertices": vertices, "faces": faces})
        lod_reports.append({
            "lod": lod,
            "target_triangles": target,
            "instance_count": len(instances),
            "instanced_triangles": actual_triangles * len(instances),
            "removed_degenerate_faces": removed_degenerate_faces,
            "removed_open_fragment_faces": removed_open_fragment_faces,
            "geometry": geometry,
        })
        bpy.data.objects.remove(obj, do_unlink=True)

    for obj in imported:
        if obj.name in bpy.data.objects:
            bpy.data.objects.remove(obj, do_unlink=True)

    report = {
        "source": {
            "path": COMMON.report_path(source_path),
            "sha256": sha256(source_path),
            "geometry": raw_geometry,
        },
        "authored_envelope_m": {
            "width_x": width,
            "height_y": height,
            "depth_z": depth,
        },
        "module": module_id,
        "source_forward": source_forward,
        "authored_forward": authored_forward,
        "applied_yaw_degrees": 180.0 if source_forward != authored_forward else 0.0,
        "voxel_union_size_m": voxel_size,
        "maximum_lod_cleanup_faces": max_cleanup_faces,
        "maximum_module_overhang_m": placement_tolerance,
        "instances": [{
            "id": instance["id"],
            "position_m": list(instance["position"]),
            "scale": list(instance["scale"]),
            "basis_engine_xyz": {
                "width_axis": list(instance["width_axis"]),
                "height_axis": list(instance["height_axis"]),
                "depth_axis": list(instance["depth_axis"]),
            },
            "bounds_min_m": list(instance["bounds_min"]),
            "bounds_max_m": list(instance["bounds_max"]),
        } for instance in instances],
        "repaired_geometry": repaired_geometry,
        "lods": lod_reports,
        "runtime_material": str(fixture["runtime_material"]),
    }
    if len(instances) == 1:
        report["module_local_bottom_center_m"] = list(instances[0]["position"])
    if required_socket and anchor is not None:
        report["mount_socket"] = socket_id
        report["module_local_anchor_m"] = list(anchor)
        report["floor_y_m"] = floor_y
    return fixture_lods, report


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


def add_cockpit_functional_surfaces(builder: MeshBuilder) -> None:
    screen_z = 3.865
    builder.box((0.0, 0.22, screen_z),
                (1.95, 0.82, 0.032), PALETTE["display_glass"])
    for x in (-1.48, 1.48):
        builder.box((x, 0.18, screen_z),
                    (0.72, 0.70, 0.032), PALETTE["display_glass"])
    builder.box((0.0, 0.83, screen_z - 0.002),
                (3.75, 0.07, 0.036), PALETTE["indicator"])
    for x in (-1.65, -1.10, -0.55, 0.55, 1.10, 1.65):
        builder.box((x, -0.32, screen_z - 0.004),
                    (0.16, 0.035, 0.040), PALETTE["safety"])


def module_shell(
    module_id: str,
    clear_size: tuple[float, float, float],
    detail: int,
    portal_sockets: list[dict],
    cockpit_dressing_lod: dict[str, dict] | None = None,
) -> MeshBuilder:
    w, h, length = clear_size
    builder = MeshBuilder()

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

    if module_id == "cockpit_module":
        required = (
            "cockpit_wall_panel",
            "cockpit_deck_panel",
            "flight_station",
            "pilot_seat",
        )
        if not cockpit_dressing_lod or any(
                fixture_id not in cockpit_dressing_lod for fixture_id in required):
            raise InteriorBuildError(
                "cockpit module has incomplete production dressing")
        if any(side_openings.values()) or end_modes[-1] != "frame" or \
                end_modes[1] != "wall":
            raise InteriorBuildError(
                "cockpit generated shell requires one authored aft portal")

        # Generated architecture owns every visible room surface. The authored
        # aft frame remains as the exact pressure/portal boundary behind its
        # generated liner skin; gameplay collision is built independently.
        add_end_frame(builder, side=-1, w=w, h=h, length=length)
        add_cockpit_functional_surfaces(builder)
        for fixture_id, color in (
            ("cockpit_wall_panel", PALETTE["cockpit_wall"]),
            ("cockpit_deck_panel", PALETTE["cockpit_deck"]),
            ("flight_station", PALETTE["flight_station"]),
            ("pilot_seat", PALETTE["pilot_seat"]),
        ):
            dressing = cockpit_dressing_lod[fixture_id]
            builder.append_mesh(
                dressing["vertices"], dressing["faces"], color)
        return builder

    add_floor_and_ceiling(builder, w, h, length)

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
    pilot_seat_source_path: Path,
    flight_station_source_path: Path,
    cockpit_wall_source_path: Path,
    cockpit_deck_source_path: Path,
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
    pilot_seat_lods, pilot_seat_report = prepare_fixture_lods(
        "pilot_seat",
        pilot_seat_source_path,
        dimensions,
        manifest,
        expected_module="cockpit_module",
        required_socket="pilot_seat_anchor",
    )
    flight_station_lods, flight_station_report = prepare_fixture_lods(
        "flight_station",
        flight_station_source_path,
        dimensions,
        manifest,
        expected_module="cockpit_module",
    )
    cockpit_wall_lods, cockpit_wall_report = prepare_fixture_lods(
        "cockpit_wall_panel",
        cockpit_wall_source_path,
        dimensions,
        manifest,
        expected_module="cockpit_module",
    )
    cockpit_deck_lods, cockpit_deck_report = prepare_fixture_lods(
        "cockpit_deck_panel",
        cockpit_deck_source_path,
        dimensions,
        manifest,
        expected_module="cockpit_module",
    )
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
            cockpit_dressing = None
            if module_id == "cockpit_module":
                cockpit_dressing = {
                    "cockpit_wall_panel": cockpit_wall_lods[lod],
                    "cockpit_deck_panel": cockpit_deck_lods[lod],
                    "flight_station": flight_station_lods[lod],
                    "pilot_seat": pilot_seat_lods[lod],
                }
            builder = module_shell(
                module_id,
                size,
                detail,
                socket_by_module.get(module_id, []),
                cockpit_dressing,
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
        "production_dressing": {
            "pilot_seat": pilot_seat_report,
            "flight_station": flight_station_report,
            "cockpit_wall_panel": cockpit_wall_report,
            "cockpit_deck_panel": cockpit_deck_report,
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
    parser.add_argument("--pilot-seat-source", type=Path, required=True)
    parser.add_argument("--flight-station-source", type=Path, required=True)
    parser.add_argument("--cockpit-wall-source", type=Path, required=True)
    parser.add_argument("--cockpit-deck-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    report = build_interior(
        args.dimensions.resolve(),
        args.manifest.resolve(),
        args.pilot_seat_source.resolve(),
        args.flight_station_source.resolve(),
        args.cockpit_wall_source.resolve(),
        args.cockpit_deck_source.resolve(),
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
