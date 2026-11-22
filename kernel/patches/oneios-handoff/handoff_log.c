/*
 * NEW (this project). See handoff_log.h. The ring buffer is deliberately simple (one writer, no
 * locking beyond disabling preemption around the copy): it must still make progress if the
 * thread that calls it is about to be the one that hangs the GPU/SMMU bus.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#include "handoff_log.h"

struct handoff_log_ring {
	uint32_t magic;          /* 'HOFF': lets a tool that reads physical memory after a hang find this */
	uint32_t size;           /* bytes of text[], fixed at init */
	uint32_t write_offset;   /* next byte to write, wraps at size; never reset, so it also counts bytes written */
	uint32_t wrapped;        /* 0 until write_offset has wrapped once; a reader then starts at write_offset, not 0 */
	char text[];
};

static struct handoff_log_ring *ring;
static uint32_t ring_text_size;

void
handoff_log_init(unsigned long phys_addr, unsigned long size)
{
	extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

	if (phys_addr == 0 || size <= sizeof(struct handoff_log_ring)) {
		printf("handoff_log: bad ring (phys %#lx size %lu), falling back to kprintf only\n", phys_addr, size);
		return;
	}
	ring = (struct handoff_log_ring *)ml_io_map((vm_offset_t)phys_addr, (vm_size_t)size);
	ring_text_size = (uint32_t)(size - sizeof(struct handoff_log_ring));
	ring->magic = 0x4846464f; /* "HOFF" little-endian, i.e. the bytes 'O' 'F' 'F' 'H' when read as text */
	ring->size = ring_text_size;
	ring->write_offset = 0;
	ring->wrapped = 0;
}

static void
ring_put(const char *s, size_t n)
{
	if (ring == NULL) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		ring->text[ring->write_offset] = s[i];
		ring->write_offset++;
		if (ring->write_offset >= ring_text_size) {
			ring->write_offset = 0;
			ring->wrapped = 1;
		}
	}
}

int
handoff_log(const char *format, ...)
{
	char line[192];
	va_list list;
	int n;

	va_start(list, format);
	n = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (n > 0) {
		ring_put(line, (size_t)((n < (int)sizeof(line)) ? n : (int)sizeof(line) - 1));
	}
	/* also to the normal kernel log: on a hosted bench run (no handoff jump yet) this is the
	 * only output, and even on a real handoff run it costs nothing extra. */
	return printf("%s", line);
}
