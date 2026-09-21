package com.orkgabb.m54tuner

import android.app.Application
import android.content.Context
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.orkgabb.m54tuner.profile.Profile
import com.orkgabb.m54tuner.profile.ThermalMode
import com.orkgabb.m54tuner.root.RootShellManager
import com.orkgabb.m54tuner.system.AppCatalog
import com.orkgabb.m54tuner.system.AppInfo
import com.orkgabb.m54tuner.system.ApplyReport
import com.orkgabb.m54tuner.system.DeviceState
import com.orkgabb.m54tuner.system.GosPref
import com.orkgabb.m54tuner.system.ModuleBridge
import com.orkgabb.m54tuner.system.Tier
import com.orkgabb.m54tuner.system.TriState
import com.orkgabb.m54tuner.system.TunerConfig
import com.orkgabb.m54tuner.ui.AppLanguage
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

enum class RootState { CHECKING, GRANTED, UNAVAILABLE }

/** Modal that needs an explicit answer before the change is worth making. */
enum class Dialog {
    NONE, AGGRESSIVE, RE_BACKEND, SF_RESTART, SOFT_REBOOT, ZRAM, DEXOPT, DEXOPT_RESET,
    LEARNING_CONTEXT, ADOPT_BRAIN,
}

enum class Picker { NONE, GAMES, RENDER_APPS, PROTECT_APPS }

data class MainUiState(
    val rootState: RootState = RootState.CHECKING,
    val config: TunerConfig = TunerConfig(),
    val device: DeviceState = DeviceState(),
    /** Tiers with an apply in flight — only the affected section shows a spinner. */
    val busy: Set<Tier> = emptySet(),
    val report: ApplyReport? = null,
    val reportOpen: Boolean = false,
    val snack: String? = null,
    val dialog: Dialog = Dialog.NONE,
    val picker: Picker = Picker.NONE,
    val apps: List<AppInfo> = emptyList(),
    val appsLoading: Boolean = false,
    val log: List<String>? = null,
    /** Choice waiting behind a confirmation dialog (applied only if the user says yes). */
    val pendingZram: String? = null,
    val pendingAdopt: String? = null,
    val pendingReBackend: String? = null,
    val pendingContextLabel: String? = null,
    val language: AppLanguage = AppLanguage.PT_BR,
) {
    val ready: Boolean get() = rootState == RootState.GRANTED && device.moduleInstalled
    fun busy(tier: Tier) = busy.contains(tier)
}

/**
 * Every user action follows the same shape: update the config in memory so the UI reacts on the
 * same frame, then apply the ONE tier that change belongs to, then refresh from the kernel and
 * report what really stuck. Nothing blocks the whole screen, and no tier ever runs a heavier tier's
 * work as a side effect.
 */
class MainViewModel(application: Application) : AndroidViewModel(application) {

    private val prefs = application.getSharedPreferences("m54tuner", Context.MODE_PRIVATE)
    private val initialLanguage = when (prefs.getString("language", AppLanguage.PT_BR.code)) {
        AppLanguage.EN.code -> AppLanguage.EN
        else -> AppLanguage.PT_BR
    }
    private val _uiState = MutableStateFlow(MainUiState(language = initialLanguage))
    val uiState: StateFlow<MainUiState> = _uiState.asStateFlow()

    private val applyLock = Mutex()
    private val pending = HashMap<Tier, Job>()
    private var pendingContextAction: (() -> Unit)? = null
    private var persistedConfig = TunerConfig()
    private var configLoaded = false

    /** Hold the operation itself: cancelling must neither save config nor run a module script. */
    private fun confirmContextChange(next: TunerConfig, label: String, action: () -> Unit): Boolean {
        if (cfg().sameLearningContext(next)) return false
        pendingContextAction = action
        update { it.copy(dialog = Dialog.LEARNING_CONTEXT, pendingContextLabel = label) }
        return true
    }

    init {
        bootstrap()
        startPolling()
    }

