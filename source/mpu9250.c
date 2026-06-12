/*******************************************************************************
 * mpu9250.c
 * Minimal MPU9250/MPU6500 gyroscope driver — PSoC6 / cyhal
 ******************************************************************************/
#include "mpu9250.h"
#include "cyhal.h"
#include <stdio.h>

/* ── Private state ───────────────────────────────────────────────────────── */
static cyhal_i2c_t *_i2c = NULL;

/* ── Private helpers ─────────────────────────────────────────────────────── */

static cy_rslt_t _write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return cyhal_i2c_master_write(_i2c, MPU9250_ADDR, buf, 2, 0, true);
}

static cy_rslt_t _read_regs(uint8_t reg, uint8_t *buf, uint8_t len)
{
    cy_rslt_t r;
    r = cyhal_i2c_master_write(_i2c, MPU9250_ADDR, &reg, 1, 0, false);
    if (r != CY_RSLT_SUCCESS) return r;
    return cyhal_i2c_master_read(_i2c, MPU9250_ADDR, buf, len, 0, true);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool mpu9250_init(cyhal_i2c_t *i2c, gyro_fs_t fs)
{
    _i2c = i2c;

    /* WHO_AM_I check: MPU9250 = 0x71, MPU6500 = 0x70, MPU6050 = 0x68 */
    uint8_t who = 0;
    _read_regs(MPU_REG_WHO_AM_I, &who, 1);
    printf("MPU WHO_AM_I = 0x%02X\r\n", who);
    if (who != 0x71u && who != 0x70u && who != 0x68u) {
        printf("MPU9250 not found! Check wiring.\r\n");
        return false;
    }

    /* Wake up — clear sleep bit */
    _write_reg(MPU_REG_PWR_MGMT_1, 0x00u);
    cyhal_system_delay_ms(10);

    /* Set gyro full-scale range */
    _write_reg(MPU_REG_GYRO_CONFIG, (uint8_t)fs);

    printf("MPU9250 initialised OK (WHO_AM_I=0x%02X)\r\n", who);
    return true;
}

bool mpu9250_read_gyro(gyro_data_t *out)
{
    uint8_t buf[6];
    cy_rslt_t r = _read_regs(MPU_REG_GYRO_XOUT_H, buf, 6);
    if (r != CY_RSLT_SUCCESS) {
        printf("Gyro read failed\r\n");
        return false;
    }

    out->x = (int16_t)((buf[0] << 8) | buf[1]);
    out->y = (int16_t)((buf[2] << 8) | buf[3]);
    out->z = (int16_t)((buf[4] << 8) | buf[5]);
    return true;
}
