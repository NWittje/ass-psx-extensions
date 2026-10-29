param([Parameter(Mandatory)][string]$CommonPublisher)
$script:iosCommonPublisher=[IO.Path]::GetFullPath($CommonPublisher)
# Bind the imported transaction API. Other modules may evolve the builder's
# main metadata independently; retain its observed whole-file preimage during
# each actual namespace commit.
$script:iosCommonHash=(Get-FileHash -LiteralPath $script:iosCommonPublisher -Algorithm SHA256).Hash
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($script:iosCommonPublisher,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Common publication contract does not parse.'}
$functions=@('Get-PublicationInventory','Assert-PublicationInventory','Assert-PublicationSources','Move-PublicationDirectory','Get-PublicationNamespaceState','Assert-PublicationNamespaceState','New-PublicationAuthority','Get-PublicationProcesses','Assert-PublicationProductIdle','Assert-PublicationAuthority','Invoke-PublicationAuthority','Assert-PublicationRoles','Commit-Publication')
$contractParts=[Collections.Generic.List[string]]::new()
foreach($name in $functions){
 $definitions=$ast.FindAll({param($node)$node-is[Management.Automation.Language.FunctionDefinitionAst]-and$node.Name-eq$name},$true)
 if($definitions.Count-ne1){throw "Common publication function is missing or ambiguous: $name"}
 $contractParts.Add($definitions[0].Extent.Text.Replace("`r`n","`n"))
 . ([scriptblock]::Create($definitions[0].Extent.Text))
}
$algorithm=[Security.Cryptography.SHA256]::Create()
try{$contractHash=[BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes(($contractParts-join"`n`n")))).Replace('-','')}
finally{$algorithm.Dispose()}
if($contractHash-cne'10A81C7E741CFEA6B9367519D2DE184FA405A907E25B7BEFB05F3A3E895CBC4D'){throw 'Imported publication transaction API differs from the qualified edition.'}
function FileHash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}
function Write-PublicationWarning([string]$Text){Write-Warning $Text}
function Invoke-IosCatalogStep([scriptblock]$Action){
 # Keep the real shared catalogue and all existing assertions. Cloud-backed
 # catalogue reads can outlast the generic one-second acquisition deadline.
 # This scope covers a single metadata check/update, never payload copying.
 $catalog=[Threading.Mutex]::new($false,'Local\PSXIOS.WorkCleanup.catalog');$held=$false
 try{try{$held=$catalog.WaitOne(20000)}catch [Threading.AbandonedMutexException]{$held=$true};if(-not$held){throw 'IOS publication catalogue busy after bounded wait.'};& $Action}
 finally{if($held){$catalog.ReleaseMutex()};$catalog.Dispose()}
}
function Enter-IosWorkCleanup([string]$WorkDirectory,[string]$Owner){
 # Acquire the actual work mutex first. Recursive same-thread acquisition lets
 # the unchanged helper validate and issue its real lease under the bounded
 # catalogue wait, without reversing the work-before-catalogue lock order.
 $path=Resolve-WorkCleanupPath $WorkDirectory
 $work=Enter-WorkCleanupMutex (Get-WorkCleanupId $path)
 try{Invoke-IosCatalogStep {Enter-WorkCleanup -WorkDirectory $path -Owner $Owner}}
 finally{Exit-WorkCleanupMutex $work}
}
function Assert-IosOuterPublicationLease([string]$Context,[string]$Work,[string]$Helper){
 $root=($Context|ConvertFrom-Json).ProjectRoot
 . $Helper -CleanupProjectRoot $root
 Invoke-IosCatalogStep {$null=Assert-WorkCleanupParent -WorkDirectory (Join-Path $Work 'authority-check') -Owner ios-6.1 -Context $Context -AllowSameProcess}
}
function Assert-IosPackageGuard($Authority,[string]$OuterContext,[string]$OuterWork,[string]$Helper,[string]$CommonPath,[string]$CommonHash,[string]$Phase,[string]$Source,[string]$Destination){
 Assert-IosOuterPublicationLease $OuterContext $OuterWork $Helper
 . $Helper -CleanupProjectRoot $Authority.Scope.Root
 Invoke-IosCatalogStep {Assert-PublicationAuthority $Authority $Phase $Source $Destination}
 if((Get-FileHash -LiteralPath $CommonPath -Algorithm SHA256).Hash-ne$CommonHash){throw 'Common publication source changed.'}
}
function New-IosPackageGuard($Authority,[string]$OuterContext,[string]$OuterWork,[string]$Helper){
 $commonPath=$script:iosCommonPublisher;$commonHash=$script:iosCommonHash
 $assertion=(Get-Command Assert-IosPackageGuard -CommandType Function).ScriptBlock
 {param($Phase,$Source,$Destination)
  & $assertion $Authority $OuterContext $OuterWork $Helper $commonPath $commonHash $Phase $Source $Destination
 }.GetNewClosure()
}
function Assert-IosExistingPackage($State,[string]$Destination,$Manifest,[string[]]$Mutable,[string]$Configuration){
 if(-not$State.Exists){return}
 if(-not$Manifest){if(@($State.Files).Count-or@($State.Directories).Count){throw 'Existing IOS package has no qualified manifest.'};return}
 if($Manifest.schema-cne'psx-ios-package-v1'-or$Manifest.configuration-cne$Configuration-or-not@($Manifest.payload).Count){throw 'Existing IOS package manifest identity is invalid.'}
 $known=@{'manifest.json'=$true};foreach($name in $Mutable){$known[$name]=$true}
 $prefix=$Destination.TrimEnd('\','/')+'\'
 foreach($entry in $Manifest.payload){
  $path=[IO.Path]::GetFullPath([string]$entry.path)
  if(-not$path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Existing IOS payload is outside the selected package.'}
  $relative=$path.Substring($prefix.Length)
  if($known.ContainsKey($relative)){throw 'Existing IOS manifest payload names overlap.'}
  $found=@($State.Files|Where-Object {$_.Path-ieq$relative})
  if($found.Count-ne1-or$found[0].Bytes-ne$entry.bytes-or$found[0].SHA256-ine$entry.sha256){throw "Existing IOS immutable payload differs: $relative"}
  $known[$relative]=$true
 }
 foreach($entry in $State.Files){if(-not$known.ContainsKey($entry.Path)){throw "Unrecognized IOS package file retained: $($entry.Path)"}}
 foreach($directory in $State.Directories){if(-not@($State.Files|Where-Object {$_.Path.StartsWith($directory+'\',[StringComparison]::OrdinalIgnoreCase)}).Count){throw "Empty or unrecognized IOS package directory retained: $directory"}}
}
