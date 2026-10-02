// Reads the e-ink payload the sender page (index.html, buildChunkedPNG) embeds
// in its PNGs. The PNG itself is just a preview — no zlib/PNG decoding needed
// on the device. The real pixels ride in custom chunks placed before IEND:
//
//   still image:  one  epRb chunk = [W:u16be, H:u16be, bpp:u8] + packed pixels (bpp = 2)
//   animation:    one  epRa chunk = [W:u16be, H:u16be, bpp:u8, frames:u8, intervalMs:u16be]
//                 then `frames` epRb chunks, each just packed pixels (no header, bpp = 1)
//
// packed pixels: row-major, MSB-first, each row padded to a whole byte,
// code 0 = black .. (2^bpp - 1) = white.
//
// Templated on the file type (Arduino's File, or any class with size(),
// seek(), read(buf, n)) and free of Arduino includes so it can be unit-tested
// on a host machine.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define EPD_MAX_FRAMES 10

struct EpdImage {
  uint16_t w = 0, h = 0;
  uint8_t bpp = 0;
  uint8_t frames = 0;       // 1 for a still
  uint16_t intervalMs = 0;  // 0 for a still
  uint32_t off[EPD_MAX_FRAMES] = {0};  // file offset of each frame's packed pixels
  uint32_t len[EPD_MAX_FRAMES] = {0};
};

inline uint32_t epdFrameBytes(const EpdImage& img) {
  return (uint32_t)((img.w * img.bpp + 7) / 8) * img.h;
}

template <class F>
bool epdParse(F& f, EpdImage& img) {
  img = EpdImage();
  static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  uint8_t hdr[8];
  uint32_t fileSize = f.size();
  if (fileSize < 8 || !f.seek(0) || f.read(hdr, 8) != 8 || memcmp(hdr, SIG, 8) != 0) return false;

  bool anim = false;
  uint8_t found = 0;
  uint32_t pos = 8;
  while (pos + 12 <= fileSize) {
    if (!f.seek(pos) || f.read(hdr, 8) != 8) return false;
    uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
    if (pos + 12 + len > fileSize) return false;  // truncated chunk
    const char* type = (const char*)hdr + 4;

    if (!memcmp(type, "IEND", 4)) break;

    if (!memcmp(type, "epRa", 4)) {
      uint8_t a[8];
      if (len != 8 || f.read(a, 8) != 8) return false;
      img.w = (a[0] << 8) | a[1];
      img.h = (a[2] << 8) | a[3];
      img.bpp = a[4];
      img.frames = a[5];
      img.intervalMs = (a[6] << 8) | a[7];
      if (img.bpp != 1 || img.frames < 1 || img.frames > EPD_MAX_FRAMES) return false;
      anim = true;
    } else if (!memcmp(type, "epRb", 4)) {
      if (anim) {
        if (found >= img.frames) return false;
        img.off[found] = pos + 8;
        img.len[found] = len;
        found++;
      } else {
        uint8_t a[5];
        if (found > 0 || len < 5 || f.read(a, 5) != 5) return false;
        img.w = (a[0] << 8) | a[1];
        img.h = (a[2] << 8) | a[3];
        img.bpp = a[4];
        if (img.bpp < 1 || img.bpp > 2) return false;
        img.frames = 1;
        img.off[0] = pos + 8 + 5;
        img.len[0] = len - 5;
        found = 1;
      }
    }
    pos += 12 + len;
  }

  if (!img.w || !img.h || img.frames == 0 || found != img.frames) return false;
  for (uint8_t i = 0; i < img.frames; i++) {
    if (img.len[i] != epdFrameBytes(img)) return false;  // pixel data must match W x H x bpp
  }
  return true;
}

// copies frame `idx`'s packed pixels into buf (needs epdFrameBytes(img) bytes)
template <class F>
bool epdReadFrame(F& f, const EpdImage& img, uint8_t idx, uint8_t* buf) {
  if (idx >= img.frames) return false;
  return f.seek(img.off[idx]) && f.read(buf, img.len[idx]) == img.len[idx];
}

// pixel code for (x, y) from a packed frame: 0 = black .. (2^bpp - 1) = white
inline uint8_t epdPixel(const uint8_t* packed, const EpdImage& img, uint16_t x, uint16_t y) {
  uint32_t bytesPerRow = (img.w * img.bpp + 7) / 8;
  uint8_t b = packed[y * bytesPerRow + ((x * img.bpp) >> 3)];
  uint8_t shift = 8 - img.bpp - ((x * img.bpp) % 8);
  return (b >> shift) & ((1 << img.bpp) - 1);
}
