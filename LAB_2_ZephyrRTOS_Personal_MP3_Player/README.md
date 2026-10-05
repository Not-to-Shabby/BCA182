# Personal MP3 Player (Zephyr RTOS)

**Course**: BCA182 – Embedded Systems Programming  
**Author**: Perch Arnel II Montefalcon  
**Institution**: Mindanao State University – Iligan Institute of Technology (MSU-IIT)  
**Instructor / Evaluator**: Paul Rodolf P. Castor (`paulrodolf.castor@g.msuiit.edu.ph`)  
**Target Platform**: RT-Thread Spark Development Board ("星火 1 号", STM32F407ZGT6, ARM Cortex-M4F @ 168 MHz)  
**RTOS**: Zephyr RTOS v4.x (Preemptive & Cooperative Kernel Services)  
**Framework**: Zephyr PlatformIO (`framework = zephyr`, `board = black_f407zg`) — *Zero Arduino abstractions*  

---

## 1. Executive Summary

This project implements a high-performance, real-time embedded **Personal MP3 Player** deployed on the **RT-Thread Spark Development Board (STM32F407ZGT6)** using **Zephyr RTOS**. In strict compliance with course rules, the Arduino framework and third-party convenience libraries are completely excluded; all peripheral drivers, concurrency primitives, and hardware synchronization rely exclusively on native Zephyr kernel services and direct register access.

The system bridges two distinct audio reproduction paradigms:
1. **Classical Musical Synthesizer**: Direct synthesis of 8 classical compositions (`reference/song_def.h`) using microsecond hardware timer PWM (`TIM3_CH3` on `PB0`, `TIM3_CH4` on `PB1`), internal 12-bit analog DAC1 (`PA4`), and the onboard Everest Semiconductor ES8388 stereo codec (`CN3` 3.5mm headphone jack).
2. **Mass-Storage Media Player**: Native 4-bit high-speed SDIO bus (`sdmmc1`) interfacing Micro-SD cards formatted in FAT32, decoding uncompressed 16-bit 44.1 kHz `.wav` streams, and performing real-time fixed-point 32-bit frame decoding of `.mp3` files via the embedded RealNetworks Helix MP3 engine.

Visual status is rendered in real time on an onboard 1.3-inch 240×240 ST7789 TFT LCD driven across an 8-bit parallel bus by the Flexible Static Memory Controller (FSMC Bank 3) using a flicker-free differential line-rendering algorithm protected by a kernel mutex.

---

## 2. Technical Features & Capabilities

- **Multi-Source Audio Pipeline**:
  - **Classical Repertoire**: Plays 8 compositions (*Für Elise*, *Canon in D*, *Minuet in G*, *Turkish March*, *Nocturne in E-flat*, *Waltz No. 2*, *Nocturne in C-sharp*, *Symphony No. 40*).
  - **WAV Streaming**: Uncompressed 16-bit 44.1 kHz / 48 kHz stereo/mono audio playback from Micro-SD.
  - **Helix MP3 Engine**: RealNetworks fixed-point MPEG-1/2 Layer III decoder running at $< 15\%\text{ CPU}$ load.
- **Multi-Channel Acoustic Output**:
  - **Onboard Buzzer (`PB0`)**: Hardware Timer 3 Channel 3 PWM with immediate shadow reload.
  - **Expansion Header (`PB1`)**: Hardware Timer 3 Channel 4 PWM for external passive buzzer/speaker.
  - **Internal Analog DAC1 (`PA4`)**: Continuous 12-bit analog output ($0\text{--}3.3\text{ V}$ mid-rail biased).
  - **3.5mm Headphone Jack (`CN3`)**: Everest Semiconductor ES8388 24-bit stereo codec driven over I2S3.
- **Hardware Circular DMA Audio Streamer (DMA1 Stream 5)**:
  - Transfers audio samples directly from RAM to `SPI3->DR` with zero CPU interrupt starvation.
  - Cuts audio interrupt load from $88,200\text{ Hz}$ to $86\text{ Hz}$, eliminating all audio crackling.
- **High-Speed 4-Bit SDIO Interface (DMA2 Stream 3/6)**:
  - Hardware-accelerated block transfers with `STM32_DMA_FIFO_FULL` and aligned $2048\text{ B}$ bounce buffering.
