#!/usr/bin/env python3

from __future__ import annotations

import argparse
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
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
MODULE_PATH = TOOLS / "compile_asset_manifest.py"
SPEC = importlib.util.spec_from_file_location("compile_asset_manifest", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
COMPILER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPILER)

INSPECTOR: Path | None = None


class CookedAssemblyCompilerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.reference_path = (
            ROOT / "assets" / "manifests" / "reference_ship.tdasset.json"
        )
        cls.reference = json.loads(cls.reference_path.read_text(encoding="utf-8"))

    def test_compilation_is_canonical_and_byte_deterministic(self) -> None:
        first = COMPILER.compile_document(copy.deepcopy(self.reference))
        reordered = copy.deepcopy(self.reference)
        for key in (
            "modules",
            "sockets",
            "zones",
            "portals",
            "interactions",
            "moving_parts",
        ):
            reordered[key].reverse()
        reordered["provenance"]["sources"].reverse()
        reordered["navigation"]["required_reachable_zones"].reverse()
        second = COMPILER.compile_document(reordered)
        self.assertEqual(first, second)

    def test_header_authenticates_payload_and_omits_source_paths(self) -> None:
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
        self.assertNotIn(b"assets/generated", cooked)
        self.assertNotIn(b"manifest.json", cooked)
        self.assertNotIn(b"https://", cooked)

    def test_invalid_source_never_produces_runtime_bytes(self) -> None:
        invalid = copy.deepcopy(self.reference)
        invalid["portals"] = invalid["portals"][:1]
        with self.assertRaises(COMPILER.AssemblyCompileError):
            COMPILER.compile_document(invalid)

    def test_runtime_socket_category_is_checked_before_compilation(self) -> None:
        invalid = copy.deepcopy(self.reference)
        invalid["interactions"][2]["socket"] = "cockpit_entry"
        with self.assertRaisesRegex(
            COMPILER.AssemblyCompileError,
            "requires a interaction socket",
        ):
            COMPILER.compile_document(invalid)

    def test_runtime_locator_control_characters_are_rejected(self) -> None:
        invalid = copy.deepcopy(self.reference)
        invalid["modules"][0]["visual_source"] = "visual://hull\u0000hidden"
        with self.assertRaisesRegex(
            COMPILER.AssemblyCompileError,
            "unsafe control character",
        ):
            COMPILER.compile_document(invalid)

    def test_compile_file_publishes_complete_output_without_temp_residue(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "ship.tdasset.json"
            output = root / "ship.tdassembly"
            source.write_text(json.dumps(self.reference), encoding="utf-8")
            byte_count = COMPILER.compile_file(source, output)
            self.assertEqual(byte_count, output.stat().st_size)
            self.assertEqual(output.read_bytes(), COMPILER.compile_document(self.reference))
            self.assertEqual(list(root.glob("*.tmp")), [])

    def test_output_alias_is_rejected_without_modifying_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "ship.tdasset.json"
            source_bytes = json.dumps(self.reference).encode("utf-8")
            source.write_bytes(source_bytes)
            with self.assertRaises(COMPILER.AssemblyCompileError):
                COMPILER.compile_file(source, source)
            self.assertEqual(source.read_bytes(), source_bytes)

    def test_cpp_loader_consumes_python_compiler_output_and_rejects_corruption(self) -> None:
        if INSPECTOR is None:
            self.skipTest("cross-language inspector is supplied by CTest")
        assert INSPECTOR is not None
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "reference_ship.tdassembly"
            COMPILER.compile_file(self.reference_path, output)
            inspected = subprocess.run(
                [str(INSPECTOR), str(output)],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(inspected.returncode, 0, inspected.stderr)
            self.assertIn("asset_id=ship.reference.fighter", inspected.stdout)
            self.assertIn("modules=3", inspected.stdout)
            self.assertIn("zones=2", inspected.stdout)
            self.assertIn("interactions=3", inspected.stdout)
            self.assertIn("entry_zone=airlock", inspected.stdout)

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
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--inspector", type=Path)
    args, remaining = parser.parse_known_args(argv)
    INSPECTOR = args.inspector
    program = unittest.main(argv=[sys.argv[0], *remaining], exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
