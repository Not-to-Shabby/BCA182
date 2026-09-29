# Personal MP3 Player (Zephyr RTOS)

**Course**: BCA182 – Embedded Systems Programming  
**Author**: Perch Arnel II Montefalcon  
**Institution**: Mindanao State University – Iligan Institute of Technology (MSU-IIT)  
**Instructor / Evaluator**: Paul Rodolf P. Castor (`paulrodolf.castor@g.msuiit.edu.ph`)  
**Target Platform**: RT-Thread Spark Development Board ("星火 1 号", STM32F407ZGT6, ARM Cortex-M4F @ 168 MHz)  
**RTOS**: Zephyr RTOS v4.x (Cooperative Multithreading & Native Kernel Services)  
**Framework**: Zephyr PlatformIO (`framework = zephyr`, `board = black_f407zg`) — *Zero Arduino abstractions*  

---

## 1. Project Overview

This project implements a concurrent, real-time embedded **Personal MP3 Player** deployed on the **RT-Thread Spark Development Board (STM32F407ZGT6)** using **Zephyr RTOS**.

The application fulfills all engineering requirements specified in **Laboratory Activity 2**:
- **8-Song Classical Repertoire**: Direct access to 8 musical compositions defined in `reference/song_def.h`.
- **Binary Button Song Selection**: Buttons 2, 3, and 4 form a 3-bit binary song selector ($2^3 = 8$ songs, indices `000` to `111`).
- **5-Second Confirmation Window**: A two-step selection gesture latches the prospective song upon pressing Button 1, requiring a second press of Button 1 within 5 seconds to confirm. If 5 seconds elapse without confirmation, the system aborts the change and resumes previous playback.
- **Audio Transport Control**: On-board `USER_BUTTON` (`PA0` / `PC5`) toggles between Play, Pause, and Replay.
- **RGB LED State Indicators**:
  - **Blue LED**: ON when audio playback is active (`PLAYER_STATE_PLAYING`).
  - **Red LED**: ON when audio is paused or stopped (`PLAYER_STATE_PAUSED` / `PLAYER_STATE_STOPPED`).
  - **Green LED**: ON during the 5-second song confirmation window (`PLAYER_STATE_CONFIRMING`).
- **Hardware-Accelerated ST7789 LCD Driver (Phase 3 Completed)**:
  - 1.3-inch 240×240 color TFT driven via **STM32F407 FSMC Bank 3 (8080 8-bit parallel bus)**.
  - Hardware reset pulse generation on **`PD3`**.
  - Hardware backlight power activation on **`PF9`**.
  - High-performance bitmap font engine (`16×8` DejaVu Sans Mono ASCII characters).
  - Mutex-guarded display telemetry (`k_mutex g_lcd_mutex`).
- **Continuous Potentiometer Volume Control**: 10 k$\Omega$ linear potentiometer sampled via ADC and mapped to $0\%\text{--}100\%$ volume with live graphic progress bar.
- **3 Concurrent Cooperative Zephyr Threads**:
  1. `update_lcd_leds_thread`: Manages graphical display rendering and physical RGB state LEDs.
  2. `polling_buttons`: Handles debounced button reading, binary index decoding, and 5-second confirmation timing.
  3. `adjust_volume`: Reads ADC potentiometer and updates volume level.
- **Low-Power Idle Sleep**: The main thread puts the MCU into low-power idle sleep (`k_sleep(K_FOREVER)` / `k_cpu_idle()`), minimizing energy consumption.

---

## 2. Hardware Architecture & Pinout Netlist

The system targets the **RT-Thread Spark Development Board (STM32F407ZGT6)** with peripheral connections defined in `include/app_config.h`:

| Peripheral Module | Hardware Pin | Interface / Mode | Function / Description |
|---|---|---|---|
| **UART1 TX** | `PA9` | Alternate Function (AF7) | Serial telemetry / user instructions (115200 8N1) |
| **UART1 RX** | `PA10` | Alternate Function (AF7) | Serial console input from onboard ST-LINK VCP |
| **Button 1** (KEY0) | `PC0` | GPIO Input (Pull-Up) | Song selection latch & confirmation button |
| **Button 2** (KEY1) | `PC1` | GPIO Input (Pull-Up) | Song selection Bit 0 (LSB, weight $2^0 = 1$) |
| **Button 3** (KEY2) | `PC4` | GPIO Input (Pull-Up) | Song selection Bit 1 (weight $2^1 = 2$) |
| **Button 4** (WK_UP) | `PC5` | GPIO Input (Pull-Up) | Song selection Bit 2 (MSB, weight $2^2 = 4$) |
| **USER_BUTTON** | `PA0` | GPIO Input (Pull-Down/Up) | Play / Pause / Replay toggle |
| **Red LED** | `PF12` | GPIO Output (Active Low) | Status indicator: Paused / Stopped |
| **Blue LED** | `PF11` | GPIO Output (Active Low) | Status indicator: Playing |
| **Green LED** | `PE3` | GPIO Output (Active High) | Status indicator: 5-Second Confirmation Window |
| **LCD Backlight** | `PF9` | GPIO Output (High Speed) | Display backlight power control |
| **LCD Reset** | `PD3` | GPIO Output (Push-Pull) | ST7789 hardware reset line |
| **LCD Data Bus (D0–D7)** | `PD14..15, PD0..1, PE7..10` | Alternate Function 12 (`AF12_FSMC`) | FSMC 8080 8-bit parallel bidirectional data |
| **LCD Chip Select ($\overline{\text{NE3}}$)** | `PG10` | Alternate Function 12 (`AF12_FSMC`) | FSMC Bank 3 Chip Select |
| **LCD Command/Data ($\text{A18}$)** | `PD13` | Alternate Function 12 (`AF12_FSMC`) | Address bit 18 ($\text{LOW}=\text{CMD}$, $\text{HIGH}=\text{DATA}$) |
| **LCD Write Enable ($\overline{\text{NWE}}$)** | `PD5` | Alternate Function 12 (`AF12_FSMC`) | FSMC Write Strobe ($\overline{\text{WR}}$) |
| **LCD Read Enable ($\overline{\text{NOE}}$)** | `PD4` | Alternate Function 12 (`AF12_FSMC`) | FSMC Read Strobe ($\overline{\text{RD}}$) |
| **Potentiometer** | `PA1` | ADC1 Channel 1 (Analog) | 10 k$\Omega$ volume control voltage divider |
| **Headphone / Codec** | `PB10 (SCL), PB11 (SDA)` | I2C2 / I2S | ES8388 stereo codec & 3.5mm audio jack |

---

## 3. ST7789 FSMC Parallel Driver (Phase 3)

The ST7789 v3 controller communicates over an 8-bit 8080 parallel interface simulated by the STM32F407 Flexible Static Memory Controller (FSMC):
- **Base Memory Mapping**:
  - `0x6803FFFE`: Address Bit 18 is 0 $\to$ ST7789 Command Register (`LCD_CMD_ADDR`).
  - `0x68040000`: Address Bit 18 is 1 $\to$ ST7789 Data RAM Register (`LCD_DATA16_ADDR`).
- **Timing Profile (Mode A)**:
  - Address Setup Time: 15 HCLK (Read) / 3 HCLK (Write).
  - Data Setup Time: 60 HCLK (Read) / 3 HCLK (Write).
- **Startup Sequence**:
  1. Active-low reset pulse on `PD3` ($20\,\text{ms}$ low, $120\,\text{ms}$ high).
  2. Memory Data Access Control (`0x36`, orientation set to normal).
  3. Color format set to 16-bit RGB565 (`0x3A`, `0x55`).
  4. Porch setting (`0xB2`), Gate control (`0xB7`), VCOM (`0xBB`), LCM (`0xC0`).
  5. Gamma correction tables (`0xE0`, `0xE1`).
  6. Display Inversion ON (`0x21`).
  7. Sleep Out (`0x11`) followed by $120\,\text{ms}$ delay.
  8. Display ON (`0x29`).
  9. Backlight enabled on `PF9`.

---

## 4. Song Catalog & Binary Index Mapping

The 8 playable classical songs are indexed via Buttons 2–4 ($B_4 B_3 B_2$ in binary):

| Binary Code ($B_4 B_3 B_2$) | Song Index | Title (`name1`) | Composer / Subtitle (`name2`) | Tempo | Length |
|---|---|---|---|---|---|
| `000` | 0 | Für Elise - | Beethoven | 0.18 | 72 notes |
| `001` | 1 | Canon In D - | Pachelbel | 0.20 | 88 notes |
| `010` | 2 | Minuet in G | major - Bach | 0.25 | 90 notes |
| `011` | 3 | Turkish March - | Mozart | 0.15 | 176 notes |
| `100` | 4 | Nocturne in E | flat - Chopin | 0.22 | 116 notes |
| `101` | 5 | Waltz No. 2 - | Shostakovich | 0.22 | 135 notes |
| `110` | 6 | Nocturne in C | sharp - Chopin | 0.25 | 64 notes |
| `111` | 7 | Symphony No. 40 | - Mozart | 0.15 | 168 notes |

