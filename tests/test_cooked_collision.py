#!/usr/bin/env python3

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "compile_collision_asset.py"
SPEC = importlib.util.spec_from_file_location("compile_collision_asset", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
COMPILER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPILER)

INSPECTOR: Path | None = None


class CookedCollisionCompilerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.reference_path = (
            ROOT / "assets" / "collision_sources" / "reference_airlock.tdcollision.json"
        )
        cls.reference = json.loads(cls.reference_path.read_text(encoding="utf-8"))

    def test_compilation_is_canonical_and_byte_deterministic(self) -> None:
        first = COMPILER.compile_document(copy.deepcopy(self.reference))
        reordered = copy.deepcopy(self.reference)
        reordered["boxes"].reverse()
        second = COMPILER.compile_document(reordered)
        self.assertEqual(first, second)

    def test_header_authenticates_payload(self) -> None:
        cooked = COMPILER.compile_document(copy.deepcopy(self.reference))
        (
            magic,
            version,
            header_bytes,
            file_bytes,
            payload_bytes,
            payload_sha,
            reserved,
        ) = COMPILER.HEADER_STRUCT.unpack_from(cooked)
        payload = cooked[header_bytes:]
        self.assertEqual(magic, COMPILER.MAGIC)
        self.assertEqual(version, COMPILER.FORMAT_VERSION)
        self.assertEqual(file_bytes, len(cooked))
        self.assertEqual(payload_bytes, len(payload))
        self.assertEqual(payload_sha, hashlib.sha256(payload).digest())
        self.assertEqual(reserved, 0)

    def test_invalid_geometry_never_produces_runtime_bytes(self) -> None:
        invalid = copy.deepcopy(self.reference)
        invalid["boxes"][0]["half_extents_m"][1] = 0.0
        with self.assertRaisesRegex(COMPILER.CollisionCompileError, "positive"):
            COMPILER.compile_document(invalid)

        duplicate = copy.deepcopy(self.reference)
        duplicate["boxes"][1]["id"] = duplicate["boxes"][0]["id"]
        with self.assertRaisesRegex(COMPILER.CollisionCompileError, "unique"):
            COMPILER.compile_document(duplicate)

        unknown = copy.deepcopy(self.reference)
        unknown["boxes"][0]["material"] = "steel"
        with self.assertRaisesRegex(COMPILER.CollisionCompileError, "unknown"):
            COMPILER.compile_document(unknown)

    def test_compile_file_is_atomic_and_rejects_alias(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "room.tdcollision.json"
            output = root / "room.tdcollision"
            source_bytes = json.dumps(self.reference).encode("utf-8")
            source.write_bytes(source_bytes)
            byte_count = COMPILER.compile_file(source, output)
            self.assertEqual(byte_count, output.stat().st_size)
            self.assertEqual(output.read_bytes(), COMPILER.compile_document(self.reference))
            self.assertEqual(list(root.glob("*.tmp")), [])
            with self.assertRaises(COMPILER.CollisionCompileError):
                COMPILER.compile_file(source, source)
            self.assertEqual(source.read_bytes(), source_bytes)

    def test_cpp_loader_consumes_compiler_output_and_rejects_corruption(self) -> None:
        if INSPECTOR is None:
            self.skipTest("cross-language inspector is supplied by CTest")
        assert INSPECTOR is not None
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "reference_airlock.tdcollision"
            COMPILER.compile_file(self.reference_path, output)
            inspected = subprocess.run(
                [str(INSPECTOR), str(output)],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(inspected.returncode, 0, inspected.stderr)
            self.assertIn("collision_id=reference.airlock", inspected.stdout)
            self.assertIn("boxes=4", inspected.stdout)
            self.assertIn("walkable=1", inspected.stdout)

            corrupted = bytearray(output.read_bytes())
            corrupted[-1] ^= 1
            output.write_bytes(corrupted)
            rejected = subprocess.run(
                [str(INSPECTOR), str(output)],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("integrity_mismatch", rejected.stderr)


def main(argv: list[str]) -> int:
    global INSPECTOR
    import argparse

    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--inspector", type=Path)
    args, remaining = parser.parse_known_args(argv)
    INSPECTOR = args.inspector
    program = unittest.main(argv=[sys.argv[0], *remaining], exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
