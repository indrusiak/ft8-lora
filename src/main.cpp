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

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <string.h>

#include "heltec_display.h"
#include "ft8_radio.h"
#include "rx_store.h"
#include "qso_log.h"
#include "web_ui.h"

WebServer     server(80);
Ft8Radio      radio;
RxStore       rxs;
QsoLog        qsos;
HeltecDisplay oled;

// ---- soft web auth (HTTP Basic). Open AP, so treat this as a gate, not armour.
// Off by default: on an open AP it adds no real protection, and the 401/retry
// handshake is a known source of "POST silently does nothing" grief. Set to 1
// to re-enable.
#define WWW_AUTH 0
#define WWW_USER "admin"
#define WWW_PASS "ft8-lora"

// ---- session configuration ------------------------------------------------
struct Config {
  char    callsign[16] = "G5LSI";
  char    grid[10]     = "IO93";
  float   freq         = 439.9125f;  // UK-coordinated 70cm low-power LoRa channel (125 kHz max BW)
  float   bw           = 125.0f;
  uint8_t sf           = 12;
} cfg;

char     board_id[16] = "------";
bool     disp_dirty   = true;
uint32_t last_disp_ms = 0;
float    g_noise      = 0;

// display "line 2" sources (latest msg to me / any / my last TX)
char     last_rx_to_me[RX_MSG_MAX + 1] = "";
char     last_rx_any[RX_MSG_MAX + 1]   = "";
char     last_tx[RX_MSG_MAX + 1]       = "";
uint32_t last_rx_to_me_ms = 0, last_rx_any_ms = 0, last_tx_ms = 0;

// ---- helpers --------------------------------------------------------------
static bool addressed_to_me(const char* text) {
  // First whitespace-delimited token; "CQ" is addressed to nobody in particular.
  char tok[24]; int i = 0, j = 0;
  while (text[i] == ' ') i++;
  while (text[i] && text[i] != ' ' && j < 23) tok[j++] = text[i++];
  tok[j] = '\0';
  if (strcasecmp(tok, "CQ") == 0) return false;
  return strcasecmp(tok, cfg.callsign) == 0;
}

static void json_escape(String& out, const char* s) {
  for (const char* p = s; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') {}
    else out += c;
  }
}

// ---- display --------------------------------------------------------------
static String disp_message_line() {
  uint32_t rxt = last_rx_to_me_ms ? last_rx_to_me_ms : last_rx_any_ms;
  const char* rxs_txt = last_rx_to_me_ms ? last_rx_to_me : last_rx_any;
  if (last_tx_ms && last_tx_ms >= rxt) return String("TX ") + last_tx;
  if (rxt) return String(rxs_txt);
  return String("(waiting...)");
}

static void render_display() {
  if (!radio.ready()) {
    char b[26]; snprintf(b, sizeof(b), "begin=%d", radio.begin_status());
    oled.show_lines("RADIO INIT FAIL", b, "check SPI/TCXO", "no RX/TX possible");
    disp_dirty = false; last_disp_ms = millis();
    return;
  }
  g_noise = radio.noise_floor();
  bool txing = (millis() - last_tx_ms) < 1200 && last_tx_ms != 0;

  char l0[26]; snprintf(l0, sizeof(l0), "%s  %s", cfg.callsign, cfg.grid);
  // status line: freq, SF, rssi, RX/TX
  char l1[26]; snprintf(l1, sizeof(l1), "%.3f SF%u %d %s",
                        cfg.freq, cfg.sf, (int)g_noise, txing ? "TX" : "RX");
  // message line (truncated to fit)
  String msg = disp_message_line();
  if (msg.length() > 21) msg = msg.substring(0, 21);
  char l3[26]; snprintf(l3, sizeof(l3), "RX %lu TX %lu QSO %d",
                        (unsigned long)radio.rx_count(), (unsigned long)radio.tx_count(), qsos.count());

  oled.show_lines(l0, l1, msg, l3);
  disp_dirty = false; last_disp_ms = millis();
}

// ---- auth -----------------------------------------------------------------
static bool need_auth() {
#if WWW_AUTH
  if (!server.authenticate(WWW_USER, WWW_PASS)) { server.requestAuthentication(); return true; }
#endif
  return false;
}

// ---- HTTP handlers --------------------------------------------------------
static void handle_root() { if (need_auth()) return; server.send_P(200, "text/html", INDEX_HTML); }

static void handle_status() {
  if (need_auth()) return;
  bool txing = (millis() - last_tx_ms) < 1200 && last_tx_ms != 0;
  String j; j.reserve(384);
  j += "{\"callsign\":\""; json_escape(j, cfg.callsign); j += "\"";
  j += ",\"grid\":\""; json_escape(j, cfg.grid); j += "\"";
  j += ",\"freq\":"; j += String(cfg.freq, 4);
  j += ",\"bw\":"; j += String(cfg.bw, 0);
  j += ",\"sf\":"; j += cfg.sf;
  j += ",\"rssi\":"; j += String(g_noise, 0);
  j += ",\"state\":\""; j += (txing ? "TX" : "RX"); j += "\"";
  j += ",\"rx\":"; j += radio.rx_count();
  j += ",\"tx\":"; j += radio.tx_count();
  j += ",\"err\":"; j += radio.err_count();
  j += ",\"count\":"; j += rxs.count();
  j += ",\"cap\":"; j += rxs.capacity();
  j += ",\"qsos\":"; j += qsos.count();
  j += "}";
  server.send(200, "application/json", j);
}

