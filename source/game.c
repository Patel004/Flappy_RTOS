/*******************************************************************************
 * game.c
 * Flappy Bird game logic
 ******************************************************************************/
#include "game.h"
#include <stdlib.h>

/* ── Private helper ──────────────────────────────────────────────────────── */

/** Pick a new random pipe gap row and reset pipe to right edge. */
static void _reset_pipe(game_t *g)
{
    g->pipe_col = PIPE_START_COL;
    g->pipe_gap = rand() % 2;   /* 0 = top open, 1 = bottom open */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void game_reset(game_t *g)
{
    g->bird_row = 1;
    g->score    = 0;
    g->tick_ms  = GAME_TICK_MS;
    g->state    = GAME_STATE_RUNNING;
    _reset_pipe(g);
}

void game_update_bird(game_t *g, int16_t gz)
{
    if (gz > GYRO_THRESHOLD) {
        g->bird_row = 0;   /* Tilt one way  → top row    */
    } else if (gz < -GYRO_THRESHOLD) {
        g->bird_row = 1;   /* Tilt other way → bottom row */
    }
    /* No significant tilt → bird holds current row */
}

bool game_tick(game_t *g)
{
    /* ── Score: increment exactly once as pipe passes bird column ─────────── */
    /* pipe_col was > BIRD_COL last tick and is now == BIRD_COL */
    if (g->pipe_col == (int)BIRD_COL) {
        /* Only score if bird is in the gap (i.e. no collision yet) */
        if (g->bird_row == g->pipe_gap) {
            g->score++;

            /* Increase difficulty every DIFFICULTY_STEP points */
            if ((g->score % DIFFICULTY_STEP) == 0) {
                if (g->tick_ms > GAME_TICK_MIN_MS + SPEED_INCREMENT_MS) {
                    g->tick_ms -= SPEED_INCREMENT_MS;
                } else {
                    g->tick_ms = GAME_TICK_MIN_MS;
                }
            }
        }
    }

    /* ── Collision: bird occupies same column as pipe AND is not in gap ───── */
    if (g->pipe_col == (int)BIRD_COL && g->bird_row != g->pipe_gap) {
        return true;   /* Collision! */
    }

    /* ── Move pipe left ───────────────────────────────────────────────────── */
    g->pipe_col--;
    if (g->pipe_col < 0) {
        _reset_pipe(g);   /* New random gap, back to right edge */
    }

    return false;   /* No collision */
}
