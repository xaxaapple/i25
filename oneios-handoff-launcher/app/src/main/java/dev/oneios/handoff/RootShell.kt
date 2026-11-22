package dev.oneios.handoff

import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter

/**
 * The only privileged primitive this app has: one `su` shell, kept open for the lifetime of a
 * single operation (a fresh `su -c` per command would be simpler but loses the module's own
 * /proc/oneios_handoff read/write pairing if two apps or two taps raced). Every command this app
 * sends is listed in MainActivity -- nothing here is built from untrusted input, and nothing
 * here does network I/O (the manifest declares no INTERNET permission at all).
 */
class RootShell {
    private var process: Process? = null
    private var writer: OutputStreamWriter? = null
    private var reader: BufferedReader? = null

    fun open(): Boolean {
        return try {
            val p = ProcessBuilder("su").redirectErrorStream(true).start()
            process = p
            writer = OutputStreamWriter(p.outputStream)
            reader = BufferedReader(InputStreamReader(p.inputStream))
            true
        } catch (e: Exception) {
            false
        }
    }

    /** Runs one command and returns everything it printed up to a sentinel line, so the caller
     * does not have to guess when a command is "done" (shell output has no natural end marker). */
    fun run(command: String): String {
        val w = writer ?: return "(no root shell open)"
        val r = reader ?: return "(no root shell open)"
        val sentinel = "__ONEIOS_HANDOFF_DONE_${System.nanoTime()}__"
        w.write("$command; echo $sentinel\n")
        w.flush()
        val out = StringBuilder()
        while (true) {
            val line = r.readLine() ?: break
            if (line == sentinel) break
            out.append(line).append('\n')
        }
        return out.toString()
    }

    fun close() {
        try {
            writer?.write("exit\n"); writer?.flush()
        } catch (_: Exception) {}
        process?.destroy()
    }
}
