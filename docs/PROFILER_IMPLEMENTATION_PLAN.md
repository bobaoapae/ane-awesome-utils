# Plano de implementação — Profiler on-demand Scout-compatível

Este documento é a **fonte de verdade** do trabalho. Atualizar conforme avançar. Tasks granulares vivem no TaskList da sessão; este arquivo fica com o desenho estável e as decisões.

## 1. Objetivo

Permitir que um app AIR em release gere, sob demanda, um arquivo compactado contendo um stream de telemetria compatível com Adobe Scout original. Usuário aperta botão → captura por N segundos → arquivo `.flmc` pronto pra envio via WhatsApp (≤ 1 GB).

**Requisitos:**

1. **Zero overhead idle** (modo B). Telemetria não inicializa no boot do runtime. Só começa quando o AS3 chama `StartProfilingCapture`.
2. **Compatível com Adobe Scout original**. Stream dentro do arquivo = bytes literais que o runtime enviaria ao Scout via TCP 7934. Ferramenta auxiliar `scout-replay` reproduz o stream para Scout ou `flash-profiler` desktop.
3. **Sem OOM**. Pipeline: hook → ring lock-free SPSC → writer thread → compressor streaming → disco. Cap duro de 900 MB + monitoramento de disco.
4. **Cross-platform preservado por design**. Core C++ compartilhado; shims por plataforma isolados em 2 arquivos por plataforma.
5. **Alvo único**: AIR SDK 51.1.3.10, Windows x64 (Fase 1). Android depois (Fase 2). Outras plataformas/versões ficam fora do escopo.

## 2. Premissas travadas

- AIR SDK: `C:\AIRSDKs\AIRSDK_51.1.3.10` (base), `C:\AIRSDKs\AIRSDK_51.1.3.12` (referência Ghidra já analisada)
- Projeto ANE: `C:\Users\Joao\IdeaProjects\ane-awesome-utils`
- Projeto app de teste: `C:\Users\Joao\IdeaProjects\ddtank-client`
- Build pipeline: `C:\Users\Joao\IdeaProjects\ddtank-client\tools\AirBuildTool` (dotnet)
- Debugger automatizado: `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`
- Ghidra: `C:\tools\ghidra_12.0.3_PUBLIC`
- Formato Scout-compat validado estaticamente (ver `docs/profiler-rva-51-1-3-10.md` §Init sequence)
- Compressor: zlib level 6 (vem built-in via zconf.h; leve, universal). Upgrade para zstd só se medição mostrar necessidade.
- **Não tocar em versões que não seja 51.1.3.10.** Quando outra versão for precisa, fazemos nova análise.

## 3. Arquitetura em camadas

```
AS3 (Profiler.start/stop/status/marker)
    ↓  FREFunction dispatch (ProfilerAneBindings.cpp por plat)
shared/profiler/  (C++17 puro, zero #ifdef platform)
  ├── CaptureController     — máquina de estados + API pública
  ├── SpscRing<ChunkDesc>   — ring buffer lock-free
  ├── ArenaBuffer           — arena circular 8MB para bytes brutos
  ├── WriterThread          — drena ring + comprime + escreve
  ├── CompressedSink        — zlib streaming
  ├── FileFormat            — header/footer .flmc
  ├── IRuntimeHook          — interface
  └── IDiskMonitor          — interface
                      ↓
WindowsNative/src/profiler/
  ├── AirTelemetryRvas.h           — constantes do 51.1.3.10 x64
  ├── WindowsRuntimeHook.cpp       — vtable overwrite + init gate
  ├── WindowsDiskMonitor.cpp       — GetDiskFreeSpaceExW
  └── ProfilerAneBindings.cpp      — FREFunctions
```

**Regra dura**: `shared/profiler/` nunca inclui `windows.h`/`jni.h`/etc. Código compila limpo num sandbox Linux com gtest.

## 4. Modo B — zero idle overhead

**Problema**: `Player::init_telemetry` (RVA `0x1e96d0`) é chamado incondicionalmente no boot do Player. Se `has_address() == 0` ele retorna cedo sem alocar nada. Se tiver address, aloca `SocketTransport` + `Telemetry` + `PlayerTelemetry`, começa a emitir `.player.version` etc. Cada chamada subsequente do runtime passa por AMF3 encoding + vtable call em `send_bytes`.

