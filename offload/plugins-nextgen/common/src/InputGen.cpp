//===- InputGen.cpp - InputGen runtime kernel hooks -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InputGenInterface.hpp"
#include "PluginInterface.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "inputgen"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <vector>

static bool getBoolEnvar(const char *Name, bool Default) {
  const char *Value = std::getenv(Name);
  if (!Value || !Value[0])
    return Default;

  return llvm::StringSwitch<bool>(Value)
      .Cases({"0", "false", "FALSE", "off", "OFF", "no", "NO"}, false)
      .Default(true);
}

using namespace llvm;
using namespace omp;
using namespace target;
using namespace plugin;
using namespace error;

namespace {

bool isInputGenEnabled() {
  static const bool Enabled = getBoolEnvar(INPUTGEN_ENABLE_ENVVAR, false);
  return Enabled;
}

void warnMissingDeviceGlobal(GenericDeviceTy &Device,
                             const GenericKernelTy &Kernel, const char *Name,
                             Error Err) {
  errs() << "inputgen GPU runtime: warning: device image";
  if (Kernel.getName())
    errs() << " for kernel " << Kernel.getName();
  errs() << " is missing global '" << Name << "'";
  if (Err)
    errs() << " (" << toString(std::move(Err)) << ")";
  errs() << "; InputGen is disabled for this launch\n";
}

Expected<GlobalTy> getDeviceGlobal(GenericDeviceTy &Device, DeviceImageTy &Image,
                                   const char *Name, uint32_t Size = 0) {
  GlobalTy DeviceGlobal(Name, Size);
  if (auto Err =
          Device.Plugin.getGlobalHandler().getGlobalMetadataFromDevice(
              Device, Image, DeviceGlobal))
    return std::move(Err);
  return DeviceGlobal;
}

Error writeUInt64Global(GenericDeviceTy &Device, const GlobalTy &DeviceGlobal,
                        uint64_t Value) {
  StaticGlobalTy<uint64_t> HostGlobal(DeviceGlobal.getName().c_str(), Value);
  return Device.Plugin.getGlobalHandler().writeGlobalToDevice(
      Device, HostGlobal, DeviceGlobal);
}

Error readUInt64Global(GenericDeviceTy &Device, const GlobalTy &DeviceGlobal,
                       uint64_t &Value) {
  StaticGlobalTy<uint64_t> HostGlobal(DeviceGlobal.getName().c_str(), 0u);
  if (auto Err = Device.Plugin.getGlobalHandler().readGlobalFromDevice(
          Device, HostGlobal, DeviceGlobal))
    return Err;
  Value = HostGlobal.getValue();
  return Plugin::success();
}

} // namespace

