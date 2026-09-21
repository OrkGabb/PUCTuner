package com.orkgabb.m54tuner.system

import com.orkgabb.m54tuner.profile.Profile
import com.orkgabb.m54tuner.profile.ThermalMode

/** GOS preference written to the module config. */
enum class GosPref(val cfg: String) {
    UNTOUCHED("untouched"),
    DISABLED("disabled"),
    ENABLED("enabled"),
}

/**
 * Which module script has to run for a given change, cheapest first. The tier decides both what the
 * app executes and how disruptive it is allowed to be — this is what keeps a renderer toggle from
 * dragging a SurfaceFlinger restart (and zram, and dexopt) along with it.
 */
enum class Tier { LIVE, RENDER, ART, MEM, DEXOPT }

/**
 * Tri-state for a prop the firmware already ships with a value. "off" is NOT "don't touch": several
 * of these are ON from the factory (debug.sf.latch_unsignaled is `true` here), so reverting has to
 * write the recorded factory value back, and AUTO has to mean "never write at all".
 */
object TriState {
    const val AUTO = "auto"
    const val ON = "on"
    const val OFF = "off"
}

private fun Profile.toCfg() = when (this) {
    Profile.GAME -> "game"
    Profile.BALANCED -> "balanced"
    Profile.POWER_SAVE -> "powersave"
    Profile.NONE -> "none"
}

private fun profileFromCfg(s: String) = when (s.trim().lowercase()) {
    "game" -> Profile.GAME
    "balanced" -> Profile.BALANCED
    "powersave" -> Profile.POWER_SAVE
    else -> Profile.NONE
}

private fun ThermalMode.toCfg() = if (this == ThermalMode.AGGRESSIVE) "aggressive" else "moderate"
private fun thermalFromCfg(s: String) =
    if (s.trim().lowercase() == "aggressive") ThermalMode.AGGRESSIVE else ThermalMode.MODERATE

/**
 * The single source of truth for tuning, mirrored 1:1 to `/data/adb/m54tuner/config`, which the KSU
 * module reads. The app only ever writes this file and triggers a script — no sysfs logic lives
 * here, so a change made from the UI and the same change made at boot go through identical code.
 *
 * Manual DVFS overrides are no longer exposed; the module and adaptive controller own those nodes.
 */
