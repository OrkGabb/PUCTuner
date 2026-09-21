package com.orkgabb.m54tuner.root

import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Single entry point for every root shell interaction in the app.
 *
 * Root here is expected to be *temporary* (survives only until reboot, per the user's own
 * grant flow). We therefore never cache a "root granted" boolean across launches — every call
 * to [hasRoot] re-checks the shell for real, and the result of that check is what decides
 * whether a profile gets re-applied on app start.
 */
object RootShellManager {

    init {
        Shell.enableVerboseLogging = false
        Shell.setDefaultBuilder(
            Shell.Builder.create()
                .setFlags(Shell.FLAG_REDIRECT_STDERR)
                // Dexopt and verified service restarts can legitimately take longer.
                .setTimeout(300)
        )
    }

    /**
     * Attempts to acquire the root shell right now. Returns false (never throws) if the
     * temp-root grant is not currently active, if the user denies the su prompt, or if
     * anything else goes wrong acquiring the shell.
     */
    suspend fun hasRoot(): Boolean = withContext(Dispatchers.IO) {
        try {
            val shell = Shell.getShell()
            shell.isRoot
        } catch (e: CancellationException) {
            throw e
        } catch (_: Exception) {
            false
        }
    }

    /**
     * Runs a shell script (root context) and returns its combined stdout lines.
     * Throws nothing: LibSU can throw when the shell dies mid-call (denied grant, killed
     * host), so anything it raises is folded into an unsuccessful result instead of
     * propagating into — and permanently killing — the polling coroutine and friends.
     * Callers must still check [ShellRunResult.isSuccess].
     */
    suspend fun run(script: String): ShellRunResult = withContext(Dispatchers.IO) {
        runCatching {
            // A subshell per call. LibSU writes the script into ONE persistent root shell and
            // then `__RET=$?; echo <marker>`, so a top-level `exit` killed that shell before the
            // marker: the output was lost and a module probe read "not installed". Inside `( )`
            // an `exit N` ends only this call and becomes its exit code, and nothing a script
            // sources (lib.sh's umask and variables) leaks into the next call.
            val result = Shell.cmd("(\n$script\n)").exec()
            ShellRunResult(
                isSuccess = result.isSuccess,
                exitCode = result.code,
                output = result.out,
            )
        }.getOrElse {
            // Cancellation must still cancel: folding it into a failure result would leave the
            // polling coroutine unkillable on ViewModel teardown.
            if (it is CancellationException) throw it
            ShellRunResult(
                isSuccess = false,
                exitCode = -1,
                output = listOf("shell exception: ${it.message}"),
            )
        }
    }
}

data class ShellRunResult(
    val isSuccess: Boolean,
    val exitCode: Int,
    val output: List<String>,
)
