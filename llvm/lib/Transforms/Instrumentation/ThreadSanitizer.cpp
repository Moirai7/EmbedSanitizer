//===-- ThreadSanitizer.cpp - race detector -------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer, a race detector.
//
// The tool is under development, for the details about previous versions see
// http://code.google.com/p/data-race-test
//
// The instrumentation phase is quite simple:
//   - Insert calls to run-time library before every memory access.
//      - Optimizations may apply to avoid instrumenting some of the accesses.
//   - Insert calls at function entry/exit.
// The rest is handled by the run-time library.
//===----------------------------------------------------------------------===//


#include "llvm/Transforms/Instrumentation.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/EscapeEnumerator.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "EmbedSanitizerExtension.h"
#include "EmbedSanitizerDebugInfo.h"

using namespace llvm;

#define DEBUG_TYPE "tsan"

static cl::opt<bool> ClInstrumentMemoryAccesses(
    "tsan-instrument-memory-accesses", cl::init(true),
    cl::desc("Instrument memory accesses"), cl::Hidden);
static cl::opt<bool> ClInstrumentFuncEntryExit(
    "tsan-instrument-func-entry-exit", cl::init(true),
    cl::desc("Instrument function entry and exit"), cl::Hidden);
static cl::opt<bool> ClHandleCxxExceptions(
    "tsan-handle-cxx-exceptions", cl::init(true),
    cl::desc("Handle C++ exceptions (insert cleanup blocks for unwinding)"),
    cl::Hidden);
static cl::opt<bool> ClInstrumentAtomics(
    "tsan-instrument-atomics", cl::init(true),
    cl::desc("Instrument atomics"), cl::Hidden);
static cl::opt<bool> ClInstrumentMemIntrinsics(
    "tsan-instrument-memintrinsics", cl::init(true),
    cl::desc("Instrument memintrinsics (memset/memcpy/memmove)"), cl::Hidden);

STATISTIC(NumInstrumentedReads, "Number of instrumented reads");
STATISTIC(NumInstrumentedWrites, "Number of instrumented writes");
STATISTIC(NumOmittedReadsBeforeWrite,
          "Number of reads ignored due to following writes");
STATISTIC(NumAccessesWithBadSize, "Number of accesses with bad size");
STATISTIC(NumInstrumentedVtableWrites, "Number of vtable ptr writes");
STATISTIC(NumInstrumentedVtableReads, "Number of vtable ptr reads");
STATISTIC(NumOmittedReadsFromConstantGlobals,
          "Number of reads from constant globals");
STATISTIC(NumOmittedReadsFromVtable, "Number of vtable reads");
STATISTIC(NumOmittedNonCaptured, "Number of accesses ignored due to capturing");

static const char *const kTsanModuleCtorName = "tsan.module_ctor";
static const char *const kTsanInitName = "__tsan_init";

namespace
{

  /// ThreadSanitizer: instrument the code in module to find races.
  struct ThreadSanitizer : public FunctionPass
  {
    ThreadSanitizer() : FunctionPass(ID) {}
    StringRef getPassName() const override;
    void getAnalysisUsage(AnalysisUsage &AU) const override;
    bool runOnFunction(Function &F) override;
    bool doInitialization(Module &M) override;
    static char ID; // Pass identification, replacement for typeid.

  private:
    void initializeCallbacks(Module &M);
    bool instrumentLoadOrStore(Instruction *I, const DataLayout &DL);
    bool instrumentAtomic(Instruction *I, const DataLayout &DL);
    bool instrumentMemIntrinsic(Instruction *I);
    void chooseInstructionsToInstrument(SmallVectorImpl<Instruction *> &Local,
                                        SmallVectorImpl<Instruction *> &All,
                                        const DataLayout &DL);
    bool addrPointsToConstantData(Value *Addr);
    int getMemoryAccessFuncIndex(Value *Addr, const DataLayout &DL);
    void InsertRuntimeIgnores(Function &F);

    Type *IntptrTy;
    IntegerType *OrdTy;
    // Callbacks to run-time library are computed in doInitialization.
    Function *TsanFuncEntry;
    Function *TsanFuncExit;
    Function *TsanMainFuncExit; // exit of main function
    Function *TsanIgnoreBegin;
    Function *TsanIgnoreEnd;
    // Callbacks by Lan
    Function *TsanPrintVariables;
    // Accesses sizes are powers of two: 1, 2, 4, 8, 16.
    static const size_t kNumberOfAccessSizes = 5;
    Function *TsanRead[kNumberOfAccessSizes];
    Function *TsanWrite[kNumberOfAccessSizes];
    Function *TsanUnalignedRead[kNumberOfAccessSizes];
    Function *TsanUnalignedWrite[kNumberOfAccessSizes];
    Function *TsanAtomicLoad[kNumberOfAccessSizes];
    Function *TsanAtomicStore[kNumberOfAccessSizes];
    Function *TsanAtomicRMW[AtomicRMWInst::LAST_BINOP + 1][kNumberOfAccessSizes];
    Function *TsanAtomicCAS[kNumberOfAccessSizes];
    Function *TsanAtomicThreadFence;
    Function *TsanAtomicSignalFence;
    Function *TsanVptrUpdate;
    Function *TsanVptrLoad;
    Function *MemmoveFn, *MemcpyFn, *MemsetFn;
    Function *TsanCtorFunction;
  };
} // namespace