data class TunerConfig(
    // live tier
    val profile: Profile = Profile.NONE,
    val adaptiveMode: String = "active",
    val adaptiveLearning: Boolean = true,
    val adaptivePelt: Boolean = true,
    val adaptiveTargetFps: Int = 60,
    val adaptiveThermalLimit: Int = 82,
    // Eighteen manual DVFS override fields were removed here: CPU governor, I/O scheduler, the
    // whole gpu_* family, MIF/INT/DISP floors, UFS RPM level and the F2FS in-place-update policy.
    // Every one of them was empty on this device, and configIdentity() treats an absent key and
    // an empty one as the same surface -- verified: the identity hash is bit-identical with them
    // gone -- so removing them cost no learning. Three of them (gpu_min, gpu_max, mif_min) named
    // levers the adaptive engine now owns outright, and a second owner for one lever is what made
    // every earlier measurement in this project compare two engines fighting.
    val thermal: ThermalMode = ThermalMode.MODERATE,
    val thermalGuard: Boolean = true,
    val thermalGuardHigh: Int = 78000,
    val thermalGuardLow: Int = 70000,
    val thermalGuardInterval: Int = 2,
    val thermalGuardTimeout: Int = 900,
    val gos: GosPref = GosPref.UNTOUCHED,
    // memory tier
    val zramAlgo: String = "lz4",
    // render tier
    // Probed on this firmware: libhwui only knows "skiagl" and "skiavk" (no threaded variant),
    // and SurfaceFlinger refuses any non-threaded RenderEngine backend. So these are the only real
    // values, and the factory state is skiagl + skiaglthreaded (dumpsys shows "RE GLES (Ganesh)").
    val hwuiRenderer: String = "skiagl",
    val reBackend: String = "skiaglthreaded",
    val renderApps: List<String> = emptyList(),
    val restartSystemUi: Boolean = false,
    // ART/zygote tier — only live after a soft reboot (zygote restart)
    val artUsap: String = TriState.AUTO,
    val artDex2oatLittle: String = TriState.AUTO,
    val artHeap: String = TriState.AUTO,
    /** Background protection: re-pin oom_score_adj for the chosen games so the kernel's low-memory
     *  killer stops picking them first. Off by default — it costs a wake-up per second. */
    val protectGames: Boolean = false,
    val protectAdj: Int = -700,
    val protectInterval: Int = 1,
    /** Turns OFF Samsung's own limiter flags (low heat, AI background restriction, restricted
     *  device performance, adaptive power saving) and turns ON its responsiveness boost. */
    val samsungPerf: Boolean = false,
    /** Adds the chosen games to Samsung's own MARs exclusion table — the same list Device Care
     *  writes for "never sleeping apps". Surgical: only the listed packages are exempted. */
    val samsungProtect: Boolean = false,
    /** Systemic, and therefore separate from the per-app exemption above. */
    val samsungSpcm: Boolean = false,
    val samsungMarsOff: Boolean = false,
    /** Separate from [games]: who gets the oom_score_adj re-pin and the MARs exemption. Falls back
     *  to [games] on the module side when empty, so an app that never uses this picker keeps the
     *  old behaviour. */
    val protectList: List<String> = emptyList(),
    /** Optional foreground-game controller. Off until its dumpsys integration is validated live. */
    val loadingBoostSeconds: Int = 45,
    // dexopt tier
    val dexoptMode: String = "speed-profile",
    val benchMinSpreadPct: Int = 5,
    val games: List<String> = emptyList(),
    /** Keys this app has no field for, carried through verbatim. This includes inert compatibility
     *  tombstones such as `fasrs_companion`, `fps_unlock` and `game_ram_clear`: no runtime path consumes them,
     *  but older whole-config identity hashes need the original key/value to rehome learned cells.
     *  New configs do not seed them. */
    val extras: Map<String, String> = emptyMap(),
) {
    /** Mirrors the editable portion of engine IdentityKeys, plus the permitted-axis mask (PELT).
     *  Profile prices share the learned physics; changing a context never clears the brain. */
    fun sameLearningContext(other: TunerConfig): Boolean =
        adaptiveTargetFps == other.adaptiveTargetFps &&
            adaptiveThermalLimit == other.adaptiveThermalLimit && thermal == other.thermal &&
            gos == other.gos && samsungPerf == other.samsungPerf &&
            samsungSpcm == other.samsungSpcm && samsungMarsOff == other.samsungMarsOff &&
            adaptivePelt == other.adaptivePelt

    fun serialize(): String = buildString {
        val known = knownLines()
        append(known)
        val written = known.lineSequence().map { it.substringBefore('=') }.toSet()
        for ((key, value) in extras.toSortedMap()) {
            if (key !in written && KEY.matches(key) && '\n' !in value) appendLine("$key=$value")
        }
    }

    private fun knownLines(): String = buildString {
        appendLine("# M54 Tuner config v3 - written by the app, read by the module.")
        appendLine("profile=${profile.toCfg()}")
        appendLine("adaptive_mode=${adaptiveMode.takeIf { it in setOf("active", "observe", "off") } ?: "active"}")
        appendLine("adaptive_learning=${if (adaptiveLearning) "1" else "0"}")
        appendLine("adaptive_pelt=${if (adaptivePelt) "1" else "0"}")
        appendLine("adaptive_target_fps=${adaptiveTargetFps.coerceIn(30, 120)}")
        appendLine("adaptive_thermal_limit=${adaptiveThermalLimit.coerceIn(60, 84)}")
        appendLine("thermal=${thermal.toCfg()}")
        appendLine("thermal_guard=${if (thermalGuard) "1" else "0"}")
        appendLine("thermal_guard_high=${thermalGuardHigh.coerceIn(65000, 95000)}")
        appendLine("thermal_guard_low=${thermalGuardLow.coerceIn(55000, 90000)}")
        appendLine("thermal_guard_interval=${thermalGuardInterval.coerceIn(1, 10)}")
        appendLine("thermal_guard_timeout=${thermalGuardTimeout.coerceIn(60, 3600)}")
        appendLine("gos=${gos.cfg}")
        appendLine("zram_algo=$zramAlgo")
        appendLine("hwui_renderer=$hwuiRenderer")
        appendLine("re_backend=$reBackend")
        appendLine("render_apps=${renderApps.joinToString(",")}")
        appendLine("restart_systemui=${if (restartSystemUi) "1" else "0"}")
        appendLine("art_usap=$artUsap")
        appendLine("art_dex2oat_little=$artDex2oatLittle")
        appendLine("art_heap=$artHeap")
        appendLine("protect_games=${if (protectGames) "1" else "0"}")
        appendLine("samsung_perf=${if (samsungPerf) "1" else "0"}")
        appendLine("samsung_protect=${if (samsungProtect) "1" else "0"}")
        appendLine("samsung_spcm=${if (samsungSpcm) "1" else "0"}")
        appendLine("samsung_mars_off=${if (samsungMarsOff) "1" else "0"}")
        appendLine("protect_adj=${protectAdj.coerceIn(-1000, 500)}")
        appendLine("protect_interval=${protectInterval.coerceIn(1, 60)}")
        appendLine("protect_list=${protectList.joinToString(",")}")
        appendLine("loading_boost_seconds=${loadingBoostSeconds.coerceIn(10, 180)}")
        appendLine("dexopt_mode=$dexoptMode")
        appendLine("bench_min_spread_pct=${benchMinSpreadPct.coerceIn(1, 50)}")
        appendLine("games=${games.joinToString(",")}")
    }

    companion object {
        private val KEY = Regex("[A-Za-z0-9_.-]+")
        private val KNOWN_KEYS: Set<String> by lazy {
            TunerConfig().knownLines().lineSequence()
                .map { it.substringBefore('=') }
                .filter { it.isNotEmpty() && !it.startsWith("#") }
                .toSet()
        }

        fun parse(lines: List<String>): TunerConfig =
            parseKnown(lines).let { cfg ->
                val extras = LinkedHashMap<String, String>()
                for (line in lines) {
                    val t = line.trim()
                    if (t.isEmpty() || t.startsWith("#")) continue
                    val i = t.indexOf('=')
                    if (i <= 0) continue
                    val key = t.substring(0, i).trim()
                    if (key !in KNOWN_KEYS && KEY.matches(key)) {
                        extras[key] = t.substring(i + 1).trim()
                    }
                }
                cfg.copy(extras = extras)
            }

        private fun parseKnown(lines: List<String>): TunerConfig {
            val m = HashMap<String, String>()
            for (line in lines) {
                val t = line.trim()
                if (t.isEmpty() || t.startsWith("#")) continue
                val i = t.indexOf('=')
                if (i <= 0) continue
                m[t.substring(0, i).trim()] = t.substring(i + 1).trim()
            }
            fun s(k: String, def: String = "") = m[k]?.takeIf { it.isNotEmpty() } ?: def
            fun csv(k: String) = (m[k] ?: "").split(",").map { it.trim() }.filter { it.isNotEmpty() }
            fun int(k: String, def: Int) = m[k]?.toIntOrNull() ?: def
            return TunerConfig(
                profile = profileFromCfg(s("profile", "none")),
                adaptiveMode = s("adaptive_mode", "active").takeIf { it in setOf("active", "observe", "off") } ?: "active",
                adaptiveLearning = m["adaptive_learning"] != "0",
                adaptivePelt = m["adaptive_pelt"] != "0",
                adaptiveTargetFps = int("adaptive_target_fps", 60).coerceIn(30, 120),
                adaptiveThermalLimit = int("adaptive_thermal_limit", 82).coerceIn(60, 84),
                thermal = thermalFromCfg(s("thermal", "moderate")),
                thermalGuard = m["thermal_guard"] != "0",
                thermalGuardHigh = int("thermal_guard_high", 78000).coerceIn(65000, 95000),
                thermalGuardLow = int("thermal_guard_low", 70000).coerceIn(55000, 90000),
                thermalGuardInterval = int("thermal_guard_interval", 2).coerceIn(1, 10),
                thermalGuardTimeout = int("thermal_guard_timeout", 900).coerceIn(60, 3600),
                gos = when (m["gos"]) {
                    "disabled" -> GosPref.DISABLED
                    "enabled" -> GosPref.ENABLED
                    else -> GosPref.UNTOUCHED
                },
                zramAlgo = s("zram_algo", "lz4"),
                hwuiRenderer = s("hwui_renderer", "skiagl").let { if (it == "default") "skiagl" else it },
                reBackend = s("re_backend", "skiaglthreaded")
                    .let { if (it == "default") "skiaglthreaded" else it },
                renderApps = csv("render_apps"),
                restartSystemUi = m["restart_systemui"] == "1",
                artUsap = s("art_usap", TriState.AUTO),
                artDex2oatLittle = s("art_dex2oat_little", TriState.AUTO),
                artHeap = s("art_heap", TriState.AUTO),
                protectGames = m["protect_games"] == "1",
                protectAdj = int("protect_adj", -700).coerceIn(-1000, 500),
                protectInterval = int("protect_interval", 1).coerceIn(1, 60),
                samsungPerf = m["samsung_perf"] == "1",
                samsungProtect = m["samsung_protect"] == "1",
                samsungSpcm = m["samsung_spcm"] == "1",
                samsungMarsOff = m["samsung_mars_off"] == "1",
                protectList = csv("protect_list"),
                loadingBoostSeconds = int("loading_boost_seconds", 45).coerceIn(10, 180),
                dexoptMode = s("dexopt_mode", "speed-profile"),
                benchMinSpreadPct = int("bench_min_spread_pct", 5).coerceIn(1, 50),
                games = csv("games"),
            )
        }
    }
}
