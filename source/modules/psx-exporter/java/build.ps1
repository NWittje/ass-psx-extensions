[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$JavaHome = '',
    [string]$Version = '',
    [switch]$RunTests,
    [switch]$HookOnly,
    [switch]$NativeRecords,
    [string]$FmcBaselineAgent='',
    [string]$PreparationComparisonAgent='',
    [string]$PreparationLowerDirectory='',
    [string[]]$FmcSuites=@(),
    [string]$PsxJar,
    [string]$ArchivedPsxJar,
    [string]$ArchivedCoreClass,
    [string]$ArchivedAerodynamicsClass,
    [string]$Owner = 'exporter-java',
    [string]$CleanupHelperPath = 'P:\development\psx_ios\scripts\work-cleanup.ps1'
)
$ErrorActionPreference = 'Stop'
$versionDeclaration=[IO.File]::ReadAllText((Join-Path $PSScriptRoot '../CMakeLists.txt'))
if($versionDeclaration -notmatch 'project\(psx_exporter VERSION (\d+\.\d+\.\d+) LANGUAGES CXX\)'){throw 'Exporter CMake product version unavailable'}
$productVersion=$Matches[1]
if($Version -and $Version -cne $productVersion){throw 'Java version differs from authoritative exporter CMake product version'}
$Version=$productVersion
# The maintained product uses the preserved capture component and native record
# adapter. The unfinished historical FMC reconstruction is not a build default.
if(-not $PSBoundParameters.ContainsKey('HookOnly')) { $HookOnly = $true }
if(-not $FmcBaselineAgent) {
    $FmcBaselineAgent = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../external/exporter-fmc-capture-0.19.0.jar'))
}
if (-not $PSBoundParameters.ContainsKey('OutputDirectory')) {
    $OutputDirectory = Join-Path 'O:\psx\bin\.work' ('exporter-java-'+[Guid]::NewGuid().ToString('N'))
}
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
. $CleanupHelperPath -CleanupProjectRoot 'O:\psx'
$outputRoot = Resolve-WorkCleanupPath $outputRoot
$lease = $null
if($env:PSX_WORK_CLEANUP_PARENT) {
    $parent = $env:PSX_WORK_CLEANUP_PARENT | ConvertFrom-Json -ErrorAction Stop
    $borrowed = Assert-WorkCleanupParent -WorkDirectory $outputRoot -Owner $parent.Owner -AllowSameProcess
} else {
    $lease = Enter-WorkCleanup -WorkDirectory $outputRoot -Owner $Owner
}
$previousTemporary = @{TEMP=$env:TEMP;TMP=$env:TMP;TMPDIR=$env:TMPDIR}
$completed = $false
try {
[IO.Directory]::CreateDirectory($outputRoot)|Out-Null
$temporary = Join-Path $outputRoot 'temporary'
[IO.Directory]::CreateDirectory($temporary)|Out-Null
$env:TEMP=$env:TMP=$env:TMPDIR=$temporary
if(-not $JavaHome) {
    if($env:JAVA_HOME) { $JavaHome = $env:JAVA_HOME }
    else {
        $javacCommand = Get-Command javac.exe -CommandType Application -ErrorAction Stop
        # Query the selected JDK so a Windows PATH forwarding stub is supported.
        $probeInfo = New-Object Diagnostics.ProcessStartInfo
        $probeInfo.FileName = $javacCommand.Source
        $probeInfo.Arguments = '-J-XshowSettings:properties -version'
        $probeInfo.UseShellExecute = $false
        $probeInfo.CreateNoWindow = $true
        $probeInfo.RedirectStandardOutput = $true
        $probeInfo.RedirectStandardError = $true
        $probe = [Diagnostics.Process]::Start($probeInfo)
        try {
            $probeOutput = $probe.StandardOutput.ReadToEndAsync()
            $probeError = $probe.StandardError.ReadToEndAsync()
            if(-not $probe.WaitForExit(10000)) { $probe.Kill(); throw 'JDK discovery timed out; supply -JavaHome.' }
            $javaSettings = $probeOutput.Result + $probeError.Result
            $probeExitCode = $probe.ExitCode
        } finally { $probe.Dispose() }
        if($probeExitCode -ne 0 -or $javaSettings -notmatch '(?m)^\s*java\.home\s*=\s*(.+?)\s*$') {
            throw 'Cannot discover a JDK from javac.exe; supply -JavaHome or JAVA_HOME.'
        }
        $JavaHome = $Matches[1].Trim()
    }
}
$JavaHome = (Resolve-Path -LiteralPath $JavaHome).Path
foreach($tool in 'javac.exe','jar.exe','java.exe') {
    if(-not (Test-Path -LiteralPath (Join-Path $JavaHome "bin/$tool") -PathType Leaf)) {
        throw "JDK tool missing: $tool. Supply -JavaHome or JAVA_HOME."
    }
}
if($NativeRecords){
    $nativeTemporary=Join-Path $outputRoot 'native-temporary'
    [IO.Directory]::CreateDirectory($nativeTemporary)|Out-Null
}
$classes = Join-Path $outputRoot 'classes'
New-Item -ItemType Directory -Force -Path $classes | Out-Null
function Invoke-JavaSourceList {
    param([string]$Destination,[string[]]$Sources,[string]$ClassPath='')
    # Windows has a finite process command line. JDK argument files retain the
    # complete source roster in the owned graph without truncating admission.
    $compilerArguments=Join-Path $outputRoot 'javac-sources.txt'
    $arguments=@('--release','25','-d',$Destination)
    if($ClassPath){$arguments+=@('-cp',$ClassPath)}
    $arguments+=@($Sources)
    $quoted=@($arguments|ForEach-Object {'"'+$_.Replace('\','/').Replace('"','\"')+'"'})
    [IO.File]::WriteAllLines($compilerArguments,$quoted,[Text.UTF8Encoding]::new($false))
    & "$JavaHome/bin/javac.exe" ('@'+$compilerArguments)
    if($LASTEXITCODE -ne 0){throw 'Java source-list compilation failed'}
}
# IOS native relocation is independently owned and not qualified for exporter0.8.
# Preserve its work-in-progress source while excluding it from this publication.
$sources = @(Get-ChildItem -LiteralPath "$PSScriptRoot/src" -Filter '*.java' -Recurse | Where-Object { $_.Name -notlike 'Ios*.java' } | ForEach-Object FullName)
function ComponentHash([string]$Path){
    $stream=[IO.File]::OpenRead($Path);$hash=[Security.Cryptography.SHA256]::Create()
    try{[BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','')}finally{$hash.Dispose();$stream.Dispose()}
}
if($HookOnly){
    if($FmcSuites.Count-and(-not$NativeRecords-or@($FmcSuites|Where-Object{$_-notlike'FmcNativeRecords*Test'}).Count)){throw 'The hook successor cannot resume paused preservation-based FMC qualification.'}
    if(-not $FmcBaselineAgent-or(ComponentHash $FmcBaselineAgent)-cne'574AC881FA96A7325DFB6FA313289E89816C4754B9AC77440C09B20D700DD38A'){throw 'Qualified capture-only FMC0.19.0 component required.'}
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $baseline=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($FmcBaselineAgent));$baselineEntries=[Collections.Generic.List[object]]::new()
    try{foreach($entry in $baseline.Entries){
        if($entry.FullName-notmatch '^psx/exporter/(Fmc[A-Za-z0-9_$]+|PfdRecorder(?:\$[A-Za-z0-9_$]+)?|RecorderPrepare(?:\$[A-Za-z0-9_$]+)?)\.class$'){continue}
        $target=Join-Path $classes $entry.FullName;[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))|Out-Null
        $input=$entry.Open();$output=[IO.File]::Open($target,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try{$input.CopyTo($output);$output.Flush($true)}finally{$output.Dispose();$input.Dispose()}
        $baselineEntries.Add([ordered]@{Entry=$entry.FullName;Bytes=[IO.FileInfo]::new($target).Length;SHA256=(ComponentHash $target)})
    }}finally{$baseline.Dispose()}
    if($baselineEntries.Count-lt50){throw 'Capture-only component roster incomplete'}
    [IO.File]::WriteAllText((Join-Path $outputRoot 'fmc-capture-component.json'),([ordered]@{Schema=1;Source=$FmcBaselineAgent;JarSHA256='574AC881FA96A7325DFB6FA313289E89816C4754B9AC77440C09B20D700DD38A';Entries=$baselineEntries.ToArray();PausedSourceCompiled=$false;RecallEnabled=$false}|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
    if($NativeRecords){
        # This data-only preparation helper is the only new legacy-prefix
        # source. Prove its preserved preimage before accepting any overlay.
        $preparationGate=Join-Path $outputRoot ('preparation-preimage-gate-'+[Guid]::NewGuid().ToString('N'))
        $preparationGateTests=Join-Path $outputRoot 'preparation-gate-tests'
        Invoke-JavaSourceList -Destination $preparationGateTests -Sources @("$PSScriptRoot/test/psx/exporter/PreparationFieldLookupTest.java") -ClassPath $FmcBaselineAgent
        & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$FmcBaselineAgent;$preparationGateTests" psx.exporter.PreparationFieldLookupTest --preimage-gate $FmcBaselineAgent $preparationGate
        if($LASTEXITCODE-ne0){throw 'Preparation helper preimage ABI/full-body provenance failed; no overlay accepted'}
    }
    $sources=@($sources|Where-Object{([IO.Path]::GetFileName($_)-notlike'Fmc*.java'-or[IO.Path]::GetFileName($_)-like'FmcNativeRecords*.java'-or($NativeRecords-and[IO.Path]::GetFileName($_)-in@('FmcSourceDiagnostics.java','FmcQualification.java')))-and[IO.Path]::GetFileName($_)-notin@('PfdRecorder.java','RecorderPrepare.java')})
}
if($NativeRecords-and-not$HookOnly){throw 'Native-record qualification requires the hash-qualified delivered capture component.'}
if(@(Get-ChildItem -LiteralPath $classes -Filter 'Ios*.class' -Recurse).Count) {
    throw 'Superseded IOS classes remain in this graph. Queue it for human cleanup and use fresh work.'
}
$versionSource=Join-Path $outputRoot 'BuildVersion.java'
[IO.File]::WriteAllText($versionSource,('package psx.exporter; final class BuildVersion { static final String VALUE="'+$Version+'"; private BuildVersion(){} }'),[Text.UTF8Encoding]::new($false))
$sources+=@($versionSource)
Invoke-JavaSourceList -Destination $classes -Sources $sources -ClassPath $(if($HookOnly){$classes}else{''})
if ($LASTEXITCODE -ne 0) { throw 'Java exporter compilation failed' }
if($HookOnly-and$NativeRecords){
    $preparationOverrides=@('psx/exporter/FmcQualification.class','psx/exporter/FmcQualification$DeclaredFieldCache.class','psx/exporter/FmcQualification$FieldQuery.class')
    foreach($entry in $baselineEntries){
        # Diagnostics was already an explicitly compiled native-record helper.
        # All other reused component bytes retain their original identities.
        if($entry.Entry-in@('psx/exporter/FmcQualification.class','psx/exporter/FmcSourceDiagnostics.class')){continue}
        $file=Join-Path $classes $entry.Entry
        if(-not(Test-Path -LiteralPath $file)-or(ComponentHash $file)-cne$entry.SHA256){throw ('Unrelated captured component entry changed: '+$entry.Entry)}
    }
    $overrideEntries=@(foreach($name in $preparationOverrides){$file=Join-Path $classes $name;if(-not(Test-Path -LiteralPath $file)){throw ('Required preparation override missing: '+$name)};[ordered]@{Entry=$name;Bytes=(Get-Item -LiteralPath $file).Length;SHA256=(ComponentHash $file)}})
    $componentPath=Join-Path $outputRoot 'fmc-capture-component.json'
    $component=[IO.File]::ReadAllText($componentPath)|ConvertFrom-Json
    $component|Add-Member NoteProperty PreparationSourceCompiled $true
    $component|Add-Member NoteProperty PreparationHelperOverlay ([ordered]@{PreimageSourceSHA256='04F7D25D728166CD92A008A1003E5150F8BC23CEBC2F296B14AD1B6878A8BAA4';Proof=(Join-Path $preparationGate 'preimage-proof.txt');ProofSHA256=(ComponentHash (Join-Path $preparationGate 'preimage-proof.txt'));SourceSHA256=(ComponentHash "$PSScriptRoot/src/psx/exporter/FmcQualification.java");Entries=$overrideEntries;Scope='Completed declared-field lookups only; no paused recall or runtime adapter source compiled'})
    [IO.File]::WriteAllText($componentPath,($component|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
}
$manifest = Join-Path $outputRoot 'agent.mf'
if($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$'){throw 'Exporter manifest version must be numeric'}
[IO.File]::WriteAllText($manifest, "Manifest-Version: 1.0`nImplementation-Title: ASS PSX Addons`nImplementation-Version: $Version`nPremain-Class: psx.exporter.Agent`nCan-Redefine-Classes: false`nCan-Retransform-Classes: false`n`n", [Text.UTF8Encoding]::new($false))
$jar = Join-Path $outputRoot 'psx-exporter-agent.jar'
& "$JavaHome/bin/jar.exe" --create --file $jar --manifest $manifest -C $classes .
if ($LASTEXITCODE -ne 0) { throw 'Java exporter packaging failed' }
if($FmcSuites.Count){
    if(-not $RunTests){throw 'Focused FMC suites require -RunTests.'}
    $tests=Join-Path $outputRoot 'test-classes';[IO.Directory]::CreateDirectory($tests)|Out-Null
    $testSources=@(Get-ChildItem -LiteralPath "$PSScriptRoot/test/psx/exporter" -Filter '*.java' |Where-Object {$_.Name -notlike 'Ios*.java'-and(-not$HookOnly-or$_.Name-like'FmcNativeRecords*Test.java')}|ForEach-Object FullName)
    foreach($suite in $FmcSuites){
        if($suite -notmatch '^Fmc[A-Za-z0-9]+Test$' -or -not(Test-Path -LiteralPath "$PSScriptRoot/test/psx/exporter/$suite.java" -PathType Leaf)){throw 'Unknown focused FMC qualification suite.'}
    }
    Invoke-JavaSourceList -Destination $tests -Sources $testSources -ClassPath $classes
    if($LASTEXITCODE -ne 0){throw 'Focused FMC test compilation failed.'}
    foreach($suite in $FmcSuites){
        $suiteArguments=@()
        if($suite-eq'FmcNativeRecordsPrepareTest'){$suiteArguments=@($PsxJar)}
        if($suite -in @('FmcNavigationCatalogTest','FmcReconstructionTest','FmcReconstructionHeaderTest','FmcReconstructionNavTest','FmcReconstructionRecipeTest','FmcNavigationSourceBindingTest','FmcStep12Test')){$suiteArguments=@($outputRoot,$PsxJar)}
        elseif($suite -in @('FmcTerrainRoutePrepareTest','FmcExecMutationTest','FmcReconstructionPerfTest','FmcReconstructionDisplayTest','FmcReconstructionNormalExecTest','FmcReconstructionPauseTest','FmcReconstructionCruiseTest','FmcReconstructionOffsetTest','FmcReconstructionBusIsolationTest','FmcReconstructionApproachEffectsTest','FmcReconstructionNavigationTest','FmcReconstructionNavReleaseTest','FmcRenderingTest','FmcRenderingCaptureContextTest','FmcReconstructionTransitionEffectsTest','FmcReconstructionBodyNdTest','FmcReconstructionActivationTest','FmcBackgroundAdmissionProducerTest','FmcBackgroundAdmissionAfdsTest','FmcBackgroundAdmissionMemoryTest','FmcBackgroundAdmissionRolesTest','FmcBackgroundAdmissionJobScopeTest','FmcBackgroundAdmissionSchedulingTest','FmcBackgroundAdmissionSaveTest','FmcBackgroundAdmissionOptionTest','FmcBackgroundAdmissionOptionBindingTest','FmcBackgroundAdmissionOptionCompositionTest','FmcBackgroundAdmissionOptionSourceStabilityTest','FmcBackgroundAdmissionDispatchTest','FmcBackgroundAdmissionPhysicalDispatchTest','FmcBackgroundAdmissionConditionDispatchTest','FmcBackgroundAdmissionDormantConstructorTest','FmcBackgroundAdmissionAudioDispatchTest','FmcBackgroundAdmissionSliderDispatchTest','FmcBackgroundAdmissionMalfTest','FmcBackgroundAdmissionPacketControlTest','FmcBackgroundAdmissionButtonsTest','FmcBackgroundAdmissionAsyncTest','FmcBackgroundAdmissionScannerTest','FmcBackgroundAdmissionOperationAsyncTest','FmcBackgroundAdmissionContextSitesTest','FmcBackgroundAdmissionNavRadioDispatchTest','FmcBackgroundAdmissionPowerMenuDispatchTest','FmcBackgroundAdmissionMessageReceiversTest','FmcBackgroundAdmissionNavTuningTest','FmcBackgroundAdmissionAcarsMenuArgumentsTest','FmcBackgroundAdmissionNavMetadataTest','FmcBackgroundAdmissionNavFaultSharingTest','FmcBackgroundAdmissionNavRadioQualificationTest','FmcBackgroundAdmissionPageSelectionTest','FmcReconstructionHoldEffectsTest','FmcReconstructionTakeoffEffectsTest','FmcReconstructionTakeoffDisplayTest','FmcBackgroundAdmissionPrivateResultTest','FmcReconstructionQhEffectsTest','FmcReconstructionDisplayOutputEffectsTest','FmcReconstructionEiuSignalEffectsTest','FmcReconstructionQhReceiverProofTest','FmcBackgroundAdmissionJumpTest','FmcBackgroundAdmissionNavFoldTest','FmcBackgroundAdmissionCoreRequestsTest','FmcBackgroundAdmissionPacketTest','FmcBackgroundAdmissionInputLifetimesTest','FmcBackgroundAdmissionCduTest','FmcBackgroundAdmissionCadenceTest','FmcReconstructionGuidanceStateTest','FmcReconstructionDisplayStateTest','FmcBackgroundAdmissionNdDrawingTest','FmcReconstructionPhaseTest','FmcReconstructionCapacityTest','FmcReconstructionAcarsTest','FmcReconstructionPageEffectsTest','FmcReconstructionLandingEffectsTest','FmcReconstructionIceEffectsTest','FmcReconstructionPreservationTest','FmcBackgroundAdmissionCompleteTest','FmcBackgroundAdmissionPageDispatchTest','FmcBackgroundAdmissionTransportTest','FmcReconstructionExecRootsTest')){$suiteArguments=@($PsxJar)}
        if($suiteArguments.Count -and -not $PsxJar){throw 'Installed byte data required for this focused FMC suite.'}
        [string[]]$temporaryArgument=@()
        if($NativeRecords){$temporaryArgument=@("-Djava.io.tmpdir=$nativeTemporary")}
        & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' @temporaryArgument -cp "$classes;$tests" "psx.exporter.$suite" @suiteArguments
        if($LASTEXITCODE -ne 0){throw "Focused FMC qualification failed: $suite"}
        if($suite -eq 'FmcBackgroundAdmissionNavTuningTest'){
            foreach($mode in @('old','owned','closed','race','replacement','escaping','missing-enter','missing-complete','overflow')){
                & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$classes;$tests" "psx.exporter.$suite" $PsxJar $mode
                if($LASTEXITCODE -ne 0){throw "FMC native procedure-tuning lifetime qualification failed: $mode"}
            }
        }
    }
    Write-Output $jar
    return
}
if ($RunTests) {
    $tests = Join-Path $outputRoot 'test-classes'
    $fixtures = Join-Path $outputRoot 'fixtures'
    $audioFixtures = Join-Path $outputRoot 'audio-fixtures'
    $terrainFixtures = Join-Path $outputRoot 'terrain-fixtures'
    $displayFixtures = Join-Path $outputRoot 'display-fixtures'
    $aircraftFixtures = Join-Path $outputRoot 'aircraft-fixtures'
    $recorderFixtures = Join-Path $outputRoot 'recorder-fixtures'
    $instrumentFixtures = Join-Path $outputRoot 'instrument-fixtures'
    $instrumentStub = Join-Path $outputRoot 'instrument-stub'
    $retainedFixtures = Join-Path $outputRoot 'retained-fixtures'
    $stub = Join-Path $outputRoot 'native-stub'
    $statusFixtures = Join-Path $outputRoot 'status-fixtures'
    $statusStub = Join-Path $outputRoot 'status-stub'
    $trafficFixtures = Join-Path $outputRoot 'traffic-fixtures'
    $trafficStub = Join-Path $outputRoot 'traffic-stub'
    New-Item -ItemType Directory -Force -Path $trafficFixtures,$trafficStub | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -cp $classes -d $trafficFixtures "$PSScriptRoot/test/traffic-fixtures/TrafficFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Traffic fixture compilation failed' }
    & "$JavaHome/bin/javac.exe" --release 25 -d $trafficStub "$PSScriptRoot/test/traffic-stub/psx/exporter/TrafficNative.java"
    if ($LASTEXITCODE -ne 0) { throw 'Traffic JNI stub compilation failed' }
    New-Item -ItemType Directory -Force -Path $statusFixtures,$statusStub | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $statusFixtures "$PSScriptRoot/test/status-fixtures/StatusFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Status fixture compilation failed' }
    & "$JavaHome/bin/javac.exe" --release 25 -d $statusStub "$PSScriptRoot/test/status-stub/psx/exporter/StatusNative.java"
    if ($LASTEXITCODE -ne 0) { throw 'Status JNI stub compilation failed' }
    New-Item -ItemType Directory -Force -Path $tests, $fixtures, $stub, $audioFixtures, $terrainFixtures, $displayFixtures | Out-Null
    New-Item -ItemType Directory -Force -Path $aircraftFixtures | Out-Null
    New-Item -ItemType Directory -Force -Path $recorderFixtures | Out-Null
    New-Item -ItemType Directory -Force -Path $instrumentFixtures,$instrumentStub | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -cp $classes -d $instrumentFixtures "$PSScriptRoot/test/instrument-fixtures/InstrumentFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Instrument drawing fixture compilation failed' }
    & "$JavaHome/bin/javac.exe" --release 25 -d $instrumentStub "$PSScriptRoot/test/instrument-stub/psx/exporter/InstrumentRecorder.java"
    if ($LASTEXITCODE -ne 0) { throw 'Instrument fault fixture compilation failed' }
    New-Item -ItemType Directory -Force -Path $retainedFixtures | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $retainedFixtures "$PSScriptRoot/test/retained-fixtures/Tcas.java"
    if ($LASTEXITCODE -ne 0) { throw 'Retained display-context fixture compilation failed' }
    & "$JavaHome/bin/javac.exe" --release 25 -d $recorderFixtures "$PSScriptRoot/test/recorder-fixtures/RecorderFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Recorder fixture compilation failed' }
    & "$JavaHome/bin/javac.exe" --release 25 -d $aircraftFixtures "$PSScriptRoot/test/aircraft-fixtures/AircraftFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Aircraft fixture compilation failed' }
    & "$JavaHome/bin/javac.exe" --release 25 -d $displayFixtures "$PSScriptRoot/test/display-fixtures/DisplayFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Display fixture compilation failed' }
    & "$JavaHome/bin/javac.exe" --release 25 -d $terrainFixtures "$PSScriptRoot/test/terrain-fixtures/SupportFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Terrain fixture compilation failed' }
    & "$JavaHome/bin/javac.exe" --release 25 -d $fixtures "$PSScriptRoot/test/fixtures/Fixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Fixture compilation failed' }
    & "$JavaHome/bin/javac.exe" --release 25 -d $stub "$PSScriptRoot/test/native-stub/psx/exporter/BridgeNative.java"
    if ($LASTEXITCODE -ne 0) { throw 'JNI stub compilation failed' }
    & "$JavaHome/bin/javac.exe" --release 25 -d $audioFixtures "$PSScriptRoot/test/audio-fixtures/AudioFixture.java" "$PSScriptRoot/test/audio-fixtures/AL10.java"
    if ($LASTEXITCODE -ne 0) { throw 'Audio fixture compilation failed' }
    $testSources = @(Get-ChildItem -LiteralPath "$PSScriptRoot/test/psx/exporter" -Filter '*.java' | Where-Object {$_.Name-notlike'Ios*.java'-and(-not$HookOnly-or$_.Name-notlike'Fmc*.java'-or$_.Name-eq'FmcSourceServiceFixture.java'-or($NativeRecords-and$_.Name-like'FmcNativeRecords*.java'))} | ForEach-Object FullName)
    Invoke-JavaSourceList -Destination $tests -Sources $testSources -ClassPath $classes
    if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed' }
    & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$classes;$tests" psx.exporter.BuildVersionTests $jar $Version $outputRoot
    if($LASTEXITCODE-ne0){throw 'Exporter manifest/recording product-version disagreement'}
    & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$classes;$tests" psx.exporter.WriterRegistryTests
    if($LASTEXITCODE-ne0){throw 'Bounded writer registry overlap/publication-context qualification failed'}
    if($HookOnly-and$NativeRecords){
        $comparison=if($PreparationComparisonAgent){[IO.Path]::GetFullPath($PreparationComparisonAgent)}else{[IO.Path]::GetFullPath($FmcBaselineAgent)}
        $prefix=if($PreparationLowerDirectory){[IO.Path]::GetFullPath($PreparationLowerDirectory)}else{'-'}
        if(-not$PsxJar){throw 'Preparation field lookup qualification requires the installed JAR as data'}
        & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' "-Djava.io.tmpdir=$nativeTemporary" -cp "$classes;$tests" psx.exporter.PreparationFieldLookupTest $comparison $PsxJar $prefix (Join-Path $outputRoot ('preparation-field-lookup-'+[guid]::NewGuid().ToString('N')))
        if($LASTEXITCODE-ne0){throw 'Preparation field lookup differential/complete audit qualification failed'}
    }
    $hookFixtures=Join-Path $outputRoot 'hook-successor-fixtures'
    [IO.Directory]::CreateDirectory($hookFixtures)|Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $hookFixtures "$PSScriptRoot/test/status-controls-fixtures/StatusControlsFixture.java"
    if($LASTEXITCODE-ne0){throw 'Hook fixture compilation failed'}
    $statusControlsStub=Join-Path $outputRoot 'status-controls-stub'
    [IO.Directory]::CreateDirectory($statusControlsStub)|Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $statusControlsStub "$PSScriptRoot/test/status-controls-stub/psx/exporter/StatusControlsNative.java"
    if($LASTEXITCODE-ne0){throw 'Status Controls handoff double compilation failed'}
    $hookArguments=@($hookFixtures);if($PsxJar){$hookArguments+=@($PsxJar)}
    & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$statusControlsStub;$classes;$tests" psx.exporter.HookSuccessorTests @hookArguments
    if($LASTEXITCODE-ne0){throw 'Generated hook successor qualification failed'}
    if($NativeRecords){
        if(-not$PsxJar){throw 'Native FMC records require the installed JAR as data.'}
        foreach($suite in @('FmcNativeRecordsArtifactTest','FmcNativeRecordsExecutorTest','FmcNativeRecordsAdapterTest','FmcNativeRecordsPrepareTest','FmcNativeRecordsLayerTest','FmcNativeRecordsServiceTest')){
            [string[]]$nativeArguments=@()
            if($suite-eq'FmcNativeRecordsPrepareTest'){$nativeArguments=@($PsxJar)}
            if($suite-eq'FmcNativeRecordsLayerTest'){$nativeArguments=@($PsxJar,(Join-Path $outputRoot 'native-profile.txt'))}
            if($suite-eq'FmcNativeRecordsServiceTest'){$nativeArguments=@((Join-Path $outputRoot 'native-profile.txt'))}
            & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' "-Djava.io.tmpdir=$nativeTemporary" -cp "$classes;$tests" "psx.exporter.$suite" @nativeArguments
            if($LASTEXITCODE-ne0){throw "Native FMC record adapter qualification failed: $suite"}
        }
    }
    $navigationFixtures = Join-Path $outputRoot 'navigation-fixtures'
    New-Item -ItemType Directory -Force -Path $navigationFixtures | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $navigationFixtures "$PSScriptRoot/test/navigation-fixtures/Core.java"
    if ($LASTEXITCODE -ne 0) { throw 'Navigation fixture compilation failed' }
    $navigationJar = Join-Path $outputRoot 'navigation-fixture.jar'
    & "$JavaHome/bin/jar.exe" --create --file $navigationJar -C $navigationFixtures .
    if ($LASTEXITCODE -ne 0) { throw 'Navigation fixture packaging failed' }
    $navigationArgs = @($navigationJar, (Join-Path $outputRoot ('navigation-metadata-'+[Guid]::NewGuid().ToString('N'))))
    if ($PsxJar) { $navigationArgs += $PsxJar }
    & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.NavigationSelectionTests @navigationArgs
    if ($LASTEXITCODE -ne 0) { throw 'Navigation selection qualification/worker tests failed' }
    $performanceFixtures = Join-Path $outputRoot 'performance-fixtures'
    New-Item -ItemType Directory -Force -Path $performanceFixtures | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $performanceFixtures "$PSScriptRoot/test/performance-fixtures/PerformanceFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Performance fixture compilation failed' }
    $performanceJar = Join-Path $outputRoot 'performance-fixture.jar'
    & "$JavaHome/bin/jar.exe" --create --file $performanceJar -C $performanceFixtures .
    if ($LASTEXITCODE -ne 0) { throw 'Performance fixture packaging failed' }
    $performanceArgs = @($performanceJar, (Join-Path $outputRoot ('performance-metadata-'+[Guid]::NewGuid().ToString('N'))))
    if ($PsxJar) { $performanceArgs += $PsxJar }
    & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.PerformanceMetadataTests @performanceArgs
    if ($LASTEXITCODE -ne 0) { throw 'Performance metadata qualification/worker tests failed' }
    $maximumArgs = @($performanceJar, (Join-Path $outputRoot ('maximum-speed-'+[Guid]::NewGuid().ToString('N'))))
    if ($PsxJar) { $maximumArgs += $PsxJar }
    & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.MaximumSpeedMetadataTests @maximumArgs
    if ($LASTEXITCODE -ne 0) { throw 'Maximum-speed metadata qualification/worker tests failed' }
    if ($PsxJar) {
        & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.RuntimeMetadataMemoryTests (Join-Path $outputRoot 'runtime-metadata-memory') $PsxJar
        if ($LASTEXITCODE -ne 0) { throw 'Runtime metadata retained-heap regression failed' }
    }
    $instructorFixtures = Join-Path $outputRoot 'instructor-fixtures'
    New-Item -ItemType Directory -Force -Path $instructorFixtures | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $instructorFixtures "$PSScriptRoot/test/instructor-fixtures/InstructorFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Instructor fixture compilation failed' }
    $instructorJar = Join-Path $outputRoot 'instructor-fixture.jar'
    & "$JavaHome/bin/jar.exe" --create --file $instructorJar -C $instructorFixtures .
    if ($LASTEXITCODE -ne 0) { throw 'Instructor fixture packaging failed' }
    $instructorArgs = @($instructorJar, (Join-Path $outputRoot ('instructor-metadata-'+[Guid]::NewGuid().ToString('N'))))
    if ($PsxJar) { $instructorArgs += $PsxJar }
    & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.InstructorMetadataTests @instructorArgs
    if ($LASTEXITCODE -ne 0) { throw 'Instructor metadata qualification/worker tests failed' }
    $situationFixtures = Join-Path $outputRoot 'situation-fixtures'
    New-Item -ItemType Directory -Force -Path $situationFixtures,(Join-Path $outputRoot 'Situations') | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $situationFixtures "$PSScriptRoot/test/situation-fixtures/SituationFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Situation fixture compilation failed' }
    $situationJar = Join-Path $outputRoot 'situation-fixture.jar'
    & "$JavaHome/bin/jar.exe" --create --file $situationJar -C $situationFixtures .
    if ($LASTEXITCODE -ne 0) { throw 'Situation fixture packaging failed' }
    $situationArgs=@($situationJar)
    if($PsxJar) { $situationArgs+=@($PsxJar,(Join-Path $outputRoot 'situation-installed-inventory.txt')) }
    Push-Location $outputRoot
    try {
        & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.SituationTests @situationArgs
        if ($LASTEXITCODE -ne 0) { throw 'Situation native/generated bridge tests failed' }
    } finally { Pop-Location }
    $resetFixtures = Join-Path $outputRoot 'reset-fixtures'
    New-Item -ItemType Directory -Force -Path $resetFixtures | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $resetFixtures "$PSScriptRoot/test/reset-fixtures/ResetFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Reset fixture compilation failed' }
    $resetJar = Join-Path $outputRoot 'reset-fixture.jar'
    & "$JavaHome/bin/jar.exe" --create --file $resetJar -C $resetFixtures .
    if ($LASTEXITCODE -ne 0) { throw 'Reset fixture packaging failed' }
    $resetArgs = @($resetJar)
    if($PsxJar) { $resetArgs += $PsxJar }
    & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.ResetMalfunctionsTests @resetArgs
    if ($LASTEXITCODE -ne 0) { throw 'Reset native Action/EDT tests failed' }
    & "$JavaHome/bin/java.exe" -cp "$trafficStub;$stub;$classes;$tests" psx.exporter.TrafficTests $trafficFixtures
    if ($LASTEXITCODE -ne 0) { throw 'Traffic source/production-generated fixture tests failed' }
    & "$JavaHome/bin/java.exe" -Xbatch '-XX:-TieredCompilation' -cp "$trafficStub;$stub;$classes;$tests" psx.exporter.TrafficBenchmark $trafficFixtures
    if ($LASTEXITCODE -ne 0) { throw 'Traffic authored callback allocation/timing check failed' }
    if($PsxJar) {
        & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.TrafficTests --installed $PsxJar (Join-Path $outputRoot 'traffic-installed-inventory.txt')
        if ($LASTEXITCODE -ne 0) { throw 'Installed traffic qualification failed' }
    }
    $statusArgs=@($statusFixtures)
    if($PsxJar) { $statusArgs+=@($PsxJar,(Join-Path $outputRoot 'status-installed-inventory.txt')) }
    & "$JavaHome/bin/java.exe" -cp "$statusStub;$stub;$classes;$tests" psx.exporter.StatusTests @statusArgs
    if ($LASTEXITCODE -ne 0) { throw 'Status source/bridge tests failed' }
    $testArgs = @($fixtures, $stub, $classes)
    $environmentFixtures=Join-Path $OutputDirectory 'environment-fixtures'
    New-Item -ItemType Directory -Force $environmentFixtures | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $environmentFixtures "$PSScriptRoot/test/environment-fixtures/EnvironmentFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Environment fixture compilation failed' }
    & "$JavaHome/bin/javac.exe" --release 25 -d $stub "$PSScriptRoot/test/environment-stub/psx/exporter/EnvironmentNative.java" "$PSScriptRoot/test/environment-stub/psx/exporter/EnvironmentContextNative.java" "$PSScriptRoot/test/environment-stub/psx/exporter/WeatherChangeNative.java" "$PSScriptRoot/test/environment-stub/psx/exporter/EnvironmentVisibilityNative.java"
    if ($LASTEXITCODE -ne 0) { throw 'Environment JNI fixture compilation failed' }
    if ($PsxJar) { $testArgs += $PsxJar }
    if ($ArchivedPsxJar) { $testArgs += $ArchivedPsxJar }
    & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.ExporterTests @testArgs
    if ($LASTEXITCODE -ne 0) { throw 'Exporter tests failed' }
    & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.AudioTests $audioFixtures $stub $classes $PsxJar
    if ($LASTEXITCODE -ne 0) { throw 'Audio exporter tests failed' }
    # PowerShell removes an empty native argument; retain a deliberate no-vendor sentinel.
    $terrainJar = if($PsxJar) { $PsxJar } else { '-' }
    & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.TerrainTests $terrainJar $terrainFixtures $stub $classes
    if ($LASTEXITCODE -ne 0) { throw 'Terrain structural/geometry tests failed' }
    & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$classes;$tests" psx.exporter.DisplayTests $displayFixtures $classes $terrainJar
    if ($LASTEXITCODE -ne 0) { throw 'Display observer tests failed' }
    & "$JavaHome/bin/java.exe" -Xbatch '-XX:-TieredCompilation' -cp "$classes;$tests" psx.exporter.AircraftTests $aircraftFixtures $stub $classes $terrainJar
    if ($LASTEXITCODE -ne 0) { throw 'Aircraft observer tests failed' }
    & "$JavaHome/bin/java.exe" -Xbatch '-XX:-TieredCompilation' -cp "$classes;$tests" psx.exporter.EnvironmentTests $environmentFixtures $stub $classes $jar
    if ($LASTEXITCODE -ne 0) { throw 'Environment observer tests failed' }
    $changeFixtures=Join-Path $OutputDirectory 'weather-change-fixtures'
    New-Item -ItemType Directory -Force $changeFixtures | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $changeFixtures "$PSScriptRoot/test/weather-change-fixtures/WeatherChangeFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Weather change fixture compilation failed' }
    & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.WeatherChangeTests $changeFixtures $stub $classes $PsxJar
    if ($LASTEXITCODE -ne 0) { throw 'Weather change qualification failed' }
    $visibilityFixtures=Join-Path $OutputDirectory 'visibility-fixtures'
    New-Item -ItemType Directory -Force $visibilityFixtures | Out-Null
    & "$JavaHome/bin/javac.exe" --release 25 -d $visibilityFixtures "$PSScriptRoot/test/visibility-fixtures/VisibilityFixture.java"
    if ($LASTEXITCODE -ne 0) { throw 'Local visibility fixture compilation failed' }
    & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.EnvironmentVisibilityTests $visibilityFixtures $stub $classes $PsxJar
    if ($LASTEXITCODE -ne 0) { throw 'Local visibility target qualification failed' }
    if ($PsxJar) {
        & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.EnvironmentTests --installed $PsxJar (Join-Path $OutputDirectory 'environment-installed-inventory.txt')
        if ($LASTEXITCODE -ne 0) { throw 'Installed environment qualification failed' }
        $fmcIntegrated = Join-Path $outputRoot 'fmc-source-installed'
        New-Item -ItemType Directory -Force -Path $fmcIntegrated | Out-Null
        $captureSuite=if($HookOnly){'CaptureBaselineTests'}else{'FmcSourceIntegrationTest'}
        & "$JavaHome/bin/java.exe" -cp "$classes;$tests" "psx.exporter.$captureSuite" $PsxJar $fmcIntegrated
        if ($LASTEXITCODE -ne 0) { throw 'Installed optional FMC source integration failed' }
        if(-not$HookOnly){
        & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.FmcNavigationCatalogTest $outputRoot $PsxJar
        if ($LASTEXITCODE -ne 0) { throw 'Detached FMC navigation binding qualification failed' }
        & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$classes;$tests" psx.exporter.FmcTerrainRoutePrepareTest $PsxJar
        if ($LASTEXITCODE -ne 0) { throw 'FMC terrain route region qualification failed' }
        & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$classes;$tests" psx.exporter.FmcExecMutationTest $PsxJar
        if ($LASTEXITCODE -ne 0) { throw 'FMC native EXEC mutation authority qualification failed' }
        foreach($suite in @('FmcReconstructionTest','FmcReconstructionHeaderTest','FmcReconstructionNavTest','FmcReconstructionRecipeTest','FmcNavigationSourceBindingTest','FmcStep12Test')){
            & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$classes;$tests" "psx.exporter.$suite" $outputRoot $PsxJar
            if($LASTEXITCODE -ne 0){throw "FMC complete reconstruction/admission qualification failed: $suite"}
        }
        foreach($suite in @('FmcReconstructionPerfTest','FmcBackgroundAdmissionCompleteTest','FmcReconstructionDisplayTest','FmcReconstructionExecRootsTest','FmcReconstructionNormalExecTest','FmcReconstructionPauseTest','FmcReconstructionCruiseTest','FmcReconstructionOffsetTest','FmcReconstructionBusIsolationTest','FmcReconstructionApproachEffectsTest','FmcReconstructionNavigationTest','FmcReconstructionNavReleaseTest','FmcRenderingTest','FmcRenderingCaptureContextTest','FmcReconstructionTransitionEffectsTest','FmcReconstructionBodyNdTest','FmcReconstructionActivationTest','FmcBackgroundAdmissionProducerTest','FmcBackgroundAdmissionAfdsTest','FmcBackgroundAdmissionMemoryTest','FmcBackgroundAdmissionRolesTest','FmcBackgroundAdmissionJobScopeTest','FmcBackgroundAdmissionSchedulingTest','FmcBackgroundAdmissionSaveTest','FmcBackgroundAdmissionOptionTest','FmcBackgroundAdmissionOptionBindingTest','FmcBackgroundAdmissionOptionCompositionTest','FmcBackgroundAdmissionOptionSourceStabilityTest','FmcBackgroundAdmissionDispatchTest','FmcBackgroundAdmissionPhysicalDispatchTest','FmcBackgroundAdmissionConditionDispatchTest','FmcBackgroundAdmissionDormantConstructorTest','FmcBackgroundAdmissionAudioDispatchTest','FmcBackgroundAdmissionSliderDispatchTest','FmcBackgroundAdmissionMalfTest','FmcBackgroundAdmissionPacketControlTest','FmcBackgroundAdmissionButtonsTest','FmcBackgroundAdmissionAsyncTest','FmcBackgroundAdmissionScannerTest','FmcBackgroundAdmissionOperationAsyncTest','FmcBackgroundAdmissionContextSitesTest','FmcBackgroundAdmissionNavRadioDispatchTest','FmcBackgroundAdmissionPowerMenuDispatchTest','FmcBackgroundAdmissionMessageReceiversTest','FmcBackgroundAdmissionNavTuningTest','FmcBackgroundAdmissionAcarsMenuArgumentsTest','FmcBackgroundAdmissionNavMetadataTest','FmcBackgroundAdmissionNavFaultSharingTest','FmcBackgroundAdmissionNavRadioQualificationTest','FmcBackgroundAdmissionPageSelectionTest','FmcReconstructionHoldEffectsTest','FmcReconstructionTakeoffEffectsTest','FmcReconstructionTakeoffDisplayTest','FmcBackgroundAdmissionPrivateResultTest','FmcReconstructionQhEffectsTest','FmcReconstructionDisplayOutputEffectsTest','FmcReconstructionEiuSignalEffectsTest','FmcReconstructionQhReceiverProofTest','FmcBackgroundAdmissionJumpTest','FmcBackgroundAdmissionNavFoldTest','FmcBackgroundAdmissionCoreRequestsTest','FmcBackgroundAdmissionPacketTest','FmcBackgroundAdmissionInputLifetimesTest','FmcBackgroundAdmissionCduTest','FmcBackgroundAdmissionCadenceTest','FmcReconstructionGuidanceStateTest','FmcReconstructionDisplayStateTest','FmcBackgroundAdmissionNdDrawingTest','FmcReconstructionPhaseTest','FmcReconstructionCapacityTest','FmcReconstructionAcarsTest','FmcReconstructionPageEffectsTest','FmcReconstructionLandingEffectsTest','FmcReconstructionIceEffectsTest','FmcReconstructionPreservationTest','FmcBackgroundAdmissionTransportTest','FmcBackgroundAdmissionPageDispatchTest')){
            & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$classes;$tests" "psx.exporter.$suite" $PsxJar
            if($LASTEXITCODE -ne 0){throw "FMC native bank/effect qualification failed: $suite"}
        if($suite -eq 'FmcBackgroundAdmissionNavTuningTest'){
            foreach($mode in @('old','owned','closed','race','replacement','escaping','missing-enter','missing-complete','overflow')){
                & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$classes;$tests" "psx.exporter.$suite" $PsxJar $mode
                if($LASTEXITCODE -ne 0){throw "FMC native procedure-tuning lifetime qualification failed: $mode"}
            }
        }
        }
    }
    if(-not$HookOnly){
    foreach($suite in @('FmcBackgroundAdmissionTest','FmcBackgroundAdmissionInputTest','FmcBackgroundAdmissionBankTest','FmcBackgroundAdmissionFieldTest','FmcBackgroundAdmissionReceiverFlowTest','FmcBackgroundAdmissionDispatchProjectionTest','FmcBackgroundAdmissionPageContentTest','FmcBackgroundAdmissionRegionTest','FmcBackgroundAdmissionPauseTest','FmcBackgroundAdmissionFaultsTest','FmcBackgroundAdmissionStartFlowTest','FmcBackgroundAdmissionClosureTest','FmcBackgroundAdmissionObserversTest','FmcBackgroundAdmissionPollTagTest','FmcBackgroundAdmissionTransportLifetimeTest','FmcReconstructionArrayProofTest')){
        & "$JavaHome/bin/java.exe" '-Djava.awt.headless=true' -cp "$classes;$tests" "psx.exporter.$suite"
        if($LASTEXITCODE -ne 0){throw "FMC receiver/lifetime/failure qualification failed: $suite"}
    }
    }
    }
    if(-not$HookOnly){
    foreach($suite in @('FmcBridgeAccessTest','FmcPublicationTest','FmcTransactionTest','FmcNarrowContentTest','FmcRecallProfileTest','FmcNarrowControlTest','FmcNarrowSourceTest','FmcNarrowBindingsTest','FmcCurrentLifecycleTest','FmcExclusionTest','FmcReceiverAdmissionTest','FmcReplacedAdmissionTest','FmcClosedFailureTest','FmcAdmissionCapacityTest','FmcJobsTest','FmcJobAdmissionTest','FmcAsyncAdmissionTest','FmcJobPrepareTest','FmcQuietAdmissionTest','FmcInputPrepareTest','FmcExecPrepareTest','FmcExecLifecycleTest','FmcExecChildMutationTest','FmcExecChildReopenTest','FmcExecFinisherTest','FmcExecStrictCheckpointTest','FmcReconstructionQhAliasTest','FmcExecKernelLinkageTest','FmcStep12FailureTest','FmcRenderingCaptureTest','FmcSelectedStartTest','FmcRecallBoundaryTest','FmcWriterPrepareTest','FmcCompatibilityWriterTest','FmcContentReconcilePrepareTest','FmcCostIndexReconcilePrepareTest','FmcFreshSelectionPrepareTest')) {
        & "$JavaHome/bin/java.exe" -cp "$classes;$tests" "psx.exporter.$suite"
        if ($LASTEXITCODE -ne 0) { throw "FMC integration regression failed: $suite" }
    }
    }
    if ($ArchivedCoreClass -and $ArchivedAerodynamicsClass) {
        & "$JavaHome/bin/java.exe" -cp "$classes;$tests" psx.exporter.CompatibilityTests $ArchivedCoreClass $ArchivedAerodynamicsClass
        if ($LASTEXITCODE -ne 0) { throw 'Archived sampling selector tests failed' }
    }
}
Write-Output $jar
$completed = $true
} finally {
    foreach($name in $previousTemporary.Keys){[Environment]::SetEnvironmentVariable($name,$previousTemporary[$name],'Process')}
    if($null -ne $lease) {
        try {
            $reason=if($completed){'Standalone Java build complete; collecting caller must verify/consume the JAR before Ready'}else{'Java build failed; caller must resolve failure and evidence before Ready'}
            Set-WorkCleanupDisposition -Lease $lease -State Held -Reason $reason -Findings 'docs/validation/code-review-20261007.md'
        } finally { Exit-WorkCleanup -Lease $lease }
    }
}