static void handle_rx() {
  if (need_auth()) return;
  uint32_t since = 0;
  if (server.hasArg("since")) since = (uint32_t)strtoul(server.arg("since").c_str(), nullptr, 10);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "[");
  bool first = true;
  rxs.for_each_since(since, 80, [&](const RxMsg& m) {
    String o = first ? "" : ",";
    first = false;
    o += "{\"id\":"; o += m.id;
    o += ",\"t\":"; o += m.t_ms;
    o += ",\"rssi\":"; o += m.rssi;
    o += ",\"snr\":"; o += String(m.snr_cdb / 100.0f, 1);
    o += ",\"text\":\""; json_escape(o, m.text); o += "\"}";
    server.sendContent(o);
  });
  server.sendContent("]");
  server.sendContent("");
}

static void handle_config() {
  if (need_auth()) return;
  // Body is application/x-www-form-urlencoded; the WebServer parses it into
  // named args reliably (unlike arg("plain") with a JSON body).
  if (server.hasArg("callsign")) {
    strncpy(cfg.callsign, server.arg("callsign").c_str(), sizeof(cfg.callsign) - 1);
    cfg.callsign[sizeof(cfg.callsign) - 1] = '\0';
  }
  if (server.hasArg("grid")) {
    strncpy(cfg.grid, server.arg("grid").c_str(), sizeof(cfg.grid) - 1);
    cfg.grid[sizeof(cfg.grid) - 1] = '\0';
  }
  for (char* p = cfg.callsign; *p; p++) *p = toupper(*p);
  for (char* p = cfg.grid; *p; p++) *p = toupper(*p);
  if (server.hasArg("freq")) cfg.freq = server.arg("freq").toFloat();
  if (server.hasArg("bw"))   cfg.bw   = server.arg("bw").toFloat();
  if (server.hasArg("sf"))   cfg.sf   = (uint8_t)server.arg("sf").toInt();
  if (cfg.sf < 7)  cfg.sf = 7;
  if (cfg.sf > 12) cfg.sf = 12;
  radio.apply(cfg.freq, cfg.bw, cfg.sf);
  disp_dirty = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handle_tx() {
  if (need_auth()) return;
  if (!server.hasArg("text")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"no text arg\"}"); return; }
  String txt = server.arg("text");
  const char* text = txt.c_str();
  if (!text[0]) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty\"}"); return; }

  int s = radio.transmit(text);
  // remember as the latest TX for the display line
  strncpy(last_tx, text, RX_MSG_MAX); last_tx[RX_MSG_MAX] = '\0';
  last_tx_ms = millis();
  disp_dirty = true;

  String j = "{\"ok\":"; j += (s == 0 ? "true" : "false");
  j += ",\"status\":"; j += s; j += "}";
  server.send(200, "application/json", j);
}

static void handle_log_post() {
  if (need_auth()) return;
  QsoRecord r{};
  auto cp = [](char* dst, size_t n, const String& src) { strncpy(dst, src.c_str(), n - 1); dst[n - 1] = '\0'; };
  cp(r.call,     sizeof(r.call),     server.arg("call"));
  cp(r.gridsq,   sizeof(r.gridsq),   server.arg("gridsquare"));
  cp(r.rst_sent, sizeof(r.rst_sent), server.arg("rst_sent"));
  cp(r.rst_rcvd, sizeof(r.rst_rcvd), server.arg("rst_rcvd"));
  cp(r.qso_date, sizeof(r.qso_date), server.arg("qso_date"));
  cp(r.time_on,  sizeof(r.time_on),  server.arg("time_on"));
  cp(r.mycall,   sizeof(r.mycall),   server.hasArg("mycall") ? server.arg("mycall") : String(cfg.callsign));
  cp(r.mygrid,   sizeof(r.mygrid),   server.hasArg("mygrid") ? server.arg("mygrid") : String(cfg.grid));
  r.freq_mhz = server.hasArg("freq") ? server.arg("freq").toFloat() : cfg.freq;
  qsos.add(r);
  disp_dirty = true;
  server.send(200, "application/json", "{\"ok\":true,\"qsos\":" + String(qsos.count()) + "}");
}

static void handle_log_get() {
  if (need_auth()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "[");
  bool first = true;
  qsos.for_each([&](const QsoRecord& r) {
    if (!r.used) return;
    String o = first ? "" : ",";
    first = false;
    o += "{\"call\":\""; json_escape(o, r.call); o += "\"";
    o += ",\"gridsquare\":\""; json_escape(o, r.gridsq); o += "\"";
    o += ",\"rst_sent\":\""; json_escape(o, r.rst_sent); o += "\"";
    o += ",\"rst_rcvd\":\""; json_escape(o, r.rst_rcvd); o += "\"";
    o += ",\"qso_date\":\""; json_escape(o, r.qso_date); o += "\"";
    o += ",\"time_on\":\""; json_escape(o, r.time_on); o += "\"";
    o += ",\"freq\":"; o += String(r.freq_mhz, 4); o += "}";
    server.sendContent(o);
  });
  server.sendContent("]");
  server.sendContent("");
}

