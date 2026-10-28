# Shared exact SDK qualification; verified source locations are interchangeable.
. (Join-Path $PSScriptRoot '../../../shared/graphics-sdk/graphics144.ps1')
function Get-DuGraphics144Sdk {
 param([Parameter(Mandatory)][string]$SdkRoot,[Parameter(Mandatory)][ValidateSet('Debug','Release')][string]$Configuration)
 Get-PsxGraphics144Sdk $SdkRoot $Configuration
}
function Get-DuGraphics144Origin {
 param([Parameter(Mandatory)][string]$Path,[Parameter(Mandatory)]$Sdk)
 $full=[IO.Path]::GetFullPath($Path)
 if($full-ieq$Sdk.vectorPath){return $Sdk.vectorPath}
 $member=@($Sdk.files|Where-Object{[IO.Path]::GetFullPath($_.path)-ieq$full})
 if($member.Count-ne1){throw 'Input is not an exact qualified graphics144 member'}
 $member[0].origin
}
