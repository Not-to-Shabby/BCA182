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
- **Directional D-Pad Navigation Controls with Unified Long-Press Architecture**:
  - **Zero Race Condition**: Every button utilizes an explicit release-versus-threshold state machine guaranteeing that holding a button never triggers an accidental short click on press or release.
  - **UP Button (`PC5` / SW2)**:
    - *Short Click*: Cycles forward to the next track ($+1$, Track 1 to 8).
    - *Long Hold ($\ge 450\,\text{ms}$)*: Instantly resets and jumps back to Track 1 (*Für Elise*).
  - **DOWN Button (`PC1` / SW4)**:
    - *Short Click*: Cycles backward to the previous track ($-1$, Track 8 to 1).
    - *Long Hold ($\ge 450\,\text{ms}$)*: Toggles **Play / Pause** on the currently selected track without changing tracks!
  - **UP + DOWN Simultaneous Hold ($\ge 450\,\text{ms}$)**:
    - **Waveform / Instrument Timbre Cycle**: Switches the active synthesis waveform across **SINE** $\to$ **TRIANGLE** $\to$ **SAWTOOTH** $\to$ **SQUARE**. Track scrolling is automatically suppressed while both buttons are held.
  - **LEFT Button (`PC0` / SW3 - Volume Down)**:
    - *Short Click*: Decreases volume by $5\%$ per step.
    - *Long Hold / Repeat*: Rapidly and smoothly decreases volume down to $0\%$ (Mute) at 90 ms intervals.
  - **RIGHT Button (`PC4` / SW5 - Volume Up)**:
    - *Short Click*: Increases volume by $5\%$ per step.
    - *Long Hold / Repeat*: Rapidly and smoothly increases volume up to $100\%$ at 90 ms intervals.
  - **LEFT + RIGHT Simultaneous Hold ($\ge 450\,\text{ms}$)**:
    - **Buzzer Mute Toggle (Headphone Mode)**: Silences the on-board buzzer (`PB0`/`PB1`) completely so that only the 3.5mm headphone jack (`CN3`) emits sound. Pressing and holding both again re-enables dual buzzer/headphone playback.
    - Zero volume jumping: individual volume adjustments are automatically suppressed while both buttons are held.
  - **PRESS / USER_BUTTON (`PA0`)**:
    - *Short Click*: Toggles Play (`PLAYER_STATE_PLAYING`) and Pause (`PLAYER_STATE_PAUSED`).
    - *Long Hold ($\ge 500\,\text{ms}$)*: Fully stops playback (`PLAYER_STATE_STOPPED`).
  - **AUX Button (`PA1`)**:
    - *Short Click*: Cycles synthesis waveform (Sine, Triangle, Sawtooth, Square).
    - *Long Hold ($\ge 450\,\text{ms}$)*: Instant Mute / Unmute volume toggle.
- **Selectable Multi-Waveform Synthesis Engine**:
  - **Sine Wave**: Ultra-pure harmonic fundamental, ideal for mellow classical music listening with zero harsh overtones.
  - **Triangle Wave**: Soft, flute-like timbre with gentle odd harmonics rolling off at $1/n^2$.
  - **Sawtooth Wave**: Bright, rich, brass/string-like timbre with full harmonic spectrum.
  - **Square Wave**: Authentic retro 8-bit chiptune sound with strong odd harmonics.
  - Waveform is rendered live on the ST7789 display card with color-coded badges (`SINE` in Cyan, `TRIANGLE` in Yellow, `SAWTOOTH` in Orange, `SQUARE` in Magenta).
- **Audio Synthesizer & Note Playback Engine (Phase 5 Completed)**:
  - Generates authentic musical pitches via **hardware timer TIM3 Channel 3 (`PB0`) PWM**.
  - Direct microsecond period tuning ($f = 1000.0 / T\text{ Hz}$) covering notes $G_3$ to $D_6$.
  - Drift-free note scheduling using Zephyr's **`struct k_timer`** ticker.
  - Distinct musical note articulation: $85\%$ note tone phase followed by a $15\%$ staccato articulation gap between consecutive notes.
  - Real-time PWM volume duty-cycle scaling ($0\%\text{--}100\%$) and live note progression tracking (`NOTE: X / Y`) on the LCD.
