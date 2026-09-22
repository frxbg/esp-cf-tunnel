param([ValidateSet('yd_s3','no_psram')][string]$Profile = 'yd_s3')
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
if (-not $env:IDF_PATH) { throw 'Activate ESP-IDF v6.1 first.' }
$projectRoot = Split-Path -Parent $PSScriptRoot
$exampleDir = Join-Path $projectRoot 'examples/system_monitor'
$buildDir = Join-Path $projectRoot "build-monitor-$Profile"
$defaults = @((Join-Path $exampleDir 'sdkconfig.defaults'), (Join-Path $exampleDir "profiles/$Profile.defaults"))
$ErrorActionPreference = 'Continue'
& python (Join-Path $env:IDF_PATH 'tools/idf.py') -C $exampleDir -B $buildDir `
    '-D' 'IDF_TARGET=esp32s3' '-D' "SDKCONFIG=$buildDir/sdkconfig" `
    '-D' "SDKCONFIG_DEFAULTS=$($defaults -join ';')" build
exit $LASTEXITCODE
