# Build-time import only. The supplied drawings keep their original geometry;
# cropped view boxes remove the Illustrator page, including the 744 page overflow.
function Get-TrafficAircraftFill($root) {
    # The exports contain fragmented strokes and triangles, not closed aircraft
    # bodies. Build one filled side-profile envelope beneath the untouched ink.
    # Sweep source vertices; this happens at build time, never during polling.
    $culture=[Globalization.CultureInfo]::InvariantCulture
    $parts=[Collections.Generic.List[object]]::new()
    $minimum=[double]::PositiveInfinity;$maximum=[double]::NegativeInfinity
    foreach($node in @($root.SelectNodes('.//*[local-name()="polygon" or local-name()="polyline"]'))){
        $values=@([regex]::Matches($node.GetAttribute('points'),'[-+]?[0-9]*\.?[0-9]+')|ForEach-Object{[double]::Parse($_.Value,$culture)})
        $points=[Collections.Generic.List[object]]::new()
        for($i=0;$i -lt $values.Count;$i+=2){$points.Add(@($values[$i],$values[$i+1]))}
        $parts.Add(@{Points=$points;Closed=$node.LocalName -eq 'polygon'})
        if($node.LocalName -eq 'polygon'){foreach($p in $points){$minimum=[Math]::Min($minimum,$p[0]);$maximum=[Math]::Max($maximum,$p[0])}}
    }
    $edges=[Collections.Generic.List[object]]::new();$xs=[Collections.Generic.HashSet[double]]::new()
    foreach($part in $parts){
        $points=$part.Points
        $count=$points.Count-$(if($part.Closed){0}else{1})
        for($i=0;$i -lt $count;$i++){
            $a=$points[$i];$b=$points[($i+1)%$points.Count]
            if($a[0] -lt $minimum -or $b[0] -lt $minimum -or $a[0] -gt $maximum -or $b[0] -gt $maximum){continue}
            if($a[0] -gt $b[0]){$swap=$a;$a=$b;$b=$swap}
            $edges.Add(@{Left=$a[0];Right=$b[0];Y=$a[1];EndY=$b[1];Slope=$(if($a[0] -eq $b[0]){0}else{($b[1]-$a[1])/($b[0]-$a[0])})})
            [void]$xs.Add($a[0]);[void]$xs.Add($b[0])
        }
    }
    $ordered=@($edges|Sort-Object { $_.Left });$active=[Collections.Generic.List[object]]::new();$next=0
    $top=[Collections.Generic.List[object]]::new();$bottom=[Collections.Generic.List[object]]::new()
    foreach($x in @($xs|Sort-Object)){
        while($next -lt $ordered.Count -and $ordered[$next].Left -le $x){$active.Add($ordered[$next]);$next++}
        $lo=[double]::PositiveInfinity;$hi=[double]::NegativeInfinity
        for($i=$active.Count-1;$i -ge 0;$i--){
            $e=$active[$i]
            if($e.Right -lt $x){$active.RemoveAt($i);continue}
            $y=$e.Y+($x-$e.Left)*$e.Slope
            $lo=[Math]::Min($lo,$y);$hi=[Math]::Max($hi,$y)
            if($e.Left -eq $e.Right){$lo=[Math]::Min($lo,$e.EndY);$hi=[Math]::Max($hi,$e.EndY)}
        }
        if([double]::IsInfinity($lo)){throw "Traffic profile is not continuous at $x ($($active.Count) edges, $next/$($ordered.Count))"}
        $top.Add(@($x,$lo));$bottom.Add(@($x,$hi))
    }
    if($top.Count -lt 3){throw 'Traffic profile has no closed body'}
    $path=[Text.StringBuilder]::new('M')
    foreach($p in $top){[void]$path.Append($p[0].ToString('0.########',$culture)+' '+$p[1].ToString('0.########',$culture)+'L')}
    for($i=$bottom.Count-1;$i -ge 0;$i--){$p=$bottom[$i];[void]$path.Append($p[0].ToString('0.########',$culture)+' '+$p[1].ToString('0.########',$culture)+'L')}
    $path.Length--
    [void]$path.Append('Z')
    return $path.ToString()
}
function Get-TrafficAircraftSymbols {
    $models=@(
        @{Name='744-right';File='744_sideview_facing_right.svg';Box='-13.2 38.77 687.39 136.8'},
        @{Name='738-left';File='738_sideview_facing_left.svg';Box='22.66 22.29 567.15 181.09'},
        @{Name='738-right';File='738_sideview_facing_right.svg';Box='22.66 22.29 567.15 181.09'}
    )
    $result=[Text.StringBuilder]::new()
    foreach($model in $models){
        $path=Join-Path $PSScriptRoot ('../../external/'+$model.File)
        $xml=[xml][IO.File]::ReadAllText($path)
        $root=$xml.DocumentElement
        if($root.LocalName -ne 'svg' -or $root.GetAttribute('viewBox') -ne '0 0 612 792'){throw 'Unexpected traffic artwork page'}
        $fill=Get-TrafficAircraftFill $root
        $prefix='traffic-'+$model.Name+'-'
        # No generic Illustrator selectors or duplicate IDs enter the document.
        foreach($node in @($root.SelectNodes('.//*[local-name()="style"]'))){
            $style=$node.InnerText
            $style=[regex]::Replace($style,'\.st([0-9]+)',('.'+$prefix+'st$1'))
            $style=[regex]::Replace($style,'(?i)(fill|stroke)\s*:\s*(blue|gray|#000)\s*;', '$1: currentColor;')
            $style=[regex]::Replace($style,'(?i)(stroke-width\s*:\s*)([0-9]*\.?[0-9]+)(px)?',{
                param($match)
                $width=[double]::Parse($match.Groups[2].Value,[Globalization.CultureInfo]::InvariantCulture)*1.2
                $match.Groups[1].Value+$width.ToString('0.########',[Globalization.CultureInfo]::InvariantCulture)+$match.Groups[3].Value
            })
            $node.InnerText=$style
        }
        foreach($node in @($root.SelectNodes('.//*'))){
            if($node.LocalName -notin @('defs','style','g','line','polyline','polygon','path')){throw 'Unexpected traffic artwork element'}
            if($node.HasAttribute('id')){$node.SetAttribute('id',$prefix+$node.GetAttribute('id'))}
            if($node.HasAttribute('class')){$node.SetAttribute('class',(($node.GetAttribute('class') -split '\s+'|ForEach-Object{$prefix+$_}) -join ' '))}
            if($node.HasAttribute('href') -or $node.HasAttribute('style')){throw 'Unexpected traffic artwork reference or inline style'}
        }
        [void]$result.Append('<symbol id="traffic-'+$model.Name+'" viewBox="'+$model.Box+'" stroke-width="1.2" preserveAspectRatio="xMidYMid meet">')
        [void]$result.Append('<path data-traffic-fill="'+$model.Name+'" d="'+$fill+'" fill="currentColor" stroke="none"/><g data-traffic-outline="'+$model.Name+'">')
        [void]$result.Append($root.InnerXml)
        [void]$result.Append('</g></symbol>')
    }
    return $result.ToString()
}
