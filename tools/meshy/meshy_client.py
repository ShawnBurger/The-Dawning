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
import base64
import hashlib
import json
import os
import re
import shutil
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
IMAGE_MODELS = ("nano-banana", "nano-banana-2", "nano-banana-pro", "gpt-image-2")
IMAGE_MODEL_CREDITS = {
    "nano-banana": 3,
    "nano-banana-2": 6,
    "nano-banana-pro": 9,
    "gpt-image-2": 9,
}
SLUG_PATTERN = re.compile(r"^[a-z0-9]+(?:[._-][a-z0-9]+)*$")
PIPELINE_VERSION = 2
MULTIVIEW_PIPELINE_VERSION = 2
REMESH_PIPELINE_VERSION = 1
RETEXTURE_PIPELINE_VERSION = 1
SUPPORTED_IMAGE_MIME = {
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".png": "image/png",
}


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
        decoded = json.loads(payload)
    except json.JSONDecodeError:
        detail = payload[:300].replace(key, "<redacted>") if key else payload[:300]
        raise MeshyError(f"Non-JSON response from {method} {path}: {detail}") from None
    if not isinstance(decoded, dict):
        raise MeshyError(f"Non-object JSON response from {method} {path}")
    return decoded


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


def load_cached_manifest(path: Path) -> dict:
    if not path.is_file():
        return {}
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return {}
    return manifest if isinstance(manifest, dict) else {}


def manifest_matches_request(manifest: dict, expected_hash: str) -> bool:
    params = manifest.get("parameters")
    return bool(
        isinstance(params, dict)
        and manifest.get("request_hash") == expected_hash
        and request_hash(params) == expected_hash
    )


def task_record(manifest: dict, label: str) -> dict:
    tasks = manifest.get("tasks")
    if not isinstance(tasks, dict):
        return {}
    task = tasks.get(label)
    return task if isinstance(task, dict) else {}


def completed_task(manifest: dict, label: str) -> bool:
    task = task_record(manifest, label)
    return bool(task.get("id") and task.get("status") == "SUCCEEDED")


def cached_file_matches(
    manifest: dict,
    asset_dir: Path,
    *,
    file_key: str,
    hash_key: str,
    expected_name: str,
) -> bool:
    files = manifest.get("files")
    if not isinstance(files, dict):
        return False
    expected_hash = files.get(hash_key)
    path = asset_dir / expected_name
    return bool(
        files.get(file_key) == expected_name
        and isinstance(expected_hash, str)
        and re.fullmatch(r"[0-9a-f]{64}", expected_hash)
        and path.is_file()
        and file_sha256(path) == expected_hash
    )


def cached_file_list_matches(
    manifest: dict,
    asset_dir: Path,
    *,
    file_key: str,
    hash_key: str,
    expected_names: list[str],
) -> bool:
    files = manifest.get("files")
    if not isinstance(files, dict):
        return False
    hashes = files.get(hash_key)
    if files.get(file_key) != expected_names or not isinstance(hashes, dict):
        return False
    return all(
        isinstance(hashes.get(name), str)
        and re.fullmatch(r"[0-9a-f]{64}", hashes[name])
        and (asset_dir / name).is_file()
        and file_sha256(asset_dir / name) == hashes[name]
        for name in expected_names
    )


def describe_input_images(paths: list[Path]) -> list[dict]:
    if not 1 <= len(paths) <= 4:
        raise MeshyError("--input-image requires between 1 and 4 images")

    descriptions: list[dict] = []
    for path in paths:
        suffix = path.suffix.lower()
        mime_type = SUPPORTED_IMAGE_MIME.get(suffix)
        if mime_type is None:
            raise MeshyError(f"unsupported input image format: {path.name}")
        if not path.is_file():
            raise MeshyError(f"input image not found: {path}")
        byte_count = path.stat().st_size
        if byte_count == 0:
            raise MeshyError(f"input image is empty: {path}")
        descriptions.append({
            "name": path.name,
            "mime_type": mime_type,
            "byte_count": byte_count,
            "sha256": file_sha256(path),
        })
    return descriptions


def input_image_data_uris(paths: list[Path]) -> list[str]:
    descriptions = describe_input_images(paths)
    return [
        f"data:{description['mime_type']};base64,"
        f"{base64.b64encode(path.read_bytes()).decode('ascii')}"
        for path, description in zip(paths, descriptions)
    ]


def describe_input_model(path: Path) -> dict:
    if path.suffix.lower() != ".glb":
        raise MeshyError("production model input requires a binary .glb")
    if not path.is_file():
        raise MeshyError(f"input model not found: {path}")
    byte_count = path.stat().st_size
    if byte_count == 0:
        raise MeshyError(f"input model is empty: {path}")
    return {
        "name": path.name,
        "mime_type": "application/octet-stream",
        "byte_count": byte_count,
        "sha256": file_sha256(path),
    }


def input_model_data_uri(path: Path) -> str:
    description = describe_input_model(path)
    encoded = base64.b64encode(path.read_bytes()).decode("ascii")
    return f"data:{description['mime_type']};base64,{encoded}"


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


