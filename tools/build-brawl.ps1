# One-shot Super Smash Bros. Brawl build: portable tools -> pinned dependencies -> translate -> configure -> compile.
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File <project>\tools\build-brawl.ps1
# Everything it downloads is the pinned set WiiCompiled's own Launcher scripts fetch (llvm-mingw, CMake,
# Ninja, SDL3, Dawn and the other Dependencies/ trees); nothing else is installed. Logs: <project>\build-logs\.
[CmdletBinding()]
param(
    [int]$Jobs = 0,
    [switch]$SkipTools,
    [switch]$SkipTranslate,
    [switch]$ConfigureOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$root = Split-Path -Parent $PSScriptRoot            # <project>\tools -> <project>
$wc = Join-Path $root 'wiicompiled'
$toolchain = Join-Path $root 'toolchain'
$logs = Join-Path $root 'build-logs'
$project = 'projects/brawl/recomp.yml'
$workspace = $wc        # the translator's cwd: runtime\, generated\ and native-build\ live under it
# Play\SuperSmashBrosBrawl.exe (the loader, runtime\src\product\engine_loader.cpp) plus the engine module engine_usa.dll.
$module = 'engine_usa'
$game = 'game'
if ($Jobs -le 0) {
    # A translated shard compile can take ~2.5 GB; one job per core exhausted a 16 GB machine.
    $ramGb = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB
    $Jobs = [Math]::Max(1, [Math]::Min([Environment]::ProcessorCount, [int][Math]::Floor($ramGb / 2.5)))
}
[IO.Directory]::CreateDirectory($logs) | Out-Null
. (Join-Path $wc 'Launcher\NativeBuildFlags.ps1')

function Set-Status([string]$Text) {
    $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $Text
    Write-Host $line
    Add-Content -LiteralPath (Join-Path $logs 'status.txt') -Value $line
}

# Runs a native tool, keeps its output in build-logs\<name>.log and stops on a non-zero exit code.
function Invoke-Step([string]$Name, [string]$Exe, [string[]]$Arguments, [string]$WorkDir) {
    $log = Join-Path $logs "$Name.log"
    Set-Status "${Name}: started"
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'            # Windows PowerShell 5.1 turns native stderr into errors
    Push-Location $WorkDir
    try { & $Exe @Arguments 2>&1 | ForEach-Object { "$_" } | Out-File -LiteralPath $log -Encoding utf8 }
    finally { Pop-Location; $ErrorActionPreference = $previous }
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        Set-Status "${Name}: FAILED (exit $code), see $log"
        Get-Content -LiteralPath $log -Tail 40
        throw "$Name failed with exit code $code."
    }
    Set-Status "${Name}: ok"
}

Remove-Item -LiteralPath (Join-Path $logs 'status.txt') -ErrorAction SilentlyContinue
Set-Status "build started, project $root, $Jobs jobs"

foreach ($required in @((Join-Path $root "$game\main.dol"), (Join-Path $wc $project))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing $required" }
}

