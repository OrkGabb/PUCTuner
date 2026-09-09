package com.orkgabb.m54tuner.system

import com.orkgabb.m54tuner.root.RootShellManager
import java.util.UUID

/**
 * The app's only channel to the tuning layer: it writes `/data/adb/m54tuner/config`, runs one of
 * the module's tier scripts, and reads back what actually stuck.
 *
 * Each entry point maps to exactly one tier, and the tier decides the disruption budget:
 * [applyLive] never restarts anything, [applyRender] force-stops only the chosen apps unless the
 * user explicitly accepted a SurfaceFlinger restart, [applyMem] touches swap, [applyDexopt] is the
 * only slow one. Nothing here ever runs a heavier tier than the change requires.
 */
object ModuleBridge {

    private const val DATA_DIR = "/data/adb/m54tuner"
    const val CONFIG = "$DATA_DIR/config"
    private const val RESULT = "$DATA_DIR/result"

    // Prefer the active temp-root mount. A staged update must not shadow a hot-installed module.
    private val MODULE_DIRS = listOf(
        "/data/adb/ksu/modules/m54tuner",
        "/data/adb/modules/m54tuner",
        "/data/adb/modules_update/m54tuner",
    )

    private suspend fun scriptDir(): String? {
        // Resolve on each operation: the live mount can change without an app/kernel restart.
        val probe = MODULE_DIRS.joinToString(" ")
        val r = RootShellManager.run(
            "for d in $probe; do [ -f \"\$d/scripts/apply_profile.sh\" ] && echo \"\$d\" && break; done"
        )
        return r.output.map { it.trim() }.firstOrNull { it.isNotEmpty() }
    }

    // ---------------- state ----------------

    /**
     * One root round trip for the whole live device state. [full] pulls in the Samsung/MARs section
     * too — 9 `content query`/`settings get` subprocess spawns, each forking a JVM. Fine once, but
     * doing it on every 3-second poll (and on every launch) is what made the app feel slow for data
     * that barely changes, so callers default to the fast path and ask for [full] only when that
     * section's values actually matter right now (first paint after boot, an explicit refresh, or a
     * Samsung/MARs toggle).
     */
    suspend fun status(full: Boolean = false): DeviceState {
        val dir = scriptDir() ?: return DeviceState(moduleInstalled = false)
        val arg = if (full) " --full" else ""
        val r = RootShellManager.run("sh \"$dir/scripts/status.sh\"$arg")
        return DeviceState.parse(r.output)
    }

    suspend fun readConfig(): TunerConfig =
        TunerConfig.parse(RootShellManager.run("cat \"$CONFIG\" 2>/dev/null").output)

    suspend fun writeConfig(cfg: TunerConfig): Boolean {
        val script = buildString {
            appendLine("umask 077")
            appendLine("mkdir -p \"$DATA_DIR\"")
            appendLine("chmod 0700 \"$DATA_DIR\"")
            appendLine("tmp=\"$CONFIG.tmp.${'$'}${'$'}\"")
            appendLine("cat > \"${'$'}tmp\" <<'M54EOF'")
            append(cfg.serialize())
            appendLine("M54EOF")
            appendLine("chmod 0600 \"${'$'}tmp\" && mv -f \"${'$'}tmp\" \"$CONFIG\"")
        }
        return RootShellManager.run(script).isSuccess
    }

    private suspend fun readReport(): ApplyReport =
        ApplyReport.parse(RootShellManager.run("cat \"$RESULT\" 2>/dev/null").output)

    private suspend fun runTier(script: String, args: String = ""): ApplyReport? {
        val dir = scriptDir() ?: return null
        val op = UUID.randomUUID().toString().replace("-", "")
        val resultFile = "$DATA_DIR/result.$op"
        val run = RootShellManager.run(
            "M54_OP_ID=$op sh \"$dir/scripts/$script\" $args; " +
                "code=${'$'}?; cat \"$resultFile\" 2>/dev/null; rm -f \"$resultFile\"; exit ${'$'}code"
        )
        return ApplyReport.parse(run.output)
    }

    // ---------------- tiers ----------------

    /** Live tier: CPU/GPU/MIF/IO/PELT/thermal/VM/GOS. Effective immediately, restarts nothing. */
    suspend fun applyLive(): ApplyReport? = runTier("apply_profile.sh")

    /**
     * Render tier. With [restartSf] false the props are set and only the chosen apps are
     * force-stopped; anything that needs SurfaceFlinger is left flagged as pending so the UI can
     * ask for it once, instead of blanking the screen behind the user's back on every toggle.
     */
    suspend fun applyRender(restartSf: Boolean = false, force: Boolean = false): ApplyReport? =
        runTier("apply_render.sh", buildString {
            if (restartSf) append("--sf ")
            if (force) append("--force")
        }.trim())

    /**
     * ART/zygote tier. These props are read once by zygote, so writing them changes nothing until
     * the framework restarts: with [softReboot] false the module only records that a soft reboot is
     * pending, and the UI asks. A zygote restart closes every open app — it is never implicit.
     */
    suspend fun applyArt(softReboot: Boolean = false): ApplyReport? =
        runTier("apply_art.sh", if (softReboot) "--soft" else "")