**Solução modo B**:

1. **Default**: não mexer no cfg. `.telemetry.cfg` ausente → `has_address()==0` → `init_telemetry` retorna cedo → **runtime nunca toca Telemetry**. Zero overhead garantido pelo próprio runtime.
2. **Trigger**: AS3 chama `Profiler.start(config)`. ANE então:
   - Escreve o stub Telemetry hook em `PlatformSocketWrapper vtable slot 11` (`.rdata + 0x00ecb828 + 0x58`) via `VirtualProtect`.
   - Invoca manualmente `Player::init_telemetry` com um `Player*` obtido do contexto + cfg sintético apontando para nosso sink (endereço qualquer não-vazio serve, o socket nem vai abrir de verdade pois vamos interceptar antes).
3. **Durante captura**: runtime gera AMF3, chama send_bytes → nosso hook copia bytes pro ring → writer thread comprime e escreve.
4. **Stop**: AS3 chama `Profiler.stop()`. ANE precisa reverter o estado — impedir que o runtime continue gerando eventos depois. Opções (em ordem de preferência):
   - **B1**: patchar o slot com uma função no-op que retorna success imediatamente. Runtime continua fazendo AMF3 encode mas bytes vão pro vácuo. **Overhead quase zero mas não zero.**
   - **B2**: chamar `Telemetry::dtor` (RVA `0x485604`) + `PlayerTelemetry::dtor` (RVA `0x48ef84`) + `SocketTransport::dtor` (RVA `0x48f118`) e zerar os slots `player+0x1650/0x1658/0x1660`. Runtime volta ao estado pré-init. **Overhead exatamente zero.** Mais invasivo; testar com cautela.
   - **B3**: esperar na própria próxima chamada de send_bytes, detectar flag `stopped`, e sabotar retorno para o runtime. Não recomendado — estado inconsistente pode crashar.

**Começamos com B1** (simples, seguro). Se o overhead residual incomodar em medição, subimos para B2.

**Detalhe crítico aberto — como obter `Player*`** para invocar `init_telemetry`:
- `init_telemetry` é método de instância. Precisamos do `this` ptr (Player).
- Opção 1: hookar `init_telemetry` no boot com um detour que **intercepta mas não executa** (grava `player_ptr` em variável global, retorna cedo sem inicializar). Depois, quando AS3 chamar start, invocamos a função real usando o ptr salvo.
- Opção 2: ler o ponteiro via símbolo/offset conhecido de `AvmCore` ou do loader do runtime.
- **Opção 1 é mais robusta.** Requer instalar hook de entrada em `init_telemetry` antes do Player bootar (a ANE já roda antes — `AneAwesomeUtilsExtension.initialize()` é o early hook point, mesmo pattern de `AirIMEGuard`).

## 5. RVAs 51.1.3.10 Windows x64 (copiados de `docs/profiler-rva-51-1-3-10.md`)

Ver arquivo completo com confiança e validação. Resumo imediato:

```c
// Base = 0x180000000
// SHA256 = e24a635554dba434d2cd08ab5b76d0453787a947d0f4a2291e8f0cae9459d6cc

#define RVA_PLAYER_INIT_TELEMETRY             0x001e96d0  // HOOK ENTRY — capturar Player*
#define RVA_SOCKET_TRANSPORT_SEND_BYTES       0x00493060  // corpo real
#define RVA_SOCKET_TRANSPORT_SEND_BYTES_THUNK 0x00492f50  // thunk referenciado na vtable
#define RVA_TELEMETRY_DTOR                    0x00485604
#define RVA_PLAYERTELEMETRY_DTOR              0x0048ef84
#define RVA_SOCKET_TRANSPORT_DTOR             0x0048f118
#define RVA_VT_PLATFORM_SOCKET_WRAPPER        0x00ecb828  // +0x58 = slot send_bytes

#define RVA_TELEMETRY_CONFIG_READ_FILE        0x0048c1e8
#define RVA_TELEMETRY_CONFIG_HAS_ADDRESS      0x0048bfc8
#define RVA_TELEMETRY_CONFIG_SET_KV           0x0048a784
```

