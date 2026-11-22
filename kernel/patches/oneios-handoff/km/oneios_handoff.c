// SPDX-License-Identifier: GPL-2.0
/*
 * oneios_handoff: the kernel-mode half of the handoff concept (docs/handoff.md). Loaded by hand
 * (insmod, as root, from the "oneios-handoff-launcher" app or a shell) on the owner's own phone.
 * Does nothing on load. Everything happens through writes to /proc/oneios_handoff, one word at
 * a time, each step logged to dmesg and readable back from the same file:
 *
 *   probe                 gates 1 and 2 of docs/handoff.md, from INSIDE the kernel (stronger
 *                          answers than tools/probe/handoff-probe.sh's: this code already runs
 *                          at EL1, so "can code run at EL1" is answered by the fact that it is
 *                          running, and kallsyms/psci/cpu_soft_restart are resolved directly,
 *                          not inferred from sysfs). Always safe: read-only.
 *   payload <phys> <size>  record where the stage-1 payload (kernel.kc + device tree + boot_args,
 *                          built by the launcher app from the owner's OWN already-built handoff
 *                          kernel -- see oneios-handoff-launcher/README.md) has been placed.
 *                          Only records the address; maps and checksums it, nothing else.
 *   dryrun                 parks every secondary CPU with PSCI CPU_OFF and brings them back with
 *                          CPU_ON. Fully reversible: if this hangs or panics, nothing has been
 *                          done that a reboot does not already undo (nothing is flashed). Proves
 *                          PSCI CPU_OFF/CPU_ON work on this phone before the real thing needs them.
 *   jump                   THE POINT OF NO RETURN. Stops every secondary CPU (PSCI CPU_OFF, not
 *                          brought back), masks interrupts, flushes and disables the D-cache,
 *                          disables the MMU, and branches to the payload's entry point with x0 =
 *                          the physical boot_args address, matching kernel/README.md "What the
 *                          booter gets" / "Entry state the kernel expects today" exactly: EL1,
 *                          MMU and caches off, interrupts masked. There is no path back into
 *                          Android from here; if the payload's entry point is wrong, or if gate 2
 *                          (what Gunyah allows) says no, the phone hangs and needs a forced
 *                          reboot (held power button) -- recoverable, because nothing on flash
 *                          changed, but NOT graceful.
 *
 * What this file does NOT know, and cannot check for you:
 *   - whether gate 2 (docs/handoff.md: what Gunyah enforces on the primary VM's kernel) actually
 *     allows disabling the MMU and running code the hypervisor did not launch. "probe" reports
 *     what it can see from EL1 (Gunyah is invisible to probing from here by design); it cannot
 *     prove the jump will be allowed, only that nothing ABOUT THIS KERNEL rules it out.
 *   - whether the payload at <phys> is actually a working handoff-mode XNU. This module trusts
 *     whatever is at that address. Building that payload correctly is the launcher app's job and
 *     the owner's own kernel build's job (see the launcher's README: that kernel does not exist
 *     yet as a drop-in build-handoff.sh output).
 * Written, not built or run: there is no arm64 kernel source tree in the environment this was
 * written in (same reason the rest of this project keeps Apple/Qualcomm files out of git), so
 * this has not seen a compiler. tools/handoff-km-build (the GitHub Actions workflow next to this
 * file) is the first real build it will get.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/kprobes.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/arm-smccc.h>
#include <linux/version.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("oneios-handoff: quiesce Linux and jump to a handoff-mode XNU payload (own device, own risk)");
MODULE_AUTHOR("oneios (iGalaxy25) project, this module written for the owner's own Galaxy S25");

#define LOG(fmt, ...) pr_info("oneios_handoff: " fmt, ##__VA_ARGS__)

/* ---- resolving unexported symbols: the standard post-5.7 kprobe trick (see e.g.
 * github.com/h33p/kallsyms-lp, github.com/4ltern4te/kallsyms_lookup_name_finder: register a
 * kprobe BY NAME, which still works because kprobe registration itself resolves the symbol
 * through kallsyms internally, then read .addr and remove the probe). ------------------------ */
