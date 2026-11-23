[CmdletBinding()]
param(
    [string]$Action = 'build',
    [string]$Port = '',
    [string]$IdfPath = 'D:\esp\v6.1\esp-idf',
    [string]$ToolsPath = 'D:\Espressif',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path $PSScriptRoot -Parent
$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath
$env:IDF_PYTHON_ENV_PATH = Join-Path $ToolsPath 'tools\python\v6.1\venv'
$env:ESP_IDF_VERSION = '6.1.0'
$env:PYTHONUTF8 = '1'
$env:IDF_TARGET = 'esp32'
$env:IDF_CCACHE_ENABLE = '1'
$env:IDF_PY_BUILD_JOBS = '2'
$env:ESP_ROM_ELF_DIR = Join-Path $ToolsPath 'tools\esp-rom-elfs\20241011'
$toolBins = @(
    (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts'),
    (Join-Path $ToolsPath 'tools\cmake\4.0.3\bin'),
    (Join-Path $ToolsPath 'tools\ninja\1.12.1'),
    (Join-Path $ToolsPath 'tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64'),
    (Join-Path $ToolsPath 'tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin'),
    (Join-Path $ToolsPath 'tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin')
)
$env:PATH = ($toolBins -join ';') + ';' + $env:PATH
$idfPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idfArgs = @((Join-Path $IdfPath 'tools\idf.py'), '-C', $projectPath)
if ($Port) { $idfArgs += @('-p', $Port) }
$actionTokens = @($Action -split '\s+' | Where-Object { $_ })
if ($actionTokens.Count -eq 0) { throw 'Action must contain at least one ESP-IDF action.' }
$idfArgs += $actionTokens
Write-Host ('ESP-IDF actions: ' + ($actionTokens -join ' -> '))
if ($Port) { Write-Host ('Serial port: ' + $Port) }
if ($DryRun) {
    # Print the exact argument array without compiling, flashing or opening COM.
    $idfArgs | ConvertTo-Json
    return
}
$ErrorActionPreference = 'Continue' # Windows PowerShell reports native stderr as ErrorRecords.
& $idfPython @idfArgs
$idfExitCode = $LASTEXITCODE
if ($idfExitCode -ne 0) {
    Write-Host ("ESP-IDF failed with exit code $idfExitCode; requested actions did not complete. " +
        'If the build/configuration failed, flashing and monitoring were not started. ' +
        'See the first error above and build/log/idf_py_stderr_output_* .') -ForegroundColor Red
}
exit $idfExitCode
