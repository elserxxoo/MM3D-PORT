#!/usr/bin/env python3
"""Build the private ThorMask data package from the player's own extracted files.

Input : the directory written by tools/mm3d/ctr_extract.py (code.bin,
        exheader.bin, romfs.bin) from a decrypted Majora's Mask 3D USA v1.1 dump.
Output: a folder that mirrors /sdcard/Android/data/org.triaevum.thormask/files
        (launch profile, process manifest, game files, empty config/save dirs).

The output is derived from the game: keep it private, never commit it, never put
it in the APK. Python 3 standard library only; runs on the player's PC.

usage: prepare_thor_data.py --game-dir salida --output thor-data [--profile safe|quality]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools/mm3d/package"))
import mm3d_setup as setup  # noqa: E402  (SUPPORTED, CLEAN_ROMFS, TITLE)

APPLICATION_ID = "org.triaevum.thormask"

# Launch profiles. "safe" is the upstream-qualified phone configuration (native
# 30 Hz, no interpolation, 640x360 logical). "quality" mirrors the PC defaults.
PROFILES = {
    "safe": {
        "--gameplay-timing": "native30_no_interpolation",
        "--presentation-rate": "30",
        "--width": "640",
        "--height": "360",
    },
    "quality": {
        "--gameplay-timing": "native30_interpolated",
        "--presentation-rate": "60",
        "--width": "1280",
        "--height": "720",
    },
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def relativize(value, root: Path):
    """Absolute paths inside the manifest become paths relative to the package."""
    if isinstance(value, dict):
        return {key: relativize(item, root) for key, item in value.items()}
    if isinstance(value, list):
        return [relativize(item, root) for item in value]
    if isinstance(value, str) and Path(value).is_absolute():
        try:
            return Path(value).resolve().relative_to(root).as_posix()
        except (ValueError, OSError):
            return value
    return value


def launch_profile(profile: str) -> dict:
    arguments = {
        "--a32-process-manifest": "${profile_dir}/process-manifest.json",
        "--resource-root": "${profile_dir}/resources",
        "--renderer": "nri",  # same Vulkan backend as "vulkan" (see bootstrap)
        "--ui-profile": "oot3d",  # what the PC port of Majora's Mask 3D uses
        "--config": "${profile_dir}/data/config/TriAevum.android.json",
        "--save-data": "${profile_dir}/data/savedata",
        "--renderer-cache-directory": "${profile_dir}/data/cache/renderer",
        "--output": "${profile_dir}/data/runtime-state.json",
    }
    arguments.update(PROFILES[profile])
    return {
        "format": "oot3d_native_game_launch_profile_v1",
        "arguments": [item for pair in arguments.items() for item in pair],
        "path_scopes": {key: "relative" for key, value in arguments.items()
                        if value.startswith("${profile_dir}/")},
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--game-dir", type=Path, required=True,
                        help="folder with code.bin, exheader.bin and romfs.bin")
    parser.add_argument("--output", type=Path, required=True,
                        help="new folder for the package (must not exist)")
    parser.add_argument("--profile", choices=sorted(PROFILES), default="safe")
    args = parser.parse_args(argv)

    game = args.game_dir.resolve()
    output = args.output.resolve()
    if output == REPO_ROOT or output.is_relative_to(REPO_ROOT):
        print("ERROR: the output must be outside the source repository.", file=sys.stderr)
        return 2
    if output.exists():
        print(f"ERROR: {output} already exists; choose a new folder.", file=sys.stderr)
        return 2

    names = ("code.bin", "exheader.bin", "romfs.bin")
    for name in names:
        if not (game / name).is_file():
            print(f"ERROR: missing {game / name}", file=sys.stderr)
            return 2

    print("Checking code.bin ...", flush=True)
    version = setup.SUPPORTED.get(sha256_file(game / "code.bin"))
    if version is None:
        print("ERROR: unsupported code.bin. Only a decrypted Majora's Mask 3D "
              "USA v1.1 dump is supported.", file=sys.stderr)
        return 3
    print(f"  Majora's Mask 3D {version}")
    print("Checking romfs.bin (this reads 650 MB) ...", flush=True)
    if sha256_file(game / "romfs.bin") not in setup.CLEAN_ROMFS:
        print("  WARNING: romfs.bin is not a clean USA v1.1 dump; the game may not start.")

    output.mkdir(parents=True)
    (output / "game").mkdir()
    for name in names:
        print(f"Copying {name} ...", flush=True)
        shutil.copyfile(game / name, output / "game" / name)

    print("Writing the process manifest ...", flush=True)
    manifest = output / "process-manifest.json"
    subprocess.run(
        [sys.executable, str(REPO_ROOT / "tools/oot3d/native_a32_runtime/build_process_manifest.py"),
         "--exheader", str(output / "game/exheader.bin"),
         "--code-bin", str(output / "game/code.bin"),
         "--romfs-image", str(output / "game/romfs.bin"),
         "--output", str(manifest)],
        check=True)
    data = json.loads(manifest.read_text(encoding="utf-8"))
    data["title"] = setup.TITLE
    data = relativize(data, output)
    manifest.write_text(json.dumps(data, indent=1), encoding="utf-8")

    # Everything the manifest points to must exist inside the package.
    source = data.get("source", {})
    for key in ("code_bin_path", "exheader_path", "romfs_image_path"):
        target = (manifest.parent / source[key]).resolve()
        if not target.is_file() or not target.is_relative_to(output):
            print(f"ERROR: manifest {key} does not resolve inside the package: {source[key]}",
                  file=sys.stderr)
            return 4

    # Resource root: the PC package ships gamecontrollerdb.txt and button prompt
    # textures. Android's SDL brings its own controller mappings; an empty (comment
    # only) file keeps any lookup harmless. Button textures are optional.
    resources = output / "resources"
    resources.mkdir()
    (resources / "gamecontrollerdb.txt").write_text("# intentionally empty\n", encoding="utf-8")

    for folder in ("data/config", "data/savedata", "data/cache/renderer", "logs"):
        (output / folder).mkdir(parents=True, exist_ok=True)
    (output / "data/config/TriAevum.android.json").write_text("{}\n", encoding="utf-8")

    (output / "TriAevum.android.launch.json").write_text(
        json.dumps(launch_profile(args.profile), indent=2) + "\n", encoding="utf-8")

    total = sum(path.stat().st_size for path in output.rglob("*") if path.is_file())
    print(f"\nOK ({args.profile} profile): {output}  [{total / 1e6:.0f} MB]")
    print("Private game data: do not publish it or add it to the APK.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
