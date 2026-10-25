# Build-time import of the supplied drawing. Geometry is untouched; names and paint are scoped.
function Get-AircraftDamageSvg {
 param([string]$InputPath=(Join-Path $PSScriptRoot '../../external/747-400-top-front.svg'),
       [string]$CataloguePath=(Join-Path $PSScriptRoot '../../shared/psx-instructor/src/failure_catalogue.inc'),
       [string]$Instance='')
 [xml]$drawing=[IO.File]::ReadAllText($InputPath)
 $svg=$drawing.DocumentElement
 $ns='http://www.w3.org/2000/svg'
 if($svg.NamespaceURI -ne $ns -or $svg.GetAttribute('viewBox') -ne '0 0 612 792'){throw 'Unexpected aircraft SVG coordinates'}
 $component=$drawing.SelectSingleNode('//*[@id="Component_layers_x2F_groups"]')
 if(-not $component){throw 'Aircraft SVG component layer missing'}
 $counts=@{}
 foreach($shape in @($component.ChildNodes | Where-Object NodeType -eq Element)){
  $name=$shape.GetAttribute('id').Replace('_x5F_','_')
  $region=switch -Regex ($name) {
   '^Engine_(Top|Front)_([1-4])$' {'engine-'+$Matches[2];break}
   '^NG_([1-4])$' {'gear-nose';break}
   '^MLG_([LR])_([1-6])$' {'gear-wing-'+$(if($Matches[1] -eq 'L'){'left'}else{'right'});break}
   '^MBG_([LR])_([1-6])$' {'gear-body-'+$(if($Matches[1] -eq 'L'){'left'}else{'right'});break}
   '^Aileron_[LR]$' {'aileron';break}
   '^Elevator_[LR]$' {'elevator';break}
   '^Stabiliser_[LR]$' {'stabilizer';break}
   '^Rudder$' {'rudder';break}
   default {throw "Unmapped aircraft shape: $name"}
  }
  $shape.SetAttribute('data-damage-region',$region)
  $shape.SetAttribute('data-damage-state','unavailable')
  $counts[$region]=1+$counts[$region]
  $title=$drawing.CreateElement('title',$ns)
  $title.InnerText=$region+' — configured status unavailable'
  [void]$shape.AppendChild($title)
 }
 if($counts.Count -ne 13 -or ($counts.Values|Measure-Object -Sum).Sum -ne 43){throw 'Aircraft SVG component count changed'}
 foreach($node in @($drawing.SelectNodes('//*'))){
  if($node.HasAttribute('id')){$node.SetAttribute('id','damage-'+$node.GetAttribute('id'))}
  if($node.HasAttribute('class')){
   $node.SetAttribute('class',(($node.GetAttribute('class') -split '\s+'|ForEach-Object {'damage-'+$_}) -join ' '))
  }
 }
 foreach($style in @($drawing.SelectNodes('//*[local-name()="style"]'))){[void]$style.ParentNode.RemoveChild($style)}
 $svg.SetAttribute('id','aircraft-damage')
 $svg.SetAttribute('preserveAspectRatio','xMidYMid meet')
 $svg.SetAttribute('role','img')
 $svg.SetAttribute('aria-label','Boeing 747-400 top and front views; configured failure status unavailable')
 $svg.SetAttribute('aria-describedby','aircraft-damage-summary')
 $defs=$drawing.SelectSingleNode('/*[local-name()="svg"]/*[local-name()="defs"]')
 $pattern=$drawing.CreateElement('pattern',$ns)
 foreach($a in @{id='damage-now-pattern';patternUnits='userSpaceOnUse';width='12';height='12'}.GetEnumerator()){$pattern.SetAttribute($a.Key,$a.Value)}
 $background=$drawing.CreateElement('rect',$ns)
 foreach($a in @{width='12';height='12';fill='#f8d3cf'}.GetEnumerator()){$background.SetAttribute($a.Key,$a.Value)}
 [void]$pattern.AppendChild($background)
 $chevrons=$drawing.CreateElement('path',$ns)
 foreach($a in @{d='M-6 0L0 6 -6 12M0 0L6 6 0 12M6 0L12 6 6 12';fill='none';stroke='#b42318';'stroke-width'='3'}.GetEnumerator()){$chevrons.SetAttribute($a.Key,$a.Value)}
 [void]$pattern.AppendChild($chevrons);[void]$defs.AppendChild($pattern)
 # Reuse the authored native catalogue for fault/variant labels. It is data, not an executed PSX class.
 $records=@(foreach($line in [IO.File]::ReadAllLines($CataloguePath)){
  if($line -match '^\{\d+,'){
   $fields=ConvertFrom-Json ($line -replace '^\{','[' -replace '\},\s*$',']')
   [ordered]@{id=$fields[0];name=$fields[1];severe=$fields[3];non_severe=$fields[4]}
  }
 })
 if($records.Count -ne 193){throw 'Native failure catalogue count changed'}
 $metadata=$drawing.CreateElement('metadata',$ns)
 $metadata.SetAttribute('id','damage-catalogue')
 $metadata.InnerText=ConvertTo-Json -InputObject $records -Depth 3 -Compress
 [void]$svg.AppendChild($metadata)
 if($Instance){
  foreach($node in @($drawing.SelectNodes('//*'))){
   if($node.HasAttribute('id')){$node.SetAttribute('id',$Instance+'-'+$node.GetAttribute('id'))}
   if($node.HasAttribute('class')){$node.SetAttribute('class',(($node.GetAttribute('class') -split '\s+'|ForEach-Object {$Instance+'-'+$_}) -join ' '))}
  }
  $svg.SetAttribute('aria-describedby',$Instance+'-aircraft-damage-summary')
 }
 $svg.OuterXml
}
