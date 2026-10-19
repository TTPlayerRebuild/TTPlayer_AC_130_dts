[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$PreviousVersion,
    [Parameter(Mandatory = $true)][string]$Repository,
    [Parameter(Mandatory = $true)][string]$Commit,
    [string]$ArtifactDirectory = 'artifact',
    [string]$ServerUrl = 'https://github.com'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
$version = (Get-Ac3DtsBuildVersion $Version).Name
if ($PreviousVersion -and -not (Read-Ac3DtsVersionTag $PreviousVersion)) { throw 'Invalid previous release version.' }
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or $Commit -notmatch '^[0-9a-fA-F]{40}$') {
    throw 'Invalid repository or commit.'
}
$archiveName = "ttp_ac3dts-$version.zip"
$hash = (Get-FileHash -LiteralPath (Join-Path $ArtifactDirectory $archiveName) -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceName = "ttp_ac3dts-$version-source.zip"
$sourceHash = (Get-FileHash -LiteralPath (Join-Path $ArtifactDirectory $sourceName) -Algorithm SHA256).Hash.ToLowerInvariant()
$expected = (Get-Content -LiteralPath (Join-Path $ArtifactDirectory 'SHA256SUMS.txt') -Encoding UTF8 -Raw).Trim() -replace "`r", ''
if ($expected -cne "$hash  $archiveName`n$sourceHash  $sourceName") { throw 'AC3DTS package SHA-256 verification failed.' }
$repoUrl = "$ServerUrl/$Repository"
$logUrl = if ($PreviousVersion) { "$repoUrl/compare/$PreviousVersion...$version" } else { "$repoUrl/commits/$version" }
$notes = @"
**更新记录**: $logUrl

解压 ttp_ac3dts-$version.zip，关闭播放器后，将 AddIn/ttp_ac3dts.dll 放入安装目录。
同一 x86 DLL 面向 XP SP3、Win7 和新系统，CPU 需要 SSE2。
运行 ZIP 仅包含 AddIn/ttp_ac3dts.dll 和 SHA256SUMS.txt。测试不在 Action 中运行。
对应源码另见 ttp_ac3dts-$version-source.zip，包含 a52dec 0.8.0 与 libdca 0.0.7 完整固定源码、适配层、构建脚本和许可证。
包含 GPL-2.0-or-later 解码核心的插件整体分发须遵循其条款；源码包内的构建说明支持重建对应版本。

[源码、构建和兼容边界]($repoUrl/tree/$Commit)
[依赖版本与版权许可证]($repoUrl/tree/$Commit/licenses)
"@
$notes | Set-Content -LiteralPath (Join-Path $ArtifactDirectory 'release-notes.md') -Encoding UTF8
