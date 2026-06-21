# FT8/LoRa
(pronounced FT8 over LoRa)

Copyright 2026 Leandro Soares Indrusiak (G5LSI)


Web-controlled firmware that runs the **FT8 message protocol and QSO workflow**
over a LoRa link, controlled by the Heltec WiFi LoRa 32 V3 board (ESP32-S3 +
SX1262 + SSD1306).

---

## What this is (and what it is not)

"FT8 over LoRa" here means the FT8 *application
layer* — the message grammar and the QSO state machine — carried as plain ASCII
text inside ordinary LoRa packets:

- the standard message exchange `CQ → call+grid → signal report → R-report →
  RR73 / 73`,
- Maidenhead grid locators,
- signed dB signal reports,
- per-QSO logging and ADIF export.

It is **not** the FT8 *physical layer*. Real FT8 is an 8-FSK waveform in
15-second slots with a 79-symbol Costas/LDPC structure, decoded down to
roughly −24 dB S/N. The SX1262 cannot synthesise that waveform, so none of that
is attempted here. Instead each FT8-style message is sent as a LoRa frame
(CSS / chirp modulation), and the **signal report is synthesised from the LoRa
RSSI** of the received frame. This is the same idea as LoRa-APRS: a well-known
ham application protocol re-homed onto a LoRa PHY.


---

