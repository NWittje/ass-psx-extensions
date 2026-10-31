function Get-IosMonitorSdk([string]$Root) {
    if(-not [IO.Path]::IsPathFullyQualified($Root)){throw 'Select an absolute XPL Monitor client SDK root.'}
    $Root=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    for($p=$Root;$p;$p=[IO.Path]::GetDirectoryName($p)){
        if((Test-Path -LiteralPath $p)-and((Get-Item -LiteralPath $p -Force).Attributes-band[IO.FileAttributes]::ReparsePoint)){throw 'Linked monitor SDK path.'}
    }
    $path=Join-Path $Root 'package-manifest.json'
    $hash='7A68D08E557BC60AEB79EAFD6E70ED8321B12422FAF604C74637F9A589B32B38'
    if((Get-FileHash -LiteralPath $path).Hash-cne$hash){throw 'Unqualified XPL Monitor client SDK manifest.'}
    $manifest=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    if($manifest.Version-cne'1.0.0'-or$manifest.CRT-cne'MD/MDd'){throw 'XPL Monitor client compiler/CRT edition differs.'}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $files=[Collections.Generic.List[object]]::new()
    foreach($row in $manifest.Files){
        $relative=[string]$row.Path
        if(-not$relative-or[IO.Path]::IsPathRooted($relative)-or$relative-match'(^|[\\/])\.\.?([\\/]|$)|:'-or-not$seen.Add($relative)){throw 'Invalid monitor SDK member.'}
        $member=Join-Path $Root $relative;$item=Get-Item -LiteralPath $member -Force
        if($item.PSIsContainer-or($item.Attributes-band[IO.FileAttributes]::ReparsePoint)-or$item.Length-ne$row.Bytes-or(Get-FileHash -LiteralPath $member).Hash-ine$row.SHA256){throw "Monitor SDK member changed: $relative"}
        $files.Add([pscustomobject]@{path=$member;bytes=$item.Length;sha256=$row.SHA256})
    }
    foreach($directory in Get-ChildItem -LiteralPath $Root -Directory -Recurse -Force){if($directory.Attributes-band[IO.FileAttributes]::ReparsePoint){throw 'Linked monitor SDK directory.'}}
    if(@(Get-ChildItem -LiteralPath $Root -File -Recurse -Force).Count-ne$files.Count+1){throw 'Monitor SDK roster differs.'}
    $files.Add([pscustomobject]@{path=$path;bytes=(Get-Item -LiteralPath $path).Length;sha256=$hash})
    [pscustomobject]@{Root=$Root;Version=$manifest.Version;ManifestSHA256=$hash;Files=$files.ToArray()}
}