## 6. Formato `.flmc`

```
offset 0       "FLMC" (4B)
offset 4       version u16 LE  (= 1)
offset 6       header_len u32 LE
offset 10      header JSON UTF-8 (bytes = header_len):
                 { platform, air_version, swf_url, started_utc,
                   compression: "zlib", wire_protocol: "scout-amf3",
                   wire_stream_offset: 10+header_len,
                   markers: [ {t_ms, name}, ... ] }
offset 10+H    zlib-comprimido do stream Scout-idêntico
                 └ descompactando = byte-identical ao que o runtime
                   enviaria em sessão live TCP:7934
últimos 64B    footer: total_bytes_raw u64, total_bytes_compressed u64,
                       dropped_chunks u64, ended_utc u64,
                       crc32 u32, "END!" (4B)
```

**Invariante**: nada de tipos de mensagem custom no wire stream. Markers vão no header JSON.

## 7. Fases de execução (com gates)

Cada fase tem um **gate** — critério objetivo que precisa passar antes de avançar.

### Fase 0 — Preparação
- **0.1** Escrever este plano em disco (✅ feito)
- **0.2** Criar `shared/profiler/include/AirTelemetryRvas.h` com as constantes
- **0.3** Criar task list com todos os passos granularizados
- **Gate**: plano committado, RVAs num header, tasks no TaskList

### Fase 1 — Validação dinâmica do RVA de send_bytes
- **1.1** Escrever script CDB `.txt` que: anexa ao processo AIR, BP em `Adobe AIR.dll+0x493060`, em cada hit dumpa args (`rcx/rdx/r8` + bytes apontados), continua
- **1.2** Escrever `.telemetry.cfg` temporário em `%USERPROFILE%` apontando para 127.0.0.1:7934 + `SamplerEnabled=1`
- **1.3** Subir `flm_replay` em modo "server" ou um netcat em 7934 para aceitar conexão
- **1.4** Build + launch do ddtank-client via AirBuildTool (configuração debug mais rápida) com CDB anexado
- **1.5** Confirmar que BP bate, args fazem sentido, primeiros bytes começam com estrutura AMF3 válida (marker `0x0A` object + classname `.player.*`)
- **Gate**: log CDB mostra ≥10 hits em send_bytes com `data` apontando pra bytes AMF3 reconhecíveis

### Fase 2 — POC standalone (DLL injectável)
- **2.1** Criar projeto CMake `shared/profiler/poc/` compilando uma DLL `profiler_poc.dll` 64-bit
- **2.2** A DLL ao ser carregada: acha `Adobe AIR.dll` base, calcula endereço do vtable slot, `VirtualProtect` RW, escreve ponteiro da nossa função, restaura proteção
- **2.3** Nossa função: ler args → `memcpy` bytes num arquivo `C:\temp\raw_capture.bin` → chamar original (para Scout real também receber)
- **2.4** Injetar via `CreateRemoteThread` + `LoadLibraryW` num processo AIR lançado suspenso
- **2.5** Rodar app 30s, parar, abrir `raw_capture.bin` com `flm_replay` → listener
- **Gate**: bytes gerados por POC, replayados via `flm_replay`, são aceitos pelo listener de validação (Scout-compat parser Rust)

### Fase 3 — Core C++ compartilhado + testes unitários
- **3.1** `SpscRing<T, N>` template header-only + testes unitários em gtest (push/pop, overflow, MT stress)
- **3.2** `ArenaBuffer` (alocador circular lock-free para bytes brutos de tamanho variável) + testes
- **3.3** `CompressedSink` (zlib streaming wrapper) + teste: comprimir→descomprimir = identidade
- **3.4** `WriterThread` + mock de ring → teste drain + pacing
- **3.5** `FileFormat` (header/footer write/parse) + teste round-trip
- **3.6** `CaptureController` (state machine start/stop/status) + teste com mock de IRuntimeHook
- **3.7** CMake do `shared/profiler/` compila em GCC/Clang/MSVC (CI-friendly)
- **Gate**: `cmake --build build --target profiler_tests && ctest` verde

