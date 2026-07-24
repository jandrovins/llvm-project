//===- InputGen.cpp - InputGen runtime kernel hooks -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InputGenInterface.hpp"
#include "PluginInterface.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
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

constexpr const char *InputGenDumpSymtabDirEnv =
    "LIBOMPTARGET_INPUTGEN_SAVE_SYMTAB_DIR";

bool isInputGenEnabled() {
  static const bool Enabled = getBoolEnvar(INPUTGEN_ENABLE_ENVVAR, false);
  return Enabled;
}

std::string getStringEnvar(const char *Name) {
  const char *Value = std::getenv(Name);
  if (!Value || !Value[0])
    return std::string();
  return std::string(Value);
}

std::string sanitizePathComponent(StringRef Name) {
  if (Name.empty())
    return "unknown";

  std::string Out;
  Out.reserve(Name.size());
  for (unsigned char C : Name) {
    if (std::isalnum(C) || C == '_' || C == '-' || C == '.')
      Out.push_back(char(C));
    else
      Out.push_back('_');
  }
  return Out;
}

std::string getKernelDumpStem(const GenericKernelTy &Kernel,
                              const DeviceImageTy &Image) {
  StringRef KernelName = Kernel.getName() ? Kernel.getName() : "unknown";
  return sanitizePathComponent(KernelName) + "-" +
         utohexstr(reinterpret_cast<uintptr_t>(Image.getStart()));
}

bool markImageOnce(const DeviceImageTy &Image, std::set<const void *> &Seen,
                   std::mutex &SeenMutex) {
  std::lock_guard<std::mutex> Lock(SeenMutex);
  return Seen.insert(Image.getStart()).second;
}

