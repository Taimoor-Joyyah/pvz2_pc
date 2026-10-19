param([string]$Ndk, [string]$AndroidSdk = "$env:LOCALAPPDATA\Android\Sdk")
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$toolsRoot = Join-Path $projectRoot 'out\tools'
if (!$Ndk) { $Ndk = Join-Path $toolsRoot 'android-ndk-r29' }
if (!(Test-Path -LiteralPath (Join-Path $Ndk 'source.properties'))) {
    $archive = Join-Path $toolsRoot 'android-ndk-r29-windows.zip'
    New-Item -ItemType Directory -Force -Path $toolsRoot | Out-Null
    if (!(Test-Path -LiteralPath $archive)) {
        & curl.exe --fail --location --retry 3 --output $archive 'https://dl.google.com/android/repository/android-ndk-r29-windows.zip'
        if ($LASTEXITCODE) { throw 'NDK download failed.' }
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA1).Hash -ne 'AB3BB30FBB9E6903666D60C55D11E78B04E07472') {
        throw 'NDK archive does not match the official r29 checksum.'
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($archive, $toolsRoot)
}
if (!(Select-String -LiteralPath (Join-Path $Ndk 'source.properties') -SimpleMatch '29.0.14206865' -Quiet)) {
    throw 'This fixture uses the pinned Android NDK 29.0.14206865.'
}
if (!(Get-Command ninja -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsPath = & $vswhere -latest -products '*' -property installationPath
    $env:PATH = (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' + $env:PATH
}
$buildRoot = Join-Path $projectRoot 'out\android-native'
& cmake -S (Join-Path $projectRoot 'examples\android_native') -B $buildRoot -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$Ndk\build\cmake\android.toolchain.cmake" '-DANDROID_ABI=arm64-v8a' `
    '-DANDROID_PLATFORM=android-24' '-DANDROID_STL=c++_shared' '-DCMAKE_BUILD_TYPE=Release'
if ($LASTEXITCODE) { throw 'Android fixture configuration failed.' }
& cmake --build $buildRoot --parallel 4
if ($LASTEXITCODE) { throw 'Android fixture compilation failed.' }
$buildTools = Join-Path $AndroidSdk 'build-tools\36.0.0'
$androidJar = Join-Path $AndroidSdk 'platforms\android-36\android.jar'
if (!(Test-Path -LiteralPath $androidJar)) {
    $androidJar = Join-Path $AndroidSdk 'platforms\android-36.1\android.jar'
}
$unsigned = Join-Path $buildRoot 'native-test-unsigned.apk'
& (Join-Path $buildTools 'aapt2.exe') link -o $unsigned --manifest (Join-Path $projectRoot 'examples\android_native\AndroidManifest.xml') -I $androidJar
if ($LASTEXITCODE) { throw 'Android fixture APK manifest compilation failed.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($unsigned, [System.IO.Compression.ZipArchiveMode]::Update)
try {
    foreach ($name in @('libacp_native_test.so', 'libc++_shared.so')) {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $buildRoot $name), "lib/arm64-v8a/$name", [System.IO.Compression.CompressionLevel]::NoCompression) | Out-Null
    }
} finally { $zip.Dispose() }
$aligned = Join-Path $buildRoot 'native-test-aligned.apk'
& (Join-Path $buildTools 'zipalign.exe') -P 16 -f 4 $unsigned $aligned
if ($LASTEXITCODE) { throw 'Android fixture APK alignment failed.' }
$keytool = Get-Command keytool.exe -ErrorAction SilentlyContinue
if (!$keytool) {
    $keytool = Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.jdks') -Filter keytool.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
}
if (!$keytool) { throw 'A JDK is required to sign the test APK.' }
$keytoolPath = if ($keytool.Source) { $keytool.Source } else { $keytool.FullName }
$javaBin = Split-Path -Parent $keytoolPath
$keystore = Join-Path $buildRoot 'fixture-debug.keystore'
if (!(Test-Path -LiteralPath $keystore)) {
    & $keytoolPath -genkeypair -keystore $keystore -storepass android -keypass android -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=ACP Native Test'
    if ($LASTEXITCODE) { throw 'Fixture debug key generation failed.' }
}
& (Join-Path $javaBin 'java.exe') -jar (Join-Path $buildTools 'lib\apksigner.jar') sign --ks $keystore --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android --out (Join-Path $buildRoot 'native-test.apk') $aligned
if ($LASTEXITCODE) { throw 'Android fixture APK signing failed.' }
Write-Output "Android C++23 fixture: $buildRoot\native-test.apk"
