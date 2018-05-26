/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/texture_info.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"

#include "third_party/xxhash/xxhash.h"

namespace xe {
namespace gpu {

using namespace xe::gpu::xenos;

bool TextureInfo::Prepare(const xe_gpu_texture_fetch_t& fetch,
                          TextureInfo* out_info) {
  // http://msdn.microsoft.com/en-us/library/windows/desktop/cc308051(v=vs.85).aspx
  // a2xx_sq_surfaceformat

  std::memset(out_info, 0, sizeof(TextureInfo));

  auto& info = *out_info;

  info.format = static_cast<TextureFormat>(fetch.format);
  info.endianness = static_cast<Endian>(fetch.endianness);

  info.dimension = static_cast<Dimension>(fetch.dimension);
  info.width = info.height = info.depth = 0;
  switch (info.dimension) {
    case Dimension::k1D:
      info.dimension = Dimension::k2D;  // we treat 1D textures as 2D
      info.width = fetch.size_1d.width;
      break;
    case Dimension::k2D:
      info.width = fetch.size_2d.width;
      info.height = fetch.size_2d.height;
      break;
    case Dimension::k3D:
      info.width = fetch.size_3d.width;
      info.height = fetch.size_3d.height;
      info.depth = fetch.size_3d.depth;
      break;
    case Dimension::kCube:
      info.width = fetch.size_stack.width;
      info.height = fetch.size_stack.height;
      info.depth = fetch.size_stack.depth;
      break;
    default:
      assert_unhandled_case(info.dimension);
      break;
  }
  info.pitch = fetch.pitch << 5;
  info.mip_levels = fetch.packed_mips ? fetch.mip_max_level + 1 : 1;

  info.is_tiled = fetch.tiled;
  info.has_packed_mips = fetch.packed_mips;

  info.guest_address = fetch.address << 12;
  info.mip_address = fetch.mip_address << 12;

  if (info.format_info()->format == TextureFormat::kUnknown) {
    XELOGE("Attempting to fetch from unsupported texture format %d",
           info.format);
    return false;
  }

  info.memory_usage = TextureMemoryUsage::Calculate(out_info, true);
  return true;
}

bool TextureInfo::PrepareResolve(uint32_t physical_address,
                                 TextureFormat format, Endian endian,
                                 uint32_t pitch, uint32_t width,
                                 uint32_t height, TextureInfo* out_info) {
  assert_true(width > 0);
  assert_true(height > 0);

  std::memset(out_info, 0, sizeof(TextureInfo));

  auto& info = *out_info;
  info.format = format;
  info.dimension = Dimension::k2D;
  info.width = width - 1;
  info.height = height - 1;
  info.mip_levels = 1;
  info.depth = 0;
  info.pitch = pitch;

  info.endianness = endian;
  info.is_tiled = true;

  info.guest_address = physical_address;
  info.mip_address = 0;

  if (info.format_info()->format == TextureFormat::kUnknown) {
    assert_true("Unsupported texture format");
    return false;
  }

  info.memory_usage = TextureMemoryUsage::Calculate(out_info, true);
  return true;
}

static void TextureSwap(Endian endianness, void* dest, const void* src,
                        size_t length) {
  switch (endianness) {
    case Endian::k8in16:
      xe::copy_and_swap_16_unaligned(dest, src, length / 2);
      break;
    case Endian::k8in32:
      xe::copy_and_swap_32_unaligned(dest, src, length / 4);
      break;
    case Endian::k16in32:  // Swap high and low 16 bits within a 32 bit word
      xe::copy_and_swap_16_in_32_unaligned(dest, src, length);
      break;
    default:
    case Endian::kUnspecified:
      std::memcpy(dest, src, length);
      break;
  }
}

static void ConvertTexelCTX1(uint8_t* dest, size_t dest_pitch,
                             const uint8_t* src, Endian src_endianness) {
  // http://fileadmin.cs.lth.se/cs/Personal/Michael_Doggett/talks/unc-xenos-doggett.pdf
  union {
    uint8_t data[8];
    struct {
      uint8_t r0, g0, r1, g1;
      uint32_t xx;
    };
  } block;
  static_assert(sizeof(block) == 8, "CTX1 block mismatch");

  const uint32_t bytes_per_block = 8;
  TextureSwap(src_endianness, block.data, src, bytes_per_block);

  uint8_t cr[4] = {
      block.r0, block.r1,
      static_cast<uint8_t>(2.f / 3.f * block.r0 + 1.f / 3.f * block.r1),
      static_cast<uint8_t>(1.f / 3.f * block.r0 + 2.f / 3.f * block.r1)};
  uint8_t cg[4] = {
      block.g0, block.g1,
      static_cast<uint8_t>(2.f / 3.f * block.g0 + 1.f / 3.f * block.g1),
      static_cast<uint8_t>(1.f / 3.f * block.g0 + 2.f / 3.f * block.g1)};

  for (uint32_t oy = 0; oy < 4; ++oy) {
    for (uint32_t ox = 0; ox < 4; ++ox) {
      uint8_t xx = (block.xx >> (((ox + (oy * 4)) * 2))) & 3;
      dest[(oy * dest_pitch) + (ox * 2) + 0] = cr[xx];
      dest[(oy * dest_pitch) + (ox * 2) + 1] = cg[xx];
    }
  }
}

uint32_t TextureInfo::GetMaxMipLevels() const {
  return 1 + xe::log2_floor(std::max({width + 1, height + 1, depth + 1}));
}

const TextureMemoryUsage TextureInfo::GetMipMemoryUsage(uint32_t mip,
                                                        bool is_guest) const {
  if (mip == 0) {
    return memory_usage;
  }
  uint32_t mip_width = xe::next_pow2(width + 1) >> mip;
  uint32_t mip_height = xe::next_pow2(height + 1) >> mip;
  return TextureMemoryUsage::Calculate(format_info(), mip_width, mip_height,
                                       depth + 1, is_tiled, is_guest);
}

void TextureInfo::GetMipSize(uint32_t mip, uint32_t* out_width,
                             uint32_t* out_height) const {
  assert_not_null(out_width);
  assert_not_null(out_height);
  if (mip == 0) {
    *out_width = width + 1;
    *out_height = height + 1;
    return;
  }
  uint32_t width_pow2 = xe::next_pow2(width + 1);
  uint32_t height_pow2 = xe::next_pow2(height + 1);
  *out_width = std::max(width_pow2 >> mip, 1u);
  *out_height = std::max(height_pow2 >> mip, 1u);
}

uint32_t TextureInfo::GetMipLocation(uint32_t mip, uint32_t* offset_x,
                                     uint32_t* offset_y, bool is_guest) const {
  if (mip == 0) {
    // Short-circuit. Mip 0 is always stored in guest_address.
    if (!has_packed_mips) {
      *offset_x = 0;
      *offset_y = 0;
    } else {
      GetPackedTileOffset(0, offset_x, offset_y);
    }
    return guest_address;
  }

  // If the texture is <= 16 pixels w/h, the mips are packed with the base
  // texture. Otherwise, they're stored beginning from mip_address.
  uint32_t address_base =
      std::min(width, height) < 16 ? guest_address : mip_address;
  uint32_t address_offset = 0;

  if (!has_packed_mips) {
    for (uint32_t i = 1; i < mip; i++) {
      address_offset += GetMipByteSize(i, is_guest);
    }
    *offset_x = 0;
    *offset_y = 0;
    return address_base + address_offset;
  }

  uint32_t width_pow2 = xe::next_pow2(width + 1);
  uint32_t height_pow2 = xe::next_pow2(height + 1);

  // Walk forward to find the address of the mip.
  uint32_t packed_mip_base = 1;
  for (uint32_t i = packed_mip_base; i < mip; i++, packed_mip_base++) {
    uint32_t mip_width = std::max(width_pow2 >> i, 1u);
    uint32_t mip_height = std::max(height_pow2 >> i, 1u);
    if (std::min(mip_width, mip_height) <= 16) {
      // We've reached the point where the mips are packed into a single tile.
      break;
    }

    address_offset += GetMipByteSize(i, is_guest);
  }

  // Now, check if the mip is packed at an offset.
  GetPackedTileOffset(width_pow2 >> mip, height_pow2 >> mip, format_info(),
                      mip - packed_mip_base, offset_x, offset_y);
  return address_base + address_offset;
}

uint32_t TextureInfo::GetMipByteSize(uint32_t mip, bool is_guest) const {
  uint32_t bytes_per_block = format_info()->bytes_per_block();
  auto mip_usage = GetMipMemoryUsage(mip, is_guest);
  return mip_usage.blocks() * bytes_per_block;
}

uint32_t TextureInfo::GetByteSize(bool is_guest) const {
  uint32_t length = 0;
  for (uint32_t mip = 0; mip < mip_levels; ++mip) {
    length += GetMipByteSize(mip, is_guest);
  }
  return length;
}

bool TextureInfo::GetPackedTileOffset(uint32_t width, uint32_t height,
                                      const FormatInfo* format_info,
                                      int packed_tile, uint32_t* offset_x,
                                      uint32_t* offset_y) {
  // Tile size is 32x32, and once textures go <=16 they are packed into a
  // single tile together. The math here is insane. Most sourced
  // from graph paper and looking at dds dumps.
  //   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  // 0         +.4x4.+ +.....8x8.....+ +............16x16............+
  // 1         +.4x4.+ +.....8x8.....+ +............16x16............+
  // 2         +.4x4.+ +.....8x8.....+ +............16x16............+
  // 3         +.4x4.+ +.....8x8.....+ +............16x16............+
  // 4 x               +.....8x8.....+ +............16x16............+
  // 5                 +.....8x8.....+ +............16x16............+
  // 6                 +.....8x8.....+ +............16x16............+
  // 7                 +.....8x8.....+ +............16x16............+
  // 8 2x2                             +............16x16............+
  // 9 2x2                             +............16x16............+
  // 0                                 +............16x16............+
  // ...                                            .....
  // This only works for square textures, or textures that are some non-pot
  // <= square. As soon as the aspect ratio goes weird, the textures start to
  // stretch across tiles.
  //
  // The 2x2 and 1x1 squares are packed in their specific positions because
  // each square is the size of at least one block (which is 4x4 pixels max)
  //
  // if (tile_aligned(w) > tile_aligned(h)) {
  //   // wider than tall, so packed horizontally
  // } else if (tile_aligned(w) < tile_aligned(h)) {
  //   // taller than wide, so packed vertically
  // } else {
  //   square
  // }
  // It's important to use logical sizes here, as the input sizes will be
  // for the entire packed tile set, not the actual texture.
  // The minimum dimension is what matters most: if either width or height
  // is <= 16 this mode kicks in.

  uint32_t log2_width = xe::log2_ceil(width);
  uint32_t log2_height = xe::log2_ceil(height);
  if (std::min(log2_width, log2_height) > 4) {
    // Too big, not packed.
    *offset_x = 0;
    *offset_y = 0;
    return false;
  }

  // Find the block offset of the mip.
  if (packed_tile < 3) {
    if (log2_width > log2_height) {
      // Wider than tall. Laid out vertically.
      *offset_x = 0;
      *offset_y = 16 >> packed_tile;
    } else {
      // Taller than wide. Laid out horizontally.
      *offset_x = 16 >> packed_tile;
      *offset_y = 0;
    }
  } else {
    if (log2_width > log2_height) {
      // Wider than tall. Laid out vertically.
      *offset_x = 16 >> (packed_tile - 2);
      *offset_y = 0;
    } else {
      // Taller than wide. Laid out horizontally.
      *offset_x = 0;
      *offset_y = 16 >> (packed_tile - 2);
    }
  }

  *offset_x /= format_info->block_width;
  *offset_y /= format_info->block_height;
  return true;
}

bool TextureInfo::GetPackedTileOffset(int packed_tile, uint32_t* offset_x,
                                      uint32_t* offset_y) const {
  if (!has_packed_mips) {
    *offset_x = 0;
    *offset_y = 0;
    return false;
  }
  return GetPackedTileOffset(xe::next_pow2(width + 1),
                             xe::next_pow2(height + 1), format_info(),
                             packed_tile, offset_x, offset_y);
}

uint64_t TextureInfo::hash() const {
  return XXH64(this, sizeof(TextureInfo), 0);
}

}  //  namespace gpu
}  //  namespace xe
