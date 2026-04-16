#!/usr/bin/env nix-shell
#!nix-shell -i "uv run --script --quiet" -p uv nix-prefetch-git
# /// script
# requires-python = ">=3.12"
# dependencies = ["pyyaml", "tomlkit"]
# ///
"""Generate west2nix.toml from the active west manifest.

Runs `west manifest --freeze --active-only` to resolve all project
revisions, then `nix-prefetch-git` for each to compute Nix hashes.
Output is compatible with west2nix's hook.nix.

Usage: uv run scripts/west2nix.py
"""

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import tomlkit
import yaml


def freeze_manifest() -> dict:
    """Get the frozen west manifest (active projects only)."""
    result = subprocess.run(
        ["west", "manifest", "--freeze", "--active-only"],
        check=True,
        capture_output=True,
    )
    return yaml.safe_load(result.stdout)


def west_topdir() -> str:
    """Return the west workspace top directory."""
    result = subprocess.run(
        ["west", "topdir"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def prefetch(project: dict, topdir: str) -> None:
    """Compute the Nix hash for a west project.

    The hash must be computed using the same fetch options that
    west2nix's hook will use at build time. `--fetch-submodules`
    is required for any project with `submodules: true` in west.yml —
    without it, the computed hash excludes submodule content while
    the CI build includes it, causing hash mismatches.
    """
    path = project.get("path", project["name"])
    abs_path = os.path.join(topdir, path)

    cmd = [
        "nix-prefetch-git",
        "--quiet",
        "--url",
        abs_path,
        "--rev",
        project["revision"],
    ]
    # Only submodules=True (bool) triggers submodule fetching in nixpkgs
    # fetchgit. A list of submodule specs is currently silently ignored by
    # fetchgit, so we must not pass --fetch-submodules for those projects
    # (the hash would diverge from CI's fetchgit result).
    if project.get("submodules") is True:
        cmd.append("--fetch-submodules")

    result = subprocess.run(cmd, check=True, capture_output=True)
    project["nix"] = {"hash": json.loads(result.stdout)["hash"]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--max-workers",
        type=int,
        default=8,
        help="concurrent nix-prefetch-git calls (default: 8)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("west2nix.toml"),
        help="output file (default: west2nix.toml)",
    )
    args = parser.parse_args()

    manifest = freeze_manifest()
    topdir = west_topdir()
    projects = manifest["manifest"]["projects"]
    print(f"Prefetching {len(projects)} projects from {topdir}...", file=sys.stderr)

    with ThreadPoolExecutor(max_workers=args.max_workers) as pool:
        futures = {pool.submit(prefetch, p, topdir): p["name"] for p in projects}
        for future in as_completed(futures):
            name = futures[future]
            try:
                future.result()
                print(f"  {name}", file=sys.stderr)
            except subprocess.CalledProcessError as e:
                print(f"  {name}: FAILED ({e})", file=sys.stderr)
                raise

    with open(args.output, "w") as f:
        tomlkit.dump(manifest, f)

    print(f"Wrote {args.output} ({len(projects)} projects)", file=sys.stderr)


if __name__ == "__main__":
    main()
