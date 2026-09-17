#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace trainer_loop {

enum class ErrorCode {
  kInvalidArgument,
  kMalformedInput,
  kUnsupported,
  kOverflow,
  kBounds,
  kConflict,
  kTimeout,
  kTransport,
  kAuthentication,
  kStorage,
  kTimeUntrusted,
};

struct Error {
  ErrorCode code = ErrorCode::kInvalidArgument;
  std::string message;
  std::size_t offset = 0U;
};

template <typename T>
struct Result {
  std::optional<T> value;
  std::optional<Error> error;

  static Result success(T result) {
    Result out;
    out.value = std::move(result);
    return out;
  }

  static Result failure(ErrorCode code, std::string message, std::size_t offset = 0U) {
    Result out;
    out.error = Error{code, std::move(message), offset};
    return out;
  }

  explicit operator bool() const { return value.has_value(); }
};

struct Status {
  std::optional<Error> error;

  static Status ok() { return Status{}; }

  static Status failure(ErrorCode code, std::string message, std::size_t offset = 0U) {
    Status out;
    out.error = Error{code, std::move(message), offset};
    return out;
  }

  explicit operator bool() const { return !error.has_value(); }
};

}  // namespace trainer_loop