# 1. Toolchain and dependencies
if (-not $SkipTools) {
    if (-not (Test-Path -LiteralPath (Join-Path $toolchain 'llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe'))) {
        Set-Status 'tools: downloading llvm-mingw, CMake and Ninja'
        & (Join-Path $wc 'Launcher\Prepare-PortableTools.ps1') -Destination $toolchain *>&1 |
            ForEach-Object { "$_" } | Out-File -LiteralPath (Join-Path $logs 'tools.log') -Encoding utf8
    }
    if (-not (Test-Path -LiteralPath (Join-Path $wc 'Dependencies\dawn_prebuilt\bin\webgpu_dawn.dll'))) {
        Set-Status 'dependencies: downloading the pinned source trees and Dawn'
        & (Join-Path $wc 'Launcher\Prepare-Dependencies.ps1') -Destination (Join-Path $wc 'Dependencies') *>&1 |
            ForEach-Object { "$_" } | Out-File -LiteralPath (Join-Path $logs 'dependencies.log') -Encoding utf8
    }
    foreach ($tool in @('llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe', 'CMake\bin\cmake.exe', 'Ninja\ninja.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $toolchain $tool))) { throw "Toolchain file missing after setup: $tool (see build-logs\tools.log)" }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $wc 'Dependencies\dawn_prebuilt\bin\webgpu_dawn.dll'))) {
        throw 'Dependencies are incomplete (see build-logs\dependencies.log)'
    }
    Set-Status 'tools and dependencies ready'
}

# Accessibility downloads. The configure step below runs offline (FETCHCONTENT_FULLY_DISCONNECTED), so the files the
# runtime's CMake would fetch are downloaded here, checked against the hashes pinned in runtime\cmake\, and handed to
# it as local copies. A failed download only turns that feature off.
function Get-PinnedValue([string]$Pin, [string]$Name) {
    $text = Get-Content -LiteralPath (Join-Path $wc "runtime\cmake\$Pin") -Raw
    return [regex]::Match($text, "$Name `"([^`"]+)`"").Groups[1].Value
}
function Get-PinnedDownload([string]$Feature, [string]$Uri, [string]$Destination, [string]$Hash) {
    if ($SkipTools -or -not $Hash -or (Test-Path -LiteralPath $Destination)) { return }
    Set-Status "${Feature}: downloading $(Split-Path -Leaf $Uri)"
    $temporary = "$Destination.part"
    try {
        Invoke-WebRequest -UseBasicParsing -OutFile $temporary -Uri $Uri
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $Hash) { throw 'hash mismatch' }
        Move-Item -LiteralPath $temporary -Destination $Destination -Force
    } catch {
        Remove-Item -LiteralPath $temporary -ErrorAction SilentlyContinue
        Set-Status "${Feature}: download failed ($($_.Exception.Message)); building without it"
    }
}
function ConvertTo-CMakePath([string]$Path) { return $Path -replace '\\', '/' }
$accessibilityArguments = @()

# Screen-reader speech: Prism's official Windows release.
$prismVersion = Get-PinnedValue 'PrismPin.cmake' 'MKW_PRISM_VERSION'
$prismArchive = Join-Path $toolchain "prism-windows-x64-$prismVersion.zip"
Get-PinnedDownload 'speech' "https://github.com/ethindp/prism/releases/download/v$prismVersion/prism-windows-x64.zip" `
    $prismArchive (Get-PinnedValue 'PrismPin.cmake' 'MKW_PRISM_WINDOWS_SHA256')
if (Test-Path -LiteralPath $prismArchive) { $accessibilityArguments += "-DMKW_PRISM_WINDOWS_ARCHIVE=$(ConvertTo-CMakePath $prismArchive)" }

# Spatial audio: Valve's Steam Audio SDK and its licence text, from the same release tag.
$steamAudioVersion = Get-PinnedValue 'SteamAudioPin.cmake' 'MKW_STEAM_AUDIO_VERSION'
$steamAudioArchive = Join-Path $toolchain "steamaudio_$steamAudioVersion.zip"
$steamAudioLicense = Join-Path $toolchain "steamaudio-$steamAudioVersion-LICENSE.md"
Get-PinnedDownload 'spatial audio' `
    "https://github.com/ValveSoftware/steam-audio/releases/download/v$steamAudioVersion/steamaudio_$steamAudioVersion.zip" `
    $steamAudioArchive (Get-PinnedValue 'SteamAudioPin.cmake' 'MKW_STEAM_AUDIO_SHA256')
Get-PinnedDownload 'spatial audio' `
    "https://raw.githubusercontent.com/ValveSoftware/steam-audio/v$steamAudioVersion/LICENSE.md" `
    $steamAudioLicense (Get-PinnedValue 'SteamAudioPin.cmake' 'MKW_STEAM_AUDIO_LICENSE_SHA256')
if ((Test-Path -LiteralPath $steamAudioArchive) -and (Test-Path -LiteralPath $steamAudioLicense)) {
    $accessibilityArguments += "-DMKW_STEAM_AUDIO_ARCHIVE=$(ConvertTo-CMakePath $steamAudioArchive)"
    $accessibilityArguments += "-DMKW_STEAM_AUDIO_LICENSE_FILE=$(ConvertTo-CMakePath $steamAudioLicense)"
}

# 2. Translate (.NET 8 runtime; the translator is already built)
if (-not $SkipTranslate) {
    $dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
    if (-not $dotnet) { throw 'dotnet was not found on PATH (the .NET 8 runtime is enough).' }
    $env:DOTNET_ROLL_FORWARD = 'Major'
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $translator = Join-Path $wc 'translator\src\Translator.Cli\bin\Release\net8.0\Translator.Cli.dll'
    Invoke-Step 'translate' $dotnet.Source @($translator, 'translate-recursive', '0x8000403C', '--project', $project,
        '--outdir', 'generated/functions', '--output-metadata', 'generated/base_translation_output.json',
        '--production-source-bundle', 'generated/base_translation_sources.bin',
        '--no-function-files', '--no-state-free-abi', '--data-in-text', 'projects/brawl/resident_data_ranges.txt', '--prune-stale', '--threads', "$Jobs") $workspace
    # emit-base-manifest (WiiCompiled's own mod manifest) needs the project's single REL; Brawl's modules come later.
    Invoke-Step 'data-init' $dotnet.Source @($translator, 'generate-data-init', '--project', $project) $workspace
    Invoke-Step 'shards' $dotnet.Source @($translator, 'emit-build-shards', '--project', $project,
        '--base-metadata', 'generated/base_translation_output.json', '--base-functions-dir', 'generated/functions',
        '--native-source-dir', 'runtime/src', '--out', 'generated/build_shards', '--base-shards', '600') $workspace
}

# 3. Configure and compile with the portable toolchain on a scrubbed PATH
$env:PATH = Get-MkwToolchainPath $toolchain
$bin = Join-Path $toolchain 'llvm-mingw\bin'
$build = Join-Path $workspace 'native-build'
$configure = Get-MkwNativeConfigureArguments `
    -SourceDirectory (Join-Path $workspace 'runtime') -BuildDirectory $build `
    -Ninja (Join-Path $toolchain 'Ninja\ninja.exe') `
    -CCompiler (Join-Path $bin 'x86_64-w64-mingw32-clang.exe') `
    -CxxCompiler (Join-Path $bin 'x86_64-w64-mingw32-clang++.exe') `
    -ResourceCompiler (Join-Path $bin 'x86_64-w64-mingw32-windres.exe') `
    -DependenciesDirectory (Join-Path $wc 'Dependencies') `
    -AdditionalArguments (@("-DMKW_TRANSLATED_COMPILE_JOBS=$Jobs", '-DMKW_ENGINE_MODULE=ON', "-DMKW_ENGINE_MODULE_NAME=$module") + $accessibilityArguments)
$cmake = Join-Path $toolchain 'CMake\bin\cmake.exe'
Invoke-Step 'configure' $cmake $configure $workspace
if ($ConfigureOnly) { Set-Status 'configure only: done'; exit 0 }
Invoke-Step 'compile' $cmake @('--build', $build, '--target', 'WiiCompiled', 'WiiCompiledLoader', '--parallel', "$Jobs") $workspace
Set-Status "DONE: $build\$module.dll"
# Only the copy running from Play\ locks the files we overwrite; sweeps run their own copy under autotest\ and must survive.
$playExe = [IO.Path]::GetFullPath((Join-Path $root 'Play\SuperSmashBrosBrawl.exe'))
Get-Process SuperSmashBrosBrawl -ErrorAction SilentlyContinue | Where-Object { $_.Path -and [IO.Path]::GetFullPath($_.Path) -ieq $playExe } | Stop-Process -Force; Start-Sleep -Seconds 1
# A killed game can leave a zombie process (one thread stuck in a driver) holding the old files open for a long
# time; Windows still lets them be renamed, so the copy moves such a file aside and the leftovers go once freed.
Get-ChildItem (Join-Path $root 'Play') -Filter '*.locked-*' | ForEach-Object { Remove-Item -LiteralPath $_.FullName -ErrorAction SilentlyContinue }
function Copy-ReplacingLocked($from, $to) {
    try { Copy-Item -LiteralPath $from -Destination $to -Force -ErrorAction Stop }
    catch {
        Move-Item -LiteralPath $to -Destination "$to.locked-$([DateTime]::Now.ToString('HHmmss'))" -Force
        Copy-Item -LiteralPath $from -Destination $to -Force
    }
}
Copy-ReplacingLocked (Join-Path $build "$module.dll") (Join-Path $root "Play\$module.dll")
Copy-ReplacingLocked (Join-Path $build 'WiiCompiled.exe') $playExe
Set-Status "copied to Play\$module.dll and Play\SuperSmashBrosBrawl.exe"
# Screen-reader speech (when the build has it): Prism, loaded at run time, and the speech wording.
if (Test-Path -LiteralPath (Join-Path $build 'prism.dll')) {
    Copy-ReplacingLocked (Join-Path $build 'prism.dll') (Join-Path $root 'Play\prism.dll')
    $wording = Join-Path $root 'Play\accessibility'
    [IO.Directory]::CreateDirectory($wording) | Out-Null
    Copy-Item -Path (Join-Path $build 'accessibility\*') -Destination $wording -Recurse -Force
    Set-Status 'copied speech: Play\prism.dll and Play\accessibility'
}
# Spatial audio (when the build has it): Steam Audio, loaded at run time, and its licence and notices.
if (Test-Path -LiteralPath (Join-Path $build 'phonon.dll')) {
    Copy-ReplacingLocked (Join-Path $build 'phonon.dll') (Join-Path $root 'Play\phonon.dll')
    foreach ($f in 'steamaudio-LICENSE.md', 'steamaudio-THIRDPARTY.md') {
        Copy-Item -LiteralPath (Join-Path $build $f) -Destination (Join-Path $root "Play\$f") -Force
    }
    Set-Status 'copied spatial audio: Play\phonon.dll'
}
# Unattended test run: autotest\autorun.txt present -> start the game (its own autoexit.txt ends it) with logs.
if (Test-Path (Join-Path $root 'autotest\autorun.txt')) {
    $auto = Join-Path $root 'autotest'
    $p = Start-Process -FilePath (Join-Path $root 'Play\SuperSmashBrosBrawl.exe') -WorkingDirectory (Join-Path $root 'Play') -PassThru `
        -RedirectStandardOutput (Join-Path $auto 'run.log') -RedirectStandardError (Join-Path $auto 'run.err.log')
    $p.WaitForExit(600000) | Out-Null
    if (-not $p.HasExited) { $p.Kill() }
    "autorun finished $(Get-Date -f s)" | Out-File (Join-Path $auto 'done.txt') -Encoding utf8
}
