#!/usr/bin/env bash
# Linux build of Brawl Recompiled. Run on Linux or in WSL, after tools\build-brawl.ps1 has translated the game on
# Windows: this reuses that output (wiicompiled\generated) and only compiles it with clang.
# With --translate it translates on Linux instead (needs the .NET SDK, and game/main_translate.dol and
# game/brawl_modules.rel from build_modules.py, as in the README), so no Windows step is needed.
#   bash tools/build-linux.sh [--jobs N] [--translate]
# Sources are copied to ~/brawl-linux first (WSL compiles far faster off /mnt/c). Output: ~/brawl-linux/usa/build/WiiCompiled
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
jobs=6   # translated shards need ~1 GB each; WSL gets half the PC's memory
translate=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) jobs=$2; shift ;;
        --translate) translate=1 ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
    shift
done

src=$root/wiicompiled
work=$HOME/brawl-linux/usa
mkdir -p "$work"

# Clang + lld: the portable toolchain WiiCompiled's own prepare-portable-tools.sh fetches.
tools=$HOME/brawl-linux/portable-tools
if [[ ! -x $tools/toolchain-x86_64/bin/clang++ ]]; then
    bash "$root/wiicompiled/Launcher/prepare-portable-tools.sh" --arch x86_64 --destination "$tools"
fi
bin=$tools/toolchain-x86_64/bin

echo "copying sources to $work"
rsync -a --delete "$src/runtime/" "$work/runtime/"
rsync -a --delete "$root/wiicompiled/aurora-main/" "$work/aurora-main/"
if (( translate )); then
    # The steps of build-brawl.ps1's translation, run in $work: recomp.yml's paths are relative, so the project goes
    # next to the runtime and the game's inputs to $work/../game, and the output's absolute paths are already right.
    for f in main_translate.dol brawl_modules.rel; do
        [[ -f $root/game/$f ]] || { echo "missing game/$f: run build_modules.py first (see the README)" >&2; exit 1; }
    done
    rsync -a --delete "$src/projects/" "$work/projects/"
    mkdir -p "$work/../game"
    cp "$root/game/main_translate.dol" "$root/game/brawl_modules.rel" "$work/../game/"
    translator=$src/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll
    [[ -f $translator ]] || dotnet build "$src/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release
    export DOTNET_ROLL_FORWARD=Major DOTNET_CLI_TELEMETRY_OPTOUT=1
    project=projects/brawl/recomp.yml
    (
        cd "$work"
        dotnet "$translator" translate-recursive 0x8000403C --project $project \
            --outdir generated/functions --output-metadata generated/base_translation_output.json \
            --production-source-bundle generated/base_translation_sources.bin \
            --no-function-files --no-state-free-abi --data-in-text projects/brawl/resident_data_ranges.txt --prune-stale \
            --threads "$jobs"
        dotnet "$translator" generate-data-init --project $project
        dotnet "$translator" emit-build-shards --project $project \
            --base-metadata generated/base_translation_output.json --base-functions-dir generated/functions \
            --native-source-dir runtime/src --out generated/build_shards --base-shards 600
    )
else
    rsync -a --delete --exclude functions "$src/generated/" "$work/generated/"
    # The translator wrote absolute Windows paths into the shard list and the data blobs' .incbin lines.
    win=$(wslpath -m "$src")
    grep -rlF "$win" "$work/generated" --include='*.cmake' --include='*.S' --include='*.cpp' --include='*.h' |
        xargs -r sed -i "s|$win|$work|g"
fi

# The bundled SDL3, Abseil, FreeType and libpng, as in the release, even where the system has its own: aurora's "auto"
# would link those, and the system Abseil's soname changes with every update.
cmake -S "$work/runtime" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$bin/clang" -DCMAKE_CXX_COMPILER="$bin/clang++" -DCMAKE_ASM_COMPILER="$bin/clang" \
    -DCMAKE_AR="$bin/llvm-ar" -DCMAKE_RANLIB="$bin/llvm-ranlib" -DCMAKE_LINKER="$bin/ld.lld" \
    -DMKW_TRANSLATED_COMPILE_JOBS="$jobs" \
    -DAURORA_SDL3_PROVIDER=vendor -DCMAKE_DISABLE_FIND_PACKAGE_absl=ON -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON
cmake --build "$work/build" --target WiiCompiled --parallel "$jobs"
echo "DONE: $work/build/WiiCompiled"

# The launcher's controller list (launcher/Gamepads.cs): SDL as a shared library, from the SDL sources the game uses.
sdl=$HOME/brawl-linux/sdl-shared
if [[ ! -f $sdl/libSDL3.so.0 ]]; then
    cmake -S "$work/build/_deps/sdl-src" -B "$sdl" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$bin/clang" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    cmake --build "$sdl" --parallel "$jobs"
    echo "DONE: $sdl/libSDL3.so.0"
fi
