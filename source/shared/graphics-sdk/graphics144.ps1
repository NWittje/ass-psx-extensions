# Compatibility entry point for existing caller names; the exact edition is
# selected once in current.ps1. Historical build stamps keep their own pins.
. (Join-Path $PSScriptRoot 'current.ps1')
function Get-PsxGraphics144Sdk {
 param([Parameter(Mandatory)][string]$SdkRoot,[Parameter(Mandatory)][ValidateSet('Debug','Release')][string]$Configuration)
 Get-PsxCurrentGraphicsSdk $SdkRoot $Configuration
}