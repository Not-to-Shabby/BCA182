# Technical Defense & Oral Checkoff Preparation Guide

**Course**: BCA182 – Embedded Systems Programming  
**Author**: Perch Arnel II Montefalcon  
**Activity**: Laboratory Activity No. 2: Personal MP3 Player  
**Target Platform**: RT-Thread Spark Development Board (STM32F407ZGT6, Zephyr RTOS v4.x)  
**Instructor / Evaluator**: Paul Rodolf P. Castor (`paulrodolf.castor@g.msuiit.edu.ph`)  

---

This document provides rigorous, engineering-grade technical explanations for **15 oral examination and code defense questions** covering real-time operating systems, STM32F407 hardware peripherals, digital signal processing, and audio pipelines.

---

### 1. Why did you use Zephyr RTOS instead of FreeRTOS or bare-metal for this lab?
**Answer**:  
Zephyr RTOS provides a modern, modular, and standards-compliant real-time operating system with built-in device tree (`.dts` / `.overlay`) abstractions, native subsystem integration (FatFs Virtual File System, Disk Access, DMA controller, and Power Management), and preemptive/cooperative multithreading. 

Compared to a bare-metal super-loop, Zephyr guarantees that periodic timebases (such as note scheduling via `k_timer` and button debouncing cadences) remain deterministic regardless of how long disk reads or LCD refreshes take. Compared to FreeRTOS, Zephyr's integrated kernel services allow seamless orchestration of multi-threading (`k_thread`), thread-safe storage access, and dynamic power management without external wrapper libraries.

---

### 2. How did you decompose the application into threads, and why are their priorities assigned the way they are?
**Answer**:  
The core architecture implements the 3 mandatory cooperative threads, augmented by 2 dedicated background stream workers:
1. **`polling_buttons` (Priority 2, Stack 4096 B - Highest UI Priority)**:
   - Evaluates user physical inputs (UP, DOWN, LEFT, RIGHT, USER_BUTTON) every 20 ms.
   - Assigned higher priority than display rendering because dropped or delayed button events directly degrade perceived UI responsiveness.
2. **`update_lcd_leds_thread` (Priority 3, Stack 4096 B - Medium Priority)**:
   - Synchronizes audio engine state (play, pause, stop) and drives the Red (`PF12`) and Blue (`PF11`) LEDs.
   - Acquires `g_lcd_mutex` to render track telemetry and volume bars every 100 ms. Human persistence of vision accommodates slight frame latencies, so display updates yield to urgent button polling.
3. **`adjust_volume` (Priority 3, Stack 2048 B - Medium Priority)**:
   - Handles continuous volume scaling and simultaneous chord detection (`LEFT + RIGHT` hold) every 100 ms.
4. **`audio_producer_thread` (Priority 3, Stack 2048 B)**:
   - Pre-fills 256-word staging buffers for the circular DMA audio output in the background.
5. **`wav_reader_thread` (Priority 4, Stack 4096 B - Background Priority)**:
   - Reads 2048-byte chunks from the SD card and decodes MP3 frames. It runs at the lowest priority so disk I/O yields immediately to real-time audio and UI deadlines.

---

### 3. How does your non-racing button debouncer eliminate short-click vs. long-press race conditions?
**Answer**:  
Traditional button debouncers that trigger short-click actions immediately upon pin contact suffer from a race condition: when a user attempts to hold a button (e.g., holding DOWN to toggle Play/Pause), the short-click fires instantly, accidentally scrolling the track before the hold threshold is reached.

Our firmware implements a **two-phase release-versus-threshold state machine**:
1. When a button is pressed, no action is executed immediately. A timestamp records the assertion start (`press_start_ms`).
2. While held, if $(t_{\text{current}} - t_{\text{start}}) \ge 450\text{ ms}$, the **Long-Hold** action executes, and a boolean flag (`long_triggered = true`) is set.
3. When the pin is physically released, the **Short-Click** action executes **only if** `long_triggered == false` and $(t_{\text{current}} - t_{\text{start}}) \ge 20\text{ ms}$ (debouncing filter). 

