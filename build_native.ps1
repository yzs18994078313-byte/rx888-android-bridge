$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot.Replace('\','/')
& python "$taskRoot/patch_fftw.py"
if($LASTEXITCODE -ne 0) { throw 'FFTW patch failed' }
$taskTools = if($env:RX888_TOOLS_DIR) { $env:RX888_TOOLS_DIR.Replace('\','/') } else { "$taskRoot/tools" }
$taskNdk = if($env:ANDROID_NDK_HOME) { $env:ANDROID_NDK_HOME.Replace('\','/') } else { "$taskTools/android-ndk-r28c" }
$taskCmake = "$taskTools/py/cmake/data/bin/cmake.exe"
$taskNinja = "$taskTools/py/bin/ninja.exe"
if(!(Test-Path -LiteralPath $taskNinja)) { $taskNinja="$taskTools/py/ninja/data/bin/ninja.exe" }
& $taskCmake -S $taskRoot -B "$taskRoot/build/android" -G Ninja "-DCMAKE_MAKE_PROGRAM=$taskNinja" "-DCMAKE_TOOLCHAIN_FILE=$taskNdk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
if($LASTEXITCODE -ne 0) { throw 'Configure failed' }
& $taskCmake --build "$taskRoot/build/android" --parallel 8 --target rx888_bridge ddc_tests queue_tests
if($LASTEXITCODE -ne 0) { throw 'Native build failed' }
