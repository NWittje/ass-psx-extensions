#requires -Version 7.0
# Private source-bound collection. Canonical delivery has its own P: transaction.
param(
 [Parameter(Mandatory)][ValidateSet('Debug','Release')][string]$Configuration,
 [Parameter(Mandatory)][string]$BuildDirectory,
 [Parameter(Mandatory)][string]$SourceInputRoot,
 [Parameter(Mandatory)][string]$SdkRoot,
 [Parameter(Mandatory)][string]$WorkDirectory,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$QualificationSHA256,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ExecutableSHA256,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$SymbolsSHA256,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$TestsSHA256,
 [string]$Python='C:\Users\NWittje\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
)
$ErrorActionPreference='Stop'
. 'P:\development\psx_ios\scripts\work-cleanup.ps1' -CleanupProjectRoot 'O:\psx'
function Enter-WorkCleanupMutex {
 param([string]$Name)
 $mutex=[Threading.Mutex]::new($false,('Local\PSXIOS.WorkCleanup.'+$Name))
 try{try{$locked=$mutex.WaitOne(30000)}catch [Threading.AbandonedMutexException]{$locked=$true};if(!$locked){throw "Actual cleanup mutex remains busy after30s: $Name"};return $mutex}catch{$mutex.Dispose();throw}
}
$work=Resolve-WorkCleanupPath $WorkDirectory
$build=[IO.Path]::GetFullPath($BuildDirectory)
$inputRoot=[IO.Path]::GetFullPath($SourceInputRoot)
$source=Join-Path $inputRoot 'source'
foreach($path in @($build)){
 if(!$path.StartsWith('O:\psx\bin\.work\',[StringComparison]::OrdinalIgnoreCase)){throw 'Collection inputs require explicit qualified O work'}
 Assert-WorkCleanupOrdinaryPath $path
}
function Exact([string]$Path,[string]$Hash){
 if(!(Test-Path -LiteralPath $Path -PathType Leaf)-or(Get-FileHash -LiteralPath $Path).Hash-ine$Hash){throw "Qualified input changed: $Path"}
}
function QualifiedInputs($Stamp){foreach($row in $Stamp.inputs){Exact $row.path $row.sha256}}
function CompleteCduCollection($Lease,[bool]$Success,[string]$Failure,$Copies,[string]$WorkDirectory){
 $errors=[Collections.Generic.List[Exception]]::new()
 if(!$Success){
  try{[ordered]@{qualified=$false;failure=$Failure;copies=$Copies}|ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $WorkDirectory 'candidate-failure.json') -Encoding utf8}
  catch{$errors.Add($_.Exception);Write-Warning "Secondary collection report failure: $_"}
 }
 try{
  Set-WorkCleanupDisposition -Lease $Lease -State Held -Reason 'Candidate/source/symbol review and separately admitted canonical-publication consumer; no existing settings copied or changed' -Findings 'docs/validation/cdu-options-20261006.md'
 }catch{$errors.Add($_.Exception);Write-Warning "Secondary collection disposition failure: $_"}
 finally{
  try{Exit-WorkCleanup $Lease}
  catch{$errors.Add($_.Exception);Write-Warning "Secondary collection lease-close failure: $_"}
 }
 # A primary failure remains the exception already propagating from collection.
 # A teardown-only failure must also fail the controller rather than mask itself.
 if($errors.Count-and!$Failure){throw [AggregateException]::new('CDU collection teardown failed',$errors.ToArray())}
}
$stampPath=Join-Path $build 'client-qualification.json'
Exact $stampPath $QualificationSHA256
$stamp=Get-Content -LiteralPath $stampPath -Raw|ConvertFrom-Json
if(!$stamp.clean-or$stamp.configuration-cne$Configuration-or$stamp.testSummary-cne("$(@($stamp.testGroups).Count)/$(@($stamp.testGroups).Count) passed")-or@($stamp.testGroups).Count-lt28){throw 'Complete matching clean CDU qualification required'}
Exact (Join-Path $build 'client-tests.xml') $TestsSHA256
Exact (Join-Path $build 'Testing/Temporary/LastTest.log') $stamp.testLogSha256
[xml]$tests=Get-Content -LiteralPath (Join-Path $build 'client-tests.xml') -Raw
$cases=@($tests.testsuite.testcase)
if($cases.Count-ne@($stamp.testGroups).Count-or@($cases|Where-Object{$_.failure-or$_.skipped-or$_.status-ne'run'}).Count){throw 'Every registered group must have actually passed'}
if(@(Compare-Object @($stamp.testGroups|Sort-Object) @($cases.name|Sort-Object)).Count){throw 'Registered/result test names differ'}
QualifiedInputs $stamp
Exact (Join-Path $build "$Configuration/psx-cdu-client.exe") $ExecutableSHA256
Exact (Join-Path $build "$Configuration/psx-cdu-client.pdb") $SymbolsSHA256
. (Join-Path $source 'modules/psx-cdu-client/tools/graphics144-inputs.ps1') -SdkRoot $SdkRoot
. (Join-Path $source 'modules/psx-du-host/tools/runtime-payload.ps1')
. (Join-Path $inputRoot 'scripts/package-layout.ps1')
$sdk=Get-CduGraphics144Sdk -Sdk (Join-Path $SdkRoot $Configuration) -Configuration $Configuration
$version=Get-DisplayProductVersion CduClient $source
Assert-DisplayFileDetails (Join-Path $build "$Configuration/psx-cdu-client.exe") $version 'ASS PSX Addons'
if(Test-Path -LiteralPath $work){throw 'Fresh collection scope required'}
$lease=Enter-WorkCleanup -WorkDirectory $work -Owner 'cdu-6.1'
$success=$false;$failure='';$startedCopies=[Collections.Generic.List[object]]::new()
try{
 [IO.Directory]::CreateDirectory($work)|Out-Null
 $package=Join-Path $work "candidate/$Configuration/psx-cdu-client"
 [IO.Directory]::CreateDirectory($package)|Out-Null
 $payload=@(Get-DisplayRuntimePayload CduClient $source $build $Configuration (Join-Path $sdk.root 'bin/ass_graphics_lib.dll') (Join-Path $sdk.root 'symbols/ass_graphics_lib.pdb') (Join-Path $sdk.root 'licenses'))
 Confirm-DisplayPackageLayout CduClient @($payload.relative)
 $rows=@(foreach($row in $payload){[pscustomobject]@{source=$row.source;relative=$row.relative;purpose=$row.purpose;bytes=(Get-Item -LiteralPath $row.source).Length;sha256=(Get-FileHash -LiteralPath $row.source).Hash}})
 # P: is pCloud: use verified native FileStream copies directly. TeraCopy
 # remains the route only for the local O:-to-O: binary/symbol rows below.
 foreach($row in @($rows|Where-Object{$_.source.StartsWith('P:\',[StringComparison]::OrdinalIgnoreCase)})){
  Exact $row.source $row.sha256
  $target=Join-Path $package $row.relative
  [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))|Out-Null
  $reader=[IO.File]::Open($row.source,'Open','Read','Read')
  try{$writer=[IO.File]::Open($target,'CreateNew','Write','None');try{$reader.CopyTo($writer,1048576);$writer.Flush($true)}finally{$writer.Dispose()}}finally{$reader.Dispose()}
  Exact $row.source $row.sha256;Exact $target $row.sha256
  if((Get-Item -LiteralPath $target).Length-ne$row.bytes){throw 'Native cloud transfer size differs'}
  $startedCopies.Add([ordered]@{source=$row.source;target=$target;route='Native verified FileStream for P/cloud';files=1;verified=1;unverified=0})
 }
 $groupIndex=0
 foreach($group in @($rows|Where-Object{!$_.source.StartsWith('P:\',[StringComparison]::OrdinalIgnoreCase)}|Group-Object {[IO.Path]::GetDirectoryName($_.relative)})){
  $destination=Join-Path $package $group.Name
  [IO.Directory]::CreateDirectory($destination)|Out-Null
  $names=@($group.Group|ForEach-Object{[IO.Path]::GetFileName($_.source)})
  if(@($names|Sort-Object -Unique).Count-ne$names.Count){throw 'Colliding source basenames in transfer group'}
  $list=Join-Path $work ("transfer-{0:D2}.txt"-f$groupIndex++)
  [IO.File]::WriteAllLines($list,[string[]]@($group.Group.source),[Text.UTF8Encoding]::new($true))
  $copy=Start-Process -FilePath 'C:\Program Files\TeraCopy\TeraCopy.exe' -ArgumentList @('Copy',('"*'+$list+'"'),('"'+$destination+'"'),'/SkipAll','/Close') -WindowStyle Hidden -PassThru
  $copyRecord=[ordered]@{pid=$copy.Id;startedUtc=$copy.StartTime.ToUniversalTime().ToString('o');exitCode=$null;exited=$false;files=$group.Count;verified=0;collisionCandidates=0;unverified=$group.Count}
  $startedCopies.Add($copyRecord)
  if(!$copy.WaitForExit(120000)){throw 'Bounded TeraCopy wait expired; retain collection and source'}
  $copy.Refresh()
  $copyRecord.exited=$true;$copyRecord.exitCode=$copy.ExitCode;$copy.Dispose()
  if($copyRecord.exitCode-ne0){throw 'TeraCopy launcher failed; retain collection and source'}
  foreach($row in $group.Group){
   $populated=Join-Path $destination ([IO.Path]::GetFileName($row.source))
   Exact $row.source $row.sha256;Exact $populated $row.sha256
   if((Get-Item -LiteralPath $populated).Length-ne$row.bytes){throw 'Transfer size differs'}
   $target=Join-Path $package $row.relative
   if($populated-ine$target){
    # TeraCopy transfers bytes; this exact, same-volume namespace commit maps
    # PACKAGE-README.md to the required single README without native copying.
    if(Test-Path -LiteralPath $target){throw 'Renamed payload collision'}
    [IO.File]::Move($populated,$target)
   }
  }
  $copyRecord.verified=$group.Count;$copyRecord.unverified=0
 }
 $archive=Join-Path $package 'corresponding-source/shaders.zip'
 New-CduShaderSourceArchive $source $archive
 $rows+= [pscustomobject]@{source=$archive;relative='corresponding-source/shaders.zip';purpose='Required corresponding shader/adaptation source and conversion recipes';bytes=(Get-Item -LiteralPath $archive).Length;sha256=(Get-FileHash -LiteralPath $archive).Hash}
 foreach($row in $rows){Exact (Join-Path $package $row.relative) $row.sha256;Exact $row.source $row.sha256}
 if(@(Get-ChildItem -LiteralPath $package -Recurse -File).Count-ne$rows.Count){throw 'Unplanned package members'}
 # Reuse only the audited pure PE/MSF helpers, never the historical audit's
 # top-level old-package workflow. No third-party Python package is needed.
 $audit=@'
import ast,json,struct,sys
from pathlib import Path
source,package=sys.argv[1:3]
tree=ast.parse(Path(source).read_text(encoding='utf-8-sig'))
nodes=[n for n in tree.body if isinstance(n,(ast.Import,ast.ImportFrom)) or isinstance(n,ast.FunctionDef) and n.name in ('sha','pe_identity','pdb_identity')]
ns={};exec(compile(ast.Module(body=nodes,type_ignores=[]),source,'exec'),ns)
root=Path(package);symbols=[];imports=[]
for binary in ('psx-cdu-client.exe','ass_graphics_lib.dll'):
 p=root/binary;q=root/'symbols'/(Path(binary).stem+'.pdb')
 pe=ns['pe_identity'](p);pdb=ns['pdb_identity'](q);assert pe==pdb,(binary,pe,pdb)
 symbols.append(dict(binary=binary,guid=pe[0],age=pe[1],matching=True))
for p in list(root.glob('*.exe'))+list(root.glob('*.dll')):
 d=p.read_bytes();pe=struct.unpack_from('<I',d,0x3c)[0];assert struct.unpack_from('<H',d,pe+4)[0]==0x8664
 n=struct.unpack_from('<H',d,pe+6)[0];opsize=struct.unpack_from('<H',d,pe+20)[0];op=pe+24;assert struct.unpack_from('<H',d,op)[0]==0x20b
 def offset(rva):
  for i in range(n):
   vs,va,rs,raw=struct.unpack_from('<4I',d,op+opsize+i*40+8)
   if va<=rva<va+max(vs,rs):return raw+rva-va
  raise ValueError('Unmapped RVA')
 rva,size=struct.unpack_from('<2I',d,op+112+8)
 if not rva:continue
 at=offset(rva)
 while any(struct.unpack_from('<5I',d,at)):
  name=offset(struct.unpack_from('<I',d,at+12)[0]);end=d.index(0,name);dll=d[name:end].decode('ascii')
  private=(root/dll).is_file();system=(Path('C:/Windows/System32')/dll).is_file() or dll.lower().startswith(('api-ms-win-','ext-ms-win-'))
  assert private or system,(str(p),dll)
  imports.append(dict(binary=p.name,dependency=dll,private=private,system=system));at+=20
print(json.dumps(dict(symbols=symbols,imports=imports,scope='Static private/transitive Windows/API-set closure; no driver or loaded physical acceptance')))
'@
 $auditJson=($audit|& $Python -B - (Join-Path $source 'modules/psx-cdu-client/tools/audit.py') $package)-join"`n"
 if($LASTEXITCODE){throw 'Native symbol/import closure audit failed'}
 $auditJson|ConvertFrom-Json|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $work 'native-audit.json') -Encoding utf8
 QualifiedInputs $stamp
 $files=@($rows|ForEach-Object{[ordered]@{path=$_.relative;bytes=$_.bytes;sha256=$_.sha256;purpose=$_.purpose}})
 [ordered]@{product='psx-cdu-client';version=$version;configuration=$Configuration;graphics='1.4.4';abi=1;cduWire=1;blackoutWire=1;shaderRevision=1;effectsDefault='off';layout='runtime-v1';mutableExcluded=@('settings.cfg','settings.cfg.bak','profiles/*.cfg','profiles/*.cfg.bak');qualification=[ordered]@{clean=$true;testSummary=$stamp.testSummary;testGroups=$stamp.testGroups;testLogSha256=$stamp.testLogSha256;producerSHA256=$QualificationSHA256;testsSHA256=$TestsSHA256};graphicsOrigin=[ordered]@{origin=$sdk.origin;manifestSHA256=$sdk.manifestSHA256;version=$sdk.version;abi=$sdk.abi};files=$files;sourceHashes=$stamp.inputs}|ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding utf8
 AssertRuntimePackageLayout -Directory $package -Product 'psx-cdu-client'
 foreach($row in $rows){Exact (Join-Path $package $row.relative) $row.sha256}
 $success=$true
 [ordered]@{qualified=$true;product='psx-cdu-client';version=$version;configuration=$Configuration;package=$package;payloads=$files.Count;manifestSHA256=(Get-FileHash -LiteralPath (Join-Path $package 'manifest.json')).Hash;copies=$startedCopies.ToArray();mutableFilesCopied=0;canonicalPublication=$false}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $work 'candidate-result.json') -Encoding utf8
 Get-Content -LiteralPath (Join-Path $work 'candidate-result.json') -Raw
}catch{$failure=$_.ToString();throw}
finally{
 CompleteCduCollection $lease $success $failure $startedCopies.ToArray() $work
}