[<img src="https://img.youtube.com/vi/3_DpiqInHS8/maxresdefault.jpg" width="600" height="300"
/>](https://www.youtube.com/embed/3_DpiqInHS8)

---


## Hardware

Heltec WiFi LoRa 32 V3: ESP32-S3, SX1262 radio, SSD1306 128×64 OLED.
Pin map and the proven V3 bring-up sequence (`heltec_display.h`, 
`ft8_radio.cpp`): `SPI.begin(9,11,10,8)` before
`radio.begin`, `Module(8,14,12,13)`, DIO2 RF switch, TCXO on DIO3 @ 1.8 V,
boosted RX gain. OLED: Vext on GPIO36 (LOW = on), reset pulse on GPIO21, I²C
SDA 17 / SCL 18 at 0x3c.

---

## Using it

1. Flash (see below, or download prebuilt binary from `prebuilt_firmware`
   directory), power up. The board starts a Wi-Fi access point
   **`FT8LoRa-XXXX`** (open). Connect and browse to `http://192.168.4.1/`.
   (Optional HTTP Basic auth is **off by default** — see *Security* below.)
2. Set **callsign** (default `G5LSI`), **grid** (default `IO93`),
     **frequency** (default 439.9125 MHz, the UK-coordinated 70cm LoRa channel),
   **spreading factor** (default SF12) and **bandwidth** (default 125 kHz),
   then **Apply**. All boards in a net must share frequency, SF and bandwidth.
3. The scrolling window shows decoded traffic, WSJT-X style: UTC time, the dB
   report derived from RSSI, raw RSSI, and the message. CQ lines are green and
   clickable; lines addressed to you are blue and clickable; your own transmits
   echo in amber.
4. Type in the **TX** field and press **TX** (or Enter) to transmit
   immediately. The text **stays in the field** so you can retransmit by
   pressing TX again.
5. The last message addressed to your callsign is **highlighted automatically**;
   click any line in the window to select a different one. The **Answer** button
   generates the appropriate reply to the highlighted line (answering a CQ, or
   continuing a QSO addressed to you) and puts it in the TX field. The **CQ**
   button loads `CQ <mycall> <mygrid>` into the TX field. Neither transmits on
   its own — they only compose. Incoming replies to your own calls also auto-fill
   the TX field. **Reset QSO** clears the current contact context.
6. A finished QSO (after RR73 / 73) is logged automatically. **Export ADIF**
   opens `/log.adi` in a new page; save it and import into your logger.

### The OLED shows
- line 0: your callsign + grid
- line 1: status — `freq SF rssi RX/TX`
- line 2: the latest message addressed to your callsign (or, if none yet, the
  latest decoded message), or your latest transmit (whichever is newest);
  transmits are shown `TX …`
- line 3: RX / TX / QSO counters

---

## The FT8 QSO protocol (`test/ft8.js`)

All protocol interpretation lives in pure JavaScript that runs 
in the browser and under Node. The firmware itself just moves bytes and 
renders status — it never parses FT8 semantics. `test/ft8.js` provides 
field predicates, `rssiToReport`, a message parser, the QSO state 
machine (`answerCQ`, `callCQ`, `onTx`, `onRx`, `suggestReply`, `makeRecord`) 
handling both roles (you call CQ, or you answer someone), auto-fill, completion 
detection and de-duplicated logging.

Run the tests:

```
cd test && node ft8.test.js     # 54 checks, all passing
```

---

## Build / flash

```
pio run                         # compile
pio run -t upload               # flash over USB
pio device monitor              # 115200 baud
```

### Pre-built / release firmware

Every `pio run` also produces a single merged image via the `post_build.py`
hook wired into `platformio.ini`:

```
.pio/build/heltec_wifi_lora_32_v3/firmware.bin   # app only (offset 0x10000)
.pio/build/heltec_wifi_lora_32_v3/ft8lora.bin    # full image, flash @ 0x0
```

`ft8lora.bin` is the artifact to publish: it bundles the bootloader, partition
table, boot_app0 and the application at their correct offsets, so it can be
flashed to a blank board in one shot, or dropped into a browser flasher such as
ESP Web Tools. Flash it with:

```
esptool.py --chip esp32s3 write_flash 0x0 ft8lora.bin
```

(`firmware.bin` alone is only the app partition and will not boot a blank board
on its own.) The hook reads all offsets and flash parameters from PlatformIO's
own build state, so they always match this board.


### Regenerating `web_ui.h`

`src/web_ui.h` is generated from `test/page_app.html` (the UI shell, with a
`/*__FT8__*/` placeholder) and `test/ft8.js` (the brain). `ft8.js` is spliced in
**verbatim** — it self-detects its environment (`module.exports` under Node,
`root.FT8` in the browser), so it must not be edited or truncated. To rebuild:

```
cd lora_ft8
python3 - <<'PY'
page = open('test/page_app.html', encoding='utf-8').read()
js   = open('test/ft8.js', encoding='utf-8').read()
assert page.count('/*__FT8__*/') == 1
page = page.replace('/*__FT8__*/', js)
assert ')FT8PAGE' not in page
open('src/web_ui.h','w',encoding='utf-8').write(
  '#pragma once\n#include <Arduino.h>\n\n'
  'static const char INDEX_HTML[] PROGMEM = R"FT8PAGE(\n'+page+'\n)FT8PAGE";\n')
print('ok')
PY
```

---

## Layout

```
platformio.ini          board/env/libs 
src/
  main.cpp              AP + web server + endpoints + OLED render
  ft8_radio.{h,cpp}     SX1262 wrapper 
  rx_store.h            RX ring buffer (browser polls via /api/rx?since=)
  qso_log.h             QSO ring + ADIF field emitter
  heltec_display.h      OLED helper 
  web_ui.h              generated PROGMEM page (do not hand-edit)
test/
  ft8.js               protocol brain (browser + Node)
  ft8.test.js          54 checks
  page_app.html        UI shell with /*__FT8__*/ splice point
```

### HTTP endpoints
`GET /` · `GET /api/status` · `GET /api/rx?since=` · `POST /api/config` ·
`POST /api/tx` · `POST /api/log` · `GET /api/log` · `POST /api/log/clear` ·
`GET /log.adi`

All POSTs send `application/x-www-form-urlencoded` named fields, which the
ESP32 `WebServer` parses into `server.arg("name")` reliably. 

---

## Additional design decisions

- **RSSI → report anchors.** `rssiToReport` maps −120 dBm → −23 and −28 dBm →
  +23, ~2 dB/unit, clamped. These anchors (`RSSI_LO`/`RSSI_HI` in `ft8.js`) are
  a guess at a useful dynamic range for 433 MHz LoRa, not a calibrated mapping.

- **ADIF `MODE`.** Logged as `MODE=DATA` with `COMMENT="FT8 protocol over
  LoRa (SX1262)"`. `DATA` is the generic choice for a digital mode that
  isn't on-air FT8; change it in `main.cpp` (the `adif_field(o, "MODE", …)`
  line) if ADIF standard includes LoRa or CSS (chirp spread spectrum).
- **Immediate TX.** Pressing TX transmits at once. Real FT8 is slotted to
  even/odd 15 s boundaries; nothing here enforces slots or alternation.
- **Fixed net parameters.** Coding rate 4/5, sync word `0x12`, preamble 8,
  CRC on, 14 dBm TX (`FT8L_*` in `ft8_radio.h`). These must match across all
  boards; they're `#define`s so a net can agree on different values.
- **Timestamps.** `qso_date`/`time_on` are UTC taken from the *browser* clock
  at log time (the board has no RTC/NTP). 
- **TX shown in the RX window.** Your own transmissions echo into the scroll
  (amber) so you can see the full exchange in one place. Toggle in the UI logic
  if you'd rather keep the window receive-only.
- **Open AP.** The access point is open and the web UI's optional Basic auth is
  **off by default** (`WWW_AUTH 0` in `main.cpp`). On an open AP the gate adds
  no real protection and its 401/retry handshake can itself break POSTs, so it
  starts disabled; set `WWW_AUTH 1` to turn it back on.


