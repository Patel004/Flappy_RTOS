# Flappy Bird PSoC6 — Session Notes

This file documents every non-trivial change made during the debugging sessions,
why each change was needed, and a complete inventory of every file in the project.
Written for future reference when revisiting the code or explaining it.

---

## Part 1: Change Audit

### 1. source/FreeRTOSConfig.h

#### 1a. Heap allocation scheme preprocessor guard

**What changed:** Added five `HEAP_ALLOCATION_TYPEn` integer constants and set
`configHEAP_ALLOCATION_SCHEME = HEAP_ALLOCATION_TYPE4`.

```c
#define HEAP_ALLOCATION_TYPE1  (1)
#define HEAP_ALLOCATION_TYPE2  (2)
#define HEAP_ALLOCATION_TYPE3  (3)
#define HEAP_ALLOCATION_TYPE4  (4)
#define HEAP_ALLOCATION_TYPE5  (5)
#define NO_HEAP_ALLOCATION     (0)
#define configHEAP_ALLOCATION_SCHEME  HEAP_ALLOCATION_TYPE4
```

**Why:** The ModusToolbox BSP wraps the FreeRTOS heap source files inside a
preprocessor guard: `heap_4.c` only compiles if
`configHEAP_ALLOCATION_SCHEME == HEAP_ALLOCATION_TYPE4`. Without defining these
named constants, the comparison is between an integer and an undefined symbol,
which either fails with a compile error or (worse) silently compiles the wrong
heap file. heap_4 was chosen because it supports both allocation and freeing, and
coalesces adjacent free blocks — appropriate for a system that creates tasks and
queues at startup and never tears them down.

#### 1b. Vector table handler name remapping — the critical fix

**What changed:** Added three `#define` lines at the bottom of the file:

```c
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler
```

**Why:** This was the root cause of FreeRTOS never running at all.

The GCC ARM startup file (`startup_psoc6_02_cm4.S`) builds the Cortex-M4 vector
table. Each exception slot is a `.weak` alias pointing to `Default_Handler`, which
is an infinite `for(;;){}` loop. The startup file creates weak symbols named
`SVC_Handler`, `PendSV_Handler`, and `SysTick_Handler`.

The FreeRTOS ARM-CM4F port (`portable/GCC/ARM_CM4F/port.c`) defines three
functions: `vPortSVCHandler`, `xPortPendSVHandler`, and `xPortSysTickHandler`.
These names do NOT match the weak symbol names in the vector table.

When the linker resolves `SVC_Handler`, it finds the weak alias pointing to
`Default_Handler` (because nothing provides a strong `SVC_Handler`). The FreeRTOS
port functions exist in the binary but are never called — they are orphaned code.

When `vTaskStartScheduler()` triggers the SVC instruction to launch the first
task, the processor jumps to `Default_Handler` and hangs. No task ever runs.

The `#define` trick works because `port.c` uses the macro names. With the defines
in place, `vPortSVCHandler` in `port.c` textually becomes `SVC_Handler` before the
compiler sees it. The linker then sees a strong definition of `SVC_Handler` from
the port file, which overrides the weak alias. The vector table now points to the
FreeRTOS implementation.

All three handlers must be remapped. Missing PendSV causes the task switcher to
hang. Missing SysTick causes `vTaskDelay` and timeouts to never fire.

#### 1c. Interrupt priority configuration

**What changed:** Set the FreeRTOS interrupt priority boundary for PSoC6.

```c
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         7
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    4
#define configKERNEL_INTERRUPT_PRIORITY   (7 << (8 - 3))  /* = 0xE0 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (4 << (8 - 3)) /* = 0x80 */
```

**Why:** The PSoC6 Cortex-M4 uses 3 priority bits, giving 8 levels (0 = highest,
7 = lowest). FreeRTOS needs a boundary: ISRs above `MAX_SYSCALL_INTERRUPT_PRIORITY`
(numerically lower, meaning higher urgency) must never call FreeRTOS API. ISRs at
or below the boundary may use `FromISR` variants.

