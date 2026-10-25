param(
    [string]$AircraftDirectory='D:\X-Plane12\Aircraft\747-400',
    [string]$ManifestPath=(Join-Path $PSScriptRoot '..\assets\sparky-audio-manifest.json')
)
$ErrorActionPreference='Stop'
$directory=(Resolve-Path -LiteralPath $AircraftDirectory).Path
$manifest=Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
function Get-BytesHash([byte[]]$Bytes) {
    $hash=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($Bytes))).Replace('-','') }
    finally { $hash.Dispose() }
}
function Get-FileIdentityHash([string]$Path) {
    # Sound banks can be large; keep hashing bounded and independent of shell modules.
    $stream=[IO.File]::OpenRead($Path)
    $hash=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-','') }
    finally { $hash.Dispose(); $stream.Dispose() }
}
function Assert-Identity([string]$Path,$Identity) {
    $file=Get-Item -LiteralPath $Path
    if($file.Length -ne $Identity.bytes -or (Get-FileIdentityHash $Path) -ne $Identity.sha256) {
        throw "Installed source identity changed: $Path"
    }
}
$source=Join-Path $directory '747-400.acf'
Assert-Identity $source $manifest.source_acf
Assert-Identity (Join-Path $directory 'fmod\747-400.snd') $manifest.source_snd
$names=@()
foreach($bank in $manifest.banks) {
    $name=[IO.Path]::GetFileName($bank.path)
    if(-not $name -or $name -ne [IO.Path]::GetFileName($name)) {throw 'Invalid bank name'}
    Assert-Identity (Join-Path (Join-Path $directory 'fmod') $name) $bank
    $names+=$name
}
# Latin-1 is a reversible byte mapping; only the ASCII aircraft name suffix changes.
$encoding=[Text.Encoding]::GetEncoding(28591)
$original=$encoding.GetString([IO.File]::ReadAllBytes($source))
$pattern='(?m)^(P acf/_name[^\r\n]*)'
if([regex]::Matches($original,$pattern).Count -ne 1) {throw 'Expected one ACF name'}
[byte[]]$acf=$encoding.GetBytes([regex]::Replace($original,$pattern,'$1 PSX'))
$snd="A`n1000`nACF_SOUNDS`n# PSX-owned instances; native attachments intentionally absent.`n"
$bankNames=[string[]]$names
[Array]::Sort($bankNames,[StringComparer]::Ordinal)
foreach($name in $bankNames) {
    if(-not $name.StartsWith('Master Bank')) {$snd+="REQUIRES_BANK $name`n"}
}
$snd+="DISABLE_LEGACY_ALERT_SOUNDS`n"
foreach($space in $manifest.sound_spaces) {$snd+=$space+"`n"}
[byte[]]$sndBytes=[Text.Encoding]::UTF8.GetBytes($snd)
if((Get-BytesHash $acf) -ne $manifest.psx_acf_sha256 -or (Get-BytesHash $sndBytes) -ne $manifest.psx_snd_sha256) {
    throw 'Derived adapter does not match the reviewed manifest'
}
$targets=@(
    @{Path=(Join-Path $directory '747-400-PSX.acf');Bytes=$acf;Hash=$manifest.psx_acf_sha256},
    @{Path=(Join-Path $directory 'fmod\747-400-PSX.snd');Bytes=$sndBytes;Hash=$manifest.psx_snd_sha256}
)
foreach($target in $targets) {
    if((Test-Path -LiteralPath $target.Path) -and (Get-FileIdentityHash $target.Path) -ne $target.Hash) {
        throw "Refusing to overwrite a different existing adapter: $($target.Path)"
    }
}
foreach($target in $targets) {
    [IO.File]::WriteAllBytes($target.Path,$target.Bytes)
    if((Get-FileIdentityHash $target.Path) -ne $target.Hash) {throw "Write verification failed: $($target.Path)"}
    [pscustomobject]@{Path=$target.Path;Bytes=$target.Bytes.Length;SHA256=$target.Hash}
}
