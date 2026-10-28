# run.ps1 -- the unit tests (D47): no game, seconds, also run by CI (.github/workflows/unit.yml).
#   .\tests\unit\run.ps1              build build\unit\fdcore_unit.exe, run it, then the Python tests
#   .\tests\unit\run.ps1 -NoPython    the C++ tests only
# The C++ side compiles core sources for the host with FDCORE_UNIT (stubs.cpp stands in for
# the services they call); the Python side is every tests\unit\test_*.py.
param([switch]$NoPython)

# Native tools report through exit codes; their stderr (compiler notes, unittest's progress)
# must not abort the script, which Windows PowerShell 5.1 does under 'Stop'.
$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $here)
# Zig and Python: the VM's local tools (environment\local\tools, untracked), else PATH (CI).
$tools = Join-Path $repo 'environment\local\tools'
$zig = (Get-ChildItem (Join-Path $tools 'zig*\zig.exe') -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (-not $zig) { $zig = (Get-Command zig -ErrorAction SilentlyContinue).Source }
if (-not $zig) { throw "zig not found: in environment\local\tools\zig*\ or on PATH" }
$PythonExe = Join-Path $tools 'python\python.exe'
if (-not (Test-Path $PythonExe)) { $PythonExe = (Get-Command python -ErrorAction SilentlyContinue).Source }
if (-not $PythonExe) { throw "Python not found: in environment\local\tools\python\ or on PATH" }

# The generated address table header (build.ps1 normally writes it).
& $PythonExe -I (Join-Path $repo 'tools\addrgen.py') header (Join-Path $repo 'core\gen\addresses_gen.h')
if ($LASTEXITCODE -ne 0) { throw "addrgen header failed" }

# Core sources under test: pure logic, no game calls (each listed on purpose).
$under = @('core\addr.cpp', 'core\crashlog.cpp', 'core\cmd.cpp', 'core\binds.cpp', 'features\replay\replay_data.cpp') | ForEach-Object { Join-Path $repo $_ }
$tests = Get-ChildItem $here -Filter *.cpp | ForEach-Object FullName

$outDir = Join-Path $repo 'build\unit'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'fdcore_unit.exe'
# x86 like the game (pointer size, struct layout); the host's C runtime is fine here.
$args = @('c++', '-target', 'x86-windows-gnu', '-std=c++17', '-O1', '-g', '-fno-sanitize=all',
          '-Wall', '-Wextra', '-Werror', '-Wno-nullability-completeness', '-Wno-missing-field-initializers',
          '-DFDCORE_UNIT=1', '-I', (Join-Path $repo 'core'), '-o', $exe) + $under + $tests + @('-lkernel32', '-luser32')
& $zig @args
if ($LASTEXITCODE -ne 0) { throw "unit build failed ($LASTEXITCODE)" }
"built $exe"

Push-Location $repo  # tests read files under tests/game/data/
& $exe
$cppFailed = $LASTEXITCODE -ne 0
Pop-Location

$pyFailed = $false
if (-not $NoPython) {
    "python unit tests"
    & $PythonExe -m unittest discover -s $here -p 'test_*.py' -v
    $pyFailed = $LASTEXITCODE -ne 0
}

if ($cppFailed -or $pyFailed) { throw "unit tests failed (C++: $(-not $cppFailed), Python: $(-not $pyFailed) passed)" }
"unit tests: all passed"