char ThreadSanitizer::ID = 0;
INITIALIZE_PASS_BEGIN(
    ThreadSanitizer, "tsan",
    "EmbedSanitizer: detects data races.",
    false, false)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(
    ThreadSanitizer, "tsan",
    "EmbedSanitizer: detects data races.",
    false, false)

StringRef ThreadSanitizer::getPassName() const { return "EmbedSanitizer"; }

void ThreadSanitizer::getAnalysisUsage(AnalysisUsage &AU) const
{
  AU.addRequired<TargetLibraryInfoWrapperPass>();
}

FunctionPass *llvm::createThreadSanitizerPass()
{
  return new ThreadSanitizer();
}

void ThreadSanitizer::initializeCallbacks(Module &M)
{
  IRBuilder<> IRB(M.getContext());
  AttributeSet Attr;
  Attr = Attr.addAttribute(M.getContext(), AttributeSet::FunctionIndex, Attribute::NoUnwind);

  // Initialize the callbacks.
  // Lan: callback func 的入口
  // 定义在runtime里面 interface.cc
  auto& context		= M.getContext();
	auto* helpTy	= FunctionType::get(Type::getVoidTy(context), {Type::getInt32Ty(context), Type::getInt8PtrTy(context)}, false);
  TsanPrintVariables = dyn_cast<Function>(M.getOrInsertFunction("__tsan_print_variables", helpTy));

  TsanMainFuncExit = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
      "__tsan_main_func_exit", Attr, IRB.getVoidTy(), nullptr));
  TsanFuncEntry = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
      "__tsan_func_entry", Attr, IRB.getVoidTy(), IRB.getInt8PtrTy(), nullptr));
  TsanFuncExit = checkSanitizerInterfaceFunction(
      M.getOrInsertFunction("__tsan_func_exit", Attr, IRB.getVoidTy(), nullptr));
  TsanIgnoreBegin = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
      "__tsan_ignore_thread_begin", Attr, IRB.getVoidTy(), nullptr));
  TsanIgnoreEnd = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
      "__tsan_ignore_thread_end", Attr, IRB.getVoidTy(), nullptr));
  OrdTy = IRB.getInt32Ty();
  for (size_t i = 0; i < kNumberOfAccessSizes; ++i)
  {
    const unsigned ByteSize = 1U << i;
    const unsigned BitSize = ByteSize * 8;
    std::string ByteSizeStr = utostr(ByteSize);
    std::string BitSizeStr = utostr(BitSize);
    SmallString<32> ReadName("__tsan_read" + ByteSizeStr);
    TsanRead[i] = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
        ReadName, Attr, IRB.getVoidTy(), IRB.getInt8PtrTy(),
        IRB.getInt8Ty(), IRB.getInt8PtrTy(), IRB.getInt8PtrTy(), nullptr));

    SmallString<32> WriteName("__tsan_write" + ByteSizeStr);
    TsanWrite[i] = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
        WriteName, Attr, IRB.getVoidTy(), IRB.getInt8PtrTy(),
        IRB.getInt8Ty(), IRB.getInt8PtrTy(), IRB.getInt8PtrTy(), nullptr));

    SmallString<64> UnalignedReadName("__tsan_unaligned_read" + ByteSizeStr);
    TsanUnalignedRead[i] =
        checkSanitizerInterfaceFunction(M.getOrInsertFunction(
            UnalignedReadName, Attr, IRB.getVoidTy(), IRB.getInt8PtrTy(),
            IRB.getInt8Ty(), IRB.getInt8PtrTy(), IRB.getInt8PtrTy(), nullptr));

    SmallString<64> UnalignedWriteName("__tsan_unaligned_write" + ByteSizeStr);
    TsanUnalignedWrite[i] =
        checkSanitizerInterfaceFunction(M.getOrInsertFunction(
            UnalignedWriteName, Attr, IRB.getVoidTy(), IRB.getInt8PtrTy(),
            IRB.getInt8Ty(), IRB.getInt8PtrTy(), IRB.getInt8PtrTy(), nullptr));

    Type *Ty = Type::getIntNTy(M.getContext(), BitSize);
    Type *PtrTy = Ty->getPointerTo();
    SmallString<32> AtomicLoadName("__tsan_atomic" + BitSizeStr + "_load");
    TsanAtomicLoad[i] = checkSanitizerInterfaceFunction(
        M.getOrInsertFunction(AtomicLoadName, Attr, Ty, PtrTy, OrdTy, nullptr));

    SmallString<32> AtomicStoreName("__tsan_atomic" + BitSizeStr + "_store");
    TsanAtomicStore[i] = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
        AtomicStoreName, Attr, IRB.getVoidTy(), PtrTy, Ty, OrdTy, nullptr));

    for (int op = AtomicRMWInst::FIRST_BINOP;
         op <= AtomicRMWInst::LAST_BINOP; ++op)
    {
      TsanAtomicRMW[op][i] = nullptr;
      const char *NamePart = nullptr;
      if (op == AtomicRMWInst::Xchg)
        NamePart = "_exchange";
      else if (op == AtomicRMWInst::Add)
        NamePart = "_fetch_add";
      else if (op == AtomicRMWInst::Sub)
        NamePart = "_fetch_sub";
      else if (op == AtomicRMWInst::And)
        NamePart = "_fetch_and";
      else if (op == AtomicRMWInst::Or)
        NamePart = "_fetch_or";
      else if (op == AtomicRMWInst::Xor)
        NamePart = "_fetch_xor";
      else if (op == AtomicRMWInst::Nand)
        NamePart = "_fetch_nand";
      else
        continue;
      SmallString<32> RMWName("__tsan_atomic" + itostr(BitSize) + NamePart);
      TsanAtomicRMW[op][i] = checkSanitizerInterfaceFunction(
          M.getOrInsertFunction(RMWName, Attr, Ty, PtrTy, Ty, OrdTy, nullptr));
    }

    SmallString<32> AtomicCASName("__tsan_atomic" + BitSizeStr +
                                  "_compare_exchange_val");
    TsanAtomicCAS[i] = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
        AtomicCASName, Attr, Ty, PtrTy, Ty, Ty, OrdTy, OrdTy, nullptr));
  }
  TsanVptrUpdate = checkSanitizerInterfaceFunction(
      M.getOrInsertFunction("__tsan_vptr_update", Attr, IRB.getVoidTy(),
                            IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
                            IRB.getInt8Ty(), IRB.getInt8PtrTy(), IRB.getInt8PtrTy(), nullptr));
  TsanVptrLoad = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
      "__tsan_vptr_read", Attr, IRB.getVoidTy(), IRB.getInt8PtrTy(),
      IRB.getInt8Ty(), IRB.getInt8PtrTy(), IRB.getInt8PtrTy(), nullptr));
  TsanAtomicThreadFence = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
      "__tsan_atomic_thread_fence", Attr, IRB.getVoidTy(), OrdTy, nullptr));
  TsanAtomicSignalFence = checkSanitizerInterfaceFunction(M.getOrInsertFunction(
      "__tsan_atomic_signal_fence", Attr, IRB.getVoidTy(), OrdTy, nullptr));

  MemmoveFn = checkSanitizerInterfaceFunction(
      M.getOrInsertFunction("memmove", Attr, IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
                            IRB.getInt8PtrTy(), IntptrTy, nullptr));
  MemcpyFn = checkSanitizerInterfaceFunction(
      M.getOrInsertFunction("memcpy", Attr, IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
                            IRB.getInt8PtrTy(), IntptrTy, nullptr));
  MemsetFn = checkSanitizerInterfaceFunction(
      M.getOrInsertFunction("memset", Attr, IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
                            IRB.getInt32Ty(), IntptrTy, nullptr));
}

