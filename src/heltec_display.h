#pragma once
// Copyright 2026 Leandro Soares Indrusiak (G5LSI)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// heltec_display.h - a small, reusable OLED helper for the Heltec WiFi LoRa 32
// V3 (ESP32-S3 + SSD1306 128x64).
//
// The V3 board has three things that trip people up, and ALL THREE must be done
// or the screen stays black:
//
//   1. The OLED is powered through Vext. Vext is enabled by driving GPIO36 LOW.
//   2. The OLED has a hardware reset line on GPIO21 that must be pulsed
//      (LOW -> wait -> HIGH) *before* I2C init. This is the step that is missing
//      from most examples and the usual reason a "correct" setup shows nothing.
//   3. The I2C pins are SDA=17, SCL=18 (not the defaults).
//
// This header wraps the Meshtastic fork of the ssd1306 library (SSD1306Wire)
// and hides those details behind a tiny status-screen API tailored to the 
// FT8/LoRa firmware. It is deliberately self-contained: include it, make one
// HeltecDisplay, call begin(), then draw.
//
// Only compiled on the device; on the host build it is simply never included.
//
#include <Arduino.h>
#include <Wire.h>
#include "SSD1306Wire.h"

class HeltecDisplay {
 public:
  // Heltec V3 pin assignment. Exposed as constants so callers can reuse them.
  static constexpr int PIN_SDA      = 17;
  static constexpr int PIN_SCL      = 18;
  static constexpr int PIN_VEXT     = 36;  // LOW = display powered
  static constexpr int PIN_OLED_RST = 21;  // pulse LOW->HIGH before I2C init
  static constexpr uint8_t I2C_ADDR = 0x3c;

  HeltecDisplay() : oled_(I2C_ADDR, PIN_SDA, PIN_SCL) {}

  // Power, reset, and initialise the panel. Returns true once ready.
  bool begin() {
    // 1. Power the rail that feeds the OLED.
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);
    delay(100);

    // 2. Hardware-reset the controller. THIS is the easy-to-miss step.
    pinMode(PIN_OLED_RST, OUTPUT);
    digitalWrite(PIN_OLED_RST, LOW);
    delay(50);
    digitalWrite(PIN_OLED_RST, HIGH);
    delay(50);

    // 3. Bring up I2C on the correct pins and init the panel.
    Wire.begin(PIN_SDA, PIN_SCL);
    delay(50);
    oled_.init();
    oled_.flipScreenVertically();
    oled_.setTextAlignment(TEXT_ALIGN_LEFT);
    oled_.setFont(ArialMT_Plain_10);
    ready_ = true;
    return ready_;
  }

  bool ready() const { return ready_; }

  // ---- Convenience: a few ready-made screens for the FT8/LoRa firmware -------

  // Boot / access-point screen.
  void show_boot(const String& board_id, const String& ssid, const String& ip) {
    if (!ready_) return;
    oled_.clear();
    oled_.setFont(ArialMT_Plain_16);
    oled_.drawString(0, 0, "FT8/LoRa");
    oled_.setFont(ArialMT_Plain_10);
    oled_.drawString(0, 20, "by G5LSI");
    oled_.drawString(0, 34, "Board " + board_id);
    oled_.drawString(0, 48, ssid);
    oled_.display();
  }

  // Main status screen used while a script runs.
  //   line 1: board id + state
  //   line 2: listening SF (or "idle") + frames heard
  //   line 3: detail
  //   line 4: a free-form message (e.g. the script's show("...") text)
  void show_status(const String& board_id, const String& state,
                   bool listening, uint8_t sf, uint32_t frames,
                   const String& detail, const String& message) {
    if (!ready_) return;
    oled_.clear();
    oled_.setFont(ArialMT_Plain_10);
    oled_.drawString(0, 0,  board_id + "  " + state);
    if (listening) oled_.drawString(0, 14, "RX SF" + String(sf) + "  frames " + String(frames));
    else           oled_.drawString(0, 14, "idle      frames " + String(frames));
    oled_.drawString(0, 30, detail);
    oled_.drawString(0, 46, message);
    oled_.display();
  }

  // Lowest-level escape hatch: clear, draw up to four lines, flush.
  void show_lines(const String& l0, const String& l1 = "",
                  const String& l2 = "", const String& l3 = "") {
    if (!ready_) return;
    oled_.clear();
    oled_.setFont(ArialMT_Plain_10);
    oled_.drawString(0, 0,  l0);
    oled_.drawString(0, 16, l1);
    oled_.drawString(0, 32, l2);
    oled_.drawString(0, 48, l3);
    oled_.display();
  }

  // Direct access if a caller wants the raw driver.
  SSD1306Wire& raw() { return oled_; }

 private:
  SSD1306Wire oled_;
  bool ready_ = false;
};
