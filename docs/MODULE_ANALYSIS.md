# Análise cruzada: PUCTuner × Encore Tweaks 5.2.2 × Stellar v22.0

Objetivo: separar o que **de fato** compra desempenho por joule no Galaxy M54 (Exynos 1380,
kernel 5.15 GKI, One UI) do que é placebo, escrita morta ou troca de bateria por FPS — e entender
por que o PUCTuner perde para heurísticas simples. Isso é a base antes de trazer o FAS-RS.

Fontes analisadas:

- **Encore Tweaks 5.2.2** (Rem01Gaming, Apache-2.0): `encore_profiler` (shell, 978 linhas, lido
  inteiro), `service.sh`, `customize.sh`, strings do daemon `encored` (C++/NDK).
- **Stellar v22.0** (Kanao, "Apache-2.0" declarado, mas **sem código-fonte**): scripts de
  instalação e strings do binário Rust `stellar-daemon` (stripped). Tudo o que é dito sobre o
  Stellar abaixo vem de strings do binário e do changelog — é inferência, não leitura de código.
- **PUCTuner** neste repositório: `module/engine/*`, `module/scripts/apply_profile.sh`,
  `post-fs-data.sh`, docs.

---

## 1. Veredito curto

1. **O problema do PUCTuner não é falta de amostras, é estrutural.** O espaço de ações só
   **sobe pisos** (CPU/GPU/MIF min, PELT). Subir piso nunca gasta *menos* que o governador stock.
   Então, em energia, o melhor resultado possível do PUCTuner é **empatar com o stock**; em FPS, ele
   precisa explorar para descobrir o que o Encore faz por decreto (pinar no máximo). Ele perde nos
   dois eixos por construção, e 40k amostras não mudam isso.
2. **A alavanca que economiza energia de verdade — teto (ceiling) guiado por frame time — está
   desligada em produção.** O único ramo que mexe em teto é o da GPU em `Tier::Powersave`
   (`platform.cpp`, `Actuator::apply`), e `RuntimeObjective` é fixo em `Tier::Balanced`
   (`core.hpp`). É exatamente isso que o FAS-RS faz, e por isso ele funciona.
3. **MCTS/RL é a ferramenta errada para este problema.** Frame time responde a frequência de forma
   quase monótona e com feedback em dezenas de milissegundos. Isso é um problema de controle em
   malha fechada (P/PI por cluster), que converge em segundos. O PUCTuner usa janelas de 4,5–12 s,
   ~625 estados de ação por contexto, recompensa ruidosa e decaimento de evidência — está tentando
   aprender por exploração o que um controlador obtém por realimentação direta.
4. **O Encore "ganha" porque reage na hora e gasta sem dó.** Ele detecta o jogo por binder
   (`IActivityManager.registerProcessObserver` → `onForegroundActivitiesChanged`, latência de ms)
   e aplica `performance` + `min=max` no CPU, GPU e MIF. Isso dá FPS estável por alguns minutos e
   depois throttling térmico. Se a comparação foi por FPS médio/estabilidade em sessões curtas, o
   Encore ganha; em **J/frame** ou sessão de 30+ min, provavelmente não. **Você precisa medir
   J/frame antes de concluir qualquer coisa** (seção 6).
5. **Stellar**: quase nada aproveitável como código (não há código). Tem 3 ideias boas
   (intervenções de Game Mode com downscale/FPS, troca de refresh rate, `uclamp.max` em
   background), e várias coisas que eu **não** colocaria no PUCTuner (spoof de device, RT -20 em
   threads de jogo, licença online, `store_mode` em nó de bateria).
6. **Licenças**: FAS-RS é **GPL-3.0** — mesmo do PUCTuner, pode ser incorporado/forkado mantendo
   avisos de copyright. Encore é **Apache-2.0** — pode entrar num projeto GPLv3 (compatibilidade é
   unidirecional), mantendo `NOTICE`. Stellar: nada a copiar.

---

## 2. Por que o PUCTuner perde (diagnóstico com referência ao código)

### 2.1 Espaço de ações sem lado de economia

`module/engine/platform.cpp`, `Actuator::apply`:

