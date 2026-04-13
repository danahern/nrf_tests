/*
 * PSE84 Voice Assistant — state machine (implementation).
 *
 * The test cycle (IDLE -> LISTENING -> THINKING -> RESPONDING -> IDLE) is
 * encoded as a transition table so swapping in button / host-driven paths
 * later is a one-line change. The table is indexed by the current state
 * and returns the next state for a "cycle" input.
 */

#include "state.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(assist_state, LOG_LEVEL_INF);

static const assist_state_t cycle_table[ASSIST_STATE_COUNT] = {
	[ASSIST_IDLE] = ASSIST_LISTENING,
	[ASSIST_LISTENING] = ASSIST_THINKING,
	[ASSIST_THINKING] = ASSIST_RESPONDING,
	[ASSIST_RESPONDING] = ASSIST_IDLE,
};

static atomic_t current_state = ATOMIC_INIT(ASSIST_IDLE);
static assist_state_on_entry_t on_entry_cb;

static const char *const state_names[ASSIST_STATE_COUNT] = {
	[ASSIST_IDLE] = "IDLE",
	[ASSIST_LISTENING] = "LISTENING",
	[ASSIST_THINKING] = "THINKING",
	[ASSIST_RESPONDING] = "RESPONDING",
};

void state_set_on_entry(assist_state_on_entry_t cb)
{
	on_entry_cb = cb;
}

void state_init(void)
{
	(void)atomic_set(&current_state, ASSIST_IDLE);
}

const char *state_name(assist_state_t s)
{
	if ((unsigned int)s >= ASSIST_STATE_COUNT) {
		return "?";
	}
	return state_names[s];
}

static assist_state_t transition(assist_state_t next)
{
	const assist_state_t prev = (assist_state_t)atomic_set(&current_state, next);

	if (prev != next) {
		LOG_INF("state: %s -> %s", state_name(prev), state_name(next));
		if (on_entry_cb != NULL) {
			on_entry_cb(prev, next);
		}
	}
	return next;
}

assist_state_t state_cycle(void)
{
	const assist_state_t prev = (assist_state_t)atomic_get(&current_state);
	const assist_state_t next =
		((unsigned int)prev < ASSIST_STATE_COUNT) ? cycle_table[prev] : ASSIST_IDLE;

	return transition(next);
}

assist_state_t state_set(assist_state_t next)
{
	if ((unsigned int)next >= ASSIST_STATE_COUNT) {
		return state_get();
	}
	return transition(next);
}

assist_state_t state_get(void)
{
	return (assist_state_t)atomic_get(&current_state);
}
