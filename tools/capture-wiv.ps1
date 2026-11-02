# Run wiv with frame capture of the real screen around start-up and each strip toggle.
# Usage:  .\capture-wiv.ps1 [image]     then press T a few times (wait ~1 s between), then close wiv.
param([string]$Image = "C:\Users\dovg\git\wiv\examples\lena.pgm")
Remove-Item "$env:USERPROFILE\.wiv\captures" -Recurse -Force -ErrorAction SilentlyContinue
$env:WIV_CAPTURE = "1"
& "C:\Users\dovg\git\wiv\build\windows-RelWithDebInfo\wiv.exe" $Image
Write-Host "Frames are in $env:USERPROFILE\.wiv\captures"
