# vnc-ocr.ps1 — Screen region OCR using Windows.Media.Ocr (Win10/11 built-in)
#
# Usage: powershell -ExecutionPolicy Bypass -File vnc-ocr.ps1 <x> <y> <w> <h> [lang]
#
# Captures a screen region, feeds it to the WinRT OCR engine, outputs recognized
# text to stdout. Language defaults to first available (usually en-US).
#
# Exit codes: 0 = success, 1 = error (message on stderr)

param(
    [Parameter(Mandatory=$true)][int]$X,
    [Parameter(Mandatory=$true)][int]$Y,
    [Parameter(Mandatory=$true)][int]$W,
    [Parameter(Mandatory=$true)][int]$H,
    [string]$Lang = ""
)

# Validate dimensions
if ($W -le 0 -or $H -le 0) {
    [Console]::Error.WriteLine("Width and height must be positive")
    exit 1
}

if ($W -gt 4096 -or $H -gt 4096) {
    [Console]::Error.WriteLine("Region too large (max 4096x4096)")
    exit 1
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Runtime.WindowsRuntime

# Load WinRT OCR types
[void][Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
[void][Windows.Graphics.Imaging.SoftwareBitmap, Windows.Foundation, ContentType = WindowsRuntime]
[void][Windows.Graphics.Imaging.BitmapPixelFormat, Windows.Foundation, ContentType = WindowsRuntime]
[void][Windows.Graphics.Imaging.BitmapAlphaMode, Windows.Foundation, ContentType = WindowsRuntime]
[void][Windows.Globalization.Language, Windows.Foundation, ContentType = WindowsRuntime]

# WinRT async bridging for PowerShell 5.1
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]

function Await($WinRtTask, $ResultType) {
    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
    $netTask = $asTask.Invoke($null, @($WinRtTask))
    $netTask.Wait(-1) | Out-Null
    $netTask.Result
}

try {
    # Capture screen region to bitmap
    $bmp = New-Object System.Drawing.Bitmap($W, $H, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size($W, $H)))
    $gfx.Dispose()

    # Convert to PNG in memory stream (WinRT BitmapDecoder needs a stream)
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $ms.Position = 0

    # Convert to WinRT IRandomAccessStream
    $ras = [System.IO.WindowsRuntimeStreamExtensions]::AsRandomAccessStream($ms)

    # Decode to SoftwareBitmap via BitmapDecoder
    [void][Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType = WindowsRuntime]
    $decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($ras)) ([Windows.Graphics.Imaging.BitmapDecoder])
    $softwareBmp = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])

    # Create OCR engine
    if ($Lang -ne "") {
        $language = New-Object Windows.Globalization.Language($Lang)
        $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage($language)
    } else {
        $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
    }

    if ($null -eq $engine) {
        [Console]::Error.WriteLine("OCR engine not available for the specified language")
        exit 1
    }

    # Run OCR
    $ocrResult = Await ($engine.RecognizeAsync($softwareBmp)) ([Windows.Media.Ocr.OcrResult])

    # Output recognized text
    [Console]::Out.Write($ocrResult.Text)

    $ms.Dispose()
}
catch {
    [Console]::Error.WriteLine("OCR failed: $_")
    exit 1
}
