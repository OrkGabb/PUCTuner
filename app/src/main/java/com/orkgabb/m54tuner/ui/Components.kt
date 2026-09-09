package com.orkgabb.m54tuner.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.orkgabb.m54tuner.ui.theme.OutlineDark
import com.orkgabb.m54tuner.ui.theme.PowerSaveAccent
import com.orkgabb.m54tuner.ui.theme.SurfaceDark
import com.orkgabb.m54tuner.ui.theme.SurfaceElevated
import com.orkgabb.m54tuner.ui.theme.SurfaceVariantDark
import com.orkgabb.m54tuner.ui.theme.TextSecondary
import com.orkgabb.m54tuner.ui.theme.TextTertiary

/**
 * A titled group of controls. The accent bar and the per-section spinner are what make "which part
 * of the app is doing something right now" readable at a glance — the previous screen disabled
 * everything at once whenever any change was in flight.
 */
@Composable
fun SectionCard(
    title: String,
    subtitle: String? = null,
    accent: Color = MaterialTheme.colorScheme.primary,
    busy: Boolean = false,
    trailing: (@Composable () -> Unit)? = null,
    content: @Composable () -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(22.dp))
            .background(SurfaceDark)
            .border(1.dp, OutlineDark, RoundedCornerShape(22.dp))
            .padding(18.dp),
    ) {
        // Top-aligned: with a two-line subtitle a centred trailing tag drifts down beside the text
        // and reads as if it belonged to the second line.
        Row(verticalAlignment = Alignment.Top) {
            Box(
                Modifier
                    .size(width = 3.dp, height = 16.dp)
                    .clip(RoundedCornerShape(2.dp))
                    .background(accent),
            )
            Spacer(Modifier.width(10.dp))
            Column(Modifier.weight(1f)) {
                Text(localize(title, LocalAppLanguage.current), style = MaterialTheme.typography.titleMedium)
                if (subtitle != null) {
                    Spacer(Modifier.height(2.dp))
                    Text(localize(subtitle, LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextTertiary)
                }
            }
            if (busy) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp, color = accent)
            else trailing?.invoke()
        }
        Spacer(Modifier.height(14.dp))
        content()
    }
}

/** A big, tappable choice — the three profiles. Selection is a glowing border, not a checkbox. */
@Composable
fun OptionCard(
    title: String,
    subtitle: String,
    accent: Color,
    selected: Boolean,
    enabled: Boolean,
    badge: String? = null,
    onClick: () -> Unit,
) {
    // NOT animated. Inside a LazyColumn an item is recomposed from scratch every time it is
    // recycled, and animate*AsState restarts from its initial value — so borders faded in on every
    // scroll and looked like they were vanishing at random. Selection is a state, not a transition.
    val border = if (selected) accent else OutlineDark
    val glow = if (selected) 1f else 0f
    Box(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(20.dp))
            .background(
                Brush.horizontalGradient(
                    listOf(
                        accent.copy(alpha = 0.16f * glow),
                        SurfaceElevated.copy(alpha = 1f),
                    ),
                ),
            )
            .border(if (selected) 1.5.dp else 1.dp, border, RoundedCornerShape(20.dp))
            .clickable(enabled = enabled, onClick = onClick)
            // Deliberately NOT faded while an apply is in flight. The section spinner already says
            // "busy", and dropping every card to 45% opacity made a working app look broken — it was
            // the single loudest complaint about this screen.
            .alpha(if (enabled) 1f else 0.9f)
            .padding(horizontal = 18.dp, vertical = 16.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier
                    .size(10.dp)
                    .clip(RoundedCornerShape(50))
                    .background(if (selected) accent else OutlineDark),
            )
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(localize(title, LocalAppLanguage.current), style = MaterialTheme.typography.titleLarge)
                    if (badge != null) {
                        Spacer(Modifier.width(8.dp))
                        Tag(badge, accent)
                    }
                }
                Spacer(Modifier.height(3.dp))
                Text(
                    localize(subtitle, LocalAppLanguage.current),
                    style = MaterialTheme.typography.bodySmall,
                    color = TextSecondary,
                )
            }
        }
    }
}

@Composable
fun Tag(text: String, accent: Color) {
    Box(
        Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(accent.copy(alpha = 0.18f))
            .padding(horizontal = 6.dp, vertical = 2.dp),
    ) {
        Text(
            localize(text, LocalAppLanguage.current).uppercase(),
            style = MaterialTheme.typography.labelSmall,
            color = accent,
            fontWeight = FontWeight.Bold,
        )
    }
}

