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
import re
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

API_BASE = "https://api.meshy.ai"

# Meshy's own documented ceiling for both prompt fields.
MAX_PROMPT_CHARS = 600

# Refuse to start work that would leave the account effectively empty. Generation
# is two-stage and a preview with no credits left to refine it is wasted spend.
CREDIT_FLOOR = 50

AI_MODELS = ("latest", "meshy-6", "meshy-5")
SLUG_PATTERN = re.compile(r"^[a-z0-9]+(?:[._-][a-z0-9]+)*$")
PIPELINE_VERSION = 2


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
        if key:
            detail = detail.replace(key, "<redacted>")
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
    return request_hash(params)[:16]


def request_hash(params: dict) -> str:
    blob = json.dumps(params, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_generation_params(
    prompt: str,
    *,
    texture_prompt: str | None,
    ai_model: str,
    polycount: int,
    should_remesh: bool,
    decimation_mode: int | None,
    enable_pbr: bool,
    hd_texture: bool,
    remove_lighting: bool,
    auto_size: bool,
    origin_at: str,
    asset_id: str | None,
    module_id: str | None,
) -> dict:
    return {
        "prompt": prompt,
        "texture_prompt": texture_prompt or "",
        "ai_model": ai_model,
        "model_type": "standard",
        "should_remesh": should_remesh,
        "target_polycount": polycount if should_remesh and decimation_mode is None else None,
        "decimation_mode": decimation_mode if should_remesh else None,
        "enable_pbr": enable_pbr,
        "hd_texture": hd_texture,
        "remove_lighting": remove_lighting,
        "auto_size": auto_size,
        "origin_at": origin_at if auto_size else None,
        "target_formats": ["glb"],
        "asset_id": asset_id,
        "module_id": module_id,
        "pipeline_version": PIPELINE_VERSION,
    }


def build_preview_request(params: dict) -> dict:
    body = {
        "mode": "preview",
        "prompt": params["prompt"],
        "model_type": params["model_type"],
        "ai_model": params["ai_model"],
        "should_remesh": params["should_remesh"],
        "moderation": True,
        "target_formats": params["target_formats"],
        "alpha_thumbnail": True,
    }
    if params["target_polycount"] is not None:
        body["target_polycount"] = params["target_polycount"]
    if params["decimation_mode"] is not None:
        body["decimation_mode"] = params["decimation_mode"]
    if params["auto_size"]:
        body["auto_size"] = True
        body["origin_at"] = params["origin_at"]
    return body


def build_refine_request(params: dict, preview_task_id: str) -> dict:
    body = {
        "mode": "refine",
        "preview_task_id": preview_task_id,
        "ai_model": params["ai_model"],
        "enable_pbr": params["enable_pbr"],
        "hd_texture": params["hd_texture"],
        "remove_lighting": params["remove_lighting"],
        "moderation": True,
        "target_formats": params["target_formats"],
        "alpha_thumbnail": True,
    }
    if params["texture_prompt"]:
        body["texture_prompt"] = params["texture_prompt"]
    if params["auto_size"]:
        body["auto_size"] = True
        body["origin_at"] = params["origin_at"]
    return body


def estimated_credits(
    ai_model: str,
    preview_only: bool,
    preview_cached: bool = False,
) -> int:
    preview = 20 if ai_model in ("latest", "meshy-6") else 5
    if preview_cached:
        return 0 if preview_only else 10
    return preview if preview_only else preview + 10


def task_summary(task: dict) -> dict:
    return {
        key: task.get(key)
        for key in (
            "id", "type", "status", "progress", "created_at", "started_at",
            "finished_at", "consumed_credits"
        )
        if task.get(key) is not None
    }


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


def generate(key: str | None, out_root: Path, prompt: str, *, name: str,
             polycount: int, enable_pbr: bool, texture_prompt: str | None,
             ai_model: str, should_remesh: bool,
             decimation_mode: int | None, hd_texture: bool,
             remove_lighting: bool, auto_size: bool, origin_at: str,
             asset_id: str | None, module_id: str | None,
             preview_only: bool, dry_run: bool,
             max_credits: int, force: bool) -> Path:
    if len(prompt) > MAX_PROMPT_CHARS:
        raise MeshyError(f"prompt is {len(prompt)} chars, Meshy's limit is {MAX_PROMPT_CHARS}")
    if texture_prompt and len(texture_prompt) > MAX_PROMPT_CHARS:
        raise MeshyError(f"texture_prompt exceeds {MAX_PROMPT_CHARS} chars")
    if not SLUG_PATTERN.fullmatch(name):
        raise MeshyError("name must be a lowercase slug")
    if ai_model not in AI_MODELS:
        raise MeshyError(f"ai_model must be one of {AI_MODELS}")
    if should_remesh and not 100 <= polycount <= 300000:
        raise MeshyError("polycount must be between 100 and 300000")
    if decimation_mode is not None and decimation_mode not in range(1, 5):
        raise MeshyError("decimation_mode must be 1, 2, 3 or 4")
    if decimation_mode is not None and not should_remesh:
        raise MeshyError("decimation_mode requires --remesh")
    if (asset_id is None) != (module_id is None):
        raise MeshyError("--asset-id and --module-id must be supplied together")
    for label, identifier in (("asset_id", asset_id), ("module_id", module_id)):
        if identifier is not None and not SLUG_PATTERN.fullmatch(identifier):
            raise MeshyError(f"{label} must use lowercase slug/id characters")

    params = build_generation_params(
        prompt,
        texture_prompt=texture_prompt,
        ai_model=ai_model,
        polycount=polycount,
        should_remesh=should_remesh,
        decimation_mode=decimation_mode,
        enable_pbr=enable_pbr,
        hd_texture=hd_texture,
        remove_lighting=remove_lighting,
        auto_size=auto_size,
        origin_at=origin_at,
        asset_id=asset_id,
        module_id=module_id,
    )
    full_digest = request_hash(params)
    digest = cache_key(params)
    asset_dir = out_root / f"{name}_{digest}"
    manifest_path = asset_dir / "manifest.json"
    glb_path = asset_dir / "model.glb"
    preview_path = asset_dir / "preview.glb"
    thumbnail_path = asset_dir / "thumbnail.png"

    existing_manifest: dict = {}
    if manifest_path.is_file() and not force:
        try:
            existing_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            existing_manifest = {}
    preview_cached = bool(
        preview_path.is_file() and
        ((existing_manifest.get("tasks") or {}).get("preview") or {}).get("id")
    )
    final_cached = bool(manifest_path.is_file() and glb_path.is_file() and not force)
    projected = 0 if final_cached else estimated_credits(
        ai_model, preview_only, preview_cached)
    if projected > max_credits:
        raise MeshyError(
            f"projected cost {projected} exceeds --max-credits {max_credits}")

    preview_body = build_preview_request(params)
    if dry_run:
        print(json.dumps({
            "request_hash": full_digest,
            "output": str(asset_dir),
            "projected_credits": projected,
            "cache_state": "refined" if final_cached else
                ("preview" if preview_cached else "miss"),
            "preview_only": preview_only,
            "preview_request": preview_body,
            "refine_request_template": None if preview_only else
                build_refine_request(params, "<preview-task-id>"),
            "credential_required": False,
        }, indent=2))
        return asset_dir

    if final_cached:
        print(f"cached: {asset_dir} (no credits spent)")
        return asset_dir
    if preview_only and preview_cached:
        print(f"cached preview: {asset_dir} (no credits spent)")
        return asset_dir

    if not key:
        raise MeshyError("MESHY_API_KEY not set")

    balance = get_balance(key)
    print(f"balance: {balance} credits")
    if balance < CREDIT_FLOOR:
        raise MeshyError(
            f"balance {balance} is below the floor of {CREDIT_FLOOR}. Generation is "
            "two-stage; starting a preview that cannot be refined wastes the spend."
        )
    if balance < projected:
        raise MeshyError(
            f"balance {balance} is below projected task cost {projected}")

    asset_dir.mkdir(parents=True, exist_ok=True)

    preview_id = (
        (existing_manifest.get("tasks") or {}).get("preview") or {}
    ).get("id")
    preview: dict
    if preview_id and preview_path.is_file() and not force:
        print(f"reusing reviewed preview task {preview_id}")
        preview = (existing_manifest.get("tasks") or {}).get("preview") or {}
    else:
        print(f"preview: {prompt[:80]!r}")
        preview_id = _request(
            "POST", "/openapi/v2/text-to-3d", key, preview_body).get("result")
        if not preview_id:
            raise MeshyError("preview request returned no task id")
        preview = poll_task(key, preview_id, "preview")

        preview_url = (preview.get("model_urls") or {}).get("glb")
        if not preview_url:
            raise MeshyError("preview succeeded but returned no GLB url")
        size = download(preview_url, preview_path)
        print(f"wrote {preview_path} ({size // 1024} KiB)")
        thumbnail_url = preview.get("alpha_thumbnail_url") or preview.get("thumbnail_url")
        if thumbnail_url:
            download(thumbnail_url, thumbnail_path)

        preview_manifest = {
            "schema_version": 2,
            "provider": "Meshy",
            "endpoint": "/openapi/v2/text-to-3d",
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "request_hash": full_digest,
            "stage": "preview",
            "parameters": params,
            "tasks": {"preview": task_summary(preview)},
            "files": {
                "preview": preview_path.name,
                "preview_sha256": file_sha256(preview_path),
                **({
                    "thumbnail": thumbnail_path.name,
                    "thumbnail_sha256": file_sha256(thumbnail_path),
                } if thumbnail_path.is_file() else {}),
            },
            "consumed_credits": int(preview.get("consumed_credits", 0)),
        }
        manifest_path.write_text(
            json.dumps(preview_manifest, indent=2) + "\n", encoding="utf-8")

    if preview_only:
        print("preview ready for geometry, scale and silhouette review; rerun without "
              "--preview-only to texture the same cached preview")
        return asset_dir

    print("refine: adding textures")
    refine_body = build_refine_request(params, preview_id)
    refine_id = _request("POST", "/openapi/v2/text-to-3d", key, refine_body).get("result")
    if not refine_id:
        raise MeshyError("refine request returned no task id")
    task = poll_task(key, refine_id, "refine")

    glb_url = (task.get("model_urls") or {}).get("glb")
    if not glb_url:
        raise MeshyError("refined task succeeded but returned no GLB url")

    size = download(glb_url, glb_path)
    print(f"wrote {glb_path} ({size // 1024} KiB)")

    textures: dict[str, str] = {}
    for i, tex in enumerate(task.get("texture_urls") or []):
        for slot, url in (tex or {}).items():
            if not url:
                continue
            dest = asset_dir / f"{slot}_{i}.png"
            try:
                download(url, dest)
                textures[f"{slot}_{i}"] = dest.name
            except (MeshyError, OSError, urllib.error.URLError) as exc:
                print(f"  warning: texture {slot} failed: {exc}")

    spent = int(preview.get("consumed_credits", 0)) + int(task.get("consumed_credits", 0))
    manifest = {
        "schema_version": 2,
        "provider": "Meshy",
        "endpoint": "/openapi/v2/text-to-3d",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "request_hash": full_digest,
        "stage": "refined",
        "parameters": params,
        "tasks": {
            "preview": task_summary(preview),
            "refine": task_summary(task),
        },
        "files": {
            "model": glb_path.name,
            "model_sha256": file_sha256(glb_path),
            "preview": preview_path.name,
            "preview_sha256": file_sha256(preview_path),
            **({
                "thumbnail": thumbnail_path.name,
                "thumbnail_sha256": file_sha256(thumbnail_path),
            } if thumbnail_path.is_file() else {}),
            "textures": textures,
            "texture_sha256": {
                name: file_sha256(asset_dir / path)
                for name, path in textures.items()
            },
        },
        "consumed_credits": spent,
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

    g = sub.add_parser(
        "generate",
        help="Generate one text-to-3D source asset. COSTS CREDITS unless --dry-run.")
    g.add_argument("--prompt", required=True)
    g.add_argument("--name", required=True, help="Short slug used for the output directory")
    g.add_argument("--texture-prompt", default=None)
    g.add_argument("--polycount", type=int, default=30000)
    g.add_argument("--ai-model", choices=AI_MODELS, default="latest",
                   help="Meshy model; latest currently resolves to Meshy 6")
    g.add_argument("--remesh", action="store_true",
                   help="Ask Meshy to remesh. Otherwise preserve the highest-detail source mesh.")
    g.add_argument("--decimation-mode", type=int, choices=range(1, 5), default=None,
                   help="Adaptive remesh level 1=ultra through 4=low; requires --remesh")
    g.add_argument("--no-pbr", action="store_true",
                   help="Skip PBR maps. The engine consumes ORM and normal maps, so "
                        "this produces a less useful asset; present for cheap tests.")
    g.add_argument("--hd-texture", action="store_true",
                   help="Request a 4K base-color map for a reviewed hero asset")
    g.add_argument("--keep-lighting", action="store_true",
                   help="Keep baked highlights/shadows; production assets normally remove them")
    g.add_argument("--auto-size", action="store_true",
                   help="Request Meshy's estimated real scale; assembly metadata remains authoritative")
    g.add_argument("--origin-at", choices=("bottom", "center"), default="bottom")
    g.add_argument("--asset-id", default=None,
                   help="Assembly asset id to record in provenance; requires --module-id")
    g.add_argument("--module-id", default=None,
                   help="Assembly module id to record in provenance; requires --asset-id")
    g.add_argument("--preview-only", action="store_true",
                   help="Stop after untextured geometry for human review; rerun to refine")
    g.add_argument("--dry-run", action="store_true",
                   help="Print the content hash and exact redacted requests; costs zero and needs no key")
    g.add_argument("--max-credits", type=int, default=40,
                   help="Hard projected-cost ceiling for this invocation")
    g.add_argument("--force", action="store_true",
                   help="Regenerate even if cached. Spends credits again.")
    g.add_argument("--out", default=str(repo_root / "assets" / "generated"))

    args = ap.parse_args(argv)

    try:
        if args.cmd == "balance":
            key = load_api_key(repo_root)
            print(f"{get_balance(key)} credits")
            return 0
        if args.cmd == "generate":
            key = None if args.dry_run else load_api_key(repo_root)
            generate(
                key, Path(args.out), args.prompt,
                name=args.name,
                polycount=args.polycount,
                enable_pbr=not args.no_pbr,
                texture_prompt=args.texture_prompt,
                ai_model=args.ai_model,
                should_remesh=args.remesh,
                decimation_mode=args.decimation_mode,
                hd_texture=args.hd_texture,
                remove_lighting=not args.keep_lighting,
                auto_size=args.auto_size,
                origin_at=args.origin_at,
                asset_id=args.asset_id,
                module_id=args.module_id,
                preview_only=args.preview_only,
                dry_run=args.dry_run,
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