bool ThreadSanitizer::doInitialization(Module &M)
{
  const DataLayout &DL = M.getDataLayout();
  IntptrTy = DL.getIntPtrType(M.getContext());
  std::tie(TsanCtorFunction, std::ignore) = createSanitizerCtorAndInitFunctions(
      M, kTsanModuleCtorName, kTsanInitName, /*InitArgTypes=*/{},
      /*InitArgs=*/{});

  appendToGlobalCtors(M, TsanCtorFunction, 0);

  return true;
}

static bool isVtableAccess(Instruction *I)
{
  if (MDNode *Tag = I->getMetadata(LLVMContext::MD_tbaa))
    return Tag->isTBAAVtableAccess();
  return false;
}

// Do not instrument known races/"benign races" that come from compiler
// instrumentatin. The user has no way of suppressing them.
// Lan: 需要修改这里么 判断哪些address 是sensitive 怎么修改 
static bool shouldInstrumentReadWriteFromAddress(Value *Addr)
{
  // Peel off GEPs and BitCasts.
  Addr = Addr->stripInBoundsOffsets();

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr))
  {
    if (GV->hasSection())
    {
      StringRef SectionName = GV->getSection();
      // Check if the global is in the PGO counters section.
      if (SectionName.endswith(getInstrProfCountersSectionName(
              /*AddSegment=*/false)))
        return false;
    }

    // Check if the global is private gcov data.
    if (GV->getName().startswith("__llvm_gcov") ||
        GV->getName().startswith("__llvm_gcda"))
      return false;
  }

  // Do not instrument acesses from different address spaces; we cannot deal
  // with them.
  if (Addr)
  {
    Type *PtrTy = cast<PointerType>(Addr->getType()->getScalarType());
    if (PtrTy->getPointerAddressSpace() != 0)
      return false;
  }

  return true;
}

