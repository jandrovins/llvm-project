//===-- InputGenGPU.cpp - InputGen GPU instrumentation pass ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/InputGenGPU.h"
#include "llvm/Transforms/IPO/Instrumentor.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Instrumentation.h"

using namespace llvm;
using namespace llvm::instrumentor;

#define DEBUG_TYPE "inputgen-gpu"

static cl::opt<std::string> InputGenGPUEntryFunction(
    "inputgen-gpu-entry-function",
    cl::desc("Device function wrapped by the InputGen GPU entry kernel"),
    cl::init(""));

static cl::list<std::string> InputGenGPURuntimeBitcodes(
    "inputgen-gpu-runtime-bitcode",
    cl::desc("InputGen GPU runtime bitcode file; may be repeated"),
    cl::ZeroOrMore);

namespace {

struct ArgumentLayout {
  Argument *Arg;
  uint64_t Offset;
  uint64_t Size;
};

struct EntryKernelInfo {
  Function *Kernel = nullptr;
  Function *EntryFunction = nullptr;
  SmallPtrSet<Function *, 8> InstrumentedFunctions;
};

constexpr StringLiteral InputGenGPUEntryPointName = "__ig_entry";

// Accept scalar values the factory callbacks can fabricate byte-for-byte by
// checking both their kind and fixed store size.
bool isSupportedScalarType(Type *Ty, const DataLayout &DL) {
  if (!(Ty->isIntegerTy() || Ty->isFloatTy() || Ty->isDoubleTy()))
    return false;
  TypeSize Size = DL.getTypeStoreSize(Ty);
  if (!Size.isFixed())
    return false;
  switch (Size.getFixedValue()) {
  case 1:
  case 2:
  case 4:
  case 8:
    return true;
  default:
    return false;
  }
}

// Keep the device callback ABI independent of llvm::Type::TypeID.  The
// callbacks only support this small, deliberate type set.
uint32_t getInputGenGPUValueKind(Type *Ty) {
  if (Ty->isIntegerTy())
    return 1;
  if (Ty->isFloatTy())
    return 2;
  if (Ty->isDoubleTy())
    return 3;
  if (Ty->isPointerTy())
    return 4;
  llvm_unreachable("unsupported InputGen GPU callback value type");
}

void setInputGenGPUValueKindGetter(InstrumentationOpportunity &IO) {
  for (IRTArg &Arg : IO.IRTArgs) {
    if (Arg.Name != "value_type_id")
      continue;
    Arg.GetterCB = [](Value &V, Type &Ty, InstrumentationConfig &,
                      InstrumentorIRBuilderTy &) {
      Type *ValueTy = isa<LoadInst>(V)
                          ? cast<LoadInst>(V).getType()
                          : cast<StoreInst>(V).getValueOperand()->getType();
      return ConstantInt::get(cast<IntegerType>(&Ty),
                              getInputGenGPUValueKind(ValueTy));
    };
    return;
  }
  llvm_unreachable("InputGen GPU callback is missing value_type_id");
}

// Instrument the selected function and every direct, defined callee it can
// reach. Virtual pointers pass across those calls unchanged, so the callee's
// accesses need the same callbacks as the entry function's accesses.
bool collectInstrumentedFunctions(Function *EntryFn,
                                  InstrumentorIRBuilderTy &IIRB,
                                  EntryKernelInfo &Info) {
  SmallVector<Function *> Worklist{EntryFn};
  while (!Worklist.empty()) {
    Function *Fn = Worklist.pop_back_val();
    if (!Info.InstrumentedFunctions.insert(Fn).second)
      continue;

    for (Instruction &I : instructions(Fn)) {
      if (auto *Store = dyn_cast<StoreInst>(&I)) {
        Value *Destination = getUnderlyingObject(Store->getPointerOperand());
        bool IsKnownNonFactoryStorage =
            isa<AllocaInst>(Destination) || isa<GlobalVariable>(Destination);
        if (Store->getValueOperand()->getType()->isPointerTy() &&
            !IsKnownNonFactoryStorage) {
          IIRB.Ctx.diagnose(DiagnosticInfoInstrumentation(
              Twine("InputGen GPU does not yet support pointer stores in '") +
                  EntryFn->getName() +
                  "'; logical relation updates must be implemented first",
              DS_Warning));
          return false;
        }
      }
      auto *Call = dyn_cast<CallBase>(&I);
      if (!Call)
        continue;
      Function *Callee = Call->getCalledFunction();
      bool HasPointerArgument = any_of(Call->args(), [](Value *Arg) {
        return Arg->getType()->isPointerTy();
      });
      if (!Callee || Callee->isDeclaration()) {
        if (HasPointerArgument) {
          IIRB.Ctx.diagnose(DiagnosticInfoInstrumentation(
              Twine("InputGen GPU does not support passing pointer arguments "
                    "to indirect or external calls in '") +
                  EntryFn->getName() + "'",
              DS_Warning));
          return false;
        }
        continue;
      }
      Worklist.push_back(Callee);
    }
  }
  return true;
}

// Build the GPU wrapper that gives every lane a factory slice, loads scalar
// arguments from its argument object, and calls the user function.
bool createInputGenGPUEntryKernel(Module &M, InstrumentorIRBuilderTy &IIRB,
                                  StringRef EntryFunctionName,
                                  EntryKernelInfo &Info) {
  // Require a named, defined user function before constructing a wrapper.
  if (EntryFunctionName.empty())
    return false;

  Function *EntryFn = M.getFunction(EntryFunctionName);
  if (!EntryFn || EntryFn->isDeclaration()) {
    IIRB.Ctx.diagnose(DiagnosticInfoInstrumentation(
        Twine("inputgen entry function '") + EntryFunctionName +
            "' was not found or is only a declaration",
        DS_Warning));
    return false;
  }

  if (!collectInstrumentedFunctions(EntryFn, IIRB, Info))
    return false;

  // Select the target kernel ABI. The runtime obtains this GPU thread's
  // hardware coordinates when it resolves the factory slice.
  const Triple &T = M.getTargetTriple();
  CallingConv::ID KernelCallingConv;
  if (T.isAMDGPU()) {
    KernelCallingConv = CallingConv::AMDGPU_KERNEL;
  } else if (T.isNVPTX()) {
    KernelCallingConv = CallingConv::PTX_Kernel;
  } else {
    IIRB.Ctx.diagnose(DiagnosticInfoInstrumentation(
        Twine("InputGen GPU objects require AMDGPU or NVPTX, not target '") +
            T.str() + "'",
        DS_Warning));
    return false;
  }

  // Reserve a deterministic wrapper name and avoid replacing an existing one.
  if (M.getNamedValue(InputGenGPUEntryPointName)) {
    IIRB.Ctx.diagnose(DiagnosticInfoInstrumentation(
        Twine("inputgen entry point '") + InputGenGPUEntryPointName +
            "' already exists",
        DS_Warning));
    return false;
  }

  // Lay out every runtime argument in object zero. Pointer target objects are
  // created lazily by the pointer-typed pre-load of their argument slots.
  const DataLayout &DL = M.getDataLayout();
  SmallVector<ArgumentLayout> Layouts;
  uint64_t ArgumentBytes = 0;
  for (Argument &Arg : EntryFn->args()) {
    Type *Ty = Arg.getType();
    if (Ty->isPointerTy() && Ty->getPointerAddressSpace() != 0) {
      IIRB.Ctx.diagnose(DiagnosticInfoInstrumentation(
          Twine("InputGen GPU objects require generic pointer arguments in '") +
              EntryFunctionName + "'",
          DS_Warning));
      return false;
    }
    if (!Ty->isPointerTy() && !isSupportedScalarType(Ty, DL)) {
      IIRB.Ctx.diagnose(DiagnosticInfoInstrumentation(
          Twine("InputGen GPU objects do not support argument type in '") +
              EntryFunctionName + "'",
          DS_Warning));
      return false;
    }

    TypeSize StoreSize = DL.getTypeStoreSize(Ty);
    if (!StoreSize.isFixed())
      return false;
    ArgumentBytes = alignTo(ArgumentBytes, DL.getABITypeAlign(Ty).value());
    ArgumentLayout Layout{&Arg, ArgumentBytes, StoreSize.getFixedValue()};
    ArgumentBytes += Layout.Size;
    Layouts.push_back(Layout);
  }
  ArgumentBytes = alignTo(ArgumentBytes, uint64_t(8));

  // Results use one fixed-width driver slot per thread, so reject unsupported
  // return types before the wrapper is emitted.
  if (!EntryFn->getReturnType()->isVoidTy() &&
      !isSupportedScalarType(EntryFn->getReturnType(), DL)) {
    IIRB.Ctx.diagnose(DiagnosticInfoInstrumentation(
        Twine("InputGen GPU objects do not support the return type of '") +
            EntryFunctionName + "'",
        DS_Warning));
    return false;
  }

  // The launcher passes the opaque factory context followed by the current
  // OpenMP kernel-launch-environment slot. InputGen does not use the latter:
  // it is present solely to satisfy the current libomptarget kernel ABI and
  // must never become a generated InputGen object.
  FunctionType *EntryPointTy =
      FunctionType::get(IIRB.VoidTy, {IIRB.PtrTy, IIRB.PtrTy},
                        /*isVarArg=*/false);
  Function *EntryPoint = Function::Create(
      EntryPointTy, GlobalValue::ExternalLinkage, InputGenGPUEntryPointName, M);
  EntryPoint->setCallingConv(KernelCallingConv);
  EntryPoint->addFnAttr("instrument");

  Argument *Context = &*EntryPoint->arg_begin();
  Context->setName("context");
  EntryPoint->getArg(1)->setName("dyn_ptr");

  // Start the wrapper body with a normal IRBuilder so its scalar loads remain
  // visible to the later Instrumentor pass.
  BasicBlock *EntryBB = BasicBlock::Create(IIRB.Ctx, "entry", EntryPoint);
  // Do not use InstrumentorIRBuilder here: its bookkeeping intentionally
  // skips pass-created instructions, while scalar argument loads need hooks.
  IRBuilder<> IRB(EntryBB);

  // Ask the device runtime to initialize or validate this thread's slice and
  // return object zero, which stores the scalar argument bytes.
  FunctionCallee PrepareThread = M.getOrInsertFunction(
      "__ig_prepare_thread",
      FunctionType::get(
          IIRB.PtrTy, {IIRB.PtrTy, IIRB.Int64Ty}, false));
  Value *ArgumentData =
      IRB.CreateCall(PrepareThread,
                     {Context, ConstantInt::get(IIRB.Int64Ty, ArgumentBytes)},
                     "inputgen.arguments");

  // A failed slice initialization has no valid argument object. Return before
  // any wrapper load can dereference the null result; the device runtime keeps
  // its per-GPU-thread error for the launcher to copy back.
  BasicBlock *AbortBB =
      BasicBlock::Create(IIRB.Ctx, "inputgen.abort", EntryPoint);
  BasicBlock *ContinueBB =
      BasicBlock::Create(IIRB.Ctx, "inputgen.continue", EntryPoint);
  IRB.CreateCondBr(IRB.CreateIsNull(ArgumentData), AbortBB, ContinueBB);
  IRB.SetInsertPoint(AbortBB);
  IRB.CreateRetVoid();
  IRB.SetInsertPoint(ContinueBB);

  // Reconstruct every argument through ordinary loads from object zero.
  SmallVector<Value *> CallArguments;
  for (const ArgumentLayout &Layout : Layouts) {
    Value *Address =
        IRB.CreateInBoundsGEP(IIRB.Int8Ty, ArgumentData,
                              ConstantInt::get(IIRB.Int64Ty, Layout.Offset));
    CallArguments.push_back(
        IRB.CreateLoad(Layout.Arg->getType(), Address, Layout.Arg->getName()));
  }

  // Invoke the requested user function with factory-backed arguments.
  CallInst *Call =
      IRB.CreateCall(EntryFn->getFunctionType(), EntryFn, CallArguments);
  if (!EntryFn->getReturnType()->isVoidTy()) {
    Type *RetTy = EntryFn->getReturnType();
    Value *Bits =
        RetTy->isFloatingPointTy()
            ? IRB.CreateBitCast(
                  Call,
                  IntegerType::get(
                      IIRB.Ctx, DL.getTypeStoreSize(RetTy).getFixedValue() * 8))
            : IRB.CreateZExtOrTrunc(Call, IIRB.Int64Ty);
    if (Bits->getType() != IIRB.Int64Ty)
      Bits = IRB.CreateZExt(Bits, IIRB.Int64Ty);
    FunctionCallee StoreResult = M.getOrInsertFunction(
        "__ig_store_result",
        FunctionType::get(IIRB.VoidTy, {IIRB.Int64Ty, IIRB.Int32Ty}, false));
    IRB.CreateCall(
        StoreResult,
        {Bits, ConstantInt::get(IIRB.Int32Ty,
                                DL.getTypeStoreSize(RetTy).getFixedValue())});
  }
  // Complete the kernel and retain the values needed to filter result stores.
  IRB.CreateRetVoid();

  Info.Kernel = EntryPoint;
  Info.EntryFunction = EntryFn;
  Info.InstrumentedFunctions.insert(EntryPoint);
  return true;
}

class InputGenGPUConfig final : public InstrumentationConfig {
public:
  explicit InputGenGPUConfig(EntryKernelInfo &Info) : Info(Info) {}

