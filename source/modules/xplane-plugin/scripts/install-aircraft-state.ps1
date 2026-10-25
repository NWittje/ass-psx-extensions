[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AircraftDirectory,
    [Parameter(Mandatory=$true)][string]$AdapterDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ManifestSha256,
    [string]$BackupDirectory,
    [switch]$Apply,
    [string]$EvidenceDirectory,
    [string]$StagingDirectory,
    [string]$CopyHelperPath=(Join-Path $PSScriptRoot 'common.ps1')
)
$ErrorActionPreference = 'Stop'
$aircraftRoot = (Resolve-Path -LiteralPath $AircraftDirectory).Path.TrimEnd('\','/')
$adapterRoot = (Resolve-Path -LiteralPath $AdapterDirectory).Path.TrimEnd('\','/')
function ChildPath([string]$Root,[string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative)) { throw "Absolute manifest path rejected: $Relative" }
    $resolved = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (-not $resolved.StartsWith($Root + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest path escapes root: $Relative"
    }
    $ancestor = $resolved
    while ($ancestor.Length -ge $Root.Length) {
        if (Test-Path -LiteralPath $ancestor) {
            if ((Get-Item -LiteralPath $ancestor).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse point rejected: $ancestor"
            }
        }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
        if (-not $ancestor) { break }
    }
    return $resolved
}
function Hash([string]$Path) {
    # Stream large aircraft meshes without depending on shell module autoload.
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','') }
    finally { $sha.Dispose(); $stream.Dispose() }
}
$manifestPath = ChildPath $adapterRoot 'adapter-manifest.json'
if ((Hash $manifestPath) -ne $ManifestSha256) { throw 'Adapter manifest identity mismatch' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.adapter -ne 1 -or -not $manifest.geometry_unchanged) { throw 'Unsupported adapter manifest' }
if ((Hash (ChildPath $aircraftRoot '747-400.acf')) -ne $manifest.original_entry_sha256) { throw 'Original entry changed' }
$xpRoot = [IO.Directory]::GetParent([IO.Directory]::GetParent($aircraftRoot).FullName).FullName
foreach ($resource in @(@('lights.txt',$manifest.lights_txt_sha256),@('1000_lights_close.png',$manifest.atlas_sha256))) {
    if ((Hash (ChildPath $xpRoot ('Resources/bitmaps/world/lites/' + $resource[0]))) -ne $resource[1]) {
        throw "Installed light resource changed: $($resource[0])"
    }
}
$changes = [Collections.Generic.List[object]]::new()
foreach ($entry in $manifest.files) {
    $payload = ChildPath $adapterRoot $entry.target
    $target = ChildPath $aircraftRoot $entry.target
    if ((Hash $payload) -ne $entry.sha256) { throw "Payload identity mismatch: $($entry.target)" }
    if (-not $entry.generated) {
        $source = ChildPath $aircraftRoot $entry.source
        $sourceHash = Hash $source
        $permittedAdapted = $entry.source -eq $entry.target -and $sourceHash -eq $entry.sha256
        if ($sourceHash -ne $entry.source_sha256 -and -not $permittedAdapted) { throw "User/source change: $($entry.source)" }
    }
    $existing = if (Test-Path -LiteralPath $target) { Hash $target } else { $null }
    if ($existing -eq $entry.sha256) { continue }
    if ($existing -and ($entry.generated -or $entry.source -ne $entry.target -or $existing -ne $entry.source_sha256)) {
        throw "Unexpected target; preserve and re-audit: $($entry.target)"
    }
    $changes.Add([pscustomobject]@{Relative=$entry.target;Payload=$payload;Target=$target;Before=$existing;BeforeBytes=$(if($existing){(Get-Item -LiteralPath $target).Length}else{0});Bytes=(Get-Item -LiteralPath $payload).Length;After=$entry.sha256})
}
$installedManifest = ChildPath $aircraftRoot 'adapter-manifest.json'
if (Test-Path -LiteralPath $installedManifest) {
    if ((Hash $installedManifest) -ne $ManifestSha256) { throw 'Existing adapter manifest differs; preserve and re-audit' }
} else {
    $changes.Add([pscustomobject]@{Relative='adapter-manifest.json';Payload=$manifestPath;Target=$installedManifest;Before=$null;BeforeBytes=0;Bytes=(Get-Item -LiteralPath $manifestPath).Length;After=$ManifestSha256})
}
if (-not $BackupDirectory) { $BackupDirectory = Join-Path $aircraftRoot ('psx-state-backups/' + $ManifestSha256.Substring(0,16)) }
$backupRoot = [IO.Path]::GetFullPath($BackupDirectory).TrimEnd('\','/')
if ($backupRoot -eq $aircraftRoot -or $backupRoot -eq $adapterRoot) { throw 'Invalid backup root' }
foreach ($change in $changes) {
    if ($change.Before) {
        $backup = ChildPath $backupRoot $change.Relative
        if ((Test-Path -LiteralPath $backup) -and (Hash $backup) -ne $change.Before) { throw "Existing backup differs: $backup" }
    }
}
$running = @(Get-Process -Name 'X-Plane' -ErrorAction SilentlyContinue).Count -gt 0
if ($Apply -and $running) { throw 'X-Plane must be closed; this installer never terminates or restarts it' }
if ($Apply) {
    if(-not $EvidenceDirectory -or -not $StagingDirectory){throw 'Apply requires owner-leased O EvidenceDirectory and caller-owned same-volume StagingDirectory.'}
    $evidenceRoot=[IO.Path]::GetFullPath($EvidenceDirectory)
    $stageRoot=[IO.Path]::GetFullPath($StagingDirectory)
    if(-not $evidenceRoot.StartsWith('O:\psx\bin\.work\',[StringComparison]::OrdinalIgnoreCase)){throw 'Transfer evidence must remain under actual owner-leased O work.'}
    if([IO.Path]::GetPathRoot($stageRoot) -ine [IO.Path]::GetPathRoot($aircraftRoot) -or $stageRoot -ieq $aircraftRoot -or $stageRoot -ieq $adapterRoot){throw 'Provide a distinct caller-owned staging directory on the aircraft target volume.'}
    if(-not(Test-Path -LiteralPath $CopyHelperPath -PathType Leaf)){throw 'Portable sibling common.ps1 is missing; no copy was attempted.'}
    . $CopyHelperPath
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$role=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($aircraftRoot.ToUpperInvariant()))).Replace('-','')}finally{$sha.Dispose()}
    $mutex=[Threading.Mutex]::new($false,('Local\PSXIOS.AircraftStateInstall.'+$role))
    $locked=$false
    $applied=[Collections.Generic.List[object]]::new()
    $pendingCopy=$null
    try{
        try{$locked=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$locked=$true}
        if(-not $locked){throw 'Another aircraft-state transaction owns this target.'}
        # All original backups are independently verified before any target is
        # replaced. Existing matching backups are preserved, never repurposed
        # as the fresh atomic-commit backup required by the shared copy API.
        $backups=@()
        foreach($change in $changes){
            SyncIdentity $change.Payload $change.Bytes $change.After
            if($change.Before){
                SyncIdentity $change.Target $change.BeforeBytes $change.Before
                $backups+=@{Source=$change.Target;Target=(ChildPath $backupRoot $change.Relative);Bytes=$change.BeforeBytes;SHA256=$change.Before}
            }elseif(Test-Path -LiteralPath $change.Target){throw 'New target appeared after preflight'}
        }
        SyncTransfer $backups $evidenceRoot 'C:\Program Files\TeraCopy\TeraCopy.exe'|Out-Null
        foreach($backup in $backups){SyncIdentity $backup.Source $backup.Bytes $backup.SHA256;SyncIdentity $backup.Target $backup.Bytes $backup.SHA256}
        foreach($change in $changes){
            SyncIdentity $change.Payload $change.Bytes $change.After
            $copy=@{Source=$change.Payload;Target=$change.Target;Bytes=$change.Bytes;SHA256=$change.After;StagingDirectory=$stageRoot}
            if($change.Before){
                SyncIdentity $change.Target $change.BeforeBytes $change.Before
                $copy.ReplaceExisting=$true;$copy.ExpectedBytes=$change.BeforeBytes;$copy.ExpectedSHA256=$change.Before
                $copy.Backup=ChildPath $stageRoot ('preimage-'+[Guid]::NewGuid().ToString('N'))
            }elseif(Test-Path -LiteralPath $change.Target){throw 'New target appeared after preflight'}
            $pendingCopy=[pscustomobject]@{Change=$change;Backup=$(if($change.Before){$copy.Backup}else{$null})}
            $result=@(SyncCopyFile $copy $evidenceRoot 'C:\Program Files\TeraCopy\TeraCopy.exe'|Where-Object {$_.PSObject.Properties['Verified']})
            if($result.Count -ne 1 -or -not $result[0].Verified){throw 'Copy helper returned no verified commit'}
            if($result[0].Changed){$applied.Add([pscustomobject]@{Change=$change;Backup=$result[0].Backup})}
            $pendingCopy=$null
            SyncIdentity $change.Target $change.Bytes $change.After
        }
    }catch{
        $failure=$_
        # A fresh, exact commit backup proves a replacement completed even if
        # the helper's final verification/reporting threw before returning.
        # Never infer ownership of an unacknowledged fresh target or foreign edit.
        if($pendingCopy){
            if($pendingCopy.Change.Before -and (Test-Path -LiteralPath $pendingCopy.Backup)){
                try{
                    SyncIdentity $pendingCopy.Backup $pendingCopy.Change.BeforeBytes $pendingCopy.Change.Before
                    SyncIdentity $pendingCopy.Change.Target $pendingCopy.Change.Bytes $pendingCopy.Change.After
                    $applied.Add($pendingCopy)
                }catch{Write-Warning "Unverified target/preimage retained for caller disposition: $($pendingCopy.Change.Target); $stageRoot; $evidenceRoot"}
            }else{Write-Warning "Unverified copy attempt retained for caller disposition: $($pendingCopy.Change.Target); $stageRoot; $evidenceRoot"}
        }
        # Roll back only our exact postimages. Native same-volume namespace
        # commits preserve foreign edits and retain rejected images for the
        # caller's disposition; this is no copy-route or cleanup fallback.
        for($i=$applied.Count-1;$i -ge 0;$i--){
            $item=$applied[$i];$change=$item.Change
            try{
                SyncIdentity $change.Target $change.Bytes $change.After
                $rejected=ChildPath $stageRoot ('rejected-'+[Guid]::NewGuid().ToString('N'))
                if($change.Before){SyncIdentity $item.Backup $change.BeforeBytes $change.Before;[IO.File]::Replace($item.Backup,$change.Target,$rejected);SyncIdentity $change.Target $change.BeforeBytes $change.Before}
                else{[IO.File]::Move($change.Target,$rejected)}
            }catch{Write-Warning "Separate rollback failure; preserve target and staging: $($_.Exception.Message)"}
        }
        throw $failure
    }finally{if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
}
[pscustomobject]@{Mode=$(if($Apply){'Apply'}else{'DryRun'});Manifest=$ManifestSha256.ToUpperInvariant();
    Aircraft=$aircraftRoot;Backup=$backupRoot;XPlaneRunning=$running;FilesToChange=$changes.Count;
    Evidence=$EvidenceDirectory;Staging=$StagingDirectory;
    ExistingFilesToBackup=@($changes | Where-Object Before).Count;Changes=@($changes)} | ConvertTo-Json -Depth 4
