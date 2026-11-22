# Handoff: XNU replaces One UI's kernel in RAM (design, status: probe stage)

Goal: start XNU while Android is running, then stop Linux completely, so that only XNU is left, the same end state
as a boot with an unlocked bootloader, reached by another way of execution (nothing flashed, bootloader untouched,
a reboot always returns to One UI).

**Rule for this work: the current `hosted` mode (xnuhost as a Linux process) stays the default and keeps building
exactly as now. `handoff` is a second, opt-in mode. Nothing that works (Metal, sound, Wi-Fi) is changed or removed
until a handoff run has shown the same thing working.**

## Why this is not a drop-in change

In `hosted` mode everything below XNU is borrowed from the Linux kernel that keeps running: KGSL (Adreno 830),
AAudio/audioserver (sound), wlan0 + wpa_supplicant (Wi-Fi), surfaceflinger (the panel), evdev (touch), the block
device (the image set). After a handoff none of that exists. XNU has to drive the silicon itself:

| Part | Hosted today | Needed in handoff |
|---|---|---|
| CPU bring-up, GICv3, timer | emulated by xnuhost | real: PSCI (SMC to firmware), GIC redistributors, arch timer, XNU's own EL1 setup (`ArmDTPlatform` is in the tree) |
| Memory | host mmap | a carve-out Linux gives up; XNU's own page tables (4 KiB, 39/48-bit VA, `0027` assumes the S24's 39) |
| Storage | host file | UFS controller driver (or: keep the image in RAM / a loop of the carve-out for a first boot) |
| GPU | drm2kgsl over KGSL | native Adreno 830 driver: clocks, GDSC, SMMU context, GMU firmware, ringbuffer. Sources adapted and in the tree now: `kernel/patches/oneios-handoff/` (see its README for status). The hosted Metal path (Turnip on KGSL) staying usable on top of a native driver is a separate, later question |
| Display | surfaceflinger layer | DPU + DSI panel bring-up (panel init sequence is Samsung's) |
| Sound | AAudio | ADSP via GLINK/GPR, AudioReach graphs, amplifiers |
| Wi-Fi | wlan0 | ath12k/WCN firmware load through the modem/Wi-Fi subsystem (remoteproc, QMI) |
| Touch, keys, haptics, battery | evdev/sysfs bridges | SPI/I2C touch, PMIC over SPMI, haptic chip |

Each row is a driver port. This is the same list as for any bare-metal XNU-on-Snapdragon port.

## The two gates that decide whether it is possible at all

1. **EL1 code execution inside the running kernel.** A handoff is a `kexec`: One UI's kernel has none (`CONFIG_KEXEC`
   is not set on Android GKI), so something must run in kernel mode, quiesce the other CPUs, turn off the MMU,
   and jump to XNU's entry with the boot arguments in `x0`. The carrier can only be a kernel module or the root exploit's
   own primitive. Needed from the phone: are modules loadable (`modules_disabled`, `MODULE_SIG_FORCE`, KernelSU LKM),
   do `kallsyms` addresses show (`kptr_restrict`), do `set_memory_x` / `cpu_soft_restart` exist.
2. **What sits above Linux.** Gunyah (EL2) owns stage-2 translation and the SMMUs on this SoC. If the kernel is booted at
   EL1 under it (`dmesg`: "Booted at EL1" / "kvm: ... not available"), XNU becomes the guest of the same hypervisor: it
   may touch only memory and devices that Gunyah maps for the primary VM, and hardware that Linux reaches by SMC (clocks,
   regulators, GPU power, Wi-Fi/ADSP firmware loading) must be reached by the same SMCs. Gunyah refuses an unsigned *guest
   VM* (docs/architecture.md), but a replacement of the *primary VM's* kernel is not a guest; whether Samsung's
   hypervisor-enforced kernel protection (stage-2 write-protection of kernel text, page-table updates through hypercalls)
   allows the jump is unknown until the probe has run.

`tools/probe/handoff-probe.sh` (read-only) answers both, plus the memory map, reserved regions and what Linux uses for
every device above. It ships in the `s25-phone-kit` artifact. **Run it first; its output decides which stage is next.**

## Stages

