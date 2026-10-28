param([ValidateSet('All','Debug','Release')][string]$Configuration='All',[string]$BuildDirectory='',[switch]$SkipPublish,[string]$AssGraphicsSdkRoot=$env:ASS_GRAPHICS_SDK_ROOT,[string]$GraphicsSourceRoot='',[string]$NodeExecutable=(Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe'))
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'graphics-sdk.ps1')
$graphicsSdk=Resolve-IosGraphicsSdk $AssGraphicsSdkRoot
$sourceRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$repo=if((Split-Path -Leaf $sourceRoot) -eq 'source'){Split-Path -Parent $sourceRoot}else{$sourceRoot}
$developmentRoot='O:\psx'
$cleanupHelper=Join-Path $developmentRoot 'scripts/work-cleanup.ps1'
. $cleanupHelper -CleanupProjectRoot $developmentRoot
if(-not $BuildDirectory){$BuildDirectory=Join-Path $developmentRoot ('bin/.work/psx-ios-'+[Guid]::NewGuid().ToString('N'))}
$BuildDirectory=Resolve-WorkCleanupPath $BuildDirectory
if(Test-Path -LiteralPath $BuildDirectory){throw 'Select a fresh admitted IOS graph; existing and queued graphs are not reused.'}
$cmake='D:\Visual_Studio\VS\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest=Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$lease=Enter-WorkCleanup -WorkDirectory $BuildDirectory -Owner 'ios-6.1'
$savedEnvironment=@{}
foreach($name in 'TEMP','TMP','TMPDIR','MSBUILDDISABLENODEREUSE'){$savedEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
$outcome=[ordered]@{configuration=$Configuration;completed=$false;published=$false;phase='setup';temporary=(Join-Path $BuildDirectory 'temporary');buildArguments=@();failure=$null}
$primaryFailure=$null
try{
 New-Item -ItemType Directory -Path $outcome.temporary | Out-Null
 foreach($name in 'TEMP','TMP','TMPDIR'){[Environment]::SetEnvironmentVariable($name,$outcome.temporary,'Process')}
 [Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE','1','Process')
 $outcome.phase='configure'
 & $cmake -S $PSScriptRoot -B $BuildDirectory -G 'Visual Studio 18 2026' -A x64 "-DIOS_NODE_EXECUTABLE=$NodeExecutable" "-DASS_GRAPHICS_SDK_ROOT=$($graphicsSdk.Root)"
 if($LASTEXITCODE -ne 0){throw 'IOS configuration failed'}
 $configs=if($Configuration -eq 'All'){@('Debug','Release')}else{@($Configuration)}
 foreach($cfg in $configs){
  $outcome.phase="build $cfg"
  $arguments=@('--build',$BuildDirectory,'--config',$cfg,'--parallel','2','--','/m:1','/nr:false')
  $outcome.buildArguments+=,@($arguments)
  & $cmake @arguments 2>&1 | Tee-Object -FilePath (Join-Path $BuildDirectory "build-$cfg.log")
  if($LASTEXITCODE -ne 0){throw "IOS $cfg build failed"}
  $outcome.phase="test $cfg"
  & $ctest --test-dir $BuildDirectory -C $cfg --output-on-failure -V 2>&1 | Tee-Object -FilePath (Join-Path $BuildDirectory "test-$cfg.log")
  if($LASTEXITCODE -ne 0){throw "IOS $cfg tests failed"}
  if(-not $SkipPublish -and $cfg -eq 'Release'){
   $outcome.phase="publish $cfg"
   & (Join-Path $PSScriptRoot 'publish.ps1') -Configuration $cfg -BuildDirectory $BuildDirectory -AssGraphicsSdkRoot $graphicsSdk.Root -GraphicsSourceRoot $GraphicsSourceRoot
  }
 }
 $outcome.completed=$true;$outcome.published=(-not $SkipPublish -and $Configuration -in @('All','Release'));$outcome.phase='owner collection review'
}catch{
 $primaryFailure=$_;$outcome.failure=$_.ToString();throw
}finally{
 foreach($entry in $savedEnvironment.GetEnumerator()){
  [Environment]::SetEnvironmentVariable($entry.Key,$(if($null -eq $entry.Value){[NullString]::Value}else{$entry.Value}),'Process')
 }
 $housekeepingFailure=$null
 try{
  if(Test-Path -LiteralPath $BuildDirectory){[IO.File]::WriteAllText((Join-Path $BuildDirectory 'build-result.json'),($outcome|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))}
  $reason=if($outcome.completed){'IOS owner must verify collected payload/source/local PDB closure and merge results before Ready; unpublished binaries remain for the pending publisher.'}else{'IOS owner must triage this failed graph and consolidate minimum necessary evidence before Ready; no automatic retry.'}
  . $cleanupHelper -CleanupProjectRoot $developmentRoot
  Set-WorkCleanupDisposition -Lease $lease -State Held -Reason $reason -Findings 'docs/validation/ios-failure-layout-20261008.json'
 }catch{$housekeepingFailure=$_;Write-Warning "IOS work registration failed separately: $_"}
 finally{try{. $cleanupHelper -CleanupProjectRoot $developmentRoot;Exit-WorkCleanup -Lease $lease}catch{$housekeepingFailure=$_;Write-Warning "IOS work lease release failed separately: $_"}}
 if($housekeepingFailure -and -not $primaryFailure){throw $housekeepingFailure}
}
