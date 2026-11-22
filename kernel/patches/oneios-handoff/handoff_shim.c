/*
 * NEW (this project): the entry point a handoff-mode boot calls (oneios-handoff-start.patch),
 * and weak defaults for the two things this drop does not include a consumer for yet:
 *   - OneiOSHandoffGPUStart(): the source project's RedwoodGPU.cpp (an IOKit service a program
 *     opens to submit real draw commands) was not pulled into this drop. Without it,
 *     boot-arg oneios-handoff-gpu_keep=1 still runs the GPU bring-up and the one-word test
 *     submission in handoff_gpu.c's hakios_dada_gpu_init(), it just has nothing further to hand
 *     the GPU to; that is enough to show the GPU answers, not to draw a frame with it.
 *   - redwood_touch_post(): the source project's backboardd-side reader of these samples is not
 *     part of this drop either. The weak default here only logs each sample, so running
 *     boot-arg oneios-handoff-touch=1 proves the touch chip is readable and decoded, without yet
 *     reaching any iOS HID service. kernel/patches/oneios-hosted/hid/ already builds a real
 *     IOHIDDevice path for touch in hosted mode; wiring handoff_touch.c's samples into an
 *     equivalent for handoff mode is the next step, not done here.
 * See README.md in this directory.
 */
#include <libkern/libkern.h>
#include <sys/cdefs.h>
#include <sys/systm.h>

#include "handoff_log.h"
#include "handoff_input.h"

void hakios_dada_gpu_init(void);        /* handoff_gpu.c */
void hakios_dada_touch_init(void);      /* handoff_touch.c */

__attribute__((weak)) void
OneiOSHandoffGPUStart(void)
{
	handoff_log("handoff: OneiOSHandoffGPUStart has no real service linked yet (handoff_shim.c)\n");
}

__attribute__((weak)) void
redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	static const char *const names[] = { [1] = "DOWN", [2] = "MOVE", [3] = "UP" };

	handoff_log("handoff: touch %s finger %u at (%u, %u) -- no HID consumer linked yet\n",
	    (type < 4) ? names[type] : "?", finger, x, y);
}

/*
 * Called from bsd_init (oneios-handoff-start.patch) after the storage hook, the same point
 * oneios_board_late_start() is called from in hosted mode -- but only under boot-arg
 * oneios-handoff=1, so a hosted build that happens to link this archive by mistake still does
 * nothing (belt and suspenders beside the fact that this archive is not in oneios_defconfig's or
 * oneios_hosted_defconfig's CONFIG_XNU_EXTRA_LD_ARCHIVES at all -- see README.md).
 */
void
oneios_handoff_start(void)
{
	uint32_t enabled = 0;

	if (!PE_parse_boot_argn("oneios-handoff", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	handoff_log("handoff: oneios-handoff=1 -- bringing up what this drop has (GPU, touch)\n");
	hakios_dada_gpu_init();
	hakios_dada_touch_init();
}