### Fase 4 — Integração Windows ANE (Modo B)
- **4.1** `AirTelemetryRvas.h` finalizado + checks de integridade (SHA256 verify no boot)
- **4.2** `WindowsRuntimeHook::install()` — hook de entrada em `init_telemetry`, intercepta e captura `Player*`, retorna sem inicializar
- **4.3** `WindowsRuntimeHook::enable()` — instala hook no vtable slot, invoca `init_telemetry(player_ptr)` com cfg sintético na thread certa
- **4.4** `WindowsRuntimeHook::disable()` — swap do slot para no-op, opcionalmente dtor em cascata (modo B2)
- **4.5** `WindowsDiskMonitor` — `GetDiskFreeSpaceExW`
- **4.6** `ProfilerAneBindings.cpp` — 4 FREFunction: `startProfilingCapture`, `stopProfilingCapture`, `getProfilingStatus`, `takeProfilingMarker`
- **4.7** Wire-up no `AneAwesomeUtilsContext.java`/equivalente Windows + registro no extension descriptor XML
- **Gate**: app AS3 vazio carrega ANE, chama start → stop → arquivo `.flmc` gerado, sem crash e sem overhead idle mensurável

### Fase 5 — Ferramentas auxiliares (Rust, reaproveitam flash-profiler-core)
- **5.1** `flmc-validate` — lê header JSON, valida magic/versão/CRC, descomprime, passa pelo `FlmDecoder` do flash-profiler-core, gera stats (mensagens por tipo, duração, dropped)
- **5.2** `scout-replay` — extensão de `flm_replay.rs` pra aceitar `.flmc` (pular header + inflar)
- **5.3** `fake-scout` — TCP listener trivial em 127.0.0.1, salva bytes em arquivo (~30 LOC)
- **Gate**: `flmc-validate arquivo.flmc` retorna 0 e imprime stats coerentes

### Fase 6 — Teste automatizado E2E
- **6.1** `tests/profiler/TestProfilerApp.as` — app AS3 com timer start (T+2s) → stop (T+12s) → exit
- **6.2** `tests/profiler/run_windows_test.ps1` — build via AirBuildTool, launch via adl, verify .flmc, roda `flmc-validate`, compara byte-diff stream descompactado vs `fake-scout` capture do replay
- **6.3** Documentar cenários cobertos (normal, burst, kill no meio, disk cheio simulado)
- **Gate**: `./run_windows_test.ps1` retorna 0 em run limpo. CI pode rodar.

### Fase 7 — Validação manual única com Adobe Scout real
- **7.1** Gerar `.flmc` de captura de 30s
- **7.2** Instalar Adobe Scout (download histórico do airsdk.harman.com se disponível)
- **7.3** Rodar `scout-replay arquivo.flmc` enquanto Scout está escutando
- **7.4** Screenshot da sessão renderizando corretamente
- **Gate**: Scout mostra timeline com frames + eventos. Salvar screenshot em `docs/scout-validation.png`

### Fase 8 — Android (escopo futuro, após Fase 7 validada)
- Repetir RE análogo para `libCore.so` 51.1.3.10 arm64 + armv7
- `AndroidRuntimeHook.cpp` usando bytehook (PLT) ou inline patching
- `AndroidDiskMonitor.cpp` com `::statfs`
- Fix Android Scout #3573 (Java-side, independente)
- Harness `run_android_test.sh` via adb

## 8. Tooling & como usar

### AirBuildTool
```bash
cd "C:\Users\Joao\IdeaProjects\ddtank-client"
dotnet run --project tools/AirBuildTool -- --group windows64-debug --package --verbose
# Ou para o app de teste dedicado:
dotnet run --project tools/AirBuildTool -- --group profiler-test --verbose
```
Pode adicionar group `profiler-test` em `.idea/airBuildGroups.json` com SWF mínimo da ANE.

### CDB (debugger automatizado)
```bash
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" \
  -cf "script.cdb" -G -o "path\to\app.exe"

# script.cdb exemplo (comentários começam com $$):
$$ breakpoint em send_bytes
bp "Adobe AIR.dll"+0x493060 ".printf \"send_bytes this=%p data=%p size=%d\\n\", @rcx, @rdx, @r8d; db @rdx L20; gc"
g
```
Logs em stdout ou via `.logopen C:\temp\trace.log`. Modo batch com `-c "g; q"` para sair no primeiro hit.

