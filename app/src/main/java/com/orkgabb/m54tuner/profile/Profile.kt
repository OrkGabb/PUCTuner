package com.orkgabb.m54tuner.profile

/** Performance profiles. Mutually exclusive; never touch thermal zones. */
enum class Profile {
    GAME,
    BALANCED,
    POWER_SAVE,
    NONE,
}

/**
 * Thermal throttling control — a SEPARATE axis from [Profile], not baked into any profile.
 *
 * [MODERATE] leaves the factory thermal governor fully intact (default, safe).
 * [AGGRESSIVE] disables the CPU/GPU thermal zones (mode=disabled) for uncapped sustained clocks;
 * the battery zone is always left enabled regardless of mode. Shows a one-time warning on first use.
 */
enum class ThermalMode {
    MODERATE,
    AGGRESSIVE,
}
