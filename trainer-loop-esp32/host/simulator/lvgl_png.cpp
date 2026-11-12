#include "lvgl_host.h"

#include <array>
#include <cstdio>
#include <vector>
#include <zlib.h>

namespace trainer_loop {
namespace {

void append_u32(std::vector<unsigned char>* out, std::uint32_t value) {
  out->push_back(static_cast<unsigned char>((value >> 24U) & 0xffU));
  out->push_back(static_cast<unsigned char>((value >> 16U) & 0xffU));
  out->push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
  out->push_back(static_cast<unsigned char>(value & 0xffU));
}

void append_chunk(std::vector<unsigned char>* png, const char type[4],
                  const unsigned char* payload, std::size_t length) {
  append_u32(png, static_cast<std::uint32_t>(length));
  const std::size_t type_offset = png->size();
  png->insert(png->end(), type, type + 4);
  if (length != 0U) png->insert(png->end(), payload, payload + length);
  const uLong crc = crc32(0L, Z_NULL, 0);
  append_u32(png, static_cast<std::uint32_t>(crc32(crc, png->data() + type_offset,
                                                  static_cast<uInt>(4U + length))));
}

}  // namespace

bool lvgl_host_write_png(const std::string& path, const std::uint16_t* pixels,
                         int width, int height) {
  if (!pixels || width <= 0 || height <= 0) return false;
  const std::size_t stride = static_cast<std::size_t>(width) * 3U;
  std::vector<unsigned char> raw((stride + 1U) * static_cast<std::size_t>(height));
  for (int y = 0; y < height; ++y) {
    unsigned char* row = raw.data() + static_cast<std::size_t>(y) * (stride + 1U);
    row[0] = 0U;
    for (int x = 0; x < width; ++x) {
      const std::uint16_t pixel = pixels[static_cast<std::size_t>(y * width + x)];
      const unsigned red = (pixel >> 11U) & 0x1fU;
      const unsigned green = (pixel >> 5U) & 0x3fU;
      const unsigned blue = pixel & 0x1fU;
      row[1U + static_cast<std::size_t>(x) * 3U] = static_cast<unsigned char>((red * 255U + 15U) / 31U);
      row[2U + static_cast<std::size_t>(x) * 3U] = static_cast<unsigned char>((green * 255U + 31U) / 63U);
      row[3U + static_cast<std::size_t>(x) * 3U] = static_cast<unsigned char>((blue * 255U + 15U) / 31U);
    }
  }
  uLongf compressed_size = compressBound(static_cast<uLong>(raw.size()));
  std::vector<unsigned char> compressed(compressed_size);
  if (compress2(compressed.data(), &compressed_size, raw.data(), static_cast<uLong>(raw.size()),
                Z_BEST_SPEED) != Z_OK) return false;
  compressed.resize(compressed_size);

  std::vector<unsigned char> png{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
  std::array<unsigned char, 13> ihdr{};
  ihdr[0] = static_cast<unsigned char>((static_cast<unsigned>(width) >> 24U) & 0xffU);
  ihdr[1] = static_cast<unsigned char>((static_cast<unsigned>(width) >> 16U) & 0xffU);
  ihdr[2] = static_cast<unsigned char>((static_cast<unsigned>(width) >> 8U) & 0xffU);
  ihdr[3] = static_cast<unsigned char>(static_cast<unsigned>(width) & 0xffU);
  ihdr[4] = static_cast<unsigned char>((static_cast<unsigned>(height) >> 24U) & 0xffU);
  ihdr[5] = static_cast<unsigned char>((static_cast<unsigned>(height) >> 16U) & 0xffU);
  ihdr[6] = static_cast<unsigned char>((static_cast<unsigned>(height) >> 8U) & 0xffU);
  ihdr[7] = static_cast<unsigned char>(static_cast<unsigned>(height) & 0xffU);
  ihdr[8] = 8U;
  ihdr[9] = 2U;
  append_chunk(&png, "IHDR", ihdr.data(), ihdr.size());
  append_chunk(&png, "IDAT", compressed.data(), compressed.size());
  append_chunk(&png, "IEND", nullptr, 0U);
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return false;
  const bool success = std::fwrite(png.data(), 1U, png.size(), file) == png.size();
  return std::fclose(file) == 0 && success;
}

}  // namespace trainer_loop
