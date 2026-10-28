#requires -Version 7.0
# Native P/cloud staging, followed by a separately invoked atomic commit.
param(
 [Parameter(Mandatory)][ValidateSet('Stage','Commit')][string]$Operation,
 [Parameter(Mandatory)][string]$PlanPath,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$PlanSHA256
)
$ErrorActionPreference='Stop'
$repo='P:\development\psx_ios'
. "$repo/scripts/deployment/common.ps1"
. "$repo/scripts/work-cleanup.ps1" -CleanupProjectRoot 'O:\psx'
function Enter-WorkCleanupMutex {
 param([string]$Name)
 $mutex=[Threading.Mutex]::new($false,('Local\PSXIOS.WorkCleanup.'+$Name))
 try{try{$locked=$mutex.WaitOne(30000)}catch [Threading.AbandonedMutexException]{$locked=$true};if(!$locked){throw "Actual cleanup mutex remains busy after30s: $Name"};return $mutex}catch{$mutex.Dispose();throw}
}
SyncIdentity $PlanPath (Get-Item -LiteralPath $PlanPath).Length $PlanSHA256
$plan=Get-Content -LiteralPath $PlanPath -Raw|ConvertFrom-Json
if($plan.schema-ne1-or$plan.product-cne'psx-cdu-client'-or$plan.configuration-notin@('Debug','Release')){throw 'Invalid CDU publication plan'}
$configuration=$plan.configuration
if($configuration-cne'Release'){throw 'Project publication is Release-only; Debug candidates remain internal.'}
$candidate=[IO.Path]::GetFullPath($plan.candidate)
$work=Resolve-WorkCleanupPath $plan.work
$commit=[IO.Path]::GetFullPath($plan.commit)
$active=Join-Path $repo "bin/$configuration/psx-cdu-client"
if(!$candidate.StartsWith('O:\psx\bin\.work\',[StringComparison]::OrdinalIgnoreCase)-or
 !$commit.StartsWith("$repo\bin\.work\",[StringComparison]::OrdinalIgnoreCase)){throw 'Candidate/commit root escaped reserved storage'}
foreach($p in @($candidate,$commit,$active)){Assert-WorkCleanupOrdinaryPath $p}
$prepared=Join-Path $commit 'stage/psx-cdu-client'
$previous=Join-Path $commit 'previous/psx-cdu-client'
$rejected=Join-Path $commit 'rejected/psx-cdu-client'
function Inventory([string]$Root){
 foreach($directory in Get-ChildItem -LiteralPath $Root -Recurse -Directory -Force){
  $null=SyncChild $Root ([IO.Path]::GetRelativePath($Root,$directory.FullName))
  if(!@(Get-ChildItem -LiteralPath $directory.FullName -File -Recurse -Force).Count){throw "Unclassified empty directory: $($directory.FullName)"}
 }
 @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force|ForEach-Object{
  $relative=[IO.Path]::GetRelativePath($Root,$_.FullName).Replace('\','/')
  $null=SyncChild $Root $relative
  [ordered]@{path=$relative;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
 }|Sort-Object path)
}
function Verify([string]$Root,$Rows,[switch]$Exact){
 foreach($r in $Rows){SyncIdentity (SyncChild $Root $r.path) $r.bytes $r.sha256}
 if($Exact-and@(Inventory $Root).Count-ne@($Rows).Count){throw "Unexpected file count: $Root"}
}
function Mutable([string]$Path){$Path-in@('settings.cfg','settings.cfg.bak')-or$Path-match'^profiles/[^/]+\.cfg(\.bak)?$'}
function NoClient{
 $running=@(Get-CimInstance Win32_Process|Where-Object{$_.ExecutablePath-and[IO.Path]::GetFullPath($_.ExecutablePath).StartsWith($active+'\',[StringComparison]::OrdinalIgnoreCase)})
 if($running.Count){throw "Close this CDU client normally before publication: $($running.ProcessId-join', ')"}
}
function Populate([string]$Source,[string]$Destination,$Rows){
 foreach($r in $Rows){
  $from=SyncChild $Source $r.path;$to=SyncChild $Destination $r.path
  SyncIdentity $from $r.bytes $r.sha256
  [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($to))|Out-Null
  $reader=[IO.File]::Open($from,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
  try{
   $writer=[IO.File]::Open($to,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
   try{$reader.CopyTo($writer,1048576);$writer.Flush($true)}finally{$writer.Dispose()}
  }finally{$reader.Dispose()}
  SyncIdentity $from $r.bytes $r.sha256;SyncIdentity $to $r.bytes $r.sha256
 }
}
function AssertPublicationAuthority{
 if(!$pLease-or$pLease.Path-ine$commit){throw 'Publication lease authority is unavailable'}
 $null=Assert-WorkCleanupParent -WorkDirectory (Join-Path $commit 'authority-check') -Owner 'cdu-6.1' -Context $pLease.ParentContext -AllowSameProcess
}
function AssertAbsent([string]$Path){
 Assert-WorkCleanupOrdinaryPath $Path
 if(Test-Path -LiteralPath $Path){throw "Unexpected transaction path: $Path"}
}
function CommitPrepared([string]$Active,[string]$Prepared,[string]$Previous,[string]$Rejected,$OldRows,$NewRows){
 AssertPublicationAuthority
 Verify $Prepared $NewRows -Exact
 foreach($p in @($Previous,$Rejected)){if(Test-Path -LiteralPath $p){throw 'Preserve prior transaction; no automatic retry'};[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($p))|Out-Null}
 NoClient
 Verify $Active $OldRows -Exact
 $retired=$false;$installed=$false
 try{
  AssertPublicationAuthority
  [IO.Directory]::Move($Active,$Previous);$retired=$true
  AssertPublicationAuthority
  Verify $Previous $OldRows -Exact;Verify $Prepared $NewRows -Exact;AssertAbsent $Active
  [IO.Directory]::Move($Prepared,$Active);$installed=$true
  Verify $Active $NewRows -Exact;Verify $Previous $OldRows -Exact
 }catch{
  $original=$_
  try{
   # Revalidate both owned trees before any rollback move. A changed candidate,
   # original, namespace or lease belongs to explicit recovery, never blind undo.
   if($retired){
    AssertPublicationAuthority
    Verify $Previous $OldRows -Exact
    if($installed){Verify $Active $NewRows -Exact;AssertAbsent $Rejected}
    else{Verify $Prepared $NewRows -Exact;AssertAbsent $Active}
    if($installed){
     AssertPublicationAuthority
     [IO.Directory]::Move($Active,$Rejected)
    }
    AssertPublicationAuthority
    Verify $Previous $OldRows -Exact
    if($installed){Verify $Rejected $NewRows -Exact}
    AssertAbsent $Active
    [IO.Directory]::Move($Previous,$Active);Verify $Active $OldRows -Exact
   }
  }catch{$script:recovery=$_.ToString();Write-Warning "Separate rollback failure; preserve all paths: $script:recovery" -WarningAction Continue}
  throw $original
 }
}
function ReleaseCduPublicationLease($Lease,$Errors){
 try{Set-WorkCleanupDisposition -Lease $Lease -State Held -Reason 'Exact staged/retired originals and publication proof await independent review; no agent deletion' -Findings 'docs/validation/cdu-options-20261006.md'}
 catch{$Errors.Add($_.Exception);Write-Warning "Secondary publication disposition failure: $_" -WarningAction Continue}
 finally{try{Exit-WorkCleanup $Lease}catch{$Errors.Add($_.Exception);Write-Warning "Secondary publication lease-close failure: $_" -WarningAction Continue}}
}
SyncIdentity (Join-Path $candidate 'manifest.json') $plan.manifestBytes $plan.manifestSHA256
$manifest=Get-Content -LiteralPath (Join-Path $candidate 'manifest.json') -Raw|ConvertFrom-Json
if($manifest.product-cne'psx-cdu-client'-or$manifest.version-cne'0.1.8'-or$manifest.configuration-cne$configuration-or
 $manifest.graphics-cne'1.4.4'-or!$manifest.qualification.clean-or$manifest.qualification.testSummary-cne("$(@($manifest.qualification.testGroups).Count)/$(@($manifest.qualification.testGroups).Count) passed")-or@($manifest.qualification.testGroups).Count-lt28){throw 'Matching complete native qualification required'}
Verify $candidate $plan.candidateFiles -Exact
Verify $candidate $manifest.files
$mutable=@($plan.activeFiles|Where-Object{Mutable $_.path})
$expected=@($plan.candidateFiles)+$mutable
if(@($expected.path|Sort-Object -Unique).Count-ne$expected.Count){throw 'Candidate collides with protected settings'}
$oldManifest=Get-Content -LiteralPath (Join-Path $active 'manifest.json') -Raw|ConvertFrom-Json
Verify $active $oldManifest.files
$known=@($oldManifest.files.path)+@('manifest.json')
foreach($r in $plan.activeFiles){if(!(Mutable $r.path)-and$r.path-notin$known){throw "Unclassified existing asset: $($r.path)"}}
if($Operation-eq'Commit'){NoClient}
$oLease=Enter-WorkCleanup -WorkDirectory $work -Owner 'cdu-6.1'
$pLease=$null;$locked=$false;$success=$false;$failure='';$retired=$false;$installed=$false;$recovery=''
$mutex=$null
try{
 $mutex=[Threading.Mutex]::new($false,'Local\ASS.PSX.CDU.Client.PackagePublication')
 try{$locked=$mutex.WaitOne(10000)}catch [Threading.AbandonedMutexException]{$locked=$true}
 if(!$locked){throw 'Another publisher owns this CDU configuration'}
 . "$repo/scripts/work-cleanup.ps1" -CleanupProjectRoot $repo
function Enter-WorkCleanupMutex {
 param([string]$Name)
 $mutex=[Threading.Mutex]::new($false,('Local\PSXIOS.WorkCleanup.'+$Name))
 try{try{$locked=$mutex.WaitOne(30000)}catch [Threading.AbandonedMutexException]{$locked=$true};if(!$locked){throw "Actual cleanup mutex remains busy after30s: $Name"};return $mutex}catch{$mutex.Dispose();throw}
}
 $pLease=Enter-WorkCleanup -WorkDirectory $commit -Owner 'cdu-6.1'
 [IO.Directory]::CreateDirectory($work)|Out-Null
 Verify $active $plan.activeFiles -Exact
 if($Operation-eq'Stage'){
  if(Test-Path -LiteralPath $prepared){throw 'Preserve existing publication stage; fresh plan required'}
  [IO.Directory]::CreateDirectory($prepared)|Out-Null
  Populate $candidate $prepared $plan.candidateFiles
  Populate $active $prepared $mutable
  Verify $prepared $expected -Exact;Verify $candidate $plan.candidateFiles -Exact;Verify $active $plan.activeFiles -Exact
 }else{
  CommitPrepared $active $prepared $previous $rejected $plan.activeFiles $expected
 }
 $success=$true
}catch{$failure=$_.ToString();throw}
finally{
 $secondary=[Collections.Generic.List[Exception]]::new()
 try{
  [ordered]@{schema=1;operation=$Operation;success=$success;failure=$failure;recoveryFailure=$recovery;planSHA256=$PlanSHA256;active=$active;prepared=$prepared;previous=$previous;immutable=@($plan.candidateFiles).Count;preservedMutable=$mutable;route='Verified native FileStream P/cloud, source-preserving CreateNew; same-volume atomic namespace commit/rollback';utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $work ($configuration+'-'+$Operation+'-result.json')) -Encoding utf8
 }catch{$secondary.Add($_.Exception);Write-Warning "Secondary publication receipt failure: $_" -WarningAction Continue}
 try{if($locked){$mutex.ReleaseMutex()}}catch{$secondary.Add($_.Exception)}
 finally{try{if($mutex){$mutex.Dispose()}}catch{$secondary.Add($_.Exception)}}
 if($pLease){ReleaseCduPublicationLease $pLease $secondary}
 try{. "$repo/scripts/work-cleanup.ps1" -CleanupProjectRoot 'O:\psx';
function Enter-WorkCleanupMutex {
 param([string]$Name)
 $mutex=[Threading.Mutex]::new($false,('Local\PSXIOS.WorkCleanup.'+$Name))
 try{try{$locked=$mutex.WaitOne(30000)}catch [Threading.AbandonedMutexException]{$locked=$true};if(!$locked){throw "Actual cleanup mutex remains busy after30s: $Name"};return $mutex}catch{$mutex.Dispose();throw}
}
}catch{$secondary.Add($_.Exception)}
 ReleaseCduPublicationLease $oLease $secondary
 if($secondary.Count-and!$failure){throw [AggregateException]::new('CDU publication teardown failed',$secondary.ToArray())}
}