def build_concept_params(
    prompt: str,
    *,
    image_model: str,
    pose_mode: str | None,
    asset_id: str,
    module_id: str,
) -> dict:
    return {
        "prompt": prompt,
        "image_model": image_model,
        "generate_multi_view": True,
        "pose_mode": pose_mode,
        "asset_id": asset_id,
        "module_id": module_id,
        "pipeline_version": MULTIVIEW_PIPELINE_VERSION,
    }


def build_concept_request(params: dict) -> dict:
    body = {
        "ai_model": params["image_model"],
        "prompt": params["prompt"],
        "generate_multi_view": True,
    }
    if params["pose_mode"]:
        body["pose_mode"] = params["pose_mode"]
    return body


def estimated_concept_credits(image_model: str) -> int:
    try:
        return IMAGE_MODEL_CREDITS[image_model]
    except KeyError:
        raise MeshyError(f"image_model must be one of {IMAGE_MODELS}") from None


def build_multiview_params(
    concept_manifest: dict,
    *,
    input_images: list[dict] | None = None,
    ai_model: str,
    polycount: int,
    should_remesh: bool,
    decimation_mode: int | None,
    enable_pbr: bool,
    hd_texture: bool,
    remove_lighting: bool,
    texture_prompt: str | None,
    auto_size: bool,
    origin_at: str,
) -> dict:
    concept_params = concept_manifest.get("parameters") or {}
    concept_task = ((concept_manifest.get("tasks") or {}).get("concept") or {})
    input_mode = "retained_images" if input_images else "concept_task"
    return {
        "concept_request_hash": concept_manifest.get("request_hash"),
        "input_mode": input_mode,
        "input_task_id": concept_task.get("id") if input_mode == "concept_task" else None,
        "input_images": input_images or [],
        "asset_id": concept_params.get("asset_id"),
        "module_id": concept_params.get("module_id"),
        "ai_model": ai_model,
        "should_texture": True,
        "enable_pbr": enable_pbr,
        "hd_texture": hd_texture,
        "texture_prompt": texture_prompt or "",
        "should_remesh": should_remesh,
        "topology": "triangle",
        "target_polycount": polycount if should_remesh and decimation_mode is None else None,
        "decimation_mode": decimation_mode if should_remesh else None,
        "save_pre_remeshed_model": should_remesh,
        "image_enhancement": False,
        "remove_lighting": remove_lighting,
        "auto_size": auto_size,
        "origin_at": origin_at if auto_size else None,
        "target_formats": ["glb"],
        "pipeline_version": MULTIVIEW_PIPELINE_VERSION,
    }


def build_multiview_request(params: dict, image_urls: list[str] | None = None) -> dict:
    body = {
        "ai_model": params["ai_model"],
        "should_texture": params["should_texture"],
        "enable_pbr": params["enable_pbr"],
        "hd_texture": params["hd_texture"],
        "should_remesh": params["should_remesh"],
        "image_enhancement": params["image_enhancement"],
        "remove_lighting": params["remove_lighting"],
        "moderation": True,
        "target_formats": params["target_formats"],
        "alpha_thumbnail": True,
    }
    if params["input_mode"] == "retained_images":
        expected = len(params["input_images"])
        if image_urls is None or len(image_urls) != expected:
            raise MeshyError(f"retained image request requires exactly {expected} data URIs")
        body["image_urls"] = image_urls
    else:
        body["input_task_id"] = params["input_task_id"]
    if params["texture_prompt"]:
        body["texture_prompt"] = params["texture_prompt"]
    if params["should_remesh"]:
        body["topology"] = params["topology"]
        body["save_pre_remeshed_model"] = params["save_pre_remeshed_model"]
    if params["target_polycount"] is not None:
        body["target_polycount"] = params["target_polycount"]
    if params["decimation_mode"] is not None:
        body["decimation_mode"] = params["decimation_mode"]
    if params["auto_size"]:
        body["auto_size"] = True
        body["origin_at"] = params["origin_at"]
    return body


def redact_multiview_request(body: dict, params: dict) -> dict:
    redacted = dict(body)
    if "image_urls" in redacted:
        redacted["image_urls"] = [
            f"<data:{image['mime_type']};base64;sha256={image['sha256']}>"
            for image in params["input_images"]
        ]
    return redacted


def estimated_multiview_credits(ai_model: str) -> int:
    if ai_model in ("latest", "meshy-6"):
        return 30
    if ai_model == "meshy-5":
        return 15
    raise MeshyError(f"ai_model must be one of {AI_MODELS}")


def build_retexture_params(
    input_model: dict,
    *,
    text_style_prompt: str,
    ai_model: str,
    enable_original_uv: bool,
    enable_pbr: bool,
    hd_texture: bool,
    remove_lighting: bool,
    asset_id: str,
    module_id: str,
) -> dict:
    return {
        "input_model": input_model,
        "text_style_prompt": text_style_prompt,
        "ai_model": ai_model,
        "enable_original_uv": enable_original_uv,
        "enable_pbr": enable_pbr,
        "hd_texture": hd_texture,
        "remove_lighting": remove_lighting,
        "target_formats": ["glb"],
        "asset_id": asset_id,
        "module_id": module_id,
        "pipeline_version": RETEXTURE_PIPELINE_VERSION,
    }