This guarantees that a hold never triggers an accidental short click, and a short click only evaluates upon confirmed release.

---

### 4. How is the ST7789 LCD driven via the STM32 FSMC 8080 parallel bus?
**Answer**:  
The 1.3-inch ST7789 LCD uses an 8-bit Intel 8080 parallel interface connected to the STM32F407 **Flexible Static Memory Controller (FSMC Bank 3)**:
- **Data Lines**: Pins `PD14`, `PD15`, `PD0`, `PD1`, `PE7`, `PE8`, `PE9`, `PE10` form the 8-bit data bus ($D_0\text{--}D_7$).
- **Control Signals**: `PG10` is Chip Select (`FSMC_NE3`), `PD4` is Read Enable (`NOE`), `PD5` is Write Enable (`NWE`), and `PD13` is Address Bit 18 (`FSMC_A18`), connected to the ST7789 Command/Data ($RS$) line.
- **Memory-Mapped Addressing**:
  Because the controller is mapped into MCU memory space:
  - When writing to `0x6803FFFE`, $A_{18} = 0$, asserting $RS$ LOW (Command Write).
  - When writing to `0x68040000`, $A_{18} = 1$, asserting $RS$ HIGH (Data Write).
  This allows single-cycle assembly write operations (`*(__IO uint8_t*) = data`), eliminating GPIO bit-banging overhead.

---

### 5. Why is a kernel mutex (`k_mutex`) necessary for the LCD display?
**Answer**:  
Because the ST7789 display is driven across a shared physical 8080 bus where every graphical transaction requires setting a column/row bounding box address followed by sequential pixel streaming, transactions are **non-atomic**. 

If `update_lcd_leds_thread` is preempted midway through setting window bounds, and another thread attempts to render a message (or vice-versa), interleaved command and data bytes will corrupt the display controller's internal registers, causing scrambled graphics or an unresponsive screen. The `k_mutex` (`g_lcd_mutex`) enforces mutual exclusion, ensuring only one thread accesses the FSMC bus at any time.

---

### 6. How does the 5-second confirmation window work, and how does it prevent 32-bit tick rollover bugs?
**Answer**:  
When selecting a prospective song via Buttons 2–4 and latching with Button 1, the player transitions to `PLAYER_STATE_CONFIRMING`, turns on the Green LED, and records the timestamp `confirmation_start_ms = k_uptime_get_32()`.

To prevent bugs when the 32-bit millisecond tick rolls over (occurring every $\approx 49.7\text{ days}$):
```c
bool check_confirmation_timeout(uint32_t start_time_ms, uint32_t current_time_ms, uint32_t timeout_duration_ms)
{
    uint32_t elapsed = current_time_ms - start_time_ms;
    return (elapsed >= timeout_duration_ms);
}
```
In modular unsigned 32-bit two's-complement arithmetic, `(current_time_ms - start_time_ms)` calculates the correct elapsed delta even across the $0\text{xFFFFFFFF} \to 0\text{x00000000}$ boundary without conditional overflow branches. If 5000 ms elapse without a second confirmation press, the state machine aborts the change and returns to the prior playback state.

---

### 7. How does the hardware timer PWM synthesizer generate musical notes on `PB0` and `PB1`?
**Answer**:  
The synthesizer uses **STM32 Hardware Timer 3 Channel 3 (`PB0`)** and **Channel 4 (`PB1`)**:
- The APB1 timer clock is $84\text{ MHz}$. Prescaler is configured to $83$, giving a $1.0\text{ MHz}$ counter clock ($1\text{ }\mu\text{s}$ resolution).
- For any musical note with period $T_{\text{note}}$ (in milliseconds):
  $$\text{ARR} = (T_{\text{note}} \times 1000.0) - 1$$
- Volume duty cycle is set to:
  $$\text{CCR} = \frac{\text{ARR} \times \text{volume\_percent}}{200}$$
  ($50\%$ duty cycle at $100\%$ volume produces maximum acoustic power).
- **Glitch Prevention**: When changing pitch, prescaler shadow register updates are forced via `TIM3->EGR = TIM_EGR_UG`, and the counter register `TIM3->CNT` is checked and reset if $\text{CNT} \ge \text{ARR}$ to prevent counter rollover delays.

