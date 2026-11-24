# Build from a fresh SDK with the checked-in dependency lock. A previous build
# directory may be reused as long as it belongs to this kit and checkout.
param(
    [Parameter(Mandatory)][string]$WorkDir,
    [ValidateRange(1, 64)][int]$Jobs = 3
)
$ErrorActionPreference = 'Stop'

function Invoke-Checked([string]$Executable, [string[]]$ArgumentList) {
    & $Executable @ArgumentList
    if ($LASTEXITCODE -ne 0) { throw "$Executable exited with code $LASTEXITCODE" }
}

$work = [IO.Path]::GetFullPath($WorkDir)
$sdk = Join-Path $work 'sdk'
$downloads = Join-Path $work 'downloads'
New-Item -ItemType Directory -Force -Path $sdk, $downloads | Out-Null
$sevenZip = (Get-Command 7z.exe -CommandType Application -ErrorAction SilentlyContinue |
             Select-Object -First 1).Source
if (!$sevenZip) {
    $installed = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
    if (Test-Path -LiteralPath $installed -PathType Leaf) { $sevenZip = $installed }
}
$lock = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'windows-sdk.lock.json') | ConvertFrom-Json
foreach ($item in $lock) {
    $archive = Join-Path $downloads ([IO.Path]::GetFileName(([uri]$item.url).AbsolutePath))
    if (!(Test-Path -LiteralPath $archive -PathType Leaf)) {
        Invoke-Checked 'curl.exe' @('--fail', '--location', '--silent', '--show-error', '--retry', '3',
                                     '--output', "$archive.partial", $item.url)
        Move-Item -LiteralPath "$archive.partial" -Destination $archive -Force
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $item.sha256) {
        throw "SHA-256 mismatch for $($item.component)"
    }
    $out = Join-Path $sdk $item.extract_to
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    # Windows Server 2022's built-in tar lacks the LZMA codec for Qt's 7z files.
    if ($sevenZip -and [IO.Path]::GetExtension($archive) -eq '.7z') {
        Invoke-Checked $sevenZip @('x', '-y', '-bd', '-bso0', "-o$out", $archive)
    } else {
        Invoke-Checked "$env:SystemRoot\System32\tar.exe" @('-xf', $archive, '-C', $out)
    }
    Write-Host "Verified $($item.component)"
}

$compiler = Join-Path $sdk 'Tools\mingw1310_64\bin'
$cmake = Join-Path $sdk 'cmake-3.31.6-windows-x86_64\bin\cmake.exe'
$ninja = Join-Path $sdk 'ninja\ninja.exe'
$python = Join-Path $sdk 'python\python.exe'
$env:PATH = "$compiler;$sdk\bin;$sdk\python;$sdk\ninja;$(Split-Path $cmake);$env:PATH"
$env:CC = "$compiler\gcc.exe"
$env:CXX = "$compiler\g++.exe"
$zsrc = Join-Path $sdk 'zlib\zlib-1.3.1'
$zbuild = Join-Path $work 'build-zlib'
$zprefix = Join-Path $sdk 'zlib-static'
Invoke-Checked $cmake @('-S', $zsrc, '-B', $zbuild, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                        '-DZLIB_BUILD_EXAMPLES=OFF', "-DCMAKE_MAKE_PROGRAM=$ninja")
Invoke-Checked $cmake @('--build', $zbuild, '--target', 'zlibstatic', '--parallel', $Jobs)
$archives = @(Get-ChildItem -LiteralPath $zbuild -File -Filter '*.a')
if ($archives.Count -ne 1) { throw "Expected one built static zlib archive in $zbuild" }
New-Item -ItemType Directory -Force -Path "$zprefix\include", "$zprefix\lib" | Out-Null
Copy-Item -LiteralPath "$zsrc\zlib.h", "$zbuild\zconf.h" -Destination "$zprefix\include" -Force
Copy-Item -LiteralPath $archives[0].FullName -Destination "$zprefix\lib\libz.a" -Force

$sodium = Join-Path $sdk 'sodium\libsodium-win64'
Invoke-Checked $python @((Join-Path $PSScriptRoot 'build-readonly-windows.py'), '--qt-prefix', $sdk,
                         '--compiler-bin', $compiler, '--sodium-root', $sodium,
                         '--sodium-library', "$sodium\lib\libsodium.a", '--zlib-root', $zprefix,
                         '--zlib-library', "$zprefix\lib\libz.a", '--cmake', $cmake, '--ninja', $ninja,
                         '--build-dir', "$work\build", '--output-dir', "$work\dist", '--jobs', $Jobs)
