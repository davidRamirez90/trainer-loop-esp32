#include "trainer_loop/storage_io.h"
#include "trainer_loop/fit_store.h"

#include "trainer_loop/fit_encoder.h"
#include "trainer_loop/hash.h"
#include "trainer_loop/limits.h"

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace trainer_loop {
namespace {

bool ensure_directory(const std::string& path) {
  if (::mkdir(path.c_str(), 0700) == 0) return true;
  return errno == EEXIST;
}

bool safe_component(const std::string& value) {
  if (value.empty() || value.size() > 96U) return false;
  for (const char character : value) {
    if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '-' || character == '_')) return false;
  }
  return true;
}

}  // namespace

FitArtifactStore::FitArtifactStore(std::string directory) : directory_(std::move(directory)) {}

Status FitArtifactStore::initialize() {
  return ensure_directory(directory_) ? Status::ok() : Status::failure(ErrorCode::kStorage, "cannot create FIT directory");
}

std::string FitArtifactStore::path_for(const std::string& session_uuid) const {
  return directory_ + "/" + session_uuid + ".fit";
}

Result<FitArtifact> FitArtifactStore::publish(const std::string& session_uuid,
                                              const std::vector<std::uint8_t>& bytes) {
  if (!safe_component(session_uuid) || bytes.empty() || bytes.size() > kMaxFitArtifactBytes || !validate_fit_bytes(bytes)) {
    return Result<FitArtifact>::failure(ErrorCode::kInvalidArgument, "FIT artifact is invalid");
  }
  const Status initialized = initialize();
  if (!initialized) return Result<FitArtifact>::failure(initialized.error->code, initialized.error->message);
  const std::string path = path_for(session_uuid);
  const std::string hash = sha256_hex(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  std::ifstream existing(path, std::ios::binary);
  if (existing) {
    existing.seekg(0, std::ios::end);
    const std::streamoff size = existing.tellg();
    if (size == static_cast<std::streamoff>(bytes.size())) {
      existing.seekg(0, std::ios::beg);
      std::string current(static_cast<std::size_t>(size), '\0');
      existing.read(current.data(), static_cast<std::streamsize>(current.size()));
      if (existing && sha256_hex(current) == hash) return Result<FitArtifact>::success({session_uuid, path, hash, bytes.size()});
    }
    return Result<FitArtifact>::failure(ErrorCode::kConflict, "FIT artifact already exists with different bytes");
  }
  const std::string temporary = path + ".tmp";
  std::FILE* output = std::fopen(temporary.c_str(), "wb");
  if (output == nullptr) return Result<FitArtifact>::failure(ErrorCode::kStorage, "cannot open FIT temporary file");
  const bool written = storage_write(bytes.data(), bytes.size(), output) == bytes.size();
  const bool flushed = std::fflush(output) == 0;
  const int descriptor = ::fileno(output);
  const bool synced = descriptor < 0 || ::fsync(descriptor) == 0;
  const bool closed = std::fclose(output) == 0;
  if (!written || !flushed || !synced || !closed || std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    return Result<FitArtifact>::failure(ErrorCode::kStorage, "cannot publish FIT artifact");
  }
  return Result<FitArtifact>::success({session_uuid, path, hash, bytes.size()});
}

Result<std::vector<std::uint8_t>> FitArtifactStore::read(const FitArtifact& artifact) const {
  if (!safe_component(artifact.session_uuid) || artifact.path != path_for(artifact.session_uuid)) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kInvalidArgument, "FIT path is outside artifact store");
  }
  std::ifstream input(artifact.path, std::ios::binary);
  if (!input) return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kStorage, "FIT artifact is unavailable");
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size < 0 || static_cast<std::size_t>(size) != artifact.size || artifact.size > kMaxFitArtifactBytes) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kBounds, "FIT artifact size is outside bounds");
  }
  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!input || sha256_hex(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())) != artifact.sha256 || !validate_fit_bytes(bytes)) {
    return Result<std::vector<std::uint8_t>>::failure(ErrorCode::kStorage, "FIT artifact integrity check failed");
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

}  // namespace trainer_loop
