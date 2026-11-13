# machin-desktop (mdk)

Native desktop apps in [machin](https://github.com/javimosch/machin) (MFL), with
a typed GPU-drawn UI and **hot reload that keeps your state**.

```
func view() {
    n := state_int("count", 0)
    col()  fill()  center()  gap(16.0)
        text(str(n), 56.0)
        row()  gap(10.0)
            if button("Increment") { set_int("count", n + 1) }
            if button_ghost("Reset") { set_int("count", 0) }
        end()
    end()
}
```

```
./mdk dev examples/counter     # edit app.src while it runs — window + count survive
./mdk build examples/counter   # one release binary, app linked in (2.5 MB)
```

## How it works

| Layer | What it does |
|---|---|
| `framework/ui.src` (MFL) | GPUI-style core: the view builds an element tree each frame, a small flexbox lays it out (direction, gap, padding, grow/fill, align, justify), paint emits rounded rects / borders / shadows / text. Input is immediate: `button()` returns `true` on the frame it's clicked, hit-tested against last frame's layout. |
| `host/host.c` (C, ~650 lines) | SDL3 window + OpenGL 3.3. Every primitive is one instance in one buffer, drawn with **one instanced draw call** through an SDF fragment shader. Text is HarfBuzz-shaped, FreeType-rasterized into a glyph atlas. Owns the state store and the element buffers. |
| `mdk` (bash) | `dev`: compile the app to a `.so`, run the host, rebuild on save. `build`: link app + host into one binary. |

**Hot reload.** In dev the app is a shared object. On save, `mdk` re-runs
`machin encode` (which typechecks) and recompiles (~0.5–1 s); the host notices,
`dlopen`s the new copy (0.3 ms) and keeps going. State lives in the host
(`state_int`/`set_int`, `state_f`, `state_str`), so it survives; the window
never closes. A rejected edit is drawn as an overlay over the last good frame —
never a blank window.

**Memory discipline.** The exported frame runs inside `arena { }`, so a
frame's temporaries are freed when it returns. The element tree lives in
host-owned raw memory (`peek`/`poke`), not MFL slices: assigning to a global
inside an arena promotes the value out of it, so a per-frame tree in globals
would leak. Keep app state in the host store, not in MFL globals.

## Numbers (milestone 1, counter app)

Measured on an Intel Xe laptop (Ubuntu 22.04, Mesa, X11):

| | mdk | empty SDL3+GL window (floor) | MyGo (its own site, Apple M5) |
|---|---|---|---|
| release binary | **2.5 MB** | — | 9.7 MB |
| first frame | 310–360 ms | 155–327 ms | 173 ms |
| memory at rest | 78 MB | 66 MB | 34 MB |
| CPU at rest | ~0% (event-driven) | — | 0.01% |
| draw calls / frame | 1 | — | — |

Startup and memory are dominated by the platform here (an empty GL window on
this Mesa stack costs 66 MB and up to 330 ms); mdk adds ~12 MB and tens of ms
over that floor. Apples-to-apples numbers need MyGo's benchmark scenes run on
the same machine — that is milestone 2.

## Verify

```
DISPLAY=:0 ./mdk shot examples/counter /tmp/c.ppm --click 198 236   # real SDL click events
kill -USR1 <host-pid>     # screenshot a running dev window to $MDK_SHOT (/tmp/mdk-shot.ppm)
```

## Requirements

machin ≥ 0.142, a C compiler, cmake (SDL3 3.4.20 is vendored and built
statically on first run), FreeType + HarfBuzz dev packages, OpenGL 3.3.

## Roadmap

- **M2** — MyGo's benchmark scenes (100k-row virtual list, 600-cell live
  dashboard, document resize) measured side by side; text input + IME;
  scroll views.
- **M3** — inspector: click an element, get its MFL file:line (agent-ready);
  `machin check` gate before every reload.
- **M4** — webview windows (machweb + reactive wasm), menus/tray/dialogs,
  `SDL_gpu` backend, macOS/Windows packaging.
