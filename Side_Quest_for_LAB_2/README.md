# RT-Spark Embedded MIDI Karaoke Player (Side Quest for Lab 2)

An embedded, standalone **MIDI Karaoke Player** with real-time **Yamaha FM Synthesis (OPL3/DX style)**, ST7789 240x240 IPS display rendering, and 3.5mm headphone audio output via Everest Semiconductor ES8388 on the **RT-Thread RT-Spark (STM32F407ZGT6)** development board under **Zephyr RTOS**.

---

## 1. Project Overview & Features

- **Synthesizer Engine:** Embedded **40-voice** two-operator FM synthesizer at 44.1 kHz stereo. Each note has its own modulation-index envelope, so a piano starts bright and mellows as it decays. It maps all 128 General MIDI programs onto 53 patches and follows velocity, channel volume (CC7), expression (CC11), pan, sustain pedal, pitch bend with RPN range, and the mod wheel. A soft limiter replaces hard clipping. The sequencer ticks on its own thread and hands notes to the audio thread through a ring buffer. `tools/synth_bench/run.py` renders the real synth on the PC to WAV files and prints measurements.
- **General MIDI & Rhythm Kit:** Supports standard General MIDI instrument families (Pianos, Organs, Guitars, Bass, Strings, Brass, Flutes, Synths) plus full percussive rhythm kit on MIDI Channel 10 (Bass Drum, Snare, Hi-Hats, Toms, Cymbals).
- **Karaoke Library:** Compatible with the 47,998 song database extracted from SongHub (`D:\idx\shub_extracted`).
- **Synchronized Lyrics:** Multi-track SMF sequencer parses Meta Event `0x05` (Lyric) and `0x01` (Text) with syllabic delta-time synchronization. `[Music / Intro]` displays only before the first sung verse, with smooth verse transitions thereafter.
- **Audio Output:** Everest Semiconductor ES8388 Stereo Codec on I2C2 and I2S3 driving the 3.5mm Headphone Jack (CN3) via circular DMA1 Stream 5 double-buffering, plus simultaneous 12-bit analog DAC output on pin `PA4`.
- **Visual Display:** 240x240 ST7789 IPS LCD driven via STM32F407 FSMC (8080 8-bit parallel bus) featuring:
  - Real-time rolling lyric window (Previous line, Active singing line with yellow box, and verse transitions).
  - Dual stereo peak VU meters (Green $\rightarrow$ Yellow $\rightarrow$ Red).
  - Dynamic progress bar, elapsed time, and tempo (BPM).
  - Active voice polyphony HUD (`Voices: X/40`).
- **Operating Modes:**
  - **Player Mode:** Real-time synchronized lyrics and VU meter playback.
  - **Browser Mode:** Paged song selection menu with instantaneous O(1) seeking directly from `SD:/songs.idx`.
  - **Number Select Mode (Direct Song Code Entry):** Press BOTH UP + DOWN simultaneously to enter 5-digit song code entry. Use LEFT/RIGHT to move digits, UP/DOWN to scroll 0-9, and HOLD UP/DOWN or click OK to play.
  - **Audio Settings Mixer Mode:** Press BOTH LEFT + RIGHT simultaneously to open the settings view. Adjust Master Volume (0-100%), Instrument / Melody Volume (20-200%), and Drum / Rhythm Volume (20-200%) using UP/DOWN to select and LEFT/RIGHT to adjust. Values auto-save and survive both reset (via STM32 RTC Backup domain) and shutdown (via `SD:/karaoke.cfg`). Press OK to save and return.
  - **ROM Fallback:** Operates standalone with built-in embedded tracks (Bryan Adams, John Lennon, Itchyworms - Beer) even when no MicroSD card is inserted.
- **PC Searchable Songbook:** Generated `songbook.txt` (7.7 MB) formatted for quick searching in Windows Notepad (`Ctrl + F`). Contains both Numerical and Alphabetical directories.

---

## 2. Hardware Pinout & Wiring

