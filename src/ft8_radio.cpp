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

#include "ft8_radio.h"
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// Heltec WiFi LoRa 32 V3 SX1262 wiring:
//   NSS=8  DIO1=14  NRST=12  BUSY=13   SPI: SCK=9 MISO=11 MOSI=10.
static SX1262 g_radio = new Module(8, 14, 12, 13);

static volatile bool g_rx_flag = false;
#if defined(ESP32)
IRAM_ATTR
#endif
static void on_dio1() { g_rx_flag = true; }

bool Ft8Radio::config(float freq_mhz, float bw_khz, uint8_t sf) {
  int s;
  s = g_radio.setFrequency(freq_mhz);        if (s != RADIOLIB_ERR_NONE) return false;
  s = g_radio.setBandwidth(bw_khz);          if (s != RADIOLIB_ERR_NONE) return false;
  s = g_radio.setSpreadingFactor(sf);        if (s != RADIOLIB_ERR_NONE) return false;
  s = g_radio.setCodingRate(FT8L_CR);        if (s != RADIOLIB_ERR_NONE) return false;
  s = g_radio.setSyncWord(FT8L_SYNC);        if (s != RADIOLIB_ERR_NONE) return false;
  s = g_radio.setPreambleLength(FT8L_PREAMBLE); if (s != RADIOLIB_ERR_NONE) return false;
  s = g_radio.setCRC(FT8L_CRC);              if (s != RADIOLIB_ERR_NONE) return false;
  return true;
}

bool Ft8Radio::begin() {
  // Bind SPI to the V3's LoRa pins BEFORE radio.begin(): arduino-esp32's
  // SPI.begin() returns early if the bus is up, so RadioLib won't override these.
  SPI.begin(9, 11, 10, 8);   // SCK, MISO, MOSI, SS

  // begin(freq, bw, sf, cr, sync, power, preamble, tcxoVoltage, useLDO=false)
  begin_status_ = g_radio.begin(freq_, bw_, sf_, FT8L_CR, FT8L_SYNC,
                                FT8L_TX_POWER, FT8L_PREAMBLE, 1.8f, false);
  if (begin_status_ != RADIOLIB_ERR_NONE) {
    Serial.printf("[radio] begin FAILED %d (check SPI pins / TCXO)\n", begin_status_);
    return false;
  }
  int s = g_radio.setDio2AsRfSwitch(true);   // antenna T/R switch on the V3
  if (s != RADIOLIB_ERR_NONE) Serial.printf("[radio] setDio2AsRfSwitch warn %d\n", s);
  g_radio.setRxBoostedGainMode(true);        // a few dB more RX sensitivity
  g_radio.setCRC(FT8L_CRC);
  g_radio.setDio1Action(on_dio1);
  ready_ = true;
  apply(freq_, bw_, sf_);
  Serial.println("[radio] init OK (SPI bound, DIO2 RF switch, TCXO 1.8V, boosted RX)");
  return true;
}

void Ft8Radio::apply(float freq_mhz, float bw_khz, uint8_t sf) {
  freq_ = freq_mhz; bw_ = bw_khz; sf_ = sf;
  if (!ready_) return;
  g_radio.standby();
  if (!config(freq_, bw_, sf_)) { Serial.println("[radio] config failed"); return; }
  g_rx_flag = false;
  int s = g_radio.startReceive();
  Serial.printf("[radio] %.4fMHz BW%.0f SF%u CR4/%u sync0x%02X startReceive=%d\n",
                freq_, bw_, sf_, FT8L_CR, FT8L_SYNC, s);
}

void Ft8Radio::poll() {
  if (!ready_ || !g_rx_flag) return;
  g_rx_flag = false;

  uint8_t buf[256];
  size_t len = g_radio.getPacketLength();
  if (len > sizeof(buf)) len = sizeof(buf);
  int s = g_radio.readData(buf, len);
  float snr = g_radio.getSNR();
  int16_t rssi = (int16_t)g_radio.getRSSI();
  g_radio.startReceive();                    // re-arm immediately

  if (s == RADIOLIB_ERR_NONE && len > 0) {
    rx_count_++;
    Serial.printf("[radio] RX %u bytes rssi=%d snr=%.1f\n", (unsigned)len, rssi, snr);
    if (on_rx_) on_rx_(buf, (uint16_t)len, rssi, snr);
  } else {
    err_count_++;
    Serial.printf("[radio] RX error %d\n", s);
  }
}

float Ft8Radio::noise_floor() {
  if (!ready_) return 0;
  return g_radio.getRSSI(false);             // instantaneous channel RSSI
}

int Ft8Radio::transmit(const uint8_t* data, uint16_t len) {
  if (!ready_) return -1;
  tx_busy_ = true;
  g_radio.standby();
  config(freq_, bw_, sf_);
  g_radio.setOutputPower(FT8L_TX_POWER);
  int s = g_radio.transmit((uint8_t*)data, len);
  if (s == RADIOLIB_ERR_NONE) tx_count_++;
  else Serial.printf("[radio] TX failed %d\n", s);
  g_rx_flag = false;
  g_radio.startReceive();
  tx_busy_ = false;
  return s;
}

int Ft8Radio::transmit(const char* str) {
  return transmit((const uint8_t*)str, (uint16_t)strlen(str));
}
