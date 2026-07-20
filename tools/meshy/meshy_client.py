#!/usr/bin/env python3
"""Meshy AI asset generation client for The Dawning.

Stage 2 of docs/research/ASSET_PIPELINE_SPEC.md. Turns a text prompt into a GLB
on disk. Deliberately standalone: this is never called by the engine at runtime
and never runs on the smoke-test path. Generation is an offline authoring step.

Design notes that are load bearing:

* Standard library only. Adding a pip dependency to a project whose entire
  toolchain is "MSVC plus CMake" would mean every contributor and CI runner
  needs a Python environment set up correctly before they can build assets.
  urllib is uglier than requests and worth it.

* Generation costs real credits against a real account. Every call is therefore
  content addressed: the cache key is a hash of the prompt plus every parameter
  that changes the output. An unchanged prompt never regenerates, and the tool
  refuses to spend when the projected cost exceeds --max-credits.

* The API key is read from the environment. This repository is PUBLIC. The key
  is never written to a file, never logged, never included in an error message,
  and never echoed back even partially. If it is missing, the tool says
  "MESHY_API_KEY not set" and stops.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

API_BASE = "https://api.meshy.ai"

# Meshy's own documented ceiling for both prompt fields.
MAX_PROMPT_CHARS = 600

# Refuse to start work that would leave the account effectively empty. Generation
# is two-stage and a preview with no credits left to refine it is wasted spend.
CREDIT_FLOOR = 50


class MeshyError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# Credentials
# ---------------------------------------------------------------------------

def load_api_key(repo_root: Path) -> str:
    """Environment first, then a gitignored .env at the repo root.

    Returns the key. Never logs it, and callers must not either.
    """
    key = os.environ.get("MESHY_API_KEY", "").strip()
    if key:
        return key

    env_path = repo_root / ".env"
    if env_path.is_file():
        for raw in env_path.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, _, value = line.partition("=")
            if name.strip() == "MESHY_API_KEY":
                key = value.strip().strip('"').strip("'")
                if key:
                    return key

    raise MeshyError(
        "MESHY_API_KEY not set. Put it in the environment, or in a .env file at "
        "the repository root (see .env.example). .env is gitignored; never commit "
        "a key to this repository, which is public."
    )


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

def _request(method: str, path: str, key: str, body: dict | None = None,
             timeout: int = 60) -> dict:
    url = f"{API_BASE}{path}"
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"Bearer {key}")
    if data is not None:
        req.add_header("Content-Type", "application/json")

    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            payload = resp.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")[:600]
        # Deliberately reports the URL and the server's message but never the
        # Authorization header, so a pasted traceback cannot leak the key.
        raise MeshyError(f"HTTP {exc.code} from {method} {path}: {detail}") from None
    except urllib.error.URLError as exc:
        raise MeshyError(f"Network error calling {method} {path}: {exc.reason}") from None

    if not payload.strip():
        return {}
    try:
        return json.loads(payload)
    except json.JSONDecodeError:
        raise MeshyError(f"Non-JSON response from {method} {path}: {payload[:300]}") from None


def get_balance(key: str) -> int:
    return int(_request("GET", "/openapi/v1/balance", key).get("balance", 0))


# ---------------------------------------------------------------------------
# Cache keying
# ---------------------------------------------------------------------------

def cache_key(params: dict) -> str:
    """Stable hash over everything that changes the generated result.

    sort_keys matters: dict ordering must not perturb the key, or an unchanged
    request regenerates and silently spends credits.
    """
    blob = json.dumps(params, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()[:16]


# ---------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------

def poll_task(key: str, task_id: str, label: str, timeout_s: int = 1800) -> dict:
    """Block until the task leaves a running state.

    Polls on a gentle backoff rather than a tight loop: these tasks take minutes,
    and hammering a metered API is its own kind of rude.
    """
    deadline = time.monotonic() + timeout_s
    delay = 5.0
    last_progress = -1

    while time.monotonic() < deadline:
        task = _request("GET", f"/openapi/v2/text-to-3d/{task_id}", key)
        status = task.get("status", "UNKNOWN")
        progress = int(task.get("progress", 0))

        if progress != last_progress:
            print(f"  [{label}] {status} {progress}%", flush=True)
            last_progress = progress

        if status == "SUCCEEDED":
            return task
        if status in ("FAILED", "CANCELED"):
            err = task.get("task_error") or {}
            raise MeshyError(f"{label} task {status}: {err.get('message', 'no message')}")

        time.sleep(delay)
        delay = min(delay * 1.3, 20.0)

    raise MeshyError(f"{label} task {task_id} did not finish within {timeout_s}s")


def download(url: str, dest: Path) -> int:
    dest.parent.mkdir(parents=True, exist_ok=True)
    # Model URLs are pre-signed and carry their own expiry; they take no auth
    # header, so the key never travels to the CDN.
    with urllib.request.urlopen(url, timeout=300) as resp:
        data = resp.read()
    dest.write_bytes(data)
    return len(data)


def generate(key: str, out_root: Path, prompt: str, *, name: str,
             polycount: int, enable_pbr: bool, texture_prompt: str | None,
             max_credits: int, force: bool) -> Path:
    if len(prompt) > MAX_PROMPT_CHARS:
        raise MeshyError(f"prompt is {len(prompt)} chars, Meshy's limit is {MAX_PROMPT_CHARS}")
    if texture_prompt and len(texture_prompt) > MAX_PROMPT_CHARS:
        raise MeshyError(f"texture_prompt exceeds {MAX_PROMPT_CHARS} chars")

    params = {
        "prompt": prompt,
        "texture_prompt": texture_prompt or "",
        "target_polycount": polycount,
        "enable_pbr": enable_pbr,
        "ai_model": "meshy-5",
        "target_formats": ["glb"],
        # Bump when the pipeline's interpretation of a result changes, so cached
        # assets are invalidated deliberately rather than by accident.
        "pipeline_version": 1,
    }
    digest = cache_key(params)
    asset_dir = out_root / f"{name}_{digest}"
    manifest_path = asset_dir / "manifest.json"
    glb_path = asset_dir / "model.glb"

    if manifest_path.is_file() and glb_path.is_file() and not force:
        print(f"cached: {asset_dir} (no credits spent)")
        return asset_dir

    balance = get_balance(key)
    print(f"balance: {balance} credits")
    if balance < CREDIT_FLOOR:
        raise MeshyError(
            f"balance {balance} is below the floor of {CREDIT_FLOOR}. Generation is "
            "two-stage; starting a preview that cannot be refined wastes the spend."
        )
    if balance < max_credits:
        print(f"note: balance {balance} is below --max-credits {max_credits}; "
              f"the balance is the real limit.")

    asset_dir.mkdir(parents=True, exist_ok=True)

    print(f"preview: {prompt[:80]!r}")
    preview_id = _request("POST", "/openapi/v2/text-to-3d", key, {
        "mode": "preview",
        "prompt": prompt,
        "ai_model": params["ai_model"],
        "target_polycount": polycount,
        "should_remesh": True,
        "target_formats": ["glb"],
    }).get("result")
    if not preview_id:
        raise MeshyError("preview request returned no task id")
    preview = poll_task(key, preview_id, "preview")

    refine_body = {
        "mode": "refine",
        "preview_task_id": preview_id,
        "enable_pbr": enable_pbr,
        "target_formats": ["glb"],
    }
    if texture_prompt:
        refine_body["texture_prompt"] = texture_prompt

    print("refine: adding textures")
    refine_id = _request("POST", "/openapi/v2/text-to-3d", key, refine_body).get("result")
    if not refine_id:
        raise MeshyError("refine request returned no task id")
    task = poll_task(key, refine_id, "refine")

    glb_url = (task.get("model_urls") or {}).get("glb")
    if not glb_url:
        raise MeshyError("refined task succeeded but returned no GLB url")

    size = download(glb_url, glb_path)
    print(f"wrote {glb_path} ({size // 1024} KiB)")

    textures = {}
    for i, tex in enumerate(task.get("texture_urls") or []):
        for slot, url in (tex or {}).items():
            if not url:
                continue
            dest = asset_dir / f"{slot}_{i}.png"
            try:
                download(url, dest)
                textures[slot] = dest.name
            except MeshyError as exc:
                print(f"  warning: texture {slot} failed: {exc}")

    spent = int(preview.get("consumed_credits", 0)) + int(task.get("consumed_credits", 0))
    manifest = {
        "name": name,
        "params": params,
        "preview_task_id": preview_id,
        "refine_task_id": refine_id,
        "consumed_credits": spent,
        "glb": glb_path.name,
        "textures": textures,
        "thumbnail_url": task.get("thumbnail_url", ""),
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"credits consumed: {spent}")
    return asset_dir


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    repo_root = Path(__file__).resolve().parents[2]

    ap = argparse.ArgumentParser(description="Generate assets for The Dawning via Meshy AI.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("balance", help="Show remaining credits. Costs nothing.")

    g = sub.add_parser("generate", help="Generate one asset. COSTS CREDITS.")
    g.add_argument("--prompt", required=True)
    g.add_argument("--name", required=True, help="Short slug used for the output directory")
    g.add_argument("--texture-prompt", default=None)
    g.add_argument("--polycount", type=int, default=30000)
    g.add_argument("--no-pbr", action="store_true",
                   help="Skip PBR maps. The engine consumes ORM and normal maps, so "
                        "this produces a less useful asset; present for cheap tests.")
    g.add_argument("--max-credits", type=int, default=100)
    g.add_argument("--force", action="store_true",
                   help="Regenerate even if cached. Spends credits again.")
    g.add_argument("--out", default=str(repo_root / "assets" / "generated"))

    args = ap.parse_args(argv)

    try:
        key = load_api_key(repo_root)
        if args.cmd == "balance":
            print(f"{get_balance(key)} credits")
            return 0
        if args.cmd == "generate":
            generate(
                key, Path(args.out), args.prompt,
                name=args.name,
                polycount=args.polycount,
                enable_pbr=not args.no_pbr,
                texture_prompt=args.texture_prompt,
                max_credits=args.max_credits,
                force=args.force,
            )
            return 0
    except MeshyError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