```cpp
// Raise only floors, bounded by the CURRENT cooling/QoS cap. Never pin min==max.
desired = level == 0 ? std::min(n.original, cap) : std::max(n.original, table[index]);
```

Nível 0 = stock. Qualquer nível > 0 = mais energia que stock (ou igual, se o governador já estava
acima). Não existe ação que diga "o big cluster não precisa passar de 1,8 GHz para manter 60 fps
neste jogo". No Exynos 1380 os OPPs do topo dos A78 (2,2–2,4 GHz) e da GPU (848–949 MHz) estão na
parte mais íngreme da curva V/f — é onde o joule por instrução é pior. **Cortar o topo quando há
folga de frame é a maior economia disponível no SoC, e o PUCTuner não tem essa ação.**

### 2.2 Latência de percepção e de atuação

| | Encore | PUCTuner |
|---|---|---|
| Detecção de app em foreground | Binder `IProcessObserver` (evento, ms) | `dumpsys activity activities` a cada refresh de contexto (6 s) |
| Tela ligada | Binder `IPowerManager.isInteractive` | `dumpsys power` a cada 6 s |
| Frames | — | `dumpsys SurfaceFlinger --latency <layer>` a cada 1 s |
| Decisão | Imediata | Ao fechar janela: 4,5–12 s |

Consequências:

- Abrir um jogo: o Encore já está em perfil de jogo antes do primeiro frame; o PUCTuner leva no
  mínimo uma janela para sequer ter dados, e depois o *toll* de aceitação para subir.
- Cada `dumpsys` é um `fork/exec` + transação binder com o SurfaceFlinger/AMS. Três processos por
  segundo, o tempo todo com tela ligada, acordando núcleos — custo de energia do próprio
  medidor, que entra na conta do PUCTuner e não na do Encore.
- `--latency` devolve um buffer circular de ~127 frames. A 120 Hz isso é ~1,06 s: polling a 1 Hz
  fica no limite, qualquer atraso do poll perde frames.

### 2.3 Sinal de recompensa fraco

- **Energia**: `current_now`/`voltage_now` do fuel gauge Samsung (`Sampler::read`). É uma média
  do gauge com atraso de segundos e ruído alto; a diferença de potência entre dois níveis
  adjacentes de piso numa janela de 6 s fica abaixo do ruído. Na prática a parte de energia da
  recompensa é quase sempre o *proxy* (`.65·cpu·clock² + .35·gpu·clock²`), que não é medida.
- **Desempenho**: p95 do *intervalo de apresentação*. Isso mede cadência, não folga. Um jogo
  travado em 60 fps com o render thread a 15,9 ms e outro a 8 ms dão o mesmo p95; só o segundo tem
  margem para cortar frequência. O FAS trabalha com **frame time contra o alvo**, que é exatamente
  essa folga.

### 2.4 Eficiência amostral

`Levels = 5`, `Axes = 4` → 625 combinações por contexto (app × carga × térmico × máscara). Com uma
amostra a cada ~6 s, 40k amostras ≈ 67 h de uso, espalhadas por todos os apps e contextos, com
meia-vida de evidência e resets por "surpresa". Por célula, isso é pouco. E o problema real é quase
unidimensional por cluster ("frame atrasado → sobe; folga → desce"), então a busca é cara para
uma resposta que um controlador obtém direto.

### 2.5 O que vale manter do PUCTuner (é bom e o FAS-RS não tem)

- Snapshot de fábrica com guardas contra captura "envenenada" (`post-fs-data.sh`).
- Escrita com read-back e journal de recuperação (`Actuator::writeOwned`, `adaptive_journal`).
- Detecção de tuner concorrente (`processConflict`) e nós travados (`lockedTuningNodes`).
- Guarda térmica fora de processo (`thermal_guard.sh`).
- Infra de eBPF já funcionando no kernel do M54 (`runqueue.bpf.c`) — é a base para trocar o
  `dumpsys --latency` por uprobe de frame.
- Ferramentas A/B (`tools/ceiling_ab.sh`, `ab_report.py`, `measure_ablation.py`).
- Ajustes estáticos medidos: read-ahead 512 KB (medido), f2fs `GC_URGENT_LOW`, `ipu_policy`
  com bit UTIL, swappiness baixo em jogo.