def build_retexture_request(params: dict, model_url: str) -> dict:
    return {
        "model_url": model_url,
        "text_style_prompt": params["text_style_prompt"],
        "ai_model": params["ai_model"],
        "enable_original_uv": params["enable_original_uv"],
        "enable_pbr": params["enable_pbr"],
        "hd_texture": params["hd_texture"],
        "remove_lighting": params["remove_lighting"],
        "target_formats": params["target_formats"],
        "alpha_thumbnail": True,
    }


def redact_retexture_request(body: dict, params: dict) -> dict:
    redacted = dict(body)
    model = params["input_model"]
    redacted["model_url"] = (
        f"<data:{model['mime_type']};base64;sha256={model['sha256']}>"
    )
    return redacted


def estimated_retexture_credits() -> int:
    return 10


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

def poll_task(key: str, task_id: str, label: str, timeout_s: int = 1800,
              endpoint: str = "/openapi/v2/text-to-3d") -> dict:
    """Block until the task leaves a running state.

    Polls on a gentle backoff rather than a tight loop: these tasks take minutes,
    and hammering a metered API is its own kind of rude.
    """
    deadline = time.monotonic() + timeout_s
    delay = 5.0
    last_progress = -1

    while time.monotonic() < deadline:
        task = _request("GET", f"{endpoint}/{task_id}", key)
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


def load_concept_manifest(path: Path) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise MeshyError(f"concept manifest not found: {path}") from None
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise MeshyError(f"could not read concept manifest {path}: {exc}") from None

    if not isinstance(manifest, dict):
        raise MeshyError("concept manifest root must be an object")
    task = task_record(manifest, "concept")
    params = manifest.get("parameters")
    if not isinstance(params, dict):
        raise MeshyError("concept manifest parameters must be an object")
    if manifest.get("provider") != "Meshy" or manifest.get("stage") != "concept":
        raise MeshyError("concept manifest must be a completed Meshy concept artifact")
    if manifest.get("endpoint") != "/openapi/v1/text-to-image":
        raise MeshyError("concept manifest has the wrong endpoint")
    digest = manifest.get("request_hash")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise MeshyError("concept manifest request_hash is invalid")
    if request_hash(params) != digest:
        raise MeshyError("concept manifest parameters do not match request_hash")
    if task.get("status") != "SUCCEEDED" or not task.get("id"):
        raise MeshyError("concept task must be SUCCEEDED and have an id")
    review = manifest.get("art_review")
    if not isinstance(review, dict):
        raise MeshyError("concept manifest art_review must be an object")
    if review.get("status") != "approved_for_reconstruction":
        raise MeshyError(
            "concept manifest requires art_review.status=approved_for_reconstruction")
    for label in ("asset_id", "module_id"):
        identifier = params.get(label)
        if not isinstance(identifier, str) or not SLUG_PATTERN.fullmatch(identifier):
            raise MeshyError(f"concept manifest {label} is invalid")
    files = manifest.get("files")
    if not isinstance(files, dict):
        raise MeshyError("concept manifest files must be an object")
    images = files.get("images") or []
    if len(images) != 3 or not all(isinstance(image, str) and image for image in images):
        raise MeshyError("concept manifest must retain exactly three multi-view images")
    hashes = files.get("image_sha256") or {}
    if not isinstance(hashes, dict):
        raise MeshyError("concept manifest image_sha256 must be an object")
    for image in images:
        image_path = path.parent / image
        if not image_path.is_file():
            raise MeshyError(f"concept image is missing: {image}")
        expected = hashes.get(image)
        if not isinstance(expected, str) or file_sha256(image_path) != expected:
            raise MeshyError(f"concept image hash mismatch: {image}")
    return manifest