The ARM NVIC register stores priorities left-justified in an 8-bit field, so a
raw 3-bit priority p maps to register value `p << (8 - 3) = p << 5`. Setting
`MAX_SYSCALL = 4` reserves priorities 0–3 for time-critical ISRs that bypass
FreeRTOS entirely, and allows priorities 4–7 to safely use `xEventGroupSetBitsFromISR`,
`xQueueSendFromISR`, etc.

The SW2 GPIO interrupt is registered at `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`
(4) so it can call `xEventGroupSetBitsFromISR`.

#### 1d. Heap size and additional options

**What changed:** `configTOTAL_HEAP_SIZE = 24 * 1024` (24 KB). Added
`configUSE_EVENT_GROUPS = 1`, `configUSE_MUTEXES = 1`,
`configSUPPORT_STATIC_ALLOCATION = 1` (needed by `clib-support`).

**Why:** Three tasks with stack sizes of 512–768 words (2–3 KB each) plus queue
and event group storage requires roughly 15–18 KB. 24 KB gives comfortable margin.
Event groups must be explicitly enabled in this port version. Static allocation
support is required by Infineon's `clib-support` library which provides FreeRTOS
implementations of `vApplicationGetIdleTaskMemory` and `vApplicationGetTimerTaskMemory`.

---

### 2. Makefile

#### 2a. TARGET board switch

**What changed:** `TARGET=APP_CY8CKIT-062-WIFI-BT` → `TARGET=APP_CY8CPROTO-062-4343W`

**Why:** The project was scaffolded for the CY8CKIT-062-WiFi-BT evaluation kit, but
the actual hardware is the CY8CPROTO-062-4343W prototype kit. These are different
boards: different header pin assignments, different system clock configuration
files, and a different CM0+ firmware image. The `TARGET` line tells the build
system which BSP folder (`bsps/TARGET_APP_*`) to use. With the wrong BSP, macros
like `CYBSP_I2C_SDA`, `CYBSP_USER_BTN`, and `CYBSP_CSD_HW` either resolve to wrong
pin numbers or fail to compile because they are not defined for that device.

The CY8CPROTO-062-4343W uses CY8C624ABZI-S2D44 (PSoC62, Cortex-M4F at up to
150 MHz, 2 MB flash, 1 MB SRAM). The I2C pins are P6.0 (SCL) and P6.1 (SDA),
same physical location on both boards but the BSP header maps them correctly only
when the right TARGET is selected.

#### 2b. CapSense entries (added then removed)

CapSense was integrated to use the on-board capacitive touch buttons. The Makefile
received `CAPSENSE_ROOT`, `SOURCES`, `INCLUDES`, and `LDLIBS` pointing to
`mtb_shared/capsense/release-v4.0.0`. This was later removed because the CapSense
hardware consistently reported all widgets as active (false positives) regardless
of debounce count, likely a threshold calibration issue that requires the CapSense
Tuner tool to diagnose. The build now has `SOURCES=`, `INCLUDES=`, `LDLIBS=` empty.

---

### 3. source/main.c

Written from scratch as the full application. Key decisions:

#### 3a. Three-task architecture

| Task | Priority | Stack | Period | Responsibility |
|---|---|---|---|---|
| vSensorTask | 3 (highest) | 768 words | 50 ms fixed | Read MPU9250 gyro, post to queue |
| vGameTask | 2 | 512 words | dynamic (220–80 ms) | Game state machine, post render commands |
| vRenderTask | 1 (lowest) | 768 words | event-driven | Draw LCD frames |

vSensorTask uses `vTaskDelayUntil` (not `vTaskDelay`) to maintain a deterministic
50 ms period regardless of how long the gyro I2C transaction takes. `vTaskDelay`
adds delay after execution completes; `vTaskDelayUntil` maintains a fixed wake time
regardless of execution time — this is the correct tool for periodic sensor tasks.