---

## 3. Encore Tweaks 5.2.2 — tweak a tweak, no M54

Legenda: **✅ adotar** · **🧪 medir antes** · **⚪ inócuo/morto no M54** · **❌ rejeitar**

### 3.1 Arquitetura

| Item | Veredito | Por quê |
|---|---|---|
| Daemon orientado a eventos via binder (`registerProcessObserver`, `isInteractive`, `isPowerSaveMode`) | ✅ | Zero polling, latência de ms. Substitui os três `dumpsys` do PUCTuner. É a melhor parte do Encore. |
| Gamelist explícita (`gamelist.json`) | ✅ | Necessária para o FAS também (alvo de FPS por jogo). |
| `device_mitigation.json` (desliga tweaks por device) | ✅ (ideia) | Bom padrão para quirks por firmware. |
| Restaurar governador "preferido" se o default for `performance` | ✅ | Defensivo, barato. |

### 3.2 `perfcommon` (aplicado uma vez)

| Tweak | Veredito no M54 | Motivo |
|---|---|---|
| `kernel.panic=0`, `panic_on_oops=0`, `panic_on_warn=0` | ❌ | O próprio autor diz que está errado. Esconde crashes de kernel e deixa o sistema rodando em estado corrompido. |
| `sched_min_granularity_ns`, `sched_wakeup_granularity_ns`, `sched_migration_cost_ns`, `sched_nr_migrate` em `/proc/sys/kernel` | ⚪ | Desde o 5.13 esses saíram do procfs para debugfs. No 5.15 GKI a escrita falha — e o `apply` do Encore engole o erro. Confirme: `ls /proc/sys/kernel/sched_*granularity*`. |
| `sched_child_runs_first=1` | ⚪ | Sem efeito mensurável em jogo. |
| `sched_autogroup_enabled=0` | ⚪ | Android não usa autogroup (cgroups próprios). |
| `sched_lib_name` / `sched_lib_mask_force` | ⚪ | Nó de kernel QCOM/MTK/Oplus. Provavelmente inexistente no Exynos GKI. Confirme com `ls`. |
| Desligar `schedstats`, `iostats`, `add_random`, `perf_cpu_time_max_percent=3` | ⚪ | Micro. O PUCTuner já faz `add_random=0`. |
| `stat_interval=15` | ⚪ | Reduz wakeups do vmstat; ganho real é desprezível, e o PUCTuner usa `/proc/vmstat` como telemetria. |
| `page-cluster=0` | 🧪 | Correto para zram (sem readahead de swap). Barato; vale medir com o `paging_report.py`. |
| `compaction_proactiveness=0` | 🧪 | Reduz compaction em background (menos CPU), mas pode piorar alocações high-order depois. Medir. |
| TCP: bbr, `tcp_low_latency`, `tcp_ecn`, `tcp_fastopen`, `tcp_timestamps=0` | ⚪/❌ | Não afetam frame nem energia de forma mensurável. `tcp_timestamps=0` pode piorar RTT/PAWS. Ignorar. |
| Política térmica `step_wise` em **todas** as zonas | ❌ | No Exynos a zona do big cluster e da GPU normalmente usam `power_allocator` (IPA), que distribui orçamento de potência entre CPU e GPU. Trocar por `step_wise` desmonta isso: throttling mais brusco, pior desempenho sustentado. Confirme o atual com `cat /sys/class/thermal/thermal_zone*/policy`. |
| Desabilitar opchain/oplus/touchpanel | ⚪ | Não existem no Samsung. |

### 3.3 Perfil `performance` (jogo em primeiro plano)

