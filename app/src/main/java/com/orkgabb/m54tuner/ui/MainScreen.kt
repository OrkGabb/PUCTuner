package com.orkgabb.m54tuner.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarDuration
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.SnackbarResult
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.orkgabb.m54tuner.Dialog
import com.orkgabb.m54tuner.MainUiState
import com.orkgabb.m54tuner.MainViewModel
import com.orkgabb.m54tuner.Picker
import com.orkgabb.m54tuner.RootState
import com.orkgabb.m54tuner.profile.ThermalMode
import com.orkgabb.m54tuner.system.AppInfo
import com.orkgabb.m54tuner.system.ApplyReport
import com.orkgabb.m54tuner.system.GosPref
import com.orkgabb.m54tuner.system.Tier
import com.orkgabb.m54tuner.system.TriState
import com.orkgabb.m54tuner.ui.theme.BalancedAccent
import com.orkgabb.m54tuner.ui.theme.DangerAccent
import com.orkgabb.m54tuner.ui.theme.GameAccent
import com.orkgabb.m54tuner.ui.theme.OutlineDark
import com.orkgabb.m54tuner.ui.theme.PowerSaveAccent
import com.orkgabb.m54tuner.ui.theme.RenderAccent
import com.orkgabb.m54tuner.ui.theme.SurfaceDark
import com.orkgabb.m54tuner.ui.theme.SurfaceVariantDark
import com.orkgabb.m54tuner.ui.theme.TextSecondary
import com.orkgabb.m54tuner.ui.theme.TextTertiary
import com.orkgabb.m54tuner.ui.theme.WarnAccent

private val TABS = listOf("Motor", "GPU", "Render", "Memória", "Apps", "Lab")
private const val CONTEXT_BADGE = "Muda o contexto"
private const val CONTEXT_NOTICE = "Esta alteração muda o contexto do motor. " +
    "O aprendizado anterior é preservado e reutilizado ao retornar à configuração."

/**
 * These props are off (or absent) from the factory, so a switch says everything: ON writes the
 * tuned value, OFF writes the recorded factory value back. A third "never touched" state would
 * land the device on exactly the same value as OFF, so it is not worth a pill.
 */

/** Fábrica / ligado / desligado — "fábrica" nunca escreve, "desligado" grava o valor original. */
private val TRI = listOf(
    PillOption(TriState.AUTO, "Fábrica"),
    PillOption(TriState.ON, "Ligado"),
    PillOption(TriState.OFF, "Desligado"),
)
/** Empty override in the config: the profile preset decides. Never a real governor/value. */
private const val AUTO = ""
private const val AUTO_LABEL = "Auto"

@Composable
fun MainScreen(viewModel: MainViewModel) {
    val state by viewModel.uiState.collectAsStateWithLifecycle()
    CompositionLocalProvider(LocalAppLanguage provides state.language) {
        MainScreenContent(state, viewModel)
    }
}

@Composable
private fun MainScreenContent(state: MainUiState, viewModel: MainViewModel) {
    val snackbar = remember { SnackbarHostState() }
    var tab by remember { mutableIntStateOf(0) }
    val language = LocalAppLanguage.current

    // The snackbar is the only place a result is announced, and it offers the detail sheet instead
    // of asserting success: "12 aplicado(s) · 1 falhou — toque para ver".
    LaunchedEffect(state.snack, language) {
        val msg = state.snack ?: return@LaunchedEffect
        val hasDetail = state.report != null
        val res = snackbar.showSnackbar(
            message = localize(msg, language),
            actionLabel = if (hasDetail) localize("VER", language) else null,
            duration = SnackbarDuration.Short,
        )
        if (res == SnackbarResult.ActionPerformed) viewModel.openReport()
        viewModel.consumeSnack()
    }

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        snackbarHost = { SnackbarHost(snackbar) },
    ) { inner ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(bottom = inner.calculateBottomPadding())
                .statusBarsPadding(),
        ) {
            Header(state, viewModel)
            StatusBanner(state, viewModel)
            TabBar(tab) { tab = it }
            // One LazyColumn per tab (key(tab) gives each its own scroll position). Lazy, not a
            // scrolling Column: only the cards on screen are composed, measured and drawn — the GPU
            // tab alone has ~40 pills, and drawing all of them every frame is what made the scroll
            // feel like it was dragging.
            key(tab) {
                val listState = rememberLazyListState()
                // Polling the kernel rewrites the whole state object, which recomposes the list.
                // Doing that mid-fling is a dropped frame, so pause it while the finger is moving.
                LaunchedEffect(listState.isScrollInProgress) {
                    viewModel.setScrolling(listState.isScrollInProgress)
                }
                LazyColumn(
                    state = listState,
                    modifier = Modifier.fillMaxSize().navigationBarsPadding(),
                    contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 4.dp, bottom = 28.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    when (tab) {
                        0 -> profileTab(state, viewModel)
                        1 -> gpuTab(state, viewModel)
                        2 -> renderTab(state, viewModel)
                        3 -> memoryTab(state, viewModel)
                        4 -> appsTab(state, viewModel)
                        else -> diagnosticTab(state, viewModel)
                    }
                }
            }
        }
    }

    Dialogs(state, viewModel)
    if (state.reportOpen) ReportSheet(state.report, viewModel::closeReport)
    if (state.picker != Picker.NONE) AppPickerSheet(state, viewModel)
    state.log?.let { LogSheet(it, viewModel::closeLog) }
}

// ---------------------------------------------------------------- header

@Composable
private fun Header(state: MainUiState, vm: MainViewModel) {
    val d = state.device
    Column(Modifier.padding(horizontal = 16.dp).padding(top = 8.dp, bottom = 10.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("M54 Tuner", style = MaterialTheme.typography.headlineSmall)
                Text(
                    "Exynos 1380 · Mali-G68 MP5",
                    style = MaterialTheme.typography.bodySmall,
                    color = TextTertiary,
                )
            }
            LanguageSelector(state.language, vm::setLanguage)
            IconButton(onClick = { vm.openLog() }) {
                Icon(
                    Icons.Filled.Info,
                    localize("Log", LocalAppLanguage.current),
                    tint = TextTertiary,
                    modifier = Modifier.size(20.dp),
                )
            }
            IconButton(onClick = { vm.refresh() }) {
                Icon(
                    Icons.Filled.Refresh,
                    localize("Atualizar", LocalAppLanguage.current),
                    tint = TextTertiary,
                    modifier = Modifier.size(20.dp),
                )
            }
        }
        Spacer(Modifier.height(10.dp))
        // Live strip: what the kernel is doing right now, not what the app asked for.
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState())) {
            val big = d.clusters.firstOrNull { it.tag != "policy0" }
            val little = d.clusters.firstOrNull { it.tag == "policy0" }
            StatChip("BIG", big?.let { "${it.curMhz()}" } ?: "—", GameAccent)
            Spacer(Modifier.width(8.dp))
            StatChip("LITTLE", little?.let { "${it.curMhz()}" } ?: "—", BalancedAccent)
            Spacer(Modifier.width(8.dp))
            // gpu_clock reads 0 while the GPU rail is powered down. That is a true reading, but
            // "0 · 0%" looks like a broken sensor, so say what it means.
            StatChip(
                "GPU",
                d.gpu?.let { g ->
                    if (g.cur <= 0) localize("ocioso", LocalAppLanguage.current)
                    else "${g.curMhz()}" + (g.busy?.let { " · $it%" } ?: "")
                } ?: "—",
                RenderAccent,
            )
            Spacer(Modifier.width(8.dp))
            StatChip("MIF", d.mifCur?.let { "${it / 1000}" } ?: "—", PowerSaveAccent)
            Spacer(Modifier.width(8.dp))
            StatChip(
                "TEMP",
                d.tempC?.let { "%.1f°".format(it) } ?: "—",
                when {
                    (d.tempC ?: 0.0) >= 70 -> DangerAccent
                    (d.tempC ?: 0.0) >= 55 -> WarnAccent
                    else -> PowerSaveAccent
                },
            )
        }
    }
}