static void handle_log_clear() {
  if (need_auth()) return;
  qsos.clear(); disp_dirty = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

// ADIF export - served as a downloadable/openable text page.
static void handle_log_adif() {
  if (need_auth()) return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Disposition", "inline; filename=\"ft8lora_log.adi\"");
  server.send(200, "text/plain", "");
  String hdr;
  hdr += "ADIF export from FT8-over-LoRa (Heltec V3)\n";
  hdr += "<ADIF_VER:5>3.1.4\n";
  hdr += "<PROGRAMID:11>FT8-over-LoRa\n";
  hdr += "<EOH>\n\n";
  server.sendContent(hdr);
  qsos.for_each([&](const QsoRecord& r) {
    if (!r.used) return;
    String o;
    adif_field(o, "CALL", r.call);
    adif_field(o, "GRIDSQUARE", r.gridsq);
    adif_field(o, "MODE", "DATA");
    adif_field(o, "SUBMODE", "");
    adif_field(o, "RST_SENT", r.rst_sent);
    adif_field(o, "RST_RCVD", r.rst_rcvd);
    adif_field(o, "QSO_DATE", r.qso_date);
    adif_field(o, "TIME_ON", r.time_on);
    if (r.freq_mhz > 0)   adif_field(o, "FREQ", String(r.freq_mhz, 4));
    const char* band = QsoLog::band_for(r.freq_mhz);
    if (band && *band) adif_field(o, "BAND", band);
    adif_field(o, "STATION_CALLSIGN", r.mycall);
    adif_field(o, "OPERATOR", r.mycall);
    adif_field(o, "MY_GRIDSQUARE", r.mygrid);
    adif_field(o, "COMMENT", "FT8 protocol over LoRa (SX1262)");
    o += "<EOR>\n";
    server.sendContent(o);
  });
  server.sendContent("");
}

// ---- setup / loop ---------------------------------------------------------
static void init_board_id() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  snprintf(board_id, sizeof(board_id), "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  oled.begin();
  oled.show_lines("FT8 over LoRa", "booting...");

  rxs.clear();
  qsos.clear();
  init_board_id();

  if (!radio.begin()) Serial.println("[boot] radio init failed");
  radio.apply(cfg.freq, cfg.bw, cfg.sf);
  radio.set_rx_callback([](const uint8_t* d, uint16_t len, int16_t rssi, float snr) {
    uint32_t id = rxs.add(d, len, rssi, snr);
    (void)id;
    // keep the display's "line 2" sources current
    const RxMsg* m = nullptr;  // re-read the text we just stored cheaply
    // (simplest: rebuild text here from d)
    char text[RX_MSG_MAX + 1]; uint16_t j = 0;
    uint16_t n = len > RX_MSG_MAX ? RX_MSG_MAX : len;
    for (uint16_t i = 0; i < n; i++) { char c = (char)d[i]; text[j++] = (c >= 0x20 && c < 0x7f) ? c : '.'; }
    text[j] = '\0';
    strncpy(last_rx_any, text, RX_MSG_MAX); last_rx_any[RX_MSG_MAX] = '\0';
    last_rx_any_ms = millis();
    if (addressed_to_me(text)) {
      strncpy(last_rx_to_me, text, RX_MSG_MAX); last_rx_to_me[RX_MSG_MAX] = '\0';
      last_rx_to_me_ms = millis();
    }
    (void)m;
    disp_dirty = true;
  });

  WiFi.mode(WIFI_AP);
  String ssid = "FT8LoRa-" + String(board_id);
  WiFi.softAP(ssid.c_str(), "");
  String ip = WiFi.softAPIP().toString();
  Serial.printf("[boot] AP %s at %s\n", ssid.c_str(), ip.c_str());

  server.on("/", HTTP_GET, handle_root);
  server.on("/api/status", HTTP_GET, handle_status);
  server.on("/api/rx", HTTP_GET, handle_rx);
  server.on("/api/config", HTTP_POST, handle_config);
  server.on("/api/tx", HTTP_POST, handle_tx);
  server.on("/api/log", HTTP_POST, handle_log_post);
  server.on("/api/log", HTTP_GET, handle_log_get);
  server.on("/api/log/clear", HTTP_POST, handle_log_clear);
  server.on("/log.adi", HTTP_GET, handle_log_adif);
  server.begin();

  oled.show_boot(String(board_id), ssid, ip);
  delay(800);
  disp_dirty = true;
}

void loop() {
  server.handleClient();
  radio.poll();
  uint32_t now = millis();
  if (disp_dirty || (now - last_disp_ms) > 500) render_display();
  delay(1);
}