| Tweak | Veredito | Motivo |
|---|---|---|
| Governador `performance` + `scaling_min=max` em todos os clusters | ❌ | FPS máximo por poucos minutos, pior J/frame possível, throttling térmico no Exynos 1380 em minutos. É o que faz o Encore parecer bom em teste curto. |
| **Lite mode**: governador default, `min` = OPP do meio, GPU sem piso, MIF piso no meio | 🧪 | É a heurística razoável do Encore e provavelmente o "modelo genérico" que bate o PUCTuner. Serve de **baseline obrigatório** nos testes (seção 6). |
| GPU `gpu_min_clock = max` | ❌ | Mesmo problema do CPU. |
| Mali `power_policy = always_on` | ❌ | Mantém os shader cores sempre energizados: corta alguns µs de power-up por job e paga leakage contínuo. Perf/W ruim. O PUCTuner já usa isso no preset Game legado (`apply_profile.sh:115`) — eu removeria. |
| MIF `min=max` (DRAM no máximo) | ❌ | DRAM/MIF é fatia grande da potência do SoC em jogo. Piso de MIF só quando o frame atrasa **e** a carga é memory-bound. |
| `read_ahead_kb=32`, `nr_requests=32` | ❌ | O PUCTuner mediu 484 MB/s a 128 KB e 522 MB/s a 512 KB neste UFS. 32 KB piora carregamento de assets. |
| `vfs_cache_pressure=80` | ⚪ | Efeito marginal. |
| `drop_caches=3` ao entrar em jogo | ❌ | Joga fora o page cache — inclusive os assets do jogo que acabou de abrir — e força relê-los do flash. Custa I/O e energia para nada. |
| `split_lock_mitigate=0` | ⚪ | x86-only. |
| `sched_features` NEXT_BUDDY / NO_TTWU_QUEUE (debugfs) | ⚪ | debugfs costuma estar indisponível em build user GKI. |
| `/dev/stune` (schedtune) | ⚪ | Não existe no 5.15 (substituído por uclamp). |

### 3.4 Perfil `powersave`

| Tweak | Veredito | Motivo |
|---|---|---|
| GPU `min=max=` menor OPP | ❌ | Trava a GPU em 221 MHz: qualquer animação engasga, e a tela pode ficar mais tempo acordada para terminar o trabalho (anti race-to-idle). |
| Mali `coarse_demand` | ⚪ | Já é o default típico. |

**Resumo Encore**: o valor está na **arquitetura** (eventos por binder, gamelist, mitigação por
device), não nos tweaks. Dos tweaks, quase todos são mortos no M54 ou trocam bateria por FPS.

---

## 4. Stellar v22.0 — o que dá para inferir do binário

O daemon é Rust, fechado, com cliente TLS (`rustls`), módulo `license/scraper.rs`, consulta
`AccountManager` via binder para "license verification" e um `.dex` injetado via `app_process`
(`sys_binderservice`). **Faz rede e verifica licença.** Isso por si só já é motivo para não
coexistir com o PUCTuner num aparelho de medição.

