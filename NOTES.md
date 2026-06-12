# Flappy Bird PSoC6 — Development Log

Build log for a FreeRTOS Flappy Bird game on the CY8CPROTO-062-4343W kit.
Covers every non-obvious problem encountered, the root cause of each, and what
to watch for if you revisit this project or start a new PSoC6/FreeRTOS build.

---

## 2026-06-10

### Starting point: wrong BSP in the Makefile

The project was scaffolded from an Infineon template that targets the
CY8CKIT-062-WiFi-BT evaluation kit. The actual hardware is the
CY8CPROTO-062-4343W prototype kit. These are different boards with different
pin header assignments, different system clock configurations, and a different
CM0+ firmware image. The `TARGET` line in the Makefile selects which BSP folder
the build system uses. With the wrong target set, macros like `CYBSP_I2C_SDA`
and `CYBSP_USER_BTN` either resolve to the wrong physical pins or fail entirely
because they are not defined for that device.

Fix: changed one line in Makefile:

```
TARGET=APP_CY8CPROTO-062-4343W
```

The board name is printed on a sticker on the PCB. It has to match exactly.
When starting from any ModusToolbox template, verify this before writing any code.

---

### USB cable with no data lines

Board was plugged in and nothing happened: no serial output, no programming,
device not detected by the OS. Ran `ls /dev/tty.*` and the expected
`/dev/tty.usbmodem*` device was not there.

The cable was a phone charger cable — power wires only, no D+ or D-. Swapped
to a verified data cable and the device appeared immediately. This is a common
time-waster. Label one cable as trusted and use it consistently.

---

## 2026-06-10 — FreeRTOS bring-up

### Heap allocation scheme preprocessor trap

Added FreeRTOS and ran into a build issue specific to how ModusToolbox integrates
the heap management files. The BSP wraps each `heap_N.c` inside a preprocessor
guard like this:

```c
#if (configHEAP_ALLOCATION_SCHEME == HEAP_ALLOCATION_TYPE4)
```

For this comparison to resolve correctly, `HEAP_ALLOCATION_TYPE4` needs to be a
defined integer constant before the preprocessor sees it. Without the definitions,
the comparison is between an integer literal and an undefined symbol — depending
on compiler settings it either errors or silently picks the wrong heap file.

Fix: add the named constants to `FreeRTOSConfig.h` before setting the scheme:

```c
#define HEAP_ALLOCATION_TYPE1  (1)
#define HEAP_ALLOCATION_TYPE2  (2)
#define HEAP_ALLOCATION_TYPE3  (3)
#define HEAP_ALLOCATION_TYPE4  (4)
#define HEAP_ALLOCATION_TYPE5  (5)
#define NO_HEAP_ALLOCATION     (0)
#define configHEAP_ALLOCATION_SCHEME  HEAP_ALLOCATION_TYPE4
```

heap_4 was chosen because it supports both alloc and free and coalesces adjacent
free blocks. For a system that creates all tasks at startup and never destroys
them, heap_1 would also work, but heap_4 is more flexible if the design changes.

---

### FreeRTOS scheduler hangs — tasks never run

This was the hardest problem. Serial output before `vTaskStartScheduler()` worked
fine. Nothing printed after it. No task ever executed. The board appeared to hang
silently inside the scheduler.

Initial suspects: heap too small, wrong stack size, missing `configASSERT`.
Increasing the heap and stack sizes changed nothing.

The real cause: a name mismatch between the GCC ARM startup file and the FreeRTOS
Cortex-M4F port.

The PSoC6 startup file (`startup_psoc6_02_cm4.S`) builds the Cortex-M4 vector
table. Every exception handler slot that doesn't have a real implementation is
filled with a `.weak` alias pointing to `Default_Handler`, which is just
`for(;;){}`. The startup file creates these weak symbols by name:
`SVC_Handler`, `PendSV_Handler`, `SysTick_Handler`.

The FreeRTOS ARM-CM4F port (`portable/GCC/ARM_CM4F/port.c`) defines three
functions with different names: `vPortSVCHandler`, `xPortPendSVHandler`,
`xPortSysTickHandler`. These are compiled into the binary but the linker never
associates them with the vector table entries — it sees no strong definition of
`SVC_Handler` and leaves the weak alias in place, pointing to `Default_Handler`.

When `vTaskStartScheduler()` triggers the SVC instruction to switch to the first
task, the processor jumps to `Default_Handler` and loops forever. Every task is
blocked waiting for a scheduler that is stuck in an infinite loop.

The fix is three `#define` lines in `FreeRTOSConfig.h`:

```c
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler
```

Because `port.c` uses the macro names when defining its functions, the preprocessor
textually replaces them before compilation. The compiler then sees a function named
`SVC_Handler`, which is a strong symbol that overrides the weak alias. The vector
table is now wired to the FreeRTOS implementation.

