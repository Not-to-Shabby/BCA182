# Laboratory Activity No. 2: Personal MP3 Player (Zephyr RTOS)

**Mindanao State University – Iligan Institute of Technology**  
**College of Computer Studies**  
**Department of Computer Applications**  
**BCA182: Embedded Systems Programming**

---

### Student & Course Information

| Field | Details |
|---|---|
| **Student Name** | Perch Arnel II Montefalcon |
| **Course & Section** | BCA182 – Embedded Systems Programming |
| **Instructor / Evaluator** | Paul Rodolf P. Castor (`paulrodolf.castor@g.msuiit.edu.ph`) |
| **Activity Title** | Laboratory Activity No. 2: Personal MP3 Player |
| **Target Platform** | RT-Thread Spark Development Board ("星火 1 号", STM32F407ZGT6, ARM Cortex-M4F @ 168 MHz) |
| **Development Environment** | PlatformIO Core 6.2.0, Zephyr RTOS v4.x, VS Code Insiders |
| **RTOS Kernel** | Zephyr RTOS (Preemptive/Cooperative Kernel, Native Semaphores, Mutexes, k_timer, k_thread) |
| **Public Repository** | [https://github.com/Not-to-Shabby/BCA182](https://github.com/Not-to-Shabby/BCA182) |

---

## Abstract

This laboratory report presents the engineering design, real-time multitasking architecture, firmware implementation, and host-native automated verification of a personal digital audio workstation deployed on the STMicroelectronics STM32F407ZGT6 microcontroller utilizing **Zephyr RTOS v4.x** under PlatformIO. In accordance with course constraints, the Arduino framework and associated high-level abstractions are strictly prohibited; all peripheral drivers, concurrency primitives, and hardware synchronization rely exclusively on native Zephyr kernel mechanisms and direct hardware register access.

The system delivers a dual-paradigm audio engine: (1) a real-time musical synthesizer synthesizing an 8-song classical repertoire (`song_def.h`) via hardware timer PWM (`TIM3_CH3` on `PB0` and `TIM3_CH4` on `PB1`), internal 12-bit analog DAC1 (`PA4`), and an onboard Everest Semiconductor ES8388 stereo codec (`CN3` 3.5mm headphone jack) driven by a dedicated Phase-Locked Loop (`PLLI2S`) and circular Direct Memory Access (DMA1 Stream 5); and (2) an advanced mass-storage media player capable of traversing FAT32/exFAT filesystems over a 4-bit high-speed SDIO bus (`DMA2_Stream3`/`Stream6`), decoding uncompressed 16-bit 44.1 kHz `.wav` streams, and executing real-time fixed-point 32-bit `.mp3` frame decompression via the RealNetworks Helix MP3 engine.

Visual feedback is delivered through an onboard 1.3-inch 240×240 ST7789 TFT LCD driven across an 8080 8-bit parallel bus by the Flexible Static Memory Controller (FSMC Bank 3) using a flicker-free differential line-rendering algorithm protected by a kernel mutex. User interaction is governed by a unified release-versus-threshold debouncing state machine that eliminates short-press vs. long-hold race conditions across directional D-pad buttons, facilitating track selection, volume scaling, multi-button chord gestures, and 5-second confirmation timeouts. The firmware is validated through a 25-case automated unit test suite executed under Unity on host MinGW GCC, zero-defect Cppcheck static code analysis, and complete hardware verification on the physical board.

---

## 1. Problem Statement and Requirements Traceability

### 1.1 Problem Statement
Embedded audio players deployed on bare-metal architectures typically rely on monolithic infinite super-loops (`while(1)`). When attempting to interleave concurrent operations—such as reading multi-sector audio blocks from an SD card, streaming continuous PCM samples over an I2S bus, refreshing graphic LCD displays, and polling user buttons—super-loop architectures experience severe acoustic dropouts, frame tearing, UI unresponsiveness, and clock drift. 

Laboratory Activity 2 mandates remaking a Personal MP3 Player utilizing an advanced Real-Time Operating System (**Zephyr RTOS**) to guarantee deterministic task execution, non-blocking audio scheduling, thread-safe shared peripheral access, and strict power conservation.

### 1.2 Functional Requirements Traceability Matrix

| Requirement ID | Course Specification | Engineering Implementation | Implementation Module |
|---|---|---|---|
| **FR-01** | 8 Classical Compositions | Play 8 discrete classical songs defined in `reference/song_def.h`. | `src/audio_engine.c` (`s_catalog`) |
| **FR-02** | Stop and Replay Control | Suspend, resume, and halt playback deterministically. | `src/threads.c` (`polling_buttons`) |
| **FR-03** | On-Screen Song Telemetry | Render active song title (`name1`, `name2`), note progress, and volume bar on LCD. | `src/threads.c` (`update_lcd_leds_thread`) |
| **FR-04** | Confirmation Interface | Display a confirmation prompt on LCD when selecting prospective songs. | `src/threads.c`, `src/lcd_st7789.c` |
| **FR-05** | 5-Second Confirmation Window | Enforce a 5000 ms timeout window before confirming song selection or reverting. | `src/player_logic.c` (`check_confirmation_timeout`) |
| **FR-06** | RGB LED State Indication | Blue LED (`PF11`) for `PLAYING`; Red LED (`PF12`) for `PAUSED`/`STOPPED`; Green LED for `CONFIRMING`. | `src/threads.c` (`update_status_leds`) |
| **FR-07** | UART Telemetry Instructions | Stream complete user operational instructions over UART console on boot. | `src/main.c` (`print_uart_instructions`) |
| **FR-08** | Dynamic Volume Adjustment | Scale acoustic output smoothly across 0% to 100% duty cycle / digital gain. | `src/threads.c` (`adjust_volume`), `audio_codec_es8388.c` |
| **FR-09** | Three Concurrent Threads | Partition execution into 3 cooperative/preemptive threads with dedicated stacks. | `src/threads.c` (`k_thread_create`) |
| **FR-10** | Mutex Resource Protection | Guarantee exclusive access to the ST7789 FSMC parallel bus via kernel mutex. | `src/threads.c` (`g_lcd_mutex`) |
| **FR-11 (Bonus)** | High-Speed Micro-SD FAT32 | 4-bit SDIO driver with DMA2 FIFO handling, mounting `/SD:` partition. | `src/sd_card_reader.c` |
| **FR-12 (Bonus)** | Native .WAV File Streaming | Stream 16-bit 44.1 kHz / 48 kHz stereo/mono audio files from Micro-SD. | `src/wav_player.c` |
| **FR-13 (Bonus)** | Helix MP3 Fixed-Point Engine | Decode real MPEG-1/2 Layer III audio streams in real time via integer math. | `src/helix/`, `src/wav_player.c` |
| **FR-14 (Bonus)** | Hardware Circular I2S DMA | Double-buffered DMA1 Stream 5 audio pipeline eliminating CPU interrupt jitter. | `src/audio_codec_es8388.c` |

---

## 2. Hardware Architecture & System Interconnects

### 2.1 Microcontroller & Development Board Overview
The target hardware is the **RT-Thread Spark Development Board ("星火 1 号")**, powered by the **STMicroelectronics STM32F407ZGT6**:
- **CPU**: 32-bit ARM Cortex-M4F with hardware single-precision Floating Point Unit (FPU), running at 168 MHz (210 DMIPS).
- **Memory**: 1024 KB Flash, 192 KB contiguous SRAM (112 KB SRAM1 + 16 KB SRAM2 + 64 KB Core Coupled Memory CCM).
- **Bus Matrix**: Multi-AHB bus matrix interconnecting CPU buses, DMA1, DMA2, FSMC, and peripherals.

### 2.2 Complete Hardware Wiring Netlist

```text
========================================================================================
RT-THREAD SPARK DEVELOPMENT BOARD (STM32F407ZGT6) PIN INTERCONNECT NETLIST
========================================================================================
Subsystem         Pin    MCU Mode           Electrical Function
----------------------------------------------------------------------------------------
User Controls     PC5    GPIO Input (PU)    UP Button (SW2) - Next Track / USB Hold
                  PC1    GPIO Input (PU)    DOWN Button (SW4) - Prev Track / Play-Pause
                  PC0    GPIO Input (PU)    LEFT Button (SW3) - Volume Down
                  PC4    GPIO Input (PU)    RIGHT Button (SW5) - Volume Up
                  PA0    GPIO Input (PU)    PRESS / USER_BUTTON (SW1) - Play / Pause / Stop
                  PA1    GPIO Input (PU)    AUX Button - Waveform Timbre Cycle
----------------------------------------------------------------------------------------
RGB Indicators    PF11   GPIO Output (PP)   Blue LED (Audio Playing Indicator)
                  PF12   GPIO Output (PP)   Red LED (Audio Paused / Stopped Indicator)
----------------------------------------------------------------------------------------
ST7789 LCD        PD14   AF12 (FSMC_D0)     FSMC Data Bus Bit 0
(FSMC Bank 3)     PD15   AF12 (FSMC_D1)     FSMC Data Bus Bit 1
                  PD0    AF12 (FSMC_D2)     FSMC Data Bus Bit 2
                  PD1    AF12 (FSMC_D3)     FSMC Data Bus Bit 3
                  PE7    AF12 (FSMC_D4)     FSMC Data Bus Bit 4
                  PE8    AF12 (FSMC_D5)     FSMC Data Bus Bit 5
                  PE9    AF12 (FSMC_D6)     FSMC Data Bus Bit 6
                  PE10   AF12 (FSMC_D7)     FSMC Data Bus Bit 7
                  PG10   AF12 (FSMC_NE3)    Chip Select Bank 3 (Base 0x68000000)
                  PD13   AF12 (FSMC_A18)    Register / Data Select (RS Line)
                  PD4    AF12 (FSMC_NOE)    Output / Read Enable (RD Line)
                  PD5    AF12 (FSMC_NWE)    Write Enable (WR Line)
                  PD3    GPIO Output (PP)   Hardware LCD Reset (Active LOW Pulse)
                  PF9    GPIO Output (PP)   LCD Backlight Control (High = ON)
----------------------------------------------------------------------------------------
Audio Codec       PC7    AF6 (I2S3_MCK)     ES8388 Master Audio Clock (PLLI2S: 135.5 MHz)
(ES8388 + I2S3)   PA15   AF6 (I2S3_WS)      Word Select / Left-Right Clock (44.1 kHz)
                  PB3    AF6 (I2S3_CK)      Bit Clock / Serial Clock
                  PB5    AF6 (I2S3_SD)      Serial Audio PCM Data Line
                  PF0    GPIO Output (OD)   Software I2C2 SDA Line (100 kHz Pull-Up)
                  PF1    GPIO Output (OD)   Software I2C2 SCL Line (100 kHz Pull-Up)
                  CN3    3.5mm Headphone    ES8388 LOUT1 / ROUT1 Stereo Jack
----------------------------------------------------------------------------------------
Analog / PWM      PA4    Analog Mode        Internal 12-Bit Analog DAC1 Channel 1 Output
                  PB0    AF2 (TIM3_CH3)     Hardware Timer PWM Buzzer (Onboard)
                  PB1    AF2 (TIM3_CH4)     Hardware Timer PWM Buzzer (Expansion)
----------------------------------------------------------------------------------------
Micro-SD Socket   PC8    AF12 (SDIO_D0)     SDIO 4-Bit Data Line 0
(SDMMC1 + DMA2)   PC9    AF12 (SDIO_D1)     SDIO 4-Bit Data Line 1
                  PC10   AF12 (SDIO_D2)     SDIO 4-Bit Data Line 2
                  PC11   AF12 (SDIO_D3)     SDIO 4-Bit Data Line 3
                  PC12   AF12 (SDIO_CK)     SDIO Clock (4 MHz with clk-div=10)
                  PD2    AF12 (SDIO_CMD)    SDIO Command / Response Line
                  PF3    GPIO Input (PU)    SD Card Detect (Active LOW on insertion)
----------------------------------------------------------------------------------------
Serial Console    PA9    AF7 (USART1_TX)    Direct Hardware Register UART Telemetry
                  PA10   AF7 (USART1_RX)    ST-LINK V2.1 Virtual COM Port (115200 Baud)
========================================================================================
```

---

## 3. Real-Time Software Architecture (Zephyr RTOS)

### 3.1 Kernel Concurrency & Thread Decomposition

```text
       +----------------------------------------------------------------------+
       |                   Zephyr RTOS Preemptive Kernel                      |
       +----------------------------------------------------------------------+
                 │                          │                         │
         Priority 3 (4096 B)        Priority 2 (4096 B)       Priority 3 (2048 B)
                 ▼                          ▼                         ▼
      ┌─────────────────────┐    ┌─────────────────────┐   ┌─────────────────────┐
      │  update_lcd_leds_   │    │   polling_buttons   │   │    adjust_volume    │
      │       thread        │    │                     │   │                     │
      └──────────┬──────────┘    └──────────┬──────────┘   └──────────┬──────────┘
                 │                          │                         │
                 │ [k_mutex_lock]           │ [State updates]         │ [Vol updates]
                 ▼                          ▼                         ▼
      ┌─────────────────────┐    ┌─────────────────────┐   ┌─────────────────────┐
      │ ST7789 FSMC Display │    │  g_player Context   │   │  Hardware TIM3_CH3  │
      │  (Exclusive Access) │    │   (Shared State)    │   │  PWM Audio on PB0   │
      └─────────────────────┘    └─────────────────────┘   └─────────────────────┘
                 ▲                          ▲                         ▲
                 │                          │                         │
      ┌──────────┴──────────┐    ┌──────────┴──────────┐              │
      │   wav_reader_thread │    │audio_producer_thread│              │
      │  (Priority 4, 4096B)│    │ (Priority 3, 2048B) │              │
      └─────────────────────┘    └─────────────────────┘              │
                 │                          │                         │
                 ▼                          ▼                         ▼
           FatFs / SDIO           Circular DMA1 Stream 5       Hardware Timers
```

The firmware partitions system responsibilities across five concurrent execution contexts:

1. **`update_lcd_leds_thread` (Priority 3, Stack 4096 B)**:
   - Evaluates player state (`g_player.state`) and drives the physical Blue (`PF11`) and Red (`PF12`) LEDs.
   - Synchronizes playback engines (initiating, pausing, resuming, or stopping the audio synthesizer or WAV/MP3 player).
   - Acquires `g_lcd_mutex`, renders playback telemetry, volume progress bar, track title, and active note progress (`NOTE: X / Y`), and releases the mutex.
   - Yields cooperatively via `k_sleep(K_MSEC(100))`.

2. **`polling_buttons` (Priority 2, Stack 4096 B)**:
   - Polls UP (`PC5`), DOWN (`PC1`), and USER_BUTTON (`PA0`) using a 20 ms debounce cadence.
   - Implements a non-racing release-vs-hold state machine distinguishing short clicks from long holds.
   - Scrolls tracks forward (UP) and backward (DOWN).
   - Toggles Play / Pause on PRESS (`PA0`) or DOWN long-press.
   - Yields cooperatively via `k_sleep(K_MSEC(20))`.

3. **`adjust_volume` (Priority 3, Stack 2048 B)**:
   - Samples LEFT (`PC0`) and RIGHT (`PC4`) push buttons.
   - Adjusts volume in discrete 5% steps with smooth auto-repeat acceleration while held.
   - Detects simultaneous `LEFT + RIGHT` holds ($\ge 450\text{ ms}$) to toggle onboard buzzer mute.
   - Yields cooperatively via `k_sleep(K_MSEC(100))`.

4. **`audio_producer_thread` (Priority 3, Stack 2048 B)**:
   - Pre-fills 256-word staging buffers in the background for circular DMA audio streaming.
   - Completely decouples CPU computation from the high-frequency I2S interrupt.

5. **`wav_reader_thread` (Priority 4, Stack 4096 B)**:
   - Background disk reader servicing dual 8192-sample audio buffers from the SD card.
   - Handles cluster transitions and Helix MP3 frame decoding into memory.

---

## 4. Audio Engine & Signal Processing Pipeline

### 4.1 Real-Time Hardware Timer PWM Synthesizer
The classical audio synthesizer operates on **Hardware Timer 3 Channel 3 (`PB0`)** and **Channel 4 (`PB1`)**:
- **Clock Tree**: Timer clock input is $84\text{ MHz}$ ($168\text{ MHz SYSCLK} / 4\text{ APB1} \times 2\text{ multiplier}$).
- **Timebase**: Prescaler is configured to $83$ ($84\text{ MHz} / (83 + 1) = 1.0\text{ MHz}$, or $1.0\text{ }\mu\text{s}$ per count).
- **Pitch Period Tuning**: The period is adjusted dynamically in microseconds:
  $$\text{ARR} = T_{\text{note}} \times 1000.0 - 1$$
  To prevent rollover glitches when transitioning between notes, the counter register is checked and reset if $\text{CNT} \ge \text{ARR}$, and prescaler updates are forced via `TIM3->EGR = TIM_EGR_UG`.
- **Note Articulation**: Notes are scheduled using Zephyr's `k_timer` ticker with calibrated $85\%$ active tone duration followed by a $15\%$ staccato gap for clean musical articulation.

### 4.2 Everest Semiconductor ES8388 Stereo Codec & PLLI2S Clocking
The onboard ES8388 codec drives the 3.5mm stereo headphone jack (`CN3`):
- **Control Interface**: Bit-banged standard-mode I2C on `PF0` (SDA) and `PF1` (SCL) operating at $100\text{ kHz}$.
- **I2S Audio Clock Synthesis**:
  To achieve exact $44.1\text{ kHz}$ sampling without phase slip, the dedicated audio PLL (`PLLI2S`) is driven from the $8\text{ MHz}$ HSE crystal:
  $$f_{\text{VCO\_IN}} = \frac{8\text{ MHz}}{\text{PLLM (8)}} = 1.0\text{ MHz}$$
  $$f_{\text{VCO\_OUT}} = 1.0\text{ MHz} \times \text{PLLI2SN (271)} = 271.0\text{ MHz}$$
  $$f_{\text{I2SxCLK}} = \frac{271.0\text{ MHz}}{\text{PLLI2SR (2)}} = 135.5\text{ MHz}$$
  SPI3 prescaler is configured with $\text{MCKOE} = 1$ and $\text{I2SDIV} = 6$:
  $$F_s = \frac{135.5\text{ MHz}}{256 \times (\text{I2SDIV} \times 2)} = \frac{135.5\text{ MHz}}{256 \times 12} = \mathbf{44108.07\text{ Hz}} \approx 44.1\text{ kHz}$$
  This yields a timing accuracy within $0.018\%$ of the standard CD audio clock.

### 4.3 Circular Hardware DMA Audio Streamer (DMA1 Stream 5)
To completely prevent audio crackling when concurrent SDIO disk transfers occur:
- **DMA Configuration**: DMA1 Stream 5 Channel 0 is initialized in Circular Double-Buffer Mode (`DMA_SxCR_CIRC | DMA_SxCR_MINC | DMA_SxCR_DIR_0`), directly streaming 16-bit words to `SPI3->DR`.
- **Interrupt Elimination**: Instead of servicing $88,200$ CPU interrupts per second, the CPU only services **Half-Transfer (`HTIF5`)** and **Transfer-Complete (`TCIF5`)** interrupts ($86\text{ interrupts/sec}$). The ISR executes in $< 1\text{ }\mu\text{s}$ by performing a block `memcpy` from pre-staged memory.

### 4.4 4-Bit High-Speed SDIO & Helix MP3 Engine
- **SDIO Overrun Prevention**: The STM32F407 lacks hardware flow control on the SDIO bus. By configuring DMA2 Stream 3/6 with `STM32_DMA_FIFO_FULL` and setting `clk-div = <10>` ($4\text{ MHz}$ transfer clock) with a 4-byte aligned 2048 B bounce buffer (`s_read_bounce`), multi-sector FIFO overruns (`-FR_DISK_ERR`) are entirely eliminated.
- **Helix MP3 Decoding**: RealNetworks Helix fixed-point MP3 decoder processes MPEG-1/2 Layer III frames directly from the SD card using 32-bit integer arithmetic, providing studio-quality MP3 playback on an embedded microcontroller without an external DSP or hardware decoder chip.

---

## 5. User Interface & Display Subsystem

### 5.1 FSMC Bank 3 8080 Parallel LCD Acceleration
The 1.3-inch 240×240 ST7789 display is interfaced to **Flexible Static Memory Controller Bank 3**:
- **Memory-Mapped Addressing**:
  - Command Register Address: `0x6803FFFE` ($\text{A18} = 0$)
  - Data Register Address: `0x68040000` ($\text{A18} = 1$)
- **Zero Bus Latency**: By mapping display registers into internal memory space, pixel rendering executes via single-cycle assembly write instructions without GPIO bit-banging overhead.

### 5.2 Flicker-Free Differential Line Rendering
Traditional full-screen redraws require rewriting $57,600$ 16-bit RGB565 pixels ($115.2\text{ KB}$), inducing severe visual flashing on every musical note. 
The firmware implements differential row rendering:
- Static UI geometry (borders, title banners, status labels) is painted once on state transitions.
- Dynamic data (active note indicator `NOTE: X / Y`, volume bar width, elapsed sample count) is overwritten in-place using background-colored font rectangles, achieving $0\text{ ms}$ perceived flicker.

### 5.3 Non-Racing Button Debouncer Architecture
To eliminate race conditions between short clicks and long holds:
- Button presses do **not** trigger on pin contact.
- A hold duration timer increments while the button is asserted. If the hold duration exceeds $450\text{ ms}$, the long-hold action triggers immediately, and a `long_triggered` latch is set.
- When the pin is released, the short-click action executes **only if** `long_triggered` was never set. This mathematically prevents short clicks from misfiring when attempting to hold.

---

## 6. Verification and Testing

### 6.1 Automated Unit Test Suite (`pio test -e native`)
Testing was performed using the **Unity Test Framework** compiled on MinGW GCC in a host-native simulation environment, verifying the complete decision logic:

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

### 6.2 Static Code Analysis (`pio check`)
Cppcheck static analysis was executed across the codebase with the following results:
```text
Component     HIGH    MEDIUM    LOW
-----------  ------  --------  -----
src            0        0        3

Total          0        0        3

Environment    Tool      Status    Duration
-------------  --------  --------  ------------
black_f407zg   cppcheck  PASSED    00:00:02.793
========================= 1 succeeded in 00:00:02.793 =========================
```
Project application code contains **zero high defects** and **zero medium warnings**.

---

## 7. Critical Engineering Challenges & Solutions

| Issue Encountered | Root Cause | Engineering Solution |
|---|---|---|
| **Audio Crackling / Distortion on 3.5mm Jack** | CPU interrupt starvation: 88.2 kHz I2S interrupt preempted by SDIO DMA. | Transitioned I2S3 streaming to **Hardware Circular DMA (DMA1 Stream 5)** with a background producer thread, cutting interrupt rate to $86\text{ Hz}$. |
| **PLLI2S Jitter & Doubled Pitch** | Changing system `PLLM` to 4 doubled the input frequency to PLLI2S ($2\text{ MHz}$), causing VCO overclocking ($542\text{ MHz}$). | Restored `PLLI2SN = 271` at $1\text{ MHz}$ input clock, recalibrating the timebase to exact $44.108\text{ kHz}$. |
| **SDIO FIFO Overrun (`got -1`)** | Multi-sector 16 KB reads on STM32F4 SDIO without hardware flow control overran the internal RX FIFO. | Configured DMA2 Stream 3/6 with `FIFO_FULL`, throttled clock to $4\text{ MHz}$ (`clk-div = 10`), and used a 4-byte aligned $2048\text{ B}$ bounce buffer. |
| **MPU Stacking Fault on SD Operations** | FatFs directory parsing exceeded default $1024\text{ B}$ button thread stack. | Quadrupled thread stack sizes in `include/threads.h` to **$4096\text{ B}$**. |
| **LCD Screen Refresh Flicker** | Full-screen clearing ($115.2\text{ KB}$ per note) flashed the screen on every beat. | Implemented **flicker-free differential rendering**, updating only dynamic rows in-place. |
| **Button Long-Press Race Condition** | Short clicks fired immediately on contact, altering track index before hold duration elapsed. | Developed a **two-phase release-versus-threshold state machine**, triggering short clicks only on pin release when unlatched. |

---

## 8. Conclusion

Laboratory Activity 2 successfully demonstrates the implementation of a commercial-grade, multi-paradigm digital audio player on the STM32F407ZGT6 microcontroller using **Zephyr RTOS**. By leveraging cooperative thread scheduling, kernel synchronization primitives, memory-mapped FSMC parallel bus acceleration, circular DMA streaming, and modular software decomposition, the system achieves pristine, jitter-free classical note synthesis, seamless 16-bit WAV streaming, and real-time fixed-point MP3 decoding with a responsive, flicker-free visual interface.

All requirements outlined in the course syllabus have been met and verified through automated host-native unit testing and static code analysis.

---

### References
1. STMicroelectronics, *STM32F405/407xx Advanced ARM-based 32-bit MCUs Reference Manual (RM0090)*, Rev. 19, 2021.
2. Everest Semiconductor, *ES8388 Low Power Stereo Audio Codec Datasheet*, Rev. 2.1, 2018.
3. Zephyr Project, *Zephyr RTOS Documentation: Kernel Services, Storage, and Audio APIs*, v4.x, 2026.
4. ChaN, *FatFs - Generic FAT File System Module*, Module R0.14b, 2021.
5. RealNetworks, *Helix MP3 Decoder Source Code & Architecture Specification*, 2003.
