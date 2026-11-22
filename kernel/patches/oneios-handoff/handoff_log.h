/*
 * NEW (this project): the logging entry point handoff_gpu.c, handoff_touch.c and
 * handoff_image.c call instead of redwood_tl()/loader_log_line(). docs/handoff.md stage 2 says
 * a handoff jump has nowhere to send a log until something reads it back (no adb, likely no
 * UART), so this writes into a small physical ring buffer instead of a device: a fixed-size,
 * fixed-address scratch region that the stage-1 payload builder reserves next to the kernel
 * image and that the jump stub (not yet written) can read back after a hang or a panic, the way
 * the stub already following docs/handoff.md reads xnuhost's own uart.c model in hosted mode.
 * Until that address is wired up (handoff_log_init, called once from oneios_handoff_start()),
 * handoff_log() only calls printf(): on a normal (non-handoff) boot that is XNU's serial/kprintf
 * path, which is enough while this is being bench-tested under hosted mode (see README.md in
 * this directory, "How to try this before a handoff jump exists").
 */
#pragma once
#include <sys/cdefs.h>

void handoff_log_init(unsigned long phys_addr, unsigned long size);
int handoff_log(const char *format, ...) __printflike(1, 2);
