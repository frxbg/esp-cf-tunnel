param(
    [ValidateSet('esp32s3','esp32p4')][string]$Target = 'esp32s3',
    [ValidateSet('no_psram','psram')][string]$Profile = 'no_psram',
    [switch]$TwoStreams
)
$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
if (-not $env:IDF_PATH) { throw 'Activate ESP-IDF v6.1 first (export.ps1 or EIM terminal).' }
$projectRoot = Split-Path -Parent $PSScriptRoot
$exampleDir = Join-Path $projectRoot 'examples/protocol_smoke'
$suffix = if ($TwoStreams) { '-2streams' } else { '' }
$buildDir = Join-Path $projectRoot "build-idf-$Target-$Profile$suffix"
$defaults = @((Join-Path $exampleDir 'sdkconfig.defaults'), (Join-Path $exampleDir "profiles/$Profile.defaults"))
if ($TwoStreams) { $defaults += Join-Path $exampleDir 'profiles/two_streams.defaults' }
$sdkFile = Join-Path $buildDir 'sdkconfig'
# Each target/profile owns its build tree; no set-target deletion or COM defaults.
# Windows PowerShell 5.1 wraps redirected native stderr as ErrorRecord even
# when IDF only prints progress. Use the process exit code as the result.
$ErrorActionPreference = 'Continue'
& python (Join-Path $env:IDF_PATH 'tools/idf.py') -C $exampleDir -B $buildDir `
    '-D' "IDF_TARGET=$Target" '-D' "SDKCONFIG=$sdkFile" `
    '-D' "SDKCONFIG_DEFAULTS=$($defaults -join ';')" build
exit $LASTEXITCODE
