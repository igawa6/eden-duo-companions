#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Sickops
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

from build_helper_plan import BUILD


ROOT = Path(__file__).resolve().parents[2]
TOOLS = Path(__file__).resolve().parent
PACKAGE = ROOT / "packages/PokemonLegendsZA"
TITLE_ID = "0100F43008C44000"


def run(*command: str | Path) -> None:
    subprocess.run(command, cwd=ROOT, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description="Build the Pokémon Legends: Z-A Android companion.")
    parser.add_argument("--eden", type=Path, required=True, help="Eden Duo checkout with its CPM cache")
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--version", default="1.1.0")
    parser.add_argument("--build", type=Path, default=ROOT / "build-za-android")
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    args = parser.parse_args()
    build = args.build.resolve()
    output = args.output.resolve()
    ndk = args.ndk.resolve()
    executable_suffix = ".exe" if os.name == "nt" else ""
    llvm_tools = list((ndk / "toolchains/llvm/prebuilt").glob(f"*/bin/clang{executable_suffix}"))
    if len(llvm_tools) != 1:
        parser.error("Select an Android NDK installation with one host LLVM toolchain.")
    llvm_bin = llvm_tools[0].parent
    toolchain = ndk / "build/cmake/android.toolchain.cmake"
    if not toolchain.is_file():
        parser.error(f"Android CMake toolchain missing: {toolchain}")
    run(sys.executable, TOOLS / "gen_manifest.py", "--runtime25")
    run(sys.executable, TOOLS / "check_manifest.py")
    run(sys.executable, TOOLS / "build_helper_plan.py", "--llvm-bin", llvm_bin, "--work", build / "helper")
    run(
        "cmake", "-S", ROOT / "native/modules", "-B", build, "-G", "Ninja",
        f"-DEDEN_SOURCE_ROOT={args.eden.resolve()}", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-24", "-DANDROID_STL=c++_static",
        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", "-DEDEN_ACNH_BUILD_DIAGNOSTICS=OFF",
    )
    run("cmake", "--build", build, "--target", "dsmod-za")
    stripped = build / "stripped" / f"{TITLE_ID}.so"
    stripped.parent.mkdir(exist_ok=True)
    run(llvm_bin / f"llvm-strip{executable_suffix}", "--strip-all", "-o", stripped, build / stripped.name)
    run(
        sys.executable, ROOT / "tools/build_dualscreen_package.py",
        "--package", PACKAGE, "--output", output, "--version", args.version,
        "--min-runtime", "25", "--build-id", BUILD,
        "--module", f"android-arm64-v8a={stripped}",
    )
    destination = output / f"{TITLE_ID}-PokemonLegendsZA-{args.version}.dsmod.zip"
    shutil.copyfile(output / f"{TITLE_ID}.dsmod.zip", destination)
    print(destination)


if __name__ == "__main__":
    main()
