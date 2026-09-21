package com.orkgabb.m54tuner.system

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TunerConfigTest {
    @Test fun retiredCompatibilityKeysRoundTripButAreNotSeeded() {
        val cfg = TunerConfig.parse(
            listOf(
                "profile=balanced", "fasrs_companion=auto", "fps_unlock=1",
                "game_ram_clear=1", "future_key=yes",
            )
        )
        val serialized = cfg.serialize()
        assertTrue(serialized.contains("fasrs_companion=auto"))
        assertTrue(serialized.contains("fps_unlock=1"))
        assertTrue(serialized.contains("game_ram_clear=1"))
        assertTrue(serialized.contains("future_key=yes"))
        val fresh = TunerConfig().serialize()
        assertFalse(fresh.contains("fasrs_companion="))
        assertFalse(fresh.contains("fps_unlock="))
        assertFalse(fresh.contains("game_ram_clear="))
    }
}
