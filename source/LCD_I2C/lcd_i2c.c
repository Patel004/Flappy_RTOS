/*******************************************************************************
 * lcd_i2c.c
 * HD44780 16x2 LCD driver via PCF8574 I2C backpack
 * PSoC6 / ModusToolbox / cyhal
 ******************************************************************************/
#include "lcd_i2c.h"
#include "cyhal.h"
#include <string.h>

/* ── Private state ───────────────────────────────────────────────────────── */
static cyhal_i2c_t *_i2c        = NULL;
static uint8_t      _backlight  = LCD_BACKLIGHT;
uint8_t             g_lcd_addr  = LCD_ADDR_DEFAULT;
static uint8_t      _dfunction  = 0;
static uint8_t      _dcontrol   = 0;
static uint8_t      _dmode      = 0;

/* ── Private helpers ─────────────────────────────────────────────────────── */

/** Send one byte to the PCF8574 expander over I2C. */
static void _expander_write(uint8_t data)
{
    uint8_t buf = data | _backlight;
    cyhal_i2c_master_write(_i2c, g_lcd_addr, &buf, 1, 0, true);
}

/** Pulse the Enable line to latch 4 bits into the HD44780. */
static void _pulse_enable(uint8_t data)
{
    _expander_write(data | LCD_EN);
    cyhal_system_delay_us(1);
    _expander_write(data & ~LCD_EN);
    cyhal_system_delay_us(50);
}

/** Send a 4-bit nibble (in the high 4 bits of value). */
static void _write4bits(uint8_t value)
{
    _expander_write(value);
    _pulse_enable(value);
}

/** Send a full byte as two 4-bit nibbles. mode = 0 (command) or LCD_RS (data). */
static void _send(uint8_t value, uint8_t mode)
{
    uint8_t hi = value & 0xF0u;
    uint8_t lo = (value << 4) & 0xF0u;
    _write4bits(hi | mode);
    _write4bits(lo | mode);
}

static void _command(uint8_t cmd)   { _send(cmd, 0);      }
static void _data(uint8_t data)     { _send(data, LCD_RS); }

/* ── Public API ──────────────────────────────────────────────────────────── */

void lcd_init(cyhal_i2c_t *i2c)
{
    _i2c = i2c;

    /* Probe for PCF8574 at 0x27 (PCF8574) then 0x3F (PCF8574A). */
    uint8_t probe = 0;
    if (cyhal_i2c_master_write(_i2c, LCD_ADDR_DEFAULT, &probe, 0, 10, true)
            == CY_RSLT_SUCCESS) {
        g_lcd_addr = LCD_ADDR_DEFAULT;
    } else {
        g_lcd_addr = LCD_ADDR_ALT;   /* fall back; init proceeds regardless */
    }

    _dfunction = LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS;
    _dcontrol  = LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF;
    _dmode     = LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT;

    /* HD44780 power-on reset sequence (datasheet fig.24) */
    cyhal_system_delay_ms(50);
    _expander_write(_backlight);
    cyhal_system_delay_ms(1000);

    /* Put LCD in 4-bit mode — three attempts required */
    _write4bits(0x03u << 4);
    cyhal_system_delay_us(4500);
    _write4bits(0x03u << 4);
    cyhal_system_delay_us(4500);
    _write4bits(0x03u << 4);
    cyhal_system_delay_us(150);
    _write4bits(0x02u << 4);   /* switch to 4-bit interface */

    /* Finalise init */
    _command(LCD_FUNCTIONSET | _dfunction);
    _command(LCD_DISPLAYCONTROL | _dcontrol);
    lcd_clear();
    _command(LCD_ENTRYMODESET | _dmode);
    _command(LCD_RETURNHOME);
    cyhal_system_delay_ms(2);
}

void lcd_clear(void)
{
    _command(LCD_CLEARDISPLAY);
    cyhal_system_delay_ms(2);
}

void lcd_set_cursor(uint8_t col, uint8_t row)
{
    static const uint8_t row_offsets[2] = {0x00u, 0x40u};
    if (row >= LCD_ROWS) row = LCD_ROWS - 1u;
    if (col >= LCD_COLS) col = LCD_COLS - 1u;
    _command(LCD_SETDDRAMADDR | (col + row_offsets[row]));
}

void lcd_write_char(uint8_t ch)
{
    _data(ch);
}

void lcd_print(const char *str)
{
    while (*str) {
        _data((uint8_t)*str++);
    }
}

void lcd_create_char(uint8_t slot, const uint8_t bitmap[8])
{
    slot &= 0x07u;
    _command(LCD_SETCGRAMADDR | (slot << 3));
    for (uint8_t i = 0; i < 8u; i++) {
        _data(bitmap[i]);
    }
    /* Return to DDRAM so subsequent setCursor calls work */
    _command(LCD_SETDDRAMADDR);
}