namespace llvm {
namespace omp {
namespace target {
namespace plugin {
namespace inputgen {

Error beforeKernelLaunch(GenericDeviceTy &Device,
                         const GenericKernelTy &Kernel) {
  LLVM_DEBUG(dbgs() << "beforeKernelLaunch: inputgen enabled...? " << isInputGenEnabled());
  if (!isInputGenEnabled())
    return Plugin::success();

  DeviceImageTy &Image = Kernel.getImage();
  auto OffsetOrErr = getDeviceGlobal(Device, Image,
                                     INPUTGEN_STRING_BUFFER_OFFSET_SYMBOL,
                                     sizeof(uint64_t));
  if (!OffsetOrErr) {
    warnMissingDeviceGlobal(Device, Kernel, INPUTGEN_STRING_BUFFER_OFFSET_SYMBOL,
                            OffsetOrErr.takeError());
    return Plugin::success();
  }

  auto RecordsOrErr = getDeviceGlobal(Device, Image,
                                      INPUTGEN_STRING_BUFFER_RECORDS_SYMBOL,
                                      sizeof(uint64_t));
  if (!RecordsOrErr) {
    warnMissingDeviceGlobal(Device, Kernel,
                            INPUTGEN_STRING_BUFFER_RECORDS_SYMBOL,
                            RecordsOrErr.takeError());
    return Plugin::success();
  }

  auto DroppedOrErr = getDeviceGlobal(Device, Image,
                                      INPUTGEN_STRING_BUFFER_DROPPED_SYMBOL,
                                      sizeof(uint64_t));
  if (!DroppedOrErr) {
    warnMissingDeviceGlobal(Device, Kernel,
                            INPUTGEN_STRING_BUFFER_DROPPED_SYMBOL,
                            DroppedOrErr.takeError());
    return Plugin::success();
  }

  if (auto Err = writeUInt64Global(Device, *OffsetOrErr, 0u))
    return Err;
  if (auto Err = writeUInt64Global(Device, *RecordsOrErr, 0u))
    return Err;
  return writeUInt64Global(Device, *DroppedOrErr, 0u);
}

Error afterKernelLaunch(GenericDeviceTy &Device, const GenericKernelTy &Kernel,
                        AsyncInfoWrapperTy &AsyncInfoWrapper,
                        bool AlreadySynchronized) {
  LLVM_DEBUG(dbgs() << "afterKernelLaunch: inputgen enabled...? " << isInputGenEnabled());
  if (!isInputGenEnabled())
    return Plugin::success();

  if (!AlreadySynchronized)
    if (auto Err = AsyncInfoWrapper.synchronize())
      return Err;

  DeviceImageTy &Image = Kernel.getImage();
  auto OffsetOrErr = getDeviceGlobal(Device, Image,
                                     INPUTGEN_STRING_BUFFER_OFFSET_SYMBOL,
                                     sizeof(uint64_t));
  if (!OffsetOrErr) {
    warnMissingDeviceGlobal(Device, Kernel, INPUTGEN_STRING_BUFFER_OFFSET_SYMBOL,
                            OffsetOrErr.takeError());
    return Plugin::success();
  }

  auto RecordsOrErr = getDeviceGlobal(Device, Image,
                                      INPUTGEN_STRING_BUFFER_RECORDS_SYMBOL,
                                      sizeof(uint64_t));
  if (!RecordsOrErr) {
    warnMissingDeviceGlobal(Device, Kernel,
                            INPUTGEN_STRING_BUFFER_RECORDS_SYMBOL,
                            RecordsOrErr.takeError());
    return Plugin::success();
  }

  auto DroppedOrErr = getDeviceGlobal(Device, Image,
                                      INPUTGEN_STRING_BUFFER_DROPPED_SYMBOL,
                                      sizeof(uint64_t));
  if (!DroppedOrErr) {
    warnMissingDeviceGlobal(Device, Kernel,
                            INPUTGEN_STRING_BUFFER_DROPPED_SYMBOL,
                            DroppedOrErr.takeError());
    return Plugin::success();
  }

  auto BufferOrErr =
      getDeviceGlobal(Device, Image, INPUTGEN_STRING_BUFFER_SYMBOL);
  if (!BufferOrErr) {
    warnMissingDeviceGlobal(Device, Kernel, INPUTGEN_STRING_BUFFER_SYMBOL,
                            BufferOrErr.takeError());
    return Plugin::success();
  }

  InputGenStringBufferCountersTy Counters = {0u, 0u, 0u};
  if (auto Err = readUInt64Global(Device, *OffsetOrErr, Counters.Offset))
    return Err;
  if (auto Err = readUInt64Global(Device, *RecordsOrErr, Counters.Records))
    return Err;
  if (auto Err = readUInt64Global(Device, *DroppedOrErr, Counters.Dropped))
    return Err;

  const uint64_t BufferSize = BufferOrErr->getSize();
  const uint64_t BytesToCopy = std::min(Counters.Offset, BufferSize);

  std::vector<char> Buffer(BytesToCopy);
  if (BytesToCopy != 0)
    if (auto Err = Device.dataRetrieve(
            Buffer.data(), BufferOrErr->getPtr(),
            static_cast<int64_t>(BytesToCopy), nullptr))
      return Err;

  raw_ostream &OS = outs();
  OS << "inputgen GPU runtime: records=" << Counters.Records
     << ", dropped=" << Counters.Dropped
     << ", buffer_size=" << BufferSize << ", kernel=";
  if (Kernel.getName())
    OS << Kernel.getName();
  else
    OS << "<unknown>";
  OS << "\n";

  if (BytesToCopy != 0)
    OS.write(Buffer.data(), static_cast<size_t>(BytesToCopy));

  OS.flush();
  return Plugin::success();
}

} // namespace inputgen
} // namespace plugin
} // namespace target
} // namespace omp
} // namespace llvm