---

### 8. How does the Everest Semiconductor ES8388 Stereo Codec communicate with the STM32F407?
**Answer**:  
The ES8388 uses two separate communication protocols:
1. **Control Interface (Software I2C on `PF0`/`PF1`)**:
   - Used during initialization to configure chip power registers (`0x01`/`0x02`), set 16-bit I2S audio format (`0x17`), configure mixer and analog volume gain (`0x2E`/`0x2F`), and unmute the DAC. Operates at $100\text{ kHz}$ with repeated-start ACK verification.
2. **Audio Data Interface (Hardware I2S3)**:
   - Receives continuous high-speed digital PCM audio streams:
     - `PC7` (`I2S3_MCK`): Master Clock ($256 \times F_s$).
     - `PA15` (`I2S3_WS`): Word Select / Left-Right Frame Clock ($44.1\text{ kHz}$).
     - `PB3` (`I2S3_CK`): Continuous Bit Clock.
     - `PB5` (`I2S3_SD`): Serial Audio Data.

---

### 9. How was the exact 44.1 kHz audio sampling rate derived from the MCU Phase-Locked Loop (`PLLI2S`)?
**Answer**:  
The standard system PLL cannot generate $44.1\text{ kHz}$ without severe fractional jitter. The STM32F407 provides a dedicated audio PLL (`PLLI2S`):
- From the $8\text{ MHz}$ HSE crystal, using $\text{PLLM} = 8$:
  $$f_{\text{VCO\_IN}} = \frac{8\text{ MHz}}{8} = 1.0\text{ MHz}$$
- With multiplier $\text{PLLI2SN} = 271$:
  $$f_{\text{VCO}} = 1.0\text{ MHz} \times 271 = 271.0\text{ MHz}$$
- With divisor $\text{PLLI2SR} = 2$:
  $$f_{\text{I2SxCLK}} = \frac{271.0\text{ MHz}}{2} = 135.5\text{ MHz}$$
- In the SPI3/I2S3 peripheral register (`SPI3->I2SPR`), setting $\text{MCKOE} = 1$ and $\text{I2SDIV} = 6$ ($12 \times \text{prescaler}$):
  $$F_s = \frac{135.5\text{ MHz}}{256 \times 12} = \mathbf{44108.07\text{ Hz}} \approx 44.1\text{ kHz}$$
  This clock yields a negligible frequency error of $0.018\%$, entirely imperceptible to human hearing.

---

### 10. What caused the 3.5mm headphone crackling when SDIO/USB were introduced, and how did Circular DMA fix it?
**Answer**:  
Originally, I2S audio samples were transmitted via CPU interrupts (`SPI3_IRQn`). On every 16-bit word transmitted ($88,200\text{ interrupts/sec}$), the CPU had to enter an ISR within an $11.3\text{ }\mu\text{s}$ deadline.

When 4-bit SDIO and USB were enabled, their high-priority DMA streams and bus interrupts occasionally delayed the CPU by $5\text{--}10\text{ }\mu\text{s}$. This caused `SPI3` TX FIFO underflows, transmitting zeroes and creating audible popping and crackling.

**The Fix**:
We migrated I2S audio transmission to **Hardware Circular DMA (DMA1 Stream 5 Channel 0)** with a two-tier double buffer:
- `audio_producer_thread` pre-fills 256-word PCM blocks in the background.
- DMA1 Stream 5 autonomously feeds `SPI3->DR`.
- The CPU only receives an interrupt when half of the DMA buffer ($5.8\text{ ms}$ of audio) is transferred, running a sub-microsecond `memcpy`. This reduced audio interrupt frequency from $88,200\text{ Hz}$ to **$86\text{ Hz}$**, completely eliminating crackling and jitter.

---

