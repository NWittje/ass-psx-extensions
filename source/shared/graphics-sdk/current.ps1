# One qualified native input edition for current Windows graphics consumers.
# Historical build stamps retain their original producer identities.
function Get-PsxCurrentGraphicsEdition {
 [pscustomobject]@{
  version='1.8.0'; abi=1
  manifest='consumer-file-manifest.json'
  manifestSHA256='1634C2BB4AF2C996B83D1DBDDBA10FAAAADEC5630CDD892262A1B761EBD6B085'
  producerManifestSHA256='56F896950A479FE514EA08B928C77FECC4B754700BF053072B73DE29C7F6F995'
 }
}

function Get-PsxCurrentGraphicsSdk {
 param([Parameter(Mandatory)][string]$SdkRoot,[Parameter(Mandatory)][ValidateSet('Debug','Release')][string]$Configuration)
 $edition=Get-PsxCurrentGraphicsEdition
 if(![IO.Path]::IsPathFullyQualified($SdkRoot)-or$SdkRoot-match'(^|[\\/])\.\.?([\\/]|$)'){throw 'Absolute ordinary SDK root required'}
 $root=[IO.Path]::GetFullPath($SdkRoot).TrimEnd('\','/')
 for($ancestor=$root;$ancestor;$ancestor=[IO.Path]::GetDirectoryName($ancestor)){
  if((Test-Path -LiteralPath $ancestor)-and((Get-Item -LiteralPath $ancestor -Force).Attributes-band[IO.FileAttributes]::ReparsePoint)){throw 'Linked SDK ancestry'}
 }
 $pin=Join-Path $root $edition.manifest
 if((Get-FileHash -LiteralPath $pin).Hash-cne$edition.manifestSHA256){throw 'Qualified current graphics input manifest differs'}
 $manifest=Get-Content -LiteralPath $pin -Raw|ConvertFrom-Json
 if($manifest.schema-ne1-or$manifest.kind-cne'psx-native-consumer-pair'-or$manifest.version-cne$edition.version-or$manifest.abi-ne$edition.abi-or
    $manifest.producerSDKManifestSHA256-cne$edition.producerManifestSHA256-or-not$manifest.files){throw 'GraphicsCurrent input edition/status differs'}
 # This native pair is a consumer mirror. Original producer indices are copied
 # unchanged alongside it; no new graphics producer identity is asserted.
 $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
 $files=[Collections.Generic.List[object]]::new()
 foreach($member in $manifest.files){
  $relative=([string]$member.path).Replace('\','/')
  if(!$relative-or[IO.Path]::IsPathRooted($relative)-or$relative-match'(^|[\\/])\.\.?([\\/]|$)|:'-or!$seen.Add($relative)){throw 'Invalid current graphics SDK member'}
  $path=Join-Path $root $relative;$file=Get-Item -LiteralPath $path -Force
  if($file.PSIsContainer-or($file.Attributes-band[IO.FileAttributes]::ReparsePoint)-or$file.Length-ne$member.bytes-or(Get-FileHash -LiteralPath $path).Hash-cne$member.sha256){throw "SDK member differs: $path"}
  $files.Add([pscustomobject]@{relative=$relative;path=$path;origin=$path;bytes=$file.Length;sha256=$member.sha256})
 }
 $seen.Add($edition.manifest)|Out-Null
 $files.Add([pscustomobject]@{relative=$edition.manifest;path=$pin;origin=$pin;bytes=(Get-Item -LiteralPath $pin).Length;sha256=$edition.manifestSHA256})
 $actual=@(Get-ChildItem -LiteralPath $root -File -Recurse -Force)
 if($actual.Count-ne$files.Count){throw 'GraphicsCurrent SDK roster differs'}
 foreach($file in $actual){if(!$seen.Contains([IO.Path]::GetRelativePath($root,$file.FullName).Replace('\','/'))){throw 'Unlisted current graphics SDK member'}}
 foreach($directory in Get-ChildItem -LiteralPath $root -Directory -Recurse -Force){if($directory.Attributes-band[IO.FileAttributes]::ReparsePoint){throw 'Linked current graphics SDK directory'}}
 $configurationRoot=Join-Path $root $Configuration
 $versionPattern='PACKAGE_VERSION\s+"'+[regex]::Escape($edition.version)+'"'
 if((Get-Content -LiteralPath "$configurationRoot/lib/cmake/AssGraphics/AssGraphicsConfigVersion.cmake" -Raw)-notmatch$versionPattern){throw 'GraphicsCurrent CMake version differs'}
 $info=(Get-Item -LiteralPath "$configurationRoot/bin/ass_graphics_lib.dll").VersionInfo
 if($info.FileVersion-cne($edition.version+'.0')-or$info.ProductName-cne'ASS Graphics Lib'){throw 'GraphicsCurrent Windows metadata differs'}
 [pscustomobject]@{profile='GraphicsCurrent';version=$edition.version;abi=$edition.abi;configuration=$Configuration;root=$root;configurationRoot=$configurationRoot;origin=$root;
  vectorPath=$pin;manifestSHA256=$edition.manifestSHA256;files=$files.ToArray();
  producerState='Pinned native consumer mirror with unchanged producer indices; symbol authority and application qualification remain separate';
  locationProof='Exact selected-file roster, sizes/SHA256 and CMake/PE edition; no runtime font gate or application acceptance implied'}
}