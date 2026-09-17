#include "trainer_loop/hash.h"

#include <array>
#include <iomanip>
#include <sstream>

namespace trainer_loop {
namespace {

constexpr std::uint32_t kInitialState[8] = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t rotr(std::uint32_t value, unsigned amount) {
  return (value >> amount) | (value << (32U - amount));
}

std::uint32_t load_be(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) |
         static_cast<std::uint32_t>(data[3]);
}

void append_be(std::string* output, std::uint32_t value) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(8) << value;
  *output += stream.str();
}

}  // namespace

std::uint32_t fnv1a32(const std::string& value) {
  std::uint32_t hash = 2166136261U;
  for (const char raw_byte : value) {
    const unsigned char byte = static_cast<unsigned char>(raw_byte);
    hash ^= static_cast<std::uint32_t>(byte);
    hash *= 16777619U;
  }
  return hash;
}

std::string hex_u32(std::uint32_t value) {
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(8) << value;
  return stream.str();
}

Sha256::Sha256() {
  for (unsigned i = 0U; i < 8U; ++i) state_[i] = kInitialState[i];
}

void Sha256::transform(const std::uint8_t* chunk) {
  std::array<std::uint32_t, 64> words{};
  for (unsigned i = 0U; i < 16U; ++i) words[i] = load_be(chunk + (i * 4U));
  for (unsigned i = 16U; i < 64U; ++i) {
    const std::uint32_t s0 = rotr(words[i - 15U], 7U) ^ rotr(words[i - 15U], 18U) ^
                             (words[i - 15U] >> 3U);
    const std::uint32_t s1 = rotr(words[i - 2U], 17U) ^ rotr(words[i - 2U], 19U) ^
                             (words[i - 2U] >> 10U);
    words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (unsigned i = 0U; i < 64U; ++i) {
    const std::uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + choose + kRoundConstants[i] + words[i];
    const std::uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t length) {
  if (finalized_ || (data == nullptr && length != 0U)) return;
  bit_count_ += static_cast<std::uint64_t>(length) * 8U;
  std::size_t offset = 0U;
  while (offset < length) {
    const std::size_t available = 64U - buffer_size_;
    const std::size_t take = (length - offset < available) ? (length - offset) : available;
    for (std::size_t i = 0U; i < take; ++i) buffer_[buffer_size_ + i] = data[offset + i];
    buffer_size_ += take;
    offset += take;
    if (buffer_size_ == 64U) {
      transform(buffer_);
      buffer_size_ = 0U;
    }
  }
}

void Sha256::update(const std::string& value) {
  update(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}

std::string Sha256::final_hex() {
  if (finalized_) return final_cache_;
  const std::uint64_t original_bit_count = bit_count_;
  buffer_[buffer_size_++] = 0x80U;
  if (buffer_size_ > 56U) {
    while (buffer_size_ < 64U) buffer_[buffer_size_++] = 0U;
    transform(buffer_);
    buffer_size_ = 0U;
  }
  while (buffer_size_ < 56U) buffer_[buffer_size_++] = 0U;
  for (unsigned i = 0U; i < 8U; ++i) {
    buffer_[56U + i] = static_cast<std::uint8_t>(original_bit_count >> (56U - (8U * i)));
  }
  transform(buffer_);
  buffer_size_ = 0U;
  finalized_ = true;
  final_cache_.clear();
  final_cache_.reserve(64U);
  for (const std::uint32_t value : state_) append_be(&final_cache_, value);
  return final_cache_;
}

std::string sha256_hex(const std::string& value) {
  Sha256 hash;
  hash.update(value);
  return hash.final_hex();
}

}  // namespace trainer_loop
