#requires -Version 7.0
<#
Prepare a source-bound DU candidate without compiling or installing anything.
Deliver explicitly returns that candidate through independent P: commit work.
Native hashes are supplied from the owner's original qualification, never repinned.
#>
[CmdletBinding()]
param(
 [Parameter(Mandatory)][ValidateSet('Host','Client')][string]$Product,
 [Parameter(Mandatory)][ValidateSet('Debug','Release')][string]$Configuration,
 [ValidateSet('Prepare','Deliver')][string]$Phase='Prepare',
 [Parameter(Mandatory)][string]$BuildDirectory,
 [Parameter(Mandatory)][string]$Graphics,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$QualificationSHA256,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ExecutableSHA256,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$SymbolsSHA256,
 [Parameter(Mandatory)][string]$WorkDirectory,
 [Parameter(Mandatory)][string]$ReportPath,
 [string]$CleanupOwner='DUs-6.1',
 [string]$ParentCleanupOwner='',
 [string]$NativeSourceRoot='',
 [string]$CandidateRoot='',
 [string]$CommitWorkDirectory='',
 [switch]$FreshDefaults,
 [string]$Python='C:\Users\NWittje\AppData\Local\Python\pythoncore-3.14-64\python.exe'
)
$ErrorActionPreference='Stop'
if($Phase-eq'Deliver'-and$Configuration-ne'Release'){throw 'Project publication is Release-only; Debug candidates remain internal.'}
function Get-DisplayPackagingRoots {
 [pscustomobject]@{Maintained='P:\development\psx_ios';Local='O:\psx'}
}
function Import-DisplayPackagingFunctions([string]$Path) {
 $tokens=$null;$errors=$null
 $ast=[Management.Automation.Language.Parser]::ParseFile($Path,[ref]$tokens,[ref]$errors)
 if($errors.Count){throw "Packaging dependency does not parse: $Path"}
 foreach($node in $ast.FindAll({param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst]},$false)){
  . ([scriptblock]::Create(($node.Extent.Text-replace '^function\s+[^\s({]+',('function script:'+$node.Name))))
 }
}
function Initialize-DisplayPackaging([string]$Python) {
 $roots=Get-DisplayPackagingRoots
 $script:displayRoots=$roots
 $script:displayTools=@()
 foreach($path in @(
  "$($roots.Maintained)\source\modules\psx-du-host\tools\inputs.ps1",
  "$($roots.Maintained)\source\modules\psx-du-host\tools\runtime-payload.ps1",
  "$($roots.Maintained)\source\modules\psx-du-host\tools\graphics141-inputs.ps1",
  "$($roots.Maintained)\source\modules\psx-du-host\tools\graphics144-inputs.ps1",
  "$($roots.Maintained)\source\shared\graphics-sdk\current.ps1",
  "$($roots.Maintained)\source\shared\graphics-sdk\graphics144.ps1",
  "$($roots.Maintained)\scripts\deployment\common.ps1",
  "$($roots.Maintained)\scripts\publish-binaries.ps1",
  "$($roots.Maintained)\scripts\package-layout.ps1",
  "$($roots.Maintained)\scripts\verify-binaries.ps1",
  "$($roots.Maintained)\source\shared\cdu\tools\verify-metadata.py",
  "$($roots.Maintained)\scripts\work-cleanup.ps1",
  "$($roots.Local)\scripts\work-cleanup.ps1",
  'C:\Program Files\TeraCopy\TeraCopy.exe',$Python,(Join-Path $roots.Maintained 'scripts\package-display.ps1'))){
  if(-not $path){continue}
  $file=Get-Item -LiteralPath $path -ErrorAction Stop
  if($file.PSIsContainer){throw 'A packaging tool must be a file'}
  $script:displayTools+=@([pscustomobject]@{Source=$file.FullName;Bytes=$file.Length;SHA256=(Get-FileHash -LiteralPath $file.FullName).Hash})
 }
 # Bind this maintenance edition independently of the original native/tool proof.
 $verifier=@($script:displayTools|Where-Object Source -eq "$($roots.Maintained)\scripts\verify-binaries.ps1")[0]
 if($verifier.Bytes-ne20196-or$verifier.SHA256-ne'583338C03F99659097FED24F6101D819318DD4542AE696A2EF977AC1BE85B1AA'){
  throw 'Qualified current DU verifier edition required; qualify any successor before use'
 }
 foreach($path in @('source\shared\graphics-sdk\current.ps1','source\shared\graphics-sdk\graphics144.ps1','source\modules\psx-du-host\tools\inputs.ps1','source\modules\psx-du-host\tools\runtime-payload.ps1','source\modules\psx-du-host\tools\graphics141-inputs.ps1','source\modules\psx-du-host\tools\graphics144-inputs.ps1','scripts\deployment\common.ps1','scripts\publish-binaries.ps1','scripts\package-layout.ps1','scripts\verify-binaries.ps1')){
  Import-DisplayPackagingFunctions (Join-Path $roots.Maintained $path)
 }
 . (Join-Path $roots.Local 'scripts\work-cleanup.ps1') -CleanupProjectRoot $roots.Local
 Import-DisplayPackagingFunctions (Join-Path $roots.Local 'scripts\work-cleanup.ps1')
}
function Assert-DisplayPackagingWork([string]$Path,[ValidateSet('Local','Maintained')][string]$Root) {
 $roots=Get-DisplayPackagingRoots
 $path=[IO.Path]::GetFullPath($Path).TrimEnd('\','/')
 $base=Join-Path $roots.$Root 'bin\.work'
 if(-not $path.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)){throw "Packaging work must be a strict $Root bin/.work child"}
 Assert-WorkCleanupOrdinaryPath $path
 $path
}
function Assert-DisplayPackagingReport([string]$Path,[string]$Work,[string]$Owner,[string]$ParentContext='') {
 $report=Resolve-DuPhaseReport (Join-Path $script:displayRoots.Local 'source') $Path $Owner
 if(-not $report.StartsWith($Work+'\',[StringComparison]::OrdinalIgnoreCase)){
  if(-not$ParentContext){throw 'Generated report must belong to this actual O work lease'}
  $parent=Assert-WorkCleanupParent -WorkDirectory $Work -Owner $Owner -Context $ParentContext -AllowSameProcess
  if(-not $report.StartsWith($parent.Path+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Generated report escaped the actual enclosing O lease'}
 }
 if(Test-Path -LiteralPath $report){
  $existing=Get-Content -LiteralPath $report -Raw|ConvertFrom-Json
  if($existing.owner-cne$Owner){throw 'Phase report belongs to another owner; preserved before staging'}
 }
 $report
}
function Get-DisplayMaintainedInput([string]$Path,[string]$Snapshot,$HostSdk=$null) {
 $roots=Get-DisplayPackagingRoots
 if(-not[IO.Path]::IsPathFullyQualified($Path)-or$Path-match'(^|[\\/])\.\.?([\\/]|$)'){throw 'Native input path must be absolute without traversal'}
 $Path=[IO.Path]::GetFullPath($Path)
 if($HostSdk-and$HostSdk.profile-eq'GraphicsCurrent'-and$Path-ieq$HostSdk.vectorPath){return Get-DuGraphics144Origin $Path $HostSdk}
 if($HostSdk-and$Path.StartsWith($HostSdk.root+'\',[StringComparison]::OrdinalIgnoreCase)){
  if($HostSdk.profile-eq'GraphicsCurrent'){return Get-DuGraphics144Origin $Path $HostSdk}
  return Get-HostGraphics141Origin $Path $HostSdk
 }
 $prefix=[IO.Path]::GetFullPath($Snapshot).TrimEnd('\','/')+'\'
 if($Path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){
  return Join-Path $roots.Maintained $Path.Substring($prefix.Length)
 }
 # Native qualification binds the actual O: helper. Its maintained P: successor
 # can evolve independently (for example, second-PC storage support). Preserve
 # that original identity; both current tools are pinned separately above.
 if($Path-ieq(Join-Path $roots.Local 'scripts\work-cleanup.ps1')){return $Path}
 throw 'Original input escaped its qualified O source snapshot and exact cleanup helper'
}
function Get-DisplayGraphicsProfile([string]$Product,$Stamp) {
 if($Stamp.graphicsSdk){
  if($Product-notin@('Host','Client')-or$Stamp.hostGraphics-or$Stamp.graphicsSdk.profile-cne'GraphicsCurrent'-or$Stamp.graphicsSdk.version-cne'1.4.5'-or$Stamp.graphicsSdk.abi-ne1){throw 'Unsupported graphics144 source profile'}
  return 'Graphics144'
 }

 if($Product-eq'Client'){
  if($Stamp.hostGraphics){throw 'Host graphics profile cannot select a client SDK'}
  return 'Client130'
 }
 if($Product-ne'Host'){throw 'Unknown display product'}
 if(-not$Stamp.hostGraphics){return 'Host122'}
 if($Stamp.hostGraphics.profile-cne'Host141'-or$Stamp.hostGraphics.version-cne'1.4.1'-or$Stamp.hostGraphics.abi-ne1){throw 'Unsupported host graphics profile'}
 return 'Host141'
}
function Get-DisplayPackagingNative([string]$Product,[string]$Configuration,[string]$Build,[string]$Graphics,[string]$NativeSource,[string]$StampHash,[string]$ExeHash,[string]$PdbHash,$Maintenance=$null) {
 $roots=Get-DisplayPackagingRoots
 $native=[IO.Path]::GetFullPath($NativeSource).TrimEnd('\','/')
 if(-not[IO.Path]::IsPathFullyQualified($NativeSource)-or(Split-Path $native -Leaf)-ine'source'){throw 'Explicit qualified O snapshot source root required'}
 $snapshot=Assert-DisplayPackagingWork (Split-Path $native -Parent) Local
 Assert-WorkCleanupOrdinaryPath $native
 $build=Assert-DisplayPackagingWork $Build Local
 Assert-WorkCleanupOrdinaryPath $Graphics

 $name=if($Product-eq'Host'){'psx-du-host'}else{'psx-du-client'}
 $stampPath=Join-Path $build 'build-qualification.json'
 if((SyncHash $stampPath)-ine$StampHash){throw 'Original native qualification stamp changed'}
 $stamp=Get-Content -LiteralPath $stampPath -Raw|ConvertFrom-Json
 if($stamp.schema-ne1-or$stamp.product-ne$name-or$stamp.configuration-ne$Configuration-or-not$stamp.clean-or-not$stamp.tests){throw 'Matching original clean native qualification required'}
 $hostSdk=$null
 $profile=Get-DisplayGraphicsProfile $Product $stamp
 if($profile-eq'GraphicsCurrent'){
  $hostSdk=Get-DuGraphics144Sdk (Split-Path $Graphics -Parent) $Configuration
  $original=$stamp.graphicsSdk
  if([IO.Path]::GetFullPath($Graphics)-ine$hostSdk.configurationRoot-or$original.root-ine$hostSdk.root-or(Get-DisplayMaintainedInput $original.vectorPath $snapshot $hostSdk)-ine$hostSdk.vectorPath-or$original.manifestSHA256-cne$hostSdk.manifestSHA256-or@($original.files).Count-ne@($hostSdk.files).Count){throw 'Original graphics144 complete SDK binding differs'}
  for($i=0;$i-lt@($hostSdk.files).Count;++$i){$old=$original.files[$i];$actual=$hostSdk.files[$i];if($old.path-ine$actual.path-or$old.origin-ine$actual.origin-or$old.relative-cne$actual.relative-or$old.bytes-ne$actual.bytes-or$old.sha256-cne$actual.sha256){throw 'Original graphics144 member correspondence differs'}}
 }elseif(-not [IO.Path]::GetFullPath($Graphics).StartsWith($native+'\external\',[StringComparison]::OrdinalIgnoreCase)){throw 'Qualified legacy SDK must remain in its recorded O source snapshot'}

 if($profile-eq'Host141'){
  $hostSdk=Get-HostGraphics141Sdk $Graphics $Configuration
  if($stamp.hostGraphics.root-ine$hostSdk.root-or$stamp.hostGraphics.origin-ine$hostSdk.origin-or$stamp.hostGraphics.manifestSHA256-ine$hostSdk.manifestSHA256-or@($stamp.hostGraphics.files).Count-ne64){throw 'Original graphics141 origin/profile differs'}
  for($i=0;$i-lt64;++$i){$old=$stamp.hostGraphics.files[$i];$actual=$hostSdk.files[$i];if($old.path-ine$actual.path-or$old.origin-ine$actual.origin-or$old.relative-cne$actual.relative-or$old.bytes-ne$actual.bytes-or$old.sha256-ine$actual.sha256){throw 'Original graphics141 member correspondence differs'}}
 }
 $current=@(Get-DuBuildInputs $Product $native $Graphics)
 if(@($stamp.inputs).Count-ne$current.Count){throw 'Original/current native input count differs'}
 if($Maintenance-and($Maintenance.Binding.Product-cne$name-or$Maintenance.Binding.Configuration-cne$Configuration-or
     $Maintenance.Binding.NativeBinding.stampSHA256-ine$StampHash-or$Maintenance.Binding.NativeBinding.executableSHA256-ine$ExeHash-or$Maintenance.Binding.NativeBinding.symbolsSHA256-ine$PdbHash)){
  throw 'Maintenance context belongs to a different native producer'
 }
 for($i=0;$i-lt$current.Count;++$i){
  $input=$stamp.inputs[$i]
  if($input.path-ine$current[$i].path){throw 'Original native source set/order differs'}
  if($input.sha256-ine$current[$i].sha256){
   if(-not(Assert-DisplayMaintenanceInput $input.path $input.sha256 $Maintenance)){throw "Native source changed: $($input.path)"}
  }else{SyncIdentity $input.path $input.bytes $input.sha256}
  $maintained=Get-DisplayMaintainedInput $input.path $snapshot $hostSdk
  if((SyncHash $maintained)-ine$input.sha256){
   if(-not(Assert-DisplayMaintenanceInput $maintained $input.sha256 $Maintenance)){throw "Maintained native source changed: $maintained"}
  }else{SyncIdentity $maintained $input.bytes $input.sha256}
 }
 $log=Join-Path $build 'Testing\Temporary\LastTest.log'
 if((SyncHash $log)-ine$stamp.testLogSha256){throw 'Original native test log changed'}
 $assessment=Read-DuTestAssessment (Join-Path $build 'qualification-tests.xml') $stamp.tests.ctestExitCode
 if($assessment.junitSha256-ine$stamp.tests.junitSha256-or$assessment.functionalFailures.Count){throw 'Native tests changed or contain functional failures'}
 $exe=Join-Path $build "$Configuration\$name.exe";$pdb=Join-Path $build "$Configuration\$name.pdb"
 if((SyncHash $exe)-ine$ExeHash-or(SyncHash $pdb)-ine$PdbHash){throw 'Native producer bytes differ from original owner binding'}
 $version=Get-DisplayProductVersion $Product $native
 if($version-ne(Get-DisplayProductVersion $Product (Join-Path $roots.Maintained 'source'))){throw 'Current source product version differs from native qualification'}
 $ass=if($profile-eq'GraphicsCurrent'){'1.4.5'}elseif($hostSdk){'1.4.1'}elseif($Product-eq'Host'){'1.2.2'}else{'1.3.0'}
 $dll=Join-Path $Graphics 'bin\ass_graphics_lib.dll';$symbols=Join-Path $Graphics 'symbols\ass_graphics_lib.pdb';$licenses=Join-Path $Graphics 'licenses'
 if(Test-Path -LiteralPath (Join-Path $Graphics 'reconstruction.json')){
  if($Product-ne'Host'){throw 'ASS reconstruction is only qualified for Host'}
  $reconstruction=Get-Content -LiteralPath (Join-Path $Graphics 'reconstruction.json') -Raw|ConvertFrom-Json
  if($reconstruction.version-ne$ass-or$reconstruction.configuration-ne$Configuration){throw 'Private SDK reconstruction identity differs'}
  $dll=$reconstruction.dll
  $symbols=if($reconstruction.symbols){$reconstruction.symbols}else{Join-Path (Split-Path $dll -Parent) 'symbols\ass_graphics_lib.pdb'}
  if((SyncHash $dll)-ine$reconstruction.dllSha256){throw 'Private reconstructed DLL changed'}
  if(-not(Test-Path -LiteralPath $licenses)-and$reconstruction.licenses){$licenses=$reconstruction.licenses}
 }else{
  $sdkVersion=Get-Content -LiteralPath (Join-Path $Graphics 'lib\cmake\AssGraphics\AssGraphicsConfigVersion.cmake') -Raw
  if($sdkVersion-notmatch('PACKAGE_VERSION "'+[regex]::Escape($ass)+'"')){throw 'Private SDK version differs'}
 }
 Assert-DisplayFileDetails $exe $version 'ASS PSX Addons'
 Assert-DisplayFileDetails $dll $ass 'ASS Graphics Lib'
 $payload=@(Get-DisplayRuntimePayload $Product $native $build $Configuration $dll $symbols $licenses)
 Confirm-DisplayPackageLayout $Product @($payload.relative)
 $maintainedInputs=@($stamp.inputs|ForEach-Object{[pscustomobject]@{path=(Get-DisplayMaintainedInput $_.path $snapshot $hostSdk);bytes=$_.bytes;sha256=$_.sha256}})
 [pscustomobject]@{Product=$name;Version=$version;Configuration=$Configuration;Graphics=$ass;GraphicsProfile=$profile;HostSdk=$hostSdk;MaintainedInputs=$maintainedInputs;Payload=$payload;Stamp=$stamp;Assessment=$assessment;StampPath=$stampPath;StampHash=$StampHash;ExecutableHash=$ExeHash;SymbolsHash=$PdbHash}
}
function Get-DisplayPackagingBinaryEvidence($Native,[string]$Python) {
 $rows=@($Native.Payload|Where-Object relative -match '\.(exe|dll)$'|ForEach-Object{
  $symbol=@($Native.Payload|Where-Object relative -eq ('symbols/'+[IO.Path]::GetFileNameWithoutExtension($_.relative)+'.pdb'))
  [ordered]@{path=$_.source;relative=$_.relative;symbols=$(if($symbol.Count){$symbol[0].source}else{''})}
 })
 $code=@'
import sys,json,runpy,struct
from pathlib import Path
m=runpy.run_path(sys.argv[1]); out=[]
for row in json.loads(sys.argv[2]):
    p=Path(row['path']); b=p.read_bytes(); u16=lambda n:struct.unpack_from('<H',b,n)[0];u32=lambda n:struct.unpack_from('<I',b,n)[0]
    nt=u32(60);assert b[nt:nt+4]==b'PE\0\0' and u16(nt+4)==0x8664 and u16(nt+24)==0x20b
    opt=nt+24; sections=[]
    for i in range(u16(nt+6)):
        q=opt+u16(nt+20)+40*i;vs,va,size,start=struct.unpack_from('<4I',b,q+8);sections.append((vs,va,size,start))
    def offset(rva):
        for vs,va,size,start in sections:
            if va<=rva<va+max(vs,size):
                assert rva-va<size;return start+rva-va
        raise ValueError('Unmapped import RVA')
    rva=u32(opt+112+8);imports=[]
    if rva:
        q=offset(rva)
        while u32(q+12):
            s=offset(u32(q+12));imports.append(b[s:b.index(b'\0',s)].decode('ascii'));q+=20
    item=dict(relative=row['relative'],imports=imports,x64=True)
    if row['symbols']:
        identity=m['PE'](p).identity();assert identity==m['pdb_identity'](Path(row['symbols'])),'PDB does not match '+str(p)
        item.update(guid=identity[0],age=identity[1],matched=True)
    out.append(item)
print(json.dumps(out))
'@
 $start=[Diagnostics.ProcessStartInfo]::new($Python);$start.UseShellExecute=$false;$start.CreateNoWindow=$true
 $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
 foreach($arg in @('-B','-c',$code,(Join-Path $script:displayRoots.Maintained 'source\shared\cdu\tools\verify-metadata.py'),(ConvertTo-Json -InputObject @($rows) -Compress -Depth 5))){$start.ArgumentList.Add($arg)}
 $proc=[Diagnostics.Process]::Start($start)
 $stdout=$proc.StandardOutput.ReadToEndAsync();$stderr=$proc.StandardError.ReadToEndAsync()
 if(-not $proc.WaitForExit(30000)){throw "Static metadata reader did not close; retain work, PID$($proc.Id)"}
 $errorText=$stderr.GetAwaiter().GetResult();$output=$stdout.GetAwaiter().GetResult()
 if($proc.ExitCode-ne0){throw "Static PE/PDB qualification failed: $errorText"}
 $evidence=@(ConvertFrom-Json $output)
 Assert-DisplayPackagingBinaryClosure $evidence $Native
 $evidence
}
function Assert-DisplayPackagingBinaryClosure($Evidence,$Native) {
 if(@($Evidence|Where-Object matched).Count-ne2){throw 'Both application and private graphics PE/PDB pairs required'}
 $payloadNames=@($Native.Payload.relative|ForEach-Object{[IO.Path]::GetFileName($_)})
 foreach($binary in $Evidence){foreach($dll in $binary.imports){
  if($dll-in$payloadNames){continue}
  if($dll-match'^(api-ms-win-|ext-ms-win-)' -or (Test-Path -LiteralPath (Join-Path ([Environment]::GetFolderPath('System')) $dll))){continue}
  throw "Unresolved runtime import: $($binary.relative) -> $dll"
 }}
}
function Copy-DisplayPackagingPayload([object[]]$Payload,[string]$Stage,[string]$Evidence) {
 $transfers=@();$renames=@()
 foreach($item in $Payload){
  $target=SyncChild $Stage $item.relative
  if([IO.Path]::GetFileName($item.source)-cne[IO.Path]::GetFileName($target)){
   $incoming=Join-Path $Evidence ('incoming\'+[Guid]::NewGuid().ToString('N'))
   $copy=Join-Path $incoming ([IO.Path]::GetFileName($item.source));$renames+=@(@{Source=$copy;Target=$target})
  }else{$copy=$target}
  $transfers+=@([pscustomobject]@{Source=$item.source;Target=$copy;Bytes=(Get-Item -LiteralPath $item.source).Length;SHA256=(SyncHash $item.source)})
 }
 SyncTransfer $transfers (Join-Path $Evidence 'transfers') 'C:\Program Files\TeraCopy\TeraCopy.exe'|Out-Host
 foreach($rename in $renames){[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($rename.Target))|Out-Null;[IO.File]::Move($rename.Source,$rename.Target)}
 foreach($item in $Payload){SyncIdentity (SyncChild $Stage $item.relative) (Get-Item -LiteralPath $item.source).Length (SyncHash $item.source)}
}
function Assert-DisplayPackagingSourceVectors($m,$Native) {
 $expectedSources=@(if($Native.GraphicsProfile-in@('Host141','Graphics144')){$Native.MaintainedInputs}else{$Native.Stamp.inputs})
 if(@($m.sourceHashes).Count-ne@($expectedSources).Count){throw 'Candidate source vector count differs'}
 for($i=0;$i-lt@($Native.Stamp.inputs).Count;++$i){
  if($m.sourceHashes[$i].path-ine$expectedSources[$i].path-or$m.sourceHashes[$i].bytes-ne$expectedSources[$i].bytes-or$m.sourceHashes[$i].sha256-ine$expectedSources[$i].sha256){throw 'Candidate maintained/original source correspondence differs'}
 }
 if($Native.GraphicsProfile-in@('Host141','Graphics144')){
  if(@($m.originalNativeSourceHashes).Count-ne@($Native.Stamp.inputs).Count-or$m.graphicsOrigin.profile-cne$Native.GraphicsProfile-or$m.graphicsOrigin.manifestSHA256-ine$Native.HostSdk.manifestSHA256-or$m.graphicsOrigin.origin-ine$Native.HostSdk.origin){throw 'Candidate lost complete original native vector or qualified SDK origin/profile'}
  for($i=0;$i-lt@($Native.Stamp.inputs).Count;++$i){$old=$m.originalNativeSourceHashes[$i];$actual=$Native.Stamp.inputs[$i];if($old.path-ine$actual.path-or$old.bytes-ne$actual.bytes-or$old.sha256-ine$actual.sha256){throw 'Candidate original native vector differs'}}
 }
}
function Assert-DisplayPackagingIdentity($m,$Native) {
 if($m.product-ne$Native.Product-or$m.version-ne$Native.Version-or$m.configuration-ne$Native.Configuration-or-not$m.candidate-or$m.layout-ne'runtime-v1'-or$m.graphics-ne$Native.Graphics-or$m.abi-ne1){throw 'Candidate product/graphics/ABI qualification differs'}
 if($m.nativeBinding.stampSHA256-ine$Native.StampHash-or$m.nativeBinding.executableSHA256-ine$Native.ExecutableHash-or$m.nativeBinding.symbolsSHA256-ine$Native.SymbolsHash){throw 'Candidate lost original native binding'}
}
function Assert-DisplayPackagingCandidate([string]$Root,$Native,$Tools,[string]$Product,$Maintenance=$null) {
 $manifestPath=Join-Path $Root 'manifest.json';$m=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
 if($Maintenance-and(SyncHash $manifestPath)-ine$Maintenance.Binding.ManifestSHA256){throw 'Sealed candidate manifest bytes changed'}
 Assert-DisplayPackagingIdentity $m $Native
 Assert-DisplayPackagingSourceVectors $m $Native
 $actual=@(Get-PublicationInventory $Root)
 if($actual.Count-ne$Native.Payload.Count+1-or$m.files.Count-ne$Native.Payload.Count){throw 'Candidate payload closure differs'}
 Confirm-DisplayPackageLayout $Product @($actual.Path)
 $seen=@{}
 foreach($item in $m.files){
  if($seen.ContainsKey($item.path)){throw 'Duplicate candidate path'};$seen[$item.path]=$true
  $nativeItem=@($Native.Payload|Where-Object relative -ceq $item.path)
  if($nativeItem.Count-ne1-or$item.source-ine$nativeItem[0].source){throw 'Candidate has an unqualified source/target'}
  SyncIdentity (SyncChild $Root $item.path) $item.size $item.sha256
  SyncIdentity $item.source $item.size $item.sha256
 }
 # The candidate records the original preparation tool edition. Delivery has
 # its own tool vector; neither replaces the native stamp or original manifest.
 foreach($tool in $m.packagingTools){Assert-DisplayHistoricalTool $tool $Maintenance}
 foreach($tool in $Tools){SyncIdentity $tool.Source $tool.Bytes $tool.SHA256}
 $m
}
function Assert-DisplayPackagingMutable([string]$Target,[switch]$FreshDefaults) {
 if(-not(Test-Path -LiteralPath $Target)){return @()}
 $old=@(Get-PublicationInventory $Target)
 $manifestPath=Join-Path $Target 'manifest.json'
 if(-not(Test-Path -LiteralPath $manifestPath)){throw 'Existing package lacks a manifest; preserve it for owner review'}
 $manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
 $known=@($manifest.files.path)+@('manifest.json','settings.cfg','settings.cfg.bak')
 foreach($f in $old){if($f.Path.Replace('\','/')-notin$known){throw "Unlisted operator asset preserved; explicitly qualify before delivery: $($f.Path)"}}
 $directories=@{}
 foreach($file in $known){
  $relative=([string]$file).Replace('\','/')
  if([IO.Path]::IsPathRooted($relative)-or$relative-match'(^|/)\.{1,2}(/|$)|:'){throw 'Previous package contains an unsafe relative path'}
  for($parent=[IO.Path]::GetDirectoryName($relative);$parent;$parent=[IO.Path]::GetDirectoryName($parent)){$directories[$parent.Replace('\','/')]=$true}
 }
 foreach($directory in Get-ChildItem -LiteralPath $Target -Directory -Recurse -Force -ErrorAction Stop){
  $relative=[IO.Path]::GetRelativePath($Target,$directory.FullName).Replace('\','/')
  if(-not$directories.ContainsKey($relative)){throw "Unlisted operator directory preserved; explicitly qualify before delivery: $relative"}
 }
 $mutable=@(Get-DisplayMutableInventory @($old|ForEach-Object{[pscustomobject]@{path=$_.Path.Replace('\','/');bytes=$_.Bytes;sha256=$_.SHA256}}))
 if($FreshDefaults-and$mutable.Count){throw 'Fresh defaults cannot discard existing operator settings/backups; use a fresh target'}
 $mutable
}
function New-DisplayPublicationAuthority([string]$Commit,$Lease,[string]$Owner,[Threading.Mutex]$Mutex,[bool]$Locked,[string]$LeaseRoot=$Commit) {
 if(-not$Lease-or$Lease.Path-ine$LeaseRoot-or-not$Mutex-or-not$Locked){throw 'Display publication requires its actual issuer lease and publication mutex'}
 if($Commit-ine$LeaseRoot-and-not$Commit.StartsWith($LeaseRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Display commit escaped its actual issuer scope'}
 [pscustomobject]@{Commit=$Commit;Root=$LeaseRoot;Lease=$Lease;Owner=$Owner;Context=(Get-WorkCleanupParentContext -Lease $Lease);Mutex=$Mutex;OwnsMutex=$Locked;IssuerPid=$PID;IssuerStartedUtcTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks;IssuerThreadId=[Threading.Thread]::CurrentThread.ManagedThreadId}
}
function Assert-DisplayPublicationAuthority($Authority) {
 if(-not$Authority-or$Authority.IssuerPid-ne$PID-or$Authority.IssuerThreadId-ne[Threading.Thread]::CurrentThread.ManagedThreadId-or
    $Authority.IssuerStartedUtcTicks-ne(Get-Process -Id $PID -ErrorAction Stop).StartTime.ToUniversalTime().Ticks-or
    -not$Authority.OwnsMutex-or-not($Authority.Mutex-is[Threading.Mutex])-or$Authority.Mutex.SafeWaitHandle.IsClosed-or
    -not$Authority.Lease-or$Authority.Lease.Path-ine$Authority.Root){throw 'Display publication issuer or mutex authority changed'}
 # The live cleanup registry checks token, PID creation identity, mutex and
 # issuer thread; a serialized context by itself grants no mutation authority.
 $null=Assert-WorkCleanupParent -WorkDirectory (Join-Path $Authority.Commit 'authority-check') -Owner $Authority.Owner -Context $Authority.Context -AllowSameProcess
 $acquired=$false
 try{$acquired=$Authority.Mutex.WaitOne(0);if(-not$acquired){throw 'Display publication mutex is no longer owned by this issuer thread'}}
 finally{if($acquired){$Authority.Mutex.ReleaseMutex()}}
}
function Assert-DisplayPublicationAbsent([string]$Path) {
 Assert-WorkCleanupOrdinaryPath $Path
 if(Test-Path -LiteralPath $Path){throw "Unexpected display publication namespace: $Path"}
}
function Invoke-DisplayPublicationGuard([scriptblock]$Guard) {
 if(-not$Guard){throw 'Actual display publication authority callback is required'}
 $output=@(& $Guard)
 if($output.Count){throw 'Display publication authority must silently succeed or throw'}
}
function Invoke-DisplayPackagingCommit([string]$Source,[string]$Target,[string]$Commit,[string]$Evidence,[switch]$FreshDefaults,[scriptblock]$ValidateBefore,[scriptblock]$ValidateAfter,[scriptblock]$ValidateAuthority) {
 Invoke-DisplayPublicationGuard $ValidateAuthority
 $script:displayPublicationRecoveryFailure=$null
 if([IO.Path]::GetPathRoot($Commit)-ine[IO.Path]::GetPathRoot($Target)){throw 'Commit stage and target must be on the same volume'}
 Assert-WorkCleanupOrdinaryPath $Source;Assert-WorkCleanupOrdinaryPath $Target;Assert-WorkCleanupOrdinaryPath $Commit
 $oldState=Get-PublicationNamespaceState $Target
 $sourceState=Get-PublicationNamespaceState $Source
 $oldBefore=@($oldState.Files)
 $mutable=@(Assert-DisplayPackagingMutable $Target -FreshDefaults:$FreshDefaults)
 $stage=Join-Path $Commit 'stage';$backup=Join-Path $Commit 'previous';$rejected=Join-Path $Commit 'rejected'
 if(Test-Path -LiteralPath $stage){throw 'Fresh commit stage required'}
 $sources=@(Get-PublicationInventory $Source|ForEach-Object{[pscustomobject]@{Source=(Join-Path $Source $_.Path);Bytes=$_.Bytes;SHA256=$_.SHA256}})
 $payload=@($sources|ForEach-Object{[pscustomobject]@{source=$_.Source;relative=[IO.Path]::GetRelativePath($Source,$_.Source)}})
 Copy-DisplayPackagingPayload $payload $stage $Evidence
 foreach($file in $oldBefore){$sources+=@([pscustomobject]@{Source=(Join-Path $Target $file.Path);Bytes=$file.Bytes;SHA256=$file.SHA256})}
 if($mutable.Count){Copy-DisplayPackagingPayload @($mutable|ForEach-Object{[pscustomobject]@{source=(Join-Path $Target $_.path);relative=$_.path}}) $stage $Evidence}
 $expected=@(Get-PublicationInventory $stage)
 $expectedState=Get-PublicationNamespaceState $stage
 [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Target))|Out-Null
 if($ValidateBefore){& $ValidateBefore}
 # Capture caller variables and commands: the common callback has its own
 # ValidateAuthority parameter and must not recursively resolve that name.
 $callerGuard=$ValidateAuthority;$guardCommand=${function:Invoke-DisplayPublicationGuard};$namespaceCommand=${function:Assert-PublicationNamespaceState}
 $guardTarget=$Target;$guardSource=$Source;$guardOldState=$oldState;$guardSourceState=$sourceState
 $commitGuard={param($Phase,$MoveSource,$MoveDestination)
  & $guardCommand $callerGuard
  & $namespaceCommand $guardSource $guardSourceState
  if($Phase-in'Preflight','MovePrevious'){& $namespaceCommand $guardTarget $guardOldState}
 }.GetNewClosure()
 Commit-Publication $stage $Target $backup $rejected $expected $sources $commitGuard
 try{
  if($ValidateAfter){& $ValidateAfter}
  Start-Sleep -Milliseconds 1000
  Assert-PublicationInventory $Target $expected
  Assert-PublicationSources $sources -PreviousTarget $Target -PreviousNow $(if(Test-Path -LiteralPath $backup){$backup}else{''})
 }catch{
  $primary=$_
  try{
   # Refuse recovery when either tree became customer data, the namespace
   # changed, or the real issuer lost ownership. Never blindly undo that state.
   Invoke-DisplayPublicationGuard $ValidateAuthority
   if($ValidateBefore){& $ValidateBefore}
   Assert-PublicationNamespaceState $Target $expectedState
   Assert-PublicationNamespaceState $Source $sourceState
   if($oldState.Exists){Assert-PublicationNamespaceState $backup $oldState}else{Assert-DisplayPublicationAbsent $backup}
   Assert-DisplayPublicationAbsent $stage
   Assert-DisplayPublicationAbsent $rejected
   Assert-PublicationSources $sources -PreviousTarget $Target -PreviousNow $(if($oldState.Exists){$backup}else{''})
   Move-PublicationDirectory $Target $rejected
   if($oldState.Exists){
    Invoke-DisplayPublicationGuard $ValidateAuthority
    if($ValidateBefore){& $ValidateBefore}
    Assert-PublicationNamespaceState $backup $oldState
    Assert-PublicationNamespaceState $rejected $expectedState
    Assert-PublicationNamespaceState $Source $sourceState
    Assert-DisplayPublicationAbsent $Target
    Assert-DisplayPublicationAbsent $stage
    Assert-PublicationSources $sources -PreviousTarget $Target -PreviousNow $backup
    Move-PublicationDirectory $backup $Target
    Assert-PublicationNamespaceState $Target $oldState
   }
  }catch{$script:publicationRollbackUnresolved=$true;$script:displayPublicationRecoveryFailure=$_.ToString();try{$primary.Exception.Data['DisplayPublicationRecoveryFailure']=$script:displayPublicationRecoveryFailure}catch{};Write-Warning ('Post-commit validation rollback unresolved; preserve all originals and work: '+$script:displayPublicationRecoveryFailure) -WarningAction Continue}
  throw $primary
 }
 [pscustomobject]@{Target=$Target;Mutable=$mutable;Files=$expected.Count;Backup=$(if(Test-Path -LiteralPath $backup){$backup}else{$null});Atomic=$true;Sources=$sources}
}
function Assert-DisplayPackagingProductIdle([string]$Target,[string]$Product,[string]$Configuration) {
 $processes=@(Get-CimInstance Win32_Process -ErrorAction Stop)
 if(-not$processes.Count){throw 'Windows process inventory is empty; idle status is unavailable and publication is refused'}
 $name=if($Product-eq'Host'){'psx-du-host.exe'}else{'psx-du-client.exe'}
 if(@($processes|Where-Object{$_.Name-ieq$name-and-not$_.ExecutablePath}).Count){throw 'Active product path could not be established; publication refused'}
 $running=@($processes|Where-Object{$_.ExecutablePath-and[IO.Path]::GetFullPath($_.ExecutablePath).StartsWith($Target+'\',[StringComparison]::OrdinalIgnoreCase)})
 if($Product-eq'Host'){$running+=@($processes|Where-Object Name -eq 'psx-cdu-host.exe')}
 if($running.Count){throw 'Exit the active/legacy product normally before delivery; no process is terminated'}
}

Initialize-DisplayPackaging $Python
$work=Assert-DisplayPackagingWork $WorkDirectory Local
if(Test-Path -LiteralPath $work){throw 'Fresh packaging operation work required; never reuse Ready or retained work'}
if($CleanupOwner-notmatch'^[A-Za-z0-9_.-]{1,64}$'){throw 'Explicit valid cleanup owner required'}
$report=Assert-DisplayPackagingReport $ReportPath $work $CleanupOwner $env:PSX_WORK_CLEANUP_PARENT
$lease=$null;$borrow=$null;$primary=$null;$complete=$false;$prepared=$false
$commitLease=$null;$commitComplete=$false;$mutex=$null;$locked=$false
$context=$env:PSX_WORK_CLEANUP_PARENT
$record=[ordered]@{schema=1;product=$Product;configuration=$Configuration;phase=$Phase;status='incomplete';nativeCompilation=$false;installedFilesChanged=$false;loadedAcceptance=$false;tools=$script:displayTools}
try{
 if($context){if(-not$ParentCleanupOwner-or$ParentCleanupOwner-cne$CleanupOwner){throw 'Inherited work requires the actual explicit parent owner'};$borrow=Assert-WorkCleanupParent -WorkDirectory $work -Owner $ParentCleanupOwner -Context $context -AllowSameProcess}
 elseif($ParentCleanupOwner){throw 'Parent owner supplied without a live parent context'}
 else{$lease=Enter-WorkCleanup -WorkDirectory $work -Owner $CleanupOwner}
 [IO.Directory]::CreateDirectory($work)|Out-Null
 $operation=Join-Path $work 'operation';[IO.Directory]::CreateDirectory($operation)|Out-Null
 $maintenance=$null
 if($Phase-eq'Deliver'){
  $null=Assert-DisplayPackagingWork $CandidateRoot Local
  $candidateName=if($Product-eq'Host'){'psx-du-host'}else{'psx-du-client'}
  $candidateManifest=Join-Path $CandidateRoot "$Configuration\$candidateName\manifest.json"
  $sourceEvidence=Read-PackageEvidenceJson (Join-Path $script:displayRoots.Maintained 'docs/validation/clean-rebuild-20261008.json')
  $candidateHash=SyncHash $candidateManifest
  $legacyBindings=@($sourceEvidence.SharedPackagingSuccessor20261009.DeliveryCompatibility.ApprovedBindings|Where-Object {$_.ManifestSHA256-ieq$candidateHash})
  if($sourceEvidence.SharedPackagingSuccessor20261009.DeliveryCompatibility.State-ceq'qualified'-and$legacyBindings.Count){
   $duEvidence=Read-PackageEvidenceJson (Join-Path $script:displayRoots.Maintained 'docs/validation/du-clean-rebuild-20261008.json')
   $candidateEvidence=Read-PackageEvidenceJson $candidateManifest
   $maintenance=Get-DisplayMaintenanceContext $sourceEvidence $duEvidence $candidateHash $candidateName $Configuration $candidateEvidence.nativeBinding
  }
 }
 $native=Get-DisplayPackagingNative $Product $Configuration $BuildDirectory $Graphics $NativeSourceRoot $QualificationSHA256 $ExecutableSHA256 $SymbolsSHA256 $maintenance
 $binary=Get-DisplayPackagingBinaryEvidence $native $Python
 $record.nativeBinding=[ordered]@{stamp=$native.StampPath;stampSHA256=$native.StampHash;executableSHA256=$native.ExecutableHash;symbolsSHA256=$native.SymbolsHash}
 $record.binaryEvidence=$binary;$record.tests=$native.Assessment
 if($Phase-eq'Prepare'){
  if($CandidateRoot-or$CommitWorkDirectory){throw 'Prepare selects its own fresh D candidate; commit arguments belong to Deliver'}
  $CandidateRoot=Join-Path $operation 'candidate';$candidate=Join-Path $CandidateRoot "$Configuration\$($native.Product)"
  Copy-DisplayPackagingPayload $native.Payload $candidate $operation
  $files=@($native.Payload|ForEach-Object{[ordered]@{path=$_.relative;size=(Get-Item -LiteralPath $_.source).Length;sha256=(SyncHash $_.source);source=$_.source;purpose=$_.purpose}})
  $qualification=[ordered]@{clean=$true;tests=$native.Assessment;testLogSha256=$native.Stamp.testLogSha256;timingPolicy='warning_only';loadedAcceptance=$false}
  $sources=if($native.GraphicsProfile-in@('Host141','Graphics144')){$native.MaintainedInputs}else{$native.Stamp.inputs}
  $packageManifest=[ordered]@{schema=1;product=$native.Product;version=$native.Version;configuration=$Configuration;graphics=$native.Graphics;abi=1;candidate=$true;layout='runtime-v1';qualification=$qualification;sourceHashes=$sources;nativeBinding=$record.nativeBinding;packagingTools=$script:displayTools;binaryEvidence=$binary;files=$files;mutableExcluded=@('settings.cfg','settings.cfg.bak')}
  if($native.GraphicsProfile-in@('Host141','Graphics144')){$packageManifest.originalNativeSourceHashes=$native.Stamp.inputs;$packageManifest.graphicsOrigin=[ordered]@{profile=$native.GraphicsProfile;origin=$native.HostSdk.origin;manifestSHA256=$native.HostSdk.manifestSHA256}}
  $packageManifest|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $candidate 'manifest.json') -Encoding utf8
  $null=Assert-DisplayPackagingCandidate $candidate $native $script:displayTools $Product
  & (Join-Path $script:displayRoots.Maintained 'scripts\verify-binaries.ps1') -Module ('DU'+$Product) -Configuration $Configuration -BinDirectory $CandidateRoot -VerifyInputs|Out-Host
  $record.candidate=$candidate;$record.candidateRoot=$CandidateRoot;$record.manifestSHA256=SyncHash (Join-Path $candidate 'manifest.json');$prepared=$true
 }else{
  if(-not$CandidateRoot-or-not$CommitWorkDirectory){throw 'Deliver requires explicit prepared candidate and separately reserved P commit work'}
  $null=Assert-DisplayPackagingWork $CandidateRoot Local
  $commit=Assert-DisplayPackagingWork $CommitWorkDirectory Maintained
  if(Test-Path -LiteralPath $commit){throw 'Fresh independent P commit work required'}
  $candidate=Join-Path $CandidateRoot "$Configuration\$($native.Product)"
  $null=Assert-DisplayPackagingCandidate $candidate $native $script:displayTools $Product $maintenance
  $target=Join-Path $script:displayRoots.Maintained "bin\$Configuration\$($native.Product)"
  Assert-DisplayPackagingProductIdle $target $Product $Configuration
  $mutex=[Threading.Mutex]::new($false,"Local\PSXIOS.Publish.DU$Product.$Configuration")
  try{$locked=$mutex.WaitOne(20000)}catch [Threading.AbandonedMutexException]{$locked=$true}
  if(-not$locked){throw 'Another DU publisher owns this product/configuration'}
  $localContext=$env:PSX_WORK_CLEANUP_PARENT
  $commitFailure=$null
  try{
   # P and O have independent issuer/catalog ownership. No inherited O proof is
   # offered as authority for P commit work; only the stable stage lives on P.
   $env:PSX_WORK_CLEANUP_PARENT=$null
   . (Join-Path $script:displayRoots.Maintained 'scripts\work-cleanup.ps1') -CleanupProjectRoot $script:displayRoots.Maintained
   $commitLease=Enter-WorkCleanup -WorkDirectory $commit -Owner $CleanupOwner
   [IO.Directory]::CreateDirectory($commit)|Out-Null
   $authority=New-DisplayPublicationAuthority $commit $commitLease $CleanupOwner $mutex $locked
   $authorityCommand=${function:Assert-DisplayPublicationAuthority};$idleCommand=${function:Assert-DisplayPackagingProductIdle}
   $authorityCheck={& $authorityCommand $authority;& $idleCommand $target $Product $Configuration}.GetNewClosure()
   $delivery=Invoke-DisplayPackagingCommit $candidate $target $commit $operation -FreshDefaults:$FreshDefaults -ValidateBefore {
    Assert-DisplayPackagingProductIdle $target $Product $Configuration
   } -ValidateAfter {
    $null=Get-DisplayPackagingNative $Product $Configuration $BuildDirectory $Graphics $NativeSourceRoot $QualificationSHA256 $ExecutableSHA256 $SymbolsSHA256 $maintenance
    $null=Assert-DisplayPackagingCandidate $candidate $native $script:displayTools $Product $maintenance
    foreach($tool in $script:displayTools){SyncIdentity $tool.Source $tool.Bytes $tool.SHA256}
   } -ValidateAuthority $authorityCheck
   $record.delivery=$delivery;$commitComplete=$true
  }catch{$commitFailure=$_;throw}finally{
   try{
    if($commitLease){
     $scope=[pscustomobject]@{Work=$commit;Lease=$commitLease}
     Close-PublicationResources $scope $commitComplete $false ([bool]$script:publicationRollbackUnresolved) $null $false $commitFailure
    }
   }finally{
    . (Join-Path $script:displayRoots.Local 'scripts\work-cleanup.ps1') -CleanupProjectRoot $script:displayRoots.Local
    $env:PSX_WORK_CLEANUP_PARENT=$localContext
   }
  }
 }
 $again=Get-DisplayPackagingNative $Product $Configuration $BuildDirectory $Graphics $NativeSourceRoot $QualificationSHA256 $ExecutableSHA256 $SymbolsSHA256 $maintenance
 foreach($tool in $script:displayTools){SyncIdentity $tool.Source $tool.Bytes $tool.SHA256}
 $record.status=if($prepared){'qualified'}else{'published'};$complete=$true
 [pscustomobject]@{Phase=$Phase;Product=$native.Product;Version=$native.Version;Configuration=$Configuration;Candidate=$record.candidate;Target=$(if($record.delivery){$record.delivery.Target});Report=$report;LoadedAcceptance=$false}
}catch{$primary=$_;$record.status='failed';$record.failure=$_.ToString();if($script:displayPublicationRecoveryFailure){$record.recoveryFailure=$script:displayPublicationRecoveryFailure}elseif($script:publicationRecoveryFailure){$record.recoveryFailure=$script:publicationRecoveryFailure.ToString()};throw}
finally{
 try{
  if(Test-Path -LiteralPath $work){
   $consumer=if($prepared){'Explicit DU candidate delivery'}else{'Failed packaging operation owner triage'}
   for($dispositionAttempt=0;$dispositionAttempt-lt10;++$dispositionAttempt){
    Complete-DuWorkPhase (Join-Path $script:displayRoots.Local 'source') ("Packaging.$Product.$Configuration.$Phase") $operation $record -KeepBuild:(!$complete-or$prepared) -Consumer $consumer -ReviewEvent 'Verified canonical collection or resolved authored failure' -ReportPath $report -ReportOwner $CleanupOwner -CleanupOwner $CleanupOwner -ParentContext $env:PSX_WORK_CLEANUP_PARENT
    if($record.cleanup.state-ne'failed_preserved'){break}
    if($record.cleanup.reason-notlike'*Cleanup/build lease is busy*'-or$dispositionAttempt-eq9){throw 'Packaging phase evidence/disposition failed; work retained'}
    Start-Sleep -Milliseconds 500
   }
  }
 }catch{Write-Warning "Packaging report failed separately; work preserved: $_";if(-not$primary){$primary=$_}}
 try{if($lease){Set-WorkCleanupDisposition -Lease $lease -State $(if($complete-and-not$prepared){'Ready'}else{'Held'}) -Reason $(if($complete-and-not$prepared){'Delivery identities verified; bounded phase evidence condensed; human removal pending'}elseif($prepared){'Qualified candidate consumer: explicit canonical DU delivery; review at delivered identity qualification'}else{'Failed operation; source/recovery retained for owner disposition'}) -Findings 'docs/operations/current-build-workflow.md'}}
 catch{Write-Warning "Packaging disposition failed separately: $_";if(-not$primary){$primary=$_}}
 finally{if($lease){Exit-WorkCleanup -Lease $lease};if($locked){$mutex.ReleaseMutex()};if($mutex){$mutex.Dispose()}}
 if($primary-and$complete){throw $primary}
}
