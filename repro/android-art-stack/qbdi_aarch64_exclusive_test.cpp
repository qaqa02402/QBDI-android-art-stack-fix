#include <inttypes.h>
#include <stdint.h>

#include <cstdio>
#include <vector>

#include <QBDI.h>

namespace {

constexpr uint32_t kVirtualStackSize = 0x100000;

struct AccessStats {
  QBDI::rword address;
  size_t reads = 0;
  size_t writes = 0;
  QBDI::rword readValue = 0;
  QBDI::rword writeValue = 0;
};

QBDI::VMAction recordAccess(QBDI::VMInstanceRef vm, QBDI::GPRState *,
                            QBDI::FPRState *, void *data) {
  AccessStats *stats = static_cast<AccessStats *>(data);
  for (const QBDI::MemoryAccess &access : vm->getInstMemoryAccess()) {
    if (access.accessAddress != stats->address || access.size != 8) {
      continue;
    }
    if ((access.type & QBDI::MEMORY_READ) != 0) {
      stats->reads++;
      stats->readValue = access.value;
    }
    if ((access.type & QBDI::MEMORY_WRITE) != 0) {
      stats->writes++;
      stats->writeValue = access.value;
    }
  }
  return QBDI::VMAction::CONTINUE;
}

__attribute__((noinline)) uint32_t exclusiveStoreSuccess(uint64_t *value) {
  uint64_t next = 0;
  uint32_t status = 0;
  asm volatile(
      "ldxr %0, [%2]\n"
      "add %0, %0, #1\n"
      "stxr %w1, %0, [%2]\n"
      : "=&r"(next), "=&r"(status)
      : "r"(value)
      : "memory");
  return status;
}

__attribute__((noinline)) uint32_t exclusiveStoreAfterClrex(uint64_t *value) {
  uint64_t next = 0;
  uint32_t status = 0;
  asm volatile(
      "ldxr %0, [%2]\n"
      "add %0, %0, #1\n"
      "clrex\n"
      "stxr %w1, %0, [%2]\n"
      : "=&r"(next), "=&r"(status)
      : "r"(value)
      : "memory");
  return status;
}

using Target = uint32_t (*)(uint64_t *);

bool runCase(const char *name, Target target, uint32_t expectedStatus,
             uint64_t expectedValue, size_t expectedWrites) {
  uint64_t value = 0x20;
  AccessStats stats{reinterpret_cast<QBDI::rword>(&value)};
  QBDI::VM vm;
  vm.recordMemoryAccess(QBDI::MEMORY_READ_WRITE);
  vm.addMemAccessCB(QBDI::MEMORY_READ_WRITE, recordAccess, &stats);

  if (!vm.addInstrumentedModuleFromAddr(
          reinterpret_cast<QBDI::rword>(target))) {
    std::printf("[%s] failed to instrument target\n", name);
    return false;
  }

  uint8_t *stack = nullptr;
  if (!QBDI::allocateVirtualStack(vm.getGPRState(), kVirtualStackSize,
                                  &stack)) {
    std::printf("[%s] failed to allocate virtual stack\n", name);
    return false;
  }

  QBDI::rword result = 0;
  bool callOk = vm.call(&result, reinterpret_cast<QBDI::rword>(target),
                        {reinterpret_cast<QBDI::rword>(&value)});
  QBDI::alignedFree(stack);

  std::printf("[%s] call=%d status=%" PRIu64 " value=0x%" PRIx64
              " reads=%zu writes=%zu read=0x%" PRIx64 " write=0x%" PRIx64 "\n",
              name, callOk, static_cast<uint64_t>(result), value, stats.reads,
              stats.writes, static_cast<uint64_t>(stats.readValue),
              static_cast<uint64_t>(stats.writeValue));

  return callOk && result == expectedStatus && value == expectedValue &&
         stats.reads == 1 && stats.writes == expectedWrites &&
         stats.readValue == 0x20 &&
         (expectedWrites == 0 || stats.writeValue == expectedValue);
}

} // namespace

int main() {
  bool success =
      runCase("exclusive-success", exclusiveStoreSuccess, 0, 0x21, 1);
  bool failed =
      runCase("exclusive-after-clrex", exclusiveStoreAfterClrex, 1, 0x20, 0);
  std::printf("[exclusive-summary] success=%d failed-write-filtered=%d\n",
              success, failed);
  return success && failed ? 0 : 1;
}