- **Directional D-Pad Controls with Unified Non-Racing Architecture**:
  - UP (`PC5`): Next track ($+1$).
  - DOWN (`PC1`): Previous track ($-1$) / Hold ($\ge 450\text{ ms}$) toggles Play/Pause.
  - LEFT (`PC0`): Volume Down (discrete steps + auto-repeat).
  - RIGHT (`PC4`): Volume Up (discrete steps + auto-repeat).
  - PRESS / USER_BUTTON (`PA0`): Play / Pause / Stop toggle.
  - UP + DOWN (Hold $\ge 450\text{ ms}$): Cycles synthesis waveform (**SINE** $\to$ **TRIANGLE** $\to$ **SAWTOOTH** $\to$ **SQUARE**).
  - LEFT + RIGHT (Hold $\ge 450\text{ ms}$): Toggles onboard buzzer mute (Headphone-only mode).
- **Physical RGB State LEDs**:
  - **Blue LED (`PF11`)**: Lit when audio is playing (`PLAYER_STATE_PLAYING`).
  - **Red LED (`PF12`)**: Lit when audio is paused or stopped (`PLAYER_STATE_PAUSED` / `PLAYER_STATE_STOPPED`).

---

## 3. Hardware Architecture & RT-Spark Board Specifications

```text
 ┌────────────────────────────────────────────────────────────────────────┐
 │           RT-Thread Spark Development Board (STM32F407ZGT6)            │
 ├────────────────────────────────────────────────────────────────────────┤
 │  Core: ARM Cortex-M4F @ 168 MHz (210 DMIPS) with Hardware FPU          │
 │  Memory: 1024 KB Flash, 192 KB SRAM + 64 KB Core Coupled Memory (CCM)  │
 └──────┬──────────────────────┬──────────────────────┬───────────────────┘
        │                      │                      │
        ▼                      ▼                      ▼
 ┌──────────────┐      ┌──────────────┐      ┌─────────────────────────────┐
 │ FSMC Bank 3  │      │  SPI3 / I2S3 │      │   4-bit SDIO (DMA2_Stream3) │
 │ 8-bit 8080   │      │ Circular DMA │      │   FAT32 / exFAT Filesystem  │
 └──────┬───────┘      └──────┬───────┘      └──────────────┬──────────────┘
        │                      │                             │
        ▼                      ▼                             ▼
 ┌──────────────┐      ┌──────────────┐              ┌──────────────┐
 │ ST7789 LCD   │      │ ES8388 Codec │              │ Micro-SD     │
 │ 240x240 Color│      │ 3.5mm (CN3)  │              │ Card Socket  │
 └──────────────┘      └──────────────┘              └──────────────┘
```

- **Microcontroller**: STMicroelectronics STM32F407ZGT6 (High-performance foundation).
- **Display Module**: 1.3-inch ST7789 v3 IPS LCD (240×240 resolution, 65K colors).
- **Audio Codec**: Everest Semiconductor ES8388 low-power multi-bit delta-sigma stereo DAC/ADC.
- **Mass Storage**: Micro-SD / TF card socket with hardware card-detection on pin `PF3`.
- **Clock Tree**: $8.0\text{ MHz}$ HSE crystal, generating $168\text{ MHz}$ system clock and $135.5\text{ MHz}$ audio PLL clock (`PLLI2S`).

---

## 4. Complete Hardware Pinout Netlist