  // Instrument supported generic-address-space accesses in the selected
  // direct-call closure. The callbacks determine whether an individual pointer
  // belongs to an InputGen object; restricting this to original arguments
  // would miss nested pointers and helper-function arguments.
  bool shouldInstrumentMemory(Instruction &I) const {
    Value *Pointer = nullptr;
    Type *ValueTy = nullptr;
    bool IsAtomic = false;
    bool IsVolatile = false;
    if (auto *Load = dyn_cast<LoadInst>(&I)) {
      Pointer = Load->getPointerOperand();
      ValueTy = Load->getType();
      IsAtomic = Load->isAtomic();
      IsVolatile = Load->isVolatile();
    } else if (auto *Store = dyn_cast<StoreInst>(&I)) {
      Pointer = Store->getPointerOperand();
      ValueTy = Store->getValueOperand()->getType();
      IsAtomic = Store->isAtomic();
      IsVolatile = Store->isVolatile();
    } else {
      return false;
    }

    bool IsPointerLoad = isa<LoadInst>(I) && ValueTy->isPointerTy();
    if ((!isSupportedScalarType(ValueTy, I.getModule()->getDataLayout()) &&
         !IsPointerLoad) ||
        Pointer->getType()->getPointerAddressSpace() != 0 || IsAtomic ||
        IsVolatile)
      return false;

    return Info.InstrumentedFunctions.contains(I.getFunction());
  }

private:
  // Configure callbacks to replace supported loads/stores with factory-aware
  // hooks while passing pointer, base object, size, alignment, and type.
  void populate(InstrumentorIRBuilderTy &IIRB) override {
    RuntimePrefix->setString("__ig_");
    HostEnabled->setBool(false);
    GPUEnabled->setBool(true);
    SmallVector<StringRef> RuntimeBitcodeRefs;
    for (StringRef RuntimeBitcode : InputGenGPURuntimeBitcodes)
      RuntimeBitcodeRefs.push_back(RuntimeBitcode);
    RuntimeBitcodes->setStringList(RuntimeBitcodeRefs);
    InlineRuntimeEagerly->setBool(false);

    LoadIO::ConfigTy LoadConfig(/*Enable=*/false);
    LoadConfig.set(LoadIO::PassPointer);
    LoadConfig.set(LoadIO::ReplacePointer);
    LoadConfig.set(LoadIO::PassPointerAS);
    LoadConfig.set(LoadIO::PassValueSize);
    LoadConfig.set(LoadIO::PassAlignment);
    LoadConfig.set(LoadIO::PassValueTypeId);
    auto *Load = InstrumentationConfig::allocate<LoadIO>(
        InstrumentationLocation::INSTRUCTION_PRE);
    Load->CB = [&](Value &V) {
      return shouldInstrumentMemory(cast<Instruction>(V));
    };
    Load->init(*this, IIRB, &LoadConfig);
    setInputGenGPUValueKindGetter(*Load);

    StoreIO::ConfigTy StoreConfig(/*Enable=*/false);
    StoreConfig.set(StoreIO::PassPointer);
    StoreConfig.set(StoreIO::ReplacePointer);
    StoreConfig.set(StoreIO::PassPointerAS);
    StoreConfig.set(StoreIO::PassStoredValueSize);
    StoreConfig.set(StoreIO::PassAlignment);
    StoreConfig.set(StoreIO::PassValueTypeId);
    auto *Store = InstrumentationConfig::allocate<StoreIO>(
        InstrumentationLocation::INSTRUCTION_PRE);
    Store->CB = [&](Value &V) {
      return shouldInstrumentMemory(cast<Instruction>(V));
    };
    Store->init(*this, IIRB, &StoreConfig);
    setInputGenGPUValueKindGetter(*Store);
  }

