# Wall and CPU time of one native shader stage run on the shadow run's roots.
param([string]$Dest = 'C:\WawConverter\work\shadow_native_time')
$roots = Get-Content 'C:\WawConverter\work\shadow_roots.txt'
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
$p = Start-Process -FilePath 'C:\WawConverter\native\build\waw2bo2n.exe' -ArgumentList (@('shaders-stage', '--dest', "`"$Dest`"") + ($roots | ForEach-Object { "`"$_`"" })) -NoNewWindow -PassThru
$h = $p.Handle
$p.WaitForExit()
"wall $([math]::Round(($p.ExitTime - $p.StartTime).TotalSeconds, 1)) s, cpu $([math]::Round($p.TotalProcessorTime.TotalSeconds, 1)) s, exit $($p.ExitCode)"