#### 3b. I2C mutex

`xI2CMutex` (SemaphoreHandle_t, created with `xSemaphoreCreateMutex`) protects the
shared I2C bus. Both vSensorTask and vRenderTask use the same physical I2C peripheral
(P6.0/P6.1): vSensorTask reads the MPU9250, vRenderTask writes to the PCF8574 LCD
expander. Without the mutex, vSensorTask (higher priority) can preempt vRenderTask
in the middle of a multi-byte LCD write sequence. The PCF8574 writes individual
nibbles to the LCD; an interrupted write leaves the LCD in an undefined state,
causing display corruption and score truncation for multi-digit numbers.

Every call to `cyhal_i2c_master_write` or `cyhal_i2c_master_read` is bracketed by
`xSemaphoreTake(xI2CMutex, portMAX_DELAY)` and `xSemaphoreGive(xI2CMutex)`.

#### 3c. SCB interrupt priority adjustment

After I2C init, the SCB interrupt priority defaults to 3 (hardware default for
PSoC6 SCB blocks). This is numerically above (higher urgency than) the FreeRTOS
`MAX_SYSCALL_INTERRUPT_PRIORITY` of 4. If vRenderTask blocks on the I2C write and
the SCB completion ISR fires, any attempt to interact with FreeRTOS internals from
that ISR would violate the priority mask and cause a fault. The fix:

```c
cyhal_i2c_enable_event(&g_i2c, CYHAL_I2C_MASTER_WR_CMPLT_EVENT,
                       configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, false);
```

This re-registers the event at priority 4 (FreeRTOS-safe) and disables the
callback (false = not enabled as an actual callback, but this is the API to
reconfigure the interrupt priority).

#### 3d. Queue design

`xGyroQueue`: depth 1, written with `xQueueOverwrite`. This is a "latest value"
queue — the game task always gets the most recent gyro reading; stale samples are
discarded. There is no risk of the queue filling up.

`xRenderQueue`: depth 3. Absorbs the sequence of splash → game-over → next-game
transitions without losing messages if the render task is briefly held off by a
higher-priority task.

#### 3e. Game over restart flow and collision cooldown

After a collision: the game sends `RENDER_GAME_OVER`, delays 1000 ms, clears the
event group, drains the gyro queue, then blocks on `EVENT_RESET` indefinitely.

The 1-second cooldown is necessary because the same tilt that causes the collision
(exceeding `GYRO_THRESHOLD = 3000 LSB`) would immediately satisfy the restart
condition (`RESTART_GYRO_THR = 2000 LSB`). Without the cooldown, the game restarts
before the player can see the game over screen.

#### 3f. score_buf size fix

`score_buf` was originally 8 bytes. `snprintf(buf, 8, "Score:%d", score)` writes
at most 7 characters + null. "Score:" is 6 characters, leaving 1 character for the
number, so scores >= 10 were truncated to their first digit ("Score:1" for score 12).
Fixed by increasing to 12 bytes.

---

### 4. libs/mtb.mk

Generated by `make getlibs`. Modified to add CapSense build integration (then
reverted). Currently free of CapSense entries. This file should not be manually
edited — run `make getlibs` to regenerate it after adding entries to `deps/`.

---

### 5. source/game.c and source/game.h

Game state machine. The 16x2 LCD has 2 rows and 16 columns. The bird occupies one
cell (column 2, row 0 or 1). A pipe is a column of cells with one gap row.

`game_update_bird`: if gyro GZ > 3000 LSB, move bird up (row 0); if GZ < -3000,
move bird down (row 1). At ±250°/s full scale, 1 LSB = 1/131 °/s, so 3000 LSB ≈
23°/s — a deliberate tilt, not noise.

`game_tick`: advances the pipe one column per call. When pipe reaches column 2
(bird column), checks if the bird row overlaps a pipe block. If the pipe passes
column 0 without collision, score increments. Difficulty: every 5 points, tick_ms
decreases by 20 ms (minimum 80 ms).

