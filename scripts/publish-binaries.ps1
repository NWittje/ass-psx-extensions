[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Plugin','Coordinator','Exporter','HWIO','Reposition')][string]$Module,
    [Parameter(Mandatory=$true)][ValidateSet('Debug','Release')][string]$Configuration,
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [string]$BinDirectory = '',
    [switch]$FreshDefaults,
    [switch]$CoordinatedPublication,
    [switch]$NativeVerifiedStaging,
    [string]$ParentCleanupOwner = '',
    [string]$SourceDirectory = '',
    [string]$CleanupProjectRoot = 'O:\psx',
    [string]$CommitWorkDirectory = ''
)
$ErrorActionPreference = 'Stop'
if($Configuration -ne 'Release' -and (-not $BinDirectory -or [IO.Path]::GetFullPath($BinDirectory).TrimEnd('\','/') -ieq 'P:\development\psx_ios\bin')){throw 'Project publication is Release-only; Debug remains internal qualification output.'}
$projectRoot = 'P:\development\psx_ios'
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
if(-not $SourceDirectory){
    $nativeSource=@(Get-Content -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') | Where-Object {$_ -match '^CMAKE_HOME_DIRECTORY:INTERNAL='})
    if($nativeSource.Count -ne 1){throw 'Native source provenance is missing/ambiguous; supply qualified SourceDirectory explicitly.'}
    $nativeModule=($nativeSource[0] -split '=',2)[1]
    $SourceDirectory=Split-Path -Parent (Split-Path -Parent $nativeModule)
}
$sourceRoot = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\','/')
if(-not (Test-Path -LiteralPath (Join-Path $sourceRoot 'modules') -PathType Container)){throw 'Explicit qualified native source directory is missing.'}
$binRoot = if($BinDirectory) { [IO.Path]::GetFullPath($BinDirectory) } else { Join-Path $projectRoot 'bin' }
. (Join-Path $PSScriptRoot 'package-layout.ps1')
. (Join-Path $PSScriptRoot 'work-cleanup.ps1') -CleanupProjectRoot $CleanupProjectRoot
$script:publicationCleanupHelper=Join-Path $PSScriptRoot 'work-cleanup.ps1'
$publicationWorkRoot=$script:WorkCleanupProjectRoot
$workBase=Join-Path $projectRoot 'bin/.work'
$null=Resolve-WorkCleanupPath -WorkDirectory $buildRoot
if($binRoot -ine (Join-Path $projectRoot 'bin') -and -not $binRoot.StartsWith($workBase+'\',[StringComparison]::OrdinalIgnoreCase) -and -not $binRoot.StartsWith((Join-Path $publicationWorkRoot 'bin/.work')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Publication destination must be canonical P: bin or an owned qualification fixture.'}
Assert-WorkCleanupOrdinaryPath $binRoot
$publicationWork=Join-Path $buildRoot ('publication-'+[Guid]::NewGuid().ToString('N'))
$publicationCommitted=$false
$publicationComplete=$false
$publicationRollbackUnresolved=$false
$publicationFailure=$null
$publicationScope=$null
$publicationCommitScope=$null
$script:publicationSources=[Collections.Generic.List[object]]::new()

function Write-PublicationWarning([string]$Message) {
    # Reporting cannot replace the transfer/commit exception, even with
    # WarningPreference=Stop or a broken reporting consumer.
    try{Write-Warning $Message}catch{}
}
function Open-PublicationWork([string]$Work,[string]$ExpectedParentOwner) {
    $null=Resolve-WorkCleanupPath -WorkDirectory $Work
    if(Test-Path -LiteralPath $Work){throw 'Publication requires a fresh owned work directory.'}
    if($env:PSX_WORK_CLEANUP_PARENT){
        if(-not $ExpectedParentOwner){throw 'An inherited publication lease requires explicit ParentCleanupOwner from the maintained caller.'}
        # Qualification reads the shared queue. Serialize that read with its
        # atomic writers; unrelated builders may finish while we borrow.
        $catalog=Enter-WorkCleanupMutex 'catalog'
        try{$borrow=Assert-WorkCleanupParent -WorkDirectory $Work -Owner $ExpectedParentOwner -AllowSameProcess}
        finally{Exit-WorkCleanupMutex $catalog}
        [IO.Directory]::CreateDirectory($Work)|Out-Null
        return [pscustomobject]@{Lease=$null;Borrow=$borrow;Context=$env:PSX_WORK_CLEANUP_PARENT;Work=$Work;Root=$script:WorkCleanupProjectRoot}
    }
    if($ExpectedParentOwner){throw 'ParentCleanupOwner was supplied without a live parent context.'}
    $lease=Enter-WorkCleanup -WorkDirectory $Work -Owner 'common-publisher'
    try{[IO.Directory]::CreateDirectory($Work)|Out-Null}
    catch{
        $primary=$_
        try{Exit-WorkCleanup -Lease $lease}catch{Write-PublicationWarning ('Lease close failed: '+$_.Exception.Message)}
        throw $primary
    }
    return [pscustomobject]@{Lease=$lease;Borrow=$null;Work=$Work;Root=$script:WorkCleanupProjectRoot}
}
function Open-PublicationCommitWork([string]$Work) {
    . $script:publicationCleanupHelper -CleanupProjectRoot 'P:\development\psx_ios'
    $work=Resolve-WorkCleanupPath $Work
    if(Test-Path -LiteralPath $work){throw 'Stable commit requires a fresh P: work subtree.'}
    # This is an independent actual P: lease; an O: parent is never borrowed.
    $lease=Enter-WorkCleanup -WorkDirectory $work -Owner 'common-publisher-commit'
    try{[IO.Directory]::CreateDirectory($work)|Out-Null}
    catch{$primary=$_;try{Exit-WorkCleanup $lease}catch{Write-PublicationWarning $_.Exception.Message};throw $primary}
    [pscustomobject]@{Lease=$lease;Borrow=$null;Work=$work;Root='P:\development\psx_ios'}
}
function Prepare-PublicationStableCommit([string]$Stage,[string]$Target,[object[]]$Expected,[string]$Work) {
    if(-not $Target.StartsWith('P:\development\psx_ios\bin\',[StringComparison]::OrdinalIgnoreCase)){throw 'Cross-volume publication must return to canonical P: bin.'}
    if([IO.Path]::GetPathRoot($Stage)-ieq[IO.Path]::GetPathRoot($Target)){throw 'Stable transfer is only required across volumes.'}
    $scope=Open-PublicationCommitWork $Work
    try {
        $stableStage=Join-Path $scope.Work 'stage'
        $records=@($Expected|ForEach-Object{[pscustomobject]@{Source=(Join-Path $Stage $_.Path);Target=(Join-Path $stableStage $_.Path);Bytes=$_.Bytes;SHA256=$_.SHA256}})
        $script:publicationTransferUncertain=$true
        $evidence=Join-Path $publicationWork 'commit-transfers'
        if($NativeVerifiedStaging){NativeVerifiedStage $records $evidence|Out-Null}
        else{SyncTransfer $records $evidence 'C:\Program Files\TeraCopy\TeraCopy.exe'|Out-Null}
        $script:publicationTransferUncertain=$false
        Assert-PublicationInventory $stableStage $Expected
        [pscustomobject]@{Scope=$scope;Stage=$stableStage;Backup=(Join-Path $scope.Work 'previous');Rejected=(Join-Path $scope.Work 'rejected')}
    } catch {
        $primary=$_
        try{Finish-PublicationWork $scope $false $script:publicationTransferUncertain $false}catch{Write-PublicationWarning $_.Exception.Message}
        throw $primary
    }
}
function Get-PublicationInventory([string]$Root) {
    Assert-WorkCleanupOrdinaryPath $Root
    $result=[Collections.Generic.List[object]]::new()
    $pending=[Collections.Generic.Stack[string]]::new();$pending.Push($Root)
    while($pending.Count){
        foreach($item in Get-ChildItem -LiteralPath $pending.Pop() -Force -ErrorAction Stop){
            if($item.Attributes-band[IO.FileAttributes]::ReparsePoint){throw 'Publication tree contains a reparse point.'}
            if($item.PSIsContainer){$pending.Push($item.FullName)}
            else{$result.Add([pscustomobject]@{Path=$item.FullName.Substring($Root.Length+1);Bytes=$item.Length;SHA256=(FileHash $item.FullName)})}
        }
    }
    return $result.ToArray()
}
function Assert-PublicationInventory([string]$Root,[object[]]$Expected) {
    $actual=@(Get-PublicationInventory $Root)
    if($actual.Count-ne$Expected.Count){throw "Publication closure changed: $Root"}
    $known=@{};foreach($item in $actual){$known[$item.Path]=$item}
    foreach($item in $Expected){$found=$known[$item.Path];if(-not $found-or$found.Bytes-ne$item.Bytes-or$found.SHA256-ne$item.SHA256){throw "Publication identity changed: $Root/$($item.Path)"}}
}
function Assert-PublicationSources([object[]]$Sources,[string]$Stage='', [string]$StageNow='', [string]$PreviousTarget='', [string]$PreviousNow='') {
    foreach($item in $Sources){
        $path=$item.Source
        if($Stage-and$StageNow-and$path.StartsWith($Stage+'\',[StringComparison]::OrdinalIgnoreCase)){$path=Join-Path $StageNow $path.Substring($Stage.Length+1)}
        elseif($PreviousTarget-and$PreviousNow-and$path.StartsWith($PreviousTarget+'\',[StringComparison]::OrdinalIgnoreCase)){$path=Join-Path $PreviousNow $path.Substring($PreviousTarget.Length+1)}
        SyncIdentity $path $item.Bytes $item.SHA256
    }
}
function Move-PublicationDirectory([string]$Source,[string]$Destination) {
    Assert-WorkCleanupOrdinaryPath $Source;Assert-WorkCleanupOrdinaryPath $Destination
    if([IO.Path]::GetPathRoot($Source)-ine[IO.Path]::GetPathRoot($Destination)){throw 'Atomic publication requires one volume.'}
    [IO.Directory]::Move($Source,$Destination)
}
function Get-PublicationNamespaceState([string]$Path) {
    Assert-WorkCleanupOrdinaryPath $Path
    if(-not(Test-Path -LiteralPath $Path -ErrorAction Stop)){return [pscustomobject]@{Exists=$false;Files=@();Directories=@()}}
    if(-not(Test-Path -LiteralPath $Path -PathType Container -ErrorAction Stop)){throw "Publication namespace is not a directory: $Path"}
    $directories=[Collections.Generic.List[string]]::new();$pending=[Collections.Generic.Stack[string]]::new();$pending.Push($Path)
    while($pending.Count){foreach($item in Get-ChildItem -LiteralPath $pending.Pop() -Force -ErrorAction Stop){
        if($item.Attributes-band[IO.FileAttributes]::ReparsePoint){throw 'Publication namespace contains a reparse point.'}
        if($item.PSIsContainer){$directories.Add($item.FullName.Substring($Path.Length+1));$pending.Push($item.FullName)}
    }}
    [pscustomobject]@{Exists=$true;Files=@(Get-PublicationInventory $Path);Directories=$directories.ToArray()}
}
function Assert-PublicationNamespaceState([string]$Path,$State) {
    if($null-eq$State){throw 'Publication namespace requires its captured state.'}
    Assert-WorkCleanupOrdinaryPath $Path
    if(-not$State.Exists){if(Test-Path -LiteralPath $Path -ErrorAction Stop){throw "Unexpected publication namespace: $Path"};return}
    if(-not(Test-Path -LiteralPath $Path -PathType Container -ErrorAction Stop)){throw "Publication directory disappeared: $Path"}
    Assert-PublicationInventory $Path @($State.Files)
    $directories=[Collections.Generic.List[string]]::new();$pending=[Collections.Generic.Stack[string]]::new();$pending.Push($Path)
    while($pending.Count){foreach($item in Get-ChildItem -LiteralPath $pending.Pop() -Force -ErrorAction Stop){
        if($item.Attributes-band[IO.FileAttributes]::ReparsePoint){throw 'Publication namespace contains a reparse point.'}
        if($item.PSIsContainer){$directories.Add($item.FullName.Substring($Path.Length+1));$pending.Push($item.FullName)}
    }}
    $known=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($name in @($State.Directories)){$null=$known.Add([string]$name)}
    if(-not$known.SetEquals([string[]]$directories.ToArray())){throw "Publication directory namespace changed: $Path"}
}
function New-PublicationAuthority($Scope,$Mutex,[bool]$Locked,[string]$Target,$OriginalTarget,[string[]]$ExecutableNames=@()) {
    if(-not$Scope-or-not$Mutex-or-not$Locked){throw 'Publication requires its actual work scope and publication mutex.'}
    $context=if($Scope.Lease){Get-WorkCleanupParentContext -Lease $Scope.Lease}else{$Scope.Context}
    if(-not$context){throw 'Publication scope has no qualified issuer context.'}
    $record=$context|ConvertFrom-Json -ErrorAction Stop
    [pscustomobject]@{Scope=$Scope;Context=$context;Owner=$record.Owner;Target=$Target;OriginalTarget=$OriginalTarget;ExecutableNames=$ExecutableNames;Mutex=$Mutex;OwnsMutex=$Locked;IssuerPid=$PID;IssuerStartedUtcTicks=(Get-Process -Id $PID -ErrorAction Stop).StartTime.ToUniversalTime().Ticks;IssuerThreadId=[Threading.Thread]::CurrentThread.ManagedThreadId}
}
function Get-PublicationProcesses {Get-CimInstance Win32_Process -ErrorAction Stop}
function Assert-PublicationProductIdle([string[]]$Roots,[string[]]$ExecutableNames) {
    if(-not$ExecutableNames.Count){return}
    $processes=@(Get-PublicationProcesses)
    if(-not$processes.Count){throw 'Windows process inventory is empty; product state is unknown.'}
    foreach($process in $processes){
        if($process.Name-notin$ExecutableNames){continue}
        if(-not$process.ExecutablePath){throw 'Matching publication product has an unavailable executable path.'}
        $path=[IO.Path]::GetFullPath([string]$process.ExecutablePath)
        foreach($root in $Roots){if($root-and$path.StartsWith($root.TrimEnd('\','/')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'A product is still using this publication namespace.'}}
    }
}
function Assert-PublicationAuthority($Authority,[string]$Phase,[string]$Source,[string]$Destination) {
    if(-not$Authority-or$Authority.IssuerPid-ne$PID-or$Authority.IssuerThreadId-ne[Threading.Thread]::CurrentThread.ManagedThreadId-or
       $Authority.IssuerStartedUtcTicks-ne(Get-Process -Id $PID -ErrorAction Stop).StartTime.ToUniversalTime().Ticks-or
       -not$Authority.OwnsMutex-or-not$Authority.Mutex-or$Authority.Mutex.SafeWaitHandle.IsClosed-or
       $Authority.Scope.Root-ine$script:WorkCleanupProjectRoot){throw 'Publication issuer, thread, root or mutex authority changed.'}
    foreach($endpoint in @($Source,$Destination)){
        if($endpoint-and$endpoint-ine$Authority.Target-and-not$endpoint.StartsWith($Authority.Scope.Work.TrimEnd('\','/')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Publication mutation escaped its actual issuer work scope.'}
    }
    if($Phase-in'Preflight','MovePrevious'){Assert-PublicationNamespaceState $Authority.Target $Authority.OriginalTarget}
    Assert-PublicationProductIdle @($Authority.Target,$Source,$Destination) $Authority.ExecutableNames
    $catalog=Enter-WorkCleanupMutex 'catalog'
    try{$null=Assert-WorkCleanupParent -WorkDirectory (Join-Path $Authority.Scope.Work 'authority-check') -Owner $Authority.Owner -Context $Authority.Context -AllowSameProcess}
    finally{Exit-WorkCleanupMutex $catalog}
    $acquired=$false
    try{$acquired=$Authority.Mutex.WaitOne(0);if(-not$acquired){throw 'Publication mutex is not owned by this issuer thread.'}}
    finally{if($acquired){$Authority.Mutex.ReleaseMutex()}}
}
function Invoke-PublicationAuthority([scriptblock]$Guard,[string]$Phase,[string]$Source,[string]$Destination) {
    if(-not$Guard){throw 'Actual publication authority callback is required.'}
    $output=@(& $Guard $Phase $Source $Destination)
    if($output.Count){throw 'Publication authority must assert silently or throw.'}
}
function Assert-PublicationRoles($Plan,[string]$StageRole,[string]$TargetRole,[string]$BackupRole,[string]$RejectedRole) {
    $states=@{stage=$Plan.Staged;old=$Plan.Previous;absent=[pscustomobject]@{Exists=$false;Files=@();Directories=@()}}
    foreach($pair in @(@($Plan.Stage,$StageRole),@($Plan.Target,$TargetRole),@($Plan.Backup,$BackupRole),@($Plan.Rejected,$RejectedRole))){Assert-PublicationNamespaceState $pair[0] $states[$pair[1]]}
}
function Get-RetiredDiagnosticsExampleSource([string]$Target,[object[]]$Inventory,[string]$PublicationModule,[string]$PublicationConfiguration,[string]$Relative='diagnostics.ini.example') {
    if($PublicationModule-notin'Plugin','Coordinator'){return}
    $name=$Relative.Replace('\','/')
    if($name-ne'diagnostics.ini.example'-and-not($PublicationModule-eq'Plugin'-and$name-eq'support/Sparky/state/scripts/prepare-aircraft-state.py')-and-not($PublicationModule-eq'Coordinator'-and$name-eq'connection-templates/coordinator.network.ini')){
        throw 'Unqualified package retirement path for this product.'
    }
    $example=SyncChild $Target $name
    if(Test-Path -LiteralPath $example -PathType Container){throw 'Retired retired package file is customer directory data; publication refused.'}
    $actual=@($Inventory|Where-Object {$_.Path.Replace('\','/')-ieq$name})
    $manifestPath=Join-Path $Target 'package-manifest.json'
    if(-not(Test-Path -LiteralPath $manifestPath -PathType Leaf)){
        if($actual.Count){throw 'Cannot retire an unqualified retired package file without its old manifest.'}
        return
    }
    $parse=@{InputObject=[IO.File]::ReadAllText($manifestPath)}
    if((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')){$parse.DateKind='String'}
    $manifest=ConvertFrom-Json @parse
    $entry=@($manifest.Files|Where-Object {([string]$_.Path).Replace('\','/')-ieq$name})
    if(-not$entry.Count-and-not$actual.Count){return}
    if($manifest.Product-cne$PublicationModule-or$manifest.Configuration-cne$PublicationConfiguration-or$entry.Count-ne1-or$actual.Count-ne1){
        throw 'Previous retired package file lacks its exact product/configuration/immutable manifest binding.'
    }
    if(-not$entry[0].PSObject.Properties['Bytes']-or$entry[0].Bytes-lt0-or[string]$entry[0].SHA256-notmatch'^[a-fA-F0-9]{64}$'-or
       $entry[0].Bytes-ne$actual[0].Bytes-or$entry[0].SHA256-ine$actual[0].SHA256){
        throw 'Previous immutable retired package file changed; preserve customer bytes and refuse publication.'
    }
    # This source is omitted from the new payload, but follows the old package
    # through every existing source/rollback check, including concurrent edits.
    [pscustomobject]@{Source=$example;Bytes=$actual[0].Bytes;SHA256=$actual[0].SHA256}
}

function Commit-Publication([string]$Stage,[string]$Target,[string]$Backup,[string]$Rejected,[object[]]$Expected,[object[]]$Sources,[scriptblock]$ValidateAuthority) {
    $script:publicationRollbackUnresolved=$false;$script:publicationRecoveryFailure=$null
    Invoke-PublicationAuthority $ValidateAuthority Preflight $Stage $Target
    $paths=@($Stage,$Target,$Backup,$Rejected)
    foreach($path in $paths){if(-not[IO.Path]::IsPathRooted($path)-or[IO.Path]::GetFullPath($path).TrimEnd('\','/')-cne$path.TrimEnd('\','/')){throw 'Publication requires explicit normalized absolute paths.'}}
    for($i=0;$i-lt$paths.Count;$i++){for($j=$i+1;$j-lt$paths.Count;$j++){
        $a=$paths[$i].TrimEnd('\','/');$b=$paths[$j].TrimEnd('\','/')
        if($a-ieq$b-or$a.StartsWith($b+'\',[StringComparison]::OrdinalIgnoreCase)-or$b.StartsWith($a+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Publication namespace roles overlap.'}
        if([IO.Path]::GetPathRoot($a)-ine[IO.Path]::GetPathRoot($b)){throw 'Publication namespace mutation requires one volume.'}
    }}
    foreach($path in $Stage,$Backup,$Rejected){$null=Resolve-WorkCleanupPath -WorkDirectory $path}
    $plan=[pscustomobject]@{Stage=$Stage;Target=$Target;Backup=$Backup;Rejected=$Rejected;Staged=(Get-PublicationNamespaceState $Stage);Previous=(Get-PublicationNamespaceState $Target)}
    if(-not$plan.Staged.Exists){throw 'Publication stage is missing.'}
    Assert-PublicationInventory $Stage $Expected
    Assert-PublicationRoles $plan stage old absent absent
    $oldExists=$plan.Previous.Exists;$old=@($plan.Previous.Files)
    $previousMoved=$false;$installed=$false;$moveAttempted=$false
    try{
        if($Module-in'Plugin','Coordinator'-and$oldExists){
            $retired=@(Get-RetiredDiagnosticsExampleSource $Target $old $Module $Configuration)
            if($Module-eq'Plugin'){$retired+=@(Get-RetiredDiagnosticsExampleSource $Target $old $Module $Configuration 'support/Sparky/state/scripts/prepare-aircraft-state.py')}
            if($Module-eq'Coordinator'){$retired+=@(Get-RetiredDiagnosticsExampleSource $Target $old $Module $Configuration 'connection-templates/coordinator.network.ini')}
            if($retired.Count){$Sources=@($Sources)+@($retired)}
        }
        if($Module-eq'HWIO'-and$oldExists){
            Assert-HwioPreviousPublicationPackage $Target $old
            Assert-HwioPreservedPublicationStage $old $Expected ([bool]$FreshDefaults)
        }
        Assert-PublicationSources $Sources
        if($oldExists){
            Invoke-PublicationAuthority $ValidateAuthority MovePrevious $Target $Backup
            Assert-PublicationRoles $plan stage old absent absent
            Assert-PublicationSources $Sources
            $moveAttempted=$true;Move-PublicationDirectory $Target $Backup;$previousMoved=$true
        }
        if($Module-eq'HWIO'-and$previousMoved){Assert-PublicationInventory $Backup $old}
        # Captured operator input follows its old directory during namespace commit.
        Assert-PublicationSources $Sources -PreviousTarget $Target -PreviousNow $(if($previousMoved){$Backup}else{''})
        Invoke-PublicationAuthority $ValidateAuthority Install $Stage $Target
        Assert-PublicationRoles $plan stage $(if($previousMoved){'absent'}else{'old'}) $(if($previousMoved){'old'}else{'absent'}) absent
        Assert-PublicationSources $Sources -PreviousTarget $Target -PreviousNow $(if($previousMoved){$Backup}else{''})
        $moveAttempted=$true;Move-PublicationDirectory $Stage $Target;$installed=$true
        Assert-PublicationInventory $Target $Expected
        if($Module-eq'HWIO'-and$previousMoved){Assert-PublicationInventory $Backup $old}
        Assert-PublicationSources $Sources -Stage $Stage -StageNow $Target -PreviousTarget $Target -PreviousNow $(if($previousMoved){$Backup}else{''})
    }catch{
        $primary=$_
        if(-not$moveAttempted){throw $primary}
        try{
            if($installed){
                Invoke-PublicationAuthority $ValidateAuthority RejectInstalled $Target $Rejected
                Assert-PublicationRoles $plan absent stage $(if($previousMoved){'old'}else{'absent'}) absent
                Assert-PublicationSources $Sources -Stage $Stage -StageNow $Target -PreviousTarget $Target -PreviousNow $(if($previousMoved){$Backup}else{''})
                Move-PublicationDirectory $Target $Rejected
            }else{
                Invoke-PublicationAuthority $ValidateAuthority RestorePrevious $Backup $Target
                Assert-PublicationRoles $plan stage $(if($previousMoved){'absent'}else{'old'}) $(if($previousMoved){'old'}else{'absent'}) absent
            }
            if($previousMoved){
                Invoke-PublicationAuthority $ValidateAuthority RestorePrevious $Backup $Target
                Assert-PublicationRoles $plan $(if($installed){'absent'}else{'stage'}) absent old $(if($installed){'stage'}else{'absent'})
                Assert-PublicationSources $Sources -Stage $Stage -StageNow $(if($installed){$Rejected}else{$Stage}) -PreviousTarget $Target -PreviousNow $Backup
                Move-PublicationDirectory $Backup $Target
                Assert-PublicationInventory $Target $old
            }
        }catch{$script:publicationRollbackUnresolved=$true;$script:publicationRecoveryFailure=$_;try{$primary.Exception.Data['PublicationRecoveryFailure']=$_.Exception.ToString()}catch{};Write-PublicationWarning ('Rollback unresolved; all work retained: '+$script:publicationRecoveryFailure.Exception.Message)}
        throw $primary
    }
}
function Finish-PublicationWork($Work,[bool]$Complete,[bool]$Uncertain,[bool]$RollbackUnresolved) {
    if($Work.Root -and $Work.Root -ine $script:WorkCleanupProjectRoot){
        . $script:publicationCleanupHelper -CleanupProjectRoot $Work.Root
    }
    $state=if($Complete-and-not$Uncertain-and-not$RollbackUnresolved){'Ready'}else{'Held'}
    $reason=if($state-eq'Ready'){'Exact final package/source identities verified; transfer and rollback consumers closed. Regenerable publication residue only.'}else{'Failed publication, uncertain transfer or unresolved rollback requires owner disposition; sources/residue preserved.'}
    if($Work.Lease){
        $failure=$null
        try{Set-WorkCleanupDisposition -Lease $Work.Lease -State $state -Reason $reason -Findings 'docs/operations/current-build-workflow.md'}
        catch{$failure=$_}
        try{Exit-WorkCleanup -Lease $Work.Lease}catch{if(-not$failure){$failure=$_}else{Write-PublicationWarning ('Lease close failed: '+$_.Exception.Message)}}
        if($failure){throw $failure}
    }
    # Borrowed work stays within the live caller's one request. Never mutate its
    # disposition; failure propagates and the enclosing builder records Held.
    Write-Output "Publication residue $state ($($Work.Work)); user-run cleanup only."
}
function Close-PublicationResources($Scope,[bool]$Complete,[bool]$Uncertain,[bool]$RollbackUnresolved,$Mutex,[bool]$Locked,$PrimaryFailure) {
    $secondary=$null
    foreach($work in @($Scope)){
        try{if($work){Finish-PublicationWork $work $Complete $Uncertain $RollbackUnresolved}}
        catch{if(-not $secondary){$secondary=$_};Write-PublicationWarning ('Publication cleanup registration failed; residue retained: '+$_.Exception.Message)}
    }
    try{if($Locked){$Mutex.ReleaseMutex()}}catch{if(-not$secondary){$secondary=$_};Write-PublicationWarning ('Publication mutex release failed: '+$_.Exception.Message)}
    try{if($Mutex){$Mutex.Dispose()}}catch{if(-not$secondary){$secondary=$_};Write-PublicationWarning ('Publication mutex disposal failed: '+$_.Exception.Message)}
    if($secondary-and-not$PrimaryFailure){throw $secondary}
}
function Track-PublicationSource([string]$Source) {
    $path=[IO.Path]::GetFullPath($Source);$f=Get-Item -LiteralPath $path -Force
    if($f.PSIsContainer-or$f.Attributes-band[IO.FileAttributes]::ReparsePoint){throw 'Publication input must be an ordinary explicit file.'}
    $record=[pscustomobject]@{Source=$path;Bytes=$f.Length;SHA256=(FileHash $path)}
    $script:publicationSources.Add($record)
    return $record
}
function Queue-HwioPublicationFile([string]$Source,[string]$Target) {
    $null=Track-PublicationSource $Source
    Add-HwioTransfer $Source $Target
}
function Test-HwioUniqueOperatorBackupName([string]$Relative) {
    if($Relative-ceq'catalogue.xml.pre-qh418-20261025.bak'){return $true}
    if($Relative-ceq'fds-modules.xml.pre-display-fix-20261022.bak'){return $true}
    if($Relative-ceq'catalogue.xml.before-upper-valve-1.24.15.bak'){return $true}
    if($Relative-ceq'actuator-mappings.cfg.pre-analogue-layout-20261019.bak'){return $true}
    if($Relative-in'catalogue.xml.pre-stepper-20261019.bak','assignments.cfg.pre-at-arm-nav-20261014.bak','catalogue.xml.pre-at-arm-nav-20261014.bak','fds-modules.xml.pre-at-arm-nav-20261014.bak'){return $true}
    if($Relative-notmatch'^(settings\.cfg|assignments\.cfg|catalogue\.xml|fds-modules\.xml|motor-settings\.cfg|motor-mappings\.cfg|input-overrides\.cfg)\.pre-at-hold-([0-9]{8})\.bak$'){return $false}
    $date=[DateTime]::MinValue
    return [DateTime]::TryParseExact($Matches[2],'yyyyMMdd',[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::None,[ref]$date)
}

function Assert-HwioPreviousPublicationPackage([string]$Target,[object[]]$Inventory) {
    if(-not(Test-Path -LiteralPath $Target)){return}
    $mutable=@(Get-HwioPreservedPublicationNames $false)
    $known=@{}
    $manifestPath=Join-Path $Target 'package-manifest.json'
    if(Test-Path -LiteralPath $manifestPath){
        $parse=@{InputObject=[IO.File]::ReadAllText($manifestPath)}
        if((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind')){$parse.DateKind='String'}
        $manifest=ConvertFrom-Json @parse
        if($manifest.Product-cne'HWIO'-or$manifest.Configuration-notin'Debug','Release'-or-not$manifest.Files.Count){throw 'Cannot qualify previous HWIO package manifest.'}
        foreach($entry in $manifest.Files){
            $relative=[string]$entry.Path
            $null=SyncChild $Target $relative
            $relative=$relative.Replace('\','/')
            if($known.ContainsKey($relative)-or$relative-ieq'package-manifest.json'-or(Test-HwioUniqueOperatorBackupName $relative)){
                throw 'Previous HWIO manifest has duplicate, metadata or operator-backup payload entries.'
            }
            $known[$relative]=$entry
        }
    }
    foreach($item in $Inventory){
        $relative=$item.Path.Replace('\','/')
        if($relative-in$mutable-or(Test-HwioUniqueOperatorBackupName $relative)-or$relative-ieq'package-manifest.json'){continue}
        if(-not$known.ContainsKey($relative)){throw "Unrecognized existing HWIO file; publication refused: $relative"}
        $entry=$known[$relative]
        if($entry.Bytes-ne$item.Bytes-or$entry.SHA256-ine$item.SHA256){throw "Previous HWIO immutable payload changed: $relative"}
    }
    foreach($relative in $known.Keys){
        if($relative-in$mutable){continue}
        if(@($Inventory|Where-Object {$_.Path.Replace('\','/')-ieq$relative}).Count-ne1){throw "Previous HWIO immutable payload missing: $relative"}
    }
    # Inventory traversal already rejected all reparse points. A directory
    # cannot masquerade as an operator file, and empty/unknown trees are retained.
    foreach($directory in Get-ChildItem -LiteralPath $Target -Directory -Recurse -Force){
        $relative=$directory.FullName.Substring($Target.Length+1).Replace('\','/')
        if($relative-in$mutable-or(Test-HwioUniqueOperatorBackupName $relative)-or
           -not@($known.Keys|Where-Object {$_.StartsWith($relative+'/',[StringComparison]::OrdinalIgnoreCase)}).Count){
            throw "Unrecognized existing HWIO directory; publication refused: $relative"
        }
    }
}
function Assert-HwioPreservedPublicationStage([object[]]$Inventory,[object[]]$Expected,[bool]$FreshDefaults) {
    $names=@(Get-HwioPreservedPublicationNames $FreshDefaults)
    foreach($item in $Inventory){
        $relative=$item.Path.Replace('\','/')
        if($relative-notin$names-and-not(Test-HwioUniqueOperatorBackupName $relative)){continue}
        $staged=@($Expected|Where-Object {$_.Path.Replace('\','/')-ieq$relative})
        if($staged.Count-ne1-or$staged[0].Bytes-ne$item.Bytes-or$staged[0].SHA256-ine$item.SHA256){
            throw "Required HWIO operator preimage missing or changed in staged package: $relative"
        }
    }
}

function Get-HwioPreservedPublicationNames([bool]$FreshDefaults) {
    # Actuator configuration and input suppression belong to the operator even
    # in a clean-default package; none has a factory seed or implicit reset.
    $names=@('motor-settings.cfg','motor-settings.cfg.bak','motor-mappings.cfg','motor-mappings.cfg.bak','input-overrides.cfg','input-overrides.cfg.bak','servo-settings.cfg','servo-settings.cfg.bak','stepper-settings.cfg','stepper-settings.cfg.bak','actuator-mappings.cfg','actuator-mappings.cfg.bak','card-assignments.xml','card-assignments.xml.bak','flightillusion.cfg','flightillusion.cfg.bak')
    if(-not $FreshDefaults) {
        $names+=@('settings.cfg','assignments.cfg','settings.cfg.bak','assignments.cfg.bak','catalogue.xml','catalogue.xml.bak','fds-modules.xml','fds-modules.xml.bak')
    }
    $names
}
function Add-HwioMutablePublicationFiles([string]$Target,[string]$Stage,[bool]$FreshDefaults) {
    $names=@(Get-HwioPreservedPublicationNames $FreshDefaults)
    if(Test-Path -LiteralPath $Target){
        $names+=@(Get-ChildItem -LiteralPath $Target -File -Force | Where-Object {Test-HwioUniqueOperatorBackupName $_.Name} | ForEach-Object {$_.Name})
    }
    foreach($name in $names) {
        $savedConfig=Join-Path $Target $name
        if(Test-Path -LiteralPath $savedConfig) {
            AssertGeneratedPath $savedConfig
            if(-not(Test-Path -LiteralPath $savedConfig -PathType Leaf)) { throw "Expected a mutable HWIO file: $savedConfig" }
            Queue-HwioPublicationFile $savedConfig (Join-Path $Stage $name)
        }
    }
}
function Complete-HwioPublicationTransfers([string]$Stage) {
    $evidence=Join-Path $publicationWork 'transfers'
    $queued=$script:hwioTransferQueue.Count
    $script:publicationTransferUncertain=$true
    Complete-HwioTransfers $Stage $evidence -NativeVerifiedStaging:$NativeVerifiedStaging
    $receipts=if(Test-Path -LiteralPath $evidence){@(Get-ChildItem -LiteralPath $evidence -Filter transfer.json -File -Recurse -ErrorAction Stop)}else{@()}
    if($queued-and-not$receipts.Count){throw 'HWIO transfer completion evidence is missing; retain work.'}
    foreach($file in $receipts){
        Assert-WorkCleanupOrdinaryPath $file.FullName
        $report=Get-Content -LiteralPath $file.FullName -Raw -ErrorAction Stop|ConvertFrom-Json -ErrorAction Stop
        # The module helper can qualify bytes while a shared launcher remains
        # alive. That is insufficient to release work for human deletion.
        if($report.Verified-ne$true-or@($report.Launchers|Where-Object {$_.Exited-ne$true}).Count){throw 'HWIO transfer completion is uncertain; retain source/staging and Held work.'}
    }
    $script:publicationTransferUncertain=$false
}

. (Join-Path $PSScriptRoot 'deployment\common.ps1')
if($Module -eq 'HWIO') {
    . (Join-Path $sourceRoot 'modules\psx-hwio\tools\publication-transfers.ps1')
    . (Join-Path $sourceRoot 'modules\psx-hwio\tools\package-payload.ps1')
    if($NativeVerifiedStaging){
        Write-Output 'Explicit verified native HWIO override selected. Cloud endpoints default to native copying.'
        . (Join-Path $PSScriptRoot 'deployment\native-verified-staging.ps1')
    }
}
else {
    if($Module -eq 'Reposition') {
        . (Join-Path $sourceRoot 'modules\psx-reposition-test\tools\package-payload.ps1')
    }
    if($NativeVerifiedStaging){
        Write-Output 'Explicit verified native override selected. Cloud endpoints default to native copying.'
        . (Join-Path $PSScriptRoot 'deployment\native-verified-staging.ps1')
    }
}
$script:publicationTransfers=[Collections.Generic.List[object]]::new()
$script:publicationTransferUncertain=$false
function Add-PublicationTransfer([string]$Source,[string]$Target) {
    $sourcePath=[IO.Path]::GetFullPath($Source)
    $targetPath=[IO.Path]::GetFullPath($Target)
    $item=Track-PublicationSource $sourcePath
    $script:publicationTransfers.Add([pscustomobject]@{Source=$sourcePath;Target=$targetPath;Bytes=$item.Bytes;SHA256=$item.SHA256})
}
function Complete-PublicationTransfers {
    if(-not $script:publicationTransfers.Count){return}
    $token=[Guid]::NewGuid().ToString('N')
    $evidence=Join-Path $publicationWork ('transfers\'+$token)
    $records=@();$renames=@()
    foreach($record in $script:publicationTransfers){
        AssertGeneratedPath $record.Target
        if([IO.Path]::GetFileName($record.Source) -cne [IO.Path]::GetFileName($record.Target)){
            # TeraCopy supplies staging; an exact native rename retains basename mapping.
            $incoming=Join-Path $publicationWork ('incoming\'+$token+'\'+$renames.Count+'\'+[IO.Path]::GetFileName($record.Source))
            $records+=[pscustomobject]@{Source=$record.Source;Target=$incoming;Bytes=$record.Bytes;SHA256=$record.SHA256}
            $renames+=[pscustomobject]@{Incoming=$incoming;Record=$record}
        }else{$records+=$record}
    }
    $script:publicationTransferUncertain=$true
    if($NativeVerifiedStaging){NativeVerifiedStage $records $evidence}
    else{SyncTransfer $records $evidence 'C:\Program Files\TeraCopy\TeraCopy.exe'}
    foreach($rename in $renames){
        if(Test-Path -LiteralPath $rename.Record.Target){throw 'Publication rename target exists'}
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($rename.Record.Target))|Out-Null
        [IO.File]::Move($rename.Incoming,$rename.Record.Target)
        SyncIdentity $rename.Record.Target $rename.Record.Bytes $rename.Record.SHA256
    }
    $script:publicationTransfers.Clear()
    $script:publicationTransferUncertain=$false
}
$cache = Get-Content -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt')
function CacheValue([string]$name) {
    $line = @($cache | Where-Object { $_ -match ('^'+[regex]::Escape($name)+':[^=]+=') })
    if($line.Count -ne 1) { throw "Missing/ambiguous CMake cache entry: $name" }
    return ($line[0] -split '=',2)[1]
}
$dumpbin = Join-Path (Split-Path -Parent (CacheValue 'CMAKE_LINKER')) 'dumpbin.exe'
$vsRoot = CacheValue 'CMAKE_GENERATOR_INSTANCE'
$toolVersion = Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $dumpbin))))
$redistRoot = Join-Path $vsRoot "VC\Redist\MSVC\$toolVersion"
function AssertGeneratedPath([string]$path) {
    $absolute = [IO.Path]::GetFullPath($path)
    if(-not $absolute.StartsWith($binRoot+'\',[StringComparison]::OrdinalIgnoreCase)-and-not$absolute.StartsWith($publicationWork+'\',[StringComparison]::OrdinalIgnoreCase)) { throw "Outside publication trees: $absolute" }
    Assert-WorkCleanupOrdinaryPath $absolute
}
function Dependencies([string]$path) {
    $output = & $dumpbin /dependents $path 2>&1
    if($LASTEXITCODE -ne 0) { throw "Cannot inspect dependencies: $path" }
    return @($output | ForEach-Object { if($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $Matches[1] } } | Sort-Object -Unique)
}
function FileHash([string]$path) {
    # Use the BCL directly: MSBuild may launch Windows PowerShell with an
    # inherited PowerShell 7 module path where Get-FileHash cannot autoload.
    $stream = [IO.File]::OpenRead($path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function RuntimeSource([string]$name) {
    if($name -ieq 'ucrtbased.dll') {
        $project = Join-Path $buildRoot $(if($Module -eq 'HWIO') {'psx-hwio.vcxproj'} else {'psx-xp-coordinator.vcxproj'})
        if(Test-Path -LiteralPath $project) {
            [xml]$xml = [IO.File]::ReadAllText($project)
            $node = $xml.SelectSingleNode('//*[local-name()="WindowsTargetPlatformVersion"]')
            $kits = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
            if($node -and $kits) {
                $sdkRuntime = Join-Path $kits "bin\$($node.InnerText)\x64\ucrt\ucrtbased.dll"
                if(Test-Path -LiteralPath $sdkRuntime) { return $sdkRuntime }
            }
        }
        $path = Join-Path $env:WINDIR 'System32\ucrtbased.dll'
        if(Test-Path -LiteralPath $path) { return $path }
        throw 'x64 debug UCRT missing; install Windows SDK debug runtimes.'
    }
    # A Debug product may load a qualified Release vendor DLL. Resolve the
    # runtime actually imported by that PE, not the product configuration.
    # MSVC helper DLLs place the Debug marker before a suffix, e.g. msvcp140d_atomic_wait.dll.
    $debugImport = $name -match '(?:d\.dll$|^(?:msvcp|vcruntime|concrt)\d+d_[a-z0-9_]+\.dll$)'
    if($debugImport -and $Configuration -ne 'Debug') { throw "Debug runtime $name is not permitted in a Release package" }
    $crtRoot = Join-Path $redistRoot $(if($debugImport) {'debug_nonredist\x64'} else {'x64'})
    $suffix = if($debugImport) {'DebugCRT'} else {'CRT'}
    $dirs = @(Get-ChildItem -LiteralPath $crtRoot -Directory | Where-Object { $_.Name -match "^Microsoft\.VC\d+\.$suffix$" })
    foreach($dir in $dirs) {
        $path = Join-Path $dir.FullName $name
        if(Test-Path -LiteralPath $path) { return $path }
    }
    throw "Required runtime $name not found in matching toolset $crtRoot"
}
$configRoot = Join-Path $binRoot $Configuration
$product = if($Module -eq 'Plugin') {'X-Plane'} elseif($Module -eq 'Exporter') {'psx-exporter'} elseif($Module -eq 'HWIO') {'psx-hwio'} elseif($Module -eq 'Reposition') {'psx-reposition-test'} else {'psx-xp-coordinator'}
$target = Join-Path $configRoot $product
$stage = Join-Path $publicationWork 'stage'
$backup = Join-Path $publicationWork 'previous'
$rejected = Join-Path $publicationWork 'rejected'
AssertGeneratedPath $target
AssertGeneratedPath $stage
AssertGeneratedPath $backup
$mutex = [Threading.Mutex]::new($false,"Local\PSXIOS.Publish.$Module.$Configuration")
$locked = $false
try {
    try { $locked = $mutex.WaitOne(20000) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if(-not $locked) { throw 'Another build is publishing this product/configuration.' }
    $publicationScope=Open-PublicationWork $publicationWork $ParentCleanupOwner
    $publicationOriginalTarget=Get-PublicationNamespaceState $target
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    $origins = @{}
    $entrypoints = @()
    $dynamicDependencies = @()
    if($Module -eq 'Plugin') {
        $source = Join-Path $buildRoot "package\$Configuration\psx_xplane"
        $destination = Join-Path $stage 'Resources\plugins\psx_xplane'
        if(-not (Test-Path -LiteralPath (Join-Path $source 'win_x64\psx_xplane.xpl'))) { throw 'Plugin build missing.' }
        foreach($relative in 'win_x64\psx_xplane.xpl','win_x64\psx_xplane.pdb') {
            $file = Join-Path $source $relative
            if(-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Plugin runtime missing: $file" }
            $out = Join-Path $destination $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
            Add-PublicationTransfer $file $out
            $origins[$out] = $file
        }
        $entrypoints = @(Join-Path $destination 'win_x64\psx_xplane.xpl')
        $sdkLicense = Join-Path (CacheValue 'XPLANE_SDK_ROOT') 'license.txt'
        if(-not (Test-Path -LiteralPath $sdkLicense -PathType Leaf)) { throw "Qualified X-Plane SDK notice missing: $sdkLicense" }
        $sdkLicenseOut = Join-Path $destination 'licenses\xplane-sdk.txt'
        Add-PublicationTransfer $sdkLicense $sdkLicenseOut
        $origins[$sdkLicenseOut] = $sdkLicense
        foreach($relative in 'scripts\install-sparky-audio.ps1','assets\sparky-audio-manifest.json') {
            $file = Join-Path $sourceRoot "modules\xplane-plugin\$relative"
            $out = Join-Path $stage "support\Sparky\audio\$relative"
            New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
            Add-PublicationTransfer $file $out
            $origins[$out] = $file
        }
        # Generate in the build graph first, then use the normal verified transfer.
        # This keeps the binary package itself a complete aircraft-file overlay.
        $sndSource=Join-Path $publicationWork 'generated\fmod\747-400-PSX.snd'
        SyncWriteSoundAttachment (Join-Path $sourceRoot 'modules\xplane-plugin\assets\sparky-audio-manifest.json') $sndSource
        $sndOut=Join-Path $stage 'Aircraft\747-400\fmod\747-400-PSX.snd'
        Add-PublicationTransfer $sndSource $sndOut
        $origins[$sndOut]=$sndSource
        foreach($relative in 'scripts\install-aircraft-state.ps1') {
            $file = Join-Path $sourceRoot "modules\xplane-plugin\$relative"
            $out = Join-Path $stage "support\Sparky\state\$relative"
            New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
            Add-PublicationTransfer $file $out
            $origins[$out] = $file
        }
        # The aircraft installer is portable; ship the same qualified routing
        # helper beside it rather than depending on a project checkout.
        $portableCommon=Join-Path $stage 'support\Sparky\state\scripts\common.ps1'
        $portableCommonSource=Join-Path $PSScriptRoot 'deployment\common.ps1'
        Add-PublicationTransfer $portableCommonSource $portableCommon
        $origins[$portableCommon]=$portableCommonSource
        $adapterRoot = Join-Path $buildRoot 'adapter'
        if(-not (Test-Path -LiteralPath (Join-Path $adapterRoot 'adapter-manifest.json') -PathType Leaf)) { throw 'Qualified aircraft-state adapter missing' }
        # The owner stages the unchanged aircraft licence alongside the overlay;
        # it is a legal sidecar, outside the qualified aircraft geometry manifest.
        if(-not (Test-Path -LiteralPath (Join-Path $adapterRoot 'LICENSE') -PathType Leaf)) { throw 'Qualified Sparky aircraft licence missing' }
        $null=SyncVerifyAdapter $adapterRoot
        foreach($file in Get-ChildItem -LiteralPath $adapterRoot -File -Recurse) {
            $relative = $file.FullName.Substring($adapterRoot.Length).TrimStart('\','/')
            $out = Join-Path $stage "Aircraft\747-400\$relative"
            New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
            Add-PublicationTransfer $file.FullName $out
            $origins[$out] = $file.FullName
        }
    } elseif($Module -eq 'Exporter') {
        foreach($name in 'psx-exporter-start.exe','psx-exporter-native.dll') {
            foreach($item in $name,[IO.Path]::ChangeExtension($name,'.pdb')) {
                $file = Join-Path $buildRoot "$Configuration\$item"
                if(-not (Test-Path -LiteralPath $file)) { throw "Exporter build missing: $file" }
                $out = Join-Path $stage $item
                Add-PublicationTransfer $file $out
                $origins[$out] = $file
            }
            $entrypoints += Join-Path $stage $name
        }
        $jar = Join-Path $buildRoot 'java\psx-exporter-agent.jar'
        if(-not (Test-Path -LiteralPath $jar)) { throw "Exporter agent missing: $jar" }
        $out = Join-Path $stage 'psx-exporter-agent.jar'
        Add-PublicationTransfer $jar $out
        $origins[$out] = $jar
    } elseif($Module -eq 'HWIO') {
        $hwioSource = Join-Path $sourceRoot 'modules\psx-hwio'
        foreach($payload in @(Get-HwioRuntimePayload -ModuleRoot $hwioSource -BuildRoot $buildRoot -Configuration $Configuration)) {
            $out = Join-Path $stage $payload.Relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
            Queue-HwioPublicationFile $payload.Source $out
            $origins[$out] = $payload.Source
        }
        $catalogueDefault = Join-Path $stage 'defaults\catalogue.xml'
        $moduleDefault = Join-Path $stage 'defaults\fds-modules.xml'
        $catalogueSource = Join-Path $hwioSource 'config\catalogue.xml'
        $moduleSource = Join-Path $hwioSource 'config\fds-modules.xml'
        $entrypoints = @((Join-Path $stage 'psx-hwio.exe'),(Join-Path $stage 'hwio-fds-replay.exe'))
    } elseif($Module -eq 'Reposition') {
        foreach($payload in @(Get-RepositionRuntimePayload -ModuleRoot (Join-Path $sourceRoot 'modules\psx-reposition-test') -BuildRoot $buildRoot -Configuration $Configuration)) {
            $out = Join-Path $stage $payload.Relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
            Add-PublicationTransfer $payload.Source $out
            $origins[$out] = $payload.Source
        }
        $entrypoints = @(Join-Path $stage 'psx-reposition-test.exe')
    } else {
        $source = Join-Path $buildRoot $Configuration
        foreach($name in 'psx-xp-coordinator','psx-observation-replay','psx-diagnostics-setup') {
            $exe = Join-Path $source ($name+'.exe')
            if(-not (Test-Path -LiteralPath $exe)) { throw "Build missing: $exe" }
            # All three coordinator products are native. Do not carry a stale
            # CLR configuration file left by an older Forms build.
            foreach($suffix in '.exe','.pdb') {
                $file = Join-Path $source ($name+$suffix)
                if(Test-Path -LiteralPath $file) {
                    $out = Join-Path $stage ($name+$suffix)
                    Add-PublicationTransfer $file $out
                    $origins[$out] = $file
                }
            }
            $entrypoints += Join-Path $stage ($name+'.exe')
        }
    }
    if($Module -in 'Coordinator','HWIO') {
        $graphicsPackage = Join-Path (CacheValue 'ASS_GRAPHICS_SDK_ROOT') $Configuration
        foreach($relative in 'bin\ass_graphics_lib.dll','symbols\ass_graphics_lib.pdb','licenses\NOTICE.txt','licenses\B612-OFL.txt') {
            $file = Join-Path $graphicsPackage $relative
            if(-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Graphics library package missing: $file" }
            $destination = if($relative.StartsWith('licenses\')) { Join-Path $stage ('licenses\ass_graphics\'+[IO.Path]::GetFileName($relative)) } else { Join-Path $stage ([IO.Path]::GetFileName($relative)) }
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            if($Module -eq 'HWIO') { Queue-HwioPublicationFile $file $destination } else { Add-PublicationTransfer $file $destination }
            $origins[$destination] = $file
        }
    }
    if($Module -notin 'HWIO','Reposition') {
        $readmeModule = if($Module -eq 'Plugin') {'xplane-plugin'} elseif($Module -eq 'Exporter') {'psx-exporter'} else {'psx-xp-coordinator'}
        $readmeSource = Join-Path $sourceRoot ("modules\$readmeModule\docs\package-README.txt")
        $readmeOut = Join-Path $stage 'README.txt'
        Add-PublicationTransfer $readmeSource $readmeOut
        $origins[$readmeOut] = $readmeSource
    }
    if($Module -in 'Plugin','Coordinator' -and (Test-Path -LiteralPath (Join-Path $sourceRoot 'shared\sync-network\CMakeLists.txt'))){
        # Norm.dll is loaded by exact adjacent path, so import discovery alone
        # cannot find it. Its own imports enter the normal closure below.
        $networkRoot=if($Module -eq 'Plugin'){Join-Path $stage 'Resources\plugins\psx_xplane\win_x64'}else{$stage}
        $normSource=Join-Path $buildRoot ("sync-network\norm-runtime\"+$Configuration+'\Norm.dll')
        $normSymbols=[IO.Path]::ChangeExtension($normSource,'.pdb')
        if(-not(Test-Path -LiteralPath $normSymbols -PathType Leaf)){throw 'Source-built NORM symbols are missing.'}
        $normDetails=[Diagnostics.FileVersionInfo]::GetVersionInfo($normSource)
        if($normDetails.ProductName-cne'NORM'-or$normDetails.FileVersion-cne'1.5.9.2'){throw 'Qualified NORM maintenance build is required.'}
        $normOut=Join-Path $networkRoot 'Norm.dll'
        Add-PublicationTransfer $normSource $normOut;$origins[$normOut]=$normSource;$dynamicDependencies+=$normOut
        $normSymbolsOut=Join-Path $networkRoot 'Norm.pdb';Add-PublicationTransfer $normSymbols $normSymbolsOut;$origins[$normSymbolsOut]=$normSymbols
        foreach($name in 'NORM-LICENSE.txt','PROTOLIB-LICENSE.txt','MSVC-REDIST-NOTICE.txt'){
            $file=Join-Path $sourceRoot ('shared\norm\vendor\norm\licenses\'+$name);$legalRoot=if($Module -eq 'Plugin'){Join-Path $stage 'Resources\plugins\psx_xplane'}else{$stage};$out=Join-Path $legalRoot ('licenses\norm\'+$name)
            Add-PublicationTransfer $file $out;$origins[$out]=$file
        }
    }
    if($Module -eq 'Plugin'){
        $file=Join-Path $sourceRoot 'external\lzma-sdk-26.05\LICENSE.txt'
        $out=Join-Path $stage 'Resources\plugins\psx_xplane\licenses\lzma-sdk.txt'
        Add-PublicationTransfer $file $out;$origins[$out]=$file
    }
    # Resolve transitive runtime imports; Windows and X-Plane host DLLs stay prerequisites.
    if($Module -eq 'HWIO') { Complete-HwioPublicationTransfers $stage }
    else { Complete-PublicationTransfers }
    if($Module -eq 'Plugin'){$null=SyncVerifyAdapter (Join-Path $stage 'Aircraft\747-400')}
    $pending = [Collections.Generic.Queue[string]]::new()
    $seen = @{}
    $imports = [Collections.Generic.List[object]]::new()
    foreach($entry in (@($entrypoints)+@($dynamicDependencies))) { $pending.Enqueue($entry) }
    while($pending.Count) {
        $binary = $pending.Dequeue()
        if($seen.ContainsKey($binary)) { continue }
        $seen[$binary] = $true
        # Discover the closure from immutable producer files before moving the
        # whole queued destination group. A queued DLL need not be staged yet.
        $sourceBinary = $origins[$binary]
        if(-not $sourceBinary) { throw "Producer origin missing: $binary" }
        $dependencies = @(Dependencies $sourceBinary)
        # A corrupt incremental image can retain exports but lose its import
        # descriptors. Never publish such a plugin as a dependency-free success.
        if($Module -eq 'Plugin' -and $binary.EndsWith('.xpl',[StringComparison]::OrdinalIgnoreCase) -and
           ('XPLM_64.dll' -notin $dependencies -or 'KERNEL32.dll' -notin $dependencies)) {
            throw "Plugin host imports missing; perform a full relink: $binary"
        }
        foreach($dll in $dependencies) {
            $directory = Split-Path -Parent $binary
            $local = Join-Path $directory $dll
            $origin = $null
            $kind = 'Windows prerequisite'
            if((Test-Path -LiteralPath $local) -or $origins.ContainsKey($local)) { $kind = 'App-local' }
            elseif($dll -match '^(msvcp|msvcr|vcruntime|concrt)\d.*\.dll$|^ucrtbased\.dll$') { $origin = RuntimeSource $dll; $kind = 'App-local runtime' }
            elseif($Module -eq 'Plugin' -and $dll -ieq 'XPLM_64.dll') { $kind = 'X-Plane host prerequisite' }
            elseif(Test-Path -LiteralPath (Join-Path (Split-Path -Parent $origins[$binary]) $dll)) { $origin = Join-Path (Split-Path -Parent $origins[$binary]) $dll; $kind = 'App-local' }
            elseif($dll -notmatch '^(api-ms-win-|ext-ms-)' -and -not (Test-Path -LiteralPath (Join-Path $env:WINDIR "System32\$dll"))) { throw "Unresolved dependency: $dll in $binary" }
            if($origin) {
                if($Module -eq 'HWIO') { Queue-HwioPublicationFile $origin $local }
                else { Add-PublicationTransfer $origin $local }
                $origins[$local] = $origin
            }
            if($kind -like 'App-local*') { $pending.Enqueue($local) }
            $imports.Add([ordered]@{Binary=$binary.Substring($stage.Length+1);Dll=$dll;Resolution=$kind})
        }
    }
    if($Module -eq 'HWIO') { Complete-HwioPublicationTransfers $stage }
    else { Complete-PublicationTransfers }
    foreach($entry in $entrypoints) {
        $pdb = [IO.Path]::ChangeExtension($entry,'.pdb')
        if(-not (Test-Path -LiteralPath $pdb)) { throw "Debug symbols missing: $pdb" }
        if($Module -ne 'HWIO') {
            $details=[Diagnostics.FileVersionInfo]::GetVersionInfo($entry)
            $version=CacheValue 'CMAKE_PROJECT_VERSION'
            if(($version -split '\.').Count -eq 3){$version+='.0'}
            if($details.ProductName -cne 'ASS PSX Addons' -or $details.FileVersion -cne $version -or $details.ProductVersion -cne $version){throw "Missing/mismatched project VERSIONINFO: $entry"}
        }
    }
    if($Module -in 'Coordinator','Reposition') {
        $graphicsDetails=[Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $stage 'ass_graphics_lib.dll'))
        if($graphicsDetails.ProductName -cne 'ASS Graphics Lib'){throw 'Graphics ProductName must be ASS Graphics Lib'}
    }
    AssertRuntimePackageLayout -Directory $stage -Product $product
    $files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | ForEach-Object {
        [ordered]@{Path=$_.FullName.Substring($stage.Length+1);Bytes=$_.Length;SHA256=(FileHash $_.FullName);Source=$origins[$_.FullName]}
    })
    $manifest = [ordered]@{Product=$Module;Configuration=$Configuration;PublishedUtc=[DateTime]::UtcNow.ToString('o');BuildDirectory=$buildRoot;Toolset=$toolVersion;Tests='Build publication is not a test-pass certificate; see build/test reports.';RuntimeLayout=$(if($Module-in'Plugin','Coordinator','Watchdog'-and(Test-Path -LiteralPath (Join-Path $sourceRoot 'shared/windows-runtime/include/psx/windows/runtime_dependency.hpp'))){'adjacent-or-parent-v1'}else{'app-local-v1'});Files=$files;Dependencies=@($imports.ToArray())}
    [IO.File]::WriteAllText((Join-Path $stage 'package-manifest.json'),($manifest | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
    # Ordinary replacement retains settings. A requested clean delivery seeds
    # current defaults instead, without importing an earlier operator profile.
    if($Module -eq 'HWIO') {
        Add-HwioMutablePublicationFiles $target $stage ([bool]$FreshDefaults)
        Complete-HwioPublicationTransfers $stage
        # Seed only during publication, after the immutable manifest is built.
        # An existing file, including an invalid edited XML, belongs to the user.
        $activeCatalogue = Join-Path $stage 'catalogue.xml'
        if(-not (Test-Path -LiteralPath $activeCatalogue)) {
            Queue-HwioPublicationFile $catalogueDefault $activeCatalogue
        }
        $activeModules = Join-Path $stage 'fds-modules.xml'
        if(-not (Test-Path -LiteralPath $activeModules)) { Queue-HwioPublicationFile $moduleDefault $activeModules }
        Complete-HwioPublicationTransfers $stage
    }
    $expectedPackage=@(Get-PublicationInventory $stage)
    if([IO.Path]::GetPathRoot($stage)-ine[IO.Path]::GetPathRoot($target)){
        if(-not $CommitWorkDirectory){$CommitWorkDirectory=Join-Path 'P:\development\psx_ios\bin\.work' ('publication-commit-'+[Guid]::NewGuid().ToString('N'))}
        $stable=Prepare-PublicationStableCommit $stage $target $expectedPackage $CommitWorkDirectory
        $publicationCommitScope=$stable.Scope;$stage=$stable.Stage;$backup=$stable.Backup;$rejected=$stable.Rejected
    }
    $authorityScope=if($publicationCommitScope){$publicationCommitScope}else{$publicationScope}
    $publicationExecutableNames=@($entrypoints|ForEach-Object{[IO.Path]::GetFileName($_)})+@($publicationOriginalTarget.Files|Where-Object{$_.Path-like'*.exe'}|ForEach-Object{[IO.Path]::GetFileName($_.Path)})
    $publicationAuthority=New-PublicationAuthority $authorityScope $mutex $locked $target $publicationOriginalTarget $publicationExecutableNames
    $publicationAuthorityCheck={param($phase,$from,$to) Assert-PublicationAuthority $publicationAuthority $phase $from $to}
    Commit-Publication $stage $target $backup $rejected $expectedPackage $script:publicationSources.ToArray() $publicationAuthorityCheck
    $publicationCommitted=$true
    Write-Output "Published $Module $Configuration ($($files.Count) files) -> $target"
    if(-not $CoordinatedPublication -and $Module -eq 'Exporter' -and $Configuration -eq 'Release') {
        & (Join-Path $PSScriptRoot 'install-exporter-runtime.ps1') -PackageDirectory $target
    }
    if(-not $CoordinatedPublication -and $Configuration -eq 'Release' -and $Module -in 'Plugin','Coordinator','Exporter') {
        # bin remains authoritative; deploy is a verified portable Release snapshot.
        & (Join-Path $PSScriptRoot 'build-deployment.ps1') -IfComplete
    }
    $publicationComplete=$true
} catch {
    $publicationFailure=$_
    throw
} finally {
    Close-PublicationResources @($publicationCommitScope,$publicationScope) $publicationComplete ($script:publicationTransferUncertain-or($Module-eq'HWIO'-and$script:hwioTransferUncertain)) $script:publicationRollbackUnresolved $mutex $locked $publicationFailure
}