- **Physical RGB LED State Indicators**:
  - **Blue LED (`PF11`)**: ON when audio playback is active (`PLAYER_STATE_PLAYING`).
  - **Red LED (`PF12`)**: ON when audio is paused or stopped (`PLAYER_STATE_PAUSED` / `PLAYER_STATE_STOPPED`).
- **Hardware-Accelerated ST7789 LCD Driver**:
  - 1.3-inch 240×240 color TFT driven via **STM32F407 FSMC Bank 3 (8080 8-bit parallel bus)**.
  - Hardware reset pulse generation on **`PD3`**.
  - Hardware backlight power activation on **`PF9`**.
  - High-performance bitmap font engine (`16×8` DejaVu Sans Mono ASCII characters).
  - Mutex-guarded display telemetry (`k_mutex g_lcd_mutex`).
  - Real-time graphical volume progress bar and track title rendering.
  - Footer navigation hints: `UP/DN: Track | L/R: Vol` and `PRESS/PA0: Play/Pause`.
- **3 Concurrent Cooperative Zephyr Threads**:
  1. `update_lcd_leds_thread`: Manages graphical display rendering and physical RGB state LEDs.
  2. `polling_buttons`: Handles debounced button reading, UP/DOWN track cycling, and Play/Pause toggling.
  3. `adjust_volume`: Dedicated thread monitoring LEFT/RIGHT volume buttons and updating shared volume state.
- **Low-Power Idle Sleep**: The main thread puts the MCU into low-power idle sleep (`k_sleep(K_FOREVER)` / `k_cpu_idle()`), minimizing energy consumption.

---

## 2. Hardware Architecture & Pinout Netlist

The system targets the **RT-Thread Spark Development Board (STM32F407ZGT6)** with peripheral connections defined in `include/app_config.h`:

| Peripheral Module | Hardware Pin | Interface / Mode | Function / Description |
|---|---|---|---|
| **UART1 TX** | `PA9` | Alternate Function (AF7) | Serial telemetry / user instructions (115200 8N1) |
| **UART1 RX** | `PA10` | Alternate Function (AF7) | Serial console input from onboard ST-LINK VCP |
| **UP Button (SW2)** | `PC5` | GPIO Input (Pull-Up) | Next Track scroll ($+1$, Track 1 to 8) |
| **DOWN Button (SW4)** | `PC1` | GPIO Input (Pull-Up) | Previous Track scroll ($-1$) / Long-press: Play/Pause |
| **LEFT Button (SW3)** | `PC0` | GPIO Input (Pull-Up) | Volume Down (-5% per step down to 0%) |
| **RIGHT Button (SW5)** | `PC4` | GPIO Input (Pull-Up) | Volume Up (+5% per step up to 100%) |
| **PRESS / USER_BUTTON** | `PA0` | GPIO Input (Pull-Down) | Play / Pause Toggle (Active High) |
| **Auxiliary Button** | `PA1` | GPIO Input (Pull-Up) | Optional 5th button: Cycles volume presets (25%, 50%, 75%, 100%, 0%) |
| **Red LED** | `PF12` | GPIO Output (Active Low) | Status indicator: Paused / Stopped |
| **Blue LED** | `PF11` | GPIO Output (Active Low) | Status indicator: Playing |
| **LCD Backlight** | `PF9` | GPIO Output (High Speed) | Display backlight power control |
| **LCD Reset** | `PD3` | GPIO Output (Push-Pull) | ST7789 hardware reset line |
| **LCD Data Bus (D0–D7)** | `PD14..15, PD0..1, PE7..10` | Alternate Function 12 (`AF12_FSMC`) | FSMC 8080 8-bit parallel bidirectional data |
| **LCD Chip Select ($\overline{\text{NE3}}$)** | `PG10` | Alternate Function 12 (`AF12_FSMC`) | FSMC Bank 3 Chip Select |
| **LCD Command/Data ($\text{A18}$)** | `PD13` | Alternate Function 12 (`AF12_FSMC`) | Address bit 18 ($\text{LOW}=\text{CMD}$, $\text{HIGH}=\text{DATA}$) |
| **LCD Write Enable ($\overline{\text{NWE}}$)** | `PD5` | Alternate Function 12 (`AF12_FSMC`) | FSMC Write Strobe ($\overline{\text{WR}}$) |
| **LCD Read Enable ($\overline{\text{NOE}}$)** | `PD4` | Alternate Function 12 (`AF12_FSMC`) | FSMC Read Strobe ($\overline{\text{RD}}$) |
| **3.5mm Headphone Jack (`CN3`)** | `PB10 (SCL), PB11 (SDA)` + I2S3 | ES8388 Stereo Codec (`U11`) + Headphone Amp (`LOUT1`/`ROUT1`) |
| **Analog Audio DAC1 Output** | `PA4` | Analog Mode (`DAC_OUT1`) | Direct 12-bit analog sinusoidal audio output for external speaker/earphones |
| **Onboard Buzzer Audio** | `PB0` | Alternate Function 2 (`TIM3_CH3`) | Hardware PWM audio synthesis on onboard buzzer (`BUZ1`) |
| **Expansion Speaker Audio** | `PB1` | Alternate Function 2 (`TIM3_CH4`) | Simultaneous PWM audio output on expansion header |

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