    private fun update(block: (MainUiState) -> MainUiState) { _uiState.value = block(_uiState.value) }
    private fun cfg() = _uiState.value.config

    // ---------------- bootstrap ----------------

    private fun bootstrap() {
        viewModelScope.launch {
            if (!RootShellManager.hasRoot()) {
                update { it.copy(rootState = RootState.UNAVAILABLE) }
                return@launch
            }
            // Fast path: skip the Samsung/MARs reads (9 subprocess spawns) so the screen is
            // interactive as soon as possible; they are fetched in the background below.
            val device = ModuleBridge.status(full = false) ?: run {
                update { it.copy(rootState = RootState.GRANTED, snack = "Falha ao ler o estado do módulo") }
                return@launch
            }
            val config = if (device.moduleInstalled) {
                ModuleBridge.readConfig() ?: run {
                    update { it.copy(rootState = RootState.GRANTED, device = device, snack = "Falha ao ler a configuração") }
                    return@launch
                }
            } else TunerConfig()
            persistedConfig = config
            configLoaded = true
            update { it.copy(rootState = RootState.GRANTED, device = device, config = config) }
            if (!device.moduleInstalled) return@launch

            // Temp-root: KSU has no boot hook, so the first app launch of a boot session IS the
            // boot sequence (props + one SurfaceFlinger restart + zram + profile). Nothing on this
            // device changes the live-tier sysfs nodes between two launches in the SAME session
            // (the only thing that does — an SF restart — is tracked separately), so later launches
            // no longer blind-reapply ~50 nodes just to open the app; they only refresh the readout
            // already fetched above.
            // Always claim the first temp-root launch. Profile NONE can still have zram, renderer,
            // ART, thermal or the automatic-game watcher configured, so profile/fps alone are not
            // a valid test for whether the boot sequence has work to do.
            val claim = ModuleBridge.consumeFirstOfSession()
            if (claim != null) {
                update { it.copy(busy = setOf(Tier.LIVE)) }
                try {
                    val report = ModuleBridge.applyBoot()
                    // A claim spent on a failed restore would run the session untuned while the
                    // UI looks normal. Release it so the next launch retries.
                    finish(report, "Restaurado após o boot", full = true)
                } catch (e: Exception) {
                    if (e is CancellationException) throw e
                    update {
                        it.copy(
                            busy = it.busy - Tier.LIVE,
                            snack = "Restauração após o boot falhou — será tentada de novo",
                        )
                    }
                } finally {
                    withContext(NonCancellable) { ModuleBridge.releaseFirstOfSession(claim) }
                }
            }

            // Samsung/MARs info is worth having, just not on the critical path — fetch it once in
            // the background now that the screen is already up.
            launch {
                val full = ModuleBridge.status(full = true) ?: return@launch
                update { it.copy(device = it.device.copy(samsung = full.samsung)) }
            }
        }
    }

    /** Live readouts (freqs, GPU load, temperature) refresh while the screen is up. */
    /**
     * Set while the user's finger is on the list. A poll rewrites the whole state object and
     * recomposes the visible cards; doing that in the middle of a fling costs frames, and the
     * readouts are worth nothing during a scroll anyway.
     */
    @Volatile
    private var scrolling = false

    fun setScrolling(value: Boolean) { scrolling = value }

    fun setLanguage(language: AppLanguage) {
        prefs.edit().putString("language", language.code).apply()
        update { it.copy(language = language) }
    }

    /** [status] without --full reports an empty Samsung/MARs map (it did not ask); keep whatever
     *  was last actually fetched instead of flashing that section blank every poll. */
    private fun mergeSamsung(fresh: DeviceState, old: DeviceState): DeviceState =
        if (fresh.samsung.isEmpty()) fresh.copy(samsung = old.samsung) else fresh

