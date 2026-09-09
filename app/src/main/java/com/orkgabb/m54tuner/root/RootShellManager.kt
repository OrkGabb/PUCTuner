package com.orkgabb.m54tuner.root

import com.topjohnwu.superuser.Shell
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
        runCatching {
            val shell = Shell.getShell()
            shell.isRoot
        }.getOrDefault(false)
    }

    /**
     * Runs a shell script (root context) and returns its combined stdout lines.
     * Throws nothing: on failure the result list will simply reflect stderr (redirected to
     * stdout) and callers should check [ShellRunResult.isSuccess].
     */
    suspend fun run(script: String): ShellRunResult = withContext(Dispatchers.IO) {
        val result = Shell.cmd(script).exec()
        ShellRunResult(
            isSuccess = result.isSuccess,
            exitCode = result.code,
            output = result.out,
        )
    }
}

data class ShellRunResult(
    val isSuccess: Boolean,
    val exitCode: Int,
    val output: List<String>,
)