| Recurso (string no binário / changelog) | Veredito | Motivo |
|---|---|---|
| **Intervenções de Game Mode**: `mode=2,downscaleFactor=…,fps=…` | ✅ | É a API oficial `GameManager` (`cmd game set --mode … --downscale … --fps … <pkg>`, Android 13+). **Downscale de resolução é a alavanca de maior ganho perf/W que existe**: reduz trabalho de fill da GPU e banda de MIF proporcionalmente. Cap de FPS idem. Precisa testar no One UI: o jogo pode ter optado por não aceitar intervenções. |
| **Troca de refresh rate** (`reduced_rr`, `force_change_rr`, "60Hz power-saving") | ✅ | Em AMOLED, 120 Hz vs 60 Hz muda custo de painel/DDIC e dobra composição do SurfaceFlinger. Para um jogo que roda a 60, manter o painel em 120 é desperdício puro. No Samsung os controles são por `settings` (ex.: `refresh_rate_mode`, `peak_refresh_rate`/`min_refresh_rate`) — confirmar quais o One UI do M54 respeita. |
| **`uclamp.max` em `background` = 50 %** (modo Battery) | ✅ | Impede que tarefas de fundo puxem os clusters para OPPs altos. Economia real com risco baixo, desde que `top-app` não seja limitado. Confirme que `/dev/cpuctl/background/cpu.uclamp.max` existe e é gravável. |
| `uclamp.min` em `top-app` = 30 % (modo GameActive) | 🧪 | Equivalente a um piso de frequência para o app em foreground. Mais cirúrgico que `scaling_min_freq` (só afeta tarefas do top-app), mas ainda é "gastar mais". Vira um eixo de ação candidato para o controlador. |
| `sched_upmigrate` / `sched_downmigrate` | ⚪ | Nós WALT (QCOM). Não existem no Exynos com EAS/PELT. |
| Props ADPF: `debug.hwui.use_hint_manager`, `debug.sf.enable_adpf_cpu_hint`, `debug.hwui.target_cpu_time_percent` | 🧪 | Fazem HWUI/SF enviarem *PerformanceHint* ao PowerHAL. Só ajuda se o PowerHAL Samsung implementar ADPF para valer. Em versões recentes `use_hint_manager` já é default. Ler o valor atual antes de achar que muda algo. |
| Detector "Lobby vs Match Combat" por queda de FPS (chamam de "FEAS") | ⚪ | Heurística: queda de FPS → sobe pisos. Não é FEAS de verdade; é uma versão grosseira do que um controlador de frame faz. |
| Threads de jogo: "Prime/Leader" com nice -20 / RT, workers -5, lobby nice 10 | ❌ | RT em thread de jogo pode deixar SurfaceFlinger, áudio e input sem CPU. O EAS já coloca o render thread no big. Ganho duvidoso, risco alto. |
| Spoof de device via `resetprop ro.product.*` (desbloquear FPS em jogos) | ❌ | Faz o jogo **gastar mais** (preset gráfico/FPS acima do que o hardware sustenta), risco de ban, e o próprio changelog registra "perda de áudio causada pelo spoof". |
| Bypass charging / nós de bateria (`store_mode`, `siop_level`, `test_mode`, …) | ❌ (no módulo) | Jogar na tomada sem carregar reduz calor da bateria — a ideia é boa, mas `store_mode` no Samsung limita a carga e muda o comportamento do carregador. Use o "Pausar fornecimento de energia USB" oficial do Game Booster, se o M54 suportar. |
| Matar `logd`/`traced`/`statsd` (`kill_logger`) | ❌ | Economia ínfima, quebra diagnóstico e relatórios de crash. |
| Restringir GMS em background (`RUN_ANY_IN_BACKGROUND`) | 🧪 (opt-in) | Ganho real em standby, mas atrasa notificações. Não é desempenho; fica fora do escopo do controlador. |
| Fast-charge (`force_fast_charge`, nós QCOM) | ⚪ | Não se aplica ao Samsung. |

---

## 5. Ranking das alavancas por desempenho/joule no Exynos 1380

Ordenado pelo ganho esperado em **J/frame com o mesmo FPS entregue**. "Esperado" = mecanismo
conhecido; nenhum número aqui é medição no M54 — a seção 6 diz como medir.

### Nível A — economizam energia sem perder frame (o núcleo do novo controlador)

1. **Teto de frequência por cluster guiado por frame time** (o que o FAS-RS faz). Corta o topo da
   curva V/f quando há folga. Maior ganho em jogos que já batem o alvo de FPS.
2. **FPS alvo explícito por jogo** + cap de FPS via Game Mode quando o jogo não oferece. Não
   renderizar frames que o usuário não pediu.
3. **Downscale via Game Mode** (opt-in por jogo). Reduz trabalho de GPU e MIF.
4. **Refresh rate casado com o FPS alvo** (60 Hz para jogo de 60).
5. **`uclamp.max` em background** durante jogo.

### Nível B — gastam um pouco para cobrir o pior caso (usar só sob demanda)

6. **Rampa da GPU** (`highspeed_load`/`highspeed_clock`/`down_staycount`, `dvfs_period`): ataca
   a rampa lenta sem fixar piso. O PUCTuner já tem os nós mapeados.
7. **Piso de MIF** só quando o frame atrasa e a GPU não está saturada (sinal de memory-bound).
8. **Boost curto de lançamento/troca de cena** (1–3 s), disparado por evento binder.
9. **`uclamp.min` no top-app** como alternativa mais cirúrgica a `scaling_min_freq`.
10. **PELT 2x** — já é baseline no PUCTuner; precisa de A/B real de energia, nunca foi medido.