## 4. Multi-Output Audio Synthesizer Architecture (Phase 5)

The audio playback subsystem supports multiple simultaneous acoustic output channels to accommodate both external speakers, headphones, and on-board hardware:

### A. 3.5mm Stereo Headphone Jack (`CN3` / PJ-320A) via ES8388 Codec
- **Hardware Integration**: Driven by the on-board **Everest Semiconductor ES8388 Low-Power Stereo Audio Codec (`U11`)**.
- **Control Interface**: Configured via **I2C2** on pins `PB10` (SCL) and `PB11` (SDA) at 7-bit address `0x10`. The driver powers up the internal DAC, sets digital attenuation, enables the dual headphone operational amplifiers on `LOUT1` and `ROUT1`, and disables muting.
- **Audio Stream**: Streamed via **I2S3** (`PC7`: Master Clock, `PA15`: Word Select, `PB3`: Bit Clock, `PB5`: Serial Data) with PCM samples synthesizing pure harmonic frequencies.
- **Usage**: Plug any standard 3.5mm stereo headphones, earphones, or desktop powered speakers directly into the board's 3.5mm jack (`CN3`).

### B. Direct 12-Bit Analog Audio DAC on Pin `PA4`
- **Hardware Integration**: Utilizes the STM32F407's internal high-speed **12-bit Analog-to-Digital Converter Channel 1 (`DAC_OUT1` on `PA4`)**.
- **Direct Digital Synthesis (DDS)**: Driven by hardware timer **TIM4** modulating a 32-point calibrated sinusoidal lookup table ($32 \times f_{\text{note}}$) with sub-microsecond precision.
- **Audio Quality**: Produces smooth analog sine waves with zero high-frequency square-wave aliasing.
- **Usage**: Connect the positive terminal of an external speaker, earphone jack, or amplifier input to `PA4` (`PMOD_PA4` on the expansion header) and the ground terminal to `GND`.

### C. Hardware Timer TIM3 PWM Audio (PB0 On-Board & PB1 Expansion)
- **Clock Configuration**: APB1 timer clock at $84\text{ MHz}$, prescaled by $83$ to produce a $1\text{ MHz}$ timer resolution ($1\ \mu\text{s}$ per count) with immediate prescaler reload (`TIM_EGR_UG`).
- **Pitch Frequency Calculation**: Note period $T$ from `reference/song_def.h` is converted into timer auto-reload value:
  $$\text{ARR} = (T_{\text{ms}} \times 1000) - 1$$
- **Volume & Amplitude Scaling**: Volume duty cycle is mapped into `CCR3` and `CCR4`:
  $$\text{CCR} = \frac{\text{ARR} \times \text{volume\_percent}}{200}$$
