# Helper: set up the ESP-IDF v6.1 environment (custom tools path) and run idf.py
# with the given arguments, e.g.:
#   pwsh -NoProfile -File .\idf.ps1 build
#   pwsh -NoProfile -File .\idf.ps1 -p COM3 flash monitor
#
# The ESP-IDF install base defaults to the author's machine; override it with
# the ESP_IDF_BASE environment variable if your install lives elsewhere:
#   $env:ESP_IDF_BASE = 'C:\tools\esp-idf'
#   pwsh -NoProfile -File .\idf.ps1 build
#
# NOTE: export.ps1 fails on this machine because it cannot run tool binaries to
# verify versions (dfu-util/esp-rom-elfs/esp-clangd reported as missing), so we
# set up the environment manually and skip the tool check instead.

Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
$idfBase = if ($env:ESP_IDF_BASE) { $env:ESP_IDF_BASE } else { 'D:\01MyProject\01ESP\v6.1' }
$env:IDF_PATH = "$idfBase\esp-idf"
$env:IDF_TOOLS_PATH = "$idfBase\tools"
$env:IDF_PYTHON_ENV_PATH = "$idfBase\tools\python\v6.1\venv"
$env:IDF_SKIP_TOOLS_CHECK = '1'
$env:ESP_IDF_VERSION = '6.1.0'

$bins = @(
    "$idfBase\tools\python\v6.1\venv\Scripts",
    "$idfBase\tools\idf-exe\1.0.3",
    "$idfBase\tools\cmake\4.0.3\bin",
    "$idfBase\tools\ninja\1.12.1",
    "$idfBase\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
    "$idfBase\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin",
    "$idfBase\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin",
    "$idfBase\tools\esp-clang\esp-21.1.3_20260408\esp-clang\bin"
)
$env:PATH = ($bins + $env:PATH) -join ';'

Set-Location $PSScriptRoot
& idf.py @args
exit $LASTEXITCODE