### 11. How does the RealNetworks Helix MP3 decoder operate on an ARM Cortex-M4 microcontroller?
**Answer**:  
The RealNetworks Helix MP3 decoder is a specialized, open-source MPEG audio decoder designed specifically for resource-constrained embedded systems:
- It uses **32-bit fixed-point integer arithmetic** (Q-format math) rather than floating-point math, utilizing the Cortex-M4's single-cycle multiply-accumulate (MAC) assembly instructions.
- It requires no external hardware decoding chip (such as a VS1053), decoding frames into raw 16-bit signed stereo PCM directly into RAM.
- `wav_player.c` streams these decoded frames into the double-buffer pipeline, providing full MP3 playback with only $15\%\text{ CPU utilization}$.

---

### 12. Why did reading 16 KB from the SD card trigger `got -1` (`-FR_DISK_ERR`), and how was it solved?
**Answer**:  
The STM32F407 SDIO peripheral hardware lacks hardware flow control. When requesting large multi-sector transfers ($16\text{ KB}$) across non-contiguous cluster boundaries at high bus speeds, the internal SDIO FIFO overran while the MCU serviced other interrupts, triggering a hardware `RXOVERR` flag that FatFs translated to `FR_DISK_ERR` (`-1`).

**The Solution**:
1. Enabled DMA2 Stream 3 (RX) and Stream 6 (TX) with `STM32_DMA_FIFO_FULL` in the device tree overlay.
2. Throttled the SDIO clock divider to `clk-div = <10>` ($4.0\text{ MHz}$).
3. Routed all FatFs file reads through a dedicated 4-byte aligned $2048\text{ B}$ bounce buffer (`s_read_bounce __attribute__((aligned(4)))`). 
By breaking transfers into aligned 2048-byte blocks, FIFO overruns were completely eliminated.

---

### 13. How does the system switch seamlessly between the 8 classical synthesizer songs and Micro-SD files?
**Answer**:  
In `src/threads.c` and `src/player_logic.c`, the track index space is dynamically partitioned:
- Total playable tracks is defined as:
  $$\text{Total Tracks} = 8\text{ (Classical Songs)} + N_{\text{SD\_Tracks}}$$
- Tracks `0` to `7` map directly to the classical repertoire (*Für Elise*, *Canon in D*, *Turkish March*, etc.) and are synthesized using hardware timer PWM and DDS.
- Tracks `8` through `(8 + N - 1)` map to files discovered by `sd_card_reader.c` on `/SD:`.
- When an SD track is active, `wav_player_start()` is invoked, handing audio generation over to the WAV/MP3 streaming pipeline while suspending the timer synthesizer.

---

### 14. What does your automated unit test suite verify, and why was it executed on MinGW native?
**Answer**:  
The unit test suite consists of **25 automated tests** compiled with Unity on host MinGW GCC via `pio test -e native`:
- **Decoupled Architecture**: All decision logic is isolated in `src/player_logic.c` without hardware register dependencies.
- **Coverage**:
  1. Binary Button Decoding: 8 tests verifying all permutations ($000_2$ to $111_2$).
  2. 5-Second Confirmation Window: 4 tests verifying elapsed intervals, exact expiration, and 32-bit rollover.
  3. State Machine Transitions: 5 tests verifying valid transitions between `STOPPED`, `PLAYING`, and `PAUSED`.
  4. Volume Normalization: 5 tests verifying boundary limits ($0\%$ and $100\%$) and ADC noise clamping.
  5. Catalog Lookup: 3 tests verifying metadata retrieval and modulo wrap-around.
- **Why Host-Native?** Executing unit tests on the host PC allows automated continuous integration (CI) execution in under 1 second without requiring physical hardware or JTAG debuggers connected.

---

### 15. How does the system conserve power when no song is playing?
**Answer**:  
The system enforces power conservation at multiple architectural levels:
1. When stopped or paused, the audio synthesizer disables timer PWM output (`TIM3->CCR3 = 0, TIM3->CCR4 = 0`) and sets DAC1 mid-rail bias.
2. In the main application loop, rather than executing a busy `while(1)` spinlock, the processor enters the Zephyr idle loop (`k_cpu_idle()` / `__WFI()`), placing the Cortex-M4F into low-power sleep mode until the next hardware timer or button interrupt fires.
3. Inactive threads remain in the **Blocked** state, consuming zero CPU execution cycles until their scheduled sleep intervals expire.