static unsigned long
lookup_symbol(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	unsigned long addr;

	if (register_kprobe(&kp) < 0) {
		return 0;
	}
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

/* cpu_soft_restart(el2_switch, entry, arg0, arg1): arch/arm64/kernel/cpu-reset.S. Used by kexec,
 * hibernation resume and (on some platforms) cpu idle/suspend, so it is often present even with
 * CONFIG_KEXEC off; "probe" is what tells you whether it is actually linked into THIS kernel. */
typedef void (*cpu_soft_restart_t)(unsigned long el2_switch, unsigned long entry,
    unsigned long arg0, unsigned long arg1);
static cpu_soft_restart_t p_cpu_soft_restart;

/* ---- PSCI, straight to firmware, bypassing Linux's own cpu hotplug/scheduler state: the jump
 * must not depend on Linux's bookkeeping still being consistent by the time it runs. ---------- */
#define PSCI_FN_CPU_OFF   0x84000002UL
#define PSCI_FN_CPU_ON64  0xC4000003UL

static void
psci_cpu_off(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(PSCI_FN_CPU_OFF, 0, 0, 0, 0, 0, 0, 0, &res);
	/* does not return on success; if it does, the firmware refused -- logged by the caller */
}

static int
psci_cpu_on(unsigned long cpu_id, unsigned long entry_point)
{
	struct arm_smccc_res res;

	arm_smccc_smc(PSCI_FN_CPU_ON64, cpu_id, entry_point, 0, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

/* ---- state recorded by "payload" ------------------------------------------------------------ */
static phys_addr_t payload_phys;
static size_t payload_size;
static bool payload_set;

/* ---- the IPI that parks a secondary CPU with PSCI CPU_OFF, for "dryrun" and for "jump" ------- */
static void
ipi_cpu_off(void *info)
{
	(void)info;
	LOG("cpu%d: PSCI CPU_OFF\n", smp_processor_id());
	local_irq_disable();
	psci_cpu_off();
	/* unreached on a phone whose firmware honours PSCI; if it returns, something is very wrong
	 * and the safest thing left to do is spin with interrupts off rather than let this core go
	 * back to running Android with half the system already quiesced around it */
	LOG("cpu%d: PSCI CPU_OFF RETURNED -- firmware refused it\n", smp_processor_id());
	for (;;) {
		cpu_relax();
	}
}

/* ---- "probe": gates 1 and 2, from EL1 --------------------------------------------------------- */
static void
do_probe(char *out, size_t out_size)
{
	unsigned long soft_restart_addr, el;
	int n = 0;

	soft_restart_addr = lookup_symbol("cpu_soft_restart");
	p_cpu_soft_restart = (cpu_soft_restart_t)soft_restart_addr;

	asm volatile("mrs %0, CurrentEL" : "=r"(el));
	el = (el >> 2) & 3;

#define A(...) do { if (out && n < (int)out_size) n += scnprintf(out + n, out_size - n, __VA_ARGS__); \
	LOG(__VA_ARGS__); } while (0)
	A("gate1 (EL1 execution): this module is running, so yes -- trivially true once insmod succeeded\n");
	A("gate1 (cpu_soft_restart): %s (%lx)\n", soft_restart_addr ? "FOUND" : "NOT FOUND -- jump has no safe kexec-style primitive", soft_restart_addr);
	A("gate1 (num_online_cpus): %d online of %d possible\n", num_online_cpus(), num_possible_cpus());
	A("CurrentEL: EL%lu (expected EL1; EL2 here would mean this kernel is NOT virtualised by Gunyah the usual way)\n", el);
	A("gate2 (what sits above): NOT determined from here -- Gunyah, if present, does not announce itself to EL1 code.\n");
	A("gate2: tools/probe/handoff-probe.sh's dmesg_boot.txt (grep -i gunyah/hyp/kvm) is still the only source for this.\n");
	A("payload: %s\n", payload_set ? "recorded" : "not recorded yet (write 'payload <phys_hex> <size>' first)");
	if (payload_set) {
		A("payload: phys %pa size %zu\n", &payload_phys, payload_size);
	}
#undef A
}

/* ---- "dryrun": every secondary CPU off then back on ------------------------------------------- */
static void
do_dryrun(void)
{
	int cpu, before = num_online_cpus();

	LOG("dryrun: parking %d secondary cpus (reversible: PSCI CPU_ON brings each back)\n", before - 1);
	for_each_online_cpu(cpu) {
		if (cpu == smp_processor_id()) {
			continue;
		}
		/* remove_cpu() is the normal, cooperative way (unlike the raw IPI "jump" below, this
		 * dry run is meant to look exactly like an ordinary CPU hotplug to the rest of Linux,
		 * so that undoing it with add_cpu() is also ordinary and nothing is left confused) */
		remove_cpu(cpu);
	}
	msleep(50);
	LOG("dryrun: %d cpus online (of %d before); bringing them back\n", num_online_cpus(), before);
	for (cpu = 0; cpu < num_possible_cpus(); cpu++) {
		if (!cpu_online(cpu)) {
			add_cpu(cpu);
		}
	}
	LOG("dryrun: done, %d cpus online\n", num_online_cpus());
}

/* ---- "jump": the point of no return ------------------------------------------------------------
 * Matches kernel/README.md's "Entry state the kernel expects today" row exactly: EL1, MMU and
 * caches off, x0 = physical boot_args, interrupts masked. The payload's own first instruction is
 * trusted completely from here on; this module has no way to verify it past the checksum
 * "payload" recorded (if any -- see the launcher app). */
static void
do_jump(void)
{
	/*
	 * The payload blob is NOT itself a boot_args structure (that would conflate two different
	 * physical addresses: where XNU's _start is, and where boot_args lives -- kernel/README.md
	 * "What the booter gets" keeps them separate: x0 is boot_args's OWN address, which need not
	 * equal the kernel's entry address). The launcher's payload builder writes a small header of
	 * its own at the very start of the blob it hands to "payload <phys> <size>":
	 *   offset  0  u32  magic 'OHJP' (0x4f484a50)
	 *   offset  4  u32  header version (1)
	 *   offset  8  u64  entry_phys      -- physical address of XNU's _start
	 *   offset 16  u64  boot_args_phys  -- physical address of the filled-in boot_args
	 * (both inside [payload_phys, payload_phys + payload_size)). See
	 * ../oneios-handoff-launcher/app/.../PayloadBuilder.kt for how this is written.
	 */
	unsigned long entry_phys, boot_args_phys, magic;

	if (!payload_set) {
		LOG("jump: refused -- no payload recorded (write 'payload <phys_hex> <size>' first)\n");
		return;
	}
	if (!p_cpu_soft_restart) {
		LOG("jump: refused -- cpu_soft_restart was not found by 'probe'; there is no safe primitive for this\n");
		return;
	}
	{
		void __iomem *header = ioremap(payload_phys, 24);

		if (!header) {
			LOG("jump: refused -- could not map the payload header at %pa\n", &payload_phys);
			return;
		}
		magic = readl(header + 0);
		entry_phys = readq(header + 8);
		boot_args_phys = readq(header + 16);
		iounmap(header);
	}
	if (magic != 0x4f484a50UL) {
		LOG("jump: refused -- no OHJP header at %pa (got %08lx); this is not a payload this module wrote\n",
		    &payload_phys, magic);
		return;
	}
	if (entry_phys < payload_phys || entry_phys >= payload_phys + payload_size ||
	    boot_args_phys < payload_phys || boot_args_phys >= payload_phys + payload_size) {
		LOG("jump: refused -- entry %lx or boot_args %lx is outside the recorded payload (%pa + %zu)\n",
		    entry_phys, boot_args_phys, &payload_phys, payload_size);
		return;
	}

	LOG("jump: THE POINT OF NO RETURN. entry %lx, boot_args (x0) %lx. Parking every other cpu now.\n",
	    entry_phys, boot_args_phys);
	preempt_disable();
	local_irq_disable();
	/* every other CPU, off and NOT brought back -- unlike dryrun(), this is one-way */
	smp_call_function(ipi_cpu_off, NULL, 0);
	msleep(200);
	LOG("jump: %d cpus online (should be 1, this one); disabling mmu and caches, branching now -- "
	    "this is the last line oneios_handoff will ever log\n", num_online_cpus());
	mdelay(50); /* let the log line actually reach the console/pstore before the mmu that backs it goes away */

	/* cpu_soft_restart(el2_switch=0, entry, arg0=boot_args phys, arg1=0): disables the MMU and
	 * D-cache for this core and branches to `entry` in physical address space, per
	 * arch/arm64/kernel/cpu-reset.S/.h (unexported past 5.7 like everything else here, resolved
	 * above). el2_switch=0: this core is already at EL1 under whatever sits above it (gate 2);
	 * asking for an EL2 switch here is for a kexec INTO a hypervisor context, not what we want. */
	p_cpu_soft_restart(0, entry_phys, boot_args_phys, 0);
	/* not reached if the payload's entry point is right */
	LOG("jump: cpu_soft_restart RETURNED -- the payload's entry point did not take over. Nothing to do but panic.\n");
	panic("oneios_handoff: jump failed to leave Linux");
}

/* ---- /proc/oneios_handoff ----------------------------------------------------------------------- */
static char last_probe[2048];

static ssize_t
handoff_write(struct file *f, const char __user *buf, size_t count, loff_t *pos)
{
	char line[128];
	unsigned long phys;
	size_t size;

	(void)f; (void)pos;
	if (count == 0 || count >= sizeof(line)) {
		return -EINVAL;
	}
	if (copy_from_user(line, buf, count)) {
		return -EFAULT;
	}
	line[count] = 0;

	if (!strncmp(line, "probe", 5)) {
		do_probe(last_probe, sizeof(last_probe));
	} else if (sscanf(line, "payload %lx %zu", &phys, &size) == 2) {
		payload_phys = (phys_addr_t)phys;
		payload_size = size;
		payload_set = true;
		LOG("payload recorded: phys %pa size %zu\n", &payload_phys, payload_size);
	} else if (!strncmp(line, "dryrun", 6)) {
		do_dryrun();
	} else if (!strncmp(line, "jump", 4)) {
		do_jump();
	} else {
		LOG("unrecognised command: %s", line);
		return -EINVAL;
	}
	return count;
}

static ssize_t
handoff_read(struct file *f, char __user *buf, size_t count, loff_t *pos)
{
	return simple_read_from_buffer(buf, count, pos, last_probe, strlen(last_probe));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
static const struct proc_ops handoff_fops = {
	.proc_write = handoff_write,
	.proc_read = handoff_read,
};
#else
static const struct file_operations handoff_fops = {
	.write = handoff_write,
	.read = handoff_read,
};
#endif

static struct proc_dir_entry *handoff_proc;

static int __init
oneios_handoff_init(void)
{
	handoff_proc = proc_create("oneios_handoff", 0600, NULL, &handoff_fops);
	if (!handoff_proc) {
		return -ENOMEM;
	}
	LOG("loaded. does nothing until you write to /proc/oneios_handoff. Start with 'probe'.\n");
	return 0;
}

static void __exit
oneios_handoff_exit(void)
{
	if (handoff_proc) {
		proc_remove(handoff_proc);
	}
	LOG("unloaded\n");
}

module_init(oneios_handoff_init);
module_exit(oneios_handoff_exit);