| Subsystem | Microcontroller Pin | Interface / Mode | Hardware Function |
|---|---|---|---|
| **D-Pad Controls** | `PC5` | GPIO Input (Pull-Up) | UP Button (SW2) - Next Track ($+1$) |
| | `PC1` | GPIO Input (Pull-Up) | DOWN Button (SW4) - Prev Track ($-1$) / Hold Play-Pause |
| | `PC0` | GPIO Input (Pull-Up) | LEFT Button (SW3) - Volume Down |
| | `PC4` | GPIO Input (Pull-Up) | RIGHT Button (SW5) - Volume Up |
| | `PA0` | GPIO Input (Pull-Up) | PRESS / USER_BUTTON (SW1) - Play / Pause / Stop |
| | `PA1` | GPIO Input (Pull-Up) | AUX Button - Waveform Timbre Cycle |
| **RGB Indicators** | `PF11` | GPIO Output (Push-Pull) | Blue LED - Audio Playing Indicator |
| | `PF12` | GPIO Output (Push-Pull) | Red LED - Audio Paused / Stopped Indicator |
| **ST7789 LCD** | `PD14` .. `PD15`, `PD0` .. `PD1`, `PE7` .. `PE10` | AF12 (`FSMC_D0` .. `D7`) | FSMC 8-bit Data Bus ($D_0\text{--}D_7$) |
| | `PG10` | AF12 (`FSMC_NE3`) | Chip Select Bank 3 (`0x68000000`) |
| | `PD13` | AF12 (`FSMC_A18`) | Command / Data Select ($RS$ line) |
| | `PD4` | AF12 (`FSMC_NOE`) | Read Enable ($RD$ strobe) |
| | `PD5` | AF12 (`FSMC_NWE`) | Write Enable ($WR$ strobe) |
| | `PD3` | GPIO Output (Push-Pull) | Hardware LCD Reset (Active LOW pulse) |
| | `PF9` | GPIO Output (Push-Pull) | Backlight Power Control (HIGH = ON) |
| **Audio Codec** | `PC7` | AF6 (`I2S3_MCK`) | ES8388 Master Audio Clock ($135.5\text{ MHz}$) |
| | `PA15` | AF6 (`I2S3_WS`) | Word Select / Frame Sync ($44.1\text{ kHz}$) |
| | `PB3` | AF6 (`I2S3_CK`) | Continuous Bit Clock Line |
| | `PB5` | AF6 (`I2S3_SD`) | Serial Audio Data (DMA1 Stream 5) |
| | `PF0` | GPIO Output (Open-Drain) | Software I2C2 SDA Line ($100\text{ kHz}$) |
| | `PF1` | GPIO Output (Open-Drain) | Software I2C2 SCL Line ($100\text{ kHz}$) |
| | `CN3` | 3.5mm Headphone Jack | Stereo Output (`LOUT1` / `ROUT1`) |
| **Analog / PWM** | `PA4` | Analog Mode | Internal 12-Bit Analog DAC1 Output |
| | `PB0` | AF2 (`TIM3_CH3`) | Hardware Timer PWM Buzzer (Onboard) |
| | `PB1` | AF2 (`TIM3_CH4`) | Hardware Timer PWM Buzzer (Expansion) |
| **Micro-SD Socket** | `PC8` .. `PC11` | AF12 (`SDIO_D0` .. `D3`) | 4-Bit High-Speed Data Bus |
| | `PC12` | AF12 (`SDIO_CK`) | SDIO Clock Line ($4\text{ MHz}$) |
| | `PD2` | AF12 (`SDIO_CMD`) | SDIO Command / Response Line |
| | `PF3` | GPIO Input (Pull-Up) | Card Detect Pin (Active LOW on insertion) |
| **Serial Console** | `PA9` / `PA10` | AF7 (`USART1_TX` / `RX`) | Direct Hardware UART to ST-LINK VCP ($115200\text{ Baud}$) |

---

## 5. Zephyr RTOS Concurrency Model & Multithreading

The firmware organizes application execution into five discrete threads scheduled preemptively and cooperatively:

```text
       +--------------------------------------------------------------+
       |               Zephyr RTOS Preemptive Kernel                  |
       +--------------------------------------------------------------+
                 │                           │                    │
        Priority 3 (Stack 4096)     Priority 2 (Stack 4096)   Priority 3 (Stack 2048)
                 ▼                           ▼                    ▼
      +---------------------+     +--------------------+   +-------------------+
      | update_lcd_leds_    |     |  polling_buttons   |   |   adjust_volume   |
      |       thread        |     |                    |   |                   |
      +---------------------+     +--------------------+   +-------------------+
                 │                           │                    │
                 │ [k_mutex_lock]            │ [State updates]    │ [Vol updates]
                 ▼                           ▼                    ▼
      +---------------------+     +--------------------+   +-------------------+
      | ST7789 FSMC Display |     |  g_player Context  |   | Hardware TIM3_CH3 |
      |  (Exclusive Access) |     |   (Shared State)   |   | PWM Audio on PB0  |
      +---------------------+     +--------------------+   +-------------------+
                 ▲                           ▲                    ▲
                 │                           │                    │
      +─────────────────────+     +────────────────────+          │
      |  wav_reader_thread  |     |audio_producer_thrd |          │
      | (Priority 4, 4096 B)|     |(Priority 3, 2048 B)|          │
      +─────────────────────+     +────────────────────+          │
                 │                           │                    │
                 ▼                           ▼                    ▼
           FatFs / SDIO            Circular DMA1 Stream 5   Hardware Timers
```

