package dev.oneios.handoff

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Builds the one blob `oneios_handoff.ko`'s "jump" command expects (see that module's do_jump()
 * comment): a 24-byte header (magic, version, entry_phys, boot_args_phys) followed by the
 * owner's OWN already-built handoff-mode kernel image and device tree, laid out and relocated by
 * this project's existing runtime/loader code (xnuloader/boot.h, machine.h) -- NOT reimplemented
 * here. This class only calls out to that: see buildViaXnuloader() below for exactly what is and
 * is not wired up yet.
 *
 * What this file does NOT do: supply the kernel.kc or the iOS root filesystem. Per
 * kernel/README.md ("Not done: ... 3. The prelink ... 5. Any boot.") even the hosted kernel's
 * prelink and first boot are not finished in this repository as committed, and a bare-metal
 * ("handoff") kernel variant does not exist at all yet (no build-handoff.sh). This class packs
 * whatever kernel.kc the owner picks; it cannot make that file be a working handoff kernel if one
 * was never built.
 */
object PayloadBuilder {
    private const val MAGIC = 0x4f484a50 // 'OHJP', little-endian on disk to match the module's readl()
    private const val HEADER_SIZE = 24L

    data class Result(val ok: Boolean, val message: String, val outFile: File? = null,
                       val size: Long = 0)

    /**
     * kernelKc: the owner's own built handoff-mode kernel (Mach-O, per kernel/README.md's "What
     *           the booter gets" table -- NOT the hosted kernel, which expects a completely
     *           different entry contract (runtime/xnuhost/include/xnuhost/hostcall.h) and must
     *           never be jumped to this way).
     * deviceTree: Apple-format device tree blob (runtime/loader already builds one from Droid-
     *             VM's make-droidvm-adt.py per kernel/README.md; this call site does not rebuild
     *             it, it expects the owner to supply the one their kernel build already produced).
     * loadPhys: the physical address the launcher has reserved for this blob (from "probe": a
     *           region gate 1's memory map shows as free and not already a Linux reservation --
     *           picking that address safely is not yet implemented here; see "Not wired up" below).
     */
    fun build(kernelKc: File, deviceTree: File, loadPhys: Long, outFile: File): Result {
        if (!kernelKc.exists() || !deviceTree.exists()) {
            return Result(false, "kernel.kc or device tree file does not exist")
        }
        return buildViaXnuloader(kernelKc, deviceTree, loadPhys, outFile)
    }

    /**
     * NOT WIRED UP: this should call into runtime/loader (xl_ramdisk_layout, the macho.c
     * relocator, boot.c's boot_args filler) through a small JNI bridge, so that the exact same
     * code this project already has (and that its own tests, runtime/loader/tests/test_loader.c,
     * already exercise) decides the layout -- not a second implementation written here that
     * could disagree with it. That JNI bridge does not exist yet (it is a CMake/NDK target that
     * would need to be added to app/build.gradle.kts's externalNativeBuild, pulling in
     * runtime/loader/src/*.c). Writing it blind, without a handoff kernel.kc to test it against,
     * risks exactly the kind of silent mismatch (an off-by-one in boot_args's offsets, say) that
     * only shows up as a hang with nothing to debug it with -- so this stays a clear stop sign
     * instead of a fake implementation that "works" only because nothing has ever run it.
     */
    private fun buildViaXnuloader(kernelKc: File, deviceTree: File, loadPhys: Long, outFile: File): Result {
        return Result(
            false,
            "Not implemented: PayloadBuilder needs a JNI bridge into runtime/loader " +
                "(xnuloader/boot.h, machine.h) to lay out kernel.kc + the device tree + boot_args " +
                "correctly. See this function's own comment for exactly what is missing and why " +
                "it was not stubbed out with a second, untested implementation instead."
        )
    }

    /**
     * The one piece of this file that IS complete and in use today: writing the 24-byte header
     * the kernel module reads. Kept separate from buildViaXnuloader so that once that function is
     * real, nothing here needs to change.
     */
    fun writeHeader(out: RandomAccessFile, entryPhys: Long, bootArgsPhys: Long) {
        val b = ByteBuffer.allocate(HEADER_SIZE.toInt()).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(MAGIC)
        b.putInt(1) // header version
        b.putLong(entryPhys)
        b.putLong(bootArgsPhys)
        out.seek(0)
        out.write(b.array())
    }
}
