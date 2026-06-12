/*******************************************************************************
 * mpu9250.h
 * Minimal MPU9250 driver — gyroscope only, I2C, PSoC6 cyhal
 *
 * Wiring (CY8CKIT-062-WiFi-BT):
 *   MPU9250 VCC  → 3.3V
 *   MPU9250 GND  → GND
 *   MPU9250 SCL  → P6.0
 *   MPU9250 SDA  → P6.1
 *   MPU9250 AD0  → GND  (sets I2C address to 0x68)
 *   MPU9250 INT  → not connected
 *   MPU9250 XDA/XCL → not connected
 ******************************************************************************/
#ifndef MPU9250_H
#define MPU9250_H

#include "cyhal.h"
#include <stdint.h>
#include <stdbool.h>

/* ── I2C address (AD0 tied to GND) ──────────────────────────────────────── */
#define MPU9250_ADDR        (0x68u)

/* ── Register addresses ──────────────────────────────────────────────────── */
#define MPU_REG_PWR_MGMT_1  (0x6Bu)
#define MPU_REG_GYRO_CONFIG (0x1Bu)
#define MPU_REG_GYRO_XOUT_H (0x43u)
#define MPU_REG_WHO_AM_I    (0x75u)

/* ── Gyro full-scale range options ───────────────────────────────────────── */
typedef enum {
    GYRO_FS_250  = 0x00u,   /* ±250  °/s — most sensitive, use this */
    GYRO_FS_500  = 0x08u,   /* ±500  °/s */
    GYRO_FS_1000 = 0x10u,   /* ±1000 °/s */
    GYRO_FS_2000 = 0x18u,   /* ±2000 °/s */
} gyro_fs_t;

/* ── Gyro data structure ─────────────────────────────────────────────────── */
typedef struct {
    int16_t x;   /* Raw gyro X */
    int16_t y;   /* Raw gyro Y */
    int16_t z;   /* Raw gyro Z — used for bird control (tilt left/right) */
} gyro_data_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise MPU9250. Call after I2C is ready.
 * @param  i2c  Pointer to initialised cyhal_i2c_t handle.
 * @param  fs   Gyro full-scale range (use GYRO_FS_250 for best sensitivity).
 * @return true if WHO_AM_I check passes (0x71 for MPU9250, 0x70 for MPU6500).
 */
bool mpu9250_init(cyhal_i2c_t *i2c, gyro_fs_t fs);

/**
 * @brief  Read all three gyro axes.
 * @param  out  Pointer to gyro_data_t to fill.
 * @return true on success.
 */
bool mpu9250_read_gyro(gyro_data_t *out);

#endif /* MPU9250_H */