bool ThreadSanitizer::addrPointsToConstantData(Value *Addr)
{
  // If this is a GEP, just analyze its pointer operand.
  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Addr))
    Addr = GEP->getPointerOperand();
  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr))
  {
    if (GV->isConstant())
    {
      // Reads from constant globals can not race with any writes.
      NumOmittedReadsFromConstantGlobals++;
      return true;
    }
  }
  else if (LoadInst *L = dyn_cast<LoadInst>(Addr))
  {
    if (isVtableAccess(L))
    {
      // Reads from a vtable pointer can not race with any writes.
      NumOmittedReadsFromVtable++;
      return true;
    }
  }
  return false;
}

// Instrumenting some of the accesses may be proven redundant.
// Currently handled:
//  - read-before-write (within same BB, no calls between)
//  - not captured variables
//
// We do not handle some of the patterns that should not survive
// after the classic compiler optimizations.
// E.g. two reads from the same temp should be eliminated by CSE,
// two writes should be eliminated by DSE, etc.
//
// 'Local' is a vector of insns within the same BB (no calls between).
// 'All' is a vector of insns that will be instrumented.
void ThreadSanitizer::chooseInstructionsToInstrument(
    SmallVectorImpl<Instruction *> &Local, SmallVectorImpl<Instruction *> &All,
    const DataLayout &DL)
{
  SmallSet<Value *, 8> WriteTargets;
  // Iterate from the end.
  for (Instruction *I : reverse(Local))
  {
    // Lan: instruction 是不是store
    // Lan: 我们需要修改么？得看看是不是能准确得判断是不是读写
    IRBuilder<> IRB(I);
    if (StoreInst *Store = dyn_cast<StoreInst>(I))
    {
      Value *Addr = Store->getPointerOperand();
      IRB.CreateCall(TsanPrintVariables, {IRB.getInt32(391),
                                  IRB.CreatePointerCast(Addr, IRB.getInt8PtrTy()),
                                  IRB.CreateIntCast(EmbedSanitizer::getLineNumber(I), IRB.getInt8Ty(), false),
                                  EmbedSanitizer::getObjectName(Addr, I, DL)});
      if (!shouldInstrumentReadWriteFromAddress(Addr))
        continue;
      WriteTargets.insert(Addr);
    }
    else
    {
      LoadInst *Load = cast<LoadInst>(I);
      Value *Addr = Load->getPointerOperand();
      IRB.CreateCall(TsanPrintVariables, {IRB.getInt32(403),
                                  IRB.CreatePointerCast(Addr, IRB.getInt8PtrTy()),
                                  IRB.CreateIntCast(EmbedSanitizer::getLineNumber(I), IRB.getInt8Ty(), false),
                                  EmbedSanitizer::getObjectName(Addr, I, DL)});
      if (!shouldInstrumentReadWriteFromAddress(Addr))
        continue;
      if (WriteTargets.count(Addr))
      {
        // We will write to this temp, so no reason to analyze the read.
        NumOmittedReadsBeforeWrite++;
        continue;
      }
      if (addrPointsToConstantData(Addr))
      {
        // Addr points to some constant data -- it can not race with any writes.
        continue;
      }
    }
    Value *Addr = isa<StoreInst>(*I)
                      ? cast<StoreInst>(I)->getPointerOperand()
                      : cast<LoadInst>(I)->getPointerOperand();
    if (isa<AllocaInst>(GetUnderlyingObject(Addr, DL)) &&
        !PointerMayBeCaptured(Addr, true, true))
    {
      // The variable is addressable but not captured, so it cannot be
      // referenced from a different thread and participate in a data race
      // (see llvm/Analysis/CaptureTracking.h for details).
      NumOmittedNonCaptured++;
      continue;
    }
    All.push_back(I);
  }
  Local.clear();
}

