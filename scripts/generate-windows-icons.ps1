param(
    [Parameter(Mandatory)][string]$WorkDirectory,
    [string]$SourceDirectory=(Join-Path $PSScriptRoot '../source/shared/windows-icons/artwork'),
    [string]$NodePath='C:\Users\NWittje\.cache\codex-runtimes\codex-primary-runtime\dependencies\node\bin\node.exe',
    [string]$SharpModule='C:\Users\NWittje\.cache\codex-runtimes\codex-primary-runtime\dependencies\node\node_modules\sharp',
    [string]$Findings='docs/operations/windows-icons.md'
)
$ErrorActionPreference='Stop'
$localProject=switch ($env:COMPUTERNAME.ToUpperInvariant()) {
    'NWITTJE-MAIN' { 'H:\psx' }
    'B744-AVIONIC' { 'D:\dev-assist' }
    default { throw 'No approved icon work root is configured for this computer.' }
}
if (-not (Test-Path -LiteralPath $localProject -PathType Container)) {
    throw "Selected icon work root is unavailable: $localProject. Preserve inputs; no fallback is permitted."
}
$maintainedProject='P:\development\psx_ios'
$localBase=Join-Path $localProject 'bin\.work'
$work=[IO.Path]::GetFullPath($WorkDirectory)
if (-not $work.StartsWith($localBase+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw "Icon work must be beneath $localBase."
}
if (Test-Path -LiteralPath $work) { throw 'Use a fresh work directory; existing work is preserved.' }
if (-not (Test-Path -LiteralPath $NodePath -PathType Leaf)) { throw 'Qualified Node runtime unavailable.' }
if ([IO.Path]::IsPathRooted($Findings) -or $Findings -notmatch '^docs[\\/]' -or
    $Findings -match '(^|[\\/])\.\.([\\/]|$)|[:*?]' -or
    -not (Test-Path -LiteralPath (Join-Path $maintainedProject $Findings) -PathType Leaf)) {
    throw 'Findings must name an existing central document relative to the maintained P: project.'
}
. (Join-Path $maintainedProject 'scripts/work-cleanup.ps1') -CleanupProjectRoot $localProject
$lease=Enter-WorkCleanup -WorkDirectory $work -Owner 'windows-icons'
$primaryFailure=$null
$completed=$false
$savedEnvironment=@{}
foreach($name in 'TEMP','TMP','TMPDIR') {
    $savedEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
}
try {
    $temporary=Join-Path $work 'temporary'
    [IO.Directory]::CreateDirectory($temporary) | Out-Null
    foreach($name in 'TEMP','TMP','TMPDIR') {
        [Environment]::SetEnvironmentVariable($name,$temporary,'Process')
    }
    & $NodePath (Join-Path $PSScriptRoot 'generate-windows-icons.mjs') `
        --source-directory $SourceDirectory --output-directory (Join-Path $work 'generated') --sharp-module $SharpModule
    if ($LASTEXITCODE -ne 0) { throw "Icon renderer failed: $LASTEXITCODE" }
    $completed=$true
} catch {
    $primaryFailure=$_
    throw
} finally {
    foreach($entry in $savedEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key,$(if($null -eq $entry.Value){[NullString]::Value}else{$entry.Value}),'Process')
    }
    try {
        $reason=if($completed) {
            'Generated icons await independent decoding/visual QA and verified maintained-source collection; review after verification.'
        } else {
            'Icon generation failed; owner must triage partial output before retry or cleanup.'
        }
        Set-WorkCleanupDisposition -Lease $lease -State Held -Reason $reason -Findings $Findings
    } catch {
        Write-Warning "Icon disposition failed separately: $_"
        if (-not $primaryFailure) { $primaryFailure=$_; throw }
    } finally {
        try { Exit-WorkCleanup -Lease $lease } catch {
            Write-Warning "Icon cleanup lease release failed separately: $_"
            if (-not $primaryFailure) { throw }
        }
    }
}
