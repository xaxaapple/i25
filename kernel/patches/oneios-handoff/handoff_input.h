/*
 * NEW (this project), trimmed from the source project's bsd/sys/redwood_input.h (APSL-2.0):
 * only the touch-feed part of that header, which handoff_touch.c needs to hand its samples to
 * whatever turns them into HID events for backboardd in this project's own hosted/handoff
 * scheme. The rest of that header (the "GPU kept on" API, Wi-Fi, battery, buttons, storage) is
 * left out here: handoff_gpu.c does not call any of it (its own comments just mention the
 * matching redwood_gpu_* names for readers who know the source project), and the other devices
 * are out of scope for this drop (docs/handoff.md). A real consumer of these touch samples (the
 * equivalent of the source project's backboardd-side reader) is not part of this drop either;
 * see README.md in this directory for what "bring up touch" means right now without one.
 */
#pragma once
#include <stdint.h>

#define REDWOOD_TOUCH_DOWN      1       /* a finger has landed */
#define REDWOOD_TOUCH_MOVE      2
#define REDWOOD_TOUCH_UP        3       /* it has lifted (x, y: where it was last) */

struct redwood_touch {
	uint64_t time;                  /* mach_absolute_time() when the sample was posted */
	uint32_t type;                  /* REDWOOD_TOUCH_* */
	uint32_t finger;                /* which finger, from 0 */
	uint32_t x, y;                  /* position on the display, 0 = left/top edge, 65535 = right/bottom edge */
};

#ifdef KERNEL
void redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);
#endif
