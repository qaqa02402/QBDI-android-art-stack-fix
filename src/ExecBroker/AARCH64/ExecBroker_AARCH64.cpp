/*
 * This file is part of QBDI.
 *
 * Copyright 2017 - 2025 Quarkslab
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
#include "QBDI/PtrAuth.h"

#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "QBDI/Memory.hpp"
#include "Engine/LLVMCPU.h"
#include "ExecBlock/ExecBlock.h"
#include "ExecBroker/ExecBroker.h"
#include "Patch/AARCH64/Layer2_AARCH64.h"
#include "Patch/AARCH64/PatchGenerator_AARCH64.h"
#include "Patch/AARCH64/RelocatableInst_AARCH64.h"
#include "Patch/Patch.h"
#include "Utility/LogSys.h"

#include "llvm/MC/MCInst.h"

namespace QBDI {
static const size_t SCAN_DISTANCE = 2;

#if defined(QBDI_PLATFORM_ANDROID)
namespace {

struct fpsimd_context {
  struct {
    uint32_t magic;
    uint32_t size;
  } head;
  uint32_t fpsr;
  uint32_t fpcr;
  __uint128_t vregs[32];
};

struct ProtectedPage {
  void *addr;
  size_t size;
  int prot;
};

struct TransferSignalState {
  ExecBroker *broker;
  GPRState *gprState;
  FPRState *fprState;
  sigjmp_buf jumpBuffer;
  bool trapped;
};

bool transferExecutionWithSignals(ExecBroker &broker, ExecBlock &transferBlock,
                                  const ExecBrokerArchData &archData,
                                  rword pageSize, rword addr, GPRState *gprState,
                                  FPRState *fprState);

std::mutex signalHandlerMutex;
size_t signalHandlerUsers = 0;
struct sigaction previousSigsegv = {};
struct sigaction previousSigbus = {};
thread_local TransferSignalState *activeTransferSignalState = nullptr;

uintptr_t getExecBlockRunAddress() {
  // Android AArch64 uses the Itanium C++ ABI. For a non-virtual member
  // function, the member-function pointer stores the target code address
  // directly in its first word.
  struct ItaniumMemberFunctionPointer {
    uintptr_t function;
    intptr_t adjustment;
  };

  auto run = &ExecBlock::run;
  ItaniumMemberFunctionPointer addr = {};
  static_assert(sizeof(run) == sizeof(addr));
  std::memcpy(&addr, &run, sizeof(addr));
  return addr.function;
}

uintptr_t alignDown(uintptr_t value, uintptr_t align) {
  return value & ~(align - 1);
}

uintptr_t alignUp(uintptr_t value, uintptr_t align) {
  return (value + align - 1) & ~(align - 1);
}

int toMProtect(Permission permission) {
  int prot = 0;
  if ((permission & PF_READ) != 0) {
    prot |= PROT_READ;
  }
  if ((permission & PF_WRITE) != 0) {
    prot |= PROT_WRITE;
  }
  if ((permission & PF_EXEC) != 0) {
    prot |= PROT_EXEC;
  }
  return prot;
}

void threadCtxToGPRState(const ucontext_t *uap, GPRState *gprState) {
  gprState->x0 = uap->uc_mcontext.regs[0];
  gprState->x1 = uap->uc_mcontext.regs[1];
  gprState->x2 = uap->uc_mcontext.regs[2];
  gprState->x3 = uap->uc_mcontext.regs[3];
  gprState->x4 = uap->uc_mcontext.regs[4];
  gprState->x5 = uap->uc_mcontext.regs[5];
  gprState->x6 = uap->uc_mcontext.regs[6];
  gprState->x7 = uap->uc_mcontext.regs[7];
  gprState->x8 = uap->uc_mcontext.regs[8];
  gprState->x9 = uap->uc_mcontext.regs[9];
  gprState->x10 = uap->uc_mcontext.regs[10];
  gprState->x11 = uap->uc_mcontext.regs[11];
  gprState->x12 = uap->uc_mcontext.regs[12];
  gprState->x13 = uap->uc_mcontext.regs[13];
  gprState->x14 = uap->uc_mcontext.regs[14];
  gprState->x15 = uap->uc_mcontext.regs[15];
  gprState->x16 = uap->uc_mcontext.regs[16];
  gprState->x17 = uap->uc_mcontext.regs[17];
  gprState->x18 = uap->uc_mcontext.regs[18];
  gprState->x19 = uap->uc_mcontext.regs[19];
  gprState->x20 = uap->uc_mcontext.regs[20];
  gprState->x21 = uap->uc_mcontext.regs[21];
  gprState->x22 = uap->uc_mcontext.regs[22];
  gprState->x23 = uap->uc_mcontext.regs[23];
  gprState->x24 = uap->uc_mcontext.regs[24];
  gprState->x25 = uap->uc_mcontext.regs[25];
  gprState->x26 = uap->uc_mcontext.regs[26];
  gprState->x27 = uap->uc_mcontext.regs[27];
  gprState->x28 = uap->uc_mcontext.regs[28];
  gprState->x29 = uap->uc_mcontext.regs[29];
  gprState->lr = uap->uc_mcontext.regs[30];
  gprState->sp = uap->uc_mcontext.sp;
  gprState->pc = uap->uc_mcontext.pc;
  gprState->nzcv = uap->uc_mcontext.pstate & 0xf0000000;
}

void floatCtxToFPRState(const ucontext_t *uap, FPRState *fprState) {
  const fpsimd_context *fuap =
      reinterpret_cast<const fpsimd_context *>(&(uap->uc_mcontext.__reserved));

  fprState->v0 = fuap->vregs[0];
  fprState->v1 = fuap->vregs[1];
  fprState->v2 = fuap->vregs[2];
  fprState->v3 = fuap->vregs[3];
  fprState->v4 = fuap->vregs[4];
  fprState->v5 = fuap->vregs[5];
  fprState->v6 = fuap->vregs[6];
  fprState->v7 = fuap->vregs[7];
  fprState->v8 = fuap->vregs[8];
  fprState->v9 = fuap->vregs[9];
  fprState->v10 = fuap->vregs[10];
  fprState->v11 = fuap->vregs[11];
  fprState->v12 = fuap->vregs[12];
  fprState->v13 = fuap->vregs[13];
  fprState->v14 = fuap->vregs[14];
  fprState->v15 = fuap->vregs[15];
  fprState->v16 = fuap->vregs[16];
  fprState->v17 = fuap->vregs[17];
  fprState->v18 = fuap->vregs[18];
  fprState->v19 = fuap->vregs[19];
  fprState->v20 = fuap->vregs[20];
  fprState->v21 = fuap->vregs[21];
  fprState->v22 = fuap->vregs[22];
  fprState->v23 = fuap->vregs[23];
  fprState->v24 = fuap->vregs[24];
  fprState->v25 = fuap->vregs[25];
  fprState->v26 = fuap->vregs[26];
  fprState->v27 = fuap->vregs[27];
  fprState->v28 = fuap->vregs[28];
  fprState->v29 = fuap->vregs[29];
  fprState->v30 = fuap->vregs[30];
  fprState->v31 = fuap->vregs[31];
  fprState->fpcr = fuap->fpcr;
  fprState->fpsr = fuap->fpsr;
}

void dispatchPreviousSignal(const struct sigaction &previous, int signo,
                            siginfo_t *info, void *ucontext) {
  if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
    previous.sa_sigaction(signo, info, ucontext);
    return;
  }
  if (previous.sa_handler == SIG_IGN) {
    return;
  }
  if (previous.sa_handler != nullptr && previous.sa_handler != SIG_DFL) {
    previous.sa_handler(signo);
    return;
  }

  signal(signo, SIG_DFL);
  raise(signo);
  _exit(128 + signo);
}

void brokerExecSignalHandler(int signo, siginfo_t *info, void *ucontext) {
  TransferSignalState *state = activeTransferSignalState;
  if (state == nullptr) {
    dispatchPreviousSignal(signo == SIGSEGV ? previousSigsegv : previousSigbus,
                           signo, info, ucontext);
    return;
  }

  auto *uap = static_cast<ucontext_t *>(ucontext);
  rword pc = strip_ptrauth(uap->uc_mcontext.pc);
  if (!state->broker->isInstrumented(pc)) {
    dispatchPreviousSignal(signo == SIGSEGV ? previousSigsegv : previousSigbus,
                           signo, info, ucontext);
    return;
  }

  threadCtxToGPRState(uap, state->gprState);
  floatCtxToFPRState(uap, state->fprState);
  state->trapped = true;
  siglongjmp(state->jumpBuffer, 1);
}

bool installExecSignalHandlers() {
  std::lock_guard<std::mutex> lock(signalHandlerMutex);
  if (signalHandlerUsers != 0) {
    signalHandlerUsers++;
    return true;
  }

  struct sigaction action = {};
  action.sa_sigaction = brokerExecSignalHandler;
  action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&action.sa_mask);

  if (sigaction(SIGSEGV, &action, &previousSigsegv) != 0) {
    return false;
  }
  if (sigaction(SIGBUS, &action, &previousSigbus) != 0) {
    sigaction(SIGSEGV, &previousSigsegv, nullptr);
    return false;
  }

  signalHandlerUsers = 1;
  return true;
}

void uninstallExecSignalHandlers() {
  std::lock_guard<std::mutex> lock(signalHandlerMutex);
  if (signalHandlerUsers == 0) {
    return;
  }
  signalHandlerUsers--;
  if (signalHandlerUsers != 0) {
    return;
  }
  sigaction(SIGSEGV, &previousSigsegv, nullptr);
  sigaction(SIGBUS, &previousSigbus, nullptr);
}

bool shouldExcludeProtectedPage(uintptr_t pageStart, rword pageSize) {
  const uintptr_t handlerPage =
      alignDown(reinterpret_cast<uintptr_t>(&brokerExecSignalHandler), pageSize);
  const uintptr_t execBlockRunPage =
      alignDown(getExecBlockRunAddress(), pageSize);
  const uintptr_t installPage =
      alignDown(reinterpret_cast<uintptr_t>(&installExecSignalHandlers), pageSize);
  const uintptr_t uninstallPage = alignDown(
      reinterpret_cast<uintptr_t>(&uninstallExecSignalHandlers), pageSize);
  const uintptr_t transferPage = alignDown(
      reinterpret_cast<uintptr_t>(&transferExecutionWithSignals), pageSize);
  return pageStart == handlerPage || pageStart == execBlockRunPage ||
         pageStart == installPage || pageStart == uninstallPage ||
         pageStart == transferPage;
}

std::string getHostModuleName() {
  Dl_info info;
  if (dladdr(reinterpret_cast<void *>(getExecBlockRunAddress()), &info) == 0 ||
      info.dli_fname == nullptr) {
    return {};
  }
  return info.dli_fname;
}

bool collectProtectedPages(const ExecBroker &broker, rword pageSize,
                           std::vector<ProtectedPage> &pages) {
  const uintptr_t hostCodeStartPage =
      alignDown(getExecBlockRunAddress(), pageSize);
  const std::string hostModuleName = getHostModuleName();

  for (const MemoryMap &map : getCurrentProcessMaps(true)) {
    if ((map.permission & PF_EXEC) == 0) {
      continue;
    }

    for (const Range<rword> &range : broker.getInstrumentedRange().getRanges()) {
      if (!range.overlaps(map.range)) {
        continue;
      }

      Range<rword> overlap = range.intersect(map.range);
      uintptr_t start = alignDown(overlap.start(), pageSize);
      uintptr_t end = alignUp(overlap.end(), pageSize);
      int prot = toMProtect(map.permission);

      for (uintptr_t page = start; page < end; page += pageSize) {
        if (!hostModuleName.empty() && map.name == hostModuleName &&
            page >= hostCodeStartPage) {
          continue;
        }
        if (shouldExcludeProtectedPage(page, pageSize)) {
          continue;
        }
        if (!pages.empty() &&
            reinterpret_cast<uintptr_t>(pages.back().addr) + pages.back().size ==
                page &&
            pages.back().prot == prot) {
          pages.back().size += pageSize;
          continue;
        }
        pages.push_back(
            {reinterpret_cast<void *>(page), static_cast<size_t>(pageSize), prot});
      }
    }
  }
  return !pages.empty();
}

bool setPagesExecutable(const std::vector<ProtectedPage> &pages, bool executable) {
  for (const ProtectedPage &page : pages) {
    int prot = executable ? page.prot : (page.prot & ~PROT_EXEC);
    if (mprotect(page.addr, page.size, prot) != 0) {
      return false;
    }
  }
  return true;
}

bool transferExecutionWithSignals(ExecBroker &broker, ExecBlock &transferBlock,
                                  const ExecBrokerArchData &archData,
                                  rword pageSize, rword addr, GPRState *gprState,
                                  FPRState *fprState) {
  std::vector<ProtectedPage> pages;
  if (!collectProtectedPages(broker, pageSize, pages)) {
    return false;
  }
  if (!installExecSignalHandlers()) {
    return false;
  }
  if (!setPagesExecutable(pages, false)) {
    uninstallExecSignalHandlers();
    return false;
  }

  TransferSignalState state = {&broker, gprState, fprState, {}, false};
  activeTransferSignalState = &state;

  if (sigsetjmp(state.jumpBuffer, 1) == 0) {
    transferBlock.selectSeq(archData.transfertX28.seqID);
    transferBlock.getContext()->gprState = *gprState;
    transferBlock.getContext()->fprState = *fprState;
    transferBlock.getContext()->hostState.brokerAddr = addr;
    transferBlock.run();

    activeTransferSignalState = nullptr;
    setPagesExecutable(pages, true);
    uninstallExecSignalHandlers();
    return false;
  }

  activeTransferSignalState = nullptr;
  setPagesExecutable(pages, true);
  uninstallExecSignalHandlers();
  return state.trapped;
}

} // namespace
#endif

void ExecBroker::initExecBrokerSequences(const LLVMCPUs &llvmCPUs) {
  llvm::MCInst nopInst = nop();
  const LLVMCPU &llvmCPU = llvmCPUs.getCPU(CPUMode::DEFAULT);

  // Sequence Broker with LR
  // =======================
  {
    QBDI_DEBUG("Create Sequence Broker LR");
    QBDI::Patch::Vec brokerLR;
    brokerLR.push_back(QBDI::Patch(nopInst, 0x4, 4, llvmCPU));
    Patch &pLR = brokerLR[0];
    pLR.finalize = true;

    // target BTIj
    pLR.append(RelocTag::unique(RelocTagPatchBegin));
    pLR.append(GenBTI().genReloc(llvmCPU));

    // Prepare to Jump
    // 1. Store jumpAddress in LR
    pLR.append(
        Ldr(Reg(REG_LR), Offset(offsetof(Context, hostState.brokerAddr))));

    // 2. backup TPIDR and restore X28 and SR
    pLR.append(FullRegisterRestore(true).genReloc(llvmCPU));

    // 3. Jump to the target. Use BLR to change LR to the hook return address
    pLR.append(Blr(Reg(REG_LR)));

    // use RelocTagPatchInstEnd to mark the return address
    pLR.append(RelocTag::unique(RelocTagPatchInstBegin));
    pLR.append(RelocTag::unique(RelocTagPatchInstEnd));

    // Hook
    // Backup X28 and SR and reset SR to base addr
    pLR.append(FullRegisterReset(true).genReloc(llvmCPU));

    // This sequence doesn't need a terminator
    pLR.setModifyPC(true);

    // Write the Patch in the ExecBlock
    SeqWriteResult res =
        transferBlock->writeSequence(brokerLR.begin(), brokerLR.end());

    QBDI_REQUIRE_ABORT(res.patchWritten == 1, "Fail to write Sequence Broker");

    uint16_t instID = transferBlock->getSeqStart(res.seqID);

    std::vector<TagInfo> retAddr =
        transferBlock->queryTagByInst(instID, RelocTagPatchInstEnd);
    QBDI_REQUIRE(retAddr.size() == 1);

    archData.transfertLR.seqID = res.seqID;
    archData.transfertLR.hook = transferBlock->getAddressTag(retAddr[0]);
    QBDI_DEBUG("Sequence Broker LR: id={} hook=0x{:x}",
               archData.transfertLR.seqID, archData.transfertLR.hook);
  }

  // Sequence Broker with X28
  // ========================
  {
    QBDI_DEBUG("Create Sequence Broker X28");
    QBDI::Patch::Vec brokerX28;
    brokerX28.push_back(QBDI::Patch(nopInst, 0x4, 4, llvmCPU));
    Patch &pX28 = brokerX28[0];
    pX28.finalize = true;

    // target BTIj
    pX28.append(RelocTag::unique(RelocTagPatchBegin));
    pX28.append(GenBTI().genReloc(llvmCPU));

    // Prepare to Jump
    // 1. Store jumpAddress in X28
    pX28.append(Ldr(Reg(28), Offset(offsetof(Context, hostState.brokerAddr))));

    // 2. Restore ScratchRegister
    pX28.append(FullRegisterRestore(false).genReloc(llvmCPU));

    // 3. Jump to the target using X28.
    // Don't link as LR does'nt have a return address
    pX28.append(Br(Reg(28)));

    // use RelocTagPatchInstEnd to mark the return address
    pX28.append(RelocTag::unique(RelocTagPatchInstBegin));
    pX28.append(RelocTag::unique(RelocTagPatchInstEnd));

    // Hook
    // 1. Backup SR and reset it to base address
    pX28.append(FullRegisterReset(false).genReloc(llvmCPU));

    // This sequence doesn't need a terminator
    pX28.setModifyPC(true);

    // Write the Patch in the ExecBlock
    SeqWriteResult res =
        transferBlock->writeSequence(brokerX28.begin(), brokerX28.end());
    QBDI_REQUIRE_ABORT(res.patchWritten == 1, "Fail to write Sequence Broker");

    uint16_t instID = transferBlock->getSeqStart(res.seqID);

    std::vector<TagInfo> retAddr =
        transferBlock->queryTagByInst(instID, RelocTagPatchInstEnd);
    QBDI_REQUIRE(retAddr.size() == 1);

    archData.transfertX28.seqID = res.seqID;
    archData.transfertX28.hook = transferBlock->getAddressTag(retAddr[0]);

    QBDI_DEBUG("Sequence Broker X28: id={} hook=0x{:x}",
               archData.transfertX28.seqID, archData.transfertX28.hook);
  }
}

rword *ExecBroker::getReturnPoint(GPRState *gprState) const {
  auto ptr = reinterpret_cast<rword *>(gprState->sp);

  if (isInstrumented(strip_ptrauth(gprState->lr))) {
    QBDI_DEBUG("Found instrumented return address in LR register");
    return &(gprState->lr);
  }

  for (size_t i = 0; i < SCAN_DISTANCE; ++i) {
    if (isInstrumented(strip_ptrauth(ptr[i]))) {
      QBDI_DEBUG("Found instrumented return address on the stack at {:p}",
                 reinterpret_cast<void *>(&(ptr[i])));
      return &(ptr[i]);
    }
  }

  QBDI_DEBUG("LR register does not contain an instrumented return address");
  return nullptr;
}

bool ExecBroker::transferExecution(rword addr, GPRState *gprState,
                                   FPRState *fprState) {

#if defined(QBDI_PLATFORM_ANDROID)
  if (transferExecutionWithSignals(*this, *transferBlock, archData, pageSize,
                                   addr, gprState, fprState)) {
    return true;
  }
#endif

  // Search all return address
  rword *ptr = getReturnPoint(gprState);
  rword *ptr2 = nullptr;
  rword returnAddress = *ptr;

  if constexpr (is_linux) {

    /* On Linux, the resolution of a plt symbol may use the follow code:
     *
     * 0000000000000820 <.plt>:
     *  820:   a9bf7bf0        stp     x16, x30, [sp, #-16]!
     *  824:   90000090        adrp    x16, 10000 <__FRAME_END__+0xf430>
     *  828:   f947fe11        ldr     x17, [x16, #4088]
     *  82c:   913fe210        add     x16, x16, #0xff8
     *  830:   d61f0220        br      x17
     *  834:   d503201f        nop
     *  838:   d503201f        nop
     *  83c:   d503201f        nop
     *
     * 0000000000000840 <__cxa_finalize@plt>:
     *  840:   b0000090        adrp    x16, 11000 <__cxa_finalize@GLIBC_2.17>
     *  844:   f9400211        ldr     x17, [x16]
     *  848:   91000210        add     x16, x16, #0x0
     *  84c:   d61f0220        br      x17
     *
     * If the symbol isn't resolved, the return address is in lr (x30) register
     * and on the stack. We need to change both of them.
     */
    if (ptr == &(gprState->lr)) {
      rword *sp = reinterpret_cast<rword *>(gprState->sp);

      for (size_t i = 0; i < SCAN_DISTANCE; ++i) {
        if (sp[i] == returnAddress) {
          QBDI_DEBUG("TransferExecution: Found return address also at {:}",
                     reinterpret_cast<void *>(&(sp[i])));

          ptr2 = &(sp[i]);
          break;
        }
      }
    }
  }
  rword hook;

  if (ptr == &(gprState->lr)) {
    hook = archData.transfertLR.hook;
    QBDI_DEBUG(
        "TransferExecution: Used LR as a jumpRegister. "
        "Return address 0x{:} replace with 0x{:x}",
        returnAddress, hook);
    transferBlock->selectSeq(archData.transfertLR.seqID);
  } else {
    hook = archData.transfertX28.hook;
    QBDI_DEBUG(
        "TransferExecution: Used x28 as a jumpRegister. "
        "Return address 0x{:} replace with 0x{:x}",
        returnAddress, hook);
    transferBlock->selectSeq(archData.transfertX28.seqID);
  }

  // set fake return address
  *ptr = hook;
  if (ptr2 != nullptr) {
    *ptr2 = hook;
  }

  // Write transfer state
  transferBlock->getContext()->gprState = *gprState;
  transferBlock->getContext()->fprState = *fprState;

  // Set the jump address in hostate.brokerAddr
  transferBlock->getContext()->hostState.brokerAddr = addr;

  // Execute transfer
  QBDI_DEBUG("Transfering execution to 0x{:x} using transferBlock 0x{:x}", addr,
             reinterpret_cast<uintptr_t>(&*transferBlock));

  transferBlock->run();

  // Read transfer result
  *gprState = transferBlock->getContext()->gprState;
  *fprState = transferBlock->getContext()->fprState;

  // Restore original return
  QBDI_GPR_SET(gprState, REG_PC, returnAddress);
  if (QBDI_GPR_GET(gprState, REG_LR) == hook) {
    QBDI_GPR_SET(gprState, REG_LR, returnAddress);
  }
  if constexpr (is_linux) {
    if (ptr2 != nullptr and *ptr2 == hook) {
      *ptr2 = returnAddress;
    }
  }

  return true;
}

} // namespace QBDI