static bool isAtomic(Instruction *I)
{
  if (LoadInst *LI = dyn_cast<LoadInst>(I))
    return LI->isAtomic() && LI->getSynchScope() == CrossThread;
  if (StoreInst *SI = dyn_cast<StoreInst>(I))
    return SI->isAtomic() && SI->getSynchScope() == CrossThread;
  if (isa<AtomicRMWInst>(I))
    return true;
  if (isa<AtomicCmpXchgInst>(I))
    return true;
  if (isa<FenceInst>(I))
    return true;
  return false;
}

void ThreadSanitizer::InsertRuntimeIgnores(Function &F)
{
  IRBuilder<> IRB(F.getEntryBlock().getFirstNonPHI());
  IRB.CreateCall(TsanIgnoreBegin);
  EscapeEnumerator EE(F, "tsan_ignore_cleanup", ClHandleCxxExceptions);
  while (IRBuilder<> *AtExit = EE.Next())
  {
    AtExit->CreateCall(TsanIgnoreEnd);
  }
}

// Lan: 找到所有的func
bool ThreadSanitizer::runOnFunction(Function &F)
{
  // This is required to prevent instrumenting call to __tsan_init from within
  // the module constructor.
  if (&F == TsanCtorFunction)
    return false;
  initializeCallbacks(*F.getParent());
  SmallVector<Instruction *, 8> AllLoadsAndStores;
  SmallVector<Instruction *, 8> LocalLoadsAndStores;
  SmallVector<Instruction *, 8> AtomicAccesses;
  SmallVector<Instruction *, 8> MemIntrinCalls;
  bool Res = false;
  bool HasCalls = false;
  bool SanitizeFunction = F.hasFnAttribute(Attribute::SanitizeThread);
  const DataLayout &DL = F.getParent()->getDataLayout();
  const TargetLibraryInfo *TLI =
      &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();

  // Traverse all instructions, collect loads/stores/returns, check for calls.
  for (auto &BB : F)
  {
    for (auto &Inst : BB)
    {

      if (isAtomic(&Inst)) {
        llvm::errs() << Inst <<" is atomic \n";
        AtomicAccesses.push_back(&Inst);
      } else if (isa<LoadInst>(Inst) || isa<StoreInst>(Inst)) {
        llvm::errs() << Inst <<" is load or store \n";
        LocalLoadsAndStores.push_back(&Inst);
      } else if (isa<CallInst>(Inst) || isa<InvokeInst>(Inst))
      {
        llvm::errs() << Inst <<" is call or invoke \n";
        // EmbedSanitizer modification:
        if (CallInst *CI = dyn_cast<CallInst>(&Inst))
        {
          // EmbedSanitizer: check for synchronizations
          // Lan: 决定了哪里是线程开始
          EmbedSanitizer::InstrIfSynchronization(Inst);

          maybeMarkSanitizerLibraryCallNoBuiltin(CI, TLI);
        }
        if (isa<MemIntrinsic>(Inst))
        {
          MemIntrinCalls.push_back(&Inst);
        }
        HasCalls = true;
        chooseInstructionsToInstrument(LocalLoadsAndStores, AllLoadsAndStores,
                                       DL);
      }
    }
    chooseInstructionsToInstrument(LocalLoadsAndStores, AllLoadsAndStores, DL);
  }

  // We have collected all loads and stores.
  // FIXME: many of these accesses do not need to be checked for races
  // (e.g. variables that do not escape, etc).
  // TODO Lan: 我们可以通过func名字 来确定哪些func可能race 比如irq;或者划定不同的优先级

  // Instrument memory accesses only if we want to report bugs in the function.
  // Lan: 这里不知道值怎么修改的 SanitizeFunction=1
  //llvm::errs() << ClInstrumentMemoryAccesses <<" looking for "<<SanitizeFunction<<" \n";
  if (ClInstrumentMemoryAccesses && SanitizeFunction)
    for (auto Inst : AllLoadsAndStores)
    {
      Res |= instrumentLoadOrStore(Inst, DL);
    }

  // Instrument atomic memory accesses in any case (they can be used to
  // implement synchronization).
  if (ClInstrumentAtomics)
    for (auto Inst : AtomicAccesses)
    {
      Res |= instrumentAtomic(Inst, DL);
    }

  if (ClInstrumentMemIntrinsics && SanitizeFunction)
    for (auto Inst : MemIntrinCalls)
    {
      Res |= instrumentMemIntrinsic(Inst);
    }

  // Lan: 不知道这些Attribute怎么定义的
  if (F.hasFnAttribute("sanitize_thread_no_checking_at_run_time"))
  {
    llvm::errs() << EmbedSanitizer::getFuncName(F) << " " << F.hasFnAttribute("sanitize_thread_no_checking_at_run_time")<<"\n";
    assert(!F.hasFnAttribute(Attribute::SanitizeThread));
    if (HasCalls)
      InsertRuntimeIgnores(F);
  }

  // Instrument function entry/exit points if there were instrumented accesses.
  if ((Res || HasCalls) && ClInstrumentFuncEntryExit)
  {
    IRBuilder<> IRB(F.getEntryBlock().getFirstNonPHI());
    // Value *ReturnAddress = IRB.CreateCall(
    //     Intrinsic::getDeclaration(F.getParent(), Intrinsic::returnaddress),
    //     IRB.getInt32(0));

    // IRB.CreateCall(TsanFuncEntry, ReturnAddress);
    //  Save function name as string into function body
    Value *func_name = EmbedSanitizer::getFuncName(F);
    IRB.CreateCall(TsanFuncEntry, {IRB.CreatePointerCast(func_name, IRB.getInt8PtrTy())});

    EscapeEnumerator EE(F, "tsan_cleanup", ClHandleCxxExceptions);
    while (IRBuilder<> *AtExit = EE.Next())
    {
      AtExit->CreateCall(TsanFuncExit, {IRB.CreatePointerCast(func_name, IRB.getInt8PtrTy())});
    }
    Res = true;

    // instrument main function to report races
    if (EmbedSanitizer::getFuncNameStr(F) == "main")
    {

      EscapeEnumerator Emain(F, "tsan_cleanup_report", ClHandleCxxExceptions);
      while (IRBuilder<> *AtExit = Emain.Next())
      {
        AtExit->CreateCall(TsanMainFuncExit,
                           {IRB.CreatePointerCast(func_name, IRB.getInt8PtrTy())});
      }
    }
  }
  return Res;
}

