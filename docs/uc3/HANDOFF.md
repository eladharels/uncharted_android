# Guia de continuidade — Uncharted 3 PC (uc3)

Leia isto antes de qualquer coisa numa sessão nova. O histórico completo está no
[DEVLOG.md](DEVLOG.md) (entradas mais novas no topo); a visão geral está em
[PROJETO.md](../upstream/ucndc_pc-PROJETO.md). Atualizado em 2026-10-14.

## Estado em uma frase

O jogo roda do início até **O Castelo (27 %)** sem crash do port numa sessão de 2 h, com som,
controle, saves e cutscenes; o núcleo gráfico é o shadPS4 **nightly `23c4133`** (12/10/2026) e
o modo de *readback* **preciso** (`GOW3_READBACKS=2`, padrão) é o único que desenha o cenário.

## Como compilar e rodar

```bash
# MSYS2 CLANG64 (D:\msys64). No Bash da ferramenta:
export PATH="/d/msys64/clang64/bin:/d/msys64/usr/bin:$PATH" MSYSTEM=CLANG64 MSYS2_ROOT=/d/msys64
bash build.sh                     # gera out/gow3-probe.exe; erros em out/gpu-build.log
```

```bash
# Jogar (PowerShell, de qualquer pasta); log em out\last-run.log (o anterior vai para previous-run.log)
powershell -ExecutionPolicy Bypass -File D:\Uncharted\uc3_pc\tools\run_uc3.ps1
powershell -ExecutionPolicy Bypass -File D:\Uncharted\uc3_pc\tools\run_uc3.ps1 -Set GOW3_FRAME_STATS=1,GOW3_FRAME_CAPTURE_KEY=1
```

Regras: **nunca compilar nem rodar testes com `gow3-probe.exe` aberto** (`tasklist`); nunca mexer
no jogo enquanto o jogador joga; só fechar instâncias que a própria sessão abriu. O `python`
do Windows é o stub da Store: use `/d/msys64/clang64/bin/python`.

Opções úteis em `-Set`:

| Variável | Efeito |
|---|---|
| `GOW3_READBACKS=2` / `1` | preciso (padrão, cenário certo) / relaxado (cenário e texturas somem, só para medir) |
| `GOW3_FRAME_STATS=1` | "Frame stats" a cada 5 s, linhas `Stall:` dos quadros > 40 ms, `uc3 draw stats` |
| `GOW3_FRAME_CAPTURE_KEY=1` | F11 captura o quadro em `user/captures/frame_*.txt` |
| `GOW3_CAPTURE_TRIGGER=<arquivo>` | criar o arquivo dispara uma captura (automação) |
| `GOW3_FAULT_HOLD=<s>` | depois de um "Fault", segura o processo: `lldb -p PID --batch -o 'thread backtrace all'` |
| `GOW3_DUMP_SHADERS=1` | SPIR-V em `user/shader/dumps`; validar com `spirv-val --target-env vulkan1.3` |
| `GOW3_FLASH_TRACE=1` | rastreio das piscadas em preto (re-uploads de imagens escritas pela GPU) |

Testes: alvos `ninja -C out/gpu <nome>-test` e executar em `out/gpu` (31 passam;
`videodec2-test` roda a partir da raiz do repositório). Não valem no Windows:
`scene-resolution`, `taa-shader`, `camera-motion` (Lavapipe), `upscaler-support` (`setenv`),
`cheat-hooks` (GoW3).

## Ferramentas de automação (tools/)

- `uc3_sendkey.ps1 <processo> <tecla> [-Hold ms] [-Post]`: teclas para a janela. Port: Espaço =
  X, Shift esq. = O, Enter = Options; use `-Hold 200` (o pad lê o estado da tecla por quadro).
  shadPS4: `n` = X, `enter` = Options. F11 sintético **não** chega ao overlay: use o gatilho.
- `uc3_screenshot.ps1 <png> [-ProcessName shadPS4]`: print da janela (traz para a frente).
- `uc3_stalls.py <log>`: agrega as linhas `Stall:` (onde o tempo dos quadros lentos vai).
- `uc3_saves_to_shadps4.py --shadps4 <pasta>`: copia os saves do port para o shadPS4 (faz backup).
- Roteiro do probe: `Start-Process powershell` com `run_uc3.ps1` (separado), esperar
  "first picture" no log, Espaço (título), Espaço ("Continuar" é o primeiro item), Espaço
  (diálogo offline), ~90 s de carregamento, print.

## Saves e emulador de comparação

