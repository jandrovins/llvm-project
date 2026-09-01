// Verify the compiler-rt host codec without requiring GPU hardware.
// RUN: %inputgen-gpu-cc -std=c11 -I%inputgen-gpu-src -c \
// RUN:   %inputgen-gpu-src/inputgen_gpu_runtime_state.c -o %t.runtime.o
// RUN: %inputgen-gpu-cxx -std=c++17 -I%inputgen-gpu-include \
// RUN:   -I%inputgen-gpu-src %inputgen-gpu-src/host/InputGenGPUCodec.cpp \
// RUN:   %s %t.runtime.o -o %t
// RUN: %t %t.inputgen %t.bad | FileCheck %s
// REQUIRES: inputgen-gpu-host-test

#include "inputgen_gpu/InputGenGPUCodec.h"
#include "inputgen_gpu_runtime_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using namespace inputgen_gpu;

static int fail(const Error &Failure) {
  std::printf("unexpected-error=%s\n", Failure.message().c_str());
  return 1;
}

template <class T> static int fail(const Result<T> &Failure) {
  return fail(Failure.error());
}

int main(int Argc, char **Argv) {
  if (Argc != 3)
    return 1;

  FactoryConfig Config{Mode::Generate, 1, 1, 4096, 64, 2};
  auto GeneratedOrErr = createGenerationFactory(Config);
  if (!GeneratedOrErr)
    return fail(GeneratedOrErr);
  Factory Generated = std::move(GeneratedOrErr).value();

  void *Arguments = __ig_prepare_thread(Generated.data(), 8, 1);
  void *PointerSlot =
      __ig_pre_load(Arguments, 0, 8, 8, INPUTGEN_GPU_VALUE_POINTER);
  void *OldPointer = *reinterpret_cast<void **>(PointerSlot);
  int *Value = static_cast<int *>(
      __ig_pre_load(OldPointer, 0, 4, 4, INPUTGEN_GPU_VALUE_INTEGER));
  __ig_store_result(81, 4);
  std::printf("generated=%d\n", *Value);

  auto ResultsOrErr = inspectThreadResults(Generated);
  if (!ResultsOrErr)
    return fail(ResultsOrErr);
  std::printf(
      "result=%llu error=%u\n",
      static_cast<unsigned long long>(ResultsOrErr.value()[0].ResultBits),
      ResultsOrErr.value()[0].ErrorCode);

  auto RecordOrErr = serializeFactory(Generated);
  if (!RecordOrErr)
    return fail(RecordOrErr);
  if (Error Failure = writeRecord(Argv[1], RecordOrErr.value()))
    return fail(Failure);

  std::ifstream Input(Argv[1], std::ios::binary);
  std::vector<uint8_t> FileBytes((std::istreambuf_iterator<char>(Input)), {});
  const uint8_t *PointerBytes = reinterpret_cast<const uint8_t *>(&OldPointer);
  bool ContainsPointer =
      std::search(FileBytes.begin(), FileBytes.end(), PointerBytes,
                  PointerBytes + sizeof(OldPointer)) != FileBytes.end();
  std::printf("serialized-old-pointer=%d\n", ContainsPointer);

  auto ParsedOrErr = readRecord(Argv[1]);
  if (!ParsedOrErr)
    return fail(ParsedOrErr);
  ReplayRequest Conflict;
  Conflict.NumTeams = 2;
  auto ConflictOrErr = createReplayFactory(ParsedOrErr.value(), Conflict);
  std::printf("conflict=%d\n", !ConflictOrErr);

  auto ReplayOrErr = createReplayFactory(ParsedOrErr.value());
  if (!ReplayOrErr)
    return fail(ReplayOrErr);
  Factory Replay = std::move(ReplayOrErr).value();
  Arguments = __ig_prepare_thread(Replay.data(), 8, 1);
  PointerSlot = __ig_pre_load(Arguments, 0, 8, 8, INPUTGEN_GPU_VALUE_POINTER);
  void *NewPointer = *reinterpret_cast<void **>(PointerSlot);
  Value = static_cast<int *>(
      __ig_pre_load(NewPointer, 0, 4, 4, INPUTGEN_GPU_VALUE_INTEGER));
  std::printf("replay=%d relocated=%d\n", *Value, NewPointer != OldPointer);

  std::ofstream Bad(Argv[2], std::ios::binary);
  Bad << "bad";
  Bad.close();
  auto BadOrErr = readRecord(Argv[2]);
  std::printf("malformed=%d\n", !BadOrErr);

  Error OutputFailure = writeRecord(std::string(Argv[1]) + "/missing/output",
                                    RecordOrErr.value());
  std::printf("output-error=%d\n", bool(OutputFailure));
  return 0;
}

// CHECK: generated=9
// CHECK: result=81 error=0
// CHECK: serialized-old-pointer=0
// CHECK: conflict=1
// CHECK: replay=9 relocated=1
// CHECK: malformed=1
// CHECK: output-error=1
