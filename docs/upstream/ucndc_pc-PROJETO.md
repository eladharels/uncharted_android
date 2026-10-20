# Uncharted 3 — port nativo para PC

Port nativo para Windows de **Uncharted 3: Drake's Deception**, a partir da versão de PS4 da
*Uncharted: The Nathan Drake Collection* (Bluepoint, 2015).

O diário do projeto, com o que foi feito e por quê, está em [docs/uc3/DEVLOG.md](docs/uc3/DEVLOG.md).

## A ideia

O PS4 usa um processador x86-64, a mesma arquitetura do PC. Então o executável original do
jogo (`eboot.bin`) pode rodar **direto na CPU**, sem emulação. O que precisa ser feito:

- **Runtime:** reimplementar as funções de sistema do PS4 que o jogo chama (memória, threads,
  arquivos, áudio, vídeo, controle, saves).
- **GPU:** traduzir os comandos e shaders da GPU do PS4 (AMD GCN) para Vulkan, usando o núcleo
  de vídeo do [shadPS4](https://github.com/shadps4-emu/shadPS4).

A técnica já funcionou em outros jogos:
[bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) (original, Linux),
o [port de Bloodborne para Windows](https://github.com/Mrsuss60/bloodborne_pc_windows_port) e o
[God of War III Remastered](https://github.com/FelipePrado0/gow3_pc). **Este repositório é um
fork do port do GoW3** (tag `windows-v2.1.7`); o trabalho do Uncharted fica na branch `uc3`.

A versão de PS3 foi descartada: o processador Cell (PowerPC + SPUs) não roda num PC sem
recompilação completa, e a Naughty Dog colocou boa parte do motor nas SPUs.

## Situação

**Jogável do início até O Castelo (27 %)**, com menu, som, controle, saves e cutscenes, sem
crash do port numa sessão de 2 h. Núcleo gráfico: shadPS4 nightly `23c4133` (12/10/2026),
atualizado por mesclagem de três vias. O modo de *readback* preciso (`GOW3_READBACKS=2`, padrão)
é o que desenha o cenário. Desempenho: 60 FPS em menus, 30–48 no museu, 18–24 nas ruas e no
castelo, no mesmo nível do próprio shadPS4 nessas cenas. Em aberto: o custo do *readback*
preciso onde ele domina, glitches visuais relatados, e dois defeitos do núcleo (cabelo branco
após carregar checkpoint, pontos de poeira) que o shadPS4 também tem.

**Para continuar o trabalho numa sessão nova, leia [docs/uc3/HANDOFF.md](docs/uc3/HANDOFF.md).**
Detalhes e histórico no [devlog](docs/uc3/DEVLOG.md).

Para jogar: `powershell -ExecutionPolicy Bypass -File tools\run_uc3.ps1` (log em `out\last-run.log`).

| Fase | Objetivo | Status |
|---|---|---|
| 0 | Ambiente de build, estudo do código, organização | concluída |
| 1 | Separar o que é específico do GoW3 do que é genérico | concluída (launcher fica para a fase 5) |
| 2 | Preparar o executável do Uncharted 3 e listar as funções faltantes (`--check-imports`) | concluída: 0 faltando ([IMPORTS.md](docs/uc3/IMPORTS.md)) |
| 3 | Implementar o runtime até o jogo abrir (menus) | concluída |
| 4 | Gameplay: gráficos, áudio, cutscenes, saves | **jogável até 27 %; desempenho e glitches em andamento** |
| 5 | Launcher, patches (resolução, FPS), polimento | — |

## O que é preciso

- **Dump da sua própria cópia** da Nathan Drake Collection, feito num PS4 desbloqueado: a pasta
  com `eboot.bin`, `sce_module` e `sce_sys`, de preferência com a atualização 1.02. Nenhum
  arquivo do jogo entra neste repositório.
- Windows 10/11 64 bits, GPU com Vulkan 1.3.
- Para compilar: [MSYS2](https://www.msys2.org) com o ambiente CLANG64 (pacotes em
  [packaging/windows/README.md](packaging/windows/README.md)).

## Compilar

No ambiente desta máquina, o MSYS2 está em `D:\msys64`:

```bash
export MSYS2_ROOT=/d/msys64
bash build.sh        # gera out/gow3-probe.exe
```

## Rodar o Uncharted 3

No terminal MSYS2 CLANG64 (ou com as variáveis definidas no PowerShell):

```bash
export MSYS2_ROOT=D:/msys64
export GOW3_GAME_DIR=D:/Uncharted/CUSA02344/app0
export GOW3_PATCH_DIR=D:/Uncharted/CUSA02344/patch
export GOW3_EXECUTABLE=big3-ps4_Shipping.elf
python run.py --check-imports   # lista as funções que faltam; sem a opção, roda o jogo
```

Há também um ambiente de apoio no **WSL (Ubuntu 26.04)**, em `~/uncharted/uc3_pc`, para os
testes que dependem do Linux e para depurar travamentos de threads (veja o devlog,
2026-10-13). O port em si é testado no Windows.

Os nomes `gow3-*` e as variáveis `GOW3_*` vêm do projeto original e por enquanto ficam como
estão (veja o DEVLOG, 2026-10-12).

## Organização da pasta de trabalho

```
D:\Uncharted\
├── uc3_pc\              ← este repositório (branch uc3)
├── CUSA02344\app0\      ← o jogo extraído (fora do git); o Uncharted 3 é big3-ps4_Shipping.elf
├── CUSA02344\patch\     ← atualização 01.02 extraída (GOW3_PATCH_DIR)
├── tools\pkgtool\       ← PkgTool (LibOrbisPkg), extrai o .pkg
├── ref\gow3_pc\         ← port do GoW3 original, só referência
├── ref\bloodborne_pc\   ← port do Bloodborne (Windows), só referência
├── tools\shadps4\       ← shadPS4 0.19.0, para comparar o comportamento do jogo
├── tools\shadps4-nightly\ ← shadPS4 nightly 23c4133 (lançar pelo ELF: veja o HANDOFF)
├── ref\shadPS4\         ← clone do shadPS4 (consulta; worktrees shadPS4-b4e7ae7, shadPS4-v0.19.0)
└── ref\merge\           ← repositório de mesclagem do núcleo gráfico (ramos base/upstream/port…)

D:\msys64\               ← MSYS2 (build do Windows)
D:\WSL\Ubuntu\           ← disco do Ubuntu no WSL (repositório em ~/uncharted/uc3_pc)
```

## Licença e créditos

GNU GPL v2 ou posterior, como o projeto de origem ([LICENSE](LICENSE)). Os créditos do
trabalho em que este port se baseia estão no [README.md](README.md) original.
Uncharted é marca registrada da Sony Interactive Entertainment. Este projeto não tem relação
com a Sony, a Naughty Dog nem a Bluepoint Games.