    private fun startPolling() {
        viewModelScope.launch {
            while (true) {
                delay(3000)
                // One bad poll (lost root, torn-down shell, unparseable line) must cost one
                // poll, not the whole loop: an unguarded throw here ends this coroutine forever
                // and the screen silently stops updating.
                try {
                    val s = _uiState.value
                    if (!scrolling && s.rootState == RootState.GRANTED && s.busy.isEmpty() && s.device.moduleInstalled) {
                        val d = ModuleBridge.status(full = false) ?: continue
                        update { it.copy(device = mergeSamsung(d, s.device)) }
                    }
                } catch (e: Exception) {
                    if (e is CancellationException) throw e
                }
            }
        }
    }

    /** Explicit, user-triggered refresh: worth paying for the Samsung/MARs reads here. */
    fun refresh() {
        viewModelScope.launch {
            if (_uiState.value.rootState != RootState.GRANTED) {
                if (!RootShellManager.hasRoot()) return@launch
                update { it.copy(rootState = RootState.GRANTED) }
            }
            val d = ModuleBridge.status(full = true) ?: run {
                update { it.copy(snack = "Falha ao atualizar o estado do módulo") }
                return@launch
            }
            val disk = if (d.moduleInstalled) ModuleBridge.readConfig() else null
            val c = disk ?: cfg()
            if (disk != null) { persistedConfig = disk; configLoaded = true }
            update { it.copy(device = d, config = c) }
        }
    }

    // ---------------- the one apply path ----------------

    /**
     * Writes the in-memory config to disk. A failed write (full /data, SELinux denial, lost
     * root) rolls the optimistic edit back to what is actually on disk and reports it: running
     * a tier script against the stale file and then snacking success would assert a state that
     * exists nowhere — not in memory, not on disk, not in the kernel.
     */
    private suspend fun persistOrRevert(tier: Tier, label: String): Boolean {
        if (ModuleBridge.writeConfig(_uiState.value.config)) {
            persistedConfig = _uiState.value.config
            configLoaded = true
            return true
        }
        val disk = ModuleBridge.readConfig()?.also {
            persistedConfig = it
            configLoaded = true
        } ?: persistedConfig.takeIf { configLoaded }
        update {
            it.copy(
                config = disk ?: it.config,
                busy = it.busy - tier,
                snack = "$label — falha ao gravar; nada foi aplicado",
            )
        }
        return false
    }
    /**
     * Optimistic edit + debounced apply of a single tier. Rapid taps (dragging across the pill row,
     * flipping a switch back and forth) coalesce into one shell run instead of queueing a run per
     * tap, which is what used to make the UI feel like it was falling behind.
     */
    private fun edit(tier: Tier, label: String, contextConfirmed: Boolean = false, transform: (TunerConfig) -> TunerConfig) {
        if (_uiState.value.rootState != RootState.GRANTED) return
        val next = transform(cfg())
        if (next == cfg()) return
        if (!contextConfirmed && confirmContextChange(next, label) {
                edit(tier, label, contextConfirmed = true, transform = transform)
            }) return
        update { it.copy(config = next, busy = it.busy + tier) }
        pending.remove(tier)?.cancel()
        pending[tier] = viewModelScope.launch {
            delay(220)
            applyLock.withLock {
                if (!persistOrRevert(tier, label)) return@withLock
                val report = when (tier) {
                    Tier.LIVE -> ModuleBridge.applyLive()
                    Tier.RENDER -> ModuleBridge.applyRender(restartSf = false)
                    Tier.ART -> ModuleBridge.applyArt(softReboot = false)
                    Tier.MEM -> ModuleBridge.applyMem()
                    Tier.DEXOPT -> ModuleBridge.applyDexopt()
                }
                finish(report, label, tier)
            }
        }
    }

    /** [full] is worth the extra subprocess spawns only when the change could have touched the
     *  Samsung/MARs tables (see [mergeSamsung] for the non-full case). */
    private suspend fun finish(report: ApplyReport?, label: String, tier: Tier = Tier.LIVE, full: Boolean = false) {
        val device = ModuleBridge.status(full)
        val msg = when {
            report == null -> "Módulo não encontrado — nada foi aplicado."
            report.isClean -> "$label · ${report.summary()}"
            else -> "$label · ${report.summary()} — toque para ver"
        }
        update {
            it.copy(
                busy = it.busy - tier,
                device = device?.let { fresh -> mergeSamsung(fresh, it.device) } ?: it.device,
                report = report ?: it.report,
                snack = msg,
            )
        }
    }

