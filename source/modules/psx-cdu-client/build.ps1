#requires -Version 7.0
param([ValidateSet('All','Debug','Release')][string]$Configuration='All',[switch]$Clean,[switch]$PublishOnly,[switch]$BuildOnly,[switch]$StageOnly,[switch]$FreshDefaults,[switch]$KeepBuild,[string]$KeepBuildConsumer='',[string]$KeepBuildReview='',[string]$GraphicsPackageRoot='',[string]$OutputRoot='',[string]$BuildRoot='',[string]$ReportDocument='',[switch]$NativeCopy)
$ErrorActionPreference='Stop'
$sourceRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$repo=if((Split-Path $sourceRoot -Leaf)-eq'source'){[IO.Path]::GetFullPath((Join-Path $sourceRoot '..'))}else{$sourceRoot}
if(!$GraphicsPackageRoot){throw 'Supply the qualified two-configuration ASS1.4.2 SDK root'}
if(!$OutputRoot){$OutputRoot=Join-Path $repo 'bin'}
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
$workRoot='O:\psx\bin\.work'
if(!$ReportDocument){$ReportDocument=Join-Path $repo 'docs/validation/cdu-options-20261006.md'}
$ReportDocument=[IO.Path]::GetFullPath($ReportDocument)
$allowedReports=@((Join-Path $repo 'docs/validation/cdu-options-20261006.md'),'P:\development\psx_ios\docs\validation\cdu-options-20261006.md')
if(!@($allowedReports | Where-Object {$ReportDocument.Equals([IO.Path]::GetFullPath($_),[StringComparison]::OrdinalIgnoreCase)}).Count -or !(Test-Path -LiteralPath $ReportDocument -PathType Leaf)){throw 'ReportDocument must be the existing CDU central topic in the calling workspace or maintained P project'}
if($BuildOnly -and ($PublishOnly -or $StageOnly)){throw 'BuildOnly cannot be combined with PublishOnly or StageOnly'}
if(($KeepBuild -or $BuildOnly -or $StageOnly) -and (!$KeepBuildConsumer.Trim() -or !$KeepBuildReview.Trim())){throw 'Keeping unpublished work requires KeepBuildConsumer and KeepBuildReview: name its current consumer/open question and next review event'}
if(!$BuildOnly -and !$StageOnly){throw 'Canonical publication uses scripts/publish-display-network.ps1; use BuildOnly or StageOnly here'}
if(!$BuildOnly -and !$OutputRoot.StartsWith($workRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Working package output must remain in the owned O work'}
$norm=Join-Path $sourceRoot 'shared/norm/vendor/norm'
. "$sourceRoot/modules/psx-du-host/tools/runtime-payload.ps1"
. "$PSScriptRoot/tools/graphics142-inputs.ps1"
. "$sourceRoot/modules/psx-du-host/tools/inputs.ps1"
. "$repo/scripts/work-cleanup.ps1" -CleanupProjectRoot "O:\psx"
function Enter-WorkCleanupMutex {
 param([string]$Name)
 $mutex=[Threading.Mutex]::new($false,('Local\PSXIOS.WorkCleanup.'+$Name))
 try{try{$locked=$mutex.WaitOne(30000)}catch [Threading.AbandonedMutexException]{$locked=$true};if(!$locked){throw "Actual cleanup mutex remains busy after30s: $Name"};return $mutex}catch{$mutex.Dispose();throw}
}
$copyApiPath=Join-Path $repo 'scripts/deployment/common.ps1'
if((Get-Item -LiteralPath $copyApiPath).Length-ne14898-or(Get-FileHash -LiteralPath $copyApiPath).Hash-cne'D7E496D962BA6051D4F5EC80941BB0A5E60057B542F23447FC220D756AB9D9A6'){throw 'Qualified CDU copy API edition changed; record a maintenance successor before use'}
. $copyApiPath

$clientVersion=Get-DisplayProductVersion CduClient $sourceRoot
if($NativeCopy){Write-Verbose 'NativeCopy is a compatibility parameter; cloud/local copy routing is automatic'}
if((Get-FileHash "$norm/runtime/Norm.dll").Hash -ne 'B96CF2B3B8313E09ADEE5EDA4B59CEE9063D73B3B454980625591CF6302B27DA' -or (Get-FileHash "$norm/include/normApi.h").Hash -ne 'D1AC15A607ED7FD63B4109C88F2659039C6C98E2E846037C316B15B48E6E8BA0'){throw 'Pinned NORM dependency changed'}
$script:payload=[Collections.Generic.List[object]]::new()
$script:transferUnresolved=$false
function Write-CduPhaseConclusion([string]$Variant,[string]$Phase,[string]$Outcome,[string]$Cleanup,[string]$Retained){
 # Retain one current conclusion per configuration, never append a build diary or
 # raw inherited report. An exclusive writer preserves all other topic content.
 if($Variant-notin@('Debug','Release')){throw 'Unknown CDU finding configuration'}
 $begin="<!-- current-cdu-build:$Variant -->";$end="<!-- /current-cdu-build:$Variant -->"
 $entry="$begin`n- $([DateTime]::UtcNow.ToString('o')) $Variant, ${Phase}: $Outcome. Cleanup: $Cleanup."
 if($Retained){$entry+=" Retained consumer/review: $Retained."}
 $entry+="`n$end"
 $stream=[IO.File]::Open($ReportDocument,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
 $original=$null
 try{
  $original=[byte[]]::new([int]$stream.Length);$offset=0
  while($offset-lt$original.Length){$n=$stream.Read($original,$offset,$original.Length-$offset);if(!$n){throw 'Incomplete CDU topic read'};$offset+=$n}
  $text=[Text.Encoding]::UTF8.GetString($original);$start=$text.IndexOf($begin,[StringComparison]::Ordinal);$finish=$text.IndexOf($end,[StringComparison]::Ordinal)
  if($start-lt0-and$finish-lt0){$text=$text.TrimEnd()+"`n`n"+$entry+"`n"}
  elseif($start-ge0-and$finish-ge$start-and$text.IndexOf($begin,$start+$begin.Length,[StringComparison]::Ordinal)-lt0-and$text.IndexOf($end,$finish+$end.Length,[StringComparison]::Ordinal)-lt0){$text=$text.Substring(0,$start)+$entry+$text.Substring($finish+$end.Length)}
  else{throw 'Ambiguous CDU conclusion markers; original topic preserved'}
  $bytes=[Text.UTF8Encoding]::new($false).GetBytes($text)
  $stream.Position=0
  try{$stream.Write($bytes,0,$bytes.Length);$stream.SetLength($bytes.Length);$stream.Flush($true)}
  catch{try{$stream.Position=0;$stream.Write($original,0,$original.Length);$stream.SetLength($original.Length);$stream.Flush($true)}catch{Write-Warning 'CDU finding rollback failed; preserve owned work and report both failures'};throw}
 }finally{$stream.Dispose()}
}
function CopyPayload([string]$src,[string]$dst){
 $script:payload.Add([pscustomobject]@{source=[IO.Path]::GetFullPath($src);target=[IO.Path]::GetFullPath($dst);bytes=(Get-Item -LiteralPath $src).Length;sha256=(Get-FileHash -LiteralPath $src).Hash})
}
function FlushPayload([string]$build,[string]$TeraCopyPath='C:\Program Files\TeraCopy\TeraCopy.exe'){
 # This facade stages only under the caller's live O lease. P publication needs
 # its own separately qualified commit owner; no cross-volume rename happens here.
 if($script:WorkCleanupProjectRoot -ine 'O:\psx'){throw 'CDU payload staging requires the actual O cleanup context'}
 $build=Resolve-WorkCleanupPath -WorkDirectory $build
 $null=Assert-WorkCleanupParent -WorkDirectory (Join-Path $build 'payload-transfers') -Owner 'cdu-6.1' -AllowSameProcess
 $rows=@($script:payload.ToArray())
 if(!$rows.Count){return}
 $targets=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
 foreach($row in $rows){
  $target=[IO.Path]::GetFullPath($row.target)
  Assert-WorkCleanupOrdinaryPath $target
  if(!$target.StartsWith($build+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Payload target escaped the owned staging graph'}
  if($target.Equals([IO.Path]::GetFullPath($row.source),[StringComparison]::OrdinalIgnoreCase)){throw 'Payload source and destination alias'}
  if(!$targets.Add($target)){throw 'Duplicate payload target'}
  SyncIdentity $row.source $row.bytes $row.sha256
  if(Test-Path -LiteralPath $target){SyncIdentity $target $row.bytes $row.sha256}
 }
 $transferRoot=Join-Path $build 'payload-transfers'
 if(Test-Path -LiteralPath $transferRoot){throw 'Prior payload evidence retained; never resubmit that transfer'}
 [IO.Directory]::CreateDirectory($transferRoot)|Out-Null
 $script:transferUnresolved=$true
 $reports=[Collections.Generic.List[object]]::new()
 $primary=$null;$reportFailure=$null
 try{
  $rows|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $transferRoot 'inventory.json') -Encoding utf8
  $index=0
  foreach($row in $rows){
   $record=[pscustomobject]@{
    Source=$row.source;Target=$row.target;Bytes=$row.bytes;SHA256=$row.sha256
    StagingDirectory=(Join-Path $transferRoot ('staging-{0:D4}' -f ++$index))
   }
   # The released shared API chooses cloud/local transport and commits the exact
   # filename. Default collision policy never authorizes replacing different bytes.
   $copied=SyncCopyFile $record $transferRoot $TeraCopyPath
   if(!$copied.Verified-or!([IO.Path]::GetFullPath($copied.Target)).Equals($row.target,[StringComparison]::OrdinalIgnoreCase)){
    throw 'Shared copier did not verify the authorized payload target'
   }
   SyncIdentity $row.target $row.bytes $row.sha256
   SyncIdentity $row.source $row.bytes $row.sha256
   $reports.Add([pscustomobject]@{source=$row.source;target=$row.target;bytes=$row.bytes;sha256=$row.sha256;result=$copied})
  }
  # Recheck all observations after the final row, including reused destinations.
  foreach($row in $rows){SyncIdentity $row.source $row.bytes $row.sha256;SyncIdentity $row.target $row.bytes $row.sha256}
 }catch{$primary=$_}
 finally{
  try{
   [ordered]@{schema=1;owner='cdu-6.1';method='Qualified shared automatic copy route';
    required=$rows.Count;verified=$reports.Count;complete=(!$primary);
    failure=$(if($primary){$primary.Exception.Message}else{''});records=$reports.ToArray()
   }|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $transferRoot 'verified.json') -Encoding utf8
  }catch{$reportFailure=$_}
 }
 if($primary){if($reportFailure){Write-Warning ('Copy report failed separately: '+$reportFailure.Exception.Message)};throw $primary}
 if($reportFailure){throw $reportFailure}
 $script:transferUnresolved=$false
 $script:payload.Clear()
}
function Inputs([string]$variant,[string]$build,[string]$graphics,[bool]$IncludeBinary=$true){
 $paths=@()
 foreach($part in @('modules/psx-cdu-client','shared/cdu','shared/graphics-sdk','shared/norm','shared/display-transport/include','shared/network-io','shared/diagnostics','shared/windows-version')){
  $paths+=@(Get-ChildItem (Join-Path $sourceRoot $part) -Recurse -File | Where-Object { $_.FullName -notmatch '[\\/](build|__pycache__)[\\/]' } | Select-Object -ExpandProperty FullName)
 }
 foreach($part in @('include','lib','bin','symbols','licenses')){$paths+=@(Get-ChildItem "$graphics/$part" -Recurse -File | Select-Object -ExpandProperty FullName)}
 # Capture the helper, template, runtime header and mapped artwork actually consumed by this target.
 foreach($iconInput in @('WindowsIcons.cmake','application-icon.rc.in','application-icon.hpp','DU.ico')){$paths+=Join-Path $sourceRoot "shared/windows-icons/$iconInput"}
 $paths+=Join-Path $repo 'scripts/package-layout.ps1'
 $paths+=Join-Path $repo 'scripts/deployment/common.ps1'
 $paths+=Join-Path $repo 'scripts/work-cleanup.ps1'
 # Pure payload/copy helpers are read-only dependencies, not DU build inputs.
 foreach($member in @('runtime-payload.ps1','inputs.ps1')){$paths+=Join-Path $sourceRoot "modules/psx-du-host/tools/$member"}
 if($IncludeBinary){foreach($name in @('psx-cdu-client.exe','psx-cdu-client.pdb','cdu_client_test.exe','cdu_client_test.pdb')){$paths+=Join-Path $build "$variant/$name"}}
 @($paths | Sort-Object -Unique | ForEach-Object {[ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash}})
}
foreach($variant in $(if($Configuration -eq 'All'){@('Debug','Release')}else{@($Configuration)})){
 $build=if($BuildRoot){Join-Path ([IO.Path]::GetFullPath($BuildRoot)) "build-$variant"}else{Join-Path $repo "bin/.work/cdu-rebuild-20261008/build-$variant"}
 $build=[IO.Path]::GetFullPath($build)
 if(!$build.StartsWith($workRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Compiler graphs and package stages must be below bin/.work'}
 $ownedGraph=$false;$primaryFailure=$null;$cleanupFailure='';$phase='dependency admission';$outcome='not completed';$testSummary='not run';$transactionRecovery=''
 $cleanupLease=$null;$published=$false
 try{
 $graphics=[IO.Path]::GetFullPath((Join-Path $GraphicsPackageRoot $variant))
 $sdk=Get-CduGraphics142Sdk -Sdk $graphics -Configuration $variant
 if(!$PublishOnly -and (Test-Path -LiteralPath $build)){throw "A fresh graph is required: $build; close the previous graph without bypassing any cleanup rejection"}
 if($PublishOnly -and !(Test-Path -LiteralPath $build -PathType Container)){throw 'PublishOnly requires the owned qualified graph'}
 $cleanupLease=Enter-WorkCleanup -WorkDirectory $build -Owner 'cdu-6.1'
 $ownedGraph=[bool]$PublishOnly
 if(!$PublishOnly){
  # File counts are advisory. No whole-cloud-workspace scan or numeric hold.
  $phase='configure'
  New-Item -ItemType Directory -Path $build -Force | Out-Null
  $ownedGraph=$true
  $before=@(Inputs $variant $build $graphics $false)
  [ordered]@{utc=[DateTime]::UtcNow.ToString('o');inputs=$before} | ConvertTo-Json -Depth 6 | Set-Content "$build/source-before.json" -Encoding utf8
  # Native fixtures that use the process temporary directory stay graph-local.
  $savedTemp=$env:TEMP;$savedTmp=$env:TMP
  $savedTmpDir=$env:TMPDIR;$savedNodeReuse=$env:MSBUILDDISABLENODEREUSE
  $testTemp=Join-Path $build 'temporary'
  New-Item -ItemType Directory -Force $testTemp | Out-Null
  $priorWorkers=@()
  try{
  $env:TEMP=$testTemp;$env:TMP=$testTemp;$env:TMPDIR=$testTemp
  $env:MSBUILDDISABLENODEREUSE='1'
  $priorWorkers=@(Get-CimInstance Win32_Process -Filter "Name='MSBuild.exe'" | ForEach-Object {"$($_.ProcessId)/$($_.CreationDate.ToUniversalTime().Ticks)"})
  [ordered]@{source=$PSScriptRoot;graph=$build;temporary=$testTemp;msbuildNodeReuse='disabled process-scoped';nativeBuildArgument='/nr:false';priorWorkerIdentities=$priorWorkers} | ConvertTo-Json -Depth 4 | Set-Content "$build/build-environment.json" -Encoding utf8
  & cmake -S $PSScriptRoot -B $build -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_PREFIX_PATH=$graphics" "-DAssGraphics_DIR=$graphics/lib/cmake/AssGraphics"
  if($LASTEXITCODE){throw 'Client configure failed'}
  $phase='build'
  $buildArgs=@('--build',$build,'--config',$variant,'--parallel','2');if($Clean){$buildArgs+='--clean-first'}
  $buildArgs+=@('--','/nr:false')
  & cmake @buildArgs
  if($LASTEXITCODE){throw 'Client build failed'}
  $phase='background qualification'
  # Fail quickly on the changed UI, while retaining the full independent suite
  # as the only source of a successful publication qualification.
  & ctest --test-dir $build -C $variant -R '^cdu_client_shader_options$' --output-on-failure --output-junit "$build/shader-options-preflight.xml"
  if($LASTEXITCODE){throw 'Focused shader editor qualification failed; no publication'}
  $plannedTests=(& ctest --test-dir $build -C $variant --show-only=json-v1 | ConvertFrom-Json).tests.name
  if($LASTEXITCODE -or !$plannedTests.Count){throw 'Cannot establish complete registered test set'}
  & ctest --test-dir $build -C $variant --output-on-failure --output-junit "$build/client-tests.xml"
  $testExit=$LASTEXITCODE
  $sharedReport=Join-Path $build 'qualification/shared-display-tools.json'
  if(Test-Path -LiteralPath $sharedReport){
   $shared=Get-Content -LiteralPath $sharedReport -Raw | ConvertFrom-Json -AsHashtable
   if($shared.owner-ne'cdu-6.1'){throw 'Inherited qualification wrote a foreign-owner report'}
   $compact=@($shared.phases.GetEnumerator() | ForEach-Object {"$($_.Key): status=$($_.Value.status), cleanup=$($_.Value.cleanup.state)"}) -join '; '
   Write-CduPhaseConclusion $variant 'inherited tool qualification' $compact 'recorded by each consumer fixture' ''
  }
  [xml]$testDocument=Get-Content -LiteralPath "$build/client-tests.xml" -Raw
  $testCases=@($testDocument.testsuite.testcase)
  if(!$testCases.Count){throw 'No authored test results available'}
  if(@(Compare-Object @($plannedTests|Sort-Object) @($testCases.name|Sort-Object)).Count){throw 'CTest results do not cover the complete registered group set'}
  $testFailures=@($testCases | Where-Object {$_.failure -or $_.skipped -or $_.status -ne 'run'})
  $testSummary="$($testCases.Count-$testFailures.Count)/$($testCases.Count) passed"
  if($testFailures.Count){$testSummary+="; incomplete/failed: $($testFailures.name -join ', ')"}
  if($testExit -or $testFailures.Count){throw "Client/CDU/host verification failed; $testSummary; no publication"}
  $after=@(Inputs $variant $build $graphics $false)
  if($before.Count -ne $after.Count){throw 'Source input set changed during qualification'}
  for($i=0;$i -lt $before.Count;$i++){if($before[$i].path -ne $after[$i].path -or $before[$i].sha256 -ne $after[$i].sha256){throw "Source changed during qualification: $($before[$i].path)"}}
  [ordered]@{configuration=$variant;clean=[bool]$Clean;utc=[DateTime]::UtcNow.ToString('o');inputs=@(Inputs $variant $build $graphics);testSummary=$testSummary;testGroups=@($plannedTests);testLogSha256=(Get-FileHash "$build/Testing/Temporary/LastTest.log").Hash} | ConvertTo-Json -Depth 6 | Set-Content "$build/client-qualification.json" -Encoding utf8
  }finally{
   $env:TEMP=$savedTemp;$env:TMP=$savedTmp;$env:TMPDIR=$savedTmpDir;$env:MSBUILDDISABLENODEREUSE=$savedNodeReuse
   # Only actual graph-referencing processes are relevant. Other tasks build
   # independently; a new global MSBuild PID is not ownership evidence.
   try{
    $remaining=@(Get-CimInstance Win32_Process -Filter "Name='MSBuild.exe'" | Where-Object {$_.CommandLine -and ($_.CommandLine.IndexOf($build,[StringComparison]::OrdinalIgnoreCase)-ge0 -or $_.CommandLine.IndexOf($build.Replace('\','/'),[StringComparison]::OrdinalIgnoreCase)-ge0)})
    [ordered]@{graphReferencedWorkers=@($remaining.ProcessId);confirmedNoGraphReferencedMSBuild=(!$remaining.Count);scope='Graph command-line observation; no claim about unrelated jobs or library producer closure';nodeReuseDisabled=$true} | ConvertTo-Json -Depth 3 | Set-Content "$build/worker-readback.json" -Encoding utf8
    if($remaining.Count){$transactionRecovery="Graph-referencing MSBuild remains; retain $build"}
   }catch{$transactionRecovery="Graph worker readback failed: $($_.Exception.Message)"}

  }
  if($transactionRecovery){throw 'Build worker ownership is unresolved; no packaging'}
 }
 $stamp=Get-Content "$build/client-qualification.json" -Raw | ConvertFrom-Json
 $testSummary=$stamp.testSummary
 $inputs=@(Inputs $variant $build $graphics)
 if(!$stamp.clean -or $stamp.configuration -ne $variant -or $stamp.inputs.Count -ne $inputs.Count -or $stamp.testLogSha256 -ne (Get-FileHash "$build/Testing/Temporary/LastTest.log").Hash){throw 'Successful matching clean qualification required before publication'}
 for($i=0;$i -lt $inputs.Count;$i++){if($stamp.inputs[$i].path -ne $inputs[$i].path -or $stamp.inputs[$i].sha256 -ne $inputs[$i].sha256){throw "Input changed after qualification: $($inputs[$i].path)"}}
 $ownedGraph=$true;$outcome="clean build, background checks ($testSummary) and producer input readback passed"
 if($BuildOnly){Write-Output "Qualified $variant CDU client; no package transfers or publication"}
 if(!$BuildOnly){
 $phase='package staging'
 $active=[IO.Path]::GetFullPath((Join-Path $OutputRoot "$variant/psx-cdu-client"))
 $package=[IO.Path]::GetFullPath((Join-Path $build 'stage/psx-cdu-client'))
 if(Test-Path -LiteralPath $package){throw "Package staging must be fresh: $package; retain a failed transfer for inspection instead of resubmitting it"}
 # Existing customer profiles are permitted. Unexpected old developer payloads
 # require explicit cleanup, rather than silently becoming the next package.
 if(Test-Path -LiteralPath $active){Confirm-DisplayPackageLayout CduClient @(Get-ChildItem -LiteralPath $active -File -Recurse|ForEach-Object{[IO.Path]::GetRelativePath($active,$_.FullName)})}
 $mutable=@{}
 foreach($name in @('settings.cfg','settings.cfg.bak')){$path=Join-Path $active $name;if(Test-Path -LiteralPath $path -PathType Leaf){$mutable[$path]=(Get-FileHash -LiteralPath $path).Hash}}
 if(Test-Path "$active/profiles"){foreach($f in Get-ChildItem "$active/profiles" -File){$mutable[$f.FullName]=(Get-FileHash -LiteralPath $f.FullName).Hash}}
 if($FreshDefaults -and $mutable.Count){throw 'Fresh-default publication cannot discard existing user settings/profiles; choose a fresh collection destination or preserve them explicitly'}
 New-Item -ItemType Directory -Force $package | Out-Null
 foreach($path in $mutable.Keys){CopyPayload $path (Join-Path $package ([IO.Path]::GetRelativePath($active,$path)))}
 $runtimePayload=@(Get-DisplayRuntimePayload CduClient $sourceRoot $build $variant "$graphics/bin/ass_graphics_lib.dll" "$graphics/symbols/ass_graphics_lib.pdb" "$graphics/licenses")
 Confirm-DisplayPackageLayout CduClient @($runtimePayload.relative)
 foreach($item in $runtimePayload){CopyPayload $item.source (Join-Path $package $item.relative)}
 $shaderSource=Join-Path $build 'package-inputs/shaders.zip'
 New-CduShaderSourceArchive $sourceRoot $shaderSource
 CopyPayload $shaderSource "$package/corresponding-source/shaders.zip"
 FlushPayload $build
 foreach($path in $mutable.Keys){if((Get-FileHash -LiteralPath $path).Hash -ne $mutable[$path]){throw "Active profile changed during publication: $path"}}
 $purposes=@{'corresponding-source/shaders.zip'='Legally required corresponding shader source, notices and conversion recipes'}
 foreach($item in $runtimePayload){$purposes[$item.relative.Replace('\','/')]=$item.purpose}
 $files=@(Get-ChildItem $package -Recurse -File | ForEach-Object {
  $relative=[IO.Path]::GetRelativePath($package,$_.FullName).Replace('\','/')
  if($relative -in @('manifest.json','settings.cfg','settings.cfg.bak') -or $relative.StartsWith('profiles/')){return}
  if(!$purposes.ContainsKey($relative)){throw "Unplanned staging payload: $relative"}
  [ordered]@{path=$relative;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash;purpose=$purposes[$relative]}
 })
 Confirm-DisplayPackageLayout CduClient @($files.path)
 [ordered]@{product='psx-cdu-client';version=$clientVersion;configuration=$variant;graphics='1.4.2';abi=1;cduWire=1;blackoutWire=1;shaderRevision=1;effectsDefault='off';normRuntime='1.5.7';normSourceTag='v1.5.9';utc=[DateTime]::UtcNow.ToString('o');layout='runtime-v1';qualification=[ordered]@{clean=$stamp.clean;testLogSha256=$stamp.testLogSha256};mutableExcluded=@('settings.cfg','settings.cfg.bak','profiles/*.cfg','profiles/*.cfg.bak');files=$files;sourceHashes=$inputs} | ConvertTo-Json -Depth 7 | Set-Content "$package/manifest.json" -Encoding utf8
 $manifestSha256=(Get-FileHash -LiteralPath "$package/manifest.json").Hash
 . "$repo/scripts/package-layout.ps1"
 AssertRuntimePackageLayout -Directory $package -Product 'psx-cdu-client'
 if($StageOnly){$outcome="staged and verified $($files.Count) immutable payloads; background tests $testSummary";Write-Output "Staged verified $variant CDU client at $package; canonical package unchanged"}
 if(!$StageOnly){
 $phase='publication'
 $publicationMutex=[Threading.Mutex]::new($false,'Local\ASS.PSX.CDU.Client.PackagePublication')
 $publicationAcquired=$false
 try{$publicationAcquired=$publicationMutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$publicationAcquired=$true;Write-Warning 'Prior publisher exited; revalidating the current transaction'}
 if(!$publicationAcquired){$publicationMutex.Dispose();throw 'Another CDU publication transaction is active'}
 $rollback=Join-Path $build 'rollback/psx-cdu-client'
 $movedOld=$false;$movedNew=$false
 try{
  foreach($path in $mutable.Keys){if((Get-FileHash -LiteralPath $path).Hash -ne $mutable[$path]){throw "Active profile changed before commit: $path"}}
  if(Test-Path -LiteralPath $rollback){throw 'Rollback destination already exists; retain the previous transaction'}
  New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($active)),([IO.Path]::GetDirectoryName($rollback)) | Out-Null
  # Verified staging is transferred by TeraCopy. Directory renames implement the
  # atomic publication/rollback boundary without changing configured user data.
  if(Test-Path -LiteralPath $active){[IO.Directory]::Move($active,$rollback);$movedOld=$true}
  [IO.Directory]::Move($package,$active);$movedNew=$true
  if((Get-FileHash -LiteralPath "$active/manifest.json").Hash -ne $manifestSha256){throw 'Published manifest changed during commit'}
  foreach($file in $files){$published=Join-Path $active $file.path;if((Get-Item -LiteralPath $published).Length -ne $file.bytes -or (Get-FileHash -LiteralPath $published).Hash -ne $file.sha256){throw "Published payload mismatch: $published"}}
  foreach($path in $mutable.Keys){$published=Join-Path $active ([IO.Path]::GetRelativePath($active,$path));if((Get-FileHash -LiteralPath $published).Hash -ne $mutable[$path]){throw "Published profile mismatch: $published"}}
  AssertRuntimePackageLayout -Directory $active -Product 'psx-cdu-client'
  $phase='final identity checks'
  $finalInputs=@(Inputs $variant $build $graphics)
  if($finalInputs.Count-ne$inputs.Count){throw 'Producer input set changed during publication'}
  for($i=0;$i-lt$inputs.Count;$i++){if($finalInputs[$i].path-ne$inputs[$i].path-or$finalInputs[$i].sha256-ne$inputs[$i].sha256){throw "Producer input changed during publication: $($inputs[$i].path)"}}
 }catch{
  $publicationFailure=$_
  try{
   if($movedNew){[IO.Directory]::Move($active,$package)}
   if($movedOld){[IO.Directory]::Move($rollback,$active)}
  }catch{
   $transactionRecovery="publication recovery at $rollback and $package; original/current package bytes required; review at next transaction recovery ($($_.Exception.Message))"
   Write-Warning $transactionRecovery
  }
  throw $publicationFailure
 }finally{$publicationMutex.ReleaseMutex();$publicationMutex.Dispose()}
 $outcome="published and verified $($files.Count) immutable runtime/legal payloads plus manifest; source/producer pins unchanged; background tests $testSummary"
 $published=$true
 Write-Output "Published $variant CDU client; other products preserved"
 }
 }
 }catch{$primaryFailure=$_;$outcome=$_.Exception.Message.Replace("`r",' ').Replace("`n",' ')}
 finally{
  $retention='';$cleanup='not applicable: no owned lease acquired'
  try{
   if($cleanupLease){
    $state='Held'
    if($transactionRecovery){$retention=$transactionRecovery}
    elseif($script:transferUnresolved){$retention="owned TeraCopy completion unresolved at $build; review after the exact request is closed/triaged"}
    elseif($primaryFailure){$retention="failed $phase; owner classifies minimum evidence and closes regenerable work at the source/test investigation review"}
    elseif($KeepBuild -or $BuildOnly -or $StageOnly){$retention="$KeepBuildConsumer; review $KeepBuildReview; graph $build"}
    elseif($published -and $ownedGraph){
     # Ready is a data-only request, never an agent deletion. Do not make an
     # ancestor cover an unresolved child/worker/transaction disposition.
     Assert-DuCleanupDisposition $sourceRoot $build "$build/qualification/shared-display-tools.json" 'cdu-6.1'
     $active=@(Get-CimInstance Win32_Process -ErrorAction Stop | Where-Object {$_.ProcessId -ne $PID -and $_.CommandLine -and ($_.CommandLine.IndexOf($build,[StringComparison]::OrdinalIgnoreCase)-ge0 -or $_.CommandLine.IndexOf($build.Replace('\','/'),[StringComparison]::OrdinalIgnoreCase)-ge0)})
     if($active.Count){$retention="process consumers remain (PIDs $($active.ProcessId -join ', ')); review after their normal exit"}
     else{$state='Ready'}
    }else{$retention='uncollected or incomplete producer; review at its qualification/collection disposition'}
    $reason=if($state-eq'Ready'){'Published payloads, symbols and producer inputs verified; no remaining consumer. Human-run cleanup pending.'}else{$retention}
    $cleanup="$state request for user-run cleanup; no deletion performed"
    Write-CduPhaseConclusion $variant $phase $outcome $cleanup $retention
    Set-WorkCleanupDisposition -Lease $cleanupLease -State $state -Reason $reason -Findings 'docs/validation/cdu-options-20261006.md'
    Write-Output "Cleanup $state; user-run queue: $($cleanupLease.Manifest)"
   }else{Write-CduPhaseConclusion $variant $phase $outcome $cleanup $retention}
  }catch{$cleanupFailure="Cleanup registration failed; work preserved: $($_.Exception.Message)";Write-Warning $cleanupFailure}
  finally{
   if($cleanupLease){try{Exit-WorkCleanup -Lease $cleanupLease}catch{if(!$cleanupFailure){$cleanupFailure="Cleanup lease release failed: $($_.Exception.Message)"};Write-Warning $cleanupFailure}}
  }
 }
 if($primaryFailure){throw $primaryFailure}
 if($cleanupFailure){throw "Operation completed, cleanup remains incomplete: $cleanupFailure"}
}
