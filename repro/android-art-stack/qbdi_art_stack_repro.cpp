#include <jni.h>

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include <QBDI.h>

namespace {

constexpr uint32_t kVirtualStackSize = 0x100000;
constexpr uint32_t kEngineStackSize = 0x20000;

uintptr_t currentSp() {
  uintptr_t sp = 0;
  asm volatile("mov %0, sp" : "=r"(sp));
  return sp;
}

__attribute__((noinline)) int targetFindClass(JNIEnv *env) {
  std::printf("[target] targetFindClass sp=0x%" PRIxPTR "\n", currentSp());
  jclass cls = env->FindClass("Repro");
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return -1;
  }
  return cls == nullptr ? 0 : 1;
}

__attribute__((noinline)) int targetGetMethodID(JNIEnv *env) {
  std::printf("[target] targetGetMethodID sp=0x%" PRIxPTR "\n", currentSp());
  jclass cls = env->FindClass("Repro");
  if (env->ExceptionCheck() || cls == nullptr) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return -1;
  }

  jmethodID mid = env->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V");
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return -2;
  }
  return mid == nullptr ? 0 : 1;
}

__attribute__((noinline)) int targetCallStatic(JNIEnv *env) {
  std::printf("[target] targetCallStatic sp=0x%" PRIxPTR "\n", currentSp());
  jclass cls = env->FindClass("Repro");
  if (env->ExceptionCheck() || cls == nullptr) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return -1;
  }

  jmethodID mid = env->GetStaticMethodID(cls, "ping", "()I");
  if (env->ExceptionCheck() || mid == nullptr) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return -2;
  }

  jint value = env->CallStaticIntMethod(cls, mid);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return -3;
  }
  return static_cast<int>(value);
}

__attribute__((noinline)) int targetPendingFindClass(JNIEnv *env) {
  std::printf("[target] targetPendingFindClass sp=0x%" PRIxPTR "\n",
              currentSp());
  jclass cls = env->FindClass("Repro");
  if (cls == nullptr || env->ExceptionCheck()) {
    return -1;
  }

  jmethodID mid = env->GetStaticMethodID(cls, "ping", "()I");
  if (mid == nullptr || env->ExceptionCheck()) {
    return -2;
  }

  env->CallStaticIntMethod(cls, mid);
  std::printf("[target] pending after CallStaticIntMethod=%d\n",
              env->ExceptionCheck());
  env->FindClass("Repro");
  return env->ExceptionCheck() ? -3 : 1;
}

using Target = int (*)(JNIEnv *);

Target selectTarget(const std::string &mode) {
  if (mode.find("pending") != std::string::npos) {
    return targetPendingFindClass;
  }
  if (mode.find("callstatic") != std::string::npos) {
    return targetCallStatic;
  }
  if (mode.find("getmethodid") != std::string::npos) {
    return targetGetMethodID;
  }
  return targetFindClass;
}

int runNative(JNIEnv *env, const std::string &mode) {
  Target target = selectTarget(mode);
  int ret = target(env);
  std::printf("[native] direct JNI target returned %d\n", ret);
  return ret;
}

