#requires -Version 7.0
param([string]$SdkRoot='')
. (Join-Path $PSScriptRoot '../../../shared/graphics-sdk/graphics144.ps1')
function Get-CduGraphics144Sdk {
 param([Parameter(Mandatory)][string]$Sdk,[Parameter(Mandatory)][ValidateSet('Debug','Release')][string]$Configuration)
 if(![IO.Path]::IsPathFullyQualified($Sdk)-or$Sdk-match'(^|[\\/])\.\.?([\\/]|$)'){throw 'Absolute ordinary SDK path required'}
 $root=[IO.Path]::GetFullPath($Sdk).TrimEnd('\','/')
 if((Split-Path $root -Leaf)-cne$Configuration){throw 'SDK configuration path differs'}
 $qualified=Get-PsxGraphics144Sdk (Split-Path $root -Parent) $Configuration
 $files=@(foreach($file in $qualified.files){if($file.path.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)){
  [pscustomobject]@{path=$file.path;origin=$file.origin;relative=[IO.Path]::GetRelativePath($root,$file.path);bytes=$file.bytes;sha256=$file.sha256}
 }})
 [pscustomobject]@{root=$root;origin=$root;version=$qualified.version;abi=$qualified.abi;configuration=$Configuration;manifestSHA256=$qualified.manifestSHA256;files=$files;producerStatus=($qualified.producerState+'; CDU consumer qualification recorded separately')}
}
if($SdkRoot){foreach($c in 'Debug','Release'){$sdk=Get-CduGraphics144Sdk (Join-Path $SdkRoot $c) $c;Write-Output "Verified graphics144 ${c}: $($sdk.files.Count) configuration members;144 SDK members total"}}
