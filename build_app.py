from pathlib import Path
import os, shutil, subprocess, zipfile, hashlib
root=Path(__file__).resolve().parent
tools=Path(os.environ.get('RX888_TOOLS_DIR',root/'tools'))
ndk=Path(os.environ.get('ANDROID_NDK_HOME',tools/'android-ndk-r28c'))
jdk=tools/'jdk-17.0.17+10'
buildtools=next(tools.glob('*/aapt2.exe')).parent
android=tools/'android-35/android.jar'
out=root/'build/app';out.mkdir(parents=True,exist_ok=True)
classes=out/'classes';dex=out/'dex'
classes.mkdir(exist_ok=True);dex.mkdir(exist_ok=True)
env=dict(os.environ,JAVA_HOME=str(jdk));env['PATH']=str(jdk/'bin')+os.pathsep+env.get('PATH','')
def run(*args):subprocess.run([str(x) for x in args],check=True,env=env)
run(jdk/'bin/javac.exe','-encoding','UTF-8','-source','8','-target','8','-bootclasspath',str(android)+os.pathsep+str(buildtools/'core-lambda-stubs.jar'),'-d',classes,*sorted((root/'app/src').rglob('*.java')))
jar=out/'app-classes.jar'
with zipfile.ZipFile(jar,'w',zipfile.ZIP_DEFLATED) as z:
    for p in classes.rglob('*.class'):z.write(p,p.relative_to(classes).as_posix())
run(jdk/'bin/java.exe','-cp',buildtools/'lib/d8.jar','com.android.tools.r8.D8','--lib',android,'--min-api','28','--output',dex,jar)
unsigned=out/'unsigned.apk'
run(buildtools/'aapt2.exe','link','-I',android,'--manifest',root/'app/AndroidManifest.xml','-o',unsigned)
payload={'librx888_bridge.so':root/'build/android/librx888_bridge.so','libusb1.0.so':root/'build/android/libusb-cmake/libusb1.0.so','libc++_shared.so':ndk/'toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'}
package=out/'lib';package.mkdir(exist_ok=True)
strip=ndk/'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe'
for name,source in payload.items():
    shutil.copy2(source,package/name)
    if name!='libc++_shared.so':run(strip,'--strip-unneeded',package/name)
with zipfile.ZipFile(unsigned,'a') as z:
    z.write(dex/'classes.dex','classes.dex',compress_type=zipfile.ZIP_DEFLATED)
    for name in payload:z.write(package/name,'lib/arm64-v8a/'+name,compress_type=zipfile.ZIP_STORED)
    z.write(root/'third_party/SDDC_FX3.img','assets/SDDC_FX3.img',compress_type=zipfile.ZIP_DEFLATED)
    z.write(root/'LICENSE','assets/licenses/bridge-GPL-3.txt',compress_type=zipfile.ZIP_DEFLATED)
    for p in sorted((root/'third_party').glob('*.txt')):z.write(p,'assets/licenses/'+p.name,compress_type=zipfile.ZIP_DEFLATED)
aligned=out/'aligned.apk';run(buildtools/'zipalign.exe','-P','16','-f','4',unsigned,aligned)
key=root/'build/debug-signing.p12'
if not key.exists():run(jdk/'bin/keytool.exe','-genkeypair','-keystore',key,'-storetype','PKCS12','-alias','rx888-bridge','-storepass','android','-keypass','android','-keyalg','RSA','-keysize','2048','-validity','3650','-dname','CN=RX888 Bridge Development')
dist=Path(os.environ.get('RX888_DIST_DIR',root/'dist'));dist.mkdir(parents=True,exist_ok=True)
apk=dist/'RX888-Bridge-1.0.0-arm64.apk'
run(jdk/'bin/java.exe','-jar',buildtools/'lib/apksigner.jar','sign','--v4-signing-enabled','false','--ks',key,'--ks-key-alias','rx888-bridge','--ks-pass','pass:android','--key-pass','pass:android','--out',apk,aligned)
run(jdk/'bin/java.exe','-jar',buildtools/'lib/apksigner.jar','verify','--verbose',apk)
run(buildtools/'zipalign.exe','-c','-P','16','4',apk)
run(buildtools/'aapt2.exe','dump','badging',apk)
print('APK:',apk,'SHA256:',hashlib.sha256(apk.read_bytes()).hexdigest())