/** value → label. `null` value means "let the profile decide" and is rendered as "Auto". */
data class PillOption(val value: String, val label: String)

/**
 * A row of exclusive pills. Wraps onto as many rows as needed, so a long list (the eight GPU
 * frequency steps, six governors) stays readable instead of being squeezed into unreadable slivers.
 */
@Composable
fun PillRow(
    options: List<PillOption>,
    selected: String,
    accent: Color,
    enabled: Boolean,
    perRow: Int = 3,
    onSelect: (String) -> Unit,
) {
    Column {
        options.chunked(perRow).forEachIndexed { rowIndex, row ->
            if (rowIndex > 0) Spacer(Modifier.height(8.dp))
            Row(Modifier.fillMaxWidth()) {
                row.forEachIndexed { i, opt ->
                    if (i > 0) Spacer(Modifier.width(8.dp))
                    Pill(opt.label, opt.value == selected, accent, enabled, Modifier.weight(1f)) {
                        onSelect(opt.value)
                    }
                }
                // keep the last row's pills the same width as the rows above
                repeat(perRow - row.size) {
                    Spacer(Modifier.width(8.dp))
                    Spacer(Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
fun Pill(
    text: String,
    selected: Boolean,
    accent: Color,
    enabled: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    // Unselected pills used to be SurfaceVariant on a SurfaceDark card — a few percent of contrast,
    // so a row of them read as loose words floating in the section instead of as controls.
    // Same reason as OptionCard: no animation inside lazy items, or the chrome flickers on scroll.
    val bg = if (selected) accent.copy(alpha = 0.22f) else SurfaceElevated
    val border = if (selected) accent else OutlineDark
    Box(
        modifier
            .clip(RoundedCornerShape(12.dp))
            .background(bg)
            .border(1.dp, border, RoundedCornerShape(12.dp))
            .clickable(enabled = enabled, onClick = onClick)
            .alpha(if (enabled) 1f else 0.9f)
            .padding(vertical = 10.dp, horizontal = 6.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            localize(text, LocalAppLanguage.current),
            style = MaterialTheme.typography.labelLarge,
            color = if (selected) accent else TextSecondary,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
fun SwitchRow(
    title: String,
    desc: String,
    checked: Boolean,
    enabled: Boolean,
    accent: Color = MaterialTheme.colorScheme.primary,
    badge: String? = null,
    onChange: (Boolean) -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(enabled = enabled) { onChange(!checked) }
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(localize(title, LocalAppLanguage.current), style = MaterialTheme.typography.bodyLarge)
            if (badge != null) {
                Spacer(Modifier.height(4.dp))
                Tag(badge, accent)
            }
            Spacer(Modifier.height(2.dp))
            Text(localize(desc, LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextTertiary)
        }
        Spacer(Modifier.width(12.dp))
        Switch(
            checked = checked,
            onCheckedChange = onChange,
            enabled = enabled,
            colors = SwitchDefaults.colors(
                checkedThumbColor = Color.White,
                checkedTrackColor = accent,
                checkedBorderColor = accent,
            ),
        )
    }
}

/** A compact live readout for the header strip. */
@Composable
fun StatChip(label: String, value: String, accent: Color, modifier: Modifier = Modifier) {
    Column(
        modifier
            .clip(RoundedCornerShape(14.dp))
            .background(SurfaceDark)
            .border(1.dp, OutlineDark, RoundedCornerShape(14.dp))
            .padding(horizontal = 12.dp, vertical = 10.dp),
    ) {
        Text(localize(label, LocalAppLanguage.current), style = MaterialTheme.typography.labelSmall, color = TextTertiary)
        Spacer(Modifier.height(3.dp))
        Text(
            localize(value, LocalAppLanguage.current),
            style = MaterialTheme.typography.titleMedium,
            color = accent,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

/** Current value inside its allowed window — the fastest way to see a floor actually holding. */
@Composable
fun RangeBar(cur: Int, min: Int, max: Int, hwMin: Int, hwMax: Int, accent: Color) {
    val span = (hwMax - hwMin).coerceAtLeast(1).toFloat()
    val lo = ((min - hwMin) / span).coerceIn(0f, 1f)
    val hi = ((max - hwMin) / span).coerceIn(0f, 1f)
    val at = ((cur - hwMin) / span).coerceIn(0f, 1f)
    Box(
        Modifier
            .fillMaxWidth()
            .height(8.dp)
            .clip(RoundedCornerShape(4.dp))
            .background(SurfaceVariantDark),
    ) {
        // the window the profile allows
        Box(
            Modifier
                .fillMaxWidth(hi)
                .height(8.dp)
                .padding(start = 0.dp)
                .background(accent.copy(alpha = 0.18f)),
        )
        Box(
            Modifier
                .fillMaxWidth(lo)
                .height(8.dp)
                .background(accent.copy(alpha = 0.35f)),
        )
        // where the kernel actually is right now
        Box(
            Modifier
                .fillMaxWidth(at)
                .height(8.dp)
                .clip(RoundedCornerShape(4.dp))
                .background(accent),
        )
    }
}

/** A plain 0..1 meter, for quantities the engine reports as a fraction. */
@Composable
fun ProgressBar(fraction: Float, accent: Color) {
    Box(
        Modifier
            .fillMaxWidth()
            .height(6.dp)
            .clip(RoundedCornerShape(3.dp))
            .background(SurfaceVariantDark),
    ) {
        Box(
            Modifier
                .fillMaxWidth(fraction.coerceIn(0f, 1f))
                .height(6.dp)
                .clip(RoundedCornerShape(3.dp))
                .background(accent),
        )
    }
    Spacer(Modifier.height(8.dp))
}

@Composable
fun InfoRow(key: String, value: String, valueColor: Color = TextSecondary) {
    Row(
        Modifier.fillMaxWidth().padding(vertical = 3.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        // weight + a hard gap: without them a long value ran into the key and both got ellipsized,
        // which is how "LITTLE · energy_aware2002 MHz" ended up on screen.
        Text(
            localize(key, LocalAppLanguage.current),
            style = MaterialTheme.typography.bodySmall,
            color = TextTertiary,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f, fill = false),
        )
        Spacer(Modifier.width(12.dp))
        Text(
            localize(value, LocalAppLanguage.current),
            style = MaterialTheme.typography.bodySmall,
            color = valueColor,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
fun FieldLabel(text: String) {
    Text(localize(text, LocalAppLanguage.current), style = MaterialTheme.typography.labelMedium, color = TextTertiary)
    Spacer(Modifier.height(6.dp))
}

/**
 * A switch for one system feature, with its trade-off spelled out instead of a paragraph dumped
 * under the control. Title, an optional recommendation tag, and two labelled lines — what you gain
 * and what it costs — so the decision is scannable rather than something to read twice.
 */
@Composable
fun FeatureSwitch(
    title: String,
    gain: String,
    cost: String,
    checked: Boolean,
    enabled: Boolean,
    accent: Color,
    badge: String? = null,
    onChange: (Boolean) -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(SurfaceVariantDark.copy(alpha = 0.5f))
            .clickable(enabled = enabled) { onChange(!checked) }
            .padding(14.dp)
            .alpha(if (enabled) 1f else 0.5f),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(localize(title, LocalAppLanguage.current), style = MaterialTheme.typography.titleSmall)
                if (badge != null) {
                    Spacer(Modifier.height(5.dp))
                    Tag(badge, accent)
                }
            }
            Spacer(Modifier.width(12.dp))
            Switch(
                checked = checked,
                onCheckedChange = onChange,
                enabled = enabled,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = Color.White,
                    checkedTrackColor = accent,
                    checkedBorderColor = accent,
                ),
            )
        }
        Spacer(Modifier.height(10.dp))
        TradeLine("GANHO", gain, PowerSaveAccent)
        Spacer(Modifier.height(4.dp))
        TradeLine("CUSTO", cost, TextTertiary)
    }
}

@Composable
private fun TradeLine(label: String, text: String, accent: Color) {
    Row(Modifier.fillMaxWidth()) {
        Text(
            localize(label, LocalAppLanguage.current),
            style = MaterialTheme.typography.labelSmall,
            color = accent,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.width(52.dp),
        )
        Text(localize(text, LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextSecondary)
    }
}

/**
 * An explanatory aside. Set apart with a left rule and its own ground so it reads as a note about
 * the control above it, not as more body copy competing with the labels.
 */
@Composable
fun Note(text: String, accent: Color = TextTertiary) {
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(10.dp))
            .background(SurfaceVariantDark.copy(alpha = 0.35f))
            .padding(vertical = 10.dp, horizontal = 12.dp),
    ) {
        Box(
            Modifier
                .width(2.dp)
                .height(34.dp)
                .clip(RoundedCornerShape(1.dp))
                .background(accent.copy(alpha = 0.6f)),
        )
        Spacer(Modifier.width(10.dp))
        Text(localize(text, LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextSecondary)
    }
}