int runQBDICall(JNIEnv *env, const std::string &mode) {
  Target target = selectTarget(mode);
  QBDI::VM vm;
  QBDI::GPRState *state = vm.getGPRState();
  uint8_t *fakeStack = nullptr;
  bool lowStack = mode.find("low") != std::string::npos;

  bool stackOk = false;
  if (lowStack) {
    constexpr uintptr_t candidates[] = {0x6000000000ULL, 0x5000000000ULL,
                                        0x4000000000ULL, 0x3000000000ULL};
    for (uintptr_t candidate : candidates) {
      void *mapped =
          mmap(reinterpret_cast<void *>(candidate), kVirtualStackSize,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (mapped != MAP_FAILED) {
        fakeStack = static_cast<uint8_t *>(mapped);
        state->sp =
            reinterpret_cast<QBDI::rword>(fakeStack) + kVirtualStackSize - 16;
        stackOk = true;
        break;
      }
      std::printf("[native] low stack mmap 0x%" PRIxPTR " failed errno=%d\n",
                  candidate, errno);
    }
  } else {
    stackOk = QBDI::allocateVirtualStack(state, kVirtualStackSize, &fakeStack);
  }
  std::printf("[native] QBDI call entry sp=0x%" PRIxPTR
              " fake stack=%p state_sp=0x%" PRIx64 " stackOk=%d lowStack=%d\n",
              currentSp(), fakeStack, static_cast<uint64_t>(state->sp), stackOk,
              lowStack);
  if (!stackOk) {
    return -10;
  }

  bool rangeOk =
      vm.addInstrumentedModuleFromAddr(reinterpret_cast<QBDI::rword>(target));
  std::printf("[native] addInstrumentedModuleFromAddr=%d target=%p\n", rangeOk,
              reinterpret_cast<void *>(target));
  if (!rangeOk) {
    QBDI::alignedFree(fakeStack);
    return -11;
  }

  QBDI::rword ret = 0;
  bool callOk = vm.call(&ret, reinterpret_cast<QBDI::rword>(target),
                        {reinterpret_cast<QBDI::rword>(env)});
  std::printf("[native] QBDI call returned callOk=%d ret=%" PRIu64 "\n", callOk,
              static_cast<uint64_t>(ret));
  if (lowStack) {
    munmap(fakeStack, kVirtualStackSize);
  } else {
    QBDI::alignedFree(fakeStack);
  }
  return callOk ? static_cast<int>(ret) : -12;
}

int runQBDISwitchStack(JNIEnv *env, const std::string &mode) {
  Target target = selectTarget(mode);
  QBDI::VM vm;

  bool rangeOk =
      vm.addInstrumentedModuleFromAddr(reinterpret_cast<QBDI::rword>(target));
  std::printf("[native] addInstrumentedModuleFromAddr=%d target=%p\n", rangeOk,
              reinterpret_cast<void *>(target));
  if (!rangeOk) {
    return -20;
  }

  QBDI::rword ret = 0;
  bool callOk = vm.switchStackAndCall(
      &ret, reinterpret_cast<QBDI::rword>(target),
      {reinterpret_cast<QBDI::rword>(env)}, kEngineStackSize);
  std::printf("[native] QBDI switchStackAndCall returned callOk=%d ret=%" PRIu64
              "\n",
              callOk, static_cast<uint64_t>(ret));
  return callOk ? static_cast<int>(ret) : -21;
}

__attribute__((noinline)) std::string targetStringReturn() {
  return std::string(
      "asdniwasdnklnweoinaszcnklwnoianaolsnclkwoanslclkwolalnksc");
}

int runStringReturn() {
  std::string direct = targetStringReturn();
  std::printf("[native] direct std::string size=%zu value=%s\n", direct.size(),
              direct.c_str());

  QBDI::VM vm;
  QBDI::GPRState *state = vm.getGPRState();
  uint8_t *fakeStack = nullptr;
  bool stackOk =
      QBDI::allocateVirtualStack(state, kVirtualStackSize, &fakeStack);
  std::printf("[native] stringret fake stack=%p state_sp=0x%" PRIx64
              " stackOk=%d\n",
              fakeStack, static_cast<uint64_t>(state->sp), stackOk);
  if (!stackOk) {
    return -30;
  }

  bool rangeOk = vm.addInstrumentedModuleFromAddr(
      reinterpret_cast<QBDI::rword>(&targetStringReturn));
  std::printf("[native] stringret addInstrumentedModuleFromAddr=%d target=%p\n",
              rangeOk, reinterpret_cast<void *>(&targetStringReturn));
  if (!rangeOk) {
    QBDI::alignedFree(fakeStack);
    return -31;
  }

  QBDI::rword ret = 0;
  bool callOk =
      vm.call(&ret, reinterpret_cast<QBDI::rword>(&targetStringReturn), {});
  std::printf("[native] stringret QBDI callOk=%d raw_ret=0x%" PRIx64 "\n",
              callOk, static_cast<uint64_t>(ret));
  QBDI::alignedFree(fakeStack);
  return callOk ? 0 : -32;
}

std::string toString(JNIEnv *env, jstring value) {
  const char *chars = env->GetStringUTFChars(value, nullptr);
  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

using CreateJavaVM = jint (*)(JavaVM **, JNIEnv **, void *);

bool preloadArtLibrary(const char *path) {
  void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  if (handle == nullptr) {
    std::fprintf(stderr, "[native] dlopen %s failed: %s\n", path, dlerror());
    return false;
  }
  return true;
}

JNIEnv *createJNIEnv(JavaVM **vmOut) {
  const char *artDeps[] = {
      "/apex/com.android.art/lib64/libc++.so",
      "/apex/com.android.art/lib64/libartpalette.so",
      "/apex/com.android.art/lib64/libbase.so",
      "/apex/com.android.art/lib64/liblz4.so",
      "/apex/com.android.art/lib64/liblzma.so",
      "/apex/com.android.art/lib64/libnativebridge.so",
      "/apex/com.android.art/lib64/libsigchain.so",
      "/apex/com.android.art/lib64/libunwindstack.so",
      "/apex/com.android.art/lib64/libartbase.so",
      "/apex/com.android.art/lib64/libdexfile.so",
      "/apex/com.android.art/lib64/libprofile.so",
      "/apex/com.android.art/lib64/libnativeloader.so",
  };
  for (const char *dep : artDeps) {
    if (!preloadArtLibrary(dep)) {
      return nullptr;
    }
  }

  void *art =
      dlopen("/apex/com.android.art/lib64/libart.so", RTLD_NOW | RTLD_GLOBAL);
  if (art == nullptr) {
    std::fprintf(stderr, "[native] dlopen libart failed: %s\n", dlerror());
    return nullptr;
  }

  auto createJavaVM =
      reinterpret_cast<CreateJavaVM>(dlsym(art, "JNI_CreateJavaVM"));
  if (createJavaVM == nullptr) {
    std::fprintf(stderr, "[native] dlsym JNI_CreateJavaVM failed: %s\n",
                 dlerror());
    return nullptr;
  }

  JavaVMOption options[] = {
      {const_cast<char *>("-Djava.class.path=."), nullptr},
  };
  JavaVMInitArgs args = {};
  args.version = JNI_VERSION_1_6;
  args.nOptions = 1;
  args.options = options;
  args.ignoreUnrecognized = JNI_TRUE;

  JNIEnv *env = nullptr;
  JavaVM *vm = nullptr;
  jint ret = createJavaVM(&vm, &env, &args);
  std::printf("[native] JNI_CreateJavaVM ret=%d vm=%p env=%p\n", ret,
              static_cast<void *>(vm), static_cast<void *>(env));
  if (ret != JNI_OK) {
    return nullptr;
  }

  *vmOut = vm;
  return env;
}

} // namespace

extern "C" JNIEXPORT jint JNICALL Java_Repro_run(JNIEnv *env, jclass,
                                                 jstring modeValue) {
  std::string mode = toString(env, modeValue);
  std::printf("[native] mode=%s\n", mode.c_str());
  std::fflush(stdout);

  if (mode == "stringret") {
    return runStringReturn();
  }
  if (mode.find("native") == 0) {
    return runNative(env, mode);
  }
  if (mode.find("switch") == 0) {
    return runQBDISwitchStack(env, mode);
  }
  return runQBDICall(env, mode);
}

int main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "call-findclass";
  JavaVM *vm = nullptr;
  JNIEnv *env = createJNIEnv(&vm);
  if (env == nullptr) {
    return 2;
  }

  int ret = 0;
  if (mode.find("native") == 0) {
    ret = runNative(env, mode);
  } else if (mode.find("switch") == 0) {
    ret = runQBDISwitchStack(env, mode);
  } else {
    ret = runQBDICall(env, mode);
  }
  std::printf("[native] final result=%d\n", ret);
  std::fflush(stdout);

  if (vm != nullptr) {
    vm->DestroyJavaVM();
  }
  return ret < 0 ? 1 : 0;
}
