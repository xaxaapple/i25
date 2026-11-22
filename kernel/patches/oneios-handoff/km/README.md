# oneios_handoff: the kernel module

`oneios_handoff.c` is the kernel-mode half of the handoff concept: once loaded (as root, by the
`oneios-handoff-launcher` app or by hand), it does nothing until told to, one command at a time,
through `/proc/oneios_handoff`. Read the module's own top comment for what each command does;
this file is about building and loading it, and about what it genuinely cannot promise.

## Which kernel source

A kernel module is only as loadable as its build matches the running kernel's Kernel Module
Interface (KMI). Samsung's own source for the S25's exact kernel is not something this project
redistributes or needs: Google's GKI program guarantees a **stable KMI within one branch**
(`android15-6.6` for any device that shipped on Android 15 with kernel 6.6, which `S931BXXSBCZG3`
/ kernel `6.6.98` is), built from a single `gki_defconfig`, with a pinned Clang. This module uses
only symbols on that stable list except two (`cpu_soft_restart`, and `kallsyms`-style resolution
in general), which are not on any stable list for any kernel branch and are reached the same way
every out-of-tree module has since they stopped being exported in 5.7: `register_kprobe()` by
name (see the module's own comment; this is documented, widely used, and NOT specific to GKI).

**This does not guarantee the module loads.** `insmod` on a GKI kernel checks the build's
`vermagic` and, if the kernel enforces it, a KMI symbol-list match; Samsung may also carry
additional lockdown. `probe`'s own log line about whether loading succeeded at all is the first
real answer; nothing here claims it in advance.

## Building (`.github/workflows/handoff-km.yml`)

Clones `android.googlesource.com/kernel/common` at `android15-6.6`, configures `gki_defconfig`
for `arm64`, and builds only this module (`make M=... modules`) against that tree's headers,
with the pinned Clang/LLVM toolchain the branch's own build config names (not a guessed one).
Output: `oneios_handoff.ko`, as a workflow artifact. **Untested beyond `make`'s own exit code**:
there is no device in CI to insmod it on, and there will not be one -- that is what `probe`, run
on the actual phone, is for.

## Loading and using it

```
adb push oneios_handoff.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/oneios_handoff.ko'
adb shell su -c 'echo probe > /proc/oneios_handoff; cat /proc/oneios_handoff'
```
Everything past `probe` is the launcher app's job (it also runs `probe` itself, as the in-app
preflight the owner asked to fold into the app rather than a separate script). See
`../oneios-handoff-launcher/README.md`.

## What loading and running `probe` does NOT prove

- That `dryrun` or `jump` will work. `probe` only resolves `cpu_soft_restart` and reports the
  CPU count; it cannot see gate 2 (what Gunyah enforces), which stays the open question
  `tools/probe/handoff-probe.sh`'s `dmesg_boot.txt` was already written to answer (run that too).
- That a payload placed at some physical address is a working handoff-mode XNU. Nothing exists
  yet that produces one (there is no `build-handoff.sh`; see
  `../README.md` and `../oneios-handoff-launcher/README.md`). `jump` will branch to whatever is
  there, correct or not.
