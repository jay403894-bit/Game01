# Game01 — Platformer Demo

A 2D platformer built on **JLib**, a from-scratch Windows game library stack: a fiber-based
work-stealing scheduler, a DirectX 12 renderer, a custom 2D physics/world engine, a zero-thread input
layer, and a task-driven audio mixer. No engine, no SDL/SFML/raylib — every subsystem is hand-written.

## Play it

Download the repo (or clone it) and run **`x64/Release/Game01.exe`**. Nothing to install: the assets
and the Visual C++ runtime DLLs ship alongside the executable.

**Controls**

| Action | Keyboard | Gamepad |
|---|---|---|
| Move | `A` / `D` | Left stick or D-pad |
| Jump / wall-jump | `Space` | `A` |
| Fire | `E` | `X` |
| Menu | arrows / `W`,`S` + `Enter` | stick/D-pad + `A` |
| Quit | `Esc` | `Start` |

Gamepads are read through Raw Input HID — including Xbox pads — so gamepad support costs the process
**zero threads**.

## The stack underneath

| Library | What it does |
|---|---|
| [JLib-Scheduler](https://github.com/jay403894-bit/JLib-Scheduler) | Fiber work-stealing scheduler: hand-written x64 context switch, Chase-Lev deques, task DAG with logic gates, P/E-core aware placement |
| [DirectX12-Renderer](https://github.com/jay403894-bit/DirectX12-Renderer) | DX12 renderer — 2D sprite batching, 3D meshes, GPU particles on the compute queue |
| [Physics2D](https://github.com/jay403894-bit/Physics2D) | SoA "ECS-lite" 2D physics and world/tile engine |
| [Input](https://github.com/jay403894-bit/Input) | Raw Input keyboard/mouse + HID gamepads, no dependencies, no threads |
| [Sound](https://github.com/jay403894-bit/Sound) | Ring-buffered mixer that runs as scheduler tasks rather than owning a thread |
| [Assets](https://github.com/jay403894-bit/Assets) | Async asset loading/management through the scheduler |
| [SceneManager](https://github.com/jay403894-bit/SceneManager) | Scene stack |

## Building from source

Requires Windows x64, Visual Studio 2022+ with the C++ workload, and the JLib libraries deployed to
`C:\libs`. Open `Game01.slnx` and build x64 Release — the assets are already in `x64/Release/`
(tracked deliberately; see `.gitignore`), so the build output is immediately runnable.

## License

BSD 3-Clause.