bool ThreadSanitizer::instrumentLoadOrStore(Instruction *I,
                                            const DataLayout &DL)
{
  IRBuilder<> IRB(I);
  // Lan: 这个write的定义是不是也需要我们自己写？
  bool IsWrite = isa<StoreInst>(*I);
  Value *Addr = IsWrite
                    ? cast<StoreInst>(I)->getPointerOperand()
                    : cast<LoadInst>(I)->getPointerOperand();

  // swifterror memory addresses are mem2reg promoted by instruction selection.
  // As such they cannot have regular uses like an instrumentation function and
  // it makes no sense to track them as memory.
  if (Addr->isSwiftError())
    return false;

  int Idx = getMemoryAccessFuncIndex(Addr, DL);
  if (Idx < 0)
    return false;
  if (IsWrite && isVtableAccess(I))
  {
    DEBUG(dbgs() << "  VPTR : " << *I << "\n");
    Value *StoredValue = cast<StoreInst>(I)->getValueOperand();
    // StoredValue may be a vector type if we are storing several vptrs at once.
    // In this case, just take the first element of the vector since this is
    // enough to find vptr races.
    // Lan: vptr(pointer to virtual function table)
    // Lan: https://static.googleusercontent.com/media/research.google.com/en//pubs/archive/35604.pdf
    if (isa<VectorType>(StoredValue->getType()))
      StoredValue = IRB.CreateExtractElement(
          StoredValue, ConstantInt::get(IRB.getInt32Ty(), 0));
    if (StoredValue->getType()->isIntegerTy())
      StoredValue = IRB.CreateIntToPtr(StoredValue, IRB.getInt8PtrTy());
    // Call TsanVptrUpdate.
    IRB.CreateCall(TsanVptrUpdate,
                   {IRB.CreatePointerCast(Addr, IRB.getInt8PtrTy()),
                    IRB.CreatePointerCast(StoredValue, IRB.getInt8PtrTy()),
                    IRB.CreateIntCast(EmbedSanitizer::getLineNumber(I), IRB.getInt8Ty(), false),
                    EmbedSanitizer::getObjectName(Addr, I, DL),
                    EmbedSanitizer::getFileName(I)});
    NumInstrumentedVtableWrites++;
    return true;
  }
  if (!IsWrite && isVtableAccess(I))
  {
    IRB.CreateCall(TsanVptrLoad,
                   {IRB.CreatePointerCast(Addr, IRB.getInt8PtrTy()),
                    IRB.CreateIntCast(EmbedSanitizer::getLineNumber(I), IRB.getInt8Ty(), false),
                    EmbedSanitizer::getObjectName(Addr, I, DL),
                    EmbedSanitizer::getFileName(I)});
    NumInstrumentedVtableReads++;
    return true;
  }
  const unsigned Alignment = IsWrite
                                 ? cast<StoreInst>(I)->getAlignment()
                                 : cast<LoadInst>(I)->getAlignment();
  Type *OrigTy = cast<PointerType>(Addr->getType())->getElementType();
  const uint32_t TypeSize = DL.getTypeStoreSizeInBits(OrigTy);
  Value *OnAccessFunc = nullptr;
  if (Alignment == 0 || Alignment >= 8 || (Alignment % (TypeSize / 8)) == 0)
    OnAccessFunc = IsWrite ? TsanWrite[Idx] : TsanRead[Idx];
  else
    OnAccessFunc = IsWrite ? TsanUnalignedWrite[Idx] : TsanUnalignedRead[Idx];
  IRB.CreateCall(OnAccessFunc, {IRB.CreatePointerCast(Addr, IRB.getInt8PtrTy()),
                                IRB.CreateIntCast(EmbedSanitizer::getLineNumber(I), IRB.getInt8Ty(), false),
                                EmbedSanitizer::getObjectName(Addr, I, DL),
                                EmbedSanitizer::getFileName(I)});

  if (IsWrite)
    NumInstrumentedWrites++;
  else
    NumInstrumentedReads++;
  return true;
}

