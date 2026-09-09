package com.orkgabb.m54tuner.system

/**
 * One step of the timing sweep, as written by the module's bench.sh.
 *
 * [jankPerMille] is janky frames per thousand — the score to minimise. It is -1 when the step was
 * discarded for having too few frames, which happens when the screen went idle: a couple of frames
 * would otherwise produce a spectacular-looking ratio out of pure noise.
 */
data class BenchRow(
    val phase: String,
    val dvfs: String,
    val polling: String,
    val js: String,
    val frames: Int,
    val janky: Int,
    val jankPerMille: Int,
) {
    val valid: Boolean get() = jankPerMille >= 0
    val isWinner: Boolean get() = phase == "melhor"

    companion object {
        fun parse(line: String): BenchRow? {
            val f = line.trim().split(',')
            if (f.size < 9) return null
            if (f[0] == "fase" || f[0] == "inconclusivo") return null
            return BenchRow(
                phase = f[0],
                dvfs = f[1],
                polling = f[2],
                js = f[3],
                frames = f[4].toIntOrNull() ?: 0,
                janky = f[5].toIntOrNull() ?: 0,
                jankPerMille = f[8].toIntOrNull() ?: -1,
            )
        }
    }
}
