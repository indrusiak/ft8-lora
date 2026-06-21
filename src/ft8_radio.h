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
// ft8_radio.h - SX1262 (Heltec WiFi LoRa 32 V3) wrapper for FT8-over-LoRa.
//
// For an FT8-style QSO, all parties must share the SAME modem config, 
// so the only things the operator changes are frequency, bandwidth and 
// spreading factor; coding rate, sync word, preamble and CRC are fixed 
// constants below and must match on every board.
//

#include <stdint.h>
#include <functional>

// ---- the fixed half of the FT8-over-LoRa modem (must match across boards) ----
#ifndef FT8L_CR
#define FT8L_CR        5        // coding rate 4/5
#endif
#ifndef FT8L_SYNC
#define FT8L_SYNC      0x14     // private LoRa sync word for FT8/LoRa
#endif
#ifndef FT8L_PREAMBLE
#define FT8L_PREAMBLE  8        // symbols
#endif
#ifndef FT8L_CRC
#define FT8L_CRC       true     // payload CRC on
#endif
#ifndef FT8L_TX_POWER
#define FT8L_TX_POWER  14       // dBm; legal/sane default on 70 cm
#endif

class Ft8Radio {
 public:
  // payload, len, rssi(dBm), snr(dB)
  using RxCallback = std::function<void(const uint8_t*, uint16_t, int16_t, float)>;

  bool begin();
  // Set the operator-tunable modem config and (re)start RX.
  void apply(float freq_mhz, float bw_khz, uint8_t sf);
  void poll();                          // service the RX flag; call often
  // Transmit a payload immediately, then restore RX. Returns RadioLib status.
  int  transmit(const uint8_t* data, uint16_t len);
  int  transmit(const char* s);

  void set_rx_callback(RxCallback cb) { on_rx_ = cb; }

  bool     ready()    const { return ready_; }
  bool     tx_busy()  const { return tx_busy_; }
  float    freq()     const { return freq_; }
  float    bw()       const { return bw_; }
  uint8_t  sf()       const { return sf_; }
  uint32_t rx_count() const { return rx_count_; }
  uint32_t tx_count() const { return tx_count_; }
  uint32_t err_count()const { return err_count_; }
  int      begin_status() const { return begin_status_; }
  float    noise_floor();               // live channel RSSI (dBm)

 private:
  bool config(float freq_mhz, float bw_khz, uint8_t sf);
  RxCallback on_rx_;
  bool   ready_   = false;
  bool   tx_busy_ = false;
  float  freq_ = 433.074f, bw_ = 125.0f;
  uint8_t sf_  = 12;
  int    begin_status_ = -999;
  uint32_t rx_count_ = 0, tx_count_ = 0, err_count_ = 0;
};
