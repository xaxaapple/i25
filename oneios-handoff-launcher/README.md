# oneios-handoff-launcher

The app half of the handoff concept (docs/handoff.md in the runtime repository): loads
`../kernel/patches/oneios-handoff/km/oneios_handoff.ko` as root and walks through its four
commands (Probe, build payload, Dry run, Jump) with a confirmation dialog in front of the two
that touch real hardware state. Read `MainActivity.kt` and the kernel module's own README before
running anything past Probe.

## Honest status

| Step | State |
|---|---|
| 1. Probe | Real: opens a root shell, loads the module if needed, asks it to probe, also greps `dmesg` for what `probe` itself cannot see (gate 2). |
| 2. Build payload | **Not implemented.** `PayloadBuilder.kt` explains exactly what is missing: a JNI bridge into `runtime/loader`, and, more fundamentally, an actual handoff-mode `kernel.kc` to pack -- which does not exist for any phone in this project yet (there is no `build-handoff.sh`; `kernel/README.md`'s own "Not done" list still includes "Any boot" for the *hosted* kernel). |
| 3. Dry run | Real, and designed to be reversible (`remove_cpu()`/`add_cpu()`, the cooperative Linux hotplug path, not the raw IPI the real jump uses). |
| 4. Jump | Real on the module side, but the module refuses outright with nothing recorded by step 2 -- which is the truthful behaviour today, not a placeholder. |

This app will not make an un-built kernel boot. What it gives you now is a safe, logged way to
find out whether this phone's firmware and kernel even allow the *mechanism* (PSCI CPU_OFF/CPU_ON,
`cpu_soft_restart` present) before any of the driver work in `../kernel/patches/oneios-handoff/`
(GPU, touch) has anywhere to run.

## Building

`.github/workflows/handoff-app.yml` (repository root) builds the debug APK with Gradle. There is
no signing config; install the debug APK with `adb install -r`.

## Permissions

None beyond what talking to `su` needs. No `INTERNET` permission is requested -- the app never
makes a network request; it only runs shell commands the owner can read in `MainActivity.kt`.
