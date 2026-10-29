# One qualified producer edition for every current Windows graphics consumer.
function Get-PsxCurrentGraphicsSdk {
 param([Parameter(Mandatory)][string]$SdkRoot,[Parameter(Mandatory)][ValidateSet('Debug','Release')][string]$Configuration)
 $manifestSHA256='283733A416CDA732A33FDC865FD480E4872813C3D2036F2F3753E6ECC35137D1'
 if(![IO.Path]::IsPathFullyQualified($SdkRoot)-or$SdkRoot-match'(^|[\\/])\.\.?([\\/]|$)'){throw 'Absolute ordinary SDK root required'}
 $root=[IO.Path]::GetFullPath($SdkRoot).TrimEnd('\','/')
 for($ancestor=$root;$ancestor;$ancestor=[IO.Path]::GetDirectoryName($ancestor)){if((Test-Path -LiteralPath $ancestor)-and((Get-Item -LiteralPath $ancestor -Force).Attributes-band[IO.FileAttributes]::ReparsePoint)){throw 'Linked SDK ancestry'}}
 $pin=Join-Path $root 'producer-file-manifest.json'
 if((Get-FileHash -LiteralPath $pin).Hash-cne$manifestSHA256){throw 'Qualified current graphics producer manifest differs'}
 $manifest=Get-Content -LiteralPath $pin -Raw|ConvertFrom-Json
 if($manifest.version-cne'1.4.5'-or$manifest.abi-ne1-or@($manifest.files).Count-ne104){throw 'GraphicsCurrent producer edition/status differs'}
 $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
 $files=[Collections.Generic.List[object]]::new()
 foreach($member in $manifest.files){
  $relative=[string]$member.path
  if(!$relative-or[IO.Path]::IsPathRooted($relative)-or$relative-match'(^|[\\/])\.\.?([\\/]|$)|:'-or!$seen.Add($relative)){throw 'Invalid current graphics SDK member'}
  $path=Join-Path $root $relative;$file=Get-Item -LiteralPath $path -Force
  if($file.PSIsContainer-or($file.Attributes-band[IO.FileAttributes]::ReparsePoint)-or$file.Length-ne$member.bytes-or(Get-FileHash -LiteralPath $path).Hash-cne$member.sha256){throw "SDK member differs: $path"}
  $files.Add([pscustomobject]@{relative=$relative;path=$path;origin=$path;bytes=$file.Length;sha256=$member.sha256})
 }
 $seen.Add('producer-file-manifest.json')|Out-Null
 $files.Add([pscustomobject]@{relative='producer-file-manifest.json';path=$pin;origin=$pin;bytes=(Get-Item -LiteralPath $pin).Length;sha256=$manifestSHA256})
 $actual=@(Get-ChildItem -LiteralPath $root -File -Recurse -Force)
 if($actual.Count-ne$files.Count){throw 'GraphicsCurrent SDK roster differs'}
 foreach($file in $actual){if(!$seen.Contains([IO.Path]::GetRelativePath($root,$file.FullName))){throw 'Unlisted current graphics SDK member'}}
 foreach($directory in Get-ChildItem -LiteralPath $root -Directory -Recurse -Force){if($directory.Attributes-band[IO.FileAttributes]::ReparsePoint){throw 'Linked current graphics SDK directory'}}
 $configurationRoot=Join-Path $root $Configuration
 if((Get-Content -LiteralPath "$configurationRoot/lib/cmake/AssGraphics/AssGraphicsConfigVersion.cmake" -Raw)-notmatch'PACKAGE_VERSION "1\.4\.5"'){throw 'GraphicsCurrent CMake version differs'}
 $info=(Get-Item -LiteralPath "$configurationRoot/bin/ass_graphics_lib.dll").VersionInfo
 if($info.FileVersion-cne'1.4.5.0'-or$info.ProductName-cne'ASS Graphics Lib'){throw 'GraphicsCurrent Windows metadata differs'}
 [pscustomobject]@{profile='GraphicsCurrent';version='1.4.5';abi=1;configuration=$Configuration;root=$root;configurationRoot=$configurationRoot;origin=$root;vectorPath=$pin;manifestSHA256=$manifestSHA256;files=$files.ToArray();producerState='Pinned producer SDK; native-symbol authority and consumer qualification recorded separately';locationProof='All104 pinned producer members plus manifest, exact roster and CMake/PE edition; consumer qualification remains separate'}
}