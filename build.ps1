[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [string]$Generator = 'Visual Studio 18 2026',
    [string[]]$CMakeArguments = @(),
    [switch]$Package,
    [switch]$SourcePackage,
    [string]$PackageVersion
)
$ErrorActionPreference = 'Stop'
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $PSScriptRoot 'build' }
. (Join-Path $PSScriptRoot 'cmake/version.ps1')
$PackageVersion = (Get-Ac3DtsBuildVersion $PackageVersion).Name
function Invoke-CMake([string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & cmake @Arguments 2>&1 | ForEach-Object { $_.ToString() }
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previous }
    if ($code -ne 0) { throw "CMake failed ($code): $Arguments" }
}
Invoke-CMake (@('-S',$PSScriptRoot,'-B',$BuildDirectory,'-G',$Generator,'-A','Win32') + $CMakeArguments +
    @("-DTTP_AC3DTS_BUILD_VERSION=$PackageVersion"))
Invoke-CMake @('--build',$BuildDirectory,'--config','Release','--target','ttp_ac3dts','--parallel','4')
Assert-Ac3DtsFileVersion (Join-Path $BuildDirectory 'Release/ttp_ac3dts.dll') $PackageVersion
if (-not ($Package -or $SourcePackage)) { return }
$output = Join-Path $BuildDirectory 'Release'
$stagingRoot = Join-Path $BuildDirectory ('package-' + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $stagingRoot 'binary'
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Invoke-CMake @('--install',$BuildDirectory,'--config','Release','--component','Runtime','--prefix',$stage)
$hash = (Get-FileHash -LiteralPath (Join-Path $stage 'AddIn/ttp_ac3dts.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  AddIn/ttp_ac3dts.dll" | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Encoding UTF8
$archive = Join-Path $output "ttp_ac3dts-$PackageVersion.zip"
Compress-Archive -LiteralPath (Join-Path $stage 'AddIn'),(Join-Path $stage 'SHA256SUMS.txt') -DestinationPath $archive -Force
$archives = @($archive)
if ($SourcePackage) {
    $source = Join-Path $stagingRoot 'source/ttp_ac3dts'
    New-Item -ItemType Directory -Path $source -Force | Out-Null
    # Corresponding source is separate from the minimal runtime package.
    # Explicit allowlist excludes private tests, binaries and local evidence.
    foreach ($entry in @('CMakeLists.txt','build.ps1','README.md','.gitignore',
                         'cmake','src','include','docs','licenses','.github')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $entry) -Destination $source -Recurse
    }
    $PackageVersion | Set-Content -LiteralPath (Join-Path $source 'BUILD_VERSION') -Encoding ASCII
    foreach ($dependency in @('a52','dca')) {
        $upstream = (Get-Content -LiteralPath (Join-Path $BuildDirectory "$dependency-source-path.txt") -Encoding UTF8 -Raw).Trim()
        $destination = Join-Path $source "third_party/$dependency/source"
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        # Complete, fixed upstream release source; patches stay in codec.cmake.
        Get-ChildItem -LiteralPath $upstream -Force | Copy-Item -Destination $destination -Recurse
    }
    $sourceArchive = Join-Path $output "ttp_ac3dts-$PackageVersion-source.zip"
    Compress-Archive -LiteralPath $source -DestinationPath $sourceArchive -Force
    $archives += $sourceArchive
}
Get-FileHash -LiteralPath $archives -Algorithm SHA256 |
    ForEach-Object { '{0}  {1}' -f $_.Hash.ToLowerInvariant(),[IO.Path]::GetFileName($_.Path) } |
    Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding UTF8
Write-Output "AC3DTS packages: $output"