| Stage | What | Risk | State |
|---|---|---|---|
| 0 | Probe (`handoff-probe.sh`): gates 1 and 2, memory map, device inventory | none, read-only | **built, in CI** |
| 1 | `handoff` payload builder: kernel.kc + device tree (`runtime/loader`, already ported from DarwinBoot) + boot_args packed to one blob and placed in a RAM carve-out | none (no jump) | next, after stage 0 |
| 2 | Quiesce + jump stub (position independent, built next to `runtime/xnuhost/stub`), first run only to a **serial/RAM log** (XNU's `uart.c` model) and a return to reset | phone reboots on failure; nothing is written to flash | needs gate 1 |
| 3 | XNU boots to launchd from a RAM-resident image, no GPU, no sound, no Wi-Fi | as above | needs gate 2 |
| 4..n | GPU and touch, native bring-up: `kernel/patches/oneios-handoff/` | as above | **sources written, adapted from a same-SoC project; not pluggable into a kernel build yet — see its README** |
| 4..n | storage, Wi-Fi, sound, display composition | as above | not started |

Until stage 3 has run on the phone, hosted mode stays the product.

## The launcher app and the kernel module (the actual jump mechanism)

`oneios-handoff-launcher/` (an Android app, root-gated, no network permission) and
`kernel/patches/oneios-handoff/km/oneios_handoff.ko` (a kernel module it loads) are stage 2's
quiesce-and-jump stub, written for real this time rather than only designed:
- `oneios_handoff.ko`'s `probe` resolves `cpu_soft_restart` (the same arm64 kexec/hibernate
  primitive discussed above) **from inside the kernel**, which is a stronger answer to gate 1 than
  `tools/probe/handoff-probe.sh` could give from a shell script.
- `dryrun` parks and un-parks every secondary CPU through Linux's own cooperative hotplug path
  (`remove_cpu()`/`add_cpu()`) — reversible, and meant to prove PSCI CPU_OFF/CPU_ON work on this
  phone's firmware before `jump` ever has to trust them one-way.
- `jump` is the actual quiesce-then-branch: every other CPU off via a raw PSCI SMC (not Linux's
  hotplug, since Linux's own bookkeeping cannot be relied on by the time this runs), interrupts
  masked, then `cpu_soft_restart(0, entry_phys, boot_args_phys, 0)` — which disables this core's
  MMU and D-cache and branches to `entry_phys` in physical address space, matching
  `kernel/README.md`'s "Entry state the kernel expects today" row exactly.
- The app walks Probe → build payload → Dry run → Jump, with a typed confirmation dialog in
  front of the last two.

**The one piece genuinely missing is the payload itself.** `jump` refuses outright — logged, not
a hang — until something has written a header + an actual handoff-mode XNU kernel image to a
physical address and told the module about it via `payload <phys> <size>`. That requires a
handoff-mode `kernel.kc`, which **does not exist for any phone in this project**: there is still
no `build-handoff.sh`, and `kernel/README.md`'s own "Not done" list includes "Any boot" for the
*hosted* kernel this project already has working pieces of. `PayloadBuilder.kt` and both
directories' own README.md files say this in full; it is not hidden in this summary.

Neither of gate 2 (what Gunyah enforces — `probe`'s own `dmesg` grep is the best this app can do
without a device) nor the GPU/touch registers below have been checked against a real probe run on
this S25 yet. Building the mechanism and confirming it are two different, both still open, steps.

## GPU and touch (ahead of the rest, by request)

`kernel/patches/oneios-handoff/` adapts `akshaynexus/hakios-arm64e-dada`'s native Adreno 830
bring-up and its Synaptics TouchComm touch driver (APSL-2.0). The Xiaomi 15 that project targets
and the Galaxy S25 share the same SM8750 silicon (docs/s25.md), so the GPU register layout and
bring-up sequence are expected to transfer; the touch chip identity does not transfer by the same
logic (touch ICs are an OEM choice, not part of the SoC) and is unconfirmed. Full detail, including
exactly what is and is not confirmed and what is still missing before this can run on a phone (a
build path, a jump stub to run it from), is in that directory's own README.md — read it before
assuming "written" means "works".

## What was not done, honestly

- No handoff code has been written beyond the probe: without the probe's answer a jump stub would be guesswork on a
  device that cannot be debugged over JTAG, and a wrong guess is a hang and a reboot, with no information gained.
- Nothing here has run on a phone. The probe itself is untested on-device (it only reads).
- Prior art: ios-sweet and hakios do the handoff with an **unlocked** bootloader (`fastboot boot` of a shim, or
  UEFI); no public project was found that does the in-RAM replacement from a running locked Samsung. The quoted
  description (execute XNU while Android runs, then stop Linux) was not found at its source; the mechanism is
  kexec-like, as above.
