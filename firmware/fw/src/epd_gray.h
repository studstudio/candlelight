// 4-level gray for the GDEY042T81 (SSD1683 controller).
//
// The panel's own (OTP) waveforms only do black/white. For gray, a custom
// waveform is loaded into the controller and the two RAMs, which in b/w modes
// hold "new image" and "previous image", hold the two bits of each pixel's
// gray level instead. The refresh is therefore always a full-screen one (the
// controller doesn't know what was on screen). Sequence, waveform and bit
// layout are from Jean-Marc Zingg's GxEPD2_4G library (GPL-3.0, like GxEPD2),
// src/gdey/GxEPD2_420_GDEY042T81.cpp (_Init_4G, writeImage_4G, _Update_4G).
//
// Panel420 is the stock GxEPD2 driver plus writeGray(); all b/w paths are the
// library's own. After a gray refresh the controller holds a custom waveform
// and other settings, so the b/w path is told to re-initialise it
// (_init_display_done = false) before its next write.
#pragma once
#include <GxEPD2_BW.h>

// the gray waveform (227 bytes) and its voltage settings (6 bytes), from GxEPD2_4G
static const uint8_t PANEL420_LUT_4G[233] PROGMEM = {
    0x01, 0x0A, 0x1B, 0x0F, 0x03, 0x01, 0x01,
    0x05, 0x0A, 0x01, 0x0A, 0x01, 0x01, 0x01,
    0x05, 0x08, 0x03, 0x02, 0x04, 0x01, 0x01,
    0x01, 0x04, 0x04, 0x02, 0x00, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x0A, 0x1B, 0x0F, 0x03, 0x01, 0x01,
    0x05, 0x4A, 0x01, 0x8A, 0x01, 0x01, 0x01,
    0x05, 0x48, 0x03, 0x82, 0x84, 0x01, 0x01,
    0x01, 0x84, 0x84, 0x82, 0x00, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x0A, 0x1B, 0x8F, 0x03, 0x01, 0x01,
    0x05, 0x4A, 0x01, 0x8A, 0x01, 0x01, 0x01,
    0x05, 0x48, 0x83, 0x82, 0x04, 0x01, 0x01,
    0x01, 0x04, 0x04, 0x02, 0x00, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x8A, 0x1B, 0x8F, 0x03, 0x01, 0x01,
    0x05, 0x4A, 0x01, 0x8A, 0x01, 0x01, 0x01,
    0x05, 0x48, 0x83, 0x02, 0x04, 0x01, 0x01,
    0x01, 0x04, 0x04, 0x02, 0x00, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x8A, 0x9B, 0x8F, 0x03, 0x01, 0x01,
    0x05, 0x4A, 0x01, 0x8A, 0x01, 0x01, 0x01,
    0x05, 0x48, 0x03, 0x42, 0x04, 0x01, 0x01,
    0x01, 0x04, 0x04, 0x42, 0x00, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x07, 0x17, 0x41, 0xA8,
    0x32, 0x30,
};

class Panel420 : public GxEPD2_420_GDEY042T81 {
 public:
  using GxEPD2_420_GDEY042T81::GxEPD2_420_GDEY042T81;

  static const uint32_t PLANE_BYTES = (WIDTH / 8) * HEIGHT;  // 15000

  // full-screen gray refresh. p24 / p26: the two bit planes, packed MSB first,
  // in controller format (see grayPlanes()). Blocks until the panel is done
  void writeGray(const uint8_t* p24, const uint8_t* p26) {
    if (_hibernating) _reset();
    delay(10);
    _writeCommand(0x12);  // SWRESET
    delay(10);
    _writeCommand(0x0C);  // soft start
    _writeData(0x8B);
    _writeData(0x9C);
    _writeData(0xA4);
    _writeData(0x0F);
    _writeCommand(0x21);  // display update control
    _writeData(0x00);
    _writeData(0x00);
    _writeCommand(0x3C);  // border
    _writeData(0x03);
    _writeCommand(0x32);  // the gray waveform
    _writeDataPGM(PANEL420_LUT_4G, 227);
    _writeCommand(0x3F);
    _writeData(PANEL420_LUT_4G[227]);
    _writeCommand(0x03);  // VGH
    _writeData(PANEL420_LUT_4G[228]);
    _writeCommand(0x04);  // VSH1, VSH2, VSL
    _writeData(PANEL420_LUT_4G[229]);
    _writeData(PANEL420_LUT_4G[230]);
    _writeData(PANEL420_LUT_4G[231]);
    _writeCommand(0x2C);  // VCOM
    _writeData(PANEL420_LUT_4G[232]);

    ramArea();
    _writeCommand(0x26);
    _writeData(p26, PLANE_BYTES);
    ramArea();
    _writeCommand(0x24);
    _writeData(p24, PLANE_BYTES);

    _writeCommand(0x21);  // display update control
    _writeData(0x88);     // b/w inverted, RED inverted
    _writeData(0x00);
    _writeCommand(0x22);
    _writeData(0xCF);     // custom waveform, display mode 2, power off after
    _writeCommand(0x20);
    _waitWhileBusy("gray refresh", 6000);
    _power_is_on = false;
    _init_display_done = false;  // the next b/w write re-initialises the controller
    _initial_write = false;
    _initial_refresh = false;
  }

  // a packed 2-bit frame (code 0 = black .. 3 = white, MSB first) to the two
  // planes: 0x26 gets the inverted high bit, 0x24 the inverted low bit
  static void grayPlanes(const uint8_t* frame, uint8_t* p24, uint8_t* p26) {
    for (uint32_t o = 0; o < PLANE_BYTES; o++) {
      const uint8_t* in = frame + o * 2;  // 8 pixels = 2 input bytes
      uint8_t hi = 0, lo = 0;
      for (int b = 0; b < 2; b++) {
        uint8_t v = in[b];
        for (int k = 0; k < 4; k++) {
          uint8_t code = (v >> 6) & 3;
          hi = (hi << 1) | (code >> 1);
          lo = (lo << 1) | (code & 1);
          v <<= 2;
        }
      }
      p26[o] = ~hi;
      p24[o] = ~lo;
    }
  }

 private:
  void ramArea() {
    _writeCommand(0x11);  // data entry: x increase, y increase
    _writeData(0x03);
    _writeCommand(0x44);
    _writeData(0);
    _writeData((WIDTH - 1) / 8);
    _writeCommand(0x45);
    _writeData(0);
    _writeData(0);
    _writeData((HEIGHT - 1) % 256);
    _writeData((HEIGHT - 1) / 256);
    _writeCommand(0x4E);
    _writeData(0);
    _writeCommand(0x4F);
    _writeData(0);
    _writeData(0);
  }

};
