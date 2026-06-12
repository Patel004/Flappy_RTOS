# Flappy Bird on PSoC6 with FreeRTOS

A real-time implementation of Flappy Bird running on a CY8CPROTO-062-4343W
development kit. The game renders on a 16x2 character LCD. The player tilts the
board to move the bird. Pipes scroll from right to left. The game speeds up as
the score increases.

This was built as an embedded systems project to demonstrate FreeRTOS task
scheduling, inter-task communication via queues and event groups, shared
peripheral access via mutex, and real-time periodic sensor sampling.

---

## Hardware

| Part | Description |
|---|---|
| CY8CPROTO-062-4343W | PSoC62 prototype kit, CY8C624ABZI-S2D44, Cortex-M4F + M0+, 2 MB flash, 1 MB SRAM |
| MPU9250 (or MPU6500) | 6-axis IMU on breakout board, used gyroscope only |
| 16x2 LCD with PCF8574 backpack | HD44780-compatible, I2C address 0x27 |
| USB data cable | Must be a data cable, not a charge-only cable |

---

## Wiring

The MPU9250 and LCD share the same I2C bus. The PSoC6 I2C pins are P6.0 (SCL)
and P6.1 (SDA). Both devices are connected in parallel to these two pins.

| Signal | PSoC6 Pin | Connected to |
|---|---|---|
| I2C SDA | P6.1 (CYBSP_I2C_SDA) | MPU9250 SDA, LCD backpack SDA |
| I2C SCL | P6.0 (CYBSP_I2C_SCL) | MPU9250 SCL, LCD backpack SCL |
| 3.3V | 3V3 header | MPU9250 VCC |
| 5V or 3.3V | 5V or 3V3 header | LCD backpack VCC (check your module) |
| GND | GND header | MPU9250 GND, LCD GND, MPU9250 AD0 |

MPU9250 AD0 must be tied to GND. This sets its I2C address to 0x68.
If AD0 floats or is pulled high, the address is 0x69 and the driver will not
find the device.

The on-board SW2 button (P0.4, CYBSP_USER_BTN) resets the game at any time.

---

## How it works

### FreeRTOS task architecture

Three tasks run concurrently. FreeRTOS preempts between them based on priority.

```
vSensorTask  (priority 3, 768 word stack)
  Runs every 50 ms using vTaskDelayUntil.
  Reads MPU9250 gyro over I2C (under mutex).
  Writes gz value to xGyroQueue (depth 1, overwrites stale samples).

vGameTask    (priority 2, 512 word stack)
  Runs at variable rate: starts at 220 ms per tick, speeds up to 80 ms.
  Reads xGyroQueue (non-blocking, uses last value if nothing new).
  Runs game state machine: move bird, advance pipe, check collision, update score.
  Writes render commands to xRenderQueue.
  Waits on EVENT_RESET event bit to restart after game over.

vRenderTask  (priority 1, 768 word stack)
  Blocks on xRenderQueue.
  Draws each frame to the LCD (under mutex).
  Handles splash screen, game frame, and game over screen.
```

### Inter-task communication

```
[MPU9250] --I2C--> vSensorTask --xGyroQueue (depth 1)--> vGameTask
                                                              |
                                                    xRenderQueue (depth 3)
                                                              |
                                                        vRenderTask --I2C--> [LCD]

SW2 ISR --xEventGroupSetBitsFromISR--> xGameEvents (EVENT_RESET) --> vGameTask
```

`xGyroQueue` uses `xQueueOverwrite` so vGameTask always reads the freshest sensor
data. There is no queue backlog.

`xI2CMutex` serialises all I2C access. Without it, vSensorTask (higher priority)
can preempt vRenderTask mid-write, corrupting a multi-byte LCD sequence.

### Game logic

The LCD has 2 rows (0 = top, 1 = bottom) and 16 columns (0 = left, 15 = right).

The bird lives in column 2. It occupies one row. The gyro GZ axis measures
rotation around the Z axis. At ±250°/s full scale, 1 LSB = 1/131°/s. A threshold
of 3000 LSB (about 23°/s) moves the bird: above threshold moves to row 0, below
negative threshold moves to row 1.

A pipe is one column wide, spans both rows, with one row left open as the gap.
Pipes enter from column 15 and move one column left per game tick. If the pipe
reaches column 2 and the bird is in a blocked row, it is a collision.