static ConstantInt *createOrdering(IRBuilder<> *IRB, AtomicOrdering ord)
{
  uint32_t v = 0;
  switch (ord)
  {
  case AtomicOrdering::NotAtomic:
    llvm_unreachable("unexpected atomic ordering!");
  case AtomicOrdering::Unordered:
    LLVM_FALLTHROUGH;
  case AtomicOrdering::Monotonic:
    v = 0;
    break;
  // Not specified yet:
  // case AtomicOrdering::Consume:                v = 1; break;
  case AtomicOrdering::Acquire:
    v = 2;
    break;
  case AtomicOrdering::Release:
    v = 3;
    break;
  case AtomicOrdering::AcquireRelease:
    v = 4;
    break;
  case AtomicOrdering::SequentiallyConsistent:
    v = 5;
    break;
  }
  return IRB->getInt32(v);
}

// If a memset intrinsic gets inlined by the code gen, we will miss races on it.
// So, we either need to ensure the intrinsic is not inlined, or instrument it.
// We do not instrument memset/memmove/memcpy intrinsics (too complicated),
// instead we simply replace them with regular function calls, which are then
// intercepted by the run-time.
// Since tsan is running after everyone else, the calls should not be
// replaced back with intrinsics. If that becomes wrong at some point,
// we will need to call e.g. __tsan_memset to avoid the intrinsics.
// Lan: Free after use 的例子应该怎么做？
bool ThreadSanitizer::instrumentMemIntrinsic(Instruction *I)
{
  IRBuilder<> IRB(I);
  if (MemSetInst *M = dyn_cast<MemSetInst>(I))
  {
    IRB.CreateCall(
        MemsetFn,
        {IRB.CreatePointerCast(M->getArgOperand(0), IRB.getInt8PtrTy()),
         IRB.CreateIntCast(M->getArgOperand(1), IRB.getInt32Ty(), false),
         IRB.CreateIntCast(M->getArgOperand(2), IntptrTy, false)});
    I->eraseFromParent();
  }
  else if (MemTransferInst *M = dyn_cast<MemTransferInst>(I))
  {
    IRB.CreateCall(
        isa<MemCpyInst>(M) ? MemcpyFn : MemmoveFn,
        {IRB.CreatePointerCast(M->getArgOperand(0), IRB.getInt8PtrTy()),
         IRB.CreatePointerCast(M->getArgOperand(1), IRB.getInt8PtrTy()),
         IRB.CreateIntCast(M->getArgOperand(2), IntptrTy, false)});
    I->eraseFromParent();
  }
  return false;
}

// Both llvm and ThreadSanitizer atomic operations are based on C++11/C1x
// standards.  For background see C++11 standard.  A slightly older, publicly
// available draft of the standard (not entirely up-to-date, but close enough
// for casual browsing) is available here:
// http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2011/n3242.pdf
// The following page contains more background information:
// http://www.hpl.hp.com/personal/Hans_Boehm/c++mm/

