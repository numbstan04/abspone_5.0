# 将 libwebrtc (GN/LLVM) 输出的 thin archive 转成 MSVC link.exe 可读的普通 .lib。
#
# 背景：m140 的 out/<cfg>/obj 下是 LLVM thin archive（文件头 "!<thin>"）。
# MSVC 的 link.exe 无法读取 thin archive，而 VS 工程仍使用 link.exe，
# 因此先在构建期把每个 thin archive 展开成普通 .lib。
#
# 用法（通常由 media/src/backend/webrtc/CMakeLists.txt 自动调用）：
#   powershell -File thin_to_thick.ps1 `
#       -ObjRoot <webrtc>/out/Debug32/obj `
#       -OutRoot E:/5.0/deps/webrtc_m140_thick `
#       -LibExe <MSVC>/bin/Hostx64/x86/lib.exe
#
# 已存在且比源 archive 新的输出会被跳过，可重复执行。

param(
    [Parameter(Mandatory=$true)][string]$ObjRoot,
    [Parameter(Mandatory=$true)][string]$OutRoot,
    [Parameter(Mandatory=$true)][string]$LibExe
)

$ErrorActionPreference = 'Stop'
$all = Get-ChildItem -Path $ObjRoot -Recurse -File -Filter '*.lib'
$total = $all.Count
$converted = 0
$skipped = 0

foreach ($lib in $all) {
    $bytes = [System.IO.File]::ReadAllBytes($lib.FullName)
    if ($bytes.Length -lt 8) { continue }
    $magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 8)
    if ($magic -ne "!<thin>`n") { $skipped++; continue }

    $rel = $lib.FullName.Substring($ObjRoot.Length).TrimStart('\')
    $outPath = Join-Path $OutRoot $rel
    $outDir = Split-Path -Parent $outPath
    if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }

    if ((Test-Path $outPath) -and ((Get-Item $outPath).LastWriteTimeUtc -ge $lib.LastWriteTimeUtc)) {
        $skipped++
        continue
    }

    $pos = 8
    $longTable = ''
    $members = New-Object System.Collections.Generic.List[string]
    while ($pos + 60 -le $bytes.Length) {
        $hdr = [System.Text.Encoding]::ASCII.GetString($bytes, $pos, 60)
        $rawName = $hdr.Substring(0, 16).Trim()
        $sizeField = $hdr.Substring(48, 10).Trim()
        if ($sizeField -eq '') { break }
        $size = [int]$sizeField
        $pos += 60

        if ($rawName -eq '/') { $pos += $size; continue }
        if ($rawName -eq '//') {
            $longTable = [System.Text.Encoding]::ASCII.GetString($bytes, $pos, $size)
            $pos += $size
            continue
        }

        $name = $null
        if ($rawName.StartsWith('/')) {
            $offset = [int]$rawName.Substring(1)
            $end = $longTable.IndexOf("`n", $offset)
            if ($end -lt 0) { $end = $longTable.Length }
            $name = $longTable.Substring($offset, $end - $offset).TrimEnd('/')
        } else {
            $name = $rawName.TrimEnd('/')
        }
        if ($name) {
            $full = [System.IO.Path]::GetFullPath((Join-Path $lib.DirectoryName $name))
            $members.Add($full)
        }
    }

    $rsp = Join-Path $env:TEMP ("thin_" + [Guid]::NewGuid().ToString('N') + ".rsp")
    $lines = foreach ($m in $members) { '"' + $m + '"' }
    Set-Content -Path $rsp -Value $lines -Encoding ASCII

    if ($members.Count -eq 0) {
        & $LibExe /NOLOGO /OUT:$outPath | Out-Null
    } else {
        & $LibExe /NOLOGO /OUT:$outPath "@$rsp" | Out-Null
    }
    if ($LASTEXITCODE -ne 0) {
        throw "lib.exe failed for $($lib.FullName)"
    }
    Remove-Item -Force $rsp
    $converted++
    if (($converted % 50) -eq 0) { "converted $converted / $total" }
}

"done: total=$total converted=$converted skipped=$skipped"