All three handlers must be remapped. A missing PendSV stalls the context switcher.
A missing SysTick means `vTaskDelay` and all timeout-based blocking never fire.

This problem only appears when using GCC ARM with the Infineon startup file.
It will not be visible in a project that was already working, and it does not
produce any compile or link warning — everything builds cleanly and the mismatch
is invisible until runtime.

---

### Interrupt priority boundary for PSoC6

PSoC6 Cortex-M4 implements 3 NVIC priority bits, giving 8 levels (0 = highest
urgency, 7 = lowest). FreeRTOS requires knowing which priority level marks the
boundary between ISRs that may use FreeRTOS API and ISRs that must not.

The ARM NVIC stores priorities left-justified in an 8-bit register field, so a
3-bit raw priority `p` maps to register value `p << 5`. FreeRTOS compares the
raw register value, so the config macros need both the human-readable value
(for registering ISRs) and the shifted value (for the kernel):

```c
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    4   /* raw 0–7 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY  (4 << (8-3)) /* = 0x80, register format */
```

Setting the boundary at priority 4 reserves levels 0–3 for time-critical ISRs
that bypass FreeRTOS entirely. ISRs at levels 4–7 may safely call `FromISR`
variants (`xEventGroupSetBitsFromISR`, `xQueueSendFromISR`, etc.).

After I2C init, the PSoC6 SCB block's interrupt defaults to hardware priority 3,
which is above the boundary. Left uncorrected, any FreeRTOS API call from the
I2C completion ISR would violate the mask check and cause a fault. Fix:

```c
cyhal_i2c_enable_event(&g_i2c, CYHAL_I2C_MASTER_WR_CMPLT_EVENT,
                       configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, false);
```

This lowers the SCB interrupt to priority 4 (FreeRTOS-safe). The SW2 GPIO
interrupt is also registered at priority 4 so it can call
`xEventGroupSetBitsFromISR`.

---

## 2026-06-11 — Tasks running, building the game

### I2C bus contention between sensor and render tasks

vSensorTask (priority 3) reads the MPU9250 gyro. vRenderTask (priority 1) writes
to the PCF8574 I2C LCD expander. Both use the same physical I2C bus on P6.0/P6.1.

The PCF8574 LCD driver sends data as a series of nibble writes. If vSensorTask
preempts vRenderTask in the middle of a write sequence, the LCD receives a partial
write and corrupts the display. This showed up as garbled characters and — more
subtly — multi-digit scores being truncated to one digit on the game-over screen.
The game would show "Score:1" instead of "Score:12" because the write of "2" was
interrupted.

Fix: a FreeRTOS mutex wrapping every I2C call in both tasks.

```c
xSemaphoreTake(xI2CMutex, portMAX_DELAY);
/* cyhal_i2c_master_write / read call */
xSemaphoreGive(xI2CMutex);
```

Using a mutex rather than a binary semaphore matters here: a mutex carries
priority inheritance, which prevents priority inversion if vRenderTask holds
the lock while vSensorTask is trying to take it.

---

### Queue design for sensor → game data flow

Two queues carry data between tasks:

`xGyroQueue` has depth 1 and is written with `xQueueOverwrite`. The game task
always reads the most recent gyro sample; if a new sample arrives before the
game task processes the previous one, the old value is discarded. This keeps the
control latency low without any risk of the queue backing up.

`xRenderQueue` has depth 3. The extra depth absorbs the burst of render commands
that happen during state transitions (splash screen → game start → game over →
next game) without dropping messages if the render task is briefly delayed.

`vTaskDelayUntil` (not `vTaskDelay`) is used in vSensorTask to maintain a fixed
50 ms wake period. `vTaskDelay` measures from when the task finishes; 
`vTaskDelayUntil` measures from a fixed reference point. For periodic sensor
sampling the latter is correct — the period stays deterministic regardless of how
long the I2C transaction takes.

---

### Collision cooldown to prevent instant restart

After a collision, the game posts `RENDER_GAME_OVER` and then waits for the SW2
button (EVENT_RESET) before resetting. Without any delay, the same tilt that caused
the collision immediately satisfies the restart check — the player never sees the
game over screen.

`GYRO_THRESHOLD = 3000 LSB` moves the bird during gameplay.
`RESTART_GYRO_THR = 2000 LSB` was the threshold to re-enter the game from the
splash screen. Since 3000 > 2000, every collision tilt also triggered an instant
restart.

Fix: after collision, delay 1000 ms, clear the event group, drain the gyro queue,
then wait indefinitely for the SW2 button. Gyro tilt no longer triggers restart
from the game-over state.

---

### Score display truncated to one digit

Scores above 9 showed as only the first digit on the game-over screen.
"Score:12" appeared as "Score:1".

`score_buf` was declared as `char score_buf[8]`. The format string `"Score:%d"`
is 6 characters before the number. `snprintf(buf, 8, ...)` writes at most 7
characters plus a null terminator — one character for the number. Any score
≥ 10 was silently truncated.