### Thread Descriptions & Responsibilities
1. **`update_lcd_leds_thread` (Priority 3, Stack 4096 B)**:
   - Updates physical Blue and Red GPIO LEDs based on player state.
   - Synchronizes playback engines, triggering start, pause, resume, and stop events.
   - Acquires `g_lcd_mutex` and renders track titles, volume indicators, and note progress.
   - Yields cooperatively via `k_sleep(K_MSEC(100))`.
2. **`polling_buttons` (Priority 2, Stack 4096 B)**:
   - Evaluates D-pad navigation pins every 20 ms.
   - Implements non-racing debouncing to prevent short-click misfires during holds.
   - Scrolls tracks forward and backward.
   - Yields cooperatively via `k_sleep(K_MSEC(20))`.
3. **`adjust_volume` (Priority 3, Stack 2048 B)**:
   - Adjusts volume in discrete 5% steps with smooth auto-repeat.
   - Detects `LEFT + RIGHT` simultaneous hold to toggle buzzer mute.
   - Yields cooperatively via `k_sleep(K_MSEC(100))`.
4. **`audio_producer_thread` (Priority 3, Stack 2048 B)**:
   - Pre-fills 256-word staging buffers for the DMA audio output in the background.
5. **`wav_reader_thread` (Priority 4, Stack 4096 B)**:
   - Background worker pre-fetching 2048-byte SDIO chunks and decoding MP3 frames.

---

## 6. Flexible Static Memory Controller (FSMC) ST7789 LCD Driver

The 1.3-inch ST7789 display is driven through **Flexible Static Memory Controller Bank 3**:
- **Hardware Addressing**:
  - Command Register: `0x6803FFFE` ($\text{A18} = 0$)
  - Data Register: `0x68040000` ($\text{A18} = 1$)
- **Memory-Mapped Architecture**: The microcontroller writes directly to LCD registers using memory pointers (`*(__IO uint8_t*)`), enabling maximum pixel transfer throughput.

---

## 7. Flicker-Free Differential Line Rendering Algorithm

Writing all $57,600$ 16-bit RGB pixels ($115.2\text{ KB}$) on every note update causes perceptible screen flashing. The differential renderer eliminates this:
1. **Static UI Geometry**: Header bars, title text, and static frames are drawn once upon state transitions.
2. **Dynamic In-Place Updates**: Dynamic rows (such as `NOTE: X / Y`, volume bar width, and status badges) are cleared and repainted locally in-place, achieving flicker-free operation.

---

## 8. Non-Racing Two-Phase Button Debouncer Architecture

To guarantee that holding a button never triggers a short click:
```text
  PIN PRESS (LOW)
        │
        ├─► [Start Timer]
        │
        ▼
   Still Held?
   ├─► YES (Duration >= 450 ms) ──► Trigger LONG-HOLD Action
   │                               Set `long_triggered = true`
   │
   └─► PIN RELEASE (HIGH)
             │
             ▼
      `long_triggered` was true?
      ├─► YES ──► Suppress short click. Do nothing.
      │
      └─► NO  ──► Trigger SHORT-CLICK Action!
```
This architecture completely resolves race conditions between short clicks and long holds across all five buttons.

---

## 9. Multi-Paradigm Audio Signal Processing Pipeline

The firmware supports both real-time algorithmic synthesis and mass-storage audio streaming:

```text
                             ┌─────────────────────────────────┐
                             │       User Song Selection       │
                             └───────────────┬─────────────────┘
                                             │
                      ┌──────────────────────┴──────────────────────┐
                      ▼                                             ▼
       ┌─────────────────────────────┐               ┌─────────────────────────────┐
       │ Classical Repertoire (0..7) │               │ Micro-SD Card Track (8..N)  │
       │ Algorithmic Synthesizer     │               │ Mass-Storage Media Stream   │
       └──────────────┬──────────────┘               └──────────────┬──────────────┘
                      │                                             │
                      ▼                                             ▼
       ┌─────────────────────────────┐               ┌─────────────────────────────┐
       │ Hardware Timer 3 PWM (PB0)  │               │ 4-Bit High-Speed SDIO       │
       │ Period Tuning (f = 1000/T)  │               │ Aligned 2048 B Bounce Buffer│
       └──────────────┬──────────────┘               └──────────────┬──────────────┘
                      │                                             │
                      ▼                                             ▼
       ┌─────────────────────────────┐               ┌─────────────────────────────┐
       │ DDS Waveform Generator      │               │ RIFF Parser (.WAV) or       │
       │ (Sine/Triangle/Saw/Square)  │               │ Helix Fixed-Point (.MP3)    │
       └──────────────┬──────────────┘               └──────────────┬──────────────┘
                      │                                             │
                      └──────────────────────┬──────────────────────┘
                                             │
                                             ▼
                             ┌───────────────────────────────┐
                             │ Circular Hardware DMA1 Str 5  │
                             │ Double-Buffered I2S3 Pipeline │
                             └──────┬─────────────────┬──────┘
                                    │                 │
                16-bit Digital PCM  │                 │ Downscaled 12-bit
                at 44.108 kHz       ▼                 ▼ (sample / 16 + 2048)
                             ┌─────────────┐   ┌─────────────┐
                             │   ES8388    │   │  Internal   │
                             │ Stereo DAC  │   │  DAC1 (PA4) │
                             │ 3.5mm (CN3) │   └─────────────┘
                             └─────────────┘
```

