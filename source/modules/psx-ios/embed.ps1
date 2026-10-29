param([Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'aircraft-svg.ps1')
. (Join-Path $PSScriptRoot 'traffic-svg.ps1')
$drawing=Get-AircraftDamageSvg
$statusDrawing=Get-AircraftDamageSvg -Instance status
$traffic=Get-TrafficAircraftSymbols
$assets=@(@('/','text/html; charset=utf-8','web/index.html'),@('/ios.css','text/css; charset=utf-8','web/ios.css'),@('/ios.js','application/javascript; charset=utf-8','web/ios.js'),@('/environment.js','application/javascript; charset=utf-8','web/environment.js'),@('/environment.css','text/css; charset=utf-8','web/environment.css'))
$assets+=,@('/non_normal.js','application/javascript; charset=utf-8','web/non_normal.js')
$assets+=,@('/non_normal.css','text/css; charset=utf-8','web/non_normal.css')
$assets+=,@('/weight_service.js','application/javascript; charset=utf-8','web/weight_service.js')
$assets+=,@('/weight_service.css','text/css; charset=utf-8','web/weight_service.css')
$assets+=,@('/status.js','application/javascript; charset=utf-8','web/status.js')
$assets+=,@('/status.css','text/css; charset=utf-8','web/status.css')
$assets+=,@('/situations.js','application/javascript; charset=utf-8','web/situations.js')
$assets+=,@('/situations.css','text/css; charset=utf-8','web/situations.css')
$assets+=,@('/aircraft_damage.js','application/javascript; charset=utf-8','web/aircraft_damage.js')
$assets+=,@('/themes.css','text/css; charset=utf-8','web/themes.css')
$assets+=,@('/reposition_graphic.js','application/javascript; charset=utf-8','web/reposition_graphic.js')
$assets+=,@('/controls.js','application/javascript; charset=utf-8','web/controls.js')
$assets+=,@('/altitude.js','application/javascript; charset=utf-8','web/altitude.js')
$assets+=,@('/terrain_preflight.js','application/javascript; charset=utf-8','web/terrain_preflight.js')
$text=[Text.StringBuilder]::new();[void]$text.AppendLine('#include <ios/web.hpp>')
$index=0
foreach($asset in $assets){$bytes=[IO.File]::ReadAllBytes((Join-Path $PSScriptRoot $asset[2]));if($asset[2] -eq 'web/index.html'){$html=[Text.Encoding]::UTF8.GetString($bytes);if(-not $html.Contains('<!-- AIRCRAFT_DAMAGE_SVG -->')){throw 'Aircraft SVG placeholder missing'};if(-not $html.Contains('<!-- TRAFFIC_AIRCRAFT_SYMBOLS -->')){throw 'Traffic SVG placeholder missing'};$html=$html.Replace('<!-- AIRCRAFT_DAMAGE_SVG -->',$drawing).Replace('<!-- TRAFFIC_AIRCRAFT_SYMBOLS -->',$traffic).Replace('<!-- STATUS_AIRCRAFT_DAMAGE_SVG -->',$statusDrawing);$bytes=[Text.Encoding]::UTF8.GetBytes($html)};[void]$text.AppendLine(('static const unsigned char a'+$index+'[]={'));for($i=0;$i -lt $bytes.Length;$i+=128){$end=[Math]::Min($i+127,$bytes.Length-1);[void]$text.AppendLine((($bytes[$i..$end]|ForEach-Object {$_.ToString()}) -join ',')+',')};[void]$text.AppendLine('};');$index++}
[void]$text.AppendLine('namespace ios { const std::vector<Asset>& assets(){static const std::vector<Asset> a{')
for($i=0;$i -lt $assets.Count;$i++){[void]$text.AppendLine(('{{"{0}","{1}",a{2},sizeof(a{2})}},' -f $assets[$i][0],$assets[$i][1],$i))}
[void]$text.AppendLine('};return a;}}')
[IO.File]::WriteAllText($Output,$text.ToString(),[Text.UTF8Encoding]::new($false))
