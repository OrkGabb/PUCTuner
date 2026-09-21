package com.orkgabb.m54tuner.system

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ApplyReportTest {
    @Test fun completeVerifiedReportIsClean() {
        val report = ApplyReport.parse(listOf("T|1|render", "R|prop|ok|true|true", "E|2|ok"), true)
        assertTrue(report.complete)
        assertTrue(report.isClean)
    }

    @Test fun truncatedReportIsNotClean() {
        val report = ApplyReport.parse(listOf("T|1|render", "R|prop|ok|true|true"), true)
        assertFalse(report.complete)
        assertFalse(report.isClean)
    }

    @Test fun nonzeroShellExitOverridesGoodLookingPayload() {
        val report = ApplyReport.parse(listOf("T|1|render", "R|prop|ok|true|true", "E|2|ok"), false)
        assertFalse(report.isClean)
    }

    @Test fun unknownStatusIsAFailure() {
        val report = ApplyReport.parse(listOf("T|1|x", "R|key|future-status|got|want", "E|2|fail"), true)
        assertFalse(report.isClean)
    }

    @Test fun routineProfileApplyWithUntouchedGosIsClean() {
        // Verbatim from the device, 2026-09-20: this is what every profile apply looks like
        // when GOS is left alone. It must not read as a failure.
        val report = ApplyReport.parse(
            listOf("T|1789958078|profile:balanced", "R|cpu.policy0.min|ok|533000|533000",
                "R|gos|skip|-|untouched", "E|1789958099|ok"),
            true,
        )
        assertTrue(report.isClean)
    }

    @Test fun pendingRestartWarningIsNotAFailure() {
        val report = ApplyReport.parse(
            listOf("T|1|render", "R|render.re_backend|ok|skiaglthreaded|skiaglthreaded",
                "R|render.sf_pending|warn|pending|pending", "E|2|ok"),
            true,
        )
        assertTrue(report.isClean)
    }

    @Test fun emptyReportIsNotClean() {
        assertFalse(ApplyReport.parse(listOf("T|1|x", "E|2|ok"), true).isClean)
    }

    @Test fun everySectionNeedsSuccessfulTerminator() {
        val report = ApplyReport.parse(
            listOf("T|1|render", "R|a|ok|1|1", "E|2|ok", "T|3|profile", "R|b|ok|1|1"),
            true,
        )
        assertFalse(report.complete)
        assertFalse(report.isClean)
    }
}
