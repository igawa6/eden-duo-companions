# Build the Pokémon Legends: Z-A Android companion

The Android package targets runtime 25 and game update 2.0.2. Its native module reads game state and ROMFS through the Eden Duo SDK. The load plan is generated from `native/helper/za_guest_helper.S` and `native/helper/za_guest_travel.S`.

## Build

Use Python 3.10 or later, CMake, Ninja, the Android NDK, and an Eden Duo checkout with its CPM dependency cache populated. The build was checked with NDK 28.2.13676358. Run this command from the repository root with the paths to your SDK and NDK.

```sh
python tools/za/build_android.py --eden "/path/to/eden-duo" --ndk "/path/to/android-ndk"
```

The script generates and checks the manifest, builds the load plan and ARM64 module, strips the module, and runs the repository's package validator. By default, the release archive is written to `dist/0100F43008C44000-PokemonLegendsZA-1.0.0.dsmod.zip`. Use `--version`, `--build`, or `--output` to select a different version or location.

## Tests and local inputs

Run `python tools/za/check_manifest.py` for manifest validation. Configure the standalone native project with `BUILD_TESTING=ON` to build the Z-A tests. The mailbox test runs without game inputs. The save, live-data, and language tests use local fixtures. Keep saves, ROMFS, keys, and fixture outputs out of the repository.

The font atlas uses Noto Sans CJK. Its notices are in the package's `font/` directory. The generator accepts a locally installed font and a local text corpus. The committed atlas is sufficient to build the default package.

## Data credits

Factual TM and encounter metadata and Mega form classification derive from [PKHeX](https://github.com/kwsch/PKHeX). Early party research referenced [SysBot.NET](https://github.com/kwsch/SysBot.NET). The module reuses the repository's ooz decoder and Eden Duo's BC decoder.
