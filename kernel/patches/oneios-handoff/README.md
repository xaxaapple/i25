# oneios-handoff: native GPU and touch bring-up for the handoff concept (docs/handoff.md)

Not hosted-mode code. Nothing here is linked into `build-linux.sh` or `build-hosted.sh` today, and
nothing in this directory touches the kernel's normal build. It exists for the `handoff` idea
(docs/handoff.md): XNU running with Linux stopped, driving the real silicon itself. Scope right
now, per the owner's request: **just GPU and touch**, not the whole driver list in
docs/handoff.md's table.

## Where this came from

Adapted from the owner's own copy of `akshaynexus/hakios-arm64e-dada`
(github.com/akshaynexus/hakios-arm64e-dada), a bare-metal XNU-on-Xiaomi-15 project, licensed
APSL-2.0 (see that project's NOTICE.md). The Xiaomi 15 ("dada") and the Galaxy S25 ("sun",
SM-S931B) use the same chip, SM8750 ("Snapdragon 8 Elite for Galaxy"), confirmed in docs/s25.md.
That is why the GPU file is a strong starting point and not a guess: the Adreno 830 IP block, its
register layout and its GMU/zap-shader bring-up sequence are the chip's, not Xiaomi's.

| File | From | What |
|---|---|---|
| `handoff_gen8_regs.h` | `bsd/dev/hakios_gen8_regs.h` | Adreno "gen8" (830) register offsets, generated from Linux's own register database. Unmodified content. |
| `handoff_gpu.c` | `bsd/dev/hakios_dada_gpu.c` | The GPU bring-up: power, GMU firmware, GMU start, 3D core power and start, a one-word test submission through the command processor. Logic unmodified; only logging/image-loading/boot-arg names adapted (see the file's own header). |
| `handoff_touch.c` | `bsd/dev/hakios_dada_touch.c` | A Synaptics TouchComm touch controller on SPI: power, clocks, pins, the GENI SPI bus, the TouchComm protocol. Logic unmodified; same kind of adaptation. |
| `handoff_image.c` | `bsd/dev/redwood_image.c` | Reads one firmware file out of a small RAM-resident APFS image carried with the kernel payload. Unmodified except the public function's name. |
| `handoff_log.h/.c`, `handoff_input.h`, `handoff_shim.c` | new, this project | Glue: logging, the touch-sample struct, weak stubs, the one entry point. See each file's header. |

Each adapted file's own top comment says exactly what changed and, separately, what is NOT
confirmed to carry over from the Xiaomi 15 to the Galaxy S25. Read those before trusting an
address or a protocol choice.

## What is confirmed, what is not

**Likely transfers as-is (same SoC):** every register offset in `handoff_gen8_regs.h`; the GCC,
GPUCC, GPU and SMMU **register layouts** in `handoff_gpu.c`; the bring-up order (power -> GMU
firmware -> GMU start -> core power -> core start); the SPI/GENI bus bring-up in `handoff_touch.c`
(clocks, the FIFO protocol) if the S25 also uses a QUP/GENI SPI engine for its touch controller
(very likely on this SoC family, but not confirmed).

**Not confirmed, board- or OEM-specific:**
- `CMD_DB_PHYS` (`0x81c60000`, the AOP command database) and any other address placed by the
  bootloader rather than fixed by the chip.
- the GPU fuse/speed-bin readout -- Samsung may bin the same silicon differently.
- the exact firmware file names (`gen80000_gmu.bin`, `gen80000_zap.mbn`, ...). These must come
  from the S25's own `/vendor/firmware` (never redistributed, same rule as the rest of this
  project) -- the names are very likely identical (Qualcomm's own naming for this GPU generation)
  but that is a guess until checked.
- **the touch controller's identity.** This is the weakest link: touch ICs are an OEM choice, not
  part of the SoC. There is no particular reason to expect Samsung used the same Synaptics
  TouchComm chip as Xiaomi. If it is a different chip (Goodix, FocalTech, Samsung's own
  "sec_ts"/"stm" family are all plausible on Samsung phones), the bus bring-up in
  `handoff_touch.c` can likely still be reused, but the protocol section (identify, report
  parsing) needs rewriting for that chip.

`tools/probe/handoff-probe.sh` decides most of this; see its `gpu`, `cmd_db` *(not yet added --
see below)* and `input` sections.

## What this drop does NOT do

- **It does not perform a handoff jump.** docs/handoff.md's stages 0-2 (probe, payload builder,
  quiesce-and-jump stub) are not built. This code cannot run today; it is written for the EL1
  context a handoff jump would leave XNU in, nothing more.
- **It does not build.** There is no `build-handoff.sh`, no handoff OS/board profile, and the new
  patch (`oneios-handoff-start.patch`) is deliberately left out of the shared
  `kernel/device/patches/xnu-12377/series` so it cannot end up in a hosted build by mistake. A real
  build path (modeled on `kernel/build-hosted.sh`, substituting a handoff series and a new
  `XNU_HANDOFF` macro for `XNU_HOSTED`) was not written here: `build-hosted.sh` drives a Rust tool
  (`tools/oneios-tool`) to generate board archives, and guessing at that tool's archive-generator
  interface without being able to run it would risk silently producing a kernel that fails to link
  or, worse, one that links but touches the wrong registers -- there is no device here to catch
  that safely. Writing `build-handoff.sh` for real is the next step, once the probe's answers are
  in and gate 1/2 of docs/handoff.md are known.
- **No IOKit/HID consumer.** `handoff_shim.c`'s weak stubs only log what the GPU and touch driver
  produce; nothing yet turns a touch sample into an iOS HID event or a GPU submission into a
  drawn frame on the panel. `kernel/patches/oneios-hosted/hid/` is the model for what that would
  look like for touch; nothing equivalent exists for handoff mode yet.
- **Not compiled, anywhere, by anything, in this drop.** There is no XNU source checkout or cross
  toolchain in this container (by this project's own rule: "No Apple files in git or in
  releases"), so these files were checked by hand (header/boot-arg renames, a brace/paren balance
  check) and against `handoff_gpu.c`'s and `handoff_touch.c`'s own, pre-existing logic, not by a
  compiler. The owner's local build (`kernel/build-hosted.sh`'s Linux or macOS toolchain) is the
  first real compile these files will see.

## How to make progress from here, in order

1. Run `tools/probe/handoff-probe.sh` on the phone; read its `input.txt` for the touch driver's
   name (decides whether `handoff_touch.c`'s protocol section applies at all) and its `kexec`/
   `modules`/`dmesg_boot` sections (decides whether a handoff jump is possible at all -- gates 1
   and 2 of docs/handoff.md, which this GPU/touch code depends on just as much as every other
   driver in the table there).
2. If the touch chip is not Synaptics TouchComm, rewrite `handoff_touch.c`'s protocol section
   (identify, application info, touch-report parsing) for the real chip; keep the bus bring-up.
3. Confirm `CMD_DB_PHYS` and the firmware file names against the phone's own device tree and
   `/vendor/firmware` (extend the probe if its current fields do not already show these).
4. Only then does writing `build-handoff.sh` and the actual quiesce-and-jump stub (docs/handoff.md
   stage 2) make sense -- before that, there is nothing to run this code on.
