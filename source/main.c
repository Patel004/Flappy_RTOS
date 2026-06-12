/*******************************************************************************
 * main.c
 * Flappy Bird — PSoC6 CY8CPROTO-062-4343W
 * FreeRTOS real-time implementation
 *
 * ── Architecture ─────────────────────────────────────────────────────────────
 *
 *   Task 1: vSensorTask   (Priority 3 — HIGHEST)
 *     Reads MPU9250 gyro every 50ms via vTaskDelayUntil.
 *     Posts gyro_data_t to xGyroQueue (depth 1, overwrite on full).
 *     Sets EVENT_RESET in xGameEvents if SW2 pressed.
 *
 *   Task 2: vGameTask     (Priority 2)
 *     Runs at dynamic tick rate (starts 220ms, speeds up with score).
 *     Reads latest gyro sample from xGyroQueue.
 *     Updates bird, advances pipe, checks collision, updates score.
 *     Posts render_msg_t to xRenderQueue for Task 3.
 *     Waits for EVENT_RESET from xGameEvents to restart at any time.
 *
 *   Task 3: vRenderTask   (Priority 1 — LOWEST)
 *     Blocks on xRenderQueue.
 *     Draws game frame on LCD (bird, pipe, score).
 *     Handles splash screen and Game Over screen display.
 *
 *   ISR: gpio_interrupt_handler
 *     Fires on SW2 (P0.4) falling edge.
 *     Sets EVENT_RESET bit in xGameEvents from ISR context.
 *
 * ── Queue / Event flow ────────────────────────────────────────────────────────
 *
 *   [MPU9250] → vSensorTask → xGyroQueue (gyro_data_t) → vGameTask
 *   vGameTask → xRenderQueue (render_msg_t) → vRenderTask → [LCD]
 *   SW2 ISR / vSensorTask → xGameEvents (EVENT_RESET) → vGameTask
 *
 * ── Wiring ────────────────────────────────────────────────────────────────────
 *   MPU9250 SDA → P6.1 (CYBSP_I2C_SDA)
 *   MPU9250 SCL → P6.0 (CYBSP_I2C_SCL)
 *   MPU9250 VCC → 3.3V
 *   MPU9250 GND → GND
 *   MPU9250 AD0 → GND  (I2C addr = 0x68)
 *   LCD SDA     → P6.1 (same I2C bus)
 *   LCD SCL     → P6.0 (same I2C bus)
 *   LCD VCC     → 5V or 3.3V (check your backpack)
 *   LCD GND     → GND
 *   SW2 (reset) → P0.4 (CYBSP_USER_BTN, onboard push-button)
 ******************************************************************************/

#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "event_groups.h"
#include "semphr.h"

#include "LCD_I2C/lcd_i2c.h"
#include "mpu9250.h"
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/*=============================================================================
 * Configuration
 *===========================================================================*/

/* Reset button: SW2 on CY8CPROTO-062-4343W */
#define SW2_PIN     (CYBSP_USER_BTN)

/* FreeRTOS task priorities */
#define PRIORITY_SENSOR     (3u)
#define PRIORITY_GAME       (2u)
#define PRIORITY_RENDER     (1u)

/* FreeRTOS task stack sizes (in words) */
#define STACK_SENSOR        (768u)
#define STACK_GAME          (512u)
#define STACK_RENDER        (768u)

/* Sensor task period */
#define SENSOR_PERIOD_MS    (50u)

/* Event bits */
#define EVENT_RESET         (1u << 0)   /* SW2 hardware button */

/*=============================================================================
 * Custom LCD characters
 *===========================================================================*/

/* Slot 0: Bird (facing right) */
static const uint8_t BIRD_CHAR[8] = {
    0b00000,
    0b01110,
    0b11111,
    0b01110,
    0b01010,
    0b00000,
    0b00000,
    0b00000
};

/* Slot 1: Solid pipe block */
static const uint8_t PIPE_CHAR[8] = {
    0b11111,
    0b11111,
    0b11111,
    0b11111,
    0b11111,
    0b11111,
    0b11111,
    0b11111
};

/*=============================================================================
 * Message types for inter-task communication
 *===========================================================================*/

/** Gyro sample posted by sensor task → game task */
typedef struct {
    int16_t gz;
} gyro_msg_t;

/** Render command posted by game task → render task */
typedef enum {
    RENDER_FRAME,       /* Draw a normal game frame     */
    RENDER_SPLASH,      /* Draw the title/splash screen */
    RENDER_GAME_OVER,   /* Draw the Game Over screen    */
} render_cmd_t;