Fix: `char score_buf[12]`. Rule: size snprintf buffers for the worst-case output,
not a typical one. For this format, worst case is `"Score:999\0"` = 10 bytes.

---

### CapSense false positives — abandoned

Attempted to use the on-board capacitive touch buttons (Button0, Button1,
LinearSlider0) to start and restart the game. The CapSense library compiled and
initialised without errors, but `Cy_CapSense_IsWidgetActive` returned true for
all widgets immediately after `Cy_CapSense_Enable()` and continued doing so
regardless of whether anything was being touched.

Increasing the debounce counter from 1 to 3 consecutive scans had no effect.
The library was calling widgets active on every single scan.

Root cause: CapSense threshold calibration. `Cy_CapSense_Enable()` runs an
automatic calibration pass, but the resulting thresholds were evidently too low
for this hardware — board capacitance, nearby components, or trace routing can
all require manual adjustment. The correct path is to run the CapSense Tuner
application (a ModusToolbox GUI tool) with the hardware connected, observe the
raw signal and noise floor, and set finger and noise thresholds manually in the
Device Configurator. This requires hardware access and is time-consuming to
iterate on.

Decision: removed CapSense entirely. The SW2 hardware push-button handles game
reset reliably. If capacitive touch is needed in a future version, use the
CapSense Tuner tool from the start rather than relying on auto-calibration.

---

## Architecture reference

### FreeRTOS task table

| Task | Priority | Stack (words) | Period | Responsibility |
|---|---|---|---|---|
| vSensorTask | 3 (highest) | 768 | 50 ms fixed | Read MPU9250 gyro, post to xGyroQueue |
| vGameTask | 2 | 512 | 220–80 ms dynamic | Game state machine, post render commands |
| vRenderTask | 1 (lowest) | 768 | event-driven | Draw LCD frames under I2C mutex |

### Inter-task communication

```
[MPU9250] --I2C (mutex)--> vSensorTask --xGyroQueue (depth 1, overwrite)--> vGameTask
                                                                                  |
                                                                      xRenderQueue (depth 3)
                                                                                  |
                                                                        vRenderTask --I2C (mutex)--> [LCD]

SW2 ISR --xEventGroupSetBitsFromISR--> xGameEvents (EVENT_RESET bit) --> vGameTask
```

---

## File inventory

### source/

| File | Purpose |
|---|---|
| main.c | FreeRTOS init, three tasks, I2C setup, SW2 ISR, game event loop |
| FreeRTOSConfig.h | Kernel config: heap scheme, interrupt priorities, handler name remaps |
| game.c | Bird movement, pipe scrolling, scoring, difficulty scaling |
| game.h | Public API and constants (GYRO_THRESHOLD, GAME_TICK_MS, game_state_t) |
| mpu9250.c | MPU9250 I2C driver: init, WHO_AM_I check, raw gyro read |
| mpu9250.h | MPU9250 API: I2C address, register addresses, gyro_data_t |
| LCD_I2C/lcd_i2c.c | PCF8574-backed HD44780 driver: 4-bit mode, CGRAM, cursor, print |
| LCD_I2C/lcd_i2c.h | LCD API: lcd_init, lcd_print, lcd_set_cursor, lcd_write_char |

### Build

| File | Purpose |
|---|---|
| Makefile | TARGET board, TOOLCHAIN, COMPONENTS (FREERTOS) |
| libs/mtb.mk | Generated by `make getlibs` — library search paths, do not edit manually |
| libs/*.mtb | URL/commit/path pointers for each library, consumed by `make getlibs` |
| deps/freertos.mtb | FreeRTOS library dependency pointer |
| deps/retarget-io.mtb | UART printf (cy_retarget_io) library pointer |
| deps/assetlocks.json | Locks library versions to specific commits |

### BSP

| Path | Purpose |
|---|---|
| bsps/TARGET_APP_CY8CPROTO-062-4343W/ | Active BSP: pin macros, clock config, CM0+ image, GeneratedSource |
| bsps/TARGET_APP_CY8CPROTO-062-4343W/config/design.modus | Device Configurator project file |
| bsps/TARGET_APP_CY8CPROTO-062-4343W/config/GeneratedSource/ | cycfg_pins.c, cycfg_clocks.c, etc. — generated from design.modus |
| bsps/TARGET_APP_CY8CKIT-062-WIFI-BT/ | Original template BSP (wrong board) — kept for reference |

### IDE and tooling

| File | Purpose |
|---|---|
| .vscode/ | IntelliSense config, KitProg3 debug and flash launch configs |
| openocd.tcl | OpenOCD target config used by the VS Code debug launcher |
| .clangd | Points clangd at build/compile_commands.json for accurate IntelliSense |
| .clang-format | Formatting rules for the clangd formatter |
| project_info.json | ModusToolbox metadata (board, device, template name) |
