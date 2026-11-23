[CmdletBinding()]
param(
    [string]$ArduinoCli,
    [string]$ConfigFile = (Join-Path $env:USERPROFILE '.arduinoIDE\arduino-cli.yaml'),
    [string]$CoreVersion = '3.3.12',
    [string]$Fqbn = 'esp32:esp32:esp32da:CPUFreq=240,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=default,DebugLevel=none,LoopCore=1,EventsCore=1',
    [string]$BuildDirectory = (Join-Path $env:TEMP 'microphone-spp-release-build')
)

$ErrorActionPreference = 'Stop'
if (-not $ArduinoCli) {
    $cliCommand = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($cliCommand) {
        $ArduinoCli = $cliCommand.Source
    } else {
        # Arduino IDE bundles its own CLI, which may not be on PATH.
        $cliProcess = Get-CimInstance Win32_Process -Filter "name = 'arduino-cli.exe'" |
            Where-Object { $_.ExecutablePath } | Select-Object -First 1
        if ($cliProcess) { $ArduinoCli = $cliProcess.ExecutablePath }
    }
}
if (-not $ArduinoCli) {
    throw 'Arduino CLI not found. Open Arduino IDE or pass -ArduinoCli with its full path.'
}
if ($Fqbn -notmatch '^esp32:esp32:(esp32|esp32da)(:|$)') {
    throw 'This build script targets classic ESP32 / ESP32-WROOM-DA with SPP support.'
}
$configArgs = @()
if (Test-Path -LiteralPath $ConfigFile) { $configArgs = @('--config-file', $ConfigFile) }
$coreJson = & $ArduinoCli core list --json @configArgs
if ($LASTEXITCODE -ne 0) { throw 'Could not list the installed Arduino cores.' }
$installedCore = ($coreJson | ConvertFrom-Json).platforms |
    Where-Object { $_.id -eq 'esp32:esp32' } | Select-Object -First 1
if (-not $installedCore -or $installedCore.installed_version -ne $CoreVersion) {
    throw "Install esp32 by Espressif Systems $CoreVersion in Boards Manager before building."
}

Write-Host "Building Arduino-ESP32 $CoreVersion with -Os (release), without uploading."
& $ArduinoCli compile --fqbn $Fqbn --build-property 'compiler.optimization_flags=-Os' `
    --build-path $BuildDirectory @configArgs $PSScriptRoot
if ($LASTEXITCODE -ne 0) { throw 'Firmware compilation failed.' }

$buildOptions = Get-Content -LiteralPath (Join-Path $BuildDirectory 'build.options.json') -Raw |
    ConvertFrom-Json
if ($buildOptions.'compiler.optimization_flags' -ne '-Os') {
    throw 'Unexpected compiler optimization flags in the build output.'
}
Write-Host ('Application binary: ' + (Join-Path $BuildDirectory 'singlemic.ino.bin'))
Write-Host 'Upload from Arduino IDE with Optimize for Debugging disabled, using the matching board/pin settings.'