| Subsystem | Peripheral / Signal | RT-Spark Pin | Description |
| :--- | :--- | :--- | :--- |
| **ST7789 LCD** | FSMC D0 – D7 | PD14, PD15, PD0, PD1, PE7, PE8, PE9, PE10 | 8080 8-bit parallel data |
| | FSMC NOE (RD) / NWE (WR) | PD4 / PD5 | Read / Write enable |
| | FSMC A18 (RS) | PD13 | Data / Command select |
| | FSMC NE3 (CS) | PG10 | Chip Select (Bank 1 NOR/SRAM 3) |
| | Backlight / Reset | PF9 / PD3 | Backlight control / Display reset |
| **ES8388 Codec** | I2C2 SCL / SDA | PF1 / PF0 | Software bit-banged register control |
| | I2S3 MCK | PC7 | Master Clock (44.1 kHz PLLI2S) |
| | I2S3 WS (LRCLK) | PA15 | Word Select (Left/Right clock) |
| | I2S3 CK (BCLK) | PB3 | Continuous Bit Clock |
| | I2S3 SD (TX) | PB5 | Serial Data Out (DMA1 Stream 5) |
| **MicroSD Card** | SDIO D0 – D3, CK, CMD | PC8, PC9, PC10, PC11, PC12, PD2 | 4-bit SDIO FATFS interface |
| **D-Pad Controls** | UP (SW2) / DOWN (SW4) | PC5 / PC1 | Previous / Next song (Active LOW) |
| | LEFT (SW3) / RIGHT (SW5)| PC0 / PC4 | Volume Down (-5%) / Volume Up (+5%) |
| | USER / CENTER (PA0) | PA0 | Click: Play/Pause/OK; **Hold (>=400ms): Browse Songs** |
| | **BOTH UP + DOWN** | PC5 + PC1 | **Enter NUMBER SELECT Mode** (Direct 5-Digit Song Entry) |
| | **BOTH LEFT + RIGHT** | PC0 + PC4 | **Enter AUDIO SETTINGS Mixer** (Inst/Drums Volume) |
| **Status LEDs** | Blue / Red | PF11 / PF12 | Playing (Blue) / Paused (Red) |

---

## 3. Preparing the MicroSD Card

To generate the binary index and copy song files from `D:\idx\shub_extracted`:

```bash
# Generate curated 50-song starter package
python tools/prepare_karaoke_sd.py --limit 50 --out /path/to/sd_card

# Or index all 48,000 songs for the entire library
python tools/prepare_karaoke_sd.py --limit 0 --out /path/to/sd_card
```

Copy the generated `songs.idx` and `/midi` directory directly to the root of a FAT32-formatted MicroSD card.

**If `songs.idx` is missing or unusable** (wrong signature, version or record size, or no records), the firmware scans the card instead: `SD:/midi` and the folders directly below it, then the card root, for `.mid`/`.midi` files. Titles and artists come from file names like `027720 - Itchyworms - Beer.mid`; other names are shown by file name. Only the first 192 files found are listed, because the board has little spare RAM, so use `songs.idx` for a large library. A truncated `songs.idx` keeps the records that are present. If the card itself does not initialise (`stm32_sdmmc ... ErrorCode 0x4` at boot) there is nothing to scan, and the three built-in songs are used.

MIDI files are written one folder per thousand song codes (`midi/036/036527.mid`), so no folder holds more than 1000 files. The full library is 47,998 files in 56 folders and about 2 GB. The firmware builds the same path in `src/song_path.c` and falls back to a flat `midi/036527.mid` card if the sharded file is not found. The tool exits with an error if any song fails to copy.

---

## 4. Verification & Testing

### Host-Based Unit Testing (PC)
```bash
pio test -e native
```
- `test_midi_fm` re-implements the SMF header, VLQ, tempo and FM math inside the test file, so it checks the formulas, not the firmware sources.
- `test_song_path` compiles `src/song_path.c` itself and checks the SD path rule.
- `test_song_scan` compiles `src/song_scan.c` and checks file-name filtering, parsing and the path list.
- `test_catalog` compiles the real `src/karaoke_catalog.c` against a folder on the PC that stands in for the SD card (stand-ins in `test/test_catalog/`). It covers a valid, missing, corrupt, truncated and empty `songs.idx`, scanning, an unmounted or empty card, and the built-in songs.
- `test_synth` renders the real `src/yamaha_fm_synth.c` and measures pitch accuracy, decay, brightness, velocity, release, sustain pedal, pitch bend, drums, voice stealing and all 128 programs.
- `test_settings` validates non-volatile RTC backup register bit packing/unpacking, SD configuration file parsing, parameter boundary clamping, and non-inverted analog gain mapping.
- Result: **76/76 tests passing**.

### Static Code Analysis
```bash
pio check
```
- Runs Cppcheck static code analysis across all source files.
- Result: **0 defects found**.

### Building Target Firmware
```bash
pio run -e black_f407zg
```
- Compiles ARM Cortex-M4 binary with 0 warnings.
- Output binary: `.pio/build/black_f407zg/firmware.bin` (Flash: ~217 KB / 20.7%, RAM: ~106 KB).