    // ---------------- live tier ----------------

    fun resumeAutomatic() = edit(Tier.LIVE, "Aprendizado automático ativo") {
        it.copy(profile = Profile.BALANCED, adaptiveMode = "active", adaptiveLearning = true)
    }
    fun setAdaptiveMode(mode: String) = edit(Tier.LIVE, "Controle adaptativo") {
        it.copy(adaptiveMode = mode.takeIf { m -> m in setOf("active", "observe", "off") } ?: "active")
    }
    fun setAdaptiveLearning(on: Boolean) = edit(Tier.LIVE, "Aprendizado local") { it.copy(adaptiveLearning = on) }
    fun setAdaptivePelt(on: Boolean) = edit(Tier.LIVE, "Ajuste PELT ${onOff(on)}") { it.copy(adaptivePelt = on) }
    fun setAdaptiveTarget(fps: Int) = edit(Tier.LIVE, "Meta de FPS: ${fps.coerceIn(30, 120)}") { it.copy(adaptiveTargetFps = fps.coerceIn(30, 120)) }

    fun selectThermal(mode: ThermalMode) {
        if (mode == cfg().thermal) return
        if (mode == ThermalMode.AGGRESSIVE && !acked("aggressive")) {
            update { it.copy(dialog = Dialog.AGGRESSIVE) }
            return
        }
        edit(Tier.LIVE, if (mode == ThermalMode.AGGRESSIVE) "Térmico agressivo" else "Térmico moderado") {
            it.copy(thermal = mode)
        }
    }

    fun toggleThermalGuard(on: Boolean) =
        edit(Tier.LIVE, "Guardião térmico ${onOff(on)}") { it.copy(thermalGuard = on) }

    fun setGos(pref: GosPref) = edit(Tier.LIVE, "GOS ${pref.cfg}") { it.copy(gos = pref) }

    /** "Limpar agora": the same one-shot reclaim, run on demand instead of tied to a profile
     *  switch. Pressing the button is itself the deliberate action — no extra confirmation. */
    fun clearRamNow() {
        update { it.copy(busy = it.busy + Tier.LIVE) }
        viewModelScope.launch {
            applyLock.withLock {
                finish(ModuleBridge.applyClearRam(), "RAM limpa", Tier.LIVE)
            }
        }
    }

    fun exportDiagnostics() {
        viewModelScope.launch {
            val path = ModuleBridge.exportDiagnostics()
            update { it.copy(snack = path?.let { p -> "Diagnóstico salvo em $p" } ?: "Falha ao gerar diagnóstico") }
        }
    }

    fun startSchedBench() {
        viewModelScope.launch {
            val started = ModuleBridge.startSchedBench()
            update {
                it.copy(
                    snack = if (started) {
                        "Medição PELT/EAS iniciada — jogue normalmente por cerca de 9 minutos."
                    } else {
                        "Não foi possível iniciar a medição PELT/EAS."
                    },
                )
            }
            if (started) delay(500)
            val d = ModuleBridge.status(full = false)
            if (d != null) update { it.copy(device = mergeSamsung(d, it.device)) }
        }
    }

    fun stopSchedBench() {
        viewModelScope.launch {
            val stopped = ModuleBridge.stopSchedBench()
            val d = ModuleBridge.status(full = false)
            update {
                it.copy(
                    device = d?.let { fresh -> mergeSamsung(fresh, it.device) } ?: it.device,
                    snack = if (stopped) "Medição interrompida; valores restaurados."
                    else "Não foi possível interromper a medição.",
                )
            }
        }
    }

    // GPU

    // ---------------- render tier ----------------

    fun setHwuiRenderer(v: String) =
        edit(Tier.RENDER, "Renderer HWUI $v") { it.copy(hwuiRenderer = v) }


