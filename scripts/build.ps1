# Native synchronization/HWIO entry point. Other products use module builders.
param(
    [ValidateSet('All','Debug','Release')][string]$Configuration = 'All',
    [ValidateSet('All','Plugin','Coordinator','Exporter','HWIO')][string]$Module = 'All',
    [string]$Generator = 'Visual Studio 18 2026',
    [string]$XPlaneSdkRoot = '',
    [string]$JavaHome = '',
    [string]$PsxJar = '',
    [string]$GraphicsSdkRoot = '',
    [string]$WorkDirectory = '',
    [string]$SourceDirectory = '',
    [switch]$Publish,
    [switch]$SkipTests,
    [switch]$KeepBuild
)
$ErrorActionPreference = 'Stop'
$projectRoot = 'P:\development\psx_ios'
if(-not $SourceDirectory) { throw 'Supply SourceDirectory: the verified source snapshot in task-owned local work.' }
$sourceRoot = [IO.Path]::GetFullPath($SourceDirectory)
if(-not $sourceRoot.StartsWith('O:\psx\bin\.work\',[StringComparison]::OrdinalIgnoreCase)){throw 'New builds require a verified O: work-scope source snapshot; maintained source stays on P:.'}
if(-not (Test-Path -LiteralPath (Join-Path $sourceRoot 'modules') -PathType Container)) {
    throw 'Qualified O: source inputs are missing; supply the verified input snapshot before building.'
}
# File count is advisory. Admit builds by resources/dependencies; close temporary
# output through docs/operations/task-housekeeping.md rather than a numeric gate.
if($Publish -and $SkipTests) { throw 'Publication requires matching tests; SkipTests builds remain unqualified.' }
# Qualification inspects installed PSX class bytes without starting PSX.
if($Module -in @('All','Exporter') -and -not $SkipTests){
    if(-not $PsxJar -or -not [IO.Path]::IsPathFullyQualified($PsxJar) -or -not (Test-Path -LiteralPath $PsxJar -PathType Leaf)){
        throw 'Supply PsxJar: the installed Aerowinx.jar used as read-only exporter qualification data.'
    }
}
if(-not $XPlaneSdkRoot) { $XPlaneSdkRoot = Join-Path $sourceRoot 'external/xplane-sdk-4.3' }
# Each module pins its qualified graphics version. An explicit override remains
# available; a shared legacy default must not replace the individual pins.
if(-not $WorkDirectory) { $WorkDirectory = Join-Path 'O:\psx' ('bin/.work/native-'+[Guid]::NewGuid().ToString('N')) }
$work = [IO.Path]::GetFullPath($WorkDirectory)
$workBase = 'O:\psx\bin\.work'
if(-not $work.StartsWith($workBase+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Build work must be a child of bin/.work.' }
if(Test-Path -LiteralPath $work) { throw 'Select a fresh work directory; old graphs are not recycled.' }
for($ancestor=$workBase;$ancestor;$ancestor=[IO.Path]::GetDirectoryName($ancestor)) {
    if((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Reparse point in build work ancestry.' }
}
$configs = if($Configuration -eq 'All') { @('Debug','Release') } else { @($Configuration) }
$products = if($Module -eq 'All') { @('Plugin','Coordinator','Exporter','HWIO') } else { @($Module) }
$names = @{Plugin='xplane-plugin';Coordinator='psx-xp-coordinator';Exporter='psx-exporter';HWIO='psx-hwio'}
$results = [Collections.Generic.List[object]]::new()
$completed = $false
. (Join-Path $PSScriptRoot 'work-cleanup.ps1') -CleanupProjectRoot 'O:\psx'
$cleanupLease = Enter-WorkCleanup -WorkDirectory $work -Owner 'root-native-builder'
$savedBuildEnvironment = @{}
foreach($name in 'TEMP','TMP','TMPDIR','MSBUILDDISABLENODEREUSE') {
    $savedBuildEnvironment[$name] = [Environment]::GetEnvironmentVariable($name,'Process')
}
$buildTemp = Join-Path $work 'temporary'
try {
    [IO.Directory]::CreateDirectory($buildTemp) | Out-Null
    foreach($name in 'TEMP','TMP','TMPDIR') { [Environment]::SetEnvironmentVariable($name,$buildTemp,'Process') }
    # Reused MSBuild nodes cache another task's temporary directory. Their lifetime
    # must end with this invocation before task-owned temporary files are removed.
    [Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE','1','Process')
    foreach($product in $products) {
        $source = Join-Path $sourceRoot ('modules/'+$names[$product])
        $graph = Join-Path $work $names[$product]
        $arguments = @('-S',$source,'-B',$graph,'-G',$Generator,'-A','x64')
        switch($product) {
            Plugin { $arguments += @('-DPSX_XPLANE_PUBLISH_BINARIES=OFF',"-DXPLANE_SDK_ROOT=$XPlaneSdkRoot") }
            Coordinator { $arguments += @('-DCOORDINATOR_PUBLISH_BINARIES=OFF',"-DCOORDINATOR_TRAFFIC_SDK_ROOT=$XPlaneSdkRoot"); if($GraphicsSdkRoot) { $arguments += "-DASS_GRAPHICS_SDK_ROOT=$GraphicsSdkRoot" } }
            Exporter { $arguments += '-DPSX_EXPORTER_PUBLISH=OFF'; if($JavaHome) { $arguments += "-DPSX_EXPORTER_JAVA_HOME=$JavaHome" }; if($PsxJar) { $arguments += "-DPSX_EXPORTER_TEST_PSX_JAR=$PsxJar" } }
            HWIO { $arguments += '-DHWIO_PUBLISH_BINARIES=OFF'; if($GraphicsSdkRoot) { $arguments += "-DASS_GRAPHICS_SDK_ROOT=$GraphicsSdkRoot" } }
        }
        & cmake @arguments
        if($LASTEXITCODE -ne 0) { throw "$product configuration failed." }
        # Native instance checks compare the two product configurations. Build
        # both before testing either, and qualify the requested set before any
        # publication; a clean Debug graph must not depend on an old Release.
        # HWIO's instance fixture needs the paired executable even when only
        # one configuration is requested for delivery. Settings stay authoritative.
        $buildConfigs=if($product-eq'HWIO'-and-not$SkipTests){@('Debug','Release')}else{$configs}
        foreach($config in $buildConfigs) {
            $buildArguments = @('--build',$graph,'--config',$config,'--parallel','4')
            if($Generator -match '^Visual Studio ') { $buildArguments += @('--','/nr:false') }
            & cmake @buildArguments
            if($LASTEXITCODE -ne 0) { throw "$product $config build failed." }
        }
        foreach($config in $configs) {
            if(-not $SkipTests) {
                & ctest --test-dir $graph -C $config --output-on-failure
                if($LASTEXITCODE -ne 0) { throw "$product $config qualification failed." }
            }
        }
        foreach($config in $configs) {
            if($Publish -and $config -eq 'Release') {
                # Plugin publication also requires the qualified Sparky overlay
                # generated by the plugin owner under graph/adapter.
                & (Join-Path $PSScriptRoot 'publish-binaries.ps1') -Module $product -Configuration $config -BuildDirectory $graph -SourceDirectory $sourceRoot -CleanupProjectRoot 'O:\psx' -CoordinatedPublication -ParentCleanupOwner 'root-native-builder'
                & (Join-Path $PSScriptRoot 'verify-binaries.ps1') -Module $product -Configuration $config -VerifyInputs
            }
            $results.Add([ordered]@{Module=$product;Configuration=$config;Tests=$(if($SkipTests){'not_run'}else{'passed'});Published=([bool]$Publish -and $config -eq 'Release');Graph=$graph})
        }
    }
    $completed = $true
} finally {
    foreach($entry in $savedBuildEnvironment.GetEnumerator()) {
        if($null -eq $entry.Value) {
            [Environment]::SetEnvironmentVariable($entry.Key,[NullString]::Value,'Process')
        } else {
            [Environment]::SetEnvironmentVariable($entry.Key,$entry.Value,'Process')
        }
    }
    try {
        # One current finding replaces loose build history. Uncollected outputs
        # and failed work stay Held until their owner resolves the consumer.
        $receipt = Join-Path $projectRoot 'docs/validation/current-native-build.json'
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($receipt)) | Out-Null
        $ready = $completed -and $Publish -and -not $KeepBuild
        $state = if($ready){'Ready'}else{'Held'}
        $reason = if($ready){'Published payloads and source inputs verified; no remaining producer consumer.'}elseif($completed){'Unpublished producer or explicit KeepBuild consumer requires owner collection/disposition.'}else{'Failed operation needs owner triage and minimum evidence disposition.'}
        [IO.File]::WriteAllText($receipt,([ordered]@{UTC=[DateTime]::UtcNow.ToString('o');Complete=$completed;Owner='native build entry point';Work=$work;Results=$results.ToArray();LoadedAcceptance='Not measured';CleanupState=$state;CleanupReason=$reason;OpenProblem=$(if($completed){$null}else{'Build/qualification/publication failed; triage retained work.'})} | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
        Set-WorkCleanupDisposition -Lease $cleanupLease -State $state -Reason $reason -Findings 'docs/validation/current-native-build.json'
        Write-Output "Cleanup $state; user-run queue: $($cleanupLease.Manifest)"
    } catch {
        Write-Warning ('Cleanup registration failed; work retained: '+$_.Exception.Message)
    } finally {
        Exit-WorkCleanup -Lease $cleanupLease
    }
}
