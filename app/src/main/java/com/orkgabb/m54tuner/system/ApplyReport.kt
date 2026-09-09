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
    val isClean: Boolean get() = failed.isEmpty()

    /** One-line summary for the snackbar. */
    fun summary(): String = buildString {
        append("$ok aplicado(s)")
        if (failed.isNotEmpty()) append(" · ${failed.size} falhou")
        if (warned.isNotEmpty()) append(" · ${warned.size} limitado(s)")
        if (skipped > 0) append(" · $skipped n/d")
    }

    /** True when this key (or any key under it, e.g. "gpu" covers "gpu.min") failed. */
    fun failedFor(prefix: String) = failed.any { it.key == prefix || it.key.startsWith("$prefix.") }

    companion object {
        fun parse(lines: List<String>): ApplyReport {
            var section = ""
            val items = ArrayList<Item>()
            for (raw in lines) {
                val line = raw.trim()
                if (line.isEmpty()) continue
                val f = line.split('|')
                when (f.getOrNull(0)) {
                    "T" -> if (section.isEmpty()) section = f.getOrNull(2).orEmpty()
                    "R" -> {
                        val key = f.getOrNull(1).orEmpty()
                        if (key.isEmpty()) continue
                        val st = when (f.getOrNull(2)) {
                            "ok" -> Status.OK
                            "fail" -> Status.FAIL
                            "warn" -> Status.WARN
                            else -> Status.SKIP
                        }
                        items.add(Item(key, st, f.getOrNull(3).orEmpty(), f.getOrNull(4).orEmpty()))
                    }
                }
            }
            return ApplyReport(section, items)
        }
    }
}
