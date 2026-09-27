#!/usr/bin/env python3
"""Build a portable Eden dual-screen mod archive.

The source tree is deliberately still an Eden add-on (``dualscreen/`` under a mod folder),
while the distributable is a ZIP whose root has a small package manifest.  Keeping the two
layouts separate means this tool can add metadata to old packages without rewriting the source
package or the game's data files.

    build_dualscreen_package.py --package packages/MetroidDread --output dist

The output is ``<TITLEID>.dsmod.zip``.  Files are written in sorted order with a fixed ZIP
timestamp, so rebuilding the same input produces byte-for-byte identical output.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import tempfile
import zipfile
from pathlib import Path, PurePosixPath


TITLE_ID_RE = re.compile(r"^[0-9A-Fa-f]{16}$")
BUILD_ID_RE = re.compile(r"^(?:[0-9A-F]{16}|[0-9A-F]{64})$")
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$")
PLATFORMS = {"android-arm64-v8a", "linux-x86_64"}

# Investigation artefacts must stay out of a portable package.  Referenced map, icon, HUD, and
# texture assets are package content and are retained; the manifest reference check below catches
# an accidentally omitted asset instead of silently producing a broken archive.
IGNORED_DIRS = {
    ".git",
    "__pycache__",
    "build",
    "cache",
    "caches",
    "debug",
    "dump",
    "dumps",
    "extract",
    "extracted",
    "tmp",
    "temp",
}
IGNORED_SUFFIXES = {
    ".bak",
    ".dump",
    ".log",
    ".orig",
    ".pyc",
    ".pyo",
    ".tmp",
    ".trace",
}


class PackageError(ValueError):
    """An input package does not satisfy the archive contract."""


def title_id(value: str, where: str = "title_id") -> str:
    if not TITLE_ID_RE.fullmatch(value):
        raise PackageError(f"{where} must be exactly 16 hexadecimal characters")
    return value.upper()


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PackageError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise PackageError(f"{path} must contain a JSON object")
    return value


def source_title_id(manifest: dict, override: str | None) -> str:
    if override is not None:
        return title_id(override, "--title-id")
    value = manifest.get("title_id")
    if not isinstance(value, str):
        raise PackageError("source manifest has no title_id; pass --title-id")
    return title_id(value, "source manifest title_id")


def iter_source_files(root: Path):
    """Yield package files below root, omitting investigation artefacts."""
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        parts = {part.lower() for part in relative.parts[:-1]}
        if parts & IGNORED_DIRS:
            continue
        if path.suffix.lower() in IGNORED_SUFFIXES:
            continue
        if path.name.lower() in {".ds_store", "thumbs.db"}:
            continue
        yield path, relative


def ensure_no_symlinks(root: Path) -> None:
    # rglob().is_file() follows links, so reject links before walking the source tree.  This
    # keeps an accidental link to a game dump from being copied into a release archive.
    for path in root.rglob("*"):
        if path.is_symlink():
            raise PackageError(f"source package may not contain symlinks: {path}")


def parse_module(value: str) -> tuple[str, Path]:
    platform, separator, path = value.partition("=")
    if not separator or not platform or not path:
        raise PackageError("--module must be PLATFORM=PATH")
    if platform not in PLATFORMS:
        allowed = ", ".join(sorted(PLATFORMS))
        raise PackageError(f"unsupported module platform {platform!r}; use {allowed}")
    module_path = Path(path).expanduser()
    if not module_path.is_file():
        raise PackageError(f"module path does not exist: {module_path}")
    if module_path.suffix.lower() != ".so":
        raise PackageError(f"module must be a .so file: {module_path}")
    return platform, module_path


def validate_staged_package(staged: Path, package_manifest: dict, expected_title: str) -> None:
    """Validate the parts the runtime and installer need before archiving."""
    if package_manifest.get("format") != 1:
        raise PackageError("package.json format must be 1")
    if package_manifest.get("type") != "dual-screen-mod":
        raise PackageError("package.json type must be 'dual-screen-mod'")
    if package_manifest.get("title_id") != expected_title:
        raise PackageError("package.json title_id does not match the package title")
    for key in ("name", "version"):
        if not isinstance(package_manifest.get(key), str) or not package_manifest[key].strip():
            raise PackageError(f"package.json {key} must be a non-empty string")

    ds_manifest_path = staged / "dualscreen" / "manifest.json"
    ds_manifest = read_json(ds_manifest_path)
    if ds_manifest.get("format", 1) != 1:
        raise PackageError("dualscreen/manifest.json format must be 1")
    if ds_manifest.get("title_id") != expected_title:
        raise PackageError("dualscreen/manifest.json title_id does not match package.json")

    # A package is only installable when all local assets named by its manifest are present.  Paths
    # are relative to dualscreen/, matching the runtime VFS; reject traversal rather than allowing
    # a malformed manifest to point outside the archive.
    def check_file_references(value: object) -> None:
        if isinstance(value, dict):
            for child in value.values():
                check_file_references(child)
        elif isinstance(value, list):
            for child in value:
                check_file_references(child)
        elif isinstance(value, str) and value.startswith("file:"):
            reference = value[5:]
            parsed = PurePosixPath(reference)
            if not reference or parsed.is_absolute() or ".." in parsed.parts:
                raise PackageError(f"manifest file reference escapes dualscreen/: {value}")
            target = staged / "dualscreen" / Path(*parsed.parts)
            if not target.is_file():
                raise PackageError(f"manifest file reference is missing: {value}")

    check_file_references(ds_manifest)

    package_module = package_manifest.get("module")
    manifest_module = ds_manifest.get("module")
    if package_module != manifest_module:
        raise PackageError("package.json and dualscreen/manifest.json module metadata differ")
    if package_module is not None:
        if not isinstance(package_module, dict) or package_module.get("abi", 0) < 1:
            raise PackageError("module.abi must be a positive integer")
        build_ids = package_module.get("build_ids")
        if not isinstance(build_ids, list) or not build_ids:
            raise PackageError("module.build_ids must be a non-empty list")
        for build_id in build_ids:
            if not isinstance(build_id, str) or not BUILD_ID_RE.fullmatch(build_id):
                raise PackageError("module.build_ids must contain uppercase 16- or 64-digit build IDs")
        # Native readers can supply all outputs without a build-specific data JSON. Any data
        # files that are present are still included and parsed below, as for existing packages.
        libraries = package_module.get("libraries")
        if not isinstance(libraries, dict) or not libraries:
            raise PackageError("module.libraries must be a non-empty object")
        for platform, library in libraries.items():
            if platform not in PLATFORMS or not isinstance(library, dict):
                raise PackageError(f"invalid module library entry: {platform}")
            path = library.get("path")
            digest = library.get("sha256")
            if not isinstance(path, str) or not path.startswith("modules/"):
                raise PackageError(f"module {platform} has an invalid path")
            module_path = staged / "dualscreen" / Path(*PurePosixPath(path).parts)
            if not module_path.is_file():
                raise PackageError(f"module {platform} is missing: {path}")
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise PackageError(f"module {platform} has an invalid sha256")
            if hashlib.sha256(module_path.read_bytes()).hexdigest() != digest:
                raise PackageError(f"module {platform} sha256 does not match payload")

    # Parse every JSON file early.  Build-ID files are intentionally named by the runtime's
    # build ID; preserving those names is what allows several updates to ship together.
    for path in staged.rglob("*.json"):
        read_json(path)


def build_package(
    package: Path,
    output: Path,
    version: str,
    name: str | None = None,
    override_title_id: str | None = None,
    modules: list[str] | None = None,
    module_abi: int = 1,
    module_build_ids: list[str] | None = None,
) -> Path:
    package = package.expanduser().resolve()
    if not package.is_dir():
        raise PackageError(f"package directory does not exist: {package}")
    ensure_no_symlinks(package)
    source_dualscreen = package / "dualscreen"
    source_manifest_path = source_dualscreen / "manifest.json"
    if not source_manifest_path.is_file():
        raise PackageError(f"missing {source_manifest_path}")
    source_manifest = read_json(source_manifest_path)
    expected_title = source_title_id(source_manifest, override_title_id)
    if not VERSION_RE.fullmatch(version):
        raise PackageError("version must use semantic version form MAJOR.MINOR.PATCH")
    package_name = name or source_manifest.get("name") or package.name
    if not isinstance(package_name, str) or not package_name.strip():
        raise PackageError("name must be a non-empty string")

    parsed_modules = [parse_module(item) for item in modules or []]
    module_names = [platform for platform, _ in parsed_modules]
    if len(module_names) != len(set(module_names)):
        raise PackageError("each module platform may be specified only once")
    if parsed_modules and not module_build_ids:
        raise PackageError("--build-id is required for native modules")
    if source_manifest.get("requires_module", False) and not parsed_modules:
        raise PackageError("this package requires a native module; pass --module PLATFORM=PATH")
    if module_abi < 1:
        raise PackageError("--abi must be positive")
    module_build_ids = [str(value).upper() for value in (module_build_ids or [])]
    if len(module_build_ids) != len(set(module_build_ids)):
        raise PackageError("each --build-id may be specified only once")
    for value in module_build_ids:
        if not BUILD_ID_RE.fullmatch(value):
            raise PackageError("--build-id values must be 16 or 64 hexadecimal characters")

    output = output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    archive_path = output / f"{expected_title}.dsmod.zip"

    with tempfile.TemporaryDirectory(prefix="eden-duo-package-") as temporary:
        staged = Path(temporary)
        staged_dualscreen = staged / "dualscreen"
        staged_dualscreen.mkdir()
        for source, relative in iter_source_files(source_dualscreen):
            destination = staged_dualscreen / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)

        # Older packages (notably Hollow Knight) predate title_id in the dualscreen manifest.
        # Inject it in the archive copy so the source checkout remains backward-compatible and
        # untouched.  Existing, differing IDs are rejected instead of silently retargeting them.
        staged_manifest_path = staged_dualscreen / "manifest.json"
        staged_manifest = read_json(staged_manifest_path)
        source_manifest_title = staged_manifest.get("title_id")
        if source_manifest_title is not None and title_id(str(source_manifest_title), "manifest title_id") != expected_title:
            raise PackageError("source dualscreen manifest title_id does not match package title")
        staged_manifest["title_id"] = expected_title
        staged_manifest_path.write_text(
            json.dumps(staged_manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8"
        )

        package_manifest = {
            "format": 1,
            "type": "dual-screen-mod",
            "title_id": expected_title,
            "name": package_name,
            "version": version,
        }
        if source_manifest.get("requires_module", False):
            package_manifest["requires_module"] = True
        if parsed_modules:
            libraries = {}
            for platform, source in parsed_modules:
                # The metadata path is relative to dualscreen/. The archive member receives the
                # dualscreen/ prefix when this staged tree is zipped.
                module_relative = PurePosixPath("modules", platform, f"{expected_title}.so")
                destination = staged / "dualscreen" / Path(*module_relative.parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, destination)
                libraries[platform] = {
                    "path": str(module_relative),
                    "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                }
            package_manifest["module"] = {
                "abi": module_abi,
                "build_ids": module_build_ids,
                "libraries": libraries,
            }

        # The runtime sees dualscreen/manifest.json after extraction. Keep the native metadata
        # there as well as at archive level so installers and the runtime validate the same bytes.
        if "module" in package_manifest:
            staged_manifest["module"] = package_manifest["module"]
            staged_manifest_path.write_text(
                json.dumps(staged_manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8"
            )

        (staged / "package.json").write_text(
            json.dumps(package_manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        validate_staged_package(staged, package_manifest, expected_title)

        with zipfile.ZipFile(
            archive_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as archive:
            files = sorted(path for path in staged.rglob("*") if path.is_file())
            for path in files:
                relative = path.relative_to(staged).as_posix()
                info = zipfile.ZipInfo(relative, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                archive.writestr(info, path.read_bytes())

    return archive_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True, help="source package directory")
    parser.add_argument("--output", type=Path, default=Path("."), help="archive output directory")
    parser.add_argument("--title-id", help="16-digit title ID (required when source lacks one)")
    parser.add_argument("--name", help="display name; defaults to dualscreen manifest name")
    parser.add_argument("--version", default="1.0.0", help="package version (default: 1.0.0)")
    parser.add_argument(
        "--module",
        action="append",
        default=[],
        metavar="PLATFORM=PATH",
        help="optional native module; repeat for each supported platform",
    )
    parser.add_argument("--abi", type=int, default=1, help="native module C ABI version (default: 1)")
    parser.add_argument(
        "--build-id",
        action="append",
        default=[],
        metavar="BUILDID",
        help="16- or 64-digit build ID supported by every native module (repeat as needed)",
    )
    args = parser.parse_args(argv)
    try:
        archive = build_package(
            package=args.package,
            output=args.output,
            version=args.version,
            name=args.name,
            override_title_id=args.title_id,
            modules=args.module,
            module_abi=args.abi,
            module_build_ids=args.build_id,
        )
    except PackageError as exc:
        parser.error(str(exc))
    print(archive)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
