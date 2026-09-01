$ErrorActionPreference = 'SilentlyContinue'
Write-Output '=== VS installs ==='
Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' | Select-Object -ExpandProperty FullName
Write-Output '=== vswhere ==='
$vswhere = Join-Path $env:ProgramFiles(x86) 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) { & $vswhere -latest -products * -property installationPath }
Write-Output '=== msbuild ==='
Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter MSBuild.exe -Depth 5 | Select-Object -ExpandProperty FullName
Write-Output '=== cl ==='
Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter cl.exe -Depth 8 | Select-Object -ExpandProperty FullName
Write-Output '=== g++ version ==='
g++ --version | Select-Object -First 1