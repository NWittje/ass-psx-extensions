#requires -Version 7.0
[CmdletBinding()]
param(
 [Parameter(Mandatory)][ValidateSet('Prepare','Commit')][string]$Phase,
 [Parameter(Mandatory)][string]$WorkDirectory,
 [Parameter(Mandatory)][string]$CommitDirectory,
 [string]$NativePlan=''
)
$ErrorActionPreference='Stop'
$repo='P:\development\psx_ios'
$owner='display-network-20261024'
function Import-Functions([string]$Path){
 $tokens=$null;$errors=$null
 $ast=[Management.Automation.Language.Parser]::ParseFile($Path,[ref]$tokens,[ref]$errors)
 if($errors.Count){throw "Tool does not parse: $Path"}
 foreach($node in $ast.FindAll({param($n)$n-is[Management.Automation.Language.FunctionDefinitionAst]},$false)){
  . ([scriptblock]::Create(($node.Extent.Text-replace '^function\s+[^\s({]+',('function script:'+$node.Name))))
 }
}
. "$repo/scripts/deployment/common.ps1"
. "$repo/scripts/package-layout.ps1"
. "$repo/source/modules/psx-du-host/tools/runtime-payload.ps1"
. "$repo/source/modules/psx-du-host/tools/graphics144-inputs.ps1"
Import-Functions "$repo/scripts/publish-binaries.ps1"
function Verify-Inputs($Entry,$Stamp){
 $prefix=[IO.Path]::GetFullPath((Split-Path $Entry.source -Parent)).TrimEnd('\')+'\'
 foreach($row in $Stamp.inputs){
  $file=Get-Item -LiteralPath $row.path
  SyncIdentity $row.path $file.Length $row.sha256
  if($row.path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){
   SyncIdentity (Join-Path $repo $row.path.Substring($prefix.Length)) $file.Length $row.sha256
  }
 }
}
function Mutable([string]$Path){
 $Path-in@('settings.cfg','settings.cfg.bak')-or$Path-match'^profiles[\\/][^\\/]+\.cfg(\.bak)?$'
}
function Audit([string]$Package){
 $code=@'
import ast,json,struct,sys
from pathlib import Path
tool,folder=sys.argv[1:]
tree=ast.parse(Path(tool).read_text(encoding="utf-8-sig"))
nodes=[n for n in tree.body if isinstance(n,(ast.Import,ast.ImportFrom)) or isinstance(n,ast.FunctionDef) and n.name in ("pe_identity","pdb_identity")]
ns={};exec(compile(ast.Module(body=nodes,type_ignores=[]),tool,"exec"),ns)
root=Path(folder);symbols=[];imports=[]
for binary in list(root.glob("psx-*.exe"))+[root/"ass_graphics_lib.dll"]:
    pe=ns["pe_identity"](binary);pdb=ns["pdb_identity"](root/"symbols"/(binary.stem+".pdb"))
    assert pe==pdb,(binary.name,pe,pdb)
    symbols.append(dict(binary=binary.name,guid=pe[0],age=pe[1],matched=True))
for binary in list(root.glob("*.exe"))+list(root.glob("*.dll")):
    b=binary.read_bytes();pe=struct.unpack_from("<I",b,60)[0]
    assert struct.unpack_from("<H",b,pe+4)[0]==0x8664
    count=struct.unpack_from("<H",b,pe+6)[0];osize=struct.unpack_from("<H",b,pe+20)[0];opt=pe+24
    assert struct.unpack_from("<H",b,opt)[0]==0x20b
    def offset(rva):
        for n in range(count):
            vs,va,raw_size,raw=struct.unpack_from("<4I",b,opt+osize+n*40+8)
            if va<=rva<va+max(vs,raw_size):return raw+rva-va
        raise ValueError("Unmapped import RVA")
    rva,size=struct.unpack_from("<2I",b,opt+112+8)
    if not rva:continue
    at=offset(rva)
    while any(struct.unpack_from("<5I",b,at)):
        start=offset(struct.unpack_from("<I",b,at+12)[0]);dll=b[start:b.index(0,start)].decode("ascii")
        private=(root/dll).is_file()
        system=(Path("C:/Windows/System32")/dll).is_file() or dll.lower().startswith(("api-ms-win-","ext-ms-win-"))
        assert private or system,(binary.name,dll)
        imports.append(dict(binary=binary.name,dependency=dll,private=private,system=system));at+=20
print(json.dumps(dict(symbols=symbols,imports=imports,scope="Static x64 imports and matching PE/PDB identities")))
'@
 $python='C:\Users\NWittje\AppData\Local\Python\pythoncore-3.14-64\python.exe'
 $result=(& $python -B -c $code "$repo/source/modules/psx-cdu-client/tools/audit.py" $Package)-join[Environment]::NewLine
 if($LASTEXITCODE){throw 'PE/symbol/import audit failed'}
 $result|ConvertFrom-Json
}
$work=[IO.Path]::GetFullPath($WorkDirectory).TrimEnd('\')
$commit=[IO.Path]::GetFullPath($CommitDirectory).TrimEnd('\')
if(!$work.StartsWith('O:\psx\bin\.work\',[StringComparison]::OrdinalIgnoreCase)-or
 !$commit.StartsWith("$repo\bin\.work\",[StringComparison]::OrdinalIgnoreCase)){throw 'Publication storage escaped its actual roots'}
. "$repo/scripts/work-cleanup.ps1" -CleanupProjectRoot 'O:\psx'
$oLease=Enter-WorkCleanup -WorkDirectory $work -Owner $owner
$pLease=$null;$complete=$false;$plan=$null
try{
 [void][IO.Directory]::CreateDirectory($work)
 . "$repo/scripts/work-cleanup.ps1" -CleanupProjectRoot $repo
 $pLease=Enter-WorkCleanup -WorkDirectory $commit -Owner $owner
 [void][IO.Directory]::CreateDirectory($commit)
 $planPath=Join-Path $work 'publication-plan.json'
 if($Phase-eq'Prepare'){
  if(Test-Path -LiteralPath $planPath){throw 'Prepared plan exists; preserve its original identity'}
  $entries=@(Get-Content -LiteralPath $NativePlan -Raw|ConvertFrom-Json)
  if($entries.Count-ne3){throw 'Release plans for all three products required'}
  $keys=@{};$prepared=[Collections.Generic.List[object]]::new()
  foreach($entry in $entries){
   if($entry.product-notin@('Host','Client','CduClient')-or$entry.configuration-cne'Release'){throw 'Invalid native plan'}
   $key=$entry.product+'/'+$entry.configuration;if($keys.ContainsKey($key)){throw 'Duplicate native plan'};$keys[$key]=$true
   foreach($path in @($entry.source,$entry.build)){
    if(!$path.StartsWith('O:\psx\bin\.work\',[StringComparison]::OrdinalIgnoreCase)){throw 'Native input escaped qualified local work'}
    Assert-WorkCleanupOrdinaryPath $path
   }
   $name=switch($entry.product){Host{'psx-du-host'} Client{'psx-du-client'} CduClient{'psx-cdu-client'}}
   $stampPath=Join-Path $entry.build $(if($entry.product-eq'CduClient'){'client-qualification.json'}else{'build-qualification.json'})
   $stamp=Get-Content -LiteralPath $stampPath -Raw|ConvertFrom-Json
   if(!$stamp.clean-or$stamp.configuration-cne$entry.configuration){throw 'Matching clean qualification required'}
   if($entry.product-eq'CduClient'){
    if(@($stamp.testGroups).Count-lt28-or$stamp.testSummary-cne("$(@($stamp.testGroups).Count)/$(@($stamp.testGroups).Count) passed")){throw 'Incomplete CDU qualification'}
   }elseif(!$stamp.tests-or@($stamp.tests.functionalFailures).Count){throw 'Functional DU qualification failure'}
   Verify-Inputs $entry $stamp
   $sdk=Get-DuGraphics144Sdk $entry.sdk $entry.configuration
   $version=Get-DisplayProductVersion $entry.product $entry.source
   if($version-cne(Get-DisplayProductVersion $entry.product "$repo/source")){throw 'Maintained/native product versions differ'}
   $active=Join-Path $repo "bin/$($entry.configuration)/$name"
   $stage=Join-Path $commit "stage/$($entry.configuration)/$name"
   if(Test-Path -LiteralPath $stage){throw 'Existing stage is preserved'}
   $old=@(Get-PublicationInventory $active)
   $oldManifest=Get-Content -LiteralPath (Join-Path $active 'manifest.json') -Raw|ConvertFrom-Json
   foreach($row in $oldManifest.files){
    $bytes=if($row.PSObject.Properties['bytes']){$row.bytes}else{$row.size}
    SyncIdentity (Join-Path $active $row.path) $bytes $row.sha256
   }
   $known=@($oldManifest.files.path)+@('manifest.json')
   foreach($row in $old){if(!(Mutable $row.Path)-and$row.Path.Replace('\','/')-notin$known){throw "Unclassified existing operator asset: $active/$($row.Path)"}}
   $payload=@(Get-DisplayRuntimePayload $entry.product $entry.source $entry.build $entry.configuration (Join-Path $sdk.configurationRoot 'bin/ass_graphics_lib.dll') (Join-Path $sdk.configurationRoot 'symbols/ass_graphics_lib.pdb') (Join-Path $sdk.configurationRoot 'licenses'))
   $records=@(foreach($row in $payload){[pscustomobject]@{Source=$row.source;Target=(Join-Path $stage $row.relative);Bytes=(Get-Item $row.source).Length;SHA256=(SyncHash $row.source)}})
   $mutable=@($old|Where-Object{Mutable $_.Path})
   $records+=@(foreach($row in $mutable){[pscustomobject]@{Source=(Join-Path $active $row.Path);Target=(Join-Path $stage $row.Path);Bytes=$row.Bytes;SHA256=$row.SHA256}})
   # Every destination is P: commit staging; the normal pCloud route is native.
   SyncNativeVerifiedTransfer $records (Join-Path $work "$key/transfers") 'Native verified FileStream for P/cloud commit staging'
   if($entry.product-eq'CduClient'){
    # Bind the guide's dependency label to this qualified SDK; native inputs stay original.
    $guidePath=Join-Path $stage 'README.md'
    $guideText=[IO.File]::ReadAllText($guidePath)
    $guideText=[regex]::Replace($guideText,'ASS Graphics\s*1\.4\.[0-9]+',('ASS Graphics '+$sdk.version))
    [IO.File]::WriteAllText($guidePath,$guideText,[Text.UTF8Encoding]::new($false))
    [void][IO.Directory]::CreateDirectory((Join-Path $stage 'corresponding-source'))
    New-CduShaderSourceArchive $entry.source (Join-Path $stage 'corresponding-source/shaders.zip')
   }
   Assert-DisplayFileDetails (Join-Path $stage "$name.exe") $version 'ASS PSX Addons'
   Assert-DisplayFileDetails (Join-Path $stage 'ass_graphics_lib.dll') '1.4.4' 'ASS Graphics Lib'
   $binary=Audit $stage
   $files=@(Get-PublicationInventory $stage|Where-Object{!(Mutable $_.Path)}|ForEach-Object{
    [ordered]@{path=$_.Path.Replace('\','/');size=$_.Bytes;sha256=$_.SHA256}
   })
   $proof=[ordered]@{stampPath=$stampPath;stampSHA256=(SyncHash $stampPath);clean=$true;
    tests=$(if($entry.product-eq'CduClient'){$stamp.testSummary}else{$stamp.tests});testLogSha256=$stamp.testLogSha256;
    sourceRoot=$entry.source;inputs=$stamp.inputs}
   [ordered]@{schema=1;product=$name;version=$version;configuration=$entry.configuration;graphics='1.4.4';abi=1;
    layout='runtime-v1';qualification=$proof;binaryEvidence=$binary;files=$files;
    graphicsOrigin=$sdk;mutableExcluded=@('settings.cfg','settings.cfg.bak','profiles/*.cfg','profiles/*.cfg.bak')
   }|ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding utf8
   AssertRuntimePackageLayout -Directory $stage -Product $name
   Assert-PublicationInventory $active $old
   Verify-Inputs $entry $stamp
   foreach($role in 'previous','rejected'){
    [void][IO.Directory]::CreateDirectory((Join-Path $commit "$role/$($entry.configuration)"))
   }
   $prepared.Add([pscustomobject]@{product=$entry.product;name=$name;version=$version;configuration=$entry.configuration;
    native=$entry;stage=$stage;active=$active;previous=(Join-Path $commit "previous/$($entry.configuration)/$name");
    rejected=(Join-Path $commit "rejected/$($entry.configuration)/$name");old=$old;expected=@(Get-PublicationInventory $stage);
    sources=$records;mutable=$mutable;committed=$false;manifestSHA256=(SyncHash (Join-Path $stage 'manifest.json'))})
  }
  $plan=[ordered]@{schema=1;issuerToolSHA256=(SyncHash $PSCommandPath);commit=$commit;work=$work;entries=$prepared.ToArray();committed=$false}
  $plan|ConvertTo-Json -Depth 18|Set-Content -LiteralPath $planPath -Encoding utf8
  Write-Output 'Prepared and audited three matching Release packages; active packages unchanged.'
 }else{
  $plan=Get-Content -LiteralPath $planPath -Raw|ConvertFrom-Json
  if($plan.issuerToolSHA256-cne(SyncHash $PSCommandPath)-or$plan.commit-ine$commit-or$plan.work-ine$work-or$plan.committed){throw 'Publication plan identity differs or already committed'}
  foreach($entry in $plan.entries){
   Assert-PublicationProductIdle @($entry.active) @($entry.name+'.exe','psx-cdu-host.exe')
   Assert-PublicationInventory $entry.active $entry.old
   Assert-PublicationInventory $entry.stage $entry.expected
   $stampFile=Join-Path $entry.native.build $(if($entry.product-eq'CduClient'){'client-qualification.json'}else{'build-qualification.json'})
   Verify-Inputs $entry.native (Get-Content -LiteralPath $stampFile -Raw|ConvertFrom-Json)
  }
  foreach($entry in $plan.entries){
   $key=if($entry.product-eq'CduClient'){'Local\ASS.PSX.CDU.Client.PackagePublication'}else{"Local\PSXIOS.Publish.DU$($entry.product).$($entry.configuration)"}
   $mutex=[Threading.Mutex]::new($false,$key);$locked=$false
   try{
    try{$locked=$mutex.WaitOne(10000)}catch [Threading.AbandonedMutexException]{$locked=$true}
    if(!$locked){throw 'Another publisher holds this product/configuration'}
    $guard={
     param($phase,$source,$destination)
     $null=Assert-WorkCleanupParent -WorkDirectory (Join-Path $commit 'authority-check') -Owner $owner -Context $pLease.ParentContext -AllowSameProcess
     if(!$locked-or$mutex.SafeWaitHandle.IsClosed){throw 'Publication mutex ownership lost'}
     Assert-PublicationProductIdle @($entry.active) @($entry.name+'.exe','psx-cdu-host.exe')
    }
    Assert-PublicationInventory $entry.active $entry.old
    Assert-PublicationInventory $entry.stage $entry.expected
    $Module='Display';$Configuration=$entry.configuration
    Commit-Publication $entry.stage $entry.active $entry.previous $entry.rejected $entry.expected $entry.sources $guard
    Assert-PublicationInventory $entry.active $entry.expected
    $entry.committed=$true
    $plan|ConvertTo-Json -Depth 18|Set-Content -LiteralPath $planPath -Encoding utf8
   }finally{if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
  }
  $plan.committed=$true
  $plan|ConvertTo-Json -Depth 18|Set-Content -LiteralPath $planPath -Encoding utf8
  Write-Output 'Deployed three Release packages with preserved settings and matching hashes; no applications or simulators started.'
 }
 $complete=$true
}finally{
 if($pLease){
  . "$repo/scripts/work-cleanup.ps1" -CleanupProjectRoot $repo
  try{Set-WorkCleanupDisposition -Lease $pLease -State Held -Reason 'Display staging/rollback consumer; review after the delivery receipt is consolidated' -Findings 'docs/research/display-network-selection.md'}finally{Exit-WorkCleanup $pLease}
 }
 . "$repo/scripts/work-cleanup.ps1" -CleanupProjectRoot 'O:\psx'
 try{Set-WorkCleanupDisposition -Lease $oLease -State Held -Reason 'Publication/source audit consumer; review after central validation is consolidated' -Findings 'docs/research/display-network-selection.md'}finally{Exit-WorkCleanup $oLease}
}
