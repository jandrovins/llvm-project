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
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace inputgen_gpu {

struct CodecAccess;

enum class Mode : uint32_t { Generate = 1, Replay = 2 };

struct FactoryConfig {
  Mode ExecutionMode = Mode::Generate;
  uint32_t NumTeams = 1;
  uint32_t NumThreads = 1;
  uint64_t SliceBytes = 0;
  uint64_t ObjectBytes = 0;
  uint32_t ConfigObjectsPerThread = 1;
};

// A present field means that the user explicitly requested that replay value.
// Replay rejects it when it differs from the value stored in the input record.
struct ReplayRequest {
  std::optional<uint32_t> NumTeams;
  std::optional<uint32_t> NumThreads;
  std::optional<uint64_t> SliceBytes;
  std::optional<uint64_t> ObjectBytes;
  std::optional<uint32_t> ConfigObjectsPerThread;
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

class Record {
public:
  struct Impl;

  Record(Record &&) noexcept;
  Record &operator=(Record &&) noexcept;
  ~Record();

  Record(const Record &) = delete;
  Record &operator=(const Record &) = delete;

  FactoryConfig config() const;

private:
  explicit Record(std::unique_ptr<Impl> Storage);

  std::unique_ptr<Impl> Storage;

  friend struct CodecAccess;
};

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
  Factory(FactoryConfig Config, std::vector<uint8_t> Bytes)
      : Config(Config), Bytes(std::move(Bytes)) {}

  FactoryConfig Config;
  std::vector<uint8_t> Bytes;

  friend struct CodecAccess;
};

struct ThreadResult {
  uint32_t ThreadIndex = 0;
  uint32_t ErrorCode = 0;
  uint32_t ResultSize = 0;
  uint64_t ResultBits = 0;
};

Result<Factory> createGenerationFactory(const FactoryConfig &Config);
Result<Record> readRecord(const std::string &Filename);
Error writeRecord(const std::string &Filename, const Record &Value);
Result<Factory> createReplayFactory(const Record &Value,
                                    const ReplayRequest &Request = {});
Result<Record> serializeFactory(const Factory &Value);
Result<std::vector<ThreadResult>> inspectThreadResults(const Factory &Value);

} // namespace inputgen_gpu

#endif // COMPILER_RT_INPUTGEN_GPU_CODEC_H
