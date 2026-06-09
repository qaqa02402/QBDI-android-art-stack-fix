# Android ART Stack Reproducer

This reproducer is for the Android ART stack-limit issue discussed in QBDI
issues #235, #242, and #243, and the `VM::call` C++ ABI limitation discussed in
issue #279.

It expects an Android arm64 QBDI installation under `usr/local` and Android SDK
tools under `/home/kali/Android/Sdk`.

Build and push:

```sh
mkdir -p repro/android-art-stack/out/classes \
         repro/android-art-stack/out/dex \
         repro/android-art-stack/out/lib

java --module jdk.compiler/com.sun.tools.javac.Main \
  -source 8 -target 8 \
  -d repro/android-art-stack/out/classes \
  repro/android-art-stack/Repro.java

/home/kali/Android/Sdk/build-tools/36.0.0/d8 \
  --min-api 24 \
  --output repro/android-art-stack/out/dex \
  repro/android-art-stack/out/classes/Repro.class

(cd repro/android-art-stack/out/dex && zip -q -r ../repro.jar classes.dex)

/home/kali/Android/Sdk/ndk/25.1.8937393/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++ \
  -shared -fPIC -std=c++17 -O0 -g -static-libstdc++ \
  repro/android-art-stack/qbdi_art_stack_helper.cpp \
  -llog -ldl -lz -lm \
  -o repro/android-art-stack/out/lib/libqbdihelper.so

/home/kali/Android/Sdk/ndk/25.1.8937393/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang++ \
  -shared -fPIC -std=c++17 -O0 -g -fvisibility=hidden -static-libstdc++ \
  -Iusr/local/include \
  repro/android-art-stack/qbdi_art_stack_repro.cpp \
  -Wl,--start-group usr/local/lib/libQBDI.a -Wl,--end-group \
  -llog -ldl -lz -lm \
  -o repro/android-art-stack/out/lib/libqbdirepro.so

adb shell 'rm -rf /data/local/tmp/qbdi-art-stack && mkdir -p /data/local/tmp/qbdi-art-stack'
adb push repro/android-art-stack/out/repro.jar /data/local/tmp/qbdi-art-stack/repro.jar
adb push repro/android-art-stack/out/lib/libqbdihelper.so /data/local/tmp/qbdi-art-stack/libqbdihelper.so
adb push repro/android-art-stack/out/lib/libqbdirepro.so /data/local/tmp/qbdi-art-stack/libqbdirepro.so
adb shell 'chmod 644 /data/local/tmp/qbdi-art-stack/repro.jar /data/local/tmp/qbdi-art-stack/libqbdihelper.so /data/local/tmp/qbdi-art-stack/libqbdirepro.so'
```

Useful commands:

```sh
adb shell 'CLASSPATH=/data/local/tmp/qbdi-art-stack/repro.jar app_process64 /data/local/tmp/qbdi-art-stack Repro switch-callstatic'
adb shell 'CLASSPATH=/data/local/tmp/qbdi-art-stack/repro.jar app_process64 /data/local/tmp/qbdi-art-stack Repro lowcall-pending'
adb shell 'CLASSPATH=/data/local/tmp/qbdi-art-stack/repro.jar app_process64 /data/local/tmp/qbdi-art-stack Repro stringret'
adb shell 'CLASSPATH=/data/local/tmp/qbdi-art-stack/repro.jar app_process64 /data/local/tmp/qbdi-art-stack Repro call-cpp-exception'
adb shell 'CLASSPATH=/data/local/tmp/qbdi-art-stack/repro.jar app_process64 /data/local/tmp/qbdi-art-stack Repro call-setjmp'
adb shell 'CLASSPATH=/data/local/tmp/qbdi-art-stack/repro.jar app_process64 /data/local/tmp/qbdi-art-stack Repro allmaps-call-cpp-exception'
```

Observed results on Pixel 6 / Android 14:

- `switch-callstatic` returns `42`.
- `lowcall-pending` aborts with `No pending exception expected:
  java.lang.StackOverflowError: stack size 8188KB`.
- `stringret` exits with `SIGSEGV`, demonstrating that `VM::call` cannot infer
  the hidden return-buffer ABI for non-trivial C++ return values.