def generate_concept(
    key: str | None,
    out_root: Path,
    prompt: str,
    *,
    name: str,
    image_model: str,
    pose_mode: str | None,
    asset_id: str,
    module_id: str,
    dry_run: bool,
    max_credits: int,
    force: bool,
) -> Path:
    if len(prompt) > MAX_PROMPT_CHARS:
        raise MeshyError(f"prompt is {len(prompt)} chars, the production limit is {MAX_PROMPT_CHARS}")
    if image_model not in IMAGE_MODELS:
        raise MeshyError(f"image_model must be one of {IMAGE_MODELS}")
    if pose_mode not in (None, "a-pose", "t-pose"):
        raise MeshyError("pose_mode must be a-pose or t-pose")
    for label, identifier in (("name", name), ("asset_id", asset_id), ("module_id", module_id)):
        if not SLUG_PATTERN.fullmatch(identifier):
            raise MeshyError(f"{label} must use lowercase slug/id characters")

    params = build_concept_params(
        prompt,
        image_model=image_model,
        pose_mode=pose_mode,
        asset_id=asset_id,
        module_id=module_id,
    )
    full_digest = request_hash(params)
    asset_dir = out_root / f"{name}_{cache_key(params)}"
    manifest_path = asset_dir / "manifest.json"
    body = build_concept_request(params)

    existing = load_cached_manifest(manifest_path) if not force else {}
    existing_task = task_record(existing, "concept")
    request_matches = manifest_matches_request(existing, full_digest)
    expected_images = [f"view_{index}.png" for index in range(3)]
    complete_cache = bool(
        request_matches
        and completed_task(existing, "concept")
        and cached_file_list_matches(
            existing,
            asset_dir,
            file_key="images",
            hash_key="image_sha256",
            expected_names=expected_images,
        )
    )
    reusable_task = bool(request_matches and existing_task.get("id") and not force)
    projected = 0 if complete_cache or reusable_task else estimated_concept_credits(image_model)
    if projected > max_credits:
        raise MeshyError(f"projected cost {projected} exceeds --max-credits {max_credits}")

    if dry_run:
        print(json.dumps({
            "request_hash": full_digest,
            "output": str(asset_dir),
            "projected_credits": projected,
            "cache_state": "complete" if complete_cache else
                ("task" if reusable_task else "miss"),
            "concept_request": body,
            "credential_required": False,
        }, indent=2))
        return asset_dir
    if complete_cache:
        print(f"cached concept: {asset_dir} (no credits spent)")
        return asset_dir
    if not key:
        raise MeshyError("MESHY_API_KEY not set")

    balance = get_balance(key)
    print(f"balance: {balance} credits")
    if projected > 0 and (balance < CREDIT_FLOOR or balance < projected):
        raise MeshyError(f"balance {balance} is below the generation safety floor")
    asset_dir.mkdir(parents=True, exist_ok=True)

    if reusable_task:
        task_id = existing_task["id"]
        print(f"reusing concept task {task_id}")
    else:
        task_id = _request("POST", "/openapi/v1/text-to-image", key, body).get("result")
        if not task_id:
            raise MeshyError("concept request returned no task id")
        pending_manifest = {
            "schema_version": 2,
            "provider": "Meshy",
            "endpoint": "/openapi/v1/text-to-image",
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "request_hash": full_digest,
            "stage": "concept",
            "parameters": params,
            "tasks": {"concept": {"id": task_id, "status": "PENDING"}},
            "files": {"images": []},
            "consumed_credits": 0,
        }
        manifest_path.write_text(
            json.dumps(pending_manifest, indent=2) + "\n", encoding="utf-8")
    task = poll_task(
        key, task_id, "concept", endpoint="/openapi/v1/text-to-image")
    image_urls = task.get("image_urls") or []
    if len(image_urls) != 3:
        raise MeshyError(f"multi-view concept returned {len(image_urls)} images; expected 3")

    manifest = {
        "schema_version": 2,
        "provider": "Meshy",
        "endpoint": "/openapi/v1/text-to-image",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "request_hash": full_digest,
        "stage": "concept",
        "parameters": params,
        "tasks": {"concept": task_summary(task)},
        "files": {"images": []},
        "consumed_credits": int(task.get("consumed_credits", 0)),
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    images: list[str] = []
    image_hashes: dict[str, str] = {}
    for index, url in enumerate(image_urls):
        image_path = asset_dir / f"view_{index}.png"
        size = download(url, image_path)
        print(f"wrote {image_path} ({size // 1024} KiB)")
        images.append(image_path.name)
        image_hashes[image_path.name] = file_sha256(image_path)
    manifest["files"] = {"images": images, "image_sha256": image_hashes}
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"credits consumed: {manifest['consumed_credits']}")
    return asset_dir


