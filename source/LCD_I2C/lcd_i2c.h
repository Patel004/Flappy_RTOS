/*******************************************************************************
 * lcd_i2c.h
 * HD44780 16x2 LCD driver via PCF8574 I2C backpack
 * Target: PSoC6 CY8CKIT-062-WiFi-BT, ModusToolbox, cyhal
 *
 * Pin mapping (PCF8574 → HD44780):
 *   P7 P6 P5 P4  P3       P2  P1  P0
 *   D7 D6 D5 D4  Backlight En  Rw  Rs
 ******************************************************************************/
#ifndef LCD_I2C_H
#define LCD_I2C_H

#include "cyhal.h"
#include "cybsp.h"
#include <stdint.h>

/* ── I2C address of PCF8574 backpack ─────────────────────────────────────── */
/* lcd_init() probes 0x27 first (PCF8574), then 0x3F (PCF8574A). */
#define LCD_ADDR_DEFAULT  (0x27u)
#define LCD_ADDR_ALT      (0x3Fu)
/* Resolved at runtime; exposed so callers can read it after lcd_init(). */
extern uint8_t g_lcd_addr;
#define LCD_COLS        (16u)
#define LCD_ROWS        (2u)

/* ── HD44780 commands ────────────────────────────────────────────────────── */
#define LCD_CLEARDISPLAY    (0x01u)
#define LCD_RETURNHOME      (0x02u)
#define LCD_ENTRYMODESET    (0x04u)
#define LCD_DISPLAYCONTROL  (0x08u)
#define LCD_FUNCTIONSET     (0x20u)
#define LCD_SETCGRAMADDR    (0x40u)
#define LCD_SETDDRAMADDR    (0x80u)

/* ── Entry mode flags ────────────────────────────────────────────────────── */
#define LCD_ENTRYLEFT           (0x02u)
#define LCD_ENTRYSHIFTDECREMENT (0x00u)

/* ── Display control flags ───────────────────────────────────────────────── */
#define LCD_DISPLAYON   (0x04u)
#define LCD_CURSOROFF   (0x00u)
#define LCD_BLINKOFF    (0x00u)

/* ── Function set flags ──────────────────────────────────────────────────── */
#define LCD_4BITMODE    (0x00u)
#define LCD_2LINE       (0x08u)
#define LCD_5x8DOTS     (0x00u)

/* ── PCF8574 bit positions ───────────────────────────────────────────────── */
#define LCD_RS          (0x01u)   /* Register select                         */
#define LCD_RW          (0x02u)   /* Read/Write (tied low = write)           */
#define LCD_EN          (0x04u)   /* Enable pulse                            */
#define LCD_BACKLIGHT   (0x08u)   /* Backlight on                            */

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise LCD. Call once after cyhal_i2c_init().
 * @param  i2c  Pointer to an already-initialised cyhal_i2c_t handle.
 */
void lcd_init(cyhal_i2c_t *i2c);

/** @brief  Clear display and return cursor to home. */
void lcd_clear(void);

/**
 * @brief  Set cursor position.
 * @param  col  0–15
 * @param  row  0–1
 */
void lcd_set_cursor(uint8_t col, uint8_t row);

/**
 * @brief  Write a single ASCII character at current cursor position.
 * @param  ch  Character to write.
 */
void lcd_write_char(uint8_t ch);

/**
 * @brief  Write a null-terminated string at current cursor position.
 * @param  str  String to print.
 */
void lcd_print(const char *str);

/**
 * @brief  Define a custom character in CGRAM.
 * @param  slot     0–7
 * @param  bitmap   8-byte array, 5 LSBs of each byte used.
 */
void lcd_create_char(uint8_t slot, const uint8_t bitmap[8]);

#endif /* LCD_I2C_H */