---

### 6. source/mpu9250.c and source/mpu9250.h

Minimal MPU9250 gyroscope driver over I2C (address 0x68, AD0 tied low). Reads
WHO_AM_I register (0x75) on init — MPU9250 returns 0x71, MPU6500 returns 0x70,
both accepted. Gyro configured to ±250°/s (GYRO_FS_250 = 0x00 in GYRO_CONFIG).
`mpu9250_read_gyro` reads 6 consecutive bytes from 0x43 (GYRO_XOUT_H) and
assembles three signed 16-bit big-endian values.

---

### 7. source/LCD_I2C/lcd_i2c.c and source/LCD_I2C/lcd_i2c.h

HD44780 16x2 LCD driver using a PCF8574 I2C port expander backpack. Communicates
at I2C address 0x27 (default for most PCF8574 backpacks; solder bridges select
0x20–0x27). Operates the LCD in 4-bit mode. Custom characters are loaded into
CGRAM slots 0 (bird sprite) and 1 (solid pipe block) during init.

---

## Part 2: File Inventory

### Application source (source/)

| File | What it does |
|---|---|
| source/main.c | Full application: FreeRTOS init, three tasks, I2C setup, SW2 ISR, game event loop |
| source/FreeRTOSConfig.h | FreeRTOS kernel configuration: heap scheme, interrupt priorities, handler remaps, feature enables |
| source/game.c | Game state machine: bird movement, pipe logic, scoring, difficulty scaling |
| source/game.h | Public API and constants for game.c (thresholds, timing, state enum) |
| source/mpu9250.c | MPU9250 I2C gyroscope driver: init, WHO_AM_I check, raw gyro read |
| source/mpu9250.h | Public API for mpu9250.c: address defines, register addresses, gyro_data_t struct |
| source/LCD_I2C/lcd_i2c.c | PCF8574-backed HD44780 LCD driver: 4-bit mode, custom chars, cursor, print |
| source/LCD_I2C/lcd_i2c.h | Public API for lcd_i2c.c: lcd_init, lcd_print, lcd_set_cursor, lcd_write_char |

### Build configuration

