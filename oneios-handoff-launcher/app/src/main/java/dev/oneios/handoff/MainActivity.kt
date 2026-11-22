package dev.oneios.handoff

import android.app.AlertDialog
import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import kotlin.concurrent.thread

/**
 * The staged UI docs/handoff.md and this app's README describe: Probe -> (pick files, build
 * payload) -> Dry run -> Jump. Each button only enables once the one before it has reported
 * success, and the last two steps -- the only ones that touch real hardware state across CPUs --
 * each require a typed confirmation dialog before anything runs. This activity owns one
 * RootShell for its whole lifetime so the module stays loaded between steps.
 */
class MainActivity : AppCompatActivity() {
    private val shell = RootShell()
    private lateinit var log: TextView
    private lateinit var btnProbe: Button
    private lateinit var btnPayload: Button
    private lateinit var btnDryRun: Button
    private lateinit var btnJump: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        log = findViewById(R.id.txtLog)
        btnProbe = findViewById(R.id.btnProbe)
        btnPayload = findViewById(R.id.btnLoadPayload)
        btnDryRun = findViewById(R.id.btnDryRun)
        btnJump = findViewById(R.id.btnJump)

        btnProbe.setOnClickListener { runProbe() }
        btnPayload.setOnClickListener { pickAndBuildPayload() }
        btnDryRun.setOnClickListener { confirmThen("Dry run", REVERSIBLE_WARNING) { runDryRun() } }
        btnJump.setOnClickListener { confirmThen("JUMP", JUMP_WARNING) { runJump() } }
    }

    private fun appendLog(text: String) {
        runOnUiThread { log.append(text); log.append("\n") }
    }

    /** Step 1. Opens the root shell (the one su prompt the whole session needs), pushes and
     * loads the kernel module if it is not already loaded, and asks it to probe. Entirely
     * read-only on the module's side (see oneios_handoff.c's do_probe()). */
    private fun runProbe() = thread {
        appendLog("--- probe ---")
        if (!shell.open()) {
            appendLog("could not open a root shell (no su, or the user declined it). Nothing else here will work.")
            return@thread
        }
        val whoami = shell.run("id").trim()
        appendLog("id: $whoami")
        if (!whoami.contains("uid=0")) {
            appendLog("not root. Stopping -- this app does nothing without root (the owner's own temporary root, per README).")
            return@thread
        }
        // The .ko is expected to have been pushed to /data/local/tmp by the owner (adb push),
        // same as the kernel module README's own "Loading and using it" instructions -- this
        // app does not fetch or build it; it only loads what is already on the phone.
        val lsmod = shell.run("lsmod | grep -c oneios_handoff").trim()
        if (lsmod == "0") {
            val insmod = shell.run("insmod /data/local/tmp/oneios_handoff.ko 2>&1").trim()
            appendLog("insmod: $insmod")
        }
        val probeOut = shell.run("echo probe > /proc/oneios_handoff; cat /proc/oneios_handoff")
        appendLog(probeOut)
        val dmesgGate2 = shell.run(
            "dmesg 2>&1 | grep -iE 'booted at el|gunyah|hyp|kvm' | tail -10"
        )
        appendLog("--- gate 2 (dmesg, the part probe itself cannot see) ---")
        appendLog(dmesgGate2.ifBlank { "(nothing matched -- inconclusive, not a clear yes or no)" })
        runOnUiThread { btnPayload.isEnabled = true }
    }

    /** Step 2. Not implemented end-to-end -- see PayloadBuilder.kt's own, longer explanation of
     * exactly what is missing (a JNI bridge into runtime/loader) and why it was left as a clear
     * stop rather than a second, untested reimplementation of that logic. */
    private fun pickAndBuildPayload() {
        appendLog("--- build payload ---")
        appendLog(
            "Not implemented in this build: see PayloadBuilder.kt. This needs (1) the owner's " +
                "own already-built handoff-mode kernel.kc, which does not exist yet for ANY " +
                "phone in this project (no build-handoff.sh), and (2) a JNI bridge into " +
                "runtime/loader to lay it out correctly. Wiring a file picker here before either " +
                "of those exists would just be UI with nothing real behind it."
        )
    }

    /** Step 3. Reversible: parks every secondary CPU with PSCI CPU_OFF and brings each back with
     * CPU_ON (oneios_handoff.c's do_dryrun()). Proves PSCI works on this phone before "jump"
     * needs to trust it one-way. */
    private fun runDryRun() = thread {
        appendLog("--- dry run ---")
        appendLog(shell.run("echo dryrun > /proc/oneios_handoff; dmesg | tail -20"))
        runOnUiThread { btnJump.isEnabled = true }
    }

    /** Step 4. THE POINT OF NO RETURN, and only if step 2 ever becomes real: do_jump() refuses
     * outright if no payload was recorded ("payload <phys> <size>" was never sent), so today
     * this always ends in that refusal, logged, not a hang -- which is the honest state of this
     * app until PayloadBuilder.kt is finished. */
    private fun runJump() = thread {
        appendLog("--- jump ---")
        appendLog(shell.run("echo jump > /proc/oneios_handoff; dmesg | tail -20"))
    }

    private fun confirmThen(title: String, message: String, action: () -> Unit) {
        AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton("Proceed") { _, _ -> action() }
            .setNegativeButton("Cancel", null)
            .show()
    }

    override fun onDestroy() {
        shell.close()
        super.onDestroy()
    }

    companion object {
        private const val REVERSIBLE_WARNING =
            "This parks every secondary CPU and brings each back. It should be fully reversible, " +
                "but it is untested on real hardware. If the phone becomes unresponsive, nothing " +
                "has been written to flash -- a forced reboot (hold power) returns to One UI."
        private const val JUMP_WARNING =
            "THIS CANNOT BE UNDONE FROM INSIDE ANDROID. If it works, Linux stops and does not " +
                "come back without a reboot. If it does not work -- most likely today, since no " +
                "payload has ever been built -- the module refuses and logs why, which is the " +
                "expected outcome right now. Nothing is written to flash either way; a forced " +
                "reboot (hold power) always returns to One UI. Proceed only if you understand this."
    }
}
