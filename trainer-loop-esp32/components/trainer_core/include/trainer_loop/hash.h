#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace trainer_loop {

std::uint32_t fnv1a32(const std::string& value);
std::string hex_u32(std::uint32_t value);

class Sha256 {
 public:
  Sha256();
  void update(const std::uint8_t* data, std::size_t length);
  void update(const std::string& value);
  std::string final_hex();

 private:
  void transform(const std::uint8_t* chunk);
  std::uint32_t state_[8];
  std::uint64_t bit_count_ = 0U;
  std::uint8_t buffer_[64]{};
  std::size_t buffer_size_ = 0U;
  bool finalized_ = false;
  std::string final_cache_;
};

std::string sha256_hex(const std::string& value);

}  // namespace trainer_loop