---

## 5. Software Architecture & Concurrency Model

```text
       +--------------------------------------------------------------+
       |               Zephyr RTOS Preemptive Kernel                  |
       +--------------------------------------------------------------+
                 |                           |                    |
        Priority 3 (Stack 2048)     Priority 2 (Stack 1024)   Priority 3 (Stack 1024)
                 v                           v                    v
      +---------------------+     +--------------------+   +-------------------+
      | update_lcd_leds_    |     |  polling_buttons   |   |   adjust_volume   |
      |       thread        |     |                    |   |                   |
      +---------------------+     +--------------------+   +-------------------+
                 |                           |                    |
                 | [k_mutex_lock]            | [State updates]    | [ADC Read]
                 v                           v                    v
      +---------------------+     +--------------------+   +-------------------+
      | ST7789 FSMC Display |     |  g_player context  |   | 10k Potentiometer |
      |  (Exclusive Access) |     |  (Shared State)    |   | (Volume 0-100%)   |
      +---------------------+     +--------------------+   +-------------------+
```

### Thread Responsibilities
1. **`update_lcd_leds_thread` (Priority 3, Stack 2048 B)**:
   - Evaluates current state: updates Red, Green, and Blue GPIO pins.
   - Acquires `g_lcd_mutex`, renders playback telemetry, volume progress bar, or confirmation dialog, and releases mutex.
   - Cooperatively sleeps for 100 ms (`k_sleep(K_MSEC(100))`).
2. **`polling_buttons` (Priority 2, Stack 1024 B)**:
   - Polls GPIO buttons with 20 ms debounce filtering.
   - Decodes binary song index and latches prospective choice upon Button 1 press.
   - Monitors 5-second confirmation countdown via `k_uptime_get_32()`.
   - Handles `USER_BUTTON` Play/Pause toggle.
   - Cooperatively sleeps for 20 ms (`k_sleep(K_MSEC(20))`).
3. **`adjust_volume` (Priority 3, Stack 1024 B)**:
   - Periodically samples ADC Channel 1 (`PA1`).
   - Normalizes raw ADC reading into a calibrated $0\%\text{--}100\%$ volume scale.
   - Cooperatively sleeps for 100 ms (`k_sleep(K_MSEC(100))`).

---

## 6. Build, Verification & Toolchain

The firmware builds cleanly under PlatformIO with Zephyr RTOS:

```bash
# Compile firmware
pio run -d LAB_2_ZephyrRTOS_Personal_MP3_Player

# Terminal Output:
# RAM:   [=         ]   9.7% (used 12689 bytes from 131072 bytes)
# Flash: [=         ]   5.6% (used 59148 bytes from 1048576 bytes)
# [SUCCESS] Took 5.98 seconds
```

---

## 7. Repository Layout & File Navigation

```text
LAB_2_ZephyrRTOS_Personal_MP3_Player/
├── .gitignore                      # Exclusions for .pio, .vscode, binaries
├── platformio.ini                  # PlatformIO configuration for black_f407zg
├── zephyr/
│   ├── CMakeLists.txt              # Application CMake target configuration
│   └── prj.conf                    # Zephyr kernel subsystem enablement (GPIO, UART, PM, C++)
├── include/
│   ├── app_config.h                # Hardware pinouts, timings, and player constants
│   ├── lcd_font.h                  # 16x8 DejaVu Sans Mono ASCII font table
│   ├── lcd_st7789.h                # ST7789 FSMC graphics and drawing API
│   ├── player_logic.h              # Pure decision logic and state definitions
│   └── threads.h                   # Thread prototypes, stacks, and mutex declarations
├── src/
│   ├── lcd_st7789.c                # Hardware FSMC 8080 driver, reset & backlight
│   ├── main.c                      # Application startup, UART guide, thread creation
│   ├── player_logic.c              # Binary decoding, 5s timeout, and volume normalization
│   └── threads.c                   # 3 cooperative Zephyr RTOS threads
├── reference/                      # Course-provided reference headers and sources
│   ├── song.h
│   ├── song_def.h
│   ├── NHD_0216HZ.h
│   └── NHD_0216HZ.cpp
├── docs/
│   └── Laboratory Activity 2.md    # Course assignment specification
└── README.md                       # This comprehensive project documentation
```
