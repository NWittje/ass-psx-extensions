#requires -Version 7.0
param([ValidateSet('All','Debug','Release')][string]$Configuration='All',[switch]$Clean,[switch]$Publish,
 [string]$BuildRoot='', [string]$PackageRoot='', [string]$GraphicsPackageRoot='',
 [switch]$KeepBuild,[string]$BuildConsumer='',[string]$ReviewEvent='', [string]$PhaseReportPath='')
$ErrorActionPreference='Stop'
$sourceRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$repo=if((Split-Path $sourceRoot -Leaf)-eq'source'){[IO.Path]::GetFullPath((Join-Path $sourceRoot '..'))}else{$sourceRoot}
if(!$BuildRoot){$BuildRoot='O:\psx\bin\.work\build\psx-du-host'}
if(!$GraphicsPackageRoot){$GraphicsPackageRoot=$env:ASS_GRAPHICS_SDK_ROOT}
if(!$GraphicsPackageRoot){throw 'Set ASS_GRAPHICS_SDK_ROOT or provide the qualified full ASS1.4.4 SDK root'}
if($Publish-and!$PackageRoot){throw 'Explicit fresh PackageRoot required; coordinated active publication is separate'}
. "$sourceRoot/modules/psx-du-host/tools/inputs.ps1"
. 'O:\psx\scripts\work-cleanup.ps1' -CleanupProjectRoot 'O:\psx'
if($PhaseReportPath){$PhaseReportPath=Resolve-DuPhaseReport $sourceRoot $PhaseReportPath}
Assert-DuWorkRetention -KeepBuild:$KeepBuild -Consumer $BuildConsumer -ReviewEvent $ReviewEvent
. "$PSScriptRoot/tools/graphics144-inputs.ps1"
foreach($variant in $(if($Configuration-eq'All'){@('Debug','Release')}else{@($Configuration)})){
 $build=[IO.Path]::GetFullPath((Join-Path $BuildRoot $variant));$graphics=[IO.Path]::GetFullPath((Join-Path $GraphicsPackageRoot $variant))
 if($PhaseReportPath-and$PhaseReportPath.StartsWith($build+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Phase report must outlive the native graph'}
 [void](Assert-DuBuildWorkspace $sourceRoot $build)
 $phase='SDK admission';$complete=$false;$before=@();$after=@();$tests=$null;$failure=$null
 $savedBuildEnvironment=@{}
 foreach($name in 'TEMP','TMP','TMPDIR','MSBUILDDISABLENODEREUSE'){
  $savedBuildEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
 }
 $nativeStarted=$false;$baselineNodes=@();$retainNative=$false
 $record=[ordered]@{product='psx-du-host';configuration=$variant;clean=[bool]$Clean;status='incomplete';tests=$null;inputs=@();candidate=$null;loadedAcceptance=$false}
 $record.nativeOwnership=[ordered]@{wrapperPid=$PID;source=$PSScriptRoot;build=$build;temporary=(Join-Path $build 'temporary');requestedNodeReuse=$false;baselineNodePids=@();closure='not_started';residual=@()}
 $record.cleanupTool=[ordered]@{path="O:\psx\scripts\work-cleanup.ps1";sha256=(Get-FileHash -LiteralPath "O:\psx\scripts\work-cleanup.ps1").Hash}
 $observeNativeClosure={
  $processes=@(Get-CimInstance Win32_Process -ErrorAction Stop)
  # A different task's new MSBuild node is not evidence of our ownership.
  $descendants=[Collections.Generic.HashSet[uint32]]::new();[void]$descendants.Add([uint32]$PID)
  do{$advanced=$false;foreach($process in $processes){if($descendants.Contains([uint32]$process.ParentProcessId)-and$descendants.Add([uint32]$process.ProcessId)){$advanced=$true}}}while($advanced)
  $unattributed=@($processes|Where-Object{$_.Name-eq'MSBuild.exe'-and$_.CommandLine-match'/nodemode:'-and$_.ProcessId-notin$baselineNodes-and-not$descendants.Contains([uint32]$_.ProcessId)}|Select-Object ProcessId,ParentProcessId,Name,CommandLine)
  $residual=@($processes|Where-Object{
   $_.ProcessId-ne$PID-and(
    ($_.ExecutablePath-and$_.ExecutablePath.StartsWith($build+'\',[StringComparison]::OrdinalIgnoreCase))-or
    ($_.CommandLine-and$_.CommandLine.IndexOf($build,[StringComparison]::OrdinalIgnoreCase)-ge0)-or
    ($_.Name-eq'MSBuild.exe'-and$_.CommandLine-match'/nodemode:'-and$descendants.Contains([uint32]$_.ProcessId)))
  }|Select-Object ProcessId,ParentProcessId,Name,CommandLine)
  [ordered]@{utc=[DateTime]::UtcNow.ToString('o');state=$(if($residual.Count){'residual_or_unattributed_new_node'}else{'no_residual_observed'});residual=$residual;unattributedNewNodes=$unattributed;method='Build-path/executable association and current wrapper descendants; unrelated nodes are recorded separately without assigning ownership'}
 }
 $cleanupLease=Enter-WorkCleanup -WorkDirectory $build -Owner 'DUs-6.1'
 try{
  $graphicsSdk=Get-DuGraphics144Sdk $GraphicsPackageRoot $variant
  $before=@(Get-DuBuildInputs 'Host' $sourceRoot $graphics)
  New-Item -ItemType Directory -Force (Join-Path $build 'temporary')|Out-Null
  foreach($name in 'TEMP','TMP','TMPDIR'){[Environment]::SetEnvironmentVariable($name,$record.nativeOwnership.temporary,'Process')}
  [Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE','1','Process')
  $record.nativeOwnership.processEnvironment=[ordered]@{}
  foreach($name in 'TEMP','TMP','TMPDIR','MSBUILDDISABLENODEREUSE'){$record.nativeOwnership.processEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
  $baselineNodes=@(Get-CimInstance Win32_Process -Filter "Name='MSBuild.exe'" -ErrorAction Stop|Select-Object -ExpandProperty ProcessId)
  $record.nativeOwnership.baselineNodePids=$baselineNodes
  $nativeLog=Join-Path $build 'native.log'
  $phase='configure'
  $nativeStarted=$true
  $phaseReportArguments=@();if($PhaseReportPath){$phaseReportArguments+="-DPSX_DISPLAY_TOOL_REPORT=$PhaseReportPath"}
  & cmake -S $PSScriptRoot -B $build -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_PREFIX_PATH=$graphics" "-DAssGraphics_DIR=$graphics/lib/cmake/AssGraphics" "-DPSX_DU_HOST_GRAPHICS144_SDK_ROOT=$GraphicsPackageRoot" @phaseReportArguments 2>&1|Tee-Object -FilePath $nativeLog -Append|Out-Host
  if($LASTEXITCODE){throw 'DU host configure failed'}
  $phase='build'
  $args=@('--build',$build,'--config',$variant,'--parallel','1');if($Clean){$args+='--clean-first'}
  $args+=@('--','/nr:false','/m:1')
  $record.nativeOwnership.buildArguments=$args
  & cmake @args 2>&1|Tee-Object -FilePath $nativeLog -Append|Out-Host
  if($LASTEXITCODE){throw 'DU host build failed'}
  $phase='qualification'
  $tests=Invoke-DuQualification $build $variant
  $after=@(Get-DuBuildInputs 'Host' $sourceRoot $graphics);Confirm-DuBuildInputs $before $after
  if((Get-FileHash -LiteralPath $record.cleanupTool.path).Hash-ne$record.cleanupTool.sha256){throw 'Cleanup tool changed during producer operation'}
  $phase='native worker closure';$record.nativeOwnership.closure=& $observeNativeClosure
  if($record.nativeOwnership.closure.residual.Count){throw 'Native worker closure unqualified; preserve owned graph for exact ownership review'}
  $graphicsSdk=Get-DuGraphics144Sdk $GraphicsPackageRoot $variant
  $stamp=[ordered]@{schema=1;product='psx-du-host';configuration=$variant;clean=[bool]$Clean;fileCountPolicy='advisory_not_scanned';inputs=$after;tests=$tests;graphicsSdk=$graphicsSdk;nativeOwnership=$record.nativeOwnership;testLogSha256=(Get-FileHash "$build/Testing/Temporary/LastTest.log").Hash}
  $stamp|ConvertTo-Json -Depth 7|Set-Content "$build/build-qualification.json" -Encoding utf8
  if($Publish){
   $phase='candidate preparation'
   $candidate=[IO.Path]::GetFullPath((Join-Path $PackageRoot "$variant/psx-du-host"))
   $packageContext=if($candidate.StartsWith($build+'\',[StringComparison]::OrdinalIgnoreCase)){$env:PSX_WORK_CLEANUP_PARENT}else{''}
   & "$sourceRoot/modules/psx-du-host/tools/package.ps1" -Product Host -Configuration $variant -BuildDirectory $build -PackageRoot $PackageRoot -Graphics $graphics -Clean:$Clean -CleanupParentContext $packageContext
   $record.candidate=[IO.Path]::GetFullPath((Join-Path $PackageRoot "$variant/psx-du-host"))
  }
  $record.status='qualified';$record.inputs=$after;$record.tests=$tests;$record.testLogSha256=$stamp.testLogSha256
  $complete=$true
 }catch{
  $failure=$_;$record.status='failed';$record.failedPhase=$phase;$record.failure=$_.ToString();$record.inputs=$before
  if(Test-Path -LiteralPath "$build/test-assessment.json"){$record.tests=Get-Content "$build/test-assessment.json" -Raw|ConvertFrom-Json}
  if(Test-Path -LiteralPath "$build/native.log"){$record.nativeFailureTail=@(Get-Content "$build/native.log" -Tail 64|ForEach-Object{if($_.Length-gt512){$_.Substring(0,512)}else{$_}})}
  throw
 }finally{
  foreach($entry in $savedBuildEnvironment.GetEnumerator()){
   [Environment]::SetEnvironmentVariable($entry.Key,$(if($null-eq$entry.Value){[NullString]::Value}else{$entry.Value}),'Process')
  }
  if($nativeStarted){
   try{$record.nativeOwnership.closure=& $observeNativeClosure;$retainNative=[bool]$record.nativeOwnership.closure.residual.Count}
   catch{$record.nativeOwnership.closure=[ordered]@{state='observation_failed';reason=$_.ToString()};$retainNative=$true}
   if($retainNative){Write-Warning 'Native worker closure remains unqualified; preserve exact graph, do not kill workers or infer ownership from node presence'}
  }
  # Publish here prepares a candidate only. Canonical publication is separate,
  # so even a successful producer stays Held until that consumer closes.
  $consumer=if($retainNative){'DU native worker ownership review'}elseif(!$complete){'DU failed-operation triage and minimum evidence disposition'}elseif($KeepBuild){$BuildConsumer}else{'Matching canonical DU package collection/publication'}
  $review=if($retainNative){'Exact worker ownership/completion resolved'}elseif(!$complete){'Recorded failure triaged and necessary original evidence preserved'}elseif($KeepBuild){$ReviewEvent}else{'Canonical package verified and producer findings/evidence disposition closed'}
  try{
   $record.utc=[DateTime]::UtcNow.ToString('o');$record.work=$build
   $record.cleanup=[ordered]@{state='disposition_pending';path=$build;manifest=$cleanupLease.Manifest;consumer=$consumer;reviewEvent=$review}
   Write-DuPhaseResult $sourceRoot ("Host."+$variant) $record $PhaseReportPath
   Set-WorkCleanupDisposition -Lease $cleanupLease -State Held -Reason ($consumer+'; review: '+$review) -Findings 'docs/validation/du-clean-rebuild-20261008.json'
   $record.cleanup.state='queued_held'
   Write-DuPhaseResult $sourceRoot ("Host."+$variant) $record $PhaseReportPath
   Write-Output "Producer work Held in user-run queue: $($cleanupLease.Manifest)"
  }catch{Write-Warning "DU queue disposition failed separately; work retained without a Ready/deletion claim, original operation outcome preserved: $_"}
  finally{try{Exit-WorkCleanup -Lease $cleanupLease}catch{Write-Warning "DU cleanup lease closure failed separately: $_"}}
 }
}