### Ghidra headless (se precisar retornar)
```bash
"C:\tools\ghidra_12.0.3_PUBLIC\support\analyzeHeadless.bat" \
  "C:\AIRSDKs\AIRSDK_51.1.3.12\binary-optimizer\ghidra-project" AIR_Windows_x64 \
  -postScript DecompileByName.java FUN_1804930d0
```

### flash-profiler Rust (reaproveitamento)
- `flash-profiler-core/src/amf/reader.rs` — decoder AMF3 (valida bytes que o hook gravou)
- `flash-profiler-core/src/flm/decoder.rs` — decoder FLM (validar message types)
- `flash-profiler-core/src/bin/flm_replay.rs` — base do nosso `scout-replay`

## 9. Critérios de sucesso (resumo executivo)

1. App roda sem mexer em `.telemetry.cfg`. Overhead = 0% (CPU profile antes/depois indistinguível).
2. AS3 chama `Profiler.start(cfg)` → arquivo `.flmc` começa a ser gravado.
3. `Profiler.stop()` → arquivo finalizado, abre no Adobe Scout via `scout-replay`.
4. Arquivo médio (30s, só frames+memory) ≤ 5 MB. Com sampler+alloc full ≤ 900 MB duro.
5. Harness `run_windows_test.ps1` verde em CI.
6. Screenshot do Scout real renderizando uma sessão capturada.

## 10. Riscos conhecidos e mitigações

| Risco | Mitigação |
|---|---|
| `Player*` não capturado antes da Telemetry bootar | Hook de entrada em `init_telemetry` instalado em `AneAwesomeUtilsExtension.initialize()`, mesma ordem do `AirIMEGuard` |
| Thread errado chamando `init_telemetry` manual | Capturar thread ID no boot; re-despachar chamada para ele via `PostMessage` se necessário |
| Adobe Scout real exigir algo não coberto pelos bytes do runtime | Usar flash-profiler como listener primário; Scout real só na Fase 7 (validação manual) |
| Vtable slot mudar entre builds do runtime | Escopo único 51.1.3.10 — não acontece |
| `VirtualProtect` em `.rdata` bloqueado por DEP/CFG | Testar; fallback é trampolim inline no corpo do send_bytes |
| Zlib comprimir devagar em pico | Medir; fallback zstd (bundle extra ~600 KB) |
| Disco encher durante captura | `IDiskMonitor` checa a cada 1s + cap duro de tamanho do arquivo |

## 11. Deltas esperados em código no `ane-awesome-utils`

Arquivos novos:
```
docs/PROFILER_IMPLEMENTATION_PLAN.md              (este)
docs/profiler-rva-51-1-3-10.md                    (já existe)
shared/profiler/CMakeLists.txt
shared/profiler/include/*.hpp
shared/profiler/src/*.cpp
shared/profiler/tests/*.cpp
shared/profiler/poc/profiler_poc.cpp
shared/profiler/poc/injector.cpp
WindowsNative/src/profiler/*.{cpp,h}
src/br/com/redesurftank/aneawesomeutils/profiler/*.as
tests/profiler/*.{as,ps1,rs}
tools/rust-cli/flmc-validate/
tools/rust-cli/scout-replay/
tools/rust-cli/fake-scout/
```

Arquivos tocados:
```
CSharpLibrary/...                   (sem alteração esperada)
AneBuild/package-ane.bat            (incluir DLL do profiler)
WindowsNative/CMakeLists.txt        (adicionar subdir shared/profiler)
src/extension.xml                   (registrar novas FREFunctions)
AneAwesomeUtils*.java               (não tocar — escopo Windows)
```

Nenhum arquivo existente precisa ser refatorado para a Fase 1.

## 12. Pendências do RE (reabrir depois da Fase 7)

- Onde exatamente `enableSampler(ISampler*)` é chamado dentro de `init_telemetry` (pending §2 de `docs/profiler-rva-51-1-3-10.md`). Não bloqueia Fase 1–7 porque nosso cfg força `SamplerEnabled=1` e o runtime resolve sozinho.
- x86 mapping — quando necessário.
- Android libCore.so arm64/armv7 — Fase 8.
