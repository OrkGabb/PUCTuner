package com.orkgabb.m54tuner.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val M54ColorScheme = darkColorScheme(
    background = BackgroundDark,
    surface = SurfaceDark,
    surfaceVariant = SurfaceVariantDark,
    surfaceContainer = SurfaceElevated,
    surfaceContainerHigh = SurfaceVariantDark,
    outline = OutlineDark,
    outlineVariant = OutlineDark,
    primary = BalancedAccent,
    onPrimary = BackgroundDark,
    secondary = GameAccent,
    tertiary = PowerSaveAccent,
    error = DangerAccent,
    onBackground = TextPrimary,
    onSurface = TextPrimary,
    onSurfaceVariant = TextSecondary,
)

@Composable
fun M54TunerTheme(content: @Composable () -> Unit) {
    // Always dark — a tuning tool for a mid-range phone should read clearly in bright sunlight and
    // in the dark alike, regardless of the system theme setting.
    MaterialTheme(
        colorScheme = M54ColorScheme,
        typography = M54Typography,
        content = content,
    )
}