    fun setReBackend(v: String) {
        if (v != "default" && !acked("re_backend")) {
            update { it.copy(dialog = Dialog.RE_BACKEND, pendingReBackend = v) }
            return
        }
        edit(Tier.RENDER, "RenderEngine $v") { it.copy(reBackend = v) }
    }

    // ---------------- ART / zygote tier ----------------
    // These only become real on the next zygote start, so each change just marks the soft reboot as
    // pending; the banner asks once and the user decides when to pay for it.

    fun setArtUsap(state: String) = edit(Tier.ART, "USAP $state") { it.copy(artUsap = state) }
    fun setArtDex2oat(state: String) = edit(Tier.ART, "dex2oat no LITTLE $state") { it.copy(artDex2oatLittle = state) }
    fun setArtHeap(state: String) = edit(Tier.ART, "Heap $state") { it.copy(artHeap = state) }

    fun askSoftReboot() = update { it.copy(dialog = Dialog.SOFT_REBOOT) }

    /** The only soft reboot outside boot, and only after an explicit confirm. */
    fun confirmSoftReboot() {
        update { it.copy(dialog = Dialog.NONE, busy = it.busy + Tier.ART) }
        viewModelScope.launch {
            applyLock.withLock {
                if (!persistOrRevert(Tier.ART, "Reinício leve")) return@withLock
                val r = ModuleBridge.applyArt(softReboot = true)
                finish(r, "Reinício leve disparado", Tier.ART)
            }
        }
    }
    fun toggleRestartSystemUi(on: Boolean) =
        edit(Tier.RENDER, "Reiniciar SystemUI ${onOff(on)}") { it.copy(restartSystemUi = on) }

    /** The only place a SurfaceFlinger restart happens outside boot, and only after a confirm. */
    fun confirmSfRestart() {
        update { it.copy(dialog = Dialog.NONE, busy = it.busy + Tier.RENDER) }
        viewModelScope.launch {
            applyLock.withLock {
                val r = ModuleBridge.applyRender(restartSf = true)
                finish(r, "SurfaceFlinger reiniciado", Tier.RENDER)
            }
        }
    }

    fun askSfRestart() = update { it.copy(dialog = Dialog.SF_RESTART) }

    // ---------------- set-aside brain ----------------

    fun askAdoptBrain(name: String) = update { it.copy(dialog = Dialog.ADOPT_BRAIN, pendingAdopt = name) }

    private fun confirmAdoptBrain() {
        val name = _uiState.value.pendingAdopt ?: return
        update { it.copy(dialog = Dialog.NONE, pendingAdopt = null, busy = it.busy + Tier.LIVE) }
        viewModelScope.launch {
            applyLock.withLock {
                val r = ModuleBridge.adoptBrain(name)
                finish(r, "Memória anterior reutilizada", Tier.LIVE)
            }
        }
    }

    // ---------------- memory / dexopt tiers ----------------

    fun setZramAlgo(algo: String) {
        if (algo == cfg().zramAlgo) return
        update { it.copy(dialog = Dialog.ZRAM, pendingZram = algo) }
    }

    fun confirmZram() {
        val algo = _uiState.value.pendingZram ?: return
        update { it.copy(dialog = Dialog.NONE, pendingZram = null) }
        edit(Tier.MEM, "zram $algo") { it.copy(zramAlgo = algo) }
    }

    /** The watcher is a process, not a value: flip the config AND start or stop it. */
    fun toggleProtect(on: Boolean) {
        update { it.copy(config = cfg().copy(protectGames = on), busy = it.busy + Tier.LIVE) }
        viewModelScope.launch {
            applyLock.withLock {
                if (!persistOrRevert(Tier.LIVE, "Proteção")) return@withLock
                val processOk = if (on) ModuleBridge.startKeepalive() else ModuleBridge.stopKeepalive()
                val device = ModuleBridge.status(full = true)
                update {
                    it.copy(
                        busy = it.busy - Tier.LIVE,
                        device = device ?: it.device,
                        snack = if (on) {
                            if (processOk && device?.keepalive == true) "Proteção ligada — vigia rodando"
                            else "Proteção pedida, mas o vigia não subiu"
                        } else if (processOk && device?.keepalive != true) "Proteção desligada"
                        else "Falha ao parar a proteção — o vigia pode continuar ativo",
                    )
                }
            }
        }
    }