  EntryKernelInfo &Info;
};

// Stop an instrumented function before it executes a memory operation whose
// callback reported an error. Checks after direct calls propagate a helper's
// error back through the selected call closure until the wrapper returns.
void addInputGenGPUErrorPropagation(Module &M, EntryKernelInfo &Info) {
  FunctionCallee ErrorPending = M.getOrInsertFunction(
      "__ig_error_pending",
      FunctionType::get(Type::getInt32Ty(M.getContext()), false));

  for (Function *Fn : Info.InstrumentedFunctions) {
    SmallVector<CallInst *> GuardedCalls;
    for (Instruction &I : instructions(Fn)) {
      auto *Call = dyn_cast<CallInst>(&I);
      if (!Call)
        continue;
      Function *Callee = Call->getCalledFunction();
      if (!Callee)
        continue;
      if (Callee->getName() == "__ig_pre_load" ||
          Callee->getName() == "__ig_pre_store" ||
          Info.InstrumentedFunctions.contains(Callee))
        GuardedCalls.push_back(Call);
    }
    if (GuardedCalls.empty())
      continue;

    BasicBlock *ErrorBB =
        BasicBlock::Create(M.getContext(), "inputgen.error", Fn);
    IRBuilder<> ErrorBuilder(ErrorBB);
    if (Fn->getReturnType()->isVoidTy())
      ErrorBuilder.CreateRetVoid();
    else
      ErrorBuilder.CreateRet(Constant::getNullValue(Fn->getReturnType()));

    for (CallInst *Call : reverse(GuardedCalls)) {
      Instruction *Next = Call->getNextNode();
      if (!Next)
        continue;
      BasicBlock *CallBB = Call->getParent();
      BasicBlock *ContinueBB =
          CallBB->splitBasicBlock(Next, "inputgen.no-error");
      CallBB->getTerminator()->eraseFromParent();
      IRBuilder<> GuardBuilder(CallBB);
      Value *HasError = GuardBuilder.CreateICmpNE(
          GuardBuilder.CreateCall(ErrorPending), GuardBuilder.getInt32(0));
      GuardBuilder.CreateCondBr(HasError, ErrorBB, ContinueBB);
    }
  }
}

} // namespace

PreservedAnalyses InputGenGPUPass::run(Module &M, ModuleAnalysisManager &MAM) {
  if (InputGenGPUEntryFunction.empty())
    return PreservedAnalyses::all();

  InstrumentorIRBuilderTy IIRB(M);
  EntryKernelInfo Info;
  if (!createInputGenGPUEntryKernel(M, IIRB, InputGenGPUEntryFunction, Info))
    return PreservedAnalyses::all();

  InputGenGPUConfig IConf(Info);
  (void)InstrumentorPass(/*FS=*/nullptr, &IConf, &IIRB).run(M, MAM);
  addInputGenGPUErrorPropagation(M, Info);
  // Wrapper creation itself mutates the module even when no callback is
  // inserted, so the Instrumentor's change result alone is insufficient.
  return PreservedAnalyses::none();
}
