param(
    [Parameter(Mandatory)][string]$WorkDirectory,
    [string]$SourceDirectory=(Join-Path $PSScriptRoot '../source/shared/windows-icons/artwork'),
    [string]$NodePath='C:\Users\NWittje\.cache\codex-runtimes\codex-primary-runtime\dependencies\node\bin\node.exe',
    [string]$SharpModule='C:\Users\NWittje\.cache\codex-runtimes\codex-primary-runtime\dependencies\node\node_modules\sharp',
    [string]$Findings='docs/operations/windows-icons.md'
)
$ErrorActionPreference='Stop'
$localProject='D:\devel_assist\psx-ios-rebuild-20261008-170322Z\psx_ios'
$localBase=Join-Path $localProject 'bin\.work'
$work=[IO.Path]::GetFullPath($WorkDirectory)
if (-not $work.StartsWith($localBase+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Icon work must be beneath the qualified D: bin/.work root.'
}
if (Test-Path -LiteralPath $work) { throw 'Use a fresh reserved work directory; existing work is preserved.' }
if (-not (Test-Path -LiteralPath $NodePath -PathType Leaf)) { throw 'Qualified Node runtime unavailable.' }
if ([IO.Path]::IsPathRooted($Findings) -or -not (Test-Path -LiteralPath (Join-Path $localProject $Findings) -PathType Leaf)) {
    throw 'Findings must name an existing central document relative to the D: project root.'
}
. (Join-Path $localProject 'scripts/work-cleanup.ps1')
$lease=Enter-WorkCleanup -WorkDirectory $work -Owner 'windows-icons'
$primaryFailure=$null
try {
    & $NodePath (Join-Path $PSScriptRoot 'generate-windows-icons.mjs') `
        --source-directory $SourceDirectory --output-directory (Join-Path $work 'generated') --sharp-module $SharpModule
    if ($LASTEXITCODE -ne 0) { throw "Icon renderer failed: $LASTEXITCODE" }
    Set-WorkCleanupDisposition -Lease $lease -State Held `
        -Reason 'Generated icons await independent decoding/visual QA and verified maintained-source collection; review at icon handoff.' `
        -Findings $Findings
} catch {
    $primaryFailure=$_
    throw
} finally {
    try { Exit-WorkCleanup -Lease $lease } catch {
        Write-Warning "Icon cleanup lease release failed separately: $_"
        if (-not $primaryFailure) { throw }
    }
}
