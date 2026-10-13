# 从 128 主题 PNG 生成设置行图标 / 主题预览小图（SPNG 不可运行时缩放）。
# 用法: powershell -File tools/gen_small_icons.ps1
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot '..\main\xingzhi-assets' | Resolve-Path

function Save-PngScaled([string]$srcPath, [string]$dstPath, [int]$size) {
    $img = [System.Drawing.Image]::FromFile($srcPath)
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::FromArgb(0, 0, 0, 0))
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.SmoothingMode = 'HighQuality'
    $g.PixelOffsetMode = 'HighQuality'
    $g.DrawImage($img, 0, 0, $size, $size)
    $g.Dispose(); $img.Dispose()
    $bmp.Save($dstPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host "wrote $dstPath ($size)"
}

$row = @{
    'ic_app_home_theme1_wifi.png'      = 'ic_s_row_wifi.png'
    'ic_app_home_theme1_theme.png'     = 'ic_s_row_theme.png'
    'ic_app_home_theme1_music.png'     = 'ic_s_row_volume.png'
    'ic_app_home_theme1_backlight.png' = 'ic_s_row_backlight.png'
    'ic_app_home_theme1_vibrate.png'   = 'ic_s_row_vibrate.png'
    'ic_app_home_theme1_settings.png'  = 'ic_s_row_ota.png'
    'ic_app_home_theme1_info.png'      = 'ic_s_row_info.png'
}
foreach ($kv in $row.GetEnumerator()) {
    Save-PngScaled (Join-Path $root $kv.Key) (Join-Path $root $kv.Value) 24
}
1..4 | ForEach-Object {
    Save-PngScaled (Join-Path $root ("ic_app_home_theme{0}_theme.png" -f $_)) `
                   (Join-Path $root ("ic_s_theme_preview_{0}.png" -f $_)) 84
}

# 电源对话框图标（源 64 → 28，1:1 显示，不可再 zoom）
foreach ($n in @('ic_s_home_power', 'ic_s_home_reboot')) {
    $bak = Join-Path $root ($n + '.src64.png')
    $dst = Join-Path $root ($n + '.png')
    if (-not (Test-Path $bak)) {
        if (Test-Path $dst) { Copy-Item $dst $bak }
    }
    if (Test-Path $bak) {
        Save-PngScaled $bak $dst 28
    }
}
