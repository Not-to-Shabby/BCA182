# Project 1 — Design Document
## Smart Baby Monitoring System on RT-Thread RT-Spark (STM32F407ZGT6, Zephyr RTOS)

**Course:** BCA182 Embedded Systems Programming (MSU-IIT) · 100 pts
**Platform:** RT-Thread RT-Spark (Spark-1, HW rev 1.1), Zephyr RTOS, PlatformIO
**Companion documents:** [project-plan.md](project-plan.md) (decisions, architecture, phase plan, risks) · [BCA182 - Project 1.pdf](BCA182%20-%20Project%201.pdf) (course spec)
**Student framework note:** this project deliberately uses **Zephyr RTOS** instead of
the spec's STM32Cube HAL + FreeRTOS mandate. Rationale, the FreeRTOS↔Zephyr
equivalence table, and the accepted rubric risk are documented in
[Section 7](#7-rtos-deviation-rationale).

> **Document status:** skeleton. Each section carries stub text and `TODO(phase)`
> markers that are resolved as the corresponding phase of
> [project-plan.md §5](project-plan.md#5-phase-plan) completes.

---

## 1. Objective

Build an IoT baby-monitoring prototype that monitors room temperature, humidity, and
ambient light, detects likely baby-cry events from the onboard microphone with an
on-device ML classifier, and delivers acknowledged, non-flooding alerts to a
caregiver's Android phone over a Wi-Fi → MQTT → backend path. The device must
degrade gracefully — offline event queueing, sensor-absent boot, network-fault
states — and must *measure*, not assume, its RAM, timing, and latency budgets. It is
an educational prototype only: it makes no medical or infant-safety claims (see §8).

**TODO(P7/P8):** record final acceptance evidence here (classifier metrics, capture-
to-notification latency, resource measurements) against the spec's Testing and
Evaluation table.

## 2. System Architecture

Three cooperating subsystems: the **device** (RT-Spark under Zephyr, running threads
for sensing, audio capture, cry inference, alerting, UI, networking, and logging),
the **backend** (Mosquitto broker + FastAPI service over SQLite on a LAN host), and
the **Android app** (live dashboard, cry notifications, searchable history,
acknowledgement). The device publishes compact JSON readings/events over MQTT; the
backend ingests, debounces, and stores them; the app polls REST endpoints and POSTs
acknowledgements back to the device. Full ASCII diagram, topic definitions, and
offline-replay behavior: [project-plan.md §2](project-plan.md#2-system-architecture).

**TODO(P4):** final MQTT topic/payload schemas. **TODO(P5):** REST endpoint table.

## 3. Component Inventory & Justification

Master inventory with per-component justification:
[project-plan.md §3](project-plan.md). Two schematic findings shape it:

- **No external microphone is needed.** The spec assumes "an external microphone or
  audio-capture module, since microphone input is not identified as an integrated
  resource of the reference Spark-1 board." Schematic sheets 9/10 contradict this
  for HW rev 1.1: the onboard electret M1 (BA012AP42) is wired MIC_P/MIC_N into the
  ES8388's differential LIN1/RIN1 inputs (pins 24/23), so baby-cry audio is captured
  by hardware already on the board. The external-module justification in the
  inventory is therefore "none — onboard mic per schematic sheet 9/10."
- **W25Q128 SPI flash is excluded.** It sits on SPI2 (CS PB12), which the RW007
  Wi-Fi driver owns; driving both would create a bus conflict. Per the spec's
  "explain any exclusion in a component-inventory table" rule it is listed as
  present-but-unused with exactly this reason.

**TODO(P0):** freeze the full pin map / board manifest table here once the board
overlay lands.

## 4. Task/Thread Design

Zephyr threads with fixed priorities are the direct analog of the spec's FreeRTOS
task table (concept map in §7). The planned decomposition — SensorTask (2 s
periodic), AudioTask (I2S DMA capture → features), inference path, AlertTask
(thresholds + one-shot-ack state machine), NetTask (RW007/MQTT + resilience),
UiTask (LCD), LogTask (SD event queue) — is sketched in
[project-plan.md §2](project-plan.md#2-system-architecture).

| Thread | Priority | Stack | Period/Trigger | IPC in | IPC out |
|---|---|---|---|---|---|
| *(placeholder — finalized with measured stacks in P1)* | | | | | |

**TODO(P1):** the real table with priorities, stack sizes, periods, and per-thread
IPC; **TODO(P7):** estimated stacks replaced by measured high-water marks.

## 5. Communication Design

The same construct family the spec names for FreeRTOS — queues, semaphores,
mutexes, task notifications — mapped onto Zephyr primitives: `k_msgq` for
sensor/latest-value and event hand-off, `k_sem` for the I2S DMA half-buffer
handoff, `k_mutex` around the shared I2C bus and the SD/FatFs mount, `k_event` for
alert/acknowledge state. Prior art for each pattern (Lab 2 Side_Quest DMA `k_sem`
handoff, Lab 4 data logger) is listed in [project-plan.md §4](project-plan.md).

**TODO(P1/P2):** one row per primitive — what it protects, producer, consumer,
blocking policy, and the interrupt-to-task coordination path (DMA ISR → `k_sem`).

## 6. Data Flow

Sensors + mic → device tasks → MQTT `babymonitor/reading` and `babymonitor/event`
(JSON) → backend subscriber → SQLite → REST API → Android poll; a confirmed cry
becomes an unacked backend event → app notification → ack POST → device clears its
local alert. When offline, events queue to microSD with RTC-relative timestamps and
replay on reconnect. The detailed flow diagram lives in
[project-plan.md §2](project-plan.md#2-system-architecture).

**TODO(P4/P5):** payload examples and the backend debounce rules.
**TODO(P7):** measured capture-to-notification latency replaces the target here.

## 7. RTOS Deviation Rationale

The project sheet prescribes STM32Cube HAL + FreeRTOS, and this submission uses
**Zephyr RTOS** (`framework = zephyr`) instead. This section documents that
deviation for the grader — what the spec literally bans, why Zephyr is the
lower-risk engineering route on this board, what of the prescribed discipline is
preserved, and what risk we knowingly accept. It follows the same
documented-deviation pattern already used in Laboratory 4
(`LAB_4_Appliance_Monitor/docs/design.md` §8).

**Zephyr is not on the ban list, but the toolchain mandate is real.** The spec's
hard ban is explicit and bounded: *"Do not use RT-Thread Studio, RT-Thread OS,
Arduino, ESP-IDF, or an RT-Thread-dependent driver stack in your final firmware"*
(spec p. 2). Zephyr appears nowhere in that list. However, the Required Toolchain
section goes further and prescribes the STM32Cube route — *"the framework setting
is `framework = stm32cube`, not `framework = freertos`. FreeRTOS must be included
as kernel/middleware within the STM32Cube-based project"* — and rubric line 2
awards 25 pts for *"STM32Cube HAL + FreeRTOS firmware."* Choosing Zephyr therefore
complies with the letter of the ban list while knowingly departing from the
prescribed toolchain; this section exists precisely to make that departure visible
and defensible rather than hidden.

**Why Zephyr is the lower-risk route here.** The decisive technical dependency is
Wi-Fi. The only working driver for the RW007 module anywhere in this codebase is
the native Zephyr driver written and graded in Laboratory 3 on this exact board and
PlatformIO setup, and the spec itself warns that *"RT-Thread Wi-Fi packages cannot
be used unchanged"* under STM32Cube and demands verified TCP/IP capability before
MQTT may even be selected. Zephyr additionally ships the entire upper stack out of
the box — sockets, DHCPv4, DNS resolver, and `CONFIG_MQTT_LIB` — none of which
exists in our STM32Cube-based codebase (Lab 1 has only USART telemetry). Hand-porting
the RW007 driver and a TCP/IP stack to bare STM32Cube HAL would concentrate the
project's largest schedule and correctness risk in exactly the subsystem the
connectivity/app rubric line (20 pts) depends on; Zephyr eliminates that risk, so
the trade is made deliberately and documented.

**What is preserved.** The engineering substance the firmware rubric line assesses —
correctly scheduled concurrent threads, documented synchronization, local
status/controls, and *measured* memory and timing — is delivered natively by Zephyr
and evidenced the same way: the thread table (§4), the IPC design (§5), and
per-thread stack high-water marks, heap use, and worst-case latency measured in P7.
All decision logic (thresholds, hysteresis, alert/ack state machine, the cry-event
confirmation rule) is kept as pure C with no RTOS includes and is unit-tested on
the host with Unity (`pio test -e native`), so the decision layer is RTOS-agnostic
and would port to FreeRTOS unchanged. The equivalence table below maps every
FreeRTOS construct the spec names to the Zephyr primitive this project uses:

| FreeRTOS construct (spec) | Zephyr equivalent (this project) |
|---|---|
| Task (`xTaskCreate`) | Thread (`k_thread_create`) |
| Queue (`xQueueCreate`/`xQueueSend`) | Message queue (`k_msgq`) |
| Mutex (`xSemaphoreCreateMutex`) | Mutex (`k_mutex`) |
| Semaphore (binary/counting) | Semaphore (`k_sem`) |
| Event group (`xEventGroupCreate`/`xEventGroupSetBits`) | Event (`k_event`) |
| Periodic task (`vTaskDelayUntil`) | Sleep on absolute deadline (`k_sleep` + `k_uptime_deadline`) |
| Software timer | `k_timer` |

**Accepted risk.** We accept the documented risk of partial or total loss of the
25-point *"STM32Cube HAL + FreeRTOS firmware"* rubric line in exchange for removing
the networking risk, consistent with the deviation approach already used in
Laboratory 4 and pre-declared in [project-plan.md §1.1](project-plan.md) (risk R1).
The alternative — STM32Cube + FreeRTOS with no proven TCP/IP path and an
unproven RW007 port — gambles the entire 20-point backend/connectivity/app line on
a bring-up with no prior art. The deviation is deliberate, visible, and defensible;
the thread and IPC design in §4–§5 demonstrates that the FreeRTOS *engineering*
requirements are met on an equivalent kernel.

## 8. Safety & Privacy Limitations

This is a classroom prototype and cannot replace direct adult supervision or a
certified infant-monitoring/medical device; it must never be presented as one. The
proximity reading is used only as a local placement/obstruction indicator — never as
evidence of infant presence — and the ICM20608 senses the *device's own* motion,
not the infant's; no breathing, movement, or medical-diagnosis claims are made
(spec pp. 3/6 forbid exactly these). Audio privacy: by default only bounded feature
windows are processed and no continuous identifiable audio is stored or streamed;
user-triggered WAV evidence captures stay on the local SD card and are documented.
Transport: the baseline is LAN-only MQTT without TLS — a documented trade-off
(MbedTLS RAM cost on the F407 to be measured; TLS is a stretch goal) — backend
secrets live in a gitignored `.env`, and no credentials are committed.

**TODO(P3):** dataset provenance/license table (donateacry-corpus ODbL 1.0; ESC-50
and UrbanSound8K per their published terms) and the audio-retention statement.
**TODO(P7/P8):** finalize this section with measured evidence and the app-side
disclaimer placement list (dashboard footer, notification subtext, first-launch
dialog, History/Settings footers).