| File | What it does |
|---|---|
| Makefile | Top-level build file: TARGET board, TOOLCHAIN, COMPONENTS (FREERTOS), empty SOURCES/INCLUDES/LDLIBS |
| libs/mtb.mk | Generated by make getlibs: lists all library search paths and COMPONENTS for the build system |
| libs/*.mtb | One-line URL/commit/path pointers used by make getlibs to download each library |
| libs/app.mk | Generated application include make file |
| deps/freertos.mtb | Dependency pointer for FreeRTOS library |
| deps/retarget-io.mtb | Dependency pointer for UART retarget-io library |
| deps/assetlocks.json | Locks library versions to specific commits (prevents silent version changes) |

### BSP (board support packages, bsps/)

| Directory | What it does |
|---|---|
| bsps/TARGET_APP_CY8CPROTO-062-4343W/ | Active BSP for the actual hardware: pin macros, clock config, CM0+ image, device configurator output |
| bsps/TARGET_APP_CY8CPROTO-062-4343W/config/design.modus | Device Configurator project: pin assignments, peripheral settings |
| bsps/TARGET_APP_CY8CPROTO-062-4343W/config/GeneratedSource/ | Auto-generated C source from design.modus: cycfg_pins.c, cycfg_clocks.c, cycfg_peripherals.c, etc. |
| bsps/TARGET_APP_CY8CPROTO-062-4343W/bsp.mk | Per-BSP make include: selects the correct device and linker script |
| bsps/TARGET_APP_CY8CKIT-062-WIFI-BT/ | Unused BSP from the original project scaffold (wrong board) — kept for reference |

### IDE and tooling

| File | What it does |
|---|---|
| .vscode/ | VS Code workspace settings: IntelliSense config, launch/debug configs for KitProg3 |
| openocd.tcl | OpenOCD target configuration used by the debug launch config |
| .clangd | clangd language server config: points to compile_commands.json in build/ |
| .clang-format | Code style config for the clangd formatter |
| .mtbLaunchConfigs/ | Eclipse/ModusToolbox debug and programming launch configurations |
| project_info.json | ModusToolbox project metadata (board, device, template) |
| mtb-example-empty-app.code-workspace | VS Code multi-root workspace file |

### Other

| File | What it does |
|---|---|
| LICENSE | Apache 2.0 license (from the ModusToolbox example template) |
| README.md | Project description, hardware list, wiring, build instructions, debugging notes |
| NOTES.md | This file: change audit and file inventory |

---

## Part 3: Debugging Notes (Things That Went Wrong)

### Bug 1: FreeRTOS tasks never executed

**Symptom:** `printf` before `vTaskStartScheduler()` printed fine. Nothing after it
ever printed. Board appeared to hang silently.

**Root cause:** The `vPortSVCHandler` / `SVC_Handler` name mismatch described in
section 1b above. The SVC instruction fired `Default_Handler`, an infinite loop.

**Diagnosis path:** Confirmed the scheduler was returning (it wasn't). Suspected
heap allocation. Found the heap guard issue (fix 1a). Tasks still didn't run.
Looked at the startup file and found `.weak SVC_Handler Default_Handler`. Traced
through `port.c` and found `vPortSVCHandler` never matches. Added the three
`#define` lines. Tasks ran immediately after.

**Lesson:** When FreeRTOS appears to hang at `vTaskStartScheduler()`, the first
thing to check is handler name mapping. Any PSoC6 GCC ARM project with FreeRTOS
needs those three defines in FreeRTOSConfig.h.

### Bug 2: Wrong BSP — board not discovered

**Symptom:** Project compiled for the wrong target. Macros like `CYBSP_USER_BTN`
resolved to wrong pin numbers. USB connection was unreliable.

**Root cause:** `TARGET=APP_CY8CKIT-062-WIFI-BT` in Makefile. The actual board is
`CY8CPROTO-062-4343W`.

**Lesson:** When starting from a template example, always check the TARGET line in
the Makefile against the actual hardware label. The board name is printed on a
sticker on the PCB. The Makefile TARGET must match exactly.

### Bug 3: USB power-only cable

**Symptom:** Board connected via USB but no serial output, no programming, device
not detected.

**Root cause:** USB cable was a charge-only cable (power wires only, no data).
This is extremely common with cheap phone charger cables.

**Diagnosis:** `ls /dev/tty.*` showed no new device after plugging in. Swapped to
a known-good data cable. Device immediately appeared as `/dev/tty.usbmodem*`.

**Lesson:** Always test your USB cable with a known-good device first. Keep a
verified data cable labelled separately.

### Bug 4: Score display truncated to one digit

**Symptom:** Score displayed correctly up to 9. Score 10 showed as "1", score 12
showed as "1".

**Root cause:** `char score_buf[8]`. The format string `"Score:%d"` is 7 characters
before the number. `snprintf(buf, 8, ...)` writes at most 7 chars + null, leaving
no room for a two-digit number.

**Fix:** `char score_buf[12]`.

**Lesson:** When using snprintf with a fixed-size buffer, calculate the maximum
possible output length: `strlen("Score:") + strlen("999") + 1 = 10`. Always size
the buffer for the worst case.

### Bug 5: CapSense always reporting active (false positives)

**Symptom:** "CapSense touch" printed continuously from task start. Touch debounce
had no effect. Game restarted mid-play and immediately after game over.

**Root cause:** Default CapSense threshold calibration reported all widgets as
active immediately after `Cy_CapSense_Enable()`. This requires running the
CapSense Tuner application with the hardware connected to determine correct
finger/noise thresholds. Without tuning, the library fires false positives.

**Resolution:** Removed CapSense from the project entirely. SW2 hardware button
handles game reset.