const char *symbolTypeName(object::SymbolRef::Type Type) {
  switch (Type) {
  case object::SymbolRef::ST_Function:
    return "FUNC";
  case object::SymbolRef::ST_Data:
    return "DATA";
  case object::SymbolRef::ST_Debug:
    return "DEBUG";
  case object::SymbolRef::ST_File:
    return "FILE";
  case object::SymbolRef::ST_Other:
    return "OTHER";
  case object::SymbolRef::ST_Unknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

void printSymbolFlags(raw_ostream &OS, uint32_t Flags) {
  bool Printed = false;
  auto PrintFlag = [&](uint32_t Flag, const char *Name) {
    if (!(Flags & Flag))
      return;
    if (Printed)
      OS << '|';
    OS << Name;
    Printed = true;
  };

  PrintFlag(object::SymbolRef::SF_Undefined, "UND");
  PrintFlag(object::SymbolRef::SF_Global, "GLOBAL");
  PrintFlag(object::SymbolRef::SF_Weak, "WEAK");
  PrintFlag(object::SymbolRef::SF_Common, "COMMON");
  PrintFlag(object::SymbolRef::SF_Absolute, "ABS");
  PrintFlag(object::SymbolRef::SF_Executable, "EXEC");
  PrintFlag(object::SymbolRef::SF_FormatSpecific, "FMT");

  if (!Printed)
    OS << '0';
}

Error dumpImageSymbolTable(raw_ostream &OS, GenericDeviceTy &Device,
                           const GenericKernelTy &Kernel, DeviceImageTy &Image) {
  auto ObjOrErr = Device.Plugin.getGlobalHandler().getELFObjectFile(Image);
  if (!ObjOrErr)
    return ObjOrErr.takeError();

  OS << "inputgen GPU runtime: device image symbols";
  if (Kernel.getName())
    OS << " for kernel " << Kernel.getName();
  OS << " (image_start=0x"
     << utohexstr(reinterpret_cast<uintptr_t>(Image.getStart()))
     << ", image_size=" << Image.getSize() << ")\n";

  for (const object::SymbolRef &Sym : (*ObjOrErr)->symbols()) {
    auto NameOrErr = Sym.getName();
    if (!NameOrErr)
      return Plugin::error(ErrorCode::INVALID_BINARY, NameOrErr.takeError(),
                           "failed to read symbol name");

    auto AddressOrErr = Sym.getAddress();
    if (!AddressOrErr)
      return Plugin::error(ErrorCode::INVALID_BINARY, AddressOrErr.takeError(),
                           "failed to read symbol address");

    auto TypeOrErr = Sym.getType();
    if (!TypeOrErr)
      return Plugin::error(ErrorCode::INVALID_BINARY, TypeOrErr.takeError(),
                           "failed to read symbol type");

    auto FlagsOrErr = Sym.getFlags();
    if (!FlagsOrErr)
      return Plugin::error(ErrorCode::INVALID_BINARY, FlagsOrErr.takeError(),
                           "failed to read symbol flags");

    OS << "  0x" << utohexstr(*AddressOrErr) << ' '
       << symbolTypeName(*TypeOrErr) << ' ';
    printSymbolFlags(OS, *FlagsOrErr);
    OS << ' ' << *NameOrErr << '\n';
  }

  OS.flush();
  return Plugin::success();
}

Error dumpImageSymbolTableToFile(GenericDeviceTy &Device,
                                 const GenericKernelTy &Kernel,
                                 DeviceImageTy &Image, StringRef Directory) {
  if (Directory.empty())
    return Plugin::success();

  if (std::error_code EC = sys::fs::create_directories(Directory))
    return Plugin::error(ErrorCode::HOST_IO,
                         "failed to create symbol table dump directory '%s': %s",
                         Directory.str().c_str(), EC.message().c_str());

  std::string Path =
      (Directory + "/" + getKernelDumpStem(Kernel, Image) + ".symtab.txt").str();
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_None);
  if (EC)
    return Plugin::error(ErrorCode::HOST_IO,
                         "failed to open symbol table dump '%s': %s",
                         Path.c_str(), EC.message().c_str());

  if (auto Err = dumpImageSymbolTable(OS, Device, Kernel, Image))
    return Err;

  errs() << "inputgen GPU runtime: wrote device symbol table to '" << Path
         << "'\n";
  return Plugin::success();
}

void emitRequestedImageDebugArtifacts(GenericDeviceTy &Device,
                                      const GenericKernelTy &Kernel) {
  DeviceImageTy &Image = Kernel.getImage();
  static std::mutex DumpedFilesMutex;
  static std::set<const void *> DumpedFiles;
  if (!markImageOnce(Image, DumpedFiles, DumpedFilesMutex))
    return;

  if (auto Err = dumpImageSymbolTableToFile(
          Device, Kernel, Image, getStringEnvar(InputGenDumpSymtabDirEnv)))
    errs() << "inputgen GPU runtime: warning: failed to dump device symbol "
              "table: "
           << toString(std::move(Err)) << "\n";
}

void emitImageSymbolTableToErr(GenericDeviceTy &Device,
                               const GenericKernelTy &Kernel) {
  DeviceImageTy &Image = Kernel.getImage();
  static std::mutex PrintedTablesMutex;
  static std::set<const void *> PrintedTables;
  if (!markImageOnce(Image, PrintedTables, PrintedTablesMutex))
    return;

  if (auto Err = dumpImageSymbolTable(errs(), Device, Kernel, Image))
    errs() << "inputgen GPU runtime: warning: failed to dump device image "
              "symbol table: "
           << toString(std::move(Err)) << "\n";
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
  emitImageSymbolTableToErr(Device, Kernel);
  emitRequestedImageDebugArtifacts(Device, Kernel);
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
  errs() << "beforeKernelLaunch: before isInputGenEnabled()\n";
  if (!isInputGenEnabled())
    return Plugin::success();
  errs() << "beforeKernelLaunch: after isInputGenEnabled()\n";

  emitRequestedImageDebugArtifacts(Device, Kernel);

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
  errs() << "afterKernelLaunch: before isInputGenEnabled()\n";
  if (!isInputGenEnabled())
    return Plugin::success();
  errs() << "afterKernelLaunch: after isInputGenEnabled()\n";

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