@Composable
private fun LanguageSelector(selected: AppLanguage, onSelect: (AppLanguage) -> Unit) {
    Row(
        Modifier
            .clip(RoundedCornerShape(10.dp))
            .background(SurfaceVariantDark),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        listOf(AppLanguage.PT_BR to "PT-BR", AppLanguage.EN to "ENG").forEach { (language, label) ->
            TextButton(
                onClick = { onSelect(language) },
                contentPadding = PaddingValues(horizontal = 5.dp, vertical = 0.dp),
                modifier = Modifier.height(34.dp),
            ) {
                Text(
                    label,
                    color = if (selected == language) RenderAccent else TextTertiary,
                    style = MaterialTheme.typography.labelSmall,
                )
            }
        }
    }
}

@Composable
private fun StatusBanner(state: MainUiState, vm: MainViewModel) {
    val problem: Triple<Color, String, (() -> Unit)?>? = when {
        state.rootState == RootState.CHECKING -> Triple(TextTertiary, "Verificando root…", null)
        state.rootState == RootState.UNAVAILABLE ->
            Triple(DangerAccent, "Sem root. Autorize o app no KernelSU e toque para tentar de novo.") { vm.refresh() }
        !state.device.moduleInstalled ->
            Triple(DangerAccent, "Módulo não instalado — flasheie m54tuner-module.zip no KernelSU.", null)
        !state.device.factoryCaptured ->
            Triple(WarnAccent, "Snapshot ausente. O motor adaptativo mantém sua própria captura reversível da sessão.", null)
        else -> null
    }
    if (problem != null) {
        val (color, text, action) = problem
        Row(
            Modifier
                .padding(horizontal = 16.dp)
                .fillMaxWidth()
                .clip(RoundedCornerShape(14.dp))
                .background(color.copy(alpha = 0.12f))
                .border(1.dp, color.copy(alpha = 0.35f), RoundedCornerShape(14.dp))
                .then(if (action != null) Modifier.clickable { action() } else Modifier)
                .padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (state.rootState == RootState.CHECKING) {
                CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp, color = color)
            } else {
                Icon(Icons.Filled.Warning, null, tint = color, modifier = Modifier.size(18.dp))
            }
            Spacer(Modifier.width(10.dp))
            Text(localize(text, LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall)
        }
        Spacer(Modifier.height(8.dp))
    }

    // The ONLY prompt for a SurfaceFlinger restart. Props that need it are already set; the blink
    // waits here for an explicit tap instead of happening behind every render toggle.
    PendingBanner(
        visible = state.device.pendingSf,
        title = "Pendente: reiniciar SurfaceFlinger",
        desc = "Reinicie o compositor para ativar as alterações (a tela pisca).",
        accent = RenderAccent,
        enabled = !state.busy(Tier.RENDER),
    ) { vm.askSfRestart() }

    // Soft reboot (reinício do zygote): a framework restart, not a boot. Everything read once by
    // zygote — o conjunto dalvik.vm.* — só vale depois dele, e o Zygisk volta a injetar limpo.
    PendingBanner(
        visible = state.device.pendingSoft,
        title = "Pendente: reinício leve (zygote)",
        desc = "Reinicia a interface do sistema para aplicar propriedades de ART.",
        accent = WarnAccent,
        enabled = !state.busy(Tier.ART),
    ) { vm.askSoftReboot() }
}

@Composable
private fun PendingBanner(
    visible: Boolean,
    title: String,
    desc: String,
    accent: Color,
    enabled: Boolean,
    onApply: () -> Unit,
) {
    AnimatedVisibility(visible = visible) {
        Column {
            Row(
                Modifier
                    .padding(horizontal = 16.dp)
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(14.dp))
                    .background(accent.copy(alpha = 0.12f))
                    .border(1.dp, accent.copy(alpha = 0.35f), RoundedCornerShape(14.dp))
                    .padding(12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text(localize(title, LocalAppLanguage.current), style = MaterialTheme.typography.titleSmall)
                    Text(localize(desc, LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextTertiary)
                }
                Spacer(Modifier.width(8.dp))
                FilledTonalButton(onClick = onApply, enabled = enabled) {
                    Text(localize("Aplicar", LocalAppLanguage.current))
                }
            }
            Spacer(Modifier.height(8.dp))
        }
    }
}

@Composable
private fun TabBar(selected: Int, onSelect: (Int) -> Unit) {
    Row(
        Modifier
            .padding(horizontal = 16.dp)
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(SurfaceDark)
            .padding(4.dp),
    ) {
        TABS.forEachIndexed { i, t ->
            val active = i == selected
            Box(
                Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(11.dp))
                    .background(if (active) SurfaceVariantDark else Color.Transparent)
                    .clickable { onSelect(i) }
                    .padding(vertical = 9.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    localize(t, LocalAppLanguage.current),
                    style = MaterialTheme.typography.labelLarge,
                    color = if (active) MaterialTheme.colorScheme.onSurface else TextTertiary,
                    maxLines = 1,
                )
            }
        }
    }
    Spacer(Modifier.height(12.dp))
}

// ---------------------------------------------------------------- tabs