    private fun applyProtectTier(label: String, contextConfirmed: Boolean = false, transform: (TunerConfig) -> TunerConfig) {
        if (transform(cfg()) == cfg()) return
        if (!contextConfirmed && confirmContextChange(transform(cfg()), label) {
                applyProtectTier(label, contextConfirmed = true, transform = transform)
            }) return
        update { it.copy(config = transform(cfg()), busy = it.busy + Tier.LIVE) }
        viewModelScope.launch {
            applyLock.withLock {
                if (!persistOrRevert(Tier.LIVE, label)) return@withLock
                finish(ModuleBridge.applyProtect(), label, full = true)
            }
        }
    }

    fun toggleSamsungProtect(on: Boolean) =
        applyProtectTier(if (on) "Jogos isentos da restrição" else "Isenção removida") {
            it.copy(samsungProtect = on)
        }

    fun toggleSpcm(on: Boolean) =
        applyProtectTier(if (on) "SPCM desligado" else "SPCM de volta") { it.copy(samsungSpcm = on) }

    fun toggleMarsOff(on: Boolean) =
        applyProtectTier(if (on) "Políticas MARs desligadas" else "Políticas MARs de volta") {
            it.copy(samsungMarsOff = on)
        }

    fun toggleSamsung(on: Boolean) = applySamsung(on)

    private fun applySamsung(on: Boolean, contextConfirmed: Boolean = false) {
        if (on == cfg().samsungPerf) return
        if (!contextConfirmed && confirmContextChange(cfg().copy(samsungPerf = on),
                if (on) "Limitadores da Samsung desligados" else "Valores da Samsung restaurados") {
                applySamsung(on, contextConfirmed = true)
            }) return
        update { it.copy(config = cfg().copy(samsungPerf = on), busy = it.busy + Tier.LIVE) }
        viewModelScope.launch {
            applyLock.withLock {
                val label = if (on) "Limitadores da Samsung desligados" else "Valores da Samsung restaurados"
                if (!persistOrRevert(Tier.LIVE, label)) return@withLock
                val report = ModuleBridge.applySamsung()
                finish(report, label, full = true)
            }
        }
    }

    fun askDexopt() = update { it.copy(dialog = Dialog.DEXOPT) }
    fun askDexoptReset() = update { it.copy(dialog = Dialog.DEXOPT_RESET) }

    fun runDexopt(reset: Boolean) {
        update { it.copy(dialog = Dialog.NONE, busy = it.busy + Tier.DEXOPT) }
        viewModelScope.launch {
            applyLock.withLock {
                if (!persistOrRevert(Tier.DEXOPT, "Compilação")) return@withLock
                val r = ModuleBridge.applyDexopt(reset)
                finish(r, if (reset) "Compilação revertida" else "Jogos compilados (${cfg().dexoptMode})", Tier.DEXOPT)
            }
        }
    }

    // ---------------- pickers ----------------

    fun openPicker(which: Picker) {
        update { it.copy(picker = which, appsLoading = _uiState.value.apps.isEmpty()) }
        viewModelScope.launch {
            val apps = AppCatalog.load(getApplication<Application>())
            update { it.copy(apps = apps, appsLoading = false) }
        }
    }

    fun closePicker() = update { it.copy(picker = Picker.NONE) }