- **Zephyr `k_timer` Note Ticker**:
  - Automatically calculates total duration per beat:
    $$\text{duration\_ms} = \text{beat} \times \text{tempo} \times 14000\text{ ms}$$
  - **Two-Phase Note Articulation**:
    - **Tone Phase ($80\%$)**: Active tone generation at target pitch.
    - **Gap Phase ($20\%$)**: Brief silence between consecutive notes for crisp note articulation.

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
                 | [k_mutex_lock]            | [State updates]    | [Vol updates]
                 v                           v                    v
      +---------------------+     +--------------------+   +-------------------+
      | ST7789 FSMC Display |     |  g_player context  |   | Hardware TIM3_CH3 |
      |  (Exclusive Access) |     |  (Shared State)    |   | PWM Audio on PB0  |
      +---------------------+     +--------------------+   +-------------------+
```

### Thread Responsibilities
1. **`update_lcd_leds_thread` (Priority 3, Stack 2048 B)**:
   - Evaluates current state: updates Red and Blue GPIO pins.
   - Synchronizes audio engine state (starts, pauses, resumes, or stops `audio_engine`).
   - Acquires `g_lcd_mutex`, renders playback telemetry, volume progress bar, track title, and active note progress (`NOTE: X / Y`), then releases mutex.
   - Cooperatively sleeps for 100 ms (`k_sleep(K_MSEC(100))`).
2. **`polling_buttons` (Priority 2, Stack 1024 B)**:
   - Polls UP (`PC5`), DOWN (`PC1`), and USER_BUTTON (`PA0`) with non-racing release-vs-hold debouncing.
   - Scrolls tracks forward (UP) and backward (DOWN).
   - Toggles Play / Pause on PRESS (`PA0`) or DOWN long-press without race condition.
   - Cooperatively sleeps for 20 ms (`k_sleep(K_MSEC(20))`).
3. **`adjust_volume` (Priority 3, Stack 1024 B)**:
   - Dedicated volume adjustment thread reading LEFT (`PC0` / SW3) and RIGHT (`PC4` / SW5).
   - Smoothly steps volume ($5\%$ per step) and supports auto-repeat while held.
   - Cooperatively sleeps for 100 ms (`k_sleep(K_MSEC(100))`).

---

## 6. Build, Verification & Toolchain

The firmware builds cleanly under PlatformIO with Zephyr RTOS:

```bash
# Compile firmware
pio run -d LAB_2_ZephyrRTOS_Personal_MP3_Player

# Terminal Output:
# RAM:   [=         ]   9.8% (used 12823 bytes from 131072 bytes)
# Flash: [=         ]   6.8% (used 71640 bytes from 1048576 bytes)
# [SUCCESS] Took 27.08 seconds
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
│   ├── audio_engine.h              # Audio engine API and musical piece structures
│   ├── lcd_font.h                  # 16x8 DejaVu Sans Mono ASCII font table
│   ├── lcd_st7789.h                # ST7789 FSMC graphics and drawing API
│   ├── player_logic.h              # Pure decision logic and state definitions
│   └── threads.h                   # Thread prototypes, stacks, and mutex declarations
├── src/
│   ├── audio_engine.c              # TIM3_CH3 PWM buzzer driver, k_timer note ticker, song data
│   ├── lcd_st7789.c                # Hardware FSMC 8080 driver, reset & backlight
│   ├── main.c                      # Application startup, UART guide, thread creation
│   ├── player_logic.c              # Binary decoding, 5s timeout, and volume normalization
│   ├── song_data.inc               # Note and beat arrays for 8 classical compositions
│   └── threads.c                   # 3 cooperative Zephyr RTOS threads
├── reference/                      # Course-provided reference headers and sources
│   ├── song.h
│   ├── song_def.h
│   ├── NHD_0216HZ.h
│   └── NHD_0216HZ.cpp
├── docs/
│   ├── Laboratory Activity 2.md    # Course assignment specification
│   └── schematic.pdf               # RT-Thread Spark Board V1.0 schematic diagram
└── README.md                       # This comprehensive project documentation
```

# Terminal Output:
# RAM:   [=         ]   9.7% (used 12689 bytes from 131072 bytes)
# Flash: [=         ]   5.7% (used 60280 bytes from 1048576 bytes)
# [SUCCESS] Took 3.87 seconds
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