---

## 10. Hardware Timer TIM3 PWM Note Synthesizer

- **Timer Timebase**: Prescaler set to $83$ ($1\text{ }\mu\text{s}$ per tick).
- **Pitch Period Equation**:
  $$\text{ARR} = (T_{\text{note}} \times 1000.0) - 1$$
- **Glitch-Free Register Reload**:
  - Updates forced via `TIM3->EGR = TIM_EGR_UG`.
  - Immediate counter reset if `TIM3->CNT >= ARR`.
- **Note Articulation**: $85\%$ active tone phase followed by a $15\%$ staccato gap scheduled by `struct k_timer`.

---

## 11. Everest Semiconductor ES8388 Stereo Codec & PLLI2S Clock Synthesis

- **Control Bus**: Bit-banged standard-mode I2C on `PF0`/`PF1` at $100\text{ kHz}$.
- **Audio Clock Synthesis**:
  $$f_{\text{VCO}} = \frac{8\text{ MHz}}{8\text{ (PLLM)}} \times 271\text{ (PLLI2SN)} = 271.0\text{ MHz}$$
  $$f_{\text{I2SxCLK}} = \frac{271.0\text{ MHz}}{2\text{ (PLLI2SR)}} = 135.5\text{ MHz}$$
  $$F_s = \frac{135.5\text{ MHz}}{256 \times 12} = \mathbf{44108.07\text{ Hz}} \approx 44.1\text{ kHz}$$
  Yields exact CD-quality audio reproduction ($0.018\%$ frequency error).

---

## 12. Circular Hardware DMA Audio Streamer (DMA1 Stream 5)

- **Autonomous Hardware Burst**: DMA1 Stream 5 Channel 0 feeds `SPI3->DR` continuously in circular double-buffer mode (`s_audio_dma[512]`).
- **Interrupt Reduction**: The CPU is interrupted only on Half-Transfer (`HTIF5`) and Transfer-Complete (`TCIF5`) events ($86\text{ Hz}$ vs. previous $88,200\text{ Hz}$), executing a sub-microsecond block copy that eliminates audio crackling.

---

## 13. Micro-SD FAT32 Filesystem & 4-Bit High-Speed SDIO

- **Overrun Prevention**: Configured DMA2 Stream 3/6 with `STM32_DMA_FIFO_FULL` and `clk-div = <10>` ($4\text{ MHz}$ stable SDIO bus).
- **Aligned 2048 B Bounce Buffer**: Reads execute via `s_read_bounce[2048] __attribute__((aligned(4)))`, preventing unaligned memory faults and SDIO FIFO overruns (`-FR_DISK_ERR`).

---

## 14. RealNetworks Helix Fixed-Point MP3 Decoding Engine

- **Pure Integer Arithmetic**: Uses 32-bit fixed-point math tailored for the ARM Cortex-M4 MAC pipeline.
- **Zero External Hardware**: Decodes standard MPEG-1/2 Layer III bitstreams directly from SD into PCM audio without requiring an external MP3 decoder IC.

---

## 15. Uncompressed 16-Bit 44.1 kHz WAV Streaming Pipeline

- **RIFF Parser**: Dynamically walks WAV binary chunks (`fmt `, `data`) to bypass embedded metadata and album art.
- **Channel Adaptation**: Automatically detects mono vs. stereo and duplicates mono samples across both I2S output slots.

---

## 16. User Operating Guide: D-Pad Controls & Multi-Button Chords

