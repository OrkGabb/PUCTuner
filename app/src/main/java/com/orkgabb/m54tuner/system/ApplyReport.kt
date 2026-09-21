package com.orkgabb.m54tuner.system

/**
 * What the module actually managed to do, parsed from `/data/adb/m54tuner/result`.
 *
 * This exists because "applied" used to mean "the shell exited 0", which is true even when every
 * single write bounced off the kernel — hence "sometimes it applies everything, sometimes not". Now
 * each lever is written, read back, retried, and reported with the value that really stuck; the UI
 * shows exactly that and never claims more.
 */
data class ApplyReport(
    val section: String,
    val items: List<Item>,
    val complete: Boolean,
    val shellSucceeded: Boolean,
) {
    data class Item(
        val key: String,
        val status: Status,
        val got: String,
        val want: String,
    )

    enum class Status { OK, FAIL, SKIP, WARN }

    val ok: Int get() = items.count { it.status == Status.OK }
    val failed: List<Item> get() = items.filter { it.status == Status.FAIL }
    val warned: List<Item> get() = items.filter { it.status == Status.WARN }
    val skipped: Int get() = items.count { it.status == Status.SKIP }
    /**
     * True when the script finished, closed every section it opened, reported at least one lever,
     * and none of them failed. An empty item list is NOT clean: the script crashed before writing
     * any per-lever record, so there is zero evidence anything stuck.
     *
     * `skip` and `warn` are not failures. Every profile apply reports `gos|skip|untouched` when
     * GOS is left alone, and `warn` is a pending restart the banner already asks about; counting
     * either as unclean made every normal apply read "toque para ver" -- a failure that exists
     * nowhere on the device. The summary still counts both.
     */
    val isClean: Boolean get() = shellSucceeded && complete && items.isNotEmpty() && failed.isEmpty()

    /** One-line summary for the snackbar. */
    fun summary(): String = buildString {
        if (items.isEmpty()) append("sem confirmação do módulo")
        else append("$ok aplicado(s)")
        if (failed.isNotEmpty()) append(" · ${failed.size} falhou")
        if (warned.isNotEmpty()) append(" · ${warned.size} limitado(s)")
        if (skipped > 0) append(" · $skipped n/d")
    }

    /** True when this key (or any key under it, e.g. "gpu" covers "gpu.min") failed. */
    fun failedFor(prefix: String) = failed.any { it.key == prefix || it.key.startsWith("$prefix.") }

    companion object {
        fun parse(lines: List<String>, shellSucceeded: Boolean = true): ApplyReport {
            var section = ""
            val items = ArrayList<Item>()
            var openSections = 0
            var terminalFailure = false
            for (raw in lines) {
                val line = raw.trim()
                if (line.isEmpty()) continue
                val f = line.split('|')
                when (f.getOrNull(0)) {
                    "T" -> {
                        if (section.isEmpty()) section = f.getOrNull(2).orEmpty()
                        openSections++
                    }
                    "R" -> {
                        val key = f.getOrNull(1).orEmpty()
                        if (key.isEmpty()) continue
                        val st = when (f.getOrNull(2)) {
                            "ok" -> Status.OK
                            "fail" -> Status.FAIL
                            "warn" -> Status.WARN
                            "skip" -> Status.SKIP
                            else -> Status.FAIL
                        }
                        items.add(Item(key, st, f.getOrNull(3).orEmpty(), f.getOrNull(4).orEmpty()))
                    }
                    "E" -> {
                        if (openSections > 0) openSections-- else terminalFailure = true
                        if (f.getOrNull(2) != "ok") terminalFailure = true
                    }
                }
            }
            return ApplyReport(
                section = section,
                items = items,
                complete = section.isNotEmpty() && openSections == 0 && !terminalFailure,
                shellSucceeded = shellSucceeded,
            )
        }
    }
}