private fun LazyListScope.profileTab(state: MainUiState, vm: MainViewModel) {
    val cfg = state.config
    val on = state.ready
    val busy = state.busy(Tier.LIVE)
    item {
        SectionCard("Controle automático", "Aprendizado contínuo durante o uso normal.", BalancedAccent, busy) {
            Note("O motor ajusta o hardware em tempo real com base na demanda, minimizando latência, consumo e calor.", BalancedAccent)
            if (cfg.adaptiveMode != "active" || !cfg.adaptiveLearning) {
                Note("O aprendizado automático está pausado por uma configuração de diagnóstico.", WarnAccent)
                FilledTonalButton(onClick = vm::resumeAutomatic, enabled = on && !busy) {
                    Text(localize("Retomar aprendizado automático", LocalAppLanguage.current))
                }
            }
            Spacer(Modifier.height(10.dp))
            SwitchRow("Ajuste PELT", "Permite ao motor ajustar o rastreamento de carga da CPU.",
                cfg.adaptivePelt, on && !busy, BalancedAccent, badge = CONTEXT_BADGE) { vm.setAdaptivePelt(it) }
            FieldLabel("Meta de FPS")
            Tag(CONTEXT_BADGE, BalancedAccent)
            Spacer(Modifier.height(6.dp))
            PillRow(listOf(30, 60, 90, 120).map { PillOption(it.toString(), it.toString()) },
                cfg.adaptiveTargetFps.toString(), BalancedAccent, on && !busy, perRow = 4) {
                it.toIntOrNull()?.let(vm::setAdaptiveTarget)
            }
            Spacer(Modifier.height(10.dp))
            InfoRow("Limite adaptativo · leitura", "${cfg.adaptiveThermalLimit} °C")
            Spacer(Modifier.height(8.dp))
            Note("Ajustes sinalizados alteram o contexto de aprendizado do motor.", TextTertiary)
        }
    }

    item { FieldLabel("Medições · somente leitura") }

    item {
        val ai = state.device.adaptive
        SectionCard("Medições do motor", "Leitura · resultados observados no aparelho", BalancedAccent) {
            InfoRow("Controlador", if (ai["running"] == "1") ai["owner"] ?: "M54" else "parado")
            val reason = when (ai["reason"]) {
                "mcts", "learned" -> "Avaliando resultados e próximos ajustes"
                "value_only" -> "Janela creditada ao avaliador; sem amostra de ajuste"
                "rehearsing" -> "Ocioso: revisando janelas já medidas"
                "settling" -> "Aguardando estabilização"
                "waiting_frames" -> "Aguardando frames recentes"
                "screen_idle" -> "Tela ociosa; ajustes liberados"
                "thermal_or_sensor_guard" -> "Proteção térmica ou sensor indisponível"
                "observe_only" -> "Observação: propostas sem aplicação nem treino"
                "benchmark" -> "Pausado durante benchmark"
                "write_rejected", "external_write" -> "Ajuste recusado ou alterado por outro controlador"
                "apply_busy" -> "Aguardando aplicação em andamento"
                "stopped_restored" -> "Parado; ajustes restaurados"
                "restore_pending" -> "Restauração pendente"
                "sample_rejected" -> "Amostra descartada por mudança de contexto"
                else -> ai["reason"]?.takeIf { it.isNotBlank() } ?: "Aguardando inicialização"
            }
            InfoRow("Estado", reason)
            InfoRow("Janelas aprendidas", ai["windows"] ?: "0")
            InfoRow("Amostras reais aceitas", ai["samples"] ?: "0")
            val confidence = ai["confidence"]?.toDoubleOrNull() ?: 0.0
            InfoRow("Confiança do avaliador", "${(confidence * 100).toInt()}%")
            ProgressBar(confidence.toFloat(), BalancedAccent)
            InfoRow("Contextos medidos", "${ai["contexts"] ?: "0"} · ${ai["policies"] ?: "0"} políticas")
            InfoRow("Mudanças de resposta detectadas", ai["model_surprises"] ?: "0")
            val budget = ai["budget"]?.toDoubleOrNull() ?: 1.0
            InfoRow("Reserva térmica", "${(budget * 100).toInt()}% · teto nível ${ai["ceiling"] ?: "4"}")
            ProgressBar(budget.toFloat(), if (budget < 0.35) GameAccent else PowerSaveAccent)
            InfoRow("Aplicativo", ai["app"]?.takeIf { it.isNotBlank() } ?: "—")
            // Frames are one estimator of unserved demand, not the definition of it. This row says
            // which estimator the controller actually read, so a deficit of zero is never mistaken
            // for a workload being served when nothing was measuring it in the first place.
            InfoRow("Cenário medido", when (ai["regime"]) {
                "render" -> "Renderizando · atraso de frame"
                "compute" -> "Sem frames · espera por recurso"
                else -> "Sem medida utilizável"
            })
            val shortfall = ai["deficit"]?.toDoubleOrNull()
            if (shortfall != null && ai["regime"] != "unmeasured") {
                InfoRow("Demanda não atendida", "${(shortfall * 100).toInt()}%")
            }
            val passive = ai["passive_windows"]?.toIntOrNull() ?: 0
            if (passive > 0) InfoRow("Janelas medidas sem controlar", passive.toString())
            val framing = ai["frames_valid"] == "1"
            // The measured cadence, not the configured ceiling: a 33 ms frame is on time for a
            // 30 fps app and two frames late for a 120 fps one, and only this line says which.
            InfoRow("Cadência medida", if (framing) "${ai["cadence"]} fps (teto ${cfg.adaptiveTargetFps})" else "—")
            InfoRow("Frame time p95", if (framing) "${ai["p95_ms"]} ms" else "—")
            // Aggregate load cannot see one saturated thread: a core pinned at 100% of eight
            // reads as 12.5% overall, which is what a CPU-bound game looks like to an average.
            val thread = ai["thread_peak"]?.toDoubleOrNull()
            if (thread != null && thread > 0) {
                InfoRow("Thread mais quente", "${(thread * 100).toInt()}% de um núcleo")
                ProgressBar(thread.toFloat(), if (thread > 0.9) GameAccent else BalancedAccent)
            }
            if (ai["queue_valid"] == "1") {
                InfoRow("Espera por CPU", "${ai["queue_ms"]} ms · pico ${ai["queue_peak_ms"]} ms")
            }
            InfoRow("Energia", if (ai["energy_source"] == "battery_mA_uV") "${ai["watts"]} W"
                else "Estimativa por carga e clocks")
            InfoRow("Limite adaptativo", "${cfg.adaptiveThermalLimit} °C")
            val locked = ai["locked_nodes"]?.toIntOrNull() ?: 0
            if (locked > 0) {
                Note(
                    "$locked nó(s) estão em modo somente-leitura. Execute scripts/unlock_nodes.sh --apply para destravar.",
                    GameAccent,
                )
            }
            Note("Métricas observadas em tempo real dos sensores do hardware.", BalancedAccent)
        }
    }

    item {
        SectionCard("Clusters", "Barra = janela permitida · sólido = frequência atual", GameAccent, busy) {
            state.device.clusters.forEach { c ->
                InfoRow(
                    "${c.label} · ${c.gov}",
                    // min == max is Samsung's top-app QoS boost holding the cluster, not a setting
                    // of ours. Printing "(2002–2002)" made it read as a bug.
                    if (c.min >= c.max) "${c.curMhz()} MHz · travado"
                    else "${c.curMhz()} MHz  (${c.min / 1000}–${c.maxMhz()})",
                )
                Spacer(Modifier.height(4.dp))
                RangeBar(c.cur, c.min, c.max, c.hwMin, c.hwMax, if (c.label == "BIG") GameAccent else BalancedAccent)
                Spacer(Modifier.height(10.dp))
            }
            if (state.device.clusters.any { it.min >= it.max }) {
                Note("Clock fixado indica boost temporário de primeiro plano da Samsung.", GameAccent)
                Spacer(Modifier.height(8.dp))
            }
            if (state.device.clusters.isEmpty()) {
                Text(localize("Sem leitura — módulo indisponível.", LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextTertiary)
            }
            // The governor picker was removed with the rest of the manual DVFS overrides: the
            // engine owns the CPU floor and reads the governor as state. What is worth showing is
            // which one is actually in effect, which is not always the one that was requested.
            state.device.clusters.forEach { c ->
                InfoRow("Governor · ${c.label}", c.gov.ifEmpty { "—" })
            }
        }
    }

    item {
        SectionCard("I/O e scheduler", "Quem decide a ordem das leituras", PowerSaveAccent, busy) {
            state.device.ioSched.forEach { (dev, sched) -> InfoRow(dev, sched) }
            InfoRow("PELT multiplier", state.device.pelt.ifEmpty { "—" })
            InfoRow("EAS", when (state.device.eas) { "1" -> "ligado"; "0" -> "desligado"; else -> "—" })
            val dp = state.device.deep
            if (dp.isNotEmpty()) {
                Spacer(Modifier.height(10.dp))
                FieldLabel("Sistema de arquivos e ociosidade")
                InfoRow(
                    "Coleta de lixo do f2fs",
                    when (dp["gc_urgent"]) {
                        "GC_URGENT_LOW" -> "só quando ocioso"
                        "GC_URGENT_HIGH" -> "forçada"
                        else -> "normal"
                    },
                )
                InfoRow(
                    "Estado profundo de idle",
                    if (dp["deep_disabled"] == "1") "desligado (acorda na hora)"
                    else "ligado (230–340 µs para acordar)",
                )
                dp["ipu_policy"]?.let { ipu ->
                    val n = ipu.toIntOrNull() ?: 0
                    val bits = buildList {
                        if (n and 0x01 != 0) add("FORCE")
                        if (n and 0x02 != 0) add("SSR")
                        if (n and 0x04 != 0) add("UTIL")
                        if (n and 0x08 != 0) add("SSR_UTIL")
                        if (n and 0x10 != 0) add("FSYNC")
                        if (n and 0x20 != 0) add("ASYNC")
                        if (n and 0x40 != 0) add("NOCACHE")
                    }
                    InfoRow("Escrita no lugar (ipu_policy)", bits.joinToString("+").ifEmpty { "nenhum" })
                }
                InfoRow("vfs_cache_pressure", dp["cache_pressure"] ?: "—")
            }
        }
    }

    item {
        SectionCard(
            "Barramento de memória",
            "Largura de banda: o teto de fps sustentado",
            BalancedAccent, busy,
        ) {
            val d = state.device
            InfoRow("DRAM agora", "${(d.mifCur ?: 0) / 1000} MHz")
            InfoRow(
                "Janela",
                "${(d.mifMin ?: 0) / 1000}–${(d.mifMax ?: 0) / 1000} MHz",
            )
            InfoRow("INT agora", "${(d.intCur ?: 0) / 1000} MHz (piso ${(d.intMin ?: 0) / 1000})")
            InfoRow("Display agora", "${(d.dispCur ?: 0) / 1000} MHz (piso ${(d.dispMin ?: 0) / 1000})")
            Note(
                "A largura de banda da DRAM define o limite de quadros sustentado pelo SoC.",
                BalancedAccent,
            )
        }
    }

    item {
        SectionCard("Fila do disco e link UFS", "O que o perfil ajustou por baixo", PowerSaveAccent, busy) {
            val q = state.device.ioq
            if (q.isEmpty()) {
                Text(localize("Sem leitura.", LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextTertiary)
            } else {
                InfoRow("nr_requests · read_ahead", "${q["nr"] ?: "—"} · ${q["ra"] ?: "—"} kB")
                InfoRow("rq_affinity · iostats", "${q["rq_affinity"] ?: "—"} · ${q["iostats"] ?: "—"}")
                q["read_expire"]?.let {
                    InfoRow("read_expire · write_expire", "$it · ${q["write_expire"] ?: "—"} ms")
                }
                q["writes_starved"]?.let {
                    InfoRow("writes_starved · fifo_batch", "$it · ${q["fifo_batch"] ?: "—"}")
                }
                q["prio_aging_expire"]?.let { InfoRow("prio_aging_expire", "$it ms") }
                q["front_merges"]?.let {
                    InfoRow("front_merges · async_depth", "$it · ${q["async_depth"] ?: "—"}")
                }
                q["max_write_starvation"]?.let { InfoRow("max_write_starvation (ssg)", it) }
                q["slice_idle"]?.let { InfoRow("slice_idle (bfq)", "$it ms") }
                q["read_lat_nsec"]?.let {
                    InfoRow(
                        "kyber read · write",
                        "${it.toLongOrNull()?.div(1_000_000) ?: 0} · " +
                            "${q["write_lat_nsec"]?.toLongOrNull()?.div(1_000_000) ?: 0} ms",
                    )
                }
            }
            val u = state.device.ufs
            if (u.isNotEmpty()) {
                Spacer(Modifier.height(10.dp))
                FieldLabel("Link UFS")
                InfoRow(
                    "clock gating",
                    if (u["clkgate"] == "0") "desligado (link acordado)"
                    else "ligado, ${u["clkgate_delay"] ?: "—"} ms",
                )
                InfoRow("auto-hibernate", "${u["hibern8"] ?: "—"} µs")
                InfoRow("piso do BIG durante I/O", "${(u["qos_big"]?.toIntOrNull() ?: 0) / 1000} MHz")
                InfoRow(
                    "runtime PM",
                    if (u["rpm_lvl"] == "1") "nível 1 (dispositivo acordado)"
                    else "nível ${u["rpm_lvl"] ?: "—"} (fábrica)",
                )
                InfoRow("WriteBooster", if (u["wb_on"] == "1") "ligado (de fábrica)" else "desligado")
            }
        }
    }

    item { FieldLabel("Ajustes manuais") }

    item {
        SectionCard(
            "Térmico",
            "Políticas térmicas (proteção da bateria permanece sempre ativa)",
            DangerAccent, busy,
        ) {
            Tag(CONTEXT_BADGE, DangerAccent)
            Spacer(Modifier.height(6.dp))
            PillRow(
                listOf(PillOption("moderate", "Moderado"), PillOption("aggressive", "Agressivo")),
                if (cfg.thermal == ThermalMode.AGGRESSIVE) "aggressive" else "moderate",
                DangerAccent, on && !busy, perRow = 2,
            ) { vm.selectThermal(if (it == "aggressive") ThermalMode.AGGRESSIVE else ThermalMode.MODERATE) }
            Spacer(Modifier.height(10.dp))
            state.device.tempZones.forEach { (z, t) -> InfoRow(z, "%.1f °C".format(t)) }
            InfoRow(
                "Zonas CPU/GPU",
                if (state.device.thermalAggressive) "throttle desligado" else "fábrica",
                if (state.device.thermalAggressive) DangerAccent else TextSecondary,
            )
            Spacer(Modifier.height(10.dp))
            SwitchRow(
                "Guardião térmico",
                "No modo agressivo, restaura o throttle automaticamente por temperatura ou após 15 minutos.",
                cfg.thermalGuard, on && !busy, DangerAccent,
            ) { vm.toggleThermalGuard(it) }
            if (cfg.thermal == ThermalMode.AGGRESSIVE && cfg.thermalGuard) {
                InfoRow(
                    "Fail-safe",
                    if (state.device.thermalGuardRunning) {
                        state.device.thermalGuardState.ifEmpty { "monitorando" }
                    } else "aguardando o vigia",
                )
            }
        }
    }

    item {
        SectionCard("Samsung", "GOS e serviços de energia da Samsung", BalancedAccent, busy) {
            // Follows the real package state, so it can never claim something the system denies.
            SwitchRow(
                "Game Optimizing Service",
                if (state.device.gos == "absent") "Não está instalado neste firmware."
                else "Ligado, a Samsung limita clocks por jogo. Desligar é reversível.",
                state.device.gos == "enabled",
                on && !busy && state.device.gos != "absent",
                DangerAccent, badge = CONTEXT_BADGE,
            ) { vm.setGos(if (it) GosPref.ENABLED else GosPref.DISABLED) }
            Spacer(Modifier.height(10.dp))
            val sm = state.device.samsung
            SwitchRow(
                "Desligar os limitadores da OneUI",
                "Desativa restrições em segundo plano e economia adaptativa da OneUI.",
                cfg.samsungPerf, on && !busy, DangerAccent, badge = CONTEXT_BADGE,
            ) { vm.toggleSamsung(it) }
            if (sm.isNotEmpty()) {
                Spacer(Modifier.height(6.dp))
                InfoRow("Baixo calor", if (sm["low_heat"] == "0") "desligado" else "LIGADO (limita)")
                InfoRow("Background por IA", if (sm["bg_ai"] == "0") "desligada" else "LIGADA (mata apps)")
                InfoRow("restricted_device_performance", sm["restricted_perf"] ?: "—")
                InfoRow("Boost de responsividade", if (sm["cpu_resp"] == "1") "ligado" else "desligado")
                InfoRow("Economia adaptativa", if (sm["adaptive_ps"] == "0") "desligada" else "LIGADA")
            }
        }
    }

}

private fun LazyListScope.diagnosticTab(state: MainUiState, vm: MainViewModel) {
    val cfg = state.config
    val on = state.ready
    val busy = state.busy(Tier.LIVE)
    item {
        SectionCard("Laboratório", "Ferramentas de diagnóstico", WarnAccent, busy) {
            Note("Controle manual do motor: Ativo (com aprendizado), Observar (sem aplicar) ou Parar.", WarnAccent)
            Spacer(Modifier.height(10.dp))
            PillRow(listOf(PillOption("active", "Retomar"), PillOption("observe", "Observar"), PillOption("off", "Parar")),
                cfg.adaptiveMode, WarnAccent, on && !busy) {
                if (it == "active") vm.resumeAutomatic() else vm.setAdaptiveMode(it)
            }
        }
    }
    item { FieldLabel("Experimentos") }

    item {
        val bench = state.device.schedBench
        val running = bench.startsWith("running|")
        SectionCard(
            "Medição PELT × EAS",
            "Compara combinações de PELT e EAS com frame times reais.",
            GameAccent,
            running,
        ) {
            Note(
                "Experimento de ~9 minutos comparando combinações de PELT e EAS. Restaura as configurações ao finalizar.",
                GameAccent,
            )
            Spacer(Modifier.height(10.dp))
            if (bench.isNotEmpty()) {
                InfoRow("Estado", bench.replace("|", " · "))
                Spacer(Modifier.height(8.dp))
            }
            if (running) {
                OutlinedButton(onClick = vm::stopSchedBench) {
                    Text(localize("Parar e restaurar", LocalAppLanguage.current))
                }
            } else {
                FilledTonalButton(
                    onClick = vm::startSchedBench,
                    enabled = on && !busy,
                ) {
                    Text(localize("Pausar motor e medir (~9 min)", LocalAppLanguage.current))
                }
            }
        }
    }

}

private fun LazyListScope.gpuTab(state: MainUiState, vm: MainViewModel) {
    val g = state.device.gpu
    val busy = state.busy(Tier.LIVE)

    item {
        SectionCard("Mali-G68 MP5", "Leitura direta de /sys/kernel/gpu", RenderAccent, busy) {
            if (g == null) {
                Text(localize("GPU não legível.", LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextTertiary)
            } else {
                InfoRow(
                "Clock atual",
                if (g.cur <= 0) "ocioso (rail desligado)"
                else "${g.curMhz()} MHz" + (g.busy?.let { "  ·  $it% de uso" } ?: ""),
            )
                Spacer(Modifier.height(4.dp))
                RangeBar(g.cur, g.min, g.max, g.table.firstOrNull() ?: 0, g.table.lastOrNull() ?: 1, RenderAccent)
                Spacer(Modifier.height(8.dp))
                InfoRow("Janela travada", "${g.minMhz()}–${g.maxMhz()} MHz")
                InfoRow("Governor ativo", g.gov)
                InfoRow("Power policy", g.powerPolicy)
                InfoRow("Highspeed", "load ${g.hsLoad}% · clock ${g.hsClock} · delay ${g.hsDelay}")
                InfoRow(
                    "dvfs_period · polling_speed",
                    "${g.dvfsPeriod.ifEmpty { "—" }} · ${g.pollingSpeed.ifEmpty { "—" }} ms",
                )
                InfoRow("js_scheduling_period", "${g.jsPeriod.ifEmpty { "—" }} ms")
            }
        }
    }

    item {
        SectionCard(
            "Controle de clocks da GPU",
            "Frequências gerenciadas pelo motor adaptativo",
            RenderAccent, busy,
        ) {
            Note(
                "Valores lidos diretamente de /sys/kernel/gpu. O motor ajusta a faixa operacional dinamicamente.",
                RenderAccent,
            )
        }
    }
}

private fun LazyListScope.renderTab(state: MainUiState, vm: MainViewModel) {
    val cfg = state.config
    val on = state.ready
    val busy = state.busy(Tier.RENDER)

    item {
        SectionCard(
            "Escopo de reinício",
            "Cada ajuste custa só o reinício que ele realmente precisa.",
            RenderAccent,
        ) {
            InfoRow("HWUI (renderer)", "força parada dos apps escolhidos")
            InfoRow("RenderEngine", "reinicia o SurfaceFlinger (tela pisca)")
            InfoRow("Perfil, GPU, I/O, térmico", "nada reinicia")
        }
    }

    item {
        SectionCard(
            "HWUI (vale para todos os apps)",
            "A prop é global; a lista abaixo só define quem é fechado para reler agora.",
            RenderAccent, busy,
        ) {
            FieldLabel("Renderer")
            PillRow(
                listOf(PillOption("skiagl", "SkiaGL"), PillOption("skiavk", "SkiaVK")),
                cfg.hwuiRenderer, RenderAccent, on && !busy, perRow = 2,
            ) { vm.setHwuiRenderer(it) }
            Spacer(Modifier.height(8.dp))
            Note(
                "Propriedade global. SkiaGL é o padrão de fábrica; SkiaVK é experimental no driver Mali.",
                DangerAccent,
            )
            Spacer(Modifier.height(8.dp))
            SwitchRow(
                "Reiniciar a SystemUI junto",
                "Aplica as props também na interface do sistema (a barra pisca).",
                cfg.restartSystemUi, on && !busy, RenderAccent,
            ) { vm.toggleRestartSystemUi(it) }
            Spacer(Modifier.height(8.dp))
            PickerRow(
                "Apps afetados",
                if (cfg.renderApps.isEmpty()) "Nenhum — as props só valerão quando o app for reaberto."
                else "${cfg.renderApps.size} app(s) serão parados ao mudar o render.",
                on && !busy,
            ) { vm.openPicker(Picker.RENDER_APPS) }
        }
    }

    item {
        SectionCard(
            "SurfaceFlinger",
            "Só o SF lê estas — mudança fica pendente até você confirmar.",
            RenderAccent, busy,
        ) {
            FieldLabel("Backend do RenderEngine")
            PillRow(
                listOf(
                    PillOption("skiaglthreaded", "SkiaGL thr."),
                    PillOption("skiavkthreaded", "SkiaVK thr."),
                ),
                cfg.reBackend, RenderAccent, on && !busy, perRow = 2,
            ) { vm.setReBackend(it) }
            Spacer(Modifier.height(8.dp))
            Note(
                "O SurfaceFlinger requer backends threaded. Padrão: SkiaGL thr.",
                DangerAccent,
            )
            Spacer(Modifier.height(8.dp))
            InfoRow("debug.hwui.renderer", state.device.props["hwui_renderer"].orEmpty().ifEmpty { "—" })
            InfoRow("debug.renderengine.backend", state.device.props["re_backend"].orEmpty().ifEmpty { "—" })
            InfoRow("RenderEngine ativo agora", state.device.props["re_live"].orEmpty().ifEmpty { "—" })
        }
    }
}

private fun LazyListScope.memoryTab(state: MainUiState, vm: MainViewModel) {
    val cfg = state.config
    val on = state.ready
    val busy = state.busy(Tier.MEM)

    item {
        SectionCard(
            "zram",
            "A Samsung reconstrói como lzo-rle todo boot; o módulo reaplica a sua escolha.",
            PowerSaveAccent, busy,
        ) {
            val algos = state.device.zramAlgos.ifEmpty { listOf("lzo", "lzo-rle", "lz4", "zstd") }
            // Selection follows the LIVE algorithm, never the stored preference: showing lz4 as
            // picked while the kernel is still on lzo-rle (because it was never applied) is exactly
            // the kind of lie this app is not allowed to tell.
            val live = state.device.zramAlgo
            PillRow(
                algos.map { PillOption(it, it) }, live, PowerSaveAccent, on && !busy, perRow = 4,
            ) { vm.setZramAlgo(it) }
            Spacer(Modifier.height(10.dp))
            InfoRow("Algoritmo ativo", live.ifEmpty { "—" })
            if (cfg.zramAlgo.isNotEmpty() && live.isNotEmpty() && cfg.zramAlgo != live) {
                InfoRow("Escolhido, ainda não aplicado", cfg.zramAlgo, WarnAccent)
            }
            InfoRow("Tamanho", "4 GB (padrão Samsung, não alterado)")
            InfoRow("swappiness", state.device.swappiness?.toString() ?: "—")
            Spacer(Modifier.height(10.dp))
            Note(
                "Trocar o algoritmo recria o swap mantendo os 4 GB de fábrica. Libera memória antes se necessário.",
                PowerSaveAccent,
            )
        }
    }
}

private fun LazyListScope.appsTab(state: MainUiState, vm: MainViewModel) {
    val cfg = state.config
    val on = state.ready
    val busy = state.busy(Tier.DEXOPT)

    // ART/zygote lives next to dexopt because it is the same problem from the other side: dexopt
    // decides how app code is compiled, these decide how the process that runs it is created.
    item {
        SectionCard(
            "ART / zygote",
            "Vale a partir do próximo reinício leve — nada aqui muda um sistema já rodando.",
            WarnAccent, state.busy(Tier.ART),
            trailing = { if (state.device.pendingSoft) Tag("pendente", WarnAccent) },
        ) {
            val artOn = on && !state.busy(Tier.ART)
            FeatureSwitch(
                "Processos prontos no zygote (USAP)",
                "O app abre sem esperar fork+preload — a fatia da abertura que nenhum ajuste de DVFS alcança.",
                "2 a 4 processos ociosos ocupando RAM. A Samsung desliga de fábrica.",
                cfg.artUsap == TriState.ON, artOn, WarnAccent, badge = "recomendado",
            ) { vm.setArtUsap(if (it) TriState.ON else TriState.OFF) }
            Spacer(Modifier.height(10.dp))
            FeatureSwitch(
                "Compilar (dex2oat) só no LITTLE",
                "Play Store e ART service compilam em cpu0-3, sem roubar os A78 durante o jogo.",
                "Instalar e atualizar app fica mais lento: 4 núcleos pequenos em vez de 8.",
                cfg.artDex2oatLittle == TriState.ON, artOn, WarnAccent, badge = "recomendado p/ jogos",
            ) { vm.setArtDex2oat(if (it) TriState.ON else TriState.OFF) }
            Spacer(Modifier.height(10.dp))
            FeatureSwitch(
                "Teto de heap por app: 256 → 288 MB",
                "Apps pesados chegam ao GC forçado menos vezes.",
                "Vale para TODO app, então a pressão de RAM sobe um pouco. Ganho pequeno.",
                cfg.artHeap == TriState.ON, artOn, WarnAccent, badge = "opcional",
            ) { vm.setArtHeap(if (it) TriState.ON else TriState.OFF) }
            Spacer(Modifier.height(12.dp))
            InfoRow("usap_pool_enabled", state.device.props["usap"].orEmpty().ifEmpty { "—" })
            InfoRow("dex2oat-cpu-set", state.device.props["dex2oat_cpuset"].orEmpty().ifEmpty { "(livre)" })
            InfoRow("heapgrowthlimit", state.device.props["heap_growth"].orEmpty().ifEmpty { "—" })
            if (state.device.pendingSoft) {
                Spacer(Modifier.height(10.dp))
                FilledTonalButton(
                    onClick = { vm.askSoftReboot() },
                    enabled = !state.busy(Tier.ART),
                ) { Text(localize("Reinício leve agora", LocalAppLanguage.current)) }
            }
        }
    }

    item {
        SectionCard(
            "Compilação AOT (${cfg.dexoptMode})",
            "Elimina o jank de JIT. Leva minutos e ocupa espaço — ação explícita.",
            GameAccent, busy,
        ) {
            PickerRow(
                "Jogos selecionados",
                if (cfg.games.isEmpty()) "Nenhum — toque para escolher." else "${cfg.games.size} selecionado(s).",
                on && !busy,
            ) { vm.openPicker(Picker.GAMES) }
            Spacer(Modifier.height(10.dp))
            Row {
                FilledTonalButton(
                    onClick = { vm.askDexopt() },
                    enabled = on && !busy && cfg.games.isNotEmpty(),
                ) { Text(localize("Compilar agora", LocalAppLanguage.current)) }
                Spacer(Modifier.width(10.dp))
                OutlinedButton(
                    onClick = { vm.askDexoptReset() },
                    enabled = on && !busy && cfg.games.isNotEmpty(),
                ) { Text(localize("Reverter", LocalAppLanguage.current)) }
            }
            if (busy) {
                Spacer(Modifier.height(10.dp))
                Text(
                    localize("Compilando… pode levar vários minutos.", LocalAppLanguage.current),
                    style = MaterialTheme.typography.bodySmall,
                    color = TextTertiary,
                )
            }
        }
    }

    item {
        // Own busy flag: every action in this card runs on Tier.LIVE (protect/SPCM/MARs/RAM-clear),
        // never Tier.DEXOPT — reusing the AOT card's `busy` here left the whole section greyed out
        // whenever a compile was running (or stuck), with no real connection to what was busy.
        val protectBusy = state.busy(Tier.LIVE)
        val protectOn = on && !protectBusy
        SectionCard(
            "Manter vivo em segundo plano",
            "Sem GOS: prioridade para o jogo direto no kernel",
            DangerAccent, protectBusy,
            trailing = { if (state.device.keepalive) Tag("vigiando", PowerSaveAccent) },
        ) {
            val sm = state.device.samsung
            PickerRow(
                "Apps protegidos",
                if (cfg.protectList.isEmpty()) "Nenhum — toque para escolher." else "${cfg.protectList.size} selecionado(s).",
                protectOn,
            ) { vm.openPicker(Picker.PROTECT_APPS) }
            Spacer(Modifier.height(14.dp))
            // The "clear RAM on Game" auto-toggle was retired here: static profiles are gone, so
            // the profile-gated automatic clear could never fire and the switch promised a clear
            // that never came. "Limpar agora" below is the real one-shot; the engine's own opt-in
            // trim (adaptive_ram_management) is the other. No auto-clear toggle is offered because
            // none exists to offer.
            Spacer(Modifier.height(10.dp))
            // "Ativar Game automaticamente" was removed here. It matched the foreground package
            // against this list and rewrote the profile — a hand-written answer to a question the
            // controller now measures. The profile is a price list; it no longer decides what the
            // engine may try or what it remembers, so nothing has to switch it for the engine to
            // adapt to a game.
            Spacer(Modifier.height(8.dp))
            OutlinedButton(onClick = vm::clearRamNow, enabled = protectOn) {
                Text(localize("Limpar agora", LocalAppLanguage.current))
            }
            Spacer(Modifier.height(14.dp))
            SwitchRow(
                "Isentar os jogos da restrição da Samsung",
                "Adiciona os pacotes à tabela MARs_ExcludeTarget contra suspensão.",
                cfg.samsungProtect, protectOn, PowerSaveAccent,
            ) { vm.toggleSamsungProtect(it) }
            if (sm.isNotEmpty()) {
                InfoRow("Total na tabela do sistema (Samsung + nossos)", sm["excluded"] ?: "—")
            }
            Spacer(Modifier.height(10.dp))
            SwitchRow(
                "Desligar o SPCM",
                "Desativa o gerenciador de processos em segundo plano da Samsung.",
                cfg.samsungSpcm, protectOn, DangerAccent, badge = CONTEXT_BADGE,
            ) { vm.toggleSpcm(it) }
            SwitchRow(
                "Desligar as políticas do MARs",
                "Desativa as políticas 1 e 8 de encerramento do MARs.",
                cfg.samsungMarsOff, protectOn, DangerAccent, badge = CONTEXT_BADGE,
            ) { vm.toggleMarsOff(it) }
            if (sm.isNotEmpty()) {
                InfoRow("SPCM", if (sm["spcm"] == "0") "desligado" else "ligado")
                InfoRow(
                    "Políticas MARs 1 e 8",
                    "${if (sm["mars_p1"] == "0") "off" else "on"} · ${if (sm["mars_p8"] == "0") "off" else "on"}",
                )
            }
            Spacer(Modifier.height(14.dp))
            SwitchRow(
                "Proteger os apps da lista",
                "Mantém o oom_score_adj prioritário contra o encerramento pelo kernel.",
                cfg.protectGames, protectOn, DangerAccent,
            ) { vm.toggleProtect(it) }
            Spacer(Modifier.height(8.dp))
            Note(
                "Protege processos contra o finalizador de memória do kernel. Políticas internas do sistema podem ignorar a pontuação.",
                DangerAccent,
            )
            Spacer(Modifier.height(6.dp))
            InfoRow("Vigia", if (state.device.keepalive) "rodando (1 s)" else "parado")
            InfoRow("Prioridade aplicada", "-700 (acima de app comum, abaixo do sistema)")
            Spacer(Modifier.height(12.dp))
            OutlinedButton(onClick = vm::exportDiagnostics, enabled = protectOn) {
                Text(localize("Exportar diagnóstico", LocalAppLanguage.current))
            }
        }
    }

    item {
        SectionCard("Lista de proteção", null, DangerAccent) {
            if (cfg.protectList.isEmpty()) {
                Text(localize("Vazia.", LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextTertiary)
            } else {
                cfg.protectList.forEach { InfoRow(it, "protegido") }
            }
        }
    }

    item {
        SectionCard("Lista atual", null, GameAccent) {
            if (cfg.games.isEmpty()) {
                Text(localize("Vazia.", LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextTertiary)
            } else {
                cfg.games.forEach { InfoRow(it, "speed") }
            }
        }
    }
}

@Composable
private fun PickerRow(title: String, subtitle: String, enabled: Boolean, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(SurfaceVariantDark)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(14.dp),
        // Top-aligned: centred, the action drifted into the middle of a wrapped subtitle and read
        // as if it were part of the sentence.
        verticalAlignment = Alignment.Top,
    ) {
        Column(Modifier.weight(1f)) {
            Text(localize(title, LocalAppLanguage.current), style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(2.dp))
            Text(localize(subtitle, LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextTertiary)
        }
        Spacer(Modifier.width(12.dp))
        Text(localize("Escolher", LocalAppLanguage.current), style = MaterialTheme.typography.labelLarge, color = RenderAccent)
    }
}

// ---------------------------------------------------------------- sheets & dialogs

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ReportSheet(report: ApplyReport?, onClose: () -> Unit) {
    ModalBottomSheet(onDismissRequest = onClose, containerColor = SurfaceDark) {
        Column(Modifier.padding(horizontal = 20.dp).padding(bottom = 28.dp)) {
            Text(localize("Resultado da aplicação", LocalAppLanguage.current), style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(4.dp))
            Text(
                localize(
                    report?.let { "${it.section} · ${it.summary()}" } ?: "Nada aplicado ainda.",
                    LocalAppLanguage.current,
                ),
                style = MaterialTheme.typography.bodySmall,
                color = TextTertiary,
            )
            Spacer(Modifier.height(14.dp))
            val items = report?.items.orEmpty()
            LazyColumn(Modifier.heightIn(max = 460.dp)) {
                items(items) { item ->
                    val color = when (item.status) {
                        ApplyReport.Status.OK -> PowerSaveAccent
                        ApplyReport.Status.FAIL -> DangerAccent
                        ApplyReport.Status.WARN -> WarnAccent
                        ApplyReport.Status.SKIP -> TextTertiary
                    }
                    Row(Modifier.fillMaxWidth().padding(vertical = 5.dp), verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.size(7.dp).clip(RoundedCornerShape(50)).background(color))
                        Spacer(Modifier.width(10.dp))
                        Text(localize(item.key, LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, modifier = Modifier.weight(1f))
                        Text(
                            when (item.status) {
                                ApplyReport.Status.OK -> item.got
                                ApplyReport.Status.SKIP -> "n/d"
                                else -> "${item.got} ≠ ${item.want}"
                            },
                            style = MaterialTheme.typography.bodySmall,
                            color = color,
                        )
                    }
                    HorizontalDivider(color = OutlineDark.copy(alpha = 0.5f))
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LogSheet(lines: List<String>, onClose: () -> Unit) {
    ModalBottomSheet(onDismissRequest = onClose, containerColor = SurfaceDark) {
        Column(Modifier.padding(horizontal = 20.dp).padding(bottom = 28.dp)) {
            Text(localize("Log do módulo", LocalAppLanguage.current), style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(10.dp))
            if (lines.isEmpty()) {
                Text(localize("Vazio.", LocalAppLanguage.current), style = MaterialTheme.typography.bodySmall, color = TextTertiary)
            } else {
                LazyColumn(Modifier.heightIn(max = 460.dp)) {
                    items(lines) { l ->
                        Text(
                            l,
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            color = TextSecondary,
                        )
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AppPickerSheet(state: MainUiState, vm: MainViewModel) {
    val current = when (state.picker) {
        Picker.GAMES -> state.config.games
        Picker.RENDER_APPS -> state.config.renderApps
        Picker.PROTECT_APPS -> state.config.protectList
        Picker.NONE -> emptyList()
    }
    val selected = remember(state.picker) { mutableStateListOf<String>().apply { addAll(current) } }
    ModalBottomSheet(onDismissRequest = vm::closePicker, containerColor = SurfaceDark) {
        Column(Modifier.padding(horizontal = 16.dp).padding(bottom = 24.dp)) {
            Text(
                localize(when (state.picker) {
                    Picker.GAMES -> "Jogos para compilar"
                    Picker.RENDER_APPS -> "Apps afetados pelo render"
                    Picker.PROTECT_APPS -> "Apps protegidos em segundo plano"
                    Picker.NONE -> ""
                }, LocalAppLanguage.current),
                style = MaterialTheme.typography.titleLarge,
                modifier = Modifier.padding(horizontal = 4.dp),
            )
            Spacer(Modifier.height(10.dp))
            if (state.appsLoading) {
                Row(Modifier.padding(20.dp), verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                    Spacer(Modifier.width(10.dp))
                    Text(localize("Lendo apps instalados…", LocalAppLanguage.current), style = MaterialTheme.typography.bodyMedium)
                }
            } else {
                LazyColumn(Modifier.heightIn(max = 420.dp)) {
                    items(state.apps) { app ->
                        AppRow(app, selected.contains(app.pkg)) {
                            if (selected.contains(app.pkg)) selected.remove(app.pkg) else selected.add(app.pkg)
                        }
                    }
                }
            }
            Spacer(Modifier.height(12.dp))
            Row(horizontalArrangement = Arrangement.End, modifier = Modifier.fillMaxWidth()) {
                TextButton(onClick = vm::closePicker) { Text(localize("Cancelar", LocalAppLanguage.current)) }
                Spacer(Modifier.width(8.dp))
                FilledTonalButton(onClick = {
                    when (state.picker) {
                        Picker.GAMES -> vm.setGames(selected.toList())
                        Picker.RENDER_APPS -> vm.setRenderApps(selected.toList())
                        Picker.PROTECT_APPS -> vm.setProtectList(selected.toList())
                        Picker.NONE -> {}
                    }
                }) { Text(localize("Salvar (${selected.size})", LocalAppLanguage.current)) }
            }
        }
    }
}

@Composable
private fun AppRow(app: AppInfo, checked: Boolean, onToggle: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable { onToggle() }.padding(vertical = 6.dp, horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Checkbox(checked = checked, onCheckedChange = { onToggle() })
        Spacer(Modifier.width(6.dp))
        app.icon?.let {
            androidx.compose.foundation.Image(
                bitmap = it,
                contentDescription = null,
                modifier = Modifier.size(28.dp).clip(RoundedCornerShape(7.dp)),
            )
            Spacer(Modifier.width(10.dp))
        }
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(app.label, style = MaterialTheme.typography.bodyLarge)
                if (app.isGame) {
                    Spacer(Modifier.width(6.dp))
                    Tag("jogo", GameAccent)
                }
            }
            Text(app.pkg, style = MaterialTheme.typography.bodySmall, color = TextTertiary)
        }
    }
}

@Composable
private fun Dialogs(state: MainUiState, vm: MainViewModel) {
    if (state.dialog == Dialog.NONE) return
    val (title, body, confirm) = when (state.dialog) {
        Dialog.AGGRESSIVE -> Triple(
            "Térmico agressivo",
            "Desativa o throttling térmico de CPU e GPU. O aparelho esquentará mais sob carga alta.\n\n" + CONTEXT_NOTICE,
            "Ativar",
        )
        Dialog.RE_BACKEND -> Triple(
            "Backend do RenderEngine",
            "Alternar o backend exige reiniciar o SurfaceFlinger (a tela pisca). O backend Vulkan é experimental no driver Mali.",
            "Continuar",
        )
        Dialog.SF_RESTART -> Triple(
            "Reiniciar o SurfaceFlinger",
            "A tela apagará por alguns segundos para reiniciar o compositor de tela.",
            "Reiniciar",
        )
        Dialog.SOFT_REBOOT -> Triple(
            "Reinício leve (zygote)",
            "Reinicia a interface do sistema para aplicar propriedades de ART. O kernel e root continuam ativos.",
            "Reiniciar interface",
        )
        Dialog.ZRAM -> Triple(
            "Trocar o algoritmo do zram",
            "O swap será recriado mantendo os 4 GB de fábrica. Memória em cache será liberada se necessário.",
            "Trocar",
        )
        Dialog.DEXOPT -> Triple(
            "Compilar os jogos",
            "Compila os jogos selecionados com o perfil de compilação ART escolhido.",
            "Compilar",
        )
        Dialog.DEXOPT_RESET -> Triple(
            "Reverter a compilação",
            "Restaura a compilação padrão (speed-profile) dos jogos selecionados.",
            "Reverter",
        )
        Dialog.LEARNING_CONTEXT -> Triple("Alterar contexto de aprendizado?", CONTEXT_NOTICE, "Aplicar alteração")
        Dialog.NONE -> Triple("", "", "")
    }
    AlertDialog(
        onDismissRequest = vm::dismissDialog,
        icon = { Icon(Icons.Filled.Warning, null, tint = WarnAccent) },
        title = { Text(localize(title, LocalAppLanguage.current)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                state.pendingContextLabel?.let {
                    Text(localize(it, LocalAppLanguage.current), style = MaterialTheme.typography.titleSmall)
                    Spacer(Modifier.height(10.dp))
                }
                Text(localize(body, LocalAppLanguage.current), style = MaterialTheme.typography.bodyMedium)
            }
        },
        confirmButton = {
            TextButton(onClick = vm::confirmDialog) { Text(localize(confirm, LocalAppLanguage.current)) }
        },
        dismissButton = {
            TextButton(onClick = vm::dismissDialog) { Text(localize("Cancelar", LocalAppLanguage.current)) }
        },
        containerColor = SurfaceDark,
    )
}