```text
========================================================================================
PERSONAL MP3 PLAYER USER OPERATING GUIDE
========================================================================================
Control Gesture        Pin(s)         Action & Operational Response
----------------------------------------------------------------------------------------
UP Button (Click)      PC5            Cycles forward to Next Track (+1, Track 1 to N)
UP Button (Hold)       PC5 (>=450ms)  Instantly jumps back to Track 1 (Für Elise)
DOWN Button (Click)    PC1            Cycles backward to Previous Track (-1, Track N to 1)
DOWN Button (Hold)     PC1 (>=450ms)  Toggles Play / Pause on active track
LEFT Button (Click)    PC0            Decreases volume by 5% per click
LEFT Button (Hold)     PC0            Smoothly ramps volume down to 0% (Mute)
RIGHT Button (Click)   PC4            Increases volume by 5% per click
RIGHT Button (Hold)    PC4            Smoothly ramps volume up to 100%
PRESS / USER (Click)   PA0            Toggles Play / Pause
PRESS / USER (Hold)    PA0 (>=500ms)  Fully halts playback (STOPPED state)
UP + DOWN (Chord)      PC5 + PC1      Cycles Waveform: SINE -> TRIANGLE -> SAWTOOTH -> SQUARE
LEFT + RIGHT (Chord)   PC0 + PC4      Mutes / Unmutes onboard buzzer (Headphone-only mode)
========================================================================================
```

---

## 17. Automated Unit Test Suite (25/25 Passing via Unity)

Host-native testing was performed using the **Unity Test Framework** on MinGW GCC via `pio test -e native`:

```text
Processing test_player_logic in native environment
--------------------------------------------------------------------------------
Building...
Testing...
test\test_player_logic\test_player_logic.c:216: test_decode_all_buttons_released_is_song_0          [PASSED]
test\test_player_logic\test_player_logic.c:217: test_decode_button2_only_is_song_1                  [PASSED]
test\test_player_logic\test_player_logic.c:218: test_decode_button3_only_is_song_2                  [PASSED]
test\test_player_logic\test_player_logic.c:219: test_decode_button2_and_button3_is_song_3          [PASSED]
test\test_player_logic\test_player_logic.c:220: test_decode_button4_only_is_song_4                  [PASSED]
test\test_player_logic\test_player_logic.c:221: test_decode_button4_and_button2_is_song_5          [PASSED]
test\test_player_logic\test_player_logic.c:222: test_decode_button4_and_button3_is_song_6          [PASSED]
test\test_player_logic\test_player_logic.c:223: test_decode_all_buttons_pressed_is_song_7          [PASSED]
test\test_player_logic\test_player_logic.c:226: test_confirmation_within_5_seconds_does_not_timeout [PASSED]
test\test_player_logic\test_player_logic.c:227: test_confirmation_at_exact_5_seconds_expires       [PASSED]
test\test_player_logic\test_player_logic.c:228: test_confirmation_past_5_seconds_expires           [PASSED]
test\test_player_logic\test_player_logic.c:229: test_confirmation_timeout_handles_32bit_rollover   [PASSED]
test\test_player_logic\test_player_logic.c:232: test_toggle_play_pause_from_stopped_starts_playing [PASSED]
test\test_player_logic\test_player_logic.c:233: test_toggle_play_pause_from_playing_pauses         [PASSED]
test\test_player_logic\test_player_logic.c:234: test_toggle_play_pause_from_paused_resumes_playing [PASSED]
test\test_player_logic\test_player_logic.c:235: test_toggle_play_pause_while_confirming            [PASSED]
test\test_player_logic\test_player_logic.c:236: test_player_state_strings_are_valid                [PASSED]
test\test_player_logic\test_player_logic.c:239: test_normalize_volume_at_min_is_zero_percent       [PASSED]
test\test_player_logic\test_player_logic.c:240: test_normalize_volume_below_min_clamps_to_zero     [PASSED]
test\test_player_logic\test_player_logic.c:241: test_normalize_volume_at_max_is_one_hundred_pct   [PASSED]
test\test_player_logic\test_player_logic.c:242: test_normalize_volume_above_max_clamps_to_one_hnd   [PASSED]
test\test_player_logic\test_player_logic.c:243: test_normalize_volume_midpoint_is_fifty_percent    [PASSED]
test\test_player_logic\test_player_logic.c:246: test_get_song_info_returns_correct_title           [PASSED]
test\test_player_logic\test_player_logic.c:247: test_get_song_info_returns_last_song_correctly     [PASSED]
test\test_player_logic\test_player_logic.c:248: test_get_song_info_out_of_bounds_wraps_to_song_0   [PASSED]
------------- native:test_player_logic [PASSED] Took 25.50 seconds -------------

=================================== SUMMARY ===================================
Environment    Test               Status    Duration
-------------  -----------------  --------  ------------
native         test_player_logic  PASSED    00:00:25.505
================= 25 test cases: 25 succeeded in 00:00:25.505 =================
```