def generate_multiview(
    key: str | None,
    out_root: Path,
    concept_manifest_path: Path,
    *,
    name: str,
    input_image_paths: list[Path] | None = None,
    ai_model: str,
    polycount: int,
    should_remesh: bool,
    decimation_mode: int | None,
    enable_pbr: bool,
    hd_texture: bool,
    remove_lighting: bool,
    texture_prompt: str | None,
    auto_size: bool,
    origin_at: str,
    dry_run: bool,
    max_credits: int,
    force: bool,
) -> Path:
    if not SLUG_PATTERN.fullmatch(name):
        raise MeshyError("name must use lowercase slug characters")
    if ai_model not in AI_MODELS:
        raise MeshyError(f"ai_model must be one of {AI_MODELS}")
    if texture_prompt and len(texture_prompt) > MAX_PROMPT_CHARS:
        raise MeshyError(f"texture_prompt exceeds {MAX_PROMPT_CHARS} chars")
    if should_remesh and not 100 <= polycount <= 300000:
        raise MeshyError("polycount must be between 100 and 300000")
    if decimation_mode is not None and (decimation_mode not in range(1, 5) or not should_remesh):
        raise MeshyError("decimation_mode 1-4 requires --remesh")

    concept = load_concept_manifest(concept_manifest_path)
    local_inputs = input_image_paths or []
    input_descriptions = describe_input_images(local_inputs) if local_inputs else None
    params = build_multiview_params(
        concept,
        input_images=input_descriptions,
        ai_model=ai_model,
        polycount=polycount,
        should_remesh=should_remesh,
        decimation_mode=decimation_mode,
        enable_pbr=enable_pbr,
        hd_texture=hd_texture,
        remove_lighting=remove_lighting,
        texture_prompt=texture_prompt,
        auto_size=auto_size,
        origin_at=origin_at,
    )
    full_digest = request_hash(params)
    asset_dir = out_root / f"{name}_{cache_key(params)}"
    manifest_path = asset_dir / "manifest.json"
    model_path = asset_dir / "model.glb"
    image_urls = ["<retained-image-data-uri>"] * len(local_inputs) if local_inputs else None
    body = build_multiview_request(params, image_urls)

    existing = load_cached_manifest(manifest_path) if not force else {}
    existing_task = task_record(existing, "generation")
    request_matches = manifest_matches_request(existing, full_digest)
    retained_inputs = [
        f"input_{index}{source.suffix.lower()}"
        for index, source in enumerate(local_inputs)
    ]
    retained_inputs_match = not retained_inputs or cached_file_list_matches(
        existing,
        asset_dir,
        file_key="input_images",
        hash_key="input_image_sha256",
        expected_names=retained_inputs,
    )
    complete_cache = bool(
        request_matches
        and completed_task(existing, "generation")
        and cached_file_matches(
            existing,
            asset_dir,
            file_key="model",
            hash_key="model_sha256",
            expected_name="model.glb",
        )
        and retained_inputs_match
    )
    reusable_task = bool(request_matches and existing_task.get("id") and not force)
    projected = 0 if complete_cache or reusable_task else estimated_multiview_credits(ai_model)
    if projected > max_credits:
        raise MeshyError(f"projected cost {projected} exceeds --max-credits {max_credits}")

    if dry_run:
        print(json.dumps({
            "request_hash": full_digest,
            "concept_request_hash": params["concept_request_hash"],
            "output": str(asset_dir),
            "projected_credits": projected,
            "cache_state": "complete" if complete_cache else
                ("task" if reusable_task else "miss"),
            "multiview_request": redact_multiview_request(body, params),
            "credential_required": False,
        }, indent=2))
        return asset_dir
    if complete_cache:
        print(f"cached multi-view asset: {asset_dir} (no credits spent)")
        return asset_dir
    if not key:
        raise MeshyError("MESHY_API_KEY not set")

    balance = get_balance(key)
    print(f"balance: {balance} credits")
    if projected > 0 and (balance < CREDIT_FLOOR or balance < projected):
        raise MeshyError(f"balance {balance} is below the generation safety floor")
    asset_dir.mkdir(parents=True, exist_ok=True)

    retained_inputs = []
    retained_hashes: dict[str, str] = {}
    if local_inputs:
        for index, source in enumerate(local_inputs):
            retained_path = asset_dir / f"input_{index}{source.suffix.lower()}"
            shutil.copyfile(source, retained_path)
            retained_inputs.append(retained_path.name)
            retained_hashes[retained_path.name] = file_sha256(retained_path)
        body = build_multiview_request(
            params,
            input_image_data_uris([asset_dir / name for name in retained_inputs]),
        )

    if reusable_task:
        task_id = existing_task["id"]
        print(f"reusing multi-view task {task_id}")
    else:
        task_id = _request("POST", "/openapi/v1/multi-image-to-3d", key, body).get("result")
        if not task_id:
            raise MeshyError("multi-view request returned no task id")
        pending_manifest = {
            "schema_version": 2,
            "provider": "Meshy",
            "endpoint": "/openapi/v1/multi-image-to-3d",
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "request_hash": full_digest,
            "stage": "multiview_textured",
            "parameters": params,
            "tasks": {"generation": {"id": task_id, "status": "PENDING"}},
            "files": {
                "input_images": retained_inputs,
                "input_image_sha256": retained_hashes,
            },
            "consumed_credits": 0,
        }
        manifest_path.write_text(
            json.dumps(pending_manifest, indent=2) + "\n", encoding="utf-8")
    task = poll_task(
        key, task_id, "multi-view", endpoint="/openapi/v1/multi-image-to-3d")
    glb_url = (task.get("model_urls") or {}).get("glb")
    if not glb_url:
        raise MeshyError("multi-view task succeeded but returned no GLB url")

    manifest = {
        "schema_version": 2,
        "provider": "Meshy",
        "endpoint": "/openapi/v1/multi-image-to-3d",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "request_hash": full_digest,
        "stage": "multiview_textured",
        "parameters": params,
        "tasks": {"generation": task_summary(task)},
        "files": {},
        "consumed_credits": int(task.get("consumed_credits", 0)),
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    size = download(glb_url, model_path)
    print(f"wrote {model_path} ({size // 1024} KiB)")
    files: dict = {
        "input_images": retained_inputs,
        "input_image_sha256": retained_hashes,
        "model": model_path.name,
        "model_sha256": file_sha256(model_path),
    }

    master_url = (task.get("model_urls") or {}).get("pre_remeshed_glb")
    if master_url:
        master_path = asset_dir / "master_pre_remesh.glb"
        download(master_url, master_path)
        files["master_pre_remesh"] = master_path.name
        files["master_pre_remesh_sha256"] = file_sha256(master_path)

    thumbnail_url = task.get("alpha_thumbnail_url") or task.get("thumbnail_url")
    if thumbnail_url:
        thumbnail_path = asset_dir / "thumbnail.png"
        download(thumbnail_url, thumbnail_path)
        files["thumbnail"] = thumbnail_path.name
        files["thumbnail_sha256"] = file_sha256(thumbnail_path)

    view_files: dict[str, str] = {}
    view_hashes: dict[str, str] = {}
    for view, url in (task.get("thumbnail_urls") or {}).items():
        if not url:
            continue
        view_path = asset_dir / f"thumbnail_{view}.png"
        download(url, view_path)
        view_files[view] = view_path.name
        view_hashes[view] = file_sha256(view_path)
    if view_files:
        files["thumbnail_views"] = view_files
        files["thumbnail_view_sha256"] = view_hashes

    textures: dict[str, str] = {}
    texture_hashes: dict[str, str] = {}
    for index, texture_set in enumerate(task.get("texture_urls") or []):
        for slot, url in (texture_set or {}).items():
            if not url:
                continue
            texture_path = asset_dir / f"{slot}_{index}.png"
            download(url, texture_path)
            key_name = f"{slot}_{index}"
            textures[key_name] = texture_path.name
            texture_hashes[key_name] = file_sha256(texture_path)
    files["textures"] = textures
    files["texture_sha256"] = texture_hashes
    manifest["files"] = files
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"credits consumed: {manifest['consumed_credits']}")
    return asset_dir


