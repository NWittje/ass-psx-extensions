#requires -Version 7.0
[CmdletBinding()]
param(
 [Parameter(Mandatory)][string]$WorkDirectory,
 [Parameter(Mandatory)][string]$GraphicsSdkRoot,
 [Parameter(Mandatory)][string]$Python,
 [string]$BinaryRoot='P:\development\psx_ios\bin\Release',
 [string]$SourceDirectory='P:\development\psx_ios\source',
 [string]$CleanupProjectRoot='O:\psx',
 [string]$ParentCleanupOwner='',
 [ValidateSet('All','Operational')][string]$Profile='All'
)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/work-cleanup.ps1" -CleanupProjectRoot $CleanupProjectRoot
. "$PSScriptRoot/deployment/common.ps1"
$WorkDirectory=Resolve-WorkCleanupPath $WorkDirectory
if(Test-Path -LiteralPath $WorkDirectory){throw 'Release preparation requires a fresh owned directory.'}
$lease=$null;$complete=$false
if($env:PSX_WORK_CLEANUP_PARENT){
 if(!$ParentCleanupOwner){throw 'Supply the real ParentCleanupOwner when borrowing build work.'}
 $catalog=Enter-WorkCleanupMutex 'catalog'
 try{$null=Assert-WorkCleanupParent $WorkDirectory $ParentCleanupOwner -AllowSameProcess}
 finally{Exit-WorkCleanupMutex $catalog}
}else{
 if($ParentCleanupOwner){throw 'Parent context is absent.'}
 $lease=Enter-WorkCleanup $WorkDirectory 'release-preparation'
}
try{
 [IO.Directory]::CreateDirectory($WorkDirectory)|Out-Null
 . (Join-Path $SourceDirectory 'shared/graphics-sdk/current.ps1')
 $null=Get-PsxCurrentGraphicsSdk $GraphicsSdkRoot Release
 $plan=Join-Path $WorkDirectory 'payload-plan.json'
 & $Python -B "$PSScriptRoot/release_payload.py" --binary-root $BinaryRoot --sdk $GraphicsSdkRoot --source $SourceDirectory --work "$WorkDirectory/derived-inputs" --output $plan --profile $Profile.ToLowerInvariant()
 if($LASTEXITCODE){throw 'Configuration-free Release payload qualification failed.'}
 $selection=Get-Content -LiteralPath $plan -Raw|ConvertFrom-Json
 if($selection.configuration-cne'Release'){throw 'Only Release downloads are supported.'}
 Add-Type -AssemblyName System.IO.Compression
 $output=Join-Path $WorkDirectory 'archives';[IO.Directory]::CreateDirectory($output)|Out-Null
 $archives=[Collections.Generic.List[object]]::new()
 foreach($group in $selection.groups){
  $main=@($group.files|Where-Object {[IO.Path]::GetFileName($_.relative)-ieq($group.product+'.exe')})
  if($group.product-eq'psx-exporter'){$main=@($group.files|Where-Object relative -EQ 'psx-exporter-start.exe')}
  if($group.product-eq'X-Plane'){$main=@($group.files|Where-Object relative -Like '*.xpl')}
  $version=if($group.version){$group.version}elseif($main.Count-eq1){[Diagnostics.FileVersionInfo]::GetVersionInfo($main[0].source).FileVersion}else{'current'}
  if($version-notmatch'^[0-9.]+$|^current$'){throw 'Ordinary package version required.'}
  $archive=Join-Path $output ($group.product+'-'+$version+'-'+$group.platform+'.zip')
  $prefix=if($group.product-eq'ass-graphics-lib'){''}else{$group.product+'/'}
  $manifest=[ordered]@{schema=1;product=$group.product;version=$version;platform=$group.platform;configuration='Release';graphics=$group.graphics;graphicsProducerManifestSHA256=$(if($group.graphics){$selection.producerManifestSHA256}else{$null});externalDependencies=@($group.externalDependencies);files=@($group.files|ForEach-Object{[ordered]@{path=$_.relative;bytes=$_.bytes;sha256=$_.sha256}})}
  $stream=[IO.File]::Open($archive,'CreateNew','ReadWrite','None')
  try{
   $zip=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create,$true)
   try{
    foreach($row in $group.files){
     SyncIdentity $row.source $row.bytes $row.sha256
     # Archive composition is a build-tool operation, not a filesystem copy.
     $entry=$zip.CreateEntry($prefix+$row.relative,[IO.Compression.CompressionLevel]::Optimal)
     $input=[IO.File]::OpenRead($row.source);$target=$entry.Open()
     try{$input.CopyTo($target,1048576)}finally{$target.Dispose();$input.Dispose()}
     SyncIdentity $row.source $row.bytes $row.sha256
    }
    $entry=$zip.CreateEntry($prefix+'manifest.json');$target=$entry.Open()
    try{$bytes=[Text.Encoding]::UTF8.GetBytes(($manifest|ConvertTo-Json -Depth 8));$target.Write($bytes,0,$bytes.Length)}finally{$target.Dispose()}
   }finally{$zip.Dispose()}
   $stream.Flush($true)
  }finally{$stream.Dispose()}
  # Re-read each archive's decompressed members, including zero-byte markers.
  $zip=[IO.Compression.ZipFile]::OpenRead($archive)
  try{
   if($zip.Entries.Count-ne@($group.files).Count+1){throw 'Archive roster differs.'}
   foreach($row in $group.files){
    $entry=$zip.GetEntry($prefix+$row.relative)
    if(!$entry-or$entry.Length-ne$row.bytes){throw 'Archive member size differs.'}
    $input=$entry.Open();$algorithm=[Security.Cryptography.SHA256]::Create()
    try{$hash=[Convert]::ToHexString($algorithm.ComputeHash($input))}finally{$algorithm.Dispose();$input.Dispose()}
    if($hash-cne$row.sha256){throw 'Archive member bytes differ.'}
   }
   if(@($zip.Entries|Where-Object {$_.FullName-match'(?i)(^|/)(Norm\.(dll|pdb)|[^/]+\.(ttf|otf|woff2?|cfg|ini|bak|psxfmc|log|db|sqlite3?))$'}).Count){throw 'Excluded dependency, operator file or build leftover entered archive.'}
  }finally{$zip.Dispose()}
  $archives.Add([pscustomobject]@{product=$group.product;version=$version;path=$archive;bytes=(Get-Item $archive).Length;sha256=(SyncHash $archive);members=@($group.files).Count+1})
 }
 [ordered]@{schema=1;configuration='Release';profile=$selection.profile;graphics=$selection.graphics;producerManifestSHA256=$selection.producerManifestSHA256;payloadPlanSHA256=(SyncHash $plan);archives=$archives.ToArray();limits=$selection.limits}|ConvertTo-Json -Depth 6|Set-Content "$WorkDirectory/release-preparation.json" -Encoding utf8
 $complete=$true
 Write-Output "Prepared and verified $($archives.Count) lean Release archives in $output. Canonical operator installations are unchanged."
}finally{
 if($lease){
  try{Set-WorkCleanupDisposition $lease Held $(if($complete){'Verified Release candidates await publication; consumer is the publishing task, review after verified delivery'}else{'Preparation failed; owner must triage before the next publication attempt'}) 'docs/operations/current-build-workflow.md'}
  finally{Exit-WorkCleanup $lease}
 }
}
