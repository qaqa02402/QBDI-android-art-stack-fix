/*
 * This file is part of QBDI.
 *
 * Copyright 2017 - 2026 Quarkslab
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "QBDI/Config.h"
#include "QBDI/Memory.h"
#include "QBDI/Memory.hpp"
#include "QBDI/Range.h"
#include "QBDI/State.h"
#include "Utility/LogSys.h"

#if defined(QBDI_PLATFORM_ANDROID) && defined(QBDI_ARCH_AARCH64)
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

#define FRAME_LENGTH 16

namespace {

#if defined(QBDI_PLATFORM_ANDROID) && defined(QBDI_ARCH_AARCH64)

struct MMapAllocation {
  void *ptr;
  void *mapping;
  size_t size;
};

std::mutex mmapAllocationsMutex;
std::vector<MMapAllocation> mmapAllocations;
thread_local uintptr_t nextStackAddress = 0;

size_t androidPageSize() {
  static const size_t pageSize = []() {
    long value = sysconf(_SC_PAGESIZE);
    if (value <= 0 || (value & (value - 1)) != 0) {
      return size_t(4096);
    }
    return static_cast<size_t>(value);
  }();
  return pageSize;
}

bool alignUp(uintptr_t value, uintptr_t align, uintptr_t *result) {
  if (value > std::numeric_limits<uintptr_t>::max() - align + 1) {
    return false;
  }
  *result = (value + align - 1) & ~(align - 1);
  return true;
}

bool pageAlignedSize(size_t size, size_t *result) {
  const size_t pageSize = androidPageSize();
  if (size == 0 || size > std::numeric_limits<size_t>::max() - pageSize + 1) {
    return false;
  }
  *result = (size + pageSize - 1) & ~(pageSize - 1);
  return true;
}

uintptr_t currentStackAddress() {
  uintptr_t sp = 0;
  asm volatile("mov %0, sp" : "=r"(sp));
  return sp;
}

uintptr_t currentStackEnd() {
#if __ANDROID_API__ >= 24
  pthread_attr_t attributes;
  if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
    void *stackBase = nullptr;
    size_t stackSize = 0;
    int result = pthread_attr_getstack(&attributes, &stackBase, &stackSize);
    pthread_attr_destroy(&attributes);
    if (result == 0 &&
        stackSize <= std::numeric_limits<uintptr_t>::max() -
                         reinterpret_cast<uintptr_t>(stackBase)) {
      return reinterpret_cast<uintptr_t>(stackBase) + stackSize;
    }
  }
#endif
  return 0;
}

void rememberMMapAllocation(void *ptr, void *mapping, size_t size) {
  std::lock_guard<std::mutex> lock(mmapAllocationsMutex);
  mmapAllocations.push_back({ptr, mapping, size});
}

bool forgetMMapAllocation(void *ptr, void **mapping, size_t *size) {
  std::lock_guard<std::mutex> lock(mmapAllocationsMutex);
  for (auto it = mmapAllocations.begin(); it != mmapAllocations.end(); ++it) {
    if (it->ptr == ptr) {
      *mapping = it->mapping;
      *size = it->size;
      mmapAllocations.erase(it);
      return true;
    }
  }
  return false;
}

void *mmapNoReplace(uintptr_t address, size_t size) {
  void *requested = reinterpret_cast<void *>(address);
  void *ptr = mmap(requested, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (ptr == MAP_FAILED) {
    return nullptr;
  }
  if (ptr != requested) {
    munmap(ptr, size);
    return nullptr;
  }
  return ptr;
}

void *mmapStack(uintptr_t address, size_t mappingSize, size_t pageSize) {
  void *mapping = mmapNoReplace(address, mappingSize);
  if (mapping == nullptr) {
    return nullptr;
  }
  if (mprotect(mapping, pageSize, PROT_NONE) != 0) {
    munmap(mapping, mappingSize);
    return nullptr;
  }
  return mapping;
}

void rememberNextStackAddress(uintptr_t address, size_t mappingSize) {
  if (address <= std::numeric_limits<uintptr_t>::max() - mappingSize) {
    nextStackAddress = address + mappingSize;
  } else {
    nextStackAddress = 0;
  }
}

void *allocateAndroidStackAboveCurrentStack(size_t stackSize) {
  const uintptr_t pageSize = androidPageSize();
  size_t allocationSize = 0;
  if (!pageAlignedSize(stackSize, &allocationSize) ||
      allocationSize > std::numeric_limits<size_t>::max() - pageSize) {
    return nullptr;
  }
  const size_t mappingSize = allocationSize + pageSize;

  uintptr_t cursor = currentStackEnd();
  if (cursor == 0) {
    cursor = currentStackAddress();
  }
  if (!alignUp(cursor, pageSize, &cursor)) {
    return nullptr;
  }
  if (nextStackAddress > cursor) {
    cursor = nextStackAddress;
  }

  // The fast path avoids parsing /proc/self/maps when the first page after
  // the current stack is available. MAP_FIXED_NOREPLACE keeps this race-safe.
  if (void *mapping = mmapStack(cursor, mappingSize, pageSize)) {
    void *ptr = static_cast<uint8_t *>(mapping) + pageSize;
    rememberMMapAllocation(ptr, mapping, mappingSize);
    rememberNextStackAddress(cursor, mappingSize);
    return ptr;
  }

  std::vector<QBDI::MemoryMap> maps = QBDI::getCurrentProcessMaps(false);

  for (const QBDI::MemoryMap &map : maps) {
    if (map.range.end() <= cursor) {
      continue;
    }

    uintptr_t candidate = cursor;
    if (candidate < map.range.start() &&
        map.range.start() - candidate >= mappingSize) {
      if (void *mapping = mmapStack(candidate, mappingSize, pageSize)) {
        void *ptr = static_cast<uint8_t *>(mapping) + pageSize;
        rememberMMapAllocation(ptr, mapping, mappingSize);
        rememberNextStackAddress(candidate, mappingSize);
        return ptr;
      }
    }

    if (cursor < map.range.end()) {
      if (!alignUp(map.range.end(), pageSize, &cursor)) {
        return nullptr;
      }
    }
  }

  if (void *mapping = mmapStack(cursor, mappingSize, pageSize)) {
    void *ptr = static_cast<uint8_t *>(mapping) + pageSize;
    rememberMMapAllocation(ptr, mapping, mappingSize);
    rememberNextStackAddress(cursor, mappingSize);
    return ptr;
  }

  return nullptr;
}

#endif

} // namespace

namespace QBDI {

// C++ method
std::vector<std::string> getModuleNames() {
  std::set<std::string> modules;

  for (const MemoryMap &m : getCurrentProcessMaps(false))
    if (!m.name.empty())
      modules.insert(m.name);

  return {std::begin(modules), std::end(modules)};
}

void *alignedAlloc(size_t size, size_t align) {
  void *allocated = nullptr;
  // Alignment needs to be a power of 2
  if ((align == 0) || ((align & (align - 1)) != 0)) {
    return nullptr;
  }
#if defined(QBDI_PLATFORM_WINDOWS)
  allocated = _aligned_malloc(size, align);
#else
  int ret = posix_memalign(&allocated, align, size);
  if (ret != 0) {
    return nullptr;
  }
#endif
  return allocated;
}

void alignedFree(void *ptr) {
#if defined(QBDI_PLATFORM_WINDOWS)
  _aligned_free(ptr);
#else
#if defined(QBDI_PLATFORM_ANDROID) && defined(QBDI_ARCH_AARCH64)
  if (ptr != nullptr &&
      reinterpret_cast<uintptr_t>(ptr) % androidPageSize() == 0) {
    void *mapping = nullptr;
    size_t mappingSize = 0;
    if (forgetMMapAllocation(ptr, &mapping, &mappingSize)) {
      munmap(mapping, mappingSize);
      return;
    }
  }
#endif
  free(ptr);
#endif
}

bool allocateVirtualStack(GPRState *ctx, uint32_t stackSize, uint8_t **stack) {
  if (ctx == nullptr || stack == nullptr) {
    return false;
  }
  *stack = nullptr;
#if defined(QBDI_PLATFORM_ANDROID) && defined(QBDI_ARCH_AARCH64)
  (*stack) = static_cast<uint8_t *>(
      allocateAndroidStackAboveCurrentStack(static_cast<size_t>(stackSize)));
#else
  (*stack) = static_cast<uint8_t *>(alignedAlloc(stackSize, 16));
#endif
  if (*stack == nullptr) {
#if defined(QBDI_PLATFORM_ANDROID) && defined(QBDI_ARCH_AARCH64)
    QBDI_WARN(
        "Failed to allocate an Android virtual stack above the current "
        "thread stack");
#endif
    return false;
  }

  QBDI_GPR_SET(ctx, REG_SP, reinterpret_cast<QBDI::rword>(*stack) + stackSize);
  QBDI_GPR_SET(ctx, REG_BP, QBDI_GPR_GET(ctx, REG_SP));

  return true;
}

void simulateCall(GPRState *ctx, rword returnAddress,
                  const std::vector<rword> &args) {
  simulateCallA(ctx, returnAddress, args.size(), args.data());
}

void simulateCallV(GPRState *ctx, rword returnAddress, uint32_t argNum,
                   va_list ap) {
  std::vector<rword> args(argNum);
  for (uint32_t i = 0; i < argNum; i++) {
    args[i] = va_arg(ap, rword);
  }
  simulateCallA(ctx, returnAddress, argNum, args.data());
}

void simulateCallA(GPRState *ctx, rword returnAddress, uint32_t argNum,
                   const rword *args) {
  uint32_t i = 0;
  uint32_t argsoff = 0;
  uint32_t limit = FRAME_LENGTH;

  // Allocate arguments frame
  QBDI_GPR_SET(ctx, REG_SP,
               QBDI_GPR_GET(ctx, REG_SP) - FRAME_LENGTH * sizeof(rword));

  // Handle the return address
#if defined(QBDI_ARCH_X86_64) || defined(QBDI_ARCH_X86)
  QBDI_GPR_SET(ctx, REG_SP, QBDI_GPR_GET(ctx, REG_SP) - sizeof(rword));
  *(rword *)(QBDI_GPR_GET(ctx, REG_SP)) = returnAddress;
  argsoff++;
#elif defined(QBDI_ARCH_ARM) || defined(QBDI_ARCH_AARCH64)
  QBDI_DEBUG("Set LR to: 0x{:x}", returnAddress);
  ctx->lr = returnAddress;
#endif

#define UNSTACK_ARG(REG)  \
  if (i < argNum) {       \
    ctx->REG = args[i++]; \
  }
#if defined(QBDI_ARCH_X86_64)
#if defined(QBDI_PLATFORM_WINDOWS)
  // Shadow space
  argsoff += 4;
  // Register args
  UNSTACK_ARG(rcx);
  UNSTACK_ARG(rdx);
  UNSTACK_ARG(r8);
  UNSTACK_ARG(r9);
#else
  // Register args
  UNSTACK_ARG(rdi);
  UNSTACK_ARG(rsi);
  UNSTACK_ARG(rdx);
  UNSTACK_ARG(rcx);
  UNSTACK_ARG(r8);
  UNSTACK_ARG(r9);
#endif // OS
#elif defined(QBDI_ARCH_X86)
  // no register used
#elif defined(QBDI_ARCH_ARM)
  UNSTACK_ARG(r0);
  UNSTACK_ARG(r1);
  UNSTACK_ARG(r2);
  UNSTACK_ARG(r3);
#elif defined(QBDI_ARCH_AARCH64)
  UNSTACK_ARG(x0);
  UNSTACK_ARG(x1);
  UNSTACK_ARG(x2);
  UNSTACK_ARG(x3);
  UNSTACK_ARG(x4);
  UNSTACK_ARG(x5);
  UNSTACK_ARG(x6);
  UNSTACK_ARG(x7);
#endif // ARCH
#undef UNSTACK_ARG
  limit -= argsoff;

  // Push remaining args on the stack
  rword *frame = (rword *)QBDI_GPR_GET(ctx, REG_SP);
  for (uint32_t j = 0; (i + j) < argNum && j < limit; j++) {
    frame[argsoff + j] = args[i + j];
  }
}

// C method
qbdi_MemoryMap *convert_MemoryMap_to_C(std::vector<MemoryMap> maps,
                                       size_t *size) {
  if (size == NULL)
    return NULL;
  *size = maps.size();
  if (*size == 0) {
    return NULL;
  }
  qbdi_MemoryMap *cmaps =
      (qbdi_MemoryMap *)malloc(*size * sizeof(qbdi_MemoryMap));
  QBDI_REQUIRE_ABORT(cmaps != NULL, "Allocation Fail");
  for (size_t i = 0; i < *size; i++) {
    cmaps[i].start = maps[i].range.start();
    cmaps[i].end = maps[i].range.end();
    cmaps[i].permission = static_cast<qbdi_Permission>(maps[i].permission);
    cmaps[i].name = strdup(maps[i].name.c_str());
  }
  return cmaps;
}

qbdi_MemoryMap *qbdi_getRemoteProcessMaps(rword pid, bool full_path,
                                          size_t *size) {
  if (size == NULL)
    return NULL;
  return convert_MemoryMap_to_C(getRemoteProcessMaps(pid, full_path), size);
}

qbdi_MemoryMap *qbdi_getCurrentProcessMaps(bool full_path, size_t *size) {
  if (size == NULL)
    return NULL;
  return convert_MemoryMap_to_C(getCurrentProcessMaps(full_path), size);
}

void qbdi_freeMemoryMapArray(qbdi_MemoryMap *arr, size_t size) {
  for (size_t i = 0; i < size; i++) {
    if (arr[i].name) {
      free(arr[i].name);
    }
  }
  free(arr);
}

char **qbdi_getModuleNames(size_t *size) {
  if (size == NULL)
    return NULL;
  std::vector<std::string> modules = getModuleNames();
  *size = modules.size();
  if (*size == 0) {
    return NULL;
  }
  char **names = (char **)malloc(modules.size() * sizeof(char *));
  QBDI_REQUIRE_ABORT(names != NULL, "Allocation Fail");
  for (size_t i = 0; i < modules.size(); i++) {
    names[i] = strdup(modules[i].c_str());
  }
  return names;
}

void *qbdi_alignedAlloc(size_t size, size_t align) {
  return alignedAlloc(size, align);
}

void qbdi_alignedFree(void *ptr) { alignedFree(ptr); }

bool qbdi_allocateVirtualStack(GPRState *ctx, uint32_t stackSize,
                               uint8_t **stack) {
  return allocateVirtualStack(ctx, stackSize, stack);
}

void qbdi_simulateCall(GPRState *ctx, rword returnAddress, uint32_t argNum,
                       ...) {
  va_list ap;
  // Handle the arguments
  va_start(ap, argNum);
  simulateCallV(ctx, returnAddress, argNum, ap);
  va_end(ap);
}
void qbdi_simulateCallV(GPRState *ctx, rword returnAddress, uint32_t argNum,
                        va_list ap) {
  simulateCallV(ctx, returnAddress, argNum, ap);
}

void qbdi_simulateCallA(GPRState *ctx, rword returnAddress, uint32_t argNum,
                        const rword *args) {
  simulateCallA(ctx, returnAddress, argNum, args);
}

} // namespace QBDI