Score increments each time a pipe passes column 0. Every 5 points, the tick period
decreases by 20 ms (floor at 80 ms).

Custom characters are stored in LCD CGRAM slots 0 (bird shape) and 1 (solid block
for pipe). Each custom character is an 8-row by 5-column bitmap.

---

## Build and flash

### Prerequisites

- ModusToolbox 3.x installed at `~/ModusToolbox/tools_*`
- GCC ARM toolchain (included with ModusToolbox)
- Board connected via a USB data cable to the KitProg3 micro-USB port (the port
  labelled "KitProg3", not "Target")

### Steps

```sh
cd Flappy_Bird

# Download all libraries (FreeRTOS, HAL, PDL, retarget-io, etc.)
# Only needed on first checkout or after changing deps/*.mtb
make getlibs

# Compile
make build

# Compile and flash
make program
```

Serial output is available at 115200 baud on the KitProg3 USB-UART port. On macOS
the device appears as `/dev/tty.usbmodem*`. On Windows it is a COM port listed in
Device Manager under "USB Serial Device".

To open serial monitor on macOS:

```sh
screen /dev/tty.usbmodem* 115200
# Press Ctrl-A then K to exit screen
```

---

## Debugging notes

### FreeRTOS tasks never executed

The single hardest problem. `printf` before `vTaskStartScheduler()` worked. Nothing
after it ever printed.

The cause is a name mismatch between the GCC ARM startup file and the FreeRTOS
port. The startup file (`startup_psoc6_02_cm4.S`) defines `.weak` vector table
entries named `SVC_Handler`, `PendSV_Handler`, and `SysTick_Handler` that alias to
`Default_Handler` (an infinite loop). The FreeRTOS port defines functions named
`vPortSVCHandler`, `xPortPendSVHandler`, and `xPortSysTickHandler` — different
names that never get linked into the vector table. When `vTaskStartScheduler()`
fires the SVC instruction to launch the first task, the processor jumps to
`Default_Handler` and hangs forever.

The fix is three defines in `FreeRTOSConfig.h`:

```c
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler
```

These cause the FreeRTOS port to compile its functions under the names the startup
file expects, overriding the weak aliases.

A related issue: the BSP guards heap management source files behind preprocessor
checks like `#if(configHEAP_ALLOCATION_SCHEME == HEAP_ALLOCATION_TYPE4)`. Without
defining the `HEAP_ALLOCATION_TYPEn` integer constants, the comparison fails and
no heap file compiles.

### Wrong BSP selected

The project template targeted `APP_CY8CKIT-062-WIFI-BT`. The actual hardware is
`CY8CPROTO-062-4343W`. One line in the Makefile needed to change:

```
TARGET=APP_CY8CPROTO-062-4343W
```

With the wrong TARGET, pin macros resolve to different pins, the clock config is
for a different device, and the CM0+ firmware image does not match.

### USB charge-only cable

Board connected but produced no serial output and the programmer did not detect it.
The cable had power lines only. Replacing it with a data cable fixed it immediately.
`ls /dev/tty.*` shows no device with a power-only cable; the correct device appears
as `/dev/tty.usbmodem*` with a data cable.

---

## Project structure

```
Flappy_Bird/
  source/
    main.c              FreeRTOS app: three tasks, I2C, SW2 ISR
    FreeRTOSConfig.h    FreeRTOS kernel config
    game.c / game.h     Game state machine
    mpu9250.c / .h      MPU9250 gyro I2C driver
    LCD_I2C/
      lcd_i2c.c / .h    PCF8574 I2C LCD driver
  bsps/
    TARGET_APP_CY8CPROTO-062-4343W/   Active BSP
  deps/
    freertos.mtb        FreeRTOS library pointer
    retarget-io.mtb     UART printf library pointer
  Makefile
  NOTES.md              Development log and file inventory
```

Libraries (`libs/`) and build output (`build/`) are not tracked in git.
Run `make getlibs` after cloning to restore them.

---

## License

This project (application source code) is licensed under the MIT License — see [LICENSE](LICENSE) for details.

The Infineon PSoC6 BSP, PDL, HAL, and FreeRTOS library components fetched via `make getlibs` carry their own licenses (Apache 2.0 and MIT) located within the respective library directories.