    fun setGames(list: List<String>) {
        update { it.copy(picker = Picker.NONE) }
        // Only the list is stored here; compiling is a separate, explicit action.
        viewModelScope.launch {
            val next = cfg().copy(games = list)
            update { it.copy(config = next) }
            if (!ModuleBridge.writeConfig(next)) {
                val disk = ModuleBridge.readConfig()?.also { persistedConfig = it; configLoaded = true }
                update { it.copy(config = disk ?: persistedConfig.takeIf { configLoaded } ?: it.config, snack = "Falha ao gravar a lista — nada foi salvo") }
                return@launch
            }
            persistedConfig = next
            configLoaded = true
            update { it.copy(snack = "${list.size} jogo(s) na lista — toque em Compilar para aplicar") }
        }
    }

    fun setRenderApps(list: List<String>) {
        update { it.copy(picker = Picker.NONE) }
        edit(Tier.RENDER, "Apps do render: ${list.size}") { it.copy(renderApps = list) }
    }

    /** Only the list is stored here, same as [setGames] — it takes effect the next time the
     *  "Isentar jogos" switch (or the keepalive watcher, which re-reads every second) runs. */
    fun setProtectList(list: List<String>) {
        update { it.copy(picker = Picker.NONE) }
        viewModelScope.launch {
            val next = cfg().copy(protectList = list)
            update { it.copy(config = next) }
            if (!ModuleBridge.writeConfig(next)) {
                val disk = ModuleBridge.readConfig()?.also { persistedConfig = it; configLoaded = true }
                update { it.copy(config = disk ?: persistedConfig.takeIf { configLoaded } ?: it.config, snack = "Falha ao gravar a lista — nada foi salvo") }
                return@launch
            }
            persistedConfig = next
            configLoaded = true
            update { it.copy(snack = "${list.size} app(s) na lista de proteção") }
        }
    }

    // ---------------- dialogs ----------------

    fun confirmDialog() {
        when (_uiState.value.dialog) {
            Dialog.AGGRESSIVE -> {
                ack("aggressive"); close()
                edit(Tier.LIVE, "Térmico agressivo", contextConfirmed = true) { it.copy(thermal = ThermalMode.AGGRESSIVE) }
            }
            Dialog.LEARNING_CONTEXT -> {
                val action = pendingContextAction
                dismissDialog()
                action?.invoke()
            }
            Dialog.RE_BACKEND -> {
                ack("re_backend")
                val v = _uiState.value.pendingReBackend ?: "default"
                update { it.copy(dialog = Dialog.NONE, pendingReBackend = null) }
                setReBackend(v)
            }
            Dialog.SF_RESTART -> confirmSfRestart()
            Dialog.ADOPT_BRAIN -> confirmAdoptBrain()
            Dialog.SOFT_REBOOT -> confirmSoftReboot()
            Dialog.ZRAM -> confirmZram()
            Dialog.DEXOPT -> runDexopt(false)
            Dialog.DEXOPT_RESET -> runDexopt(true)
            Dialog.NONE -> {}
        }
    }

    fun dismissDialog() {
        pendingContextAction = null
        update { it.copy(dialog = Dialog.NONE, pendingZram = null, pendingReBackend = null, pendingContextLabel = null, pendingAdopt = null) }
    }
    private fun close() = update { it.copy(dialog = Dialog.NONE) }

    private fun acked(k: String) = prefs.getBoolean("ack_$k", false)
    private fun ack(k: String) = prefs.edit().putBoolean("ack_$k", true).apply()

    // ---------------- report / log ----------------

    fun openReport() = update { it.copy(reportOpen = true) }
    fun closeReport() = update { it.copy(reportOpen = false) }
    fun consumeSnack() = update { it.copy(snack = null) }

    fun openLog() {
        viewModelScope.launch {
            update { it.copy(log = emptyList()) }
            val lines = ModuleBridge.readLog()
            update { it.copy(log = lines) }
        }
    }

    fun closeLog() = update { it.copy(log = null) }

    private fun onOff(b: Boolean) = if (b) "ligado" else "desligado"

    private fun label(p: Profile) = when (p) {
        Profile.GAME -> "Game"
        Profile.BALANCED -> "Balanceado"
        Profile.POWER_SAVE -> "Economia"
        Profile.NONE -> "Nenhum"
    }
}
