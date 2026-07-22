#!/usr/bin/env python3
"""Render deterministic diagnostic views and topology statistics for a GLB.

This is an offline acceptance tool, not a production renderer. It deliberately
ignores materials and uses the authored geometry itself so a flattering vendor
thumbnail cannot hide disconnected fragments or a bad silhouette.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path


GLB_MAGIC = 0x46546C67
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
COMPONENT_FORMAT = {
    5121: ("B", 1),
    5123: ("H", 2),
    5125: ("I", 4),
    5126: ("f", 4),
}
TYPE_WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


class ReviewError(RuntimeError):
    pass


def read_glb(path: Path) -> tuple[dict, bytes]:
    data = path.read_bytes()
    if len(data) < 20:
        raise ReviewError("file is too small to be a GLB")
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != 2 or length != len(data):
        raise ReviewError("invalid GLB header")

    document = None
    binary = None
    offset = 12
    while offset + 8 <= len(data):
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk = data[offset:offset + chunk_length]
        offset += chunk_length
        if chunk_type == JSON_CHUNK:
            document = json.loads(chunk.rstrip(b" \t\r\n\0").decode("utf-8"))
        elif chunk_type == BIN_CHUNK:
            binary = chunk
    if document is None or binary is None:
        raise ReviewError("GLB must contain JSON and binary chunks")
    return document, binary


def read_accessor(document: dict, binary: bytes, accessor_index: int) -> list:
    accessor = document["accessors"][accessor_index]
    view = document["bufferViews"][accessor["bufferView"]]
    component_type = accessor["componentType"]
    if component_type not in COMPONENT_FORMAT:
        raise ReviewError(f"unsupported component type {component_type}")
    fmt, component_size = COMPONENT_FORMAT[component_type]
    width = TYPE_WIDTH[accessor["type"]]
    item_size = component_size * width
    stride = view.get("byteStride", item_size)
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    unpack = struct.Struct("<" + fmt * width).unpack_from
    values = []
    for index in range(accessor["count"]):
        value = unpack(binary, start + index * stride)
        values.append(value[0] if width == 1 else value)
    return values


def load_geometries(path: Path) -> list[dict]:
    document, binary = read_glb(path)
    meshes = document.get("meshes") or []
    if not meshes:
        raise ReviewError("GLB contains no meshes")
    geometries = []
    for mesh_index, mesh in enumerate(meshes):
        primitives = mesh.get("primitives") or []
        if not primitives:
            raise ReviewError(f"mesh {mesh_index} contains no primitives")
        for primitive_index, primitive in enumerate(primitives):
            if primitive.get("mode", 4) != 4:
                raise ReviewError("diagnostic renderer requires triangle topology")
            position_accessor = (primitive.get("attributes") or {}).get("POSITION")
            if position_accessor is None or "indices" not in primitive:
                raise ReviewError("primitive must contain POSITION and indices")
            positions = [tuple(map(float, value)) for value in read_accessor(
                document, binary, position_accessor)]
            indices = [int(value) for value in read_accessor(
                document, binary, primitive["indices"])]
            if len(indices) % 3:
                raise ReviewError("index count is not divisible by three")
            geometries.append({
                "mesh_index": mesh_index,
                "primitive_index": primitive_index,
                "mesh_name": str(mesh.get("name") or f"mesh_{mesh_index}"),
                "positions": positions,
                "indices": indices,
            })
    return geometries


def load_geometry(path: Path) -> tuple[list[tuple[float, float, float]], list[int]]:
    """Return the first primitive for compatibility with focused callers."""
    geometry = load_geometries(path)[0]
    return geometry["positions"], geometry["indices"]


def component_statistics(positions: list[tuple[float, float, float]],
                         indices: list[int],
                         weld_tolerance: float = 0.0) -> list[dict]:
    parent = list(range(len(positions)))
    rank = [0] * len(positions)

    def find(value: int) -> int:
        while parent[value] != value:
            parent[value] = parent[parent[value]]
            value = parent[value]
        return value

    def union(left: int, right: int) -> None:
        left = find(left)
        right = find(right)
        if left == right:
            return
        if rank[left] < rank[right]:
            left, right = right, left
        parent[right] = left
        if rank[left] == rank[right]:
            rank[left] += 1

    if weld_tolerance > 0.0:
        representatives: dict[tuple[int, int, int], int] = {}
        inverse = 1.0 / weld_tolerance
        for index, position in enumerate(positions):
            key = tuple(round(value * inverse) for value in position)
            representative = representatives.setdefault(key, index)
            union(index, representative)

    for offset in range(0, len(indices), 3):
        a, b, c = indices[offset:offset + 3]
        union(a, b)
        union(b, c)

    used = set(indices)
    vertices_by_root: dict[int, list[int]] = {}
    for vertex in used:
        vertices_by_root.setdefault(find(vertex), []).append(vertex)
    triangles_by_root: dict[int, int] = {root: 0 for root in vertices_by_root}
    for offset in range(0, len(indices), 3):
        triangles_by_root[find(indices[offset])] += 1

    components = []
    for root, vertices in vertices_by_root.items():
        points = [positions[index] for index in vertices]
        minimum = [min(point[axis] for point in points) for axis in range(3)]
        maximum = [max(point[axis] for point in points) for axis in range(3)]
        components.append({
            "vertices": len(vertices),
            "triangles": triangles_by_root[root],
            "bounds_min": [round(value, 6) for value in minimum],
            "bounds_max": [round(value, 6) for value in maximum],
        })
    components.sort(key=lambda item: item["triangles"], reverse=True)
    return components


def normalize(vector: tuple[float, float, float]) -> tuple[float, float, float]:
    length = math.sqrt(sum(value * value for value in vector))
    return tuple(value / length for value in vector) if length else (0.0, 0.0, 0.0)


def cross(a: tuple[float, float, float], b: tuple[float, float, float]) -> tuple[float, float, float]:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def render_view(positions: list[tuple[float, float, float]], indices: list[int],
                output: Path, right: tuple[float, float, float],
                up: tuple[float, float, float],
                toward_camera: tuple[float, float, float]) -> None:
    try:
        from PIL import Image, ImageDraw
    except ImportError as exc:
        raise ReviewError("Pillow is required to render diagnostic PNGs") from exc

    right = normalize(right)
    up = normalize(up)
    toward_camera = normalize(toward_camera)
    projected = [
        (
            sum(point[i] * right[i] for i in range(3)),
            sum(point[i] * up[i] for i in range(3)),
            sum(point[i] * toward_camera[i] for i in range(3)),
        )
        for point in positions
    ]
    x_values = [point[0] for point in projected]
    y_values = [point[1] for point in projected]
    span_x = max(x_values) - min(x_values)
    span_y = max(y_values) - min(y_values)
    scale = 700.0 / max(span_x, span_y, 1.0e-9)
    center_x = (min(x_values) + max(x_values)) * 0.5
    center_y = (min(y_values) + max(y_values)) * 0.5

    image = Image.new("RGB", (800, 800), (17, 20, 25))
    draw = ImageDraw.Draw(image)
    light = normalize((0.35, 0.8, 0.45))
    triangles = []
    for offset in range(0, len(indices), 3):
        ids = indices[offset:offset + 3]
        points = [positions[index] for index in ids]
        edge_a = tuple(points[1][axis] - points[0][axis] for axis in range(3))
        edge_b = tuple(points[2][axis] - points[0][axis] for axis in range(3))
        normal = normalize(cross(edge_a, edge_b))
        facing = sum(normal[axis] * toward_camera[axis] for axis in range(3))
        if facing <= 0.0:
            continue
        intensity = 0.24 + 0.76 * abs(sum(normal[axis] * light[axis] for axis in range(3)))
        shade = max(58, min(225, int(205 * intensity)))
        screen = [
            (
                int(400 + (projected[index][0] - center_x) * scale),
                int(400 - (projected[index][1] - center_y) * scale),
            )
            for index in ids
        ]
        depth = sum(projected[index][2] for index in ids) / 3.0
        triangles.append((depth, screen, (shade, shade, min(245, shade + 8))))
    triangles.sort(key=lambda item: item[0])
    for _, polygon, color in triangles:
        draw.polygon(polygon, fill=color)
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("glb", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--target-size",
        type=float,
        nargs=3,
        metavar=("X", "Y", "Z"),
        help="Normalize geometry to an authored X/Y/Z envelope before rendering",
    )
    parser.add_argument(
        "--weld-tolerance",
        type=float,
        default=1.0e-6,
        help="Position tolerance used to distinguish UV seams from geometric fragments",
    )
    args = parser.parse_args()
    if args.weld_tolerance < 0.0 or not math.isfinite(args.weld_tolerance):
        raise ReviewError("weld tolerance must be finite and non-negative")
    geometries = load_geometries(args.glb)
    positions = geometries[0]["positions"]
    indices = geometries[0]["indices"]
    raw_minimum = [min(point[axis] for point in positions) for axis in range(3)]
    raw_maximum = [max(point[axis] for point in positions) for axis in range(3)]
    raw_size = [raw_maximum[i] - raw_minimum[i] for i in range(3)]
    applied_scale = [1.0, 1.0, 1.0]
    if args.target_size:
        if any(value <= 0.0 for value in args.target_size):
            raise ReviewError("target size values must be positive")
        applied_scale = [args.target_size[i] / raw_size[i] for i in range(3)]
        center = [(raw_minimum[i] + raw_maximum[i]) * 0.5 for i in range(3)]
        for geometry in geometries:
            geometry["positions"] = [
                tuple((point[i] - center[i]) * applied_scale[i] for i in range(3))
                for point in geometry["positions"]
            ]
        positions = geometries[0]["positions"]
    indexed_components = component_statistics(positions, indices)
    components = component_statistics(
        positions, indices, weld_tolerance=args.weld_tolerance)
    minimum = [min(point[axis] for point in positions) for axis in range(3)]
    maximum = [max(point[axis] for point in positions) for axis in range(3)]
    report = {
        "source": str(args.glb),
        "primitive_count": len(geometries),
        "vertices": len(positions),
        "triangles": len(indices) // 3,
        "source_bounds_min": raw_minimum,
        "source_bounds_max": raw_maximum,
        "source_bounds_size": raw_size,
        "applied_scale": applied_scale,
        "review_bounds_min": minimum,
        "review_bounds_max": maximum,
        "review_bounds_size": [maximum[i] - minimum[i] for i in range(3)],
        "connected_component_summary": {
            "count": len(components),
            "largest_triangle_fraction": (
                components[0]["triangles"] / (len(indices) // 3)
                if components else 0.0
            ),
            "top_20_triangle_fraction": (
                sum(component["triangles"] for component in components[:20]) /
                (len(indices) // 3)
            ),
            "single_triangle_count": sum(
                component["triangles"] == 1 for component in components),
            "under_10_triangle_count": sum(
                component["triangles"] < 10 for component in components),
            "largest_components": components[:20],
        },
        "indexed_component_summary": {
            "count": len(indexed_components),
            "largest_triangle_fraction": (
                indexed_components[0]["triangles"] / (len(indices) // 3)
                if indexed_components else 0.0
            ),
            "single_triangle_count": sum(
                component["triangles"] == 1 for component in indexed_components),
            "under_10_triangle_count": sum(
                component["triangles"] < 10 for component in indexed_components),
        },
        "geometric_weld_tolerance": args.weld_tolerance,
        "primitives": [],
    }
    for geometry in geometries:
        primitive_positions = geometry["positions"]
        primitive_indices = geometry["indices"]
        primitive_indexed_components = component_statistics(
            primitive_positions, primitive_indices)
        primitive_components = component_statistics(
            primitive_positions,
            primitive_indices,
            weld_tolerance=args.weld_tolerance,
        )
        primitive_minimum = [
            min(point[axis] for point in primitive_positions) for axis in range(3)]
        primitive_maximum = [
            max(point[axis] for point in primitive_positions) for axis in range(3)]
        primitive_triangles = len(primitive_indices) // 3
        report["primitives"].append({
            "mesh_index": geometry["mesh_index"],
            "primitive_index": geometry["primitive_index"],
            "mesh_name": geometry["mesh_name"],
            "vertices": len(primitive_positions),
            "triangles": primitive_triangles,
            "bounds_min": primitive_minimum,
            "bounds_max": primitive_maximum,
            "bounds_size": [
                primitive_maximum[i] - primitive_minimum[i] for i in range(3)],
            "connected_component_summary": {
                "count": len(primitive_components),
                "largest_triangle_fraction": (
                    primitive_components[0]["triangles"] / primitive_triangles
                    if primitive_components and primitive_triangles else 0.0
                ),
                "single_triangle_count": sum(
                    component["triangles"] == 1
                    for component in primitive_components),
                "under_10_triangle_count": sum(
                    component["triangles"] < 10
                    for component in primitive_components),
                "largest_components": primitive_components[:20],
            },
            "indexed_component_summary": {
                "count": len(primitive_indexed_components),
                "largest_triangle_fraction": (
                    primitive_indexed_components[0]["triangles"] /
                    primitive_triangles
                    if primitive_indexed_components and primitive_triangles else 0.0
                ),
                "single_triangle_count": sum(
                    component["triangles"] == 1
                    for component in primitive_indexed_components),
                "under_10_triangle_count": sum(
                    component["triangles"] < 10
                    for component in primitive_indexed_components),
            },
        })
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "review.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    views = {
        "front": ((1, 0, 0), (0, 1, 0), (0, 0, 1)),
        "rear": ((-1, 0, 0), (0, 1, 0), (0, 0, -1)),
        "left": ((0, 0, 1), (0, 1, 0), (-1, 0, 0)),
        "top": ((1, 0, 0), (0, 0, 1), (0, 1, 0)),
        "three_quarter": (
            normalize((1, 0, -1)),
            normalize((-0.25, 1, -0.25)),
            normalize((1, 0.5, 1)),
        ),
    }
    for name, basis in views.items():
        render_view(positions, indices, args.out / f"{name}.png", *basis)
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
