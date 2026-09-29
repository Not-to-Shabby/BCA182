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
- **Mutex-Protected LCD Telemetry**: Exclusive thread access to the 240×240 ST7789 LCD display guarded by `k_mutex g_lcd_mutex`.
- **Continuous Potentiometer Volume Control**: 10 k$\Omega$ linear potentiometer sampled via ADC and mapped to $0\%\text{--}100\%$ volume.
- **3 Concurrent Cooperative Zephyr Threads**:
  1. `update_lcd_leds_thread`: Manages display updates and RGB state LEDs.
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
| **Red LED** | `PF12` | GPIO Output (Active Low/High) | Status indicator: Paused / Stopped |
| **Blue LED** | `PF11` | GPIO Output (Active Low/High) | Status indicator: Playing |
| **Green LED** | `GPIOE_3` | GPIO Output | Status indicator: 5-Second Confirmation Window |
| **LCD ST7789 v3** | `PD14..15, PD0..1, PE7..10` | FSMC 8080 8-bit Parallel | 240×240 Color TFT screen data bus |
| **LCD CS / RS / WR / RD** | `PG10, PD13, PD5, PD4` | FSMC Control Lines | Chip select, register select, write/read enables |
| **Potentiometer** | `PA1` | ADC1 Channel 1 (Analog) | 10 k$\Omega$ volume control voltage divider |
| **Headphone / Codec** | `PB10 (SCL), PB11 (SDA)` | I2C2 / I2S | ES8388 stereo codec & 3.5mm audio jack |

---

## 3. Song Catalog & Binary Index Mapping

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

## 4. Software Architecture & Concurrency Model

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
      |  ST7789 LCD & LEDs  |     |  g_player context  |   | 10k Potentiometer |
      |  (Exclusive Access) |     |  (Shared State)    |   | (Volume 0-100%)   |
      +---------------------+     +--------------------+   +-------------------+
```

### Thread Responsibilities
1. **`update_lcd_leds_thread` (Priority 3, Stack 2048 B)**:
   - Evaluates current state: updates Red, Green, and Blue GPIO pins.
   - Acquires `g_lcd_mutex`, renders playback telemetry or confirmation prompt, and releases mutex.
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

## 5. Build, Verification & Toolchain

The firmware builds cleanly under PlatformIO with Zephyr RTOS:

```bash
# Compile firmware
pio run -d LAB_2_ZephyrRTOS_Personal_MP3_Player

# Terminal Output:
# RAM:   [=         ]   8.1% (used 10601 bytes from 131072 bytes)
# Flash: [          ]   2.6% (used 27592 bytes from 1048576 bytes)
# [SUCCESS] Took 24.76 seconds
```

---

## 6. Repository Layout & File Navigation

```text
LAB_2_ZephyrRTOS_Personal_MP3_Player/
├── .gitignore                      # Exclusions for .pio, .vscode, binaries
├── platformio.ini                  # PlatformIO configuration for black_f407zg
├── zephyr/
│   ├── CMakeLists.txt              # Application CMake target configuration
│   └── prj.conf                    # Zephyr kernel subsystem enablement (GPIO, UART, PM, C++)
├── include/
│   ├── app_config.h                # Hardware pinouts, timings, and player constants
│   ├── player_logic.h              # Pure decision logic and state definitions
│   └── threads.h                   # Thread prototypes, stacks, and mutex declarations
├── src/
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
