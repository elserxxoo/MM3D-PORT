#!/usr/bin/env python3
"""Generate the translated C++ sources of Majora's Mask 3D (USA v1.1) for the NDK.

Publisher/developer tooling. It reuses the exact translation pipeline that
tools/mm3d/package/mm3d_setup.py runs on a PC (Thumb->A32 lowering, whole-AOT
program, function selection) and stops before the x86-64 compile step, writing
the sharded C++ and the TITLE_SOURCE_MANIFEST.json that
ports/android/native (triaevum_android_title) verifies and compiles for ARM64.

Input : a directory with the player's decrypted `code.bin` and `exheader.bin`
        (what tools/mm3d/ctr_extract.py writes). Nothing else of the game is used.
Output: <out>/ *.cpp, *.h and TITLE_SOURCE_MANIFEST.json

The output is derived from the game. Never publish it.

usage: generate_title_from_mm3d.py --game-dir rom --work build/work --out build/title_src
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for _path in (
    REPO_ROOT / "tools/mm3d/package",
    REPO_ROOT / "tools/mm3d",
    REPO_ROOT / "tools/oot3d/native_a32_runtime",
):
    sys.path.insert(0, str(_path))

import mm3d_setup as setup  # noqa: E402  (SUPPORTED, prepare_work, translate)
from whole_aot_cpp import generate  # noqa: E402

SOURCE_NAME = re.compile(r"^[A-Za-z0-9_]+\.(cpp|h)$")
SHARD_COUNT = 256


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--game-dir", type=Path, required=True,
                        help="directory containing code.bin and exheader.bin")
    parser.add_argument("--work", type=Path, required=True,
                        help="scratch directory for the translation passes")
    parser.add_argument("--out", type=Path, required=True,
                        help="output directory for the generated C++ sources")
    args = parser.parse_args(argv)

    game = args.game_dir.resolve()
    work = args.work.resolve()
    out = args.out.resolve()

    code = game / "code.bin"
    exheader = game / "exheader.bin"
    for required in (code, exheader):
        if not required.is_file():
            print(f"ERROR: missing {required}", file=sys.stderr)
            return 2

    digest = sha256_file(code)
    version = setup.SUPPORTED.get(digest)
    if version is None:
        print("ERROR: unsupported code.bin.\n"
              f"  sha256 : {digest}\n"
              f"  allowed: {', '.join(f'{h} ({v})' for h, v in setup.SUPPORTED.items())}\n"
              "Only a decrypted Majora's Mask 3D USA v1.1 dump is supported.",
              file=sys.stderr)
        return 3
    print(f"Majora's Mask 3D {version} (code.bin sha256 verified)", flush=True)

    # Fresh output: the manifest must describe exactly what is in the directory.
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    # 1. Same preparation and translation passes as the PC setup.
    setup.prepare_work(work)
    setup.translate(work, game)  # exits with an error message on failure

    program = work / "aot_program.json"
    selection = work / "mm3d_functions.json"
    code_image = work / "code_thumbfree.bin"
    for produced in (program, selection, code_image):
        if not produced.is_file():
            print(f"ERROR: the translation did not produce {produced.name}", file=sys.stderr)
            return 4

    # 2. Same generator call as build_whole_aot_plugin(), without the x86 compile.
    setup.step(f"Generating C++ ({SHARD_COUNT} shards, affinity strategy)")
    manifest = generate(program, selection, code_image, out,
                        shard_count=SHARD_COUNT, shard_strategy="affinity")
    files = manifest["files"]
    print(f"  {len(manifest['functions'])} functions in {manifest['shard_count']} shards, "
          f"{len(files)} files", flush=True)

    # 3. Self-check exactly what CMake (TranslatedTitleSources.cmake) will check.
    problems = []
    for name, expected in files.items():
        path = out / name
        if not SOURCE_NAME.match(name):
            problems.append(f"invalid source name: {name}")
        elif not path.is_file():
            problems.append(f"listed but missing: {name}")
        elif sha256_file(path) != expected:
            problems.append(f"hash mismatch: {name}")
    if not any(name.endswith(".cpp") for name in files):
        problems.append("no C++ sources were generated")
    if problems:
        print("ERROR: inconsistent generator output:\n  " + "\n  ".join(problems[:20]),
              file=sys.stderr)
        return 5

    title_manifest = {
        "format": "triaevum_translated_title_source_v1",
        "recipe": "mm3d-usa-v1.1",
        "build_source_commit": os.environ.get("GITHUB_SHA", "manual"),
        "build": {},
        "files": files,
    }
    (out / "TITLE_SOURCE_MANIFEST.json").write_text(
        json.dumps(title_manifest, indent=2), encoding="utf-8")
    print(f"\nOK: {len(files)} files + TITLE_SOURCE_MANIFEST.json in {out}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