bool ThreadSanitizer::instrumentAtomic(Instruction *I, const DataLayout &DL)
{
  IRBuilder<> IRB(I);
  if (LoadInst *LI = dyn_cast<LoadInst>(I))
  {
    Value *Addr = LI->getPointerOperand();
    int Idx = getMemoryAccessFuncIndex(Addr, DL);
    if (Idx < 0)
      return false;
    const unsigned ByteSize = 1U << Idx;
    const unsigned BitSize = ByteSize * 8;
    Type *Ty = Type::getIntNTy(IRB.getContext(), BitSize);
    Type *PtrTy = Ty->getPointerTo();
    Value *Args[] = {IRB.CreatePointerCast(Addr, PtrTy),
                     createOrdering(&IRB, LI->getOrdering())};
    Type *OrigTy = cast<PointerType>(Addr->getType())->getElementType();
    Value *C = IRB.CreateCall(TsanAtomicLoad[Idx], Args);
    Value *Cast = IRB.CreateBitOrPointerCast(C, OrigTy);
    I->replaceAllUsesWith(Cast);
  }
  else if (StoreInst *SI = dyn_cast<StoreInst>(I))
  {
    Value *Addr = SI->getPointerOperand();
    int Idx = getMemoryAccessFuncIndex(Addr, DL);
    if (Idx < 0)
      return false;
    const unsigned ByteSize = 1U << Idx;
    const unsigned BitSize = ByteSize * 8;
    Type *Ty = Type::getIntNTy(IRB.getContext(), BitSize);
    Type *PtrTy = Ty->getPointerTo();
    Value *Args[] = {IRB.CreatePointerCast(Addr, PtrTy),
                     IRB.CreateBitOrPointerCast(SI->getValueOperand(), Ty),
                     createOrdering(&IRB, SI->getOrdering())};
    CallInst *C = CallInst::Create(TsanAtomicStore[Idx], Args);
    ReplaceInstWithInst(I, C);
  }
  else if (AtomicRMWInst *RMWI = dyn_cast<AtomicRMWInst>(I))
  {
    Value *Addr = RMWI->getPointerOperand();
    int Idx = getMemoryAccessFuncIndex(Addr, DL);
    if (Idx < 0)
      return false;
    Function *F = TsanAtomicRMW[RMWI->getOperation()][Idx];
    if (!F)
      return false;
    const unsigned ByteSize = 1U << Idx;
    const unsigned BitSize = ByteSize * 8;
    Type *Ty = Type::getIntNTy(IRB.getContext(), BitSize);
    Type *PtrTy = Ty->getPointerTo();
    Value *Args[] = {IRB.CreatePointerCast(Addr, PtrTy),
                     IRB.CreateIntCast(RMWI->getValOperand(), Ty, false),
                     createOrdering(&IRB, RMWI->getOrdering())};
    CallInst *C = CallInst::Create(F, Args);
    ReplaceInstWithInst(I, C);
  }
  else if (AtomicCmpXchgInst *CASI = dyn_cast<AtomicCmpXchgInst>(I))
  {
    Value *Addr = CASI->getPointerOperand();
    int Idx = getMemoryAccessFuncIndex(Addr, DL);
    if (Idx < 0)
      return false;
    const unsigned ByteSize = 1U << Idx;
    const unsigned BitSize = ByteSize * 8;
    Type *Ty = Type::getIntNTy(IRB.getContext(), BitSize);
    Type *PtrTy = Ty->getPointerTo();
    Value *CmpOperand =
        IRB.CreateBitOrPointerCast(CASI->getCompareOperand(), Ty);
    Value *NewOperand =
        IRB.CreateBitOrPointerCast(CASI->getNewValOperand(), Ty);
    Value *Args[] = {IRB.CreatePointerCast(Addr, PtrTy),
                     CmpOperand,
                     NewOperand,
                     createOrdering(&IRB, CASI->getSuccessOrdering()),
                     createOrdering(&IRB, CASI->getFailureOrdering())};
    CallInst *C = IRB.CreateCall(TsanAtomicCAS[Idx], Args);
    Value *Success = IRB.CreateICmpEQ(C, CmpOperand);
    Value *OldVal = C;
    Type *OrigOldValTy = CASI->getNewValOperand()->getType();
    if (Ty != OrigOldValTy)
    {
      // The value is a pointer, so we need to cast the return value.
      OldVal = IRB.CreateIntToPtr(C, OrigOldValTy);
    }

    Value *Res =
        IRB.CreateInsertValue(UndefValue::get(CASI->getType()), OldVal, 0);
    Res = IRB.CreateInsertValue(Res, Success, 1);

    I->replaceAllUsesWith(Res);
    I->eraseFromParent();
  }
  else if (FenceInst *FI = dyn_cast<FenceInst>(I))
  {
    Value *Args[] = {createOrdering(&IRB, FI->getOrdering())};
    Function *F = FI->getSynchScope() == SingleThread ? TsanAtomicSignalFence : TsanAtomicThreadFence;
    CallInst *C = CallInst::Create(F, Args);
    ReplaceInstWithInst(I, C);
  }
  return true;
}

int ThreadSanitizer::getMemoryAccessFuncIndex(Value *Addr,
                                              const DataLayout &DL)
{
  Type *OrigPtrTy = Addr->getType();
  Type *OrigTy = cast<PointerType>(OrigPtrTy)->getElementType();
  assert(OrigTy->isSized());
  uint32_t TypeSize = DL.getTypeStoreSizeInBits(OrigTy);
  if (TypeSize != 8 && TypeSize != 16 &&
      TypeSize != 32 && TypeSize != 64 && TypeSize != 128)
  {
    NumAccessesWithBadSize++;
    // Ignore all unusual sizes.
    return -1;
  }
  size_t Idx = countTrailingZeros(TypeSize / 8);
  assert(Idx < kNumberOfAccessSizes);
  return Idx;
}
