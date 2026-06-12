/*******************************************************************************
 * game.h
 * Flappy Bird game logic — state, update, render
 ******************************************************************************/
#ifndef GAME_H
#define GAME_H

#include <stdint.h>
#include <stdbool.h>

/* ── Game constants ──────────────────────────────────────────────────────── */
#define BIRD_COL            (2u)     /* Bird is always in column 2           */
#define PIPE_START_COL      (15u)    /* Pipe enters from right edge          */
#define GAME_TICK_MS        (220u)   /* Base frame period in ms              */
#define GAME_TICK_MIN_MS    (80u)    /* Fastest frame period (difficulty cap) */
#define DIFFICULTY_STEP     (5u)     /* Score points between speed increases */
#define SPEED_INCREMENT_MS  (20u)    /* ms removed per difficulty step       */

/* ── Gyro threshold for bird control ────────────────────────────────────── */
/* At ±250°/s full scale, 1 LSB = 1/131 °/s.
   3000 LSB ≈ 23°/s — noticeable deliberate tilt, won't misfire at rest.   */
#define GYRO_THRESHOLD      (3000)

/* ── Gyro threshold to trigger game restart after Game Over ──────────────── */
#define RESTART_GYRO_THR    (2000)

/* ── Game state enum ─────────────────────────────────────────────────────── */
typedef enum {
    GAME_STATE_SPLASH,      /* Title screen, waiting for first tilt         */
    GAME_STATE_RUNNING,     /* Active gameplay                              */
    GAME_STATE_OVER,        /* Game Over screen, waiting for restart        */
} game_state_t;

/* ── Public game data (read by render task) ──────────────────────────────── */
typedef struct {
    int          bird_row;      /* 0 = top row, 1 = bottom row              */
    int          pipe_col;      /* Current pipe column (0–15)               */
    int          pipe_gap;      /* Which row has NO pipe block (0 or 1)     */
    int          score;         /* Current score                            */
    uint32_t     tick_ms;       /* Current frame period in ms               */
    game_state_t state;         /* Current game state                       */
} game_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/** @brief  Reset all game state for a new game. */
void game_reset(game_t *g);

/**
 * @brief  Update bird position based on latest gyro Z reading.
 * @param  g    Game state.
 * @param  gz   Raw gyro Z value from MPU9250.
 */
void game_update_bird(game_t *g, int16_t gz);

/**
 * @brief  Advance one game tick: move pipe, check score, check collision.
 * @param  g    Game state.
 * @return true if collision occurred (caller should trigger Game Over).
 */
bool game_tick(game_t *g);

#endif /* GAME_H */