def generate_retexture(
    key: str | None,
    out_root: Path,
    input_model_path: Path,
    *,
    name: str,
    text_style_prompt: str,
    ai_model: str,
    enable_original_uv: bool,
    enable_pbr: bool,
    hd_texture: bool,
    remove_lighting: bool,
    asset_id: str,
    module_id: str,
    dry_run: bool,
    max_credits: int,
    force: bool,
) -> Path:
    if not SLUG_PATTERN.fullmatch(name):
        raise MeshyError("name must use lowercase slug characters")
    if not text_style_prompt.strip():
        raise MeshyError("text_style_prompt must not be empty")
    if len(text_style_prompt) > MAX_PROMPT_CHARS:
        raise MeshyError(
            f"text_style_prompt exceeds {MAX_PROMPT_CHARS} characters")
    if ai_model not in AI_MODELS:
        raise MeshyError(f"ai_model must be one of {AI_MODELS}")
    if (hd_texture or remove_lighting) and ai_model not in ("latest", "meshy-6"):
        raise MeshyError(
            "HD texture and lighting removal require Meshy 6 or latest")
    for label, identifier in (("asset_id", asset_id), ("module_id", module_id)):
        if not SLUG_PATTERN.fullmatch(identifier):
            raise MeshyError(f"{label} must use lowercase slug/id characters")

    source = input_model_path.resolve()
    description = describe_input_model(source)
    params = build_retexture_params(
        description,
        text_style_prompt=text_style_prompt,
        ai_model=ai_model,
        enable_original_uv=enable_original_uv,
        enable_pbr=enable_pbr,
        hd_texture=hd_texture,
        remove_lighting=remove_lighting,
        asset_id=asset_id,
        module_id=module_id,
    )
    full_digest = request_hash(params)
    asset_dir = out_root / f"{name}_{cache_key(params)}"
    manifest_path = asset_dir / "manifest.json"
    retained_path = asset_dir / "input.glb"
    model_path = asset_dir / "model.glb"
    body = build_retexture_request(params, "<input-model-data-uri>")

    existing = load_cached_manifest(manifest_path) if not force else {}
    existing_task = task_record(existing, "retexture")
    request_matches = manifest_matches_request(existing, full_digest)
    complete_cache = bool(
        request_matches
        and completed_task(existing, "retexture")
        and cached_file_matches(
            existing,
            asset_dir,
            file_key="input_model",
            hash_key="input_model_sha256",
            expected_name="input.glb",
        )
        and cached_file_matches(
            existing,
            asset_dir,
            file_key="model",
            hash_key="model_sha256",
            expected_name="model.glb",
        )
    )
    reusable_task = bool(
        request_matches and existing_task.get("id") and not force
    )
    projected = 0 if complete_cache or reusable_task else estimated_retexture_credits()
    if projected > max_credits:
        raise MeshyError(f"projected cost {projected} exceeds --max-credits {max_credits}")

    if dry_run:
        print(json.dumps({
            "request_hash": full_digest,
            "output": str(asset_dir),
            "projected_credits": projected,
            "cache_state": "complete" if complete_cache else
                ("task" if reusable_task else "miss"),
            "retexture_request": redact_retexture_request(body, params),
            "credential_required": False,
        }, indent=2))
        return asset_dir
    if complete_cache:
        print(f"cached retextured asset: {asset_dir} (no credits spent)")
        return asset_dir
    if not key:
        raise MeshyError("MESHY_API_KEY not set")

    balance = get_balance(key)
    print(f"balance: {balance} credits")
    if projected > 0 and (balance < CREDIT_FLOOR or balance < projected):
        raise MeshyError(f"balance {balance} is below the generation safety floor")
    asset_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, retained_path)
    if file_sha256(retained_path) != description["sha256"]:
        raise MeshyError("retained retexture input hash mismatch")
    body = build_retexture_request(params, input_model_data_uri(retained_path))

    if reusable_task:
        task_id = existing_task["id"]
        print(f"reusing retexture task {task_id}")
    else:
        task_id = _request("POST", "/openapi/v1/retexture", key, body).get("result")
        if not task_id:
            raise MeshyError("retexture request returned no task id")
        pending_manifest = {
            "schema_version": 2,
            "provider": "Meshy",
            "endpoint": "/openapi/v1/retexture",
            "generated_at_utc": datetime.now(timezone.utc).isoformat(),
            "request_hash": full_digest,
            "stage": "retexture",
            "parameters": params,
            "tasks": {"retexture": {"id": task_id, "status": "PENDING"}},
            "files": {
                "input_model": retained_path.name,
                "input_model_sha256": file_sha256(retained_path),
            },
            "consumed_credits": 0,
        }
        manifest_path.write_text(
            json.dumps(pending_manifest, indent=2) + "\n", encoding="utf-8")

    task = poll_task(
        key, task_id, "retexture", endpoint="/openapi/v1/retexture")
    glb_url = (task.get("model_urls") or {}).get("glb")
    if not glb_url:
        raise MeshyError("retexture task succeeded but returned no GLB url")

    files: dict = {
        "input_model": retained_path.name,
        "input_model_sha256": file_sha256(retained_path),
    }
    size = download(glb_url, model_path)
    print(f"wrote {model_path} ({size // 1024} KiB)")
    files["model"] = model_path.name
    files["model_sha256"] = file_sha256(model_path)

    thumbnail_url = task.get("alpha_thumbnail_url") or task.get("thumbnail_url")
    if thumbnail_url:
        thumbnail_path = asset_dir / "thumbnail.png"
        download(thumbnail_url, thumbnail_path)
        files["thumbnail"] = thumbnail_path.name
        files["thumbnail_sha256"] = file_sha256(thumbnail_path)

    textures: dict[str, str] = {}
    texture_hashes: dict[str, str] = {}
    for index, texture_set in enumerate(task.get("texture_urls") or []):
        for slot, url in (texture_set or {}).items():
            if not url:
                continue
            texture_path = asset_dir / f"{slot}_{index}.png"
            download(url, texture_path)
            key_name = f"{slot}_{index}"
            textures[key_name] = texture_path.name
            texture_hashes[key_name] = file_sha256(texture_path)
    files["textures"] = textures
    files["texture_sha256"] = texture_hashes

    manifest = {
        "schema_version": 2,
        "provider": "Meshy",
        "endpoint": "/openapi/v1/retexture",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "request_hash": full_digest,
        "stage": "retexture",
        "parameters": params,
        "tasks": {"retexture": task_summary(task)},
        "files": files,
        "consumed_credits": int(task.get("consumed_credits", 0)),
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"credits consumed: {manifest['consumed_credits']}")
    return asset_dir


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

    existing_manifest = load_cached_manifest(manifest_path) if not force else {}
    request_matches = manifest_matches_request(existing_manifest, full_digest)
    preview_cached = bool(
        request_matches
        and completed_task(existing_manifest, "preview")
        and cached_file_matches(
            existing_manifest,
            asset_dir,
            file_key="preview",
            hash_key="preview_sha256",
            expected_name="preview.glb",
        )
    )
    final_cached = bool(
        preview_cached
        and completed_task(existing_manifest, "refine")
        and cached_file_matches(
            existing_manifest,
            asset_dir,
            file_key="model",
            hash_key="model_sha256",
            expected_name="model.glb",
        )
    )
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
    if projected > 0 and balance < CREDIT_FLOOR:
        raise MeshyError(
            f"balance {balance} is below the floor of {CREDIT_FLOOR}. Generation is "
            "two-stage; starting a preview that cannot be refined wastes the spend."
        )
    if balance < projected:
        raise MeshyError(
            f"balance {balance} is below projected task cost {projected}")

    asset_dir.mkdir(parents=True, exist_ok=True)

    preview_id = task_record(existing_manifest, "preview").get("id") \
        if preview_cached else None
    preview: dict
    if preview_id and preview_path.is_file() and not force:
        print(f"reusing reviewed preview task {preview_id}")
        preview = task_record(existing_manifest, "preview")
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

    c = sub.add_parser(
        "concept",
        help="Generate three approved multi-view concept images. COSTS CREDITS unless --dry-run.")
    c.add_argument("--prompt", required=True)
    c.add_argument("--name", required=True)
    c.add_argument("--image-model", choices=IMAGE_MODELS, default="gpt-image-2")
    c.add_argument("--pose-mode", choices=("a-pose", "t-pose"), default=None)
    c.add_argument("--asset-id", required=True)
    c.add_argument("--module-id", required=True)
    c.add_argument("--dry-run", action="store_true")
    c.add_argument("--max-credits", type=int, default=9)
    c.add_argument("--force", action="store_true")
    c.add_argument("--out", default=str(repo_root / "assets" / "generated"))

    mv = sub.add_parser(
        "multiview",
        help="Generate a Meshy 6 PBR GLB from an approved concept manifest. COSTS CREDITS unless --dry-run.")
    mv.add_argument("--concept-manifest", required=True)
    mv.add_argument("--name", required=True)
    mv.add_argument(
        "--input-image",
        action="append",
        default=None,
        help="Use a reviewed local PNG/JPEG as a base64 data URI instead of the raw concept task; repeat 1-4 times",
    )
    mv.add_argument("--texture-prompt", default=None)
    mv.add_argument("--polycount", type=int, default=120000)
    mv.add_argument("--ai-model", choices=AI_MODELS, default="meshy-6")
    mv.add_argument("--remesh", action="store_true")
    mv.add_argument("--decimation-mode", type=int, choices=range(1, 5), default=None)
    mv.add_argument("--no-pbr", action="store_true")
    mv.add_argument("--hd-texture", action="store_true")
    mv.add_argument("--keep-lighting", action="store_true")
    mv.add_argument("--auto-size", action="store_true")
    mv.add_argument("--origin-at", choices=("bottom", "center"), default="bottom")
    mv.add_argument("--dry-run", action="store_true")
    mv.add_argument("--max-credits", type=int, default=30)
    mv.add_argument("--force", action="store_true")
    mv.add_argument("--out", default=str(repo_root / "assets" / "generated"))

    rt = sub.add_parser(
        "retexture",
        help="Create UVs and PBR textures for an accepted local GLB. COSTS CREDITS unless --dry-run.")
    rt.add_argument("--input-model", required=True)
    rt.add_argument("--name", required=True)
    rt.add_argument("--text-style-prompt", required=True)
    rt.add_argument("--ai-model", choices=AI_MODELS, default="meshy-6")
    rt.add_argument("--keep-original-uv", action="store_true")
    rt.add_argument("--no-pbr", action="store_true")
    rt.add_argument("--hd-texture", action="store_true")
    rt.add_argument("--keep-lighting", action="store_true")
    rt.add_argument("--asset-id", required=True)
    rt.add_argument("--module-id", required=True)
    rt.add_argument("--dry-run", action="store_true")
    rt.add_argument("--max-credits", type=int, default=10)
    rt.add_argument("--force", action="store_true")
    rt.add_argument("--out", default=str(repo_root / "assets" / "generated"))

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
        if args.cmd == "concept":
            key = None if args.dry_run else load_api_key(repo_root)
            generate_concept(
                key,
                Path(args.out),
                args.prompt,
                name=args.name,
                image_model=args.image_model,
                pose_mode=args.pose_mode,
                asset_id=args.asset_id,
                module_id=args.module_id,
                dry_run=args.dry_run,
                max_credits=args.max_credits,
                force=args.force,
            )
            return 0
        if args.cmd == "multiview":
            key = None if args.dry_run else load_api_key(repo_root)
            generate_multiview(
                key,
                Path(args.out),
                Path(args.concept_manifest),
                name=args.name,
                input_image_paths=(
                    [Path(path) for path in args.input_image]
                    if args.input_image else None
                ),
                ai_model=args.ai_model,
                polycount=args.polycount,
                should_remesh=args.remesh,
                decimation_mode=args.decimation_mode,
                enable_pbr=not args.no_pbr,
                hd_texture=args.hd_texture,
                remove_lighting=not args.keep_lighting,
                texture_prompt=args.texture_prompt,
                auto_size=args.auto_size,
                origin_at=args.origin_at,
                dry_run=args.dry_run,
                max_credits=args.max_credits,
                force=args.force,
            )
            return 0
        if args.cmd == "retexture":
            key = None if args.dry_run else load_api_key(repo_root)
            generate_retexture(
                key,
                Path(args.out),
                Path(args.input_model),
                name=args.name,
                text_style_prompt=args.text_style_prompt,
                ai_model=args.ai_model,
                enable_original_uv=args.keep_original_uv,
                enable_pbr=not args.no_pbr,
                hd_texture=args.hd_texture,
                remove_lighting=not args.keep_lighting,
                asset_id=args.asset_id,
                module_id=args.module_id,
                dry_run=args.dry_run,
                max_credits=args.max_credits,
                force=args.force,
            )
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
