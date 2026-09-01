//===-- InputGenGPUCodec.h - InputGen GPU host codec API --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_INPUTGEN_GPU_CODEC_H
#define COMPILER_RT_INPUTGEN_GPU_CODEC_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace inputgen_gpu {

enum class Mode : uint32_t { Generate = 1, Replay = 2 };

struct FactoryConfig {
  uint32_t NumTeams = 1;
  uint32_t NumThreads = 1;
  uint64_t SliceBytes = 0;
  uint64_t ObjectBytes = 0;
  uint32_t ObjectsPerThread = 4;
};

// A present field means that the user explicitly requested that replay value.
// Replay rejects it when it differs from the value stored in the input record.
struct ReplayRequest {
  std::optional<uint32_t> NumTeams;
  std::optional<uint32_t> NumThreads;
  std::optional<uint64_t> SliceBytes;
  std::optional<uint64_t> ObjectBytes;
  std::optional<uint32_t> ObjectsPerThread;
};

class Error {
public:
  Error() = default;
  explicit Error(std::string Message) : Message(std::move(Message)) {}

  explicit operator bool() const { return !Message.empty(); }
  const std::string &message() const { return Message; }

private:
  std::string Message;
};

template <class T> class Result {
public:
  Result(T Value) : Storage(std::move(Value)) {}
  Result(Error Failure) : Storage(std::move(Failure)) {}

  explicit operator bool() const { return std::holds_alternative<T>(Storage); }
  T &value() & { return std::get<T>(Storage); }
  const T &value() const & { return std::get<T>(Storage); }
  T &&value() && { return std::get<T>(std::move(Storage)); }
  const Error &error() const { return std::get<Error>(Storage); }

private:
  std::variant<T, Error> Storage;
};

class Factory;
Result<Factory> createGenerationFactory(const FactoryConfig &Config);
Result<Factory> createReplayFactory(const std::string &Filename,
                                    const ReplayRequest &Request = {});
Error writeGenerationRecord(const std::string &Filename,
                            const Factory &Value);

class Factory {
public:
  Factory(Factory &&) noexcept = default;
  Factory &operator=(Factory &&) noexcept = default;

  Factory(const Factory &) = delete;
  Factory &operator=(const Factory &) = delete;

  const FactoryConfig &config() const { return Config; }
  const uint8_t *data() const { return Bytes.data(); }
  uint8_t *data() { return Bytes.data(); }
  size_t size() const { return Bytes.size(); }

private:
  Factory(Mode ExecutionMode, FactoryConfig Config,
          std::vector<uint8_t> Bytes)
      : ExecutionMode(ExecutionMode), Config(Config), Bytes(std::move(Bytes)) {}

  Mode ExecutionMode;
  FactoryConfig Config;
  std::vector<uint8_t> Bytes;

  friend Result<Factory>
  createGenerationFactory(const FactoryConfig &Config);
  friend Result<Factory> createReplayFactory(const std::string &Filename,
                                             const ReplayRequest &Request);
  friend Error writeGenerationRecord(const std::string &Filename,
                                     const Factory &Value);
};

struct ThreadResult {
  uint32_t ThreadIndex = 0;
  uint32_t ErrorCode = 0;
  uint32_t ResultSize = 0;
  uint64_t ResultBits = 0;
};

Result<std::vector<ThreadResult>> inspectThreadResults(const Factory &Value);

} // namespace inputgen_gpu

#endif // COMPILER_RT_INPUTGEN_GPU_CODEC_H
