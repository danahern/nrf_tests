/*
 * PSE84 Voice Assistant — state machine.
 *
 * Single-threaded: state_cycle() is called from the input callback
 * (gpio_keys deferred workqueue) and state_get() from the main loop.
 * Transitions use atomic_t so read/write don't tear between contexts.
 */

#ifndef PSE84_ASSISTANT_STATE_H_
#define PSE84_ASSISTANT_STATE_H_

#include <stdbool.h>

typedef enum {
	ASSIST_IDLE = 0,
	ASSIST_LISTENING,
	ASSIST_THINKING,
	ASSIST_RESPONDING,
	ASSIST_STATE_COUNT,
} assist_state_t;

/* Entry callback fired (from whichever thread triggered the transition)
 * when a new state becomes active. Allowed to be NULL.
 */
typedef void (*assist_state_on_entry_t)(assist_state_t prev, assist_state_t next);

/* Install an on-entry callback. One slot; later calls replace earlier. */
void state_set_on_entry(assist_state_on_entry_t cb);

/* Initialise the state machine. Idempotent. */
void state_init(void);

/* Advance to the next state along the test cycle:
 *   IDLE -> LISTENING -> THINKING -> RESPONDING -> IDLE.
 * Returns the new state.
 */
assist_state_t state_cycle(void);

/* Force a specific state (used by non-test drivers like the host bridge). */
assist_state_t state_set(assist_state_t next);

/* Current state. Safe to call from any context. */
assist_state_t state_get(void);

/* Human-readable name for logs / UI. */
const char *state_name(assist_state_t s);

#endif /* PSE84_ASSISTANT_STATE_H_ */