    /** Samsung's MARs background-restriction system: per-app exemption plus the systemic switches. */
    suspend fun applyProtect(): ApplyReport? = runTier("apply_protect.sh")

    /** Samsung's own limiter switches (settings tables, not sysfs). Reversible from a backup. */
    suspend fun applySamsung(): ApplyReport? = runTier("apply_samsung.sh")

    /** Memory tier: zram algorithm rebuild + swappiness. Guarded against OOM inside the module. */
    suspend fun applyMem(): ApplyReport? = runTier("apply_mem.sh")

    /** Dexopt tier: selected ART compilation mode over the chosen games. Explicit and potentially slow. */
    suspend fun applyDexopt(reset: Boolean = false): ApplyReport? =
        runTier("apply_dexopt.sh", if (reset) "--reset" else "")

    /** Manual "Limpar agora": one-shot background/cached-app kill, on demand. */
    suspend fun applyClearRam(): ApplyReport? = runTier("clear_ram.sh")

    /** The whole boot sequence: render props (+one SF restart) → memory → live profile. */
    suspend fun applyBoot(): ApplyReport? {
        val dir = scriptDir() ?: return null
        RootShellManager.run("sh \"$dir/post-fs-data.sh\"; sh \"$dir/service.sh\"")
        return readReport()
    }

    /**
     * True exactly once per boot session (marker in /dev, a tmpfs that clears on reboot). With
     * temp-root the module never runs at boot, so the first app launch of a session is what
     * restores everything; every later launch takes the cheap path.
     */
    suspend fun consumeFirstOfSession(): Boolean {
        val r = RootShellManager.run(
            "if [ -e /dev/.m54tuner_session ]; then echo N; exit; fi; " +
                "now=\$(cut -d. -f1 /proc/uptime); " +
                "old=\$(cat /dev/.m54tuner_app_claim/at 2>/dev/null); " +
                "case \$old in ''|*[!0-9]*) old=0;; esac; " +
                "[ \$((now-old)) -gt 120 ] && rm -rf /dev/.m54tuner_app_claim; " +
                "if mkdir /dev/.m54tuner_app_claim 2>/dev/null; then " +
                "echo \$now > /dev/.m54tuner_app_claim/at; echo Y; else echo N; fi"
        )
        return r.output.any { it.trim() == "Y" }
    }

    /** Starts the background-protection watcher detached; it must outlive this shell call. */
    suspend fun startKeepalive(): Boolean {
        val dir = scriptDir() ?: return false
        return RootShellManager.run(
            "nohup sh \"$dir/scripts/keepalive.sh\" </dev/null >/dev/null 2>&1 &"
        ).isSuccess
    }

    suspend fun stopKeepalive(): Boolean {
        val dir = scriptDir() ?: return false
        return RootShellManager.run("sh \"$dir/scripts/keepalive_stop.sh\"").isSuccess
    }

    suspend fun exportDiagnostics(): String? {
        val dir = scriptDir() ?: return null
        val r = RootShellManager.run("sh \"$dir/scripts/diagnostics.sh\"")
        return r.output.lastOrNull { it.contains("M54Tuner-diagnostics.txt") }?.trim()
    }

    // ---------------- automatic sweep ----------------

    /**
     * Starts the timing sweep detached: it runs for minutes while the user plays, so the shell call
     * must return immediately instead of blocking the UI for the whole session.
     */
    suspend fun startBench(stepSeconds: Int = 90): Boolean {
        val dir = scriptDir() ?: return false
        return RootShellManager.run(
            "nohup sh \"$dir/scripts/bench.sh\" $stepSeconds </dev/null >/dev/null 2>&1 &"
        ).isSuccess
    }

    /** Starts the interleaved 3×2 PELT/EAS experiment; the module restores live values on exit. */
    suspend fun startSchedBench(stepSeconds: Int = 45, rounds: Int = 2): Boolean {
        val dir = scriptDir() ?: return false
        return RootShellManager.run(
            "nohup sh \"$dir/scripts/bench_sched.sh\" $stepSeconds $rounds </dev/null >/dev/null 2>&1 &"
        ).isSuccess
    }

    suspend fun stopSchedBench(): Boolean {
        val dir = scriptDir() ?: return false
        return RootShellManager.run("sh \"$dir/scripts/bench_stop.sh\" sched").isSuccess
    }

    /** "done", or "label|dvfs|polling|js|seconds" for the step in progress, or "" if never run. */
    suspend fun benchState(): String =
        RootShellManager.run("cat \"$DATA_DIR/bench_state\" 2>/dev/null").output
            .firstOrNull()?.trim().orEmpty()

    suspend fun benchRows(): List<BenchRow> =
        RootShellManager.run("cat \"$DATA_DIR/bench.csv\" 2>/dev/null").output
            .mapNotNull { BenchRow.parse(it) }

    /** Tail of the module log, for the diagnostics sheet. */
    suspend fun readLog(lines: Int = 120): List<String> =
        RootShellManager.run("tail -n $lines \"$DATA_DIR/m54tuner.log\" 2>/dev/null").output
}
