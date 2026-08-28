//===-- InputGenGPU.cpp - InputGen GPU instrumentation pass ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/InputGenGPU.h"
#include "llvm/Transforms/IPO/Instrumentor.h"

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
};

std::string getInputGenGPUEntryPointName(StringRef EntryFunctionName) {
  return (Twine("__ig_entry_") + EntryFunctionName).str();
}

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

  // The v1 factory runtime uses AMDGPU intrinsics and an AMDGPU kernel ABI.
  const Triple &T = M.getTargetTriple();
  if (!T.isAMDGPU()) {
    IIRB.Ctx.diagnose(DiagnosticInfoInstrumentation(
        Twine("InputGen GPU objects currently require AMDGPU, not target '") +
            T.str() + "'",
        DS_Warning));
    return false;
  }

  // Reserve a deterministic wrapper name and avoid replacing an existing one.
  std::string EntryPointName = getInputGenGPUEntryPointName(EntryFunctionName);
  if (M.getNamedValue(EntryPointName)) {
    IIRB.Ctx.diagnose(DiagnosticInfoInstrumentation(
        Twine("inputgen entry point '") + EntryPointName + "' already exists",
        DS_Warning));
    return false;
  }

  // Lay out every runtime argument in object zero. Pointer target objects are
  // created lazily by the pointer-typed pre-load of their argument slots.
  const DataLayout &DL = M.getDataLayout();
  SmallVector<ArgumentLayout> Layouts;
  uint64_t ArgumentBytes = 0;
  uint32_t PointerArgumentCount = 0;
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
    if (Ty->isPointerTy())
      ++PointerArgumentCount;
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

  // The launcher passes one opaque factory context. Runtime-private state
  // derives this GPU thread's slice from it.
  FunctionType *EntryPointTy =
      FunctionType::get(IIRB.VoidTy, {IIRB.PtrTy}, /*isVarArg=*/false);
  Function *EntryPoint = Function::Create(
      EntryPointTy, GlobalValue::ExternalLinkage, EntryPointName, M);
  EntryPoint->setCallingConv(CallingConv::AMDGPU_KERNEL);
  EntryPoint->addFnAttr("instrument");

  Argument *Context = &*EntryPoint->arg_begin();
  Context->setName("context");

  // Start the wrapper body with a normal IRBuilder so its scalar loads remain
  // visible to the later Instrumentor pass.
  BasicBlock *EntryBB = BasicBlock::Create(IIRB.Ctx, "entry", EntryPoint);
  // Do not use InstrumentorIRBuilder here: its bookkeeping intentionally
  // skips pass-created instructions, while scalar argument loads need hooks.
  IRBuilder<> IRB(EntryBB);

  // Linearize the AMDGPU workgroup and workitem IDs into the unique factory
  // slice index owned by this GPU thread.
  FunctionCallee WorkgroupId = M.getOrInsertFunction(
      "llvm.amdgcn.workgroup.id.x",
      FunctionType::get(IIRB.Int32Ty, /*isVarArg=*/false));
  FunctionCallee WorkitemId = M.getOrInsertFunction(
      "llvm.amdgcn.workitem.id.x",
      FunctionType::get(IIRB.Int32Ty, /*isVarArg=*/false));
  Value *Workgroup = IRB.CreateZExt(IRB.CreateCall(WorkgroupId), IIRB.Int64Ty);
  Value *Workitem = IRB.CreateZExt(IRB.CreateCall(WorkitemId), IIRB.Int64Ty);

  // Ask the device runtime to initialize or validate this thread's slice and
  // return object zero, which stores the scalar argument bytes.
  FunctionCallee PrepareLane = M.getOrInsertFunction(
      "__ig_prepare_lane",
      FunctionType::get(
          IIRB.PtrTy,
          {IIRB.PtrTy, IIRB.Int64Ty, IIRB.Int64Ty, IIRB.Int64Ty, IIRB.Int32Ty},
          false));
  Value *ArgumentData =
      IRB.CreateCall(PrepareLane,
                     {Context, Workgroup, Workitem,
                      ConstantInt::get(IIRB.Int64Ty, ArgumentBytes),
                      ConstantInt::get(IIRB.Int32Ty, PointerArgumentCount)},
                     "inputgen.arguments");

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
  return true;
}

class InputGenGPUConfig final : public InstrumentationConfig {
public:
  explicit InputGenGPUConfig(EntryKernelInfo &Info) : Info(Info) {}

  // Select only generic-address-space accesses rooted in user arguments or the
  // argument object, leaving private allocas and output slots untouched.
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

    Value *Underlying =
        const_cast<Value *>(getUnderlyingObjectAggressive(Pointer));
    if (auto *Arg = dyn_cast<Argument>(Underlying))
      return Arg->getParent() == Info.EntryFunction;
    auto *Call = dyn_cast<CallBase>(Underlying);
    return Call && Call->getCalledFunction() &&
           Call->getCalledFunction()->getName() == "__ig_prepare_lane";
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
  }

  EntryKernelInfo &Info;
};

} // namespace

PreservedAnalyses InputGenGPUPass::run(Module &M, ModuleAnalysisManager &MAM) {
  if (InputGenGPUEntryFunction.empty())
    return PreservedAnalyses::all();

  InstrumentorIRBuilderTy IIRB(M);
  EntryKernelInfo Info;
  if (!createInputGenGPUEntryKernel(M, IIRB, InputGenGPUEntryFunction, Info))
    return PreservedAnalyses::all();

  InputGenGPUConfig IConf(Info);
  return InstrumentorPass(/*FS=*/nullptr, &IConf, &IIRB).run(M, MAM);
}
