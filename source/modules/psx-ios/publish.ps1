param([Parameter(Mandatory=$true)][ValidateSet('Debug','Release')][string]$Configuration,
      [Parameter(Mandatory=$true)][string]$BuildDirectory,
      [string]$WorkDirectory='',
      [string]$NativeStagingReason='',
      [string]$CommitWorkDirectory='',
      [switch]$RefreshInternalDebug,
      [string]$AssGraphicsSdkRoot=$env:ASS_GRAPHICS_SDK_ROOT,
      [string]$GraphicsSourceRoot='',
      [string]$XplMonitorClientRoot='P:\development\xpl-monitor\sdk\client\1.0.0',
      [string]$WorkProjectRoot='')
$ErrorActionPreference='Stop'
if($Configuration -ne 'Release' -and -not($Configuration -eq 'Debug' -and $RefreshInternalDebug)){throw 'Project publication is Release-only; local Debug refresh requires -RefreshInternalDebug.'}
if($RefreshInternalDebug -and $Configuration -ne 'Debug'){throw 'Internal Debug refresh requires Debug configuration.'}
$sourceRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$repo=if((Split-Path -Leaf $sourceRoot) -eq 'source'){Split-Path -Parent $sourceRoot}else{$sourceRoot}
$destination=Join-Path $repo "bin/$Configuration/psx-ios"
if($RefreshInternalDebug -and $destination -ine 'P:\development\psx_ios\bin\Debug\psx-ios'){throw 'Internal Debug refresh is restricted to the canonical local Debug package.'}
$outerContext=$env:PSX_WORK_CLEANUP_PARENT
. (Join-Path $repo 'scripts/deployment/common.ps1')
. (Join-Path $PSScriptRoot 'publication.ps1') -CommonPublisher (Join-Path $repo 'scripts/publish-binaries.ps1')
$BuildDirectory=[IO.Path]::GetFullPath($BuildDirectory)
. (Join-Path $PSScriptRoot 'graphics-sdk.ps1')
$graphicsSdk=Get-IosBuildGraphicsSdk $BuildDirectory $AssGraphicsSdkRoot
. (Join-Path $PSScriptRoot 'monitor-sdk.ps1')
$monitorSdk=Get-IosMonitorSdk $XplMonitorClientRoot
$monitorCache=[regex]::Match([IO.File]::ReadAllText((Join-Path $BuildDirectory 'CMakeCache.txt')),'(?m)^XPL_MONITOR_CLIENT_ROOT:PATH=([^\r\n]+)')
if(-not $monitorCache.Success-or[IO.Path]::GetFullPath($monitorCache.Groups[1].Value).TrimEnd('\','/')-ine$monitorSdk.Root){throw 'Monitor SDK differs from native build input.'}
$graphicsSource=Resolve-IosGraphicsSdk $(if($GraphicsSourceRoot){$GraphicsSourceRoot}else{$graphicsSdk.Root})
$nativeSdkFiles=@(Get-ChildItem -LiteralPath $graphicsSdk.Root -File -Recurse -Force)
$sourceSdkFiles=@(Get-ChildItem -LiteralPath $graphicsSource.Root -File -Recurse -Force)
if($nativeSdkFiles.Count-ne$sourceSdkFiles.Count){throw 'Graphics source location successor file set differs.'}
$graphicsSuccessor=@()
foreach($native in $nativeSdkFiles){
 $relative=$native.FullName.Substring($graphicsSdk.Root.Length+1)
 $source=Join-Path $graphicsSource.Root $relative
 if(-not[IO.File]::Exists($source)-or$native.Length-ne(Get-Item -LiteralPath $source).Length-or(Get-FileHash -LiteralPath $native.FullName).Hash-ne(Get-FileHash -LiteralPath $source).Hash){throw 'Graphics source location successor differs from native input.'}
 $graphicsSuccessor+=@{relative=$relative;native=$native.FullName;source=$source;bytes=$native.Length;sha256=(Get-FileHash -LiteralPath $source).Hash}
}
if($repo-ieq'P:\development\psx_ios'-and[IO.Path]::GetPathRoot($graphicsSource.Root)-ine'P:\'){throw 'Canonical IOS publication requires the verified durable P graphics source successor.'}
$work=if($WorkDirectory){[IO.Path]::GetFullPath($WorkDirectory)}else{Join-Path $BuildDirectory ('publication-'+[Guid]::NewGuid().ToString('N'))}
$developmentRoot='O:\psx'
if($WorkProjectRoot){$developmentRoot=[IO.Path]::GetFullPath($WorkProjectRoot)}
$cleanupHelper=Join-Path $developmentRoot 'scripts/work-cleanup.ps1'
. $cleanupHelper -CleanupProjectRoot $developmentRoot
$work=Resolve-WorkCleanupPath $work
if([IO.Directory]::Exists($work)){throw 'Publication staging already exists; retain it and select a new path'}
$versionMatch=[regex]::Match([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'CMakeLists.txt')),'project\(psx_ios VERSION ([0-9]+\.[0-9]+\.[0-9]+) ')
if(-not $versionMatch.Success){throw 'IOS source version unavailable'}
$version=$versionMatch.Groups[1].Value
$stage=Join-Path $work 'payload'
$rollback=Join-Path $work 'rollback'
$commitWork=$null
$commitLease=$null
$mutable=@('settings.cfg','settings.cfg.bak','clients.cfg','clients.cfg.bak','customer.png','customer.png.bak','fmc-quicksave.psxfmc','fmc-quicksave.psxfmc.bak')
$teracopy='C:\Program Files\TeraCopy\TeraCopy.exe'
if($NativeStagingReason){Write-Output "Explicit native staging exception: $NativeStagingReason"}
$borrowedCleanupParent=$null
if($env:PSX_WORK_CLEANUP_PARENT){
    # A live enclosing build owns this staging. Invalid context never falls back
    # to an overlapping standalone lease or changes the parent's disposition.
    $borrowedCleanupParent=Invoke-IosCatalogStep {Assert-WorkCleanupParent -WorkDirectory $work -Owner 'ios-6.1' -AllowSameProcess}
}
$cleanupLease=$null
$publicationFailure=$null
$originalTarget=$null
$transactionScope=$null
$transactionGuard=$null
$mutex=[Threading.Mutex]::new($false,"Local\PSXIOS.Publish.IOS.$Configuration")
$locked=$false
$before=@{}
$transfers=[Collections.Generic.List[object]]::new()
$applied=[Collections.Generic.List[object]]::new()
$immutable=[Collections.Generic.List[string]]::new()
$complete=$false
$obsolete=[Collections.Generic.List[object]]::new()
$predecessorManifest=$null
$relocatedPredecessorCount=0
$returnedPredecessor=$null
function Identity([string]$p){$i=Get-Item -LiteralPath $p;[ordered]@{path=$i.FullName;bytes=$i.Length;sha256=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()}}
function EqualIdentity($a,$b){$a.bytes -eq $b.bytes -and $a.sha256 -eq $b.sha256}
function QualifyReturnedPredecessor($receipt,$manifestIdentity,$old){
    $canonical='P:\development\psx_ios'
    $local='D:\devel_assist\psx-ios-rebuild-20261008-170322Z\psx_ios'
    $expectedDestination=Join-Path $canonical "bin/$Configuration/psx-ios"
    $origin=Join-Path $local "bin/$Configuration/psx-ios"
    if($repo -ine $canonical -or $destination -ine $expectedDestination -or
       $receipt.CanonicalRoot -ine $canonical -or $receipt.LocalRoot -ine $local -or
       $receipt.State -cne 'Verified selective return complete; human cleanup pending' -or
       -not $receipt.AtomicCommit.OriginalPackageManifestsUnchanged -or $receipt.AtomicCommit.RollbackUnresolved){throw 'Unqualified canonical package return'}
    $package=@($receipt.Packages|Where-Object{$_.Path -ceq "bin/$Configuration/psx-ios"})
    if($package.Count -ne 1 -or $package[0].ManifestSHA256 -ine $manifestIdentity.sha256 -or
       $package[0].OriginalDSource -ine $origin -or $package[0].VerifiedCanonicalLocation -ine $destination -or
       -not $package[0].InputVectorsUnchanged -or $manifestIdentity.path -ine (Join-Path $destination 'manifest.json') -or
       $old.schema -cne 'psx-ios-package-v1' -or $old.version -cne '0.6.4' -or $old.configuration -cne $Configuration){throw 'Returned predecessor manifest is not the recorded original'}
    $entries=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase)
    $prefix=$origin+'\'
    foreach($entry in $old.payload){
        $absolute=[IO.Path]::GetFullPath($entry.path)
        if(-not $absolute.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Returned predecessor payload is outside its recorded package'}
        $relative=$absolute.Substring($prefix.Length)
        if(-not $relative -or $relative -match '[:*?]' -or $entries.ContainsKey($absolute)){throw 'Invalid or duplicate returned predecessor payload'}
        $retained=Join-Path $destination $relative
        if(-not (EqualIdentity $entry (Identity $retained))){throw 'Returned predecessor retained payload differs'}
        $entries.Add($absolute,[pscustomobject]@{relative=$relative;identity=$entry})
    }
    if($entries.Count -ne 15){throw 'Returned predecessor payload closure differs'}
    return [pscustomobject]@{entries=$entries;origin=$origin;manifest_sha256=$manifestIdentity.sha256;verified=$entries.Count}
}
function PreviousPayloadRelative($entry){
    $absolute=[IO.Path]::GetFullPath($entry.path)
    $prefix=$destination.TrimEnd('\')+'\'
    if($absolute.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){return $absolute.Substring($prefix.Length)}
    if($returnedPredecessor){
        if(-not $returnedPredecessor.entries.ContainsKey($absolute)){throw 'Payload is not in the qualified returned predecessor'}
        $qualified=$returnedPredecessor.entries[$absolute]
        if(-not (EqualIdentity $entry $qualified.identity) -or -not (EqualIdentity $entry (Identity (Join-Path $destination $qualified.relative)))){throw 'Returned predecessor changed after qualification'}
        ++$script:relocatedPredecessorCount
        return $qualified.relative
    }
    # The qualified temporary mirror retains original P: manifests as evidence.
    # Accept that one predecessor namespace only after checking its D: payload;
    # no original manifest, P: file or operator setting is rewritten here.
    if(-not $repo.Equals('D:\devel_assist\psx-ios-rebuild-20261008-170322Z\psx_ios',[StringComparison]::OrdinalIgnoreCase)){throw 'Old package manifest names a foreign path'}
    $originalPrefix="P:\development\psx_ios\bin\$Configuration\psx-ios\"
    if(-not $absolute.StartsWith($originalPrefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Old package manifest names a foreign path'}
    $relative=$absolute.Substring($originalPrefix.Length)
    $retained=Join-Path $destination $relative
    if(-not [IO.File]::Exists($retained) -or -not (EqualIdentity $entry (Identity $retained))){throw "Relocated predecessor payload differs or is missing: $relative"}
    ++$script:relocatedPredecessorCount
    return $relative
}
function WriteText([string]$p,[string]$text){[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($p))|Out-Null;[IO.File]::WriteAllText($p,$text,[Text.UTF8Encoding]::new($false))}
# Only the documented project roots are identified non-cloud endpoints.
# Fixed drive letters alone do not establish that a directory is non-cloud.
function GetIosCopyEndpoint([string]$Path){
    $absolute=[IO.Path]::GetFullPath($Path)
    if([IO.Path]::GetPathRoot($absolute) -ieq 'P:\'){return 'Cloud'}
    foreach($root in @('O:\psx','D:\devel_assist\psx-ios-rebuild-20261008-170322Z\psx_ios')){
        if($absolute -ieq $root -or $absolute.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)){
            $ancestor=$absolute
            while($ancestor){
                if(([IO.File]::Exists($ancestor) -or [IO.Directory]::Exists($ancestor)) -and ([IO.File]::GetAttributes($ancestor) -band [IO.FileAttributes]::ReparsePoint)){throw "Copy endpoint traverses an unclassified reparse point: $ancestor"}
                $ancestor=[IO.Path]::GetDirectoryName($ancestor)
            }
            return 'Local'
        }
    }
    return 'Unclassified'
}
function GetIosCopyRoute([string]$Source,[string]$Target){
    $from=GetIosCopyEndpoint $Source;$to=GetIosCopyEndpoint $Target
    if($from -eq 'Cloud' -or $to -eq 'Cloud'){return 'NativeFileStream'}
    if($from -eq 'Local' -and $to -eq 'Local'){return 'TeraCopy'}
    throw "Copy endpoints must be identified before local TeraCopy use: $Source -> $Target"
}
function CopyIosNativeVerified($Source,[string]$Target){
    if(-not (EqualIdentity $Source (Identity $Source.path))){throw 'Native copy source changed before transmission'}
    if([IO.File]::Exists($Target)){
        if(-not (EqualIdentity $Source (Identity $Target))){throw 'Preserve-existing native destination differs; it was not overwritten'}
        if(-not (EqualIdentity $Source (Identity $Source.path))){throw 'Native copy source changed during destination validation'}
        return $false
    }
    # The qualified native stream path preserves fresh-file collision intent.
    # Flush concerns the local filesystem/provider; remote cloud completion is separate.
    $inputStream=[IO.File]::OpenRead($Source.path)
    try{
        $outputStream=[IO.File]::Open($Target,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try{$inputStream.CopyTo($outputStream);$outputStream.Flush($true)}finally{$outputStream.Dispose()}
    }finally{$inputStream.Dispose()}
    if(-not (EqualIdentity $Source (Identity $Target)) -or -not (EqualIdentity $Source (Identity $Source.path))){throw 'Native copy destination/source identity differs'}
    return $true
}
function StageFiles([object[]]$entries,[string]$TargetStage=$stage,[switch]$PreparedCommit){
    $batch=Join-Path $work ('transfer-'+$transfers.Count)
    [IO.Directory]::CreateDirectory($batch)|Out-Null
    $sources=@($entries|ForEach-Object{Identity $_.source})
    $names=@($sources|ForEach-Object{[IO.Path]::GetFileName($_.path)})
    if(@($names|Select-Object -Unique).Count -ne $names.Count){throw 'Duplicate basenames in a transfer batch'}
    $list=$null
    $incoming=if($PreparedCommit){Join-Path $commitWork 'incoming'}else{Join-Path $batch 'incoming'}
    [IO.Directory]::CreateDirectory($incoming)|Out-Null
    $record=[ordered]@{sources=$sources;destination=$incoming;list=$list;collision='SkipAll';verified=0;skipped=0;errors=0;launcher_exit=$null;vendor_counts='not exposed by CLI; independent exact destination verification required'}
    $transfers.Add($record)
    $process=$null
    try{
        $nativeSources=@();$localSources=@()
        foreach($source in $sources){
            $route=if($NativeStagingReason){'NativeFileStream'}else{GetIosCopyRoute $source.path $incoming}
            if($route -eq 'NativeFileStream'){$nativeSources+=@($source)}else{$localSources+=@($source)}
        }
        $record.native_count=$nativeSources.Count;$record.tera_count=$localSources.Count
        $record.transport=if($nativeSources.Count -and $localSources.Count){'Mixed'}elseif($nativeSources.Count){'NativeFileStream'}else{'TeraCopy'}
        $record.native_api='System.IO.FileStream CopyTo/CreateNew/Flush(true)'
        $record.reason=if($NativeStagingReason){$NativeStagingReason}else{'Default native for P/cloud endpoints; TeraCopy only for identified project-local pairs'}
        foreach($source in $nativeSources){
            $target=Join-Path $incoming ([IO.Path]::GetFileName($source.path))
            if(-not (CopyIosNativeVerified $source $target)){++$record.skipped}
        }
        if($localSources.Count){
            if(-not [IO.File]::Exists($teracopy)){throw 'Identified local transfer requires installed TeraCopy; no automatic native fallback.'}
            if(@(Get-Process -Name TeraCopy -ErrorAction SilentlyContinue).Count){throw 'TeraCopy is already active; no overlapping transfer was started.'}
            $list=Join-Path $batch 'sources.txt';$record.list=$list
            [IO.File]::WriteAllLines($list,[string[]]@($localSources|ForEach-Object{$_.path}),[Text.UTF8Encoding]::new($true))
            $process=Start-Process -FilePath $teracopy -ArgumentList ('Copy "*'+$list+'" "'+$incoming+'" /SkipAll /Close') -WindowStyle Hidden -PassThru
            $null=$process.Handle
            $record.pid=$process.Id
            $record.started_utc_ticks=$process.StartTime.ToUniversalTime().Ticks.ToString()
        }
        $deadline=[DateTime]::UtcNow.AddSeconds(30)
        do {
            $valid=$true
            for($k=0;$k -lt $sources.Count;++$k){
                $target=Join-Path $incoming $names[$k]
                try{if(-not [IO.File]::Exists($target) -or -not (EqualIdentity $sources[$k] (Identity $target))){$valid=$false;break}}
                catch{$valid=$false;break}
            }
            $retired=$null -eq $process
            if($process -and $process.WaitForExit(0)){
                $record.launcher_exit=$process.ExitCode
                if($record.launcher_exit -ne 0){throw "TeraCopy exited $($record.launcher_exit); staged files are not admitted."}
                $retired=@(Get-Process -Name TeraCopy -ErrorAction SilentlyContinue).Count -eq 0
            }
            if($valid -and $retired){break}
            Start-Sleep -Milliseconds 100
        } while([DateTime]::UtcNow -lt $deadline)
        if(-not $valid -or -not $retired){throw "Staging identity/transfer retirement not verified within 30 seconds. Sources/staging retained at $work; no retry or source deletion."}
    if(@(Get-ChildItem -LiteralPath $incoming -File).Count -ne $sources.Count){throw 'Unexpected files in the unique transfer destination'}
    # Native same-volume atomic renames establish package names after verified staging.
    # These commit/rollback operations deliberately retain native atomic semantics.
    for($k=0;$k -lt $sources.Count;++$k){
        if(-not (EqualIdentity $sources[$k] (Identity $sources[$k].path))){throw 'Producer source changed during transfer'}
        $target=Join-Path $TargetStage $entries[$k].relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))|Out-Null
        [IO.File]::Move((Join-Path $incoming $names[$k]),$target)
        if(-not $PreparedCommit-and$entries[$k].relative-notin$mutable){$immutable.Add($entries[$k].relative)}
        ++$record.verified
    }
    }catch{$record.errors=1;throw}
    finally{if($process){$process.Dispose();$record.handle_disposed=$true}}
}

try{
    if($null -eq $borrowedCleanupParent){$cleanupLease=Enter-IosWorkCleanup -WorkDirectory $work -Owner 'ios-6.1';$outerContext=Get-WorkCleanupParentContext -Lease $cleanupLease}
    try{$locked=$mutex.WaitOne(20000)}catch [Threading.AbandonedMutexException]{$locked=$true}
    if(-not $locked){throw 'Another IOS publication is in progress'}
    $originalTarget=Get-PublicationNamespaceState $destination
    Assert-PublicationProductIdle @($destination) @('psx-ios.exe')
    $old=$null
    $oldManifest=Join-Path $destination 'manifest.json'
    if([IO.File]::Exists($oldManifest)){
        $predecessorManifest=Identity $oldManifest
        $old=Get-Content -LiteralPath $oldManifest -Raw|ConvertFrom-Json
        if($repo -ieq 'P:\development\psx_ios' -and @($old.payload|Where-Object{-not ([IO.Path]::GetFullPath($_.path)).StartsWith($destination+'\',[StringComparison]::OrdinalIgnoreCase)}).Count){
            $returnReceiptPath=Join-Path $repo 'docs/validation/clean-rebuild-20261008.json'
            $returnReceipt=Get-Content -LiteralPath $returnReceiptPath -Raw|ConvertFrom-Json
            $returnedPredecessor=QualifyReturnedPredecessor $returnReceipt.PersistentStorageReturn20261009 $predecessorManifest $old
            $returnReceiptIdentity=Identity $returnReceiptPath
        }
    }
    Assert-IosExistingPackage $originalTarget $destination $old $mutable $Configuration
    # Symbols are admitted only for the exact graphics DLL used by this build.
    # Keep the complete matching native SDK edition, including its own symbols.
    $producerDll=Identity (Join-Path $BuildDirectory "$Configuration/ass_graphics_lib.dll")
    $vendorDll=Identity (Join-Path $graphicsSdk.Root "$Configuration/bin/ass_graphics_lib.dll")
    if(-not (EqualIdentity $producerDll $vendorDll)){throw 'Graphics producer DLL differs from the pinned vendor DLL'}
    $symbolPath=Join-Path $graphicsSource.Root "$Configuration/symbols/ass_graphics_lib.pdb"
    $symbolDll=Identity (Join-Path $graphicsSource.Root "$Configuration/bin/ass_graphics_lib.dll")
    $symbolSource='selected-build-matched-ass-sdk'
    if(-not [IO.File]::Exists($symbolPath)){throw 'Matching selected ASS symbols unavailable'}
    if(-not (EqualIdentity $producerDll $symbolDll)){throw 'Graphics symbol-authority DLL differs; no symbols were admitted'}
    $assSymbols=[ordered]@{source=$symbolSource;dll=$symbolDll;pdb=(Identity $symbolPath);native_sdk_root=$graphicsSdk.Root;source_sdk_root=$graphicsSource.Root;location_successor=$graphicsSuccessor}
    foreach($name in $mutable){$p=Join-Path $destination $name;if([IO.File]::Exists($p)){$before[$name]=Identity $p}}
    [IO.Directory]::CreateDirectory($stage)|Out-Null
    StageFiles @('psx-ios.exe','psx-ios.pdb','environment-time.bin'|ForEach-Object{@{source=Join-Path $BuildDirectory "$Configuration/$_";relative=$_}})
    # The configuration parent owns one graphics pair shared by all consumers.
    $sharedFiles=@()
    foreach($binding in @(@{name='ass_graphics_lib.dll';identity=$producerDll},@{name='ass_graphics_lib.pdb';identity=(Identity $symbolPath)})){
        $installed=Join-Path (Split-Path -Parent $destination) $binding.name
        if(-not [IO.File]::Exists($installed)-or-not (EqualIdentity $binding.identity (Identity $installed))){throw "Install the matching shared graphics dependency before publication: $installed"}
        $sharedFiles+=Identity $installed
    }
    $sharedRuntime=[ordered]@{directory=(Split-Path -Parent $destination);configuration=$Configuration;version='1.4.5 ABI1';files=$sharedFiles}
    StageFiles @(@{source=Join-Path $PSScriptRoot 'vendor/civetweb/LICENSE.md';relative='licenses/CivetWeb-LICENSE.md'},
                 @{source=Join-Path $graphicsSource.Root "$Configuration/licenses/B612-OFL.txt";relative='licenses/B612-OFL.txt'},
                 @{source=Join-Path $graphicsSource.Root "$Configuration/licenses/NOTICE.txt";relative='licenses/NOTICE.txt'})
    StageFiles @('IANA-LICENSE','IANA-LICENSE_APACHE','NOTICE.md','ODbL.html','TBB-CODE-LICENSE.txt','TBB-DATA-LICENSE.txt' | ForEach-Object{@{source=Join-Path $PSScriptRoot ('vendor/time/'+$_);relative=('licenses/time/'+$_)}})
    $readme=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'README.md'))
    WriteText (Join-Path $stage 'README.md') $readme
    $immutable.Add('README.md')
    # Complete package staging keeps operator files opaque and byte-identical.
    if($before.Count){StageFiles @($before.Keys|ForEach-Object{@{source=Join-Path $destination $_;relative=$_}})}
    if(-not $before.ContainsKey('settings.cfg')){WriteText (Join-Path $stage 'settings.cfg') "schema=2`npsx=127.0.0.1`nmain_port=10747`nweb_port=8090`nadapter_guid=`nconfirmation=1`ndatabase_folder=`n"}
    if(-not $before.ContainsKey('clients.cfg')){WriteText (Join-Path $stage 'clients.cfg') "schema=3`n"}
    foreach($transfer in $transfers){foreach($source in $transfer.sources){if(-not (EqualIdentity $source (Identity $source.path))){throw 'Producer source changed before publication'}}}
    $payload=@($immutable|Sort-Object|ForEach-Object{$identity=Identity (Join-Path $stage $_);$identity.relative=$_.Replace('\','/');$identity.path=Join-Path $destination $_;$identity})
    $sources=@(Get-ChildItem -LiteralPath $PSScriptRoot -File -Recurse|Where-Object{$_.FullName -notmatch '\\(build|evidence)\\|\\vendor\\ass(?:-[^\\]+)?\\'}|Sort-Object FullName|ForEach-Object{Identity $_.FullName})
    foreach($art in @('747-400-top-front.svg','744_sideview_facing_right.svg','738_sideview_facing_left.svg','738_sideview_facing_right.svg')){$sources+=Identity (Join-Path $sourceRoot ('external/'+$art))}
    $sources+=@(Get-ChildItem -LiteralPath (Join-Path $sourceRoot 'shared/psx-instructor') -File -Recurse|Sort-Object FullName|ForEach-Object{Identity $_.FullName})
    $sources+=@(Get-ChildItem -LiteralPath (Join-Path $sourceRoot 'shared/non-normal') -File -Recurse|Sort-Object FullName|ForEach-Object{Identity $_.FullName})
    foreach($dependency in @('shared/environment','shared/motion','shared/aircraft-state','shared/windows-version','shared/hwio/include','shared/situation','shared/scenery-coverage','shared/status','shared/sync-network','shared/bridge-protocol','shared/terrain','shared/aircraft-sync','shared/traffic-visual','shared/traffic-source','shared/audio','shared/concurrency','shared/diagnostics/include','shared/network-io','shared/windows-runtime')){$sources+=@(Get-ChildItem -LiteralPath (Join-Path $sourceRoot $dependency) -File -Recurse|Sort-Object FullName|ForEach-Object{Identity $_.FullName})}
    foreach($entry in @('shared/cdu/src/blackout.cpp','shared/cdu/include/psx/cdu/blackout.hpp','shared/cdu/include/psx/cdu/snapshot.hpp','shared/cdu/include/psx/cdu/model.hpp')){$sources+=Identity (Join-Path $sourceRoot $entry)}
    foreach($entry in @('WindowsIcons.cmake','application-icon.rc.in','application-icon.hpp','IOS.ico')){$sources+=Identity (Join-Path $sourceRoot ('shared/windows-icons/'+$entry))}
    $sources+=@($monitorSdk.Files)
    $sources+=Identity $cleanupHelper
    $sources+=Identity (Join-Path $repo 'scripts/publish-binaries.ps1')
    $sources+=Identity (Join-Path $repo 'scripts/deployment/common.ps1')
    foreach($input in @($sourceSdkFiles|ForEach-Object{Identity $_.FullName})){
        if(-not (EqualIdentity $input (Identity $input.path))){throw 'Graphics symbol authority changed during publication'}
        if($input.path -notin $sources.path){$sources+=$input}
    }
    if([IO.File]::Exists($oldManifest)){
        if(-not (EqualIdentity $predecessorManifest (Identity $oldManifest))){throw 'Predecessor manifest changed during staging'}
        foreach($entry in $old.payload){
            $relative=PreviousPayloadRelative $entry
            if($relative -notin $immutable -and $relative -notin $mutable -and ($relative -match '^(docs|defaults)[\\/]')){
                $obsolete.Add([ordered]@{relative=$relative;identity=$entry})
            }
        }
    }
    $transport=[ordered]@{policy='P/cloud native default; identified project-local pairs TeraCopy';native_api='System.IO.FileStream';runtime=[Environment]::Version.ToString();explicit_native_reason=$NativeStagingReason;local_tool=$(if(@($transfers|Where-Object{$_.tera_count -gt 0}).Count){Identity $teracopy}else{$null})}
    $manifest=[ordered]@{schema='psx-ios-package-v1';version=$version;configuration=$Configuration;published_utc=[DateTime]::UtcNow.ToString('o');payload=$payload;sources=$sources;mutable_preserved=$before;retired_package_files=$obsolete;predecessor_manifest=$predecessorManifest;relocated_predecessor_payloads=$relocatedPredecessorCount;returned_predecessor=$(if($returnedPredecessor){@{origin=$returnedPredecessor.origin;manifest_sha256=$returnedPredecessor.manifest_sha256;verified=$returnedPredecessor.verified;receipt=$returnReceiptIdentity}}else{$null});ass_version=($graphicsSdk.Version+' ABI1');ass_symbols=$assSymbols;civetweb='Moog source snapshot; header 1.17; local Transfer-Encoding/Content-Length patch retained';scope='IOS only; shared public reposition sender with destination-terrain preflight; no service installation';transfer_tool=$transport;transfers=$transfers}
    $manifest['shared_runtime']=$sharedRuntime
    $manifest['monitor_sdk']=@{version=$monitorSdk.Version;api=1;role='collector';root=$monitorSdk.Root;manifest_sha256=$monitorSdk.ManifestSHA256}
    $manifest['distribution']=if($Configuration -eq 'Debug'){'Internal Debug; not a release asset'}else{'Release'}
    WriteText (Join-Path $stage 'manifest.json') ($manifest|ConvertTo-Json -Depth 10)
    # ReplaceFile backup, replacement and destination must share a volume.
    # Stable prepared bytes alone cross to P; lists/results remain in O work.
    if([IO.Path]::GetPathRoot($destination) -ine [IO.Path]::GetPathRoot($stage)){
        if($repo -ine 'P:\development\psx_ios'){throw 'Cross-volume commit has no qualified finished-package root.'}
        $commitWork=if($CommitWorkDirectory){[IO.Path]::GetFullPath($CommitWorkDirectory)}else{Join-Path $repo ('bin/.work/ios-commit-'+[Guid]::NewGuid().ToString('N'))}
        . $cleanupHelper -CleanupProjectRoot $repo
        $commitWork=Resolve-WorkCleanupPath $commitWork
        if([IO.Directory]::Exists($commitWork) -or [IO.File]::Exists($commitWork)){throw 'Commit staging must be fresh; existing bytes are preserved.'}
        $commitLease=Enter-IosWorkCleanup -WorkDirectory $commitWork -Owner 'ios-6.1'
        . $cleanupHelper -CleanupProjectRoot $developmentRoot
        $preparedStage=Join-Path $commitWork 'payload'
        $prepared=@(Get-ChildItem -LiteralPath $stage -File -Recurse|Sort-Object FullName|ForEach-Object{@{source=$_.FullName;relative=$_.FullName.Substring($stage.Length+1)}})
        StageFiles $prepared -TargetStage $preparedStage -PreparedCommit
        $stage=$preparedStage
        $rollback=Join-Path $commitWork 'rollback'
    }
    if(-not $commitLease){throw 'Canonical directory publication requires its actual same-volume commit lease.'}
    if(-not $outerContext){throw 'IOS publication requires its actual O issuer context.'}
    $scope=[pscustomobject]@{Root=$repo;Work=$commitWork;Lease=$commitLease;Context=$null}
    . $cleanupHelper -CleanupProjectRoot $repo
    $authority=New-PublicationAuthority $scope $mutex $locked $destination $originalTarget @('psx-ios.exe')
    $transactionGuard=New-IosPackageGuard $authority $outerContext $work $cleanupHelper
    $previous=Join-Path $commitWork 'previous';$rejected=Join-Path $commitWork 'rejected'
    $expected=@(Get-PublicationInventory $stage)
    $inputPins=@($sharedFiles|ForEach-Object{@{Source=$_.path;Bytes=$_.bytes;SHA256=$_.sha256}})
    $inputPins+=@($sources|ForEach-Object{@{Source=$_.path;Bytes=$_.bytes;SHA256=$_.sha256}})
    $inputPins+=@($transfers|ForEach-Object{$_.sources}|ForEach-Object{@{Source=$_.path;Bytes=$_.bytes;SHA256=$_.sha256}})
    $Module='IOS'
    Commit-Publication $stage $destination $previous $rejected $expected $inputPins $transactionGuard
    foreach($name in $before.Keys){if(-not (EqualIdentity $before[$name] (Identity (Join-Path $destination $name)))){throw "Mutable file changed during publication: $name"}}
    $complete=$true
    Write-Output "Published $Configuration psx-ios $version; $($payload.Count) immutable runtime/license files verified; configuration, backups and customer asset preserved."
}catch{$publicationFailure=$_;throw}
finally{
    $housekeepingFailure=$null
    try{
        if([IO.Directory]::Exists($work)){
            WriteText (Join-Path $work 'transfer-results.json') ($transfers|ConvertTo-Json -Depth 8)
            WriteText (Join-Path $work 'publication-result.json') ([ordered]@{complete=$complete;destination=$destination;mutable_before=$before;applied=$applied;stage_retained=[IO.Directory]::Exists($stage);commit_work=$commitWork;previous=$previous;rejected=$rejected;rollback_unresolved=$script:publicationRollbackUnresolved;recovery_error=$(if($script:publicationRecoveryFailure){$script:publicationRecoveryFailure.ToString()}else{$null})}|ConvertTo-Json -Depth 8)
        }
    }catch{$housekeepingFailure=$_;Write-Warning "IOS publication result write failed separately: $_"}
    # Each resource closes independently, including when report or queue writes fail.
    if($null -ne $commitLease){
        try{
            . $cleanupHelper -CleanupProjectRoot $repo
            $reason=if($complete){'Stable IOS commit/rollback bytes await final package identity review before Ready.'}else{'IOS owner must resolve retained cross-volume commit/rollback work; no retry.'}
            Invoke-IosCatalogStep {Set-WorkCleanupDisposition -Lease $commitLease -State Held -Reason $reason -Findings 'docs/validation/ios-failure-layout-20261008.json'}
        }catch{$housekeepingFailure=$_;Write-Warning "IOS stable commit registration failed separately: $_"}
        finally{try{. $cleanupHelper -CleanupProjectRoot $repo;Invoke-IosCatalogStep {Exit-WorkCleanup -Lease $commitLease}}catch{$housekeepingFailure=$_;Write-Warning "IOS stable commit lease release failed separately: $_"}}
    }
    try{if($locked){$mutex.ReleaseMutex()}}catch{$housekeepingFailure=$_;Write-Warning "IOS publication mutex release failed separately: $_"}
    try{$mutex.Dispose()}catch{$housekeepingFailure=$_;Write-Warning "IOS publication mutex disposal failed separately: $_"}
    if($null -ne $cleanupLease){
        try{
            . $cleanupHelper -CleanupProjectRoot $developmentRoot
            $reason=if($complete){'IOS owner must verify final package/source/local PDB closure and merge publication results before Ready.'}else{'IOS owner must resolve failed publication and retained rollback before Ready; no automatic retry.'}
            Invoke-IosCatalogStep {Set-WorkCleanupDisposition -Lease $cleanupLease -State Held -Reason $reason -Findings 'docs/validation/ios-failure-layout-20261008.json'}
        }catch{$housekeepingFailure=$_;Write-Warning "IOS publication work registration failed separately: $_"}
        finally{try{. $cleanupHelper -CleanupProjectRoot $developmentRoot;Invoke-IosCatalogStep {Exit-WorkCleanup -Lease $cleanupLease}}catch{$housekeepingFailure=$_;Write-Warning "IOS publication work lease release failed separately: $_"}}
    }
    if($housekeepingFailure -and -not $publicationFailure){throw $housekeepingFailure}
}
