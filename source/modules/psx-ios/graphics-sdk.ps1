function Resolve-IosGraphicsSdk([string]$Root) {
    if(-not $Root){throw 'Set ASS_GRAPHICS_SDK_ROOT to the selected ASS Graphics SDK.'}
    $Root=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    $ancestor=$Root
    while($ancestor){if((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Graphics SDK traverses a reparse point.'};$ancestor=[IO.Path]::GetDirectoryName($ancestor)}
    foreach($entry in Get-ChildItem -LiteralPath $Root -Recurse -Force){if($entry.Attributes-band[IO.FileAttributes]::ReparsePoint){throw 'Graphics SDK contains a reparse point.'}}
    $version=$null;$files=[Collections.Generic.List[string]]::new()
    foreach($cfg in 'Debug','Release'){
        $versionFile=Join-Path $Root "$cfg/lib/cmake/AssGraphics/AssGraphicsConfigVersion.cmake"
        $text=[IO.File]::ReadAllText($versionFile)
        $match=[regex]::Match($text,'set\(PACKAGE_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"\)')
        if(-not$match.Success){throw 'ASS Graphics SDK version metadata unavailable.'}
        $observed=[version]$match.Groups[1].Value
        if($observed-ne[version]'1.4.4'){throw 'Exact ASS Graphics 1.4.4 SDK is required.'}
        if($version-and$version-ne$observed){throw 'Graphics Debug/Release SDK versions differ.'};$version=$observed
        foreach($relative in @('bin/ass_graphics_lib.dll','lib/ass_graphics_lib.lib','symbols/ass_graphics_lib.pdb','include/ass_graphics/ass_graphics.h','include/ass_graphics/graphics.hpp','licenses/B612-OFL.txt','licenses/NOTICE.txt','lib/cmake/AssGraphics/AssGraphicsConfig.cmake','lib/cmake/AssGraphics/AssGraphicsConfigVersion.cmake')){
            $path=Join-Path $Root "$cfg/$relative";if(-not[IO.File]::Exists($path)){throw "Graphics SDK input unavailable: $path"};$files.Add($path)
        }
        $info=[Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $Root "$cfg/bin/ass_graphics_lib.dll"))
        if($info.ProductName-cne'ASS Graphics Lib'-or$info.FileMajorPart-ne$version.Major-or$info.FileMinorPart-ne$version.Minor-or$info.FileBuildPart-ne$version.Build){throw 'Graphics DLL version differs from its SDK.'}
    }
    [pscustomobject]@{Root=$Root;Version=$version.ToString();Files=$files.ToArray()}
}
function Get-IosBuildGraphicsSdk([string]$BuildDirectory,[string]$Root) {
    $sdk=Resolve-IosGraphicsSdk $Root
    $cache=[IO.File]::ReadAllText((Join-Path $BuildDirectory 'CMakeCache.txt'))
    $match=[regex]::Match($cache,'(?m)^ASS_GRAPHICS_SDK_ROOT:PATH=([^\r\n]+)')
    if(-not$match.Success-or[IO.Path]::GetFullPath($match.Groups[1].Value).TrimEnd('\','/')-ine$sdk.Root){throw 'Publisher SDK differs from the SDK selected by the native build.'}
    $sdk
}