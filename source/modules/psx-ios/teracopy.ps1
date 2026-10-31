function Invoke-IosTeraCopy {
 param(
  [Parameter(Mandatory)][string[]]$Sources,
  [Parameter(Mandatory)][string]$Destination,
  [Parameter(Mandatory)][string]$EvidenceDirectory,
  [string]$Python=(Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'),
  [ValidateRange(5,60)][int]$TimeoutSeconds=45
 )
 $tool='C:\Program Files\TeraCopy\TeraCopy.exe'
 $reader=Join-Path $PSScriptRoot 'teracopy-job.py'
 foreach($path in @($tool,$reader,$Python)){if(-not[IO.File]::Exists($path)){throw "Required transfer tool unavailable; no fallback: $path"}}
 foreach($path in (@($Sources)+@($Destination,$EvidenceDirectory))){if(-not[IO.Path]::IsPathFullyQualified($path)){throw 'TeraCopy paths must be absolute.'}}
 if(-not$Sources.Count){throw 'Empty TeraCopy batch.'}
 $pins=@($Sources|ForEach-Object{
  $file=Get-Item -LiteralPath $_ -Force
  if($file.PSIsContainer-or($file.Attributes-band[IO.FileAttributes]::ReparsePoint)){throw 'TeraCopy batch requires ordinary files.'}
  [pscustomobject]@{path=$file.FullName;bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
 })
 $names=@($pins|ForEach-Object {[IO.Path]::GetFileName($_.path)})
 if(@($names|Select-Object -Unique).Count-ne$names.Count){throw 'Duplicate transfer basenames.'}
 if((Test-Path -LiteralPath $Destination)-and@(Get-ChildItem -LiteralPath $Destination -Force).Count){throw 'Transfer destination must be empty; existing bytes preserved.'}
 if(Test-Path -LiteralPath $EvidenceDirectory){throw 'Transfer evidence must be fresh.'}
 if(@(Get-Process -Name TeraCopy -ErrorAction SilentlyContinue).Count){throw 'TeraCopy is active; no overlapping transfer started.'}
 [IO.Directory]::CreateDirectory($Destination)|Out-Null
 [IO.Directory]::CreateDirectory($EvidenceDirectory)|Out-Null
 $list=Join-Path $EvidenceDirectory 'sources.txt'
 [IO.File]::WriteAllLines($list,[string[]]$pins.path,[Text.UTF8Encoding]::new($true))
 $receipt=[ordered]@{tool=$tool;tool_sha256=(Get-FileHash -LiteralPath $tool).Hash;reader_sha256=(Get-FileHash -LiteralPath $reader).Hash;sources=$pins;destination=$Destination;list=$list;collision='SkipAll';verified=$false;source_preserved=$false;job=$null;launcher_exit=$null;failure=$null;remote_cloud_completion='Not established by local copy/hash'}
 $receiptPath=Join-Path $EvidenceDirectory 'receipt.json';$process=$null
 try{
  [IO.File]::WriteAllText($receiptPath,($receipt|ConvertTo-Json -Depth 8))
  $process=Start-Process -FilePath $tool -ArgumentList ('Copy "*'+$list+'" "'+$Destination+'" /SkipAll /Close') -WindowStyle Hidden -PassThru
  $null=$process.Handle;$receipt['pid']=$process.Id;$receipt['started_utc_ticks']=$process.StartTime.ToUniversalTime().Ticks.ToString()
  $clock=[Diagnostics.Stopwatch]::StartNew();$job=$null
  do{
   $raw=& $Python -B $reader $Destination $list
   if($LASTEXITCODE-eq0){$candidate=$raw|ConvertFrom-Json;if($candidate.finished-and$candidate.completion.Count){$job=$candidate}}
   elseif($LASTEXITCODE-ne2){throw 'TeraCopy job reader failed; no fallback.'}
   $retired=$process.WaitForExit(0)
   if($retired){$receipt.launcher_exit=$process.ExitCode;if($process.ExitCode-ne0){throw "TeraCopy exited $($process.ExitCode)."}}
   if($job-and$retired){break}
   Start-Sleep -Milliseconds 200
  }while($clock.Elapsed.TotalSeconds-lt$TimeoutSeconds)
  if(-not$job-or-not$retired){throw 'TeraCopy completion deadline expired; source/staging preserved.'}
  $receipt.job=$job
  if($job.errors-ne0-or$job.skipped-ne0-or$job.copied-ne$pins.Count){throw 'TeraCopy copy/skip/error counts differ from the fresh batch.'}
  if(@(Get-ChildItem -LiteralPath $Destination -File).Count-ne$pins.Count){throw 'TeraCopy destination roster differs.'}
  for($i=0;$i-lt$pins.Count;++$i){
   $pin=$pins[$i];$target=Join-Path $Destination $names[$i]
   if((Get-Item -LiteralPath $target).Length-ne$pin.bytes-or(Get-FileHash -LiteralPath $target).Hash-ine$pin.sha256){throw 'TeraCopy destination size/hash differs.'}
   if((Get-Item -LiteralPath $pin.path).Length-ne$pin.bytes-or(Get-FileHash -LiteralPath $pin.path).Hash-ine$pin.sha256){throw 'TeraCopy source changed.'}
  }
  $receipt.verified=$true;$receipt.source_preserved=$true
  return [pscustomobject]$receipt
 }catch{$receipt.failure=$_.ToString();throw}
 finally{
  if($process){$process.Dispose()}
  [IO.File]::WriteAllText($receiptPath,($receipt|ConvertTo-Json -Depth 8))
 }
}
