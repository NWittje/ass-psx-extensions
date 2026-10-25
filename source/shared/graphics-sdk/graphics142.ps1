# Complete qualified SDK identity; a verified relocation keeps the same edition.
$script:PsxGraphics142Pin=Join-Path $PSScriptRoot 'graphics142-files.json'
function Get-PsxGraphics142Sdk {
 param([Parameter(Mandatory)][string]$SdkRoot,[Parameter(Mandatory)][ValidateSet('Debug','Release')][string]$Configuration)
 if(![IO.Path]::IsPathFullyQualified($SdkRoot)-or$SdkRoot-match'(^|[\\/])\.\.?([\\/]|$)'){throw 'Absolute ordinary SDK root required'}
 $root=[IO.Path]::GetFullPath($SdkRoot).TrimEnd('\','/')
 $pin=$script:PsxGraphics142Pin
 if((Get-FileHash -LiteralPath $pin).Hash-cne'A1E542D6ED60351CB6E2A09667E841CACC855FB66188F8917B27E2F4426CFD63'){throw 'Qualified graphics142 member pin differs'}
 $members=@(Get-Content -LiteralPath $pin -Raw|ConvertFrom-Json)
 if($members.Count-ne36){throw 'Complete36-member graphics142 SDK required'}
 for($p=$root;$p;$p=[IO.Path]::GetDirectoryName($p)){if((Test-Path -LiteralPath $p)-and((Get-Item -LiteralPath $p -Force).Attributes-band[IO.FileAttributes]::ReparsePoint)){throw 'SDK ancestry contains a reparse point'}}
 $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
 $files=[Collections.Generic.List[object]]::new()
 foreach($member in $members){
  $relative=[string]$member.path
  if(!$relative-or[IO.Path]::IsPathRooted($relative)-or$relative-match'(^|[\\/])\.\.?([\\/]|$)|:'-or!$seen.Add($relative)){throw 'Invalid graphics142 SDK member'}
  $path=Join-Path $root $relative;$file=Get-Item -LiteralPath $path -Force
  if($file.PSIsContainer-or($file.Attributes-band[IO.FileAttributes]::ReparsePoint)-or$file.Length-ne$member.bytes-or(Get-FileHash -LiteralPath $path).Hash-cne$member.sha256){throw "SDK member differs: $path"}
  $files.Add([pscustomobject]@{relative=$relative;path=$path;origin=$path;bytes=$file.Length;sha256=$member.sha256})
 }
 $actual=@(Get-ChildItem -LiteralPath $root -File -Recurse -Force)
 if($actual.Count-ne36){throw 'Complete graphics142 SDK roster differs'}
 foreach($file in $actual){if(!$seen.Contains([IO.Path]::GetRelativePath($root,$file.FullName))){throw 'Unlisted graphics142 SDK member'}}
 foreach($directory in Get-ChildItem -LiteralPath $root -Directory -Recurse -Force){if($directory.Attributes-band[IO.FileAttributes]::ReparsePoint){throw 'Linked graphics142 SDK directory'}}
 $status=Get-Content -LiteralPath (Join-Path $root 'BUILD-STATUS.json') -Raw|ConvertFrom-Json
 if($status.version-cne'1.4.2.0'-or$status.abi-ne1-or$status.state-cne'BUILT_NOT_RUNTIME_TESTED'){throw 'Graphics142 producer edition/status differs'}
 $configurationRoot=Join-Path $root $Configuration
 $version=Get-Content -LiteralPath (Join-Path $configurationRoot 'lib/cmake/AssGraphics/AssGraphicsConfigVersion.cmake') -Raw
 if($version-notmatch'PACKAGE_VERSION "1\.4\.2"'){throw 'Graphics142 CMake version differs'}
 $info=(Get-Item -LiteralPath (Join-Path $configurationRoot 'bin/ass_graphics_lib.dll') -Force).VersionInfo
 if($info.FileVersion-cne'1.4.2.0'-or$info.ProductName-cne'ASS Graphics Lib'){throw 'Graphics142 Windows metadata differs'}
 [pscustomobject]@{profile='Graphics142';version='1.4.2';abi=1;configuration=$Configuration;root=$root;configurationRoot=$configurationRoot;origin=$root;vectorPath=$pin;manifestSHA256=(Get-FileHash -LiteralPath $pin).Hash;files=$files.ToArray();producerState=$status.state;originalProducerRoot='O:\psx\bin\.work\ass-graphics-choice-native-sdk-20261018\sdk';locationProof='All36 pinned members, exact roster, CMake/PE edition and original producer status; original path is provenance only'}
}
