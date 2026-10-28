. 'O:\psx\scripts\work-cleanup.ps1' -CleanupProjectRoot 'O:\psx'

function Assert-DuOwnedWorkPath([string]$Repository,[string]$Directory){
 $source=[IO.Path]::GetFullPath($Repository)
 $root=if((Split-Path $source -Leaf)-eq'source'){[IO.Path]::GetFullPath((Join-Path $source '..'))}else{$source}
 $work='O:\psx\bin\.work'
 if($script:WorkCleanupProjectRoot-ine'O:\psx'){throw 'DU work requires the explicit O cleanup context'}
 if(-not[IO.Path]::IsPathFullyQualified($Directory)){throw 'A fully qualified O work path is required'}
 $target=[IO.Path]::GetFullPath($Directory).TrimEnd('\','/')
 if(!$target.StartsWith($work+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'DU temporary work must remain strictly beneath O:\psx\bin\.work'}
 for($ancestor=$target;$ancestor;$ancestor=[IO.Path]::GetDirectoryName($ancestor)){
  if((Test-Path -LiteralPath $ancestor)-and((Get-Item -LiteralPath $ancestor -Force).Attributes-band[IO.FileAttributes]::ReparsePoint)){throw "Reparse point in DU work ancestry: $ancestor"}
 }
 $target
}

function Assert-DuBuildWorkspace([string]$Repository,[string]$Build,[Nullable[long]]$WorkspaceFileCount=$null){
 [void](Assert-DuOwnedWorkPath $Repository $Build)
 if(!(Test-Path -LiteralPath $Repository -PathType Container)){throw 'DU source root missing'}
 if(Test-Path -LiteralPath $Build){throw 'Fresh DU graph required; never recycle retained or previously rejected work'}
 # A caller may supply a coordinated snapshot as metadata. Counts never decide
 # admission, and this helper never scans the cloud workspace to obtain one.
 $WorkspaceFileCount
}

function Assert-DuWorkRetention([switch]$KeepBuild,[string]$Consumer,[string]$ReviewEvent){
 if($KeepBuild-and([string]::IsNullOrWhiteSpace($Consumer)-or[string]::IsNullOrWhiteSpace($ReviewEvent))){throw 'KeepBuild requires a named current consumer and review/closure event'}
}

function Resolve-DuPhaseReport([string]$Repository,[string]$ReportPath='',[string]$ReportOwner='DUs-6.1'){
 $source=[IO.Path]::GetFullPath($Repository)
 $root=if((Split-Path $source -Leaf)-eq'source'){[IO.Path]::GetFullPath((Join-Path $source '..'))}else{$source}
 $producer='P:\development\psx_ios\docs\validation\du-clean-rebuild-20261008.json'
 if($ReportOwner-notmatch'^[A-Za-z0-9_.-]{1,64}$'){throw 'Explicit valid phase-report owner required'}
 if(!$ReportPath){
  if($ReportOwner-ne'DUs-6.1'){throw 'A consumer must select its own report destination'}
  $ReportPath=$producer
 }
 if(![IO.Path]::IsPathFullyQualified($ReportPath)){throw 'An absolute phase-report destination is required'}
 $path=[IO.Path]::GetFullPath($ReportPath)
 $work='O:\psx\bin\.work'
 $consumer='P:\development\psx_ios\docs\validation\cdu-rebuild-20261008.json'
 $allowed=($ReportOwner-eq'DUs-6.1'-and$path-eq$producer)-or($ReportOwner-eq'cdu-6.1'-and$path-eq$consumer)-or$path.StartsWith($work+'\',[StringComparison]::OrdinalIgnoreCase)
 if(!$allowed-or[IO.Path]::GetExtension($path)-ne'.json'){throw 'Phase report escaped its owned central topic or temporary work'}
 if($path-eq$producer-and$ReportOwner-ne'DUs-6.1'){throw 'A consumer cannot write the DU producer report'}
 for($ancestor=$path;$ancestor;$ancestor=[IO.Path]::GetDirectoryName($ancestor)){
  if((Test-Path -LiteralPath $ancestor)-and((Get-Item -LiteralPath $ancestor -Force).Attributes-band[IO.FileAttributes]::ReparsePoint)){throw 'Reparse point in phase-report ancestry'}
 }
 $path
}

function Assert-DuCleanupDisposition([string]$Repository,[string]$Target,[string]$ReportPath='',[string]$ReportOwner='DUs-6.1'){
 $target=[IO.Path]::GetFullPath($Target).TrimEnd('\','/')
 $source=[IO.Path]::GetFullPath($Repository)
 $root=if((Split-Path $source -Leaf)-eq'source'){[IO.Path]::GetFullPath((Join-Path $source '..'))}else{$source}
 $result=Resolve-DuPhaseReport $Repository $ReportPath $ReportOwner
 # A separate consumer report cannot erase a producer's prior rejection. The
 # producer disposition is read-only here; writes go only to the selected owner.
 $reports=@($result,'P:\development\psx_ios\docs\validation\du-clean-rebuild-20261008.json')|Select-Object -Unique
 foreach($result in $reports){if(Test-Path -LiteralPath $result){
  $previous=Get-Content -LiteralPath $result -Raw|ConvertFrom-Json -AsHashtable
  $dispositions=@($previous.cleanup)+@($previous.phases.Values|ForEach-Object{$_.cleanup})
  foreach($disposition in $dispositions){
   if(!$disposition.path-or$disposition.state-notmatch'failed_preserved|rejected'){continue}
   $blocked=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($disposition.path)){$disposition.path}else{Join-Path $root $disposition.path})).TrimEnd('\','/')
   if($blocked-eq$target-or$blocked.StartsWith($target+'\',[StringComparison]::OrdinalIgnoreCase)){throw "Previously failed/rejected DU cleanup preserved without retry: $blocked"}
  }
 }}
}

function Remove-DuOwnedWork([string]$Repository,[string]$Directory,[string]$ReportPath='',[string]$ReportOwner='DUs-6.1'){
 throw 'Direct DU work removal is retired; use the user-run cleanup queue, never an agent destructive consumer'
}

function Enter-DuWorkPhase([string]$Repository,[string]$Directory,[string]$CleanupOwner,
 [string]$ParentContext=$env:PSX_WORK_CLEANUP_PARENT){
 $target=Assert-DuOwnedWorkPath $Repository $Directory
 if($ParentContext){
  [void](Assert-WorkCleanupParent -WorkDirectory $target -Owner $CleanupOwner -Context $ParentContext -AllowSameProcess)
  return $null
 }
 Enter-WorkCleanup -WorkDirectory $target -Owner $CleanupOwner
}

function Write-DuPhaseResult([string]$Repository,[string]$Phase,$Record,[string]$ReportPath='',[string]$ReportOwner='DUs-6.1'){
 $path=Resolve-DuPhaseReport $Repository $ReportPath $ReportOwner
 if($Phase-notmatch'^[A-Za-z0-9_.-]{1,128}$'){throw 'Explicit valid phase identifier required'}
 $key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($path.ToUpperInvariant())))
 $mutex=[Threading.Mutex]::new($false,('Local\PSXIOS.PhaseResult.'+$key));$locked=$false
 try{
  try{$locked=$mutex.WaitOne(20000)}catch [Threading.AbandonedMutexException]{$locked=$true}
  if(!$locked){throw 'DU result record is in use'}
  $report=if(Test-Path -LiteralPath $path){Get-Content -LiteralPath $path -Raw|ConvertFrom-Json -AsHashtable}else{[ordered]@{schema=1;owner=$ReportOwner}}
  if($report.owner-ne$ReportOwner){throw 'Phase report belongs to a different owner; preserved unchanged'}
  if(!$report.phases){$report.phases=[ordered]@{}}
  $report.phases[$Phase]=$Record
  [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))|Out-Null
  $stage=$path+'.stage'
  if(Test-Path -LiteralPath $stage){throw 'Unfinished DU result transaction retained; no overwrite'}
  [IO.File]::WriteAllText($stage,($report|ConvertTo-Json -Depth 14)+[Environment]::NewLine,[Text.UTF8Encoding]::new($false))
  [IO.File]::Move($stage,$path,$true)
 }finally{if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
}

function Complete-DuWorkPhase([string]$Repository,[string]$Phase,[string]$Directory,$Record,
 [switch]$KeepBuild,[string]$Consumer,[string]$ReviewEvent,[string]$ReportPath='',[string]$ReportOwner='DUs-6.1',
 $QueueLease=$null,[string]$CleanupOwner='',[string]$ParentContext=$env:PSX_WORK_CLEANUP_PARENT){
 try{
 $target=Assert-DuOwnedWorkPath $Repository $Directory
 $report=Resolve-DuPhaseReport $Repository $ReportPath $ReportOwner
 if($report.StartsWith($target+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Phase result must outlive the queued work subtree'}
 Assert-DuWorkRetention -KeepBuild:$KeepBuild -Consumer $Consumer -ReviewEvent $ReviewEvent
 if(!$CleanupOwner){$CleanupOwner=$ReportOwner}
 $Record.utc=[DateTime]::UtcNow.ToString('o')
 $Record.work=$target
 $Record.fileCountPolicy='advisory; no prerequisite workspace scan'
 $Record.cleanup=[ordered]@{state='pending';path=$target;consumer=$(if($KeepBuild){$Consumer}else{$null});reviewEvent=$(if($KeepBuild){$ReviewEvent}else{$null})}
 try{
  if($QueueLease){
   # Explicit issuer-owned exact root only: a child may never change its parent's disposition.
   if([IO.Path]::GetFullPath($QueueLease.Path).TrimEnd('\','/')-ine$target-or$QueueLease.Record.Owner-cne$CleanupOwner){throw 'Queue lease does not own the exact phase root/owner'}
   [void](Get-WorkCleanupParentContext -Lease $QueueLease)
   $state=if($KeepBuild-or$Record.status-notin@('pass','qualified','collected','published')){'Held'}else{'Ready'}
   if($state-eq'Ready'){Assert-DuCleanupDisposition $Repository $target $report $ReportOwner}
   $Record.cleanup.manifest=$QueueLease.Manifest;$Record.cleanup.state='disposition_pending'
   Write-DuPhaseResult $Repository $Phase $Record $report $ReportOwner
   $findings=[IO.Path]::GetRelativePath($script:WorkCleanupFindingsProjectRoot,$report)
   if($findings-notmatch'^docs[\\/]'){
    $findings=if($ReportOwner-eq'cdu-6.1'){'docs/validation/cdu-rebuild-20261008.json'}else{'docs/validation/du-clean-rebuild-20261008.json'}
   }
   $reason=if($state-eq'Ready'){'Completed phase findings condensed; no remaining consumer; human deletion pending'}elseif($KeepBuild){$Consumer+'; review: '+$ReviewEvent}else{'Unfinished/failed phase; owner triage and minimum evidence disposition required'}
   Set-WorkCleanupDisposition -Lease $QueueLease -State $state -Reason $reason -Findings $findings
   $Record.cleanup.state='queued_'+$state.ToLowerInvariant()
  }else{
   # Real child ancestry or the released same-thread live-issuer proof is mandatory.
   $parent=Assert-WorkCleanupParent -WorkDirectory $target -Owner $CleanupOwner -Context $ParentContext -AllowSameProcess
   $Record.cleanup.state='deferred_to_parent';$Record.cleanup.parent=$parent.Path
   $Record.cleanup.manifest=$parent.Manifest;$Record.cleanup.owner=$parent.Owner
   $Record.cleanup.requestedDisposition=if($KeepBuild-or$Record.status-notin@('pass','qualified','collected','published')){'Held'}else{'Completed child; enclosing owner decides Ready/Held'}
  }
  Write-DuPhaseResult $Repository $Phase $Record $report $ReportOwner
 }catch{
  $Record.cleanup.state='failed_preserved';$Record.cleanup.reason=$_.ToString()
  try{Write-DuPhaseResult $Repository $Phase $Record $report $ReportOwner}catch{Write-Warning "DU result commit failed separately; exact work retained: $_"}
  Write-Warning "DU queue/deferred disposition failed; no deletion or parent-state fallback; primary outcome preserved: $target : $_"
 }
 }finally{if($QueueLease){try{Exit-WorkCleanup -Lease $QueueLease}catch{Write-Warning "DU issuer lease closure failed separately: $_"}}}
}

function Get-DuBuildInputs([string]$Product,[string]$Repository,[string]$Graphics){
 $name=if($Product-eq'Host'){'psx-du-host'}else{'psx-du-client'}
 $parts=@("modules/$name",'shared/norm','shared/graphics-sdk','shared/display-transport','shared/display-replay','shared/network-io','shared/windows-version')
 if($Product-eq'Host'){$parts+=@('shared/cdu','shared/diagnostics')}
 $paths=@(foreach($part in $parts){Get-ChildItem "$Repository/$part" -Recurse -File|Where-Object Name -ne 'AGENTS.md'|Select-Object -ExpandProperty FullName})
 $paths+=@('WindowsIcons.cmake','application-icon.rc.in','application-icon.hpp','DU.ico'|ForEach-Object{"$Repository/shared/windows-icons/$_"})
 $paths+=@(Get-ChildItem $Graphics -Recurse -File|Select-Object -ExpandProperty FullName)
 if($Product-eq'Client'){
  $paths+=@('inputs.ps1','package.ps1','publish.ps1','transfer.ps1','runtime-payload.ps1'|ForEach-Object{"$Repository/modules/psx-du-host/tools/$_"})
 }
 # Declare the full shared144-member SDK and its vector, not a copied DLL.
 if((Get-Content -LiteralPath "$Graphics/lib/cmake/AssGraphics/AssGraphicsConfigVersion.cmake" -Raw)-match'PACKAGE_VERSION "1\.4\.4"'){
  $sdkHelper="$Repository/modules/psx-du-host/tools/graphics144-inputs.ps1"
  . $sdkHelper
  $sdk=Get-DuGraphics144Sdk (Split-Path $Graphics -Parent) (Split-Path $Graphics -Leaf)
  $paths+=$sdkHelper;$paths+=$sdk.vectorPath;$paths+=@($sdk.files.path)
 }
 $paths+="$Repository/modules/psx-exporter/include/psx_exporter/recorder_protocol.hpp","$Repository/modules/psx-exporter/include/psx_exporter/display_protocol.hpp"
 $paths+='O:\psx\scripts\work-cleanup.ps1'
 if($Product-eq'Host'-and(Test-Path -LiteralPath "$Graphics/reconstruction.json")){$sdk=Get-Content "$Graphics/reconstruction.json" -Raw|ConvertFrom-Json;$paths+=$sdk.dll;$paths+=@($sdk.headers.path)}
 @($paths|Sort-Object -Unique|ForEach-Object{[ordered]@{path=$_;bytes=(Get-Item -LiteralPath $_).Length;sha256=(Get-FileHash -LiteralPath $_).Hash}})
}
function Confirm-DuBuildInputs($Before,$After){
 if($Before.Count-ne$After.Count){throw 'Qualification input count changed while building/testing'}
 for($n=0;$n-lt$Before.Count;++$n){if($Before[$n].path-ne$After[$n].path-or$Before[$n].sha256-ne$After[$n].sha256){throw "Qualification input changed while building/testing: $($Before[$n].path)"}}
}
function Read-DuTestAssessment([string]$Path,[int]$ExitCode){
 [xml]$document=Get-Content -LiteralPath $Path -Raw
 $cases=@($document.testsuite.testcase)
 if(!$cases.Count){throw 'No executable CTest results; qualification unavailable'}
 $deadlines=@();$failures=@();$passed=@()
 foreach($case in $cases){
  if($case.failure){if($case.failure.message-in@('Timeout','Test timeout')){$deadlines+=@($case.name)}else{$failures+=@($case.name)}}
  elseif($case.status-ne'run'-or$case.skipped){$failures+=@($case.name)}
  else{$passed+=@($case.name)}
 }
 if($ExitCode-and!$deadlines.Count-and!$failures.Count){throw 'CTest failed without a classified test result'}
 [ordered]@{timingPolicy='warning_only';ctestExitCode=$ExitCode;passed=$passed;deadlineWarnings=$deadlines;functionalFailures=$failures;allTestsCompleted=(!$deadlines.Count-and!$failures.Count);junitSha256=(Get-FileHash $Path).Hash}
}
function Invoke-DuQualification([string]$Build,[string]$Configuration){
 $junit=Join-Path $Build 'qualification-tests.xml'
 & ctest --test-dir $Build -C $Configuration --output-on-failure --output-junit $junit | Out-Host
 $result=Read-DuTestAssessment $junit $LASTEXITCODE
 $result|ConvertTo-Json -Depth 6|Set-Content "$Build/test-assessment.json" -Encoding utf8
 if($result.functionalFailures.Count){throw "Functional checks failed: $($result.functionalFailures-join', ')"}
 if($result.deadlineWarnings.Count){Write-Warning "Harness deadline exceeded: $($result.deadlineWarnings-join', '). Results remain incomplete; timing alone does not block candidate packaging."}
 $result
}