### Nível C — morto, placebo ou negativo no M54 (não implementar)

`performance` + `min=max`, `always_on` na Mali, MIF no máximo, `drop_caches`, `step_wise` em todas
as zonas, `kernel.panic=0`, granularidades do CFS via procfs, stune, `sched_lib_*`,
`sched_up/downmigrate`, TCP, spoof de device, RT em threads de jogo, matar loggers, `store_mode`.

---

## 6. Protocolo de medição (pré-requisito para qualquer conclusão)

Sem isso, "o PUCTuner perde" pode significar só "o outro gastou mais". Proposta:

- **Métrica principal: J/frame entregue**, em sessões de 20–30 min, mesmo jogo, mesma cena,
  mesma temperatura ambiente, brilho fixo, rádios fixos.
  - Energia: diferença de `POWER_SUPPLY_CHARGE_COUNTER` (µAh) × tensão média — muito mais preciso
    que integrar `current_now`. Confirme que o nó existe em
    `/sys/class/power_supply/battery/uevent`.
  - Frames: contagem de frames apresentados + p50/p95/p99 do frame time + % de frames acima do
    orçamento (jank).
- **Métricas secundárias**: FPS nos minutos 25–30 (desempenho sustentado), temperatura máxima e
  de bateria, tempo até o primeiro throttle.
- **Braços mínimos**: stock; Encore lite (reproduzido pelos nós, sem instalar o módulo); Encore
  full; PUCTuner atual; e depois cada alavanca do nível A isolada.
- **Desenho ABBA** (como em `tools/ceiling_ab.sh`) para cancelar deriva térmica.
- **Nunca dois tuners instalados ao mesmo tempo.** Encore e Stellar escrevem os mesmos nós que o
  PUCTuner e o snapshot de fábrica já foi envenenado uma vez por isso
  (`post-fs-data.sh`, comentário sobre `cpu_policy4_max=2208000`).

---

## 7. Implicações para a integração com o FAS-RS

O que esta análise diz sobre o desenho, antes de abrir o código do FAS-RS:

1. **O núcleo de decisão vira um controlador de frame (FAS)**, não MCTS. O MCTS/critic, se
   sobreviver, fica como camada lenta de ajuste de parâmetros (margem, ganhos), nunca no loop de
   frame.
2. **Fonte de frame**: trocar `dumpsys SurfaceFlinger --latency` a 1 Hz por captura por evento.
   O PUCTuner já carrega eBPF neste kernel; um uprobe no caminho de `queueBuffer` do libgui é o
   caminho natural (confirmar como o FAS-RS faz hoje antes de decidir).
3. **Detecção de foreground e tela por binder**, no modelo do Encore.
4. **Atuação em teto (`scaling_max_freq`, `gpu_max_clock`)** como eixo principal; pisos só no
   nível B, sob demanda.
5. **Manter a infraestrutura de segurança do PUCTuner**: snapshot de fábrica, read-back,
   journal, guarda térmica, detecção de conflito. O FAS-RS não tem equivalentes desse nível para o
   Exynos.
6. **Módulos do nível A além do FAS**: Game Mode (FPS/downscale), refresh rate e `uclamp.max` de
   background entram como atuadores extras do mesmo controlador.

### Comandos para confirmar no aparelho as suposições marcadas acima

```sh
su -c '
ls /proc/sys/kernel/ | grep -E "sched_(min_gran|wakeup_gran|migration_cost|nr_migrate|lib_name|upmigrate)"
for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type) $(cat $z/policy)"; done
ls -l /dev/cpuctl/top-app/cpu.uclamp.* /dev/cpuctl/background/cpu.uclamp.*
grep -E "CHARGE_COUNTER|CURRENT_NOW|VOLTAGE_NOW" /sys/class/power_supply/battery/uevent
getprop debug.hwui.use_hint_manager; getprop debug.sf.enable_adpf_cpu_hint
settings list system | grep -iE "refresh|peak|min_refresh"
settings list secure | grep -iE "refresh"
cmd game help | head -40
cat /sys/class/misc/mali0/device/power_policy
'
```