typedef struct {
    render_cmd_t cmd;
    int          bird_row;
    int          pipe_col;
    int          pipe_gap;
    int          score;
} render_msg_t;

/*=============================================================================
 * Shared handles (created in main, used across tasks)
 *===========================================================================*/
static cyhal_i2c_t      g_i2c;
static QueueHandle_t    xGyroQueue   = NULL;
static QueueHandle_t    xRenderQueue = NULL;
static EventGroupHandle_t xGameEvents = NULL;

/* Button interrupt handle */
static cyhal_gpio_callback_data_t g_sw2_cb_data;

/* I2C mutex — prevents vSensorTask and vRenderTask from sharing the bus
 * simultaneously; all cyhal_i2c_* calls must be made under this lock. */
static SemaphoreHandle_t xI2CMutex = NULL;

/*=============================================================================
 * GPIO interrupt handler — fires from ISR for BTN0 and BTN1
 *===========================================================================*/
static void gpio_interrupt_handler(void *handler_arg, cyhal_gpio_event_t event)
{
    (void)handler_arg;
    (void)event;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(xGameEvents, EVENT_RESET,
                              &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/*=============================================================================
 * Stack overflow hook — prints the offending task name over UART
 *===========================================================================*/
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("STACK OVERFLOW in task: %s\r\n", pcTaskName);
    for (;;) {}
}

/*=============================================================================
 * Task 1: vSensorTask
 * Reads MPU9250 gyro at exactly 50ms intervals using vTaskDelayUntil.
 * Posts gz value to xGyroQueue (overwrites old value if game task is slow).
 * Also checks buttons in software as backup.
 *===========================================================================*/
static void vSensorTask(void *pvParameters)
{
    (void)pvParameters;
    printf("vSensorTask started\r\n");

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xPeriod = pdMS_TO_TICKS(SENSOR_PERIOD_MS);

    gyro_data_t raw;
    gyro_msg_t  msg;

    for (;;)
    {
        vTaskDelayUntil(&xLastWakeTime, xPeriod);

        xSemaphoreTake(xI2CMutex, portMAX_DELAY);
        bool gyro_ok = mpu9250_read_gyro(&raw);
        xSemaphoreGive(xI2CMutex);

        if (gyro_ok) {
            msg.gz = raw.z;
            xQueueOverwrite(xGyroQueue, &msg);
        }
    }
}

/*=============================================================================
 * Task 2: vGameTask
 * Core game loop. Runs at dynamic tick rate.
 * Consumes gyro samples, updates state, posts render commands.
 *===========================================================================*/
static void vGameTask(void *pvParameters)
{
    (void)pvParameters;
    printf("vGameTask started\r\n");

    game_t      game;
    gyro_msg_t  gyro_msg;
    render_msg_t render_msg;
    EventBits_t bits;

    /* ── Splash screen ─────────────────────────────────────────────────── */
    render_msg.cmd = RENDER_SPLASH;
    xQueueSend(xRenderQueue, &render_msg, portMAX_DELAY);

    /* Wait for first tilt or SW2 button to start */
    for (;;) {
        bits = xEventGroupWaitBits(xGameEvents, EVENT_RESET,
                                   pdTRUE, pdFALSE, pdMS_TO_TICKS(10));
        if (bits & EVENT_RESET) break;

        if (xQueueReceive(xGyroQueue, &gyro_msg, 0) == pdTRUE) {
            if (gyro_msg.gz > RESTART_GYRO_THR ||
                gyro_msg.gz < -RESTART_GYRO_THR) break;
        }
    }

    /* Seed random with gyro noise */
    srand((unsigned int)xTaskGetTickCount());

    /* Start first game */
    game_reset(&game);

    /* ── Main game loop ─────────────────────────────────────────────────── */
    for (;;)
    {
        /* Mid-game reset: SW2 button only */
        bits = xEventGroupWaitBits(xGameEvents, EVENT_RESET,
                                   pdTRUE, pdFALSE, 0);
        if (bits & EVENT_RESET) {
            game_reset(&game);
        }

        /* Get latest gyro sample (non-blocking — use last if none ready) */
        xQueueReceive(xGyroQueue, &gyro_msg, 0);

        if (game.state == GAME_STATE_RUNNING) {

            game_update_bird(&game, gyro_msg.gz);

            /* Advance one game tick */
            bool collision = game_tick(&game);

            if (collision) {
                game.state = GAME_STATE_OVER;
                printf("COLLISION — Score: %d\r\n", game.score);

                /* Send Game Over render command */
                render_msg.cmd   = RENDER_GAME_OVER;
                render_msg.score = game.score;
                xQueueSend(xRenderQueue, &render_msg, portMAX_DELAY);

                /* 1-second cooldown so the collision tilt doesn't immediately
                 * re-trigger a restart. Drain event and gyro queue. */
                vTaskDelay(pdMS_TO_TICKS(1000));
                xEventGroupClearBits(xGameEvents, EVENT_RESET);
                while (xQueueReceive(xGyroQueue, &gyro_msg, 0) == pdTRUE) {}

                /* Wait for SW2 to restart */
                xEventGroupWaitBits(xGameEvents, EVENT_RESET,
                                    pdTRUE, pdFALSE, portMAX_DELAY);
                game_reset(&game);
                continue;
            }

            /* Send frame render command */
            render_msg.cmd      = RENDER_FRAME;
            render_msg.bird_row = game.bird_row;
            render_msg.pipe_col = game.pipe_col;
            render_msg.pipe_gap = game.pipe_gap;
            render_msg.score    = game.score;
            xQueueSend(xRenderQueue, &render_msg, portMAX_DELAY);
        }

        /* Dynamic tick delay — speeds up as score increases */
        vTaskDelay(pdMS_TO_TICKS(game.tick_ms));
    }
}

/*=============================================================================
 * Task 3: vRenderTask
 * Lowest priority — handles all LCD output.
 * Blocks on xRenderQueue waiting for commands from game task.
 *===========================================================================*/
static void vRenderTask(void *pvParameters)
{
    (void)pvParameters;
    printf("vRenderTask started\r\n");

    render_msg_t msg;
    char score_buf[12];

    for (;;)
    {
        /* Block until game task sends something to display */
        if (xQueueReceive(xRenderQueue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Hold the I2C mutex for the entire frame so vSensorTask cannot
         * seize the bus mid-write and corrupt a multi-character sequence. */
        xSemaphoreTake(xI2CMutex, portMAX_DELAY);

        switch (msg.cmd)
        {
            /* ── Splash screen ─────────────────────────────────────────── */
            case RENDER_SPLASH:
                lcd_clear();
                lcd_set_cursor(2, 0);
                lcd_print("FLAPPY  BIRD");
                lcd_set_cursor(0, 1);
                lcd_print("Tilt to start!");
                break;

            /* ── Normal game frame ─────────────────────────────────────── */
            case RENDER_FRAME:
                lcd_clear();

                /* Bird — custom char slot 0 */
                lcd_set_cursor((uint8_t)BIRD_COL, (uint8_t)msg.bird_row);
                lcd_write_char(0);

                /* Pipe blocks — custom char slot 1, every row except gap */
                for (uint8_t row = 0; row < LCD_ROWS; row++) {
                    if (row != (uint8_t)msg.pipe_gap) {
                        lcd_set_cursor((uint8_t)msg.pipe_col, row);
                        lcd_write_char(1);
                    }
                }

                /* Score — right-justified at top row */
                snprintf(score_buf, sizeof(score_buf), "%d", msg.score);
                uint8_t score_col = (uint8_t)(LCD_COLS - strlen(score_buf));
                lcd_set_cursor(score_col, 0);
                lcd_print(score_buf);
                break;

            /* ── Game Over screen ──────────────────────────────────────── */
            case RENDER_GAME_OVER:
                lcd_clear();
                lcd_set_cursor(3, 0);
                lcd_print("GAME  OVER");
                snprintf(score_buf, sizeof(score_buf), "Score:%d", msg.score);
                lcd_set_cursor(0, 1);
                lcd_print(score_buf);
                break;

            default:
                break;
        }

        xSemaphoreGive(xI2CMutex);
    }
}

/*=============================================================================
 * main() — hardware init + FreeRTOS task creation
 *===========================================================================*/
int main(void)
{
    cy_rslt_t result;

    /* ── BSP init ──────────────────────────────────────────────────────── */
    result = cybsp_init();
    CY_ASSERT(result == CY_RSLT_SUCCESS);
    __enable_irq();

    /* ── UART debug (serial monitor at 115200 baud) ────────────────────── */
    result = cy_retarget_io_init(CYBSP_DEBUG_UART_TX, CYBSP_DEBUG_UART_RX,
                                 CY_RETARGET_IO_BAUDRATE);
    CY_ASSERT(result == CY_RSLT_SUCCESS);
    printf("\x1b[2J\x1b[;H");
    printf("=== Flappy Bird PSoC6 FreeRTOS ===\r\n");

    /* ── I2C init (shared by LCD and MPU9250) ──────────────────────────── */
    cyhal_i2c_cfg_t i2c_cfg = {
        .is_slave     = false,
        .address      = 0,
        .frequencyhal_hz = 400000UL   /* 400kHz fast mode */
    };
    result = cyhal_i2c_init(&g_i2c, CYBSP_I2C_SDA, CYBSP_I2C_SCL, NULL);
    CY_ASSERT(result == CY_RSLT_SUCCESS);
    result = cyhal_i2c_configure(&g_i2c, &i2c_cfg);
    CY_ASSERT(result == CY_RSLT_SUCCESS);
    /* Default SCB interrupt priority (3) is above FreeRTOS max-syscall level (4).
     * Lower it to a FreeRTOS-safe priority so fromISR APIs work in tasks. */
    cyhal_i2c_enable_event(&g_i2c, CYHAL_I2C_MASTER_WR_CMPLT_EVENT,
                           configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, false);
    printf("I2C initialised (SDA=P6.1, SCL=P6.0, 400kHz)\r\n");

    /* ── LCD init ──────────────────────────────────────────────────────── */
    lcd_init(&g_i2c);
    printf("LCD initialised at I2C addr 0x%02X\r\n", (unsigned)g_lcd_addr);

    lcd_create_char(0, BIRD_CHAR);
    lcd_create_char(1, PIPE_CHAR);

    /* ── MPU9250 init ──────────────────────────────────────────────────── */
    bool imu_ok = mpu9250_init(&g_i2c, GYRO_FS_250);
    if (!imu_ok) {
        printf("WARNING: MPU9250 not detected — using zero gyro.\r\n");
    }

    /* ── SW2 GPIO interrupt init (P0.4 — CYBSP_USER_BTN) ─────────────── */
    result = cyhal_gpio_init(SW2_PIN, CYHAL_GPIO_DIR_INPUT,
                             CYHAL_GPIO_DRIVE_PULLUP, 1u);
    CY_ASSERT(result == CY_RSLT_SUCCESS);
    g_sw2_cb_data.callback = gpio_interrupt_handler;
    g_sw2_cb_data.callback_arg = NULL;
    cyhal_gpio_register_callback(SW2_PIN, &g_sw2_cb_data);
    cyhal_gpio_enable_event(SW2_PIN, CYHAL_GPIO_IRQ_FALL,
                            configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY,
                            true);
    printf("SW2 initialised (P0.4)\r\n");

    /* ── FreeRTOS objects ──────────────────────────────────────────────── */
    xI2CMutex = xSemaphoreCreateMutex();
    CY_ASSERT(xI2CMutex != NULL);

    /* Gyro queue: depth 1, overwrite semantics via xQueueOverwrite */
    xGyroQueue = xQueueCreate(1, sizeof(gyro_msg_t));
    CY_ASSERT(xGyroQueue != NULL);

    /* Render queue: depth 3 to absorb bursts */
    xRenderQueue = xQueueCreate(3, sizeof(render_msg_t));
    CY_ASSERT(xRenderQueue != NULL);

    /* Event group for reset signal */
    xGameEvents = xEventGroupCreate();
    CY_ASSERT(xGameEvents != NULL);

    xTaskCreate(vSensorTask, "Sensor", STACK_SENSOR, NULL,
                PRIORITY_SENSOR, NULL);
    xTaskCreate(vGameTask,   "Game",   STACK_GAME,   NULL,
                PRIORITY_GAME,   NULL);
    xTaskCreate(vRenderTask, "Render", STACK_RENDER, NULL,
                PRIORITY_RENDER, NULL);

    printf("Tasks created. Starting scheduler...\r\n");

    /* ── Start FreeRTOS scheduler (never returns) ──────────────────────── */
    vTaskStartScheduler();

    /* If we reach here, scheduler returned — insufficient heap or port failure */
    printf("FATAL: vTaskStartScheduler returned!\r\n");
    for (;;) {}
    return 0;
}

/* Required when configSUPPORT_STATIC_ALLOCATION=1 */
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *puxIdleTaskStackSize)
{
    static StaticTask_t xIdleTaskTCB;
    static StackType_t  uxIdleTaskStack[configMINIMAL_STACK_SIZE];
    *ppxIdleTaskTCBBuffer   = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *puxIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *puxTimerTaskStackSize)
{
    static StaticTask_t xTimerTaskTCB;
    static StackType_t  uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];
    *ppxTimerTaskTCBBuffer   = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *puxTimerTaskStackSize   = configTIMER_TASK_STACK_DEPTH;
}