- Saves do port: `user/savedata/1/CUSA02344` (`SAVEDATA038` = "O Castelo", 27 %, 1h54).
- shadPS4 0.19.0: `D:\Uncharted\tools\shadps4\shadPS4.exe -g D:\Uncharted\CUSA02344\CUSA02344\eboot.bin`.
- shadPS4 nightly: o lançador não funciona (regressão #5332 apaga o `..` do caminho); use
  `D:\Uncharted\tools\shadps4-nightly\shadPS4.exe -g D:\Uncharted\CUSA02344\CUSA02344\big3-ps4_Shipping.elf -- eboot.bin ispartofuccollection`.
- Os dois caem na assertiva `m_gfxEopTick` com o cache de shaders frio; reabrir 2 ou 3 vezes.

## Desempenho medido (modo preciso, salvo indicação)

| Cena | Port | shadPS4 | Onde vai o tempo nos quadros lentos |
|---|---|---|---|
| Título/menus | 60 | 60 | — |
| Museu (cap. 2) | 32–48 | 41 (nightly) | falhas de leitura do readback preciso (~3 ms cada) |
| Ruas de Cartagena | 17–24 | — | GPU ~30–40 ms + falhas de leitura 20–30 ms |
| O Castelo (combate) | 18–24 (relaxado 20–24) | 18–20 | espera pela GPU ~30 ms, 145 submissões/quadro |

Cada falha de leitura = `Rasterizer::ReadMemory` → `BufferCache::DownloadMemory` →
`scheduler.Finish()` (drena a fila da GPU inteira). Na sessão de 2 h do jogador foram 56 mil
falhas (166 s). **Medir sempre nos dois modos** antes de atacar uma cena: no castelo o readback
custa só 2–4 FPS. Logs guardados em `out/`: `player-0.19.0-session.log` (2 h),
`temple-nightly.log`, `temple-relaxed.log`, `streets-0.19.0.log`.

## Problemas em aberto

1. **Custo do readback preciso** onde ele domina (museu, ruas). Caminhos: esperar só o *tick*
   da última escrita da janela em vez de `Finish()`; baixar na EOP, em lote, as faixas que a CPU
   leu no quadro anterior; marcar e baixar por quadro.
2. **Glitches visuais** relatados pelo jogador na sessão de 2 h (16 capturas F11 em
   `user/captures/frame_20261013-19*`), ainda sem descrição.
3. **Piscadas em preto** de um quadro (não reproduzidas desde o rastreio; `GOW3_FLASH_TRACE=1`).
4. **Tela cinza** do `big3` em modo coleção (lançador `eboot.bin`) no port.
5. **Defeitos do núcleo** (iguais no shadPS4 0.19.0 e nightly): cabelo do Drake branco ao
   carregar checkpoint, pontos amarelos de poeira. Não é trabalho do port.

## Núcleo gráfico: como atualizar de novo

`gpu/shadps4` é uma cópia de subconjunto do shadPS4 (sem git) com as marcas `gow3:`/`uc3:`.
As duas atualizações de 13/10 (b4e7ae7 → v.0.19.0 → nightly 23c4133) foram mesclagens de três
vias no repositório de trabalho `D:\Uncharted\ref\merge` (ramos `base`, `upstream`, `port`,
`upstream2`, `port2`; `port2~1` é a árvore do port antes da nightly). Clone de consulta em
`D:\Uncharted\ref\shadPS4` (HEAD = 23c4133; worktrees `shadPS4-b4e7ae7`, `shadPS4-v0.19.0`).

Receita: ramo novo a partir da árvore atual de `gpu/shadps4` (copiar, normalizar para LF);
ramo do upstream novo como filho de `upstream2` (subconjunto `common`, `core/libraries/{gnmdriver,
videoout,...}`, `shader_recompiler`, `video_core`); `git merge`; resolver; copiar de volta;
ajustar a lista `COMMON` do `gpu/CMakeLists.txt` para `.cpp` novos/removidos; compilar; testes;
`GOW3_DUMP_SHADERS=1` + `spirv-val` nos dumps; rodar o checkpoint.

Política de resolução: **cache de texturas** (`texture_cache.*`, `image.*`, `sampler.*`,
`common/lru_cache.h`) fica na versão do port (a reestruturação #5219 não foi levada; só
correções pontuais); os outros arquivos reescritos pelo port ficam com a versão do port mais as
mudanças pequenas do upstream. A mesclagem automática mistura metades de refatorações do upstream
nos arquivos do port: conferir sempre os trechos auto-mesclados em `vk_rasterizer`,
`texture_cache` e `vk_scheduler`.

Armadilhas conhecidas: o bloco de *push constants* do port (`DefinePushDataBlock`) declara
membros depois dos `NUM_BUFFERS` offsets (hoje 56: quatro blocos, `motion_param` em 72,
`scene_size` em 76, com `static_assert`); um SPIR-V inválido **derruba o compilador do driver
AMD** em `createComputePipeline` ("Fault … amdvlk64.dll" na inicialização). `ExtendWriteFault`
(region manager) segue os helpers `UpdateProtection`/`UpdateState` do upstream. O sirit tem
`OpNClamp` acrescentado. Versões do cache de shaders: 10/9/7 (subir ao mudar layouts).

## Mapa do código

- `src/` runtime HLE em C (`probe.c` carrega o ELF; `runtime_*.c` memória, threads, fibers,
  pad, videodec2, saves…). `run.py` monta o ambiente e chama o probe.
- `gpu/shim/` cola com o núcleo: `gow3gpu.cpp` (MemoryManager, hooks de memória, janela),
  `core/emulator_settings.h` (opções `GOW3_*`), `gow3_toggles.h` (máscara decimal em
  `GOW3_TOGGLE_FILE`), overlay, cópias em threads, write log.
- `gpu/shadps4/` núcleo (video_core, shader_recompiler, common). Pontos do port: `vk_rasterizer`
  (draw pipe em dois estágios, binding helper), `vk_scheduler` (gravação em thread),
  `vk_draw_pipe.h`, `vk_frame_capture.cpp`, `texture_cache` (aliasing, readbacks),
  `buffer_cache` (uploads em threads, `ExtendWriteFault`), `page_manager`/`region_manager`.
- `docs/uc3/IMPORTS.md`: funções do PS4 que o port implementa.