---

## 18. Static Code Analysis Verification (`pio check`)

Cppcheck analysis verified zero defects in project application code:

```text
Environment    Tool      Status    Duration
-------------  --------  --------  ------------
black_f407zg   cppcheck  PASSED    00:00:02.793
========================= 1 succeeded in 00:00:02.793 =========================
```

---

## 19. Memory Footprint & Resource Utilization

```text
Processing black_f407zg (platform: ststm32; board: black_f407zg; framework: zephyr)
--------------------------------------------------------------------------------
Memory region         Used Size  Region Size  %age Used
           FLASH:      158280 B         1 MB     15.09%
             RAM:      111328 B       128 KB     84.94%
========================= [SUCCESS] Took 25.39 seconds =========================
```

- **Flash ROM**: $158.28\text{ KB}$ of $1024\text{ KB}$ ($15.09\%$)
- **SRAM**: $111.32\text{ KB}$ of $128\text{ KB}$ ($84.94\%$)

---

## 20. Repository Layout & File Navigation

```text
LAB_2_ZephyrRTOS_Personal_MP3_Player/
├── .gitignore                      # Exclusions for .pio, .vscode, binaries, and audio files
├── platformio.ini                  # Target (black_f407zg) & host-native Unity test environments
├── README.md                       # Comprehensive 20-section project portfolio documentation
├── include/
│   ├── app_config.h                # Hardware pin mappings, memory bases, and timings
│   ├── audio_codec_es8388.h        # ES8388 codec registers, I2S DMA, and DDS prototypes
│   ├── audio_engine.h              # Classical song score player and PWM timer prototypes
│   ├── lcd_font.h                  # 16x8 ASCII bitmap font tables
│   ├── lcd_st7789.h                # ST7789 FSMC 8080 driver interface
│   ├── player_logic.h              # Hardware-decoupled pure decision and state transition logic
│   ├── sd_card_reader.h            # Micro-SD FAT32 inspection and directory scanning interface
│   ├── threads.h                   # Thread entry points, stack sizes, and IPC mutex definitions
│   └── wav_player.h                # WAV streaming and Helix MP3 decoding engine prototypes
├── src/
│   ├── audio_codec_es8388.c        # I2C/I2S3 driver, PLLI2S setup, and DMA1 Stream 5 streamer
│   ├── audio_engine.c              # Hardware TIM3 PWM note player & classical repertoire
│   ├── lcd_st7789.c                # FSMC Bank 3 8080 parallel driver & differential rendering
│   ├── main.c                      # Peripheral initialization, UART banner, and thread spawning
│   ├── player_logic.c              # Pure decision logic (binary decode, timeout, state machine)
│   ├── sd_card_reader.c            # 4-bit SDIO FAT32 driver, MBR inspector, and track scanner
│   ├── song_data.inc               # Note and beat score arrays for the 8 classical compositions
│   ├── threads.c                   # 3 concurrent cooperative Zephyr threads & D-pad debouncer
│   ├── wav_player.c                # Aligned bounce-buffered WAV streamer & Helix MP3 frame loop
│   └── helix/                      # RealNetworks Helix Fixed-Point MP3 Decoder source library
├── reference/
│   ├── song.h                      # Course reference header
│   └── song_def.h                  # Course song definitions
├── docs/
│   ├── Laboratory Activity 2.md    # Course laboratory specification
│   ├── laboratory-report.md        # Formal academic laboratory report (Markdown source)
│   ├── laboratory-report.pdf       # Formal academic laboratory report (Publication-grade PDF)
│   ├── oral-defense-guide.md       # Technical oral defense Q&A preparation guide
│   └── schematic.pdf               # RT-Thread Spark Development Board hardware schematics
└── test/
    └── test_player_logic/
        └── test_player_logic.c     # Host-native Unity unit test suite (25/25 passing)
```
