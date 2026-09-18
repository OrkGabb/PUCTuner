package com.orkgabb.m54tuner.system

/**
 * The live kernel state, parsed from the module's `status.sh`. Everything the UI displays about the
 * device comes from here — never from what the app believes it wrote — so a lever that silently
 * did not stick shows up as a mismatch instead of a green check.
 */
data class DeviceState(
    val moduleInstalled: Boolean = false,
    val factoryCaptured: Boolean = false,
    val adaptive: Map<String, String> = emptyMap(),
    val pendingSf: Boolean = false,
    val pendingSoft: Boolean = false,
    val clusters: List<Cluster> = emptyList(),
    val gpu: Gpu? = null,
    val mifCur: Int? = null,
    val mifMin: Int? = null,
    val mifMax: Int? = null,
    val mifTable: List<Int> = emptyList(),
    val intCur: Int? = null,
    val intMin: Int? = null,
    val intMax: Int? = null,
    val dispCur: Int? = null,
    val dispMin: Int? = null,
    val ioSched: Map<String, String> = emptyMap(),
    val pelt: String = "",
    val eas: String = "",
    val schedBench: String = "",
    /** f2fs GC state, cpuidle deep-state and VFS cache pressure — keyed without their prefix. */
    val deep: Map<String, String> = emptyMap(),
    /** Live block-queue and elevator tunables of the data LUN, keyed without the "ioq." prefix. */
    val ioq: Map<String, String> = emptyMap(),
    /** UFS link state: clock gating, hibernate timer and the QoS floor it asks for while busy. */
    val ufs: Map<String, String> = emptyMap(),
    val zramAlgo: String = "",
    val zramAlgos: List<String> = emptyList(),
    val swappiness: Int? = null,
    val tempC: Double? = null,
    val tempZones: Map<String, Double> = emptyMap(),
    val thermalAggressive: Boolean = false,
    val props: Map<String, String> = emptyMap(),
    val gos: String = "unknown",
    /** Whether the background-protection watcher is actually alive (read from its PID). */
    val keepalive: Boolean = false,
    val thermalGuardRunning: Boolean = false,
    val thermalGuardState: String = "",
    /** Samsung's limiter flags as they read right now, keyed without the "sam." prefix. */
    val samsung: Map<String, String> = emptyMap(),
) {
    /** A cpufreq policy: policy0 = 4×A55 little, policy4 = 4×A78 big on the Exynos 1380. */
    data class Cluster(
        val tag: String,
        val cur: Int,
        val min: Int,
        val max: Int,
        val hwMin: Int,
        val hwMax: Int,
        val gov: String,
        val govs: List<String>,
    ) {
        val label: String get() = if (tag == "policy0") "LITTLE" else "BIG"
        fun curMhz() = cur / 1000
        fun maxMhz() = max / 1000
    }

    data class Gpu(
        val table: List<Int>,
        val min: Int,
        val max: Int,
        val cur: Int,
        val busy: Int?,
        val gov: String,
        val govs: List<String>,
        val hsLoad: String,
        val hsClock: String,
        val hsDelay: String,
        val powerPolicy: String,
        val powerPolicies: List<String>,
        val clBoostDisabled: Boolean,
        /** DVFS/job-scheduler timing, in ms: how often it rules, looks, and re-schedules jobs. */
        val dvfsPeriod: String,
        val pollingSpeed: String,
        val jsPeriod: String,
    ) {
        fun curMhz() = cur / 1000
        fun minMhz() = min / 1000
        fun maxMhz() = max / 1000
    }

    companion object {
        /** All GPU governors this kernel exposes; the driver prints the active one in brackets. */
        private fun bracketList(raw: String): Pair<List<String>, String> {
            val parts = raw.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
            val active = parts.firstOrNull { it.startsWith("[") }?.trim('[', ']')
            val all = parts.map { it.trim('[', ']') }
            return all to (active ?: parts.firstOrNull().orEmpty())
        }

        fun parse(lines: List<String>): DeviceState {
            val m = HashMap<String, String>()
            for (raw in lines) {
                val line = raw.trim()
                val i = line.indexOf('=')
                if (i <= 0) continue
                m[line.substring(0, i)] = line.substring(i + 1).trim()
            }
            fun int(k: String) = m[k]?.toIntOrNull()

            val clusters = m.keys.filter { it.startsWith("cpu.") && it.endsWith(".cur") }
                .map { it.removePrefix("cpu.").removeSuffix(".cur") }
                .sorted()
                .mapNotNull { tag ->
                    val cur = int("cpu.$tag.cur") ?: return@mapNotNull null
                    DeviceState.Cluster(
                        tag = tag,
                        cur = cur,
                        min = int("cpu.$tag.min") ?: 0,
                        max = int("cpu.$tag.max") ?: 0,
                        hwMin = int("cpu.$tag.hwmin") ?: 0,
                        hwMax = int("cpu.$tag.hwmax") ?: 0,
                        gov = m["cpu.$tag.gov"].orEmpty(),
                        govs = m["cpu.$tag.govs"].orEmpty().split(" ").filter { it.isNotEmpty() },
                    )
                }

            val gpu = m["gpu.table"]?.let { table ->
                val (govs, activeGov) = bracketList(m["gpu.gov"].orEmpty())
                val (pols, activePol) = bracketList(m["gpu.power_policy"].orEmpty())
                DeviceState.Gpu(
                    table = table.split(" ").mapNotNull { it.trim().toIntOrNull() }.sorted(),
                    min = int("gpu.min") ?: 0,
                    max = int("gpu.max") ?: 0,
                    cur = int("gpu.cur") ?: 0,
                    busy = int("gpu.busy"),
                    gov = activeGov,
                    govs = govs,
                    hsLoad = m["gpu.hs_load"].orEmpty(),
                    hsClock = m["gpu.hs_clock"].orEmpty(),
                    hsDelay = m["gpu.hs_delay"].orEmpty(),
                    powerPolicy = activePol,
                    powerPolicies = pols,
                    clBoostDisabled = m["gpu.cl_boost_disable"] == "1",
                    dvfsPeriod = m["gpu.dvfs_period"].orEmpty(),
                    pollingSpeed = m["gpu.polling_speed"].orEmpty(),
                    jsPeriod = m["gpu.js_period"].orEmpty(),
                )
            }

            val zones = HashMap<String, Double>()
            for ((k, v) in m) {
                if (k.startsWith("temp.") && k != "temp") {
                    v.toDoubleOrNull()?.let { zones[k.removePrefix("temp.")] = milli(it) }
                }
            }

            return DeviceState(
                moduleInstalled = m["module"] == "1",
                factoryCaptured = m["factory"] == "1",
                adaptive = m.filterKeys { it.startsWith("adaptive.") }.mapKeys { it.key.removePrefix("adaptive.") },
                pendingSf = m["pending_sf"] == "1",
                pendingSoft = m["pending_soft"] == "1",
                clusters = clusters,
                gpu = gpu,
                mifCur = int("mif.cur"),
                mifMin = int("mif.min"),
                mifMax = int("mif.max"),
                mifTable = m["mif.table"].orEmpty().split(" ").mapNotNull { it.trim().toIntOrNull() },
                intCur = int("int.cur"),
                intMin = int("int.min"),
                intMax = int("int.max"),
                dispCur = int("disp.cur"),
                dispMin = int("disp.min"),
                ioSched = m.filterKeys { it.startsWith("io.") }
                    .mapKeys { it.key.removePrefix("io.") }
                    .filterValues { it.isNotEmpty() },
                pelt = m["pelt"].orEmpty(),
                eas = m["eas"].orEmpty(),
                schedBench = m["bench.sched"].orEmpty(),
                deep = (m.filterKeys { it.startsWith("f2fs.") || it.startsWith("cpuidle.") } +
                    m.filterKeys { it == "vm.cache_pressure" })
                    .mapKeys { it.key.substringAfter('.') },
                ioq = m.filterKeys { it.startsWith("ioq.") }.mapKeys { it.key.removePrefix("ioq.") },
                ufs = m.filterKeys { it.startsWith("ufs.") }.mapKeys { it.key.removePrefix("ufs.") },
                zramAlgo = m["zram.algo"].orEmpty(),
                zramAlgos = m["zram.algos"].orEmpty().split(" ").filter { it.isNotEmpty() },
                swappiness = int("swappiness"),
                tempC = m["temp"]?.toDoubleOrNull()?.let { milli(it) }?.takeIf { it > 0 },
                tempZones = zones,
                thermalAggressive = m["thermal.mode"] == "aggressive",
                props = m.filterKeys { it.startsWith("prop.") }.mapKeys { it.key.removePrefix("prop.") },
                gos = m["gos"].orEmpty().ifEmpty { "unknown" },
                keepalive = m["keepalive.running"] == "1",
                thermalGuardRunning = m["thermal.guard"] == "1",
                thermalGuardState = m["thermal.guard_state"].orEmpty(),
                samsung = m.filterKeys { it.startsWith("sam.") }.mapKeys { it.key.removePrefix("sam.") },
            )
        }

        /** Thermal zones report milli-°C on this kernel, but not every zone does — be tolerant. */
        private fun milli(v: Double) = if (v > 1000) v / 1000.0 else v
    }
}
