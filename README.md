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
./mdk build examples/counter   # one release binary, app linked in (2.6 MB)
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

## Benchmarks: mdk vs MyGo, same machine

[MyGo's own comparison scenes](https://github.com/egoist/mygo/tree/main/benchmarks)
— its app (`benchmarks/mygo`, built as its docs say: `-tags mygo_noinspector
-ldflags "-s -w -X ...production=1"`) against ours (`examples/bench`), the same
text from the same generators in windows of the same size. MyGo's runner is
macOS-only, so [`bench/run.py`](bench/run.py) keeps its method on Linux:
CPU and memory from outside the process (`/proc`, every thread and the GL
driver included), frame intervals from the apps' own `ready`/`start`/`done`
protocol, 120 warmup + 600 measured frames, frameworks taking turns, medians.
Both draw on the GPU here (verified: `MYGO_FRAME_STATS=all` reports
`drawn on the GPU: 4133 instances in 1 batches`).

Intel i5-11320H (Xe graphics), Ubuntu 22.04, Mesa, X11, 60 Hz — 3 rounds
([raw](bench/results-linux.json)); MyGo d5c76cf (0.9.0), mdk M2:

| | mdk | MyGo | |
|---|---|---|---|
| **binary size** | **2.6 MB** | 10.1 MB | 3.9× smaller |
| **counter**: startup to first frame | **283 ms** | 506 ms | (5 runs; noisy on this laptop) |
| counter: memory at rest | **76.5 MB** | 116.1 MB | |
| counter: CPU at rest | 0.00% | 0.00% | tie |
| **list** (100k rows, scrolling): CPU/frame | **0.48 ms** | 2.83 ms | 5.9× |
| list: fps · p99 frame | 60.0 · 20.7 ms | 59.5 · 28.1 ms | |
| list: peak memory | **77 MB** | 123 MB | |
| **grid** (600 live cells): CPU/frame | **3.9 ms** | 37.6 ms | 9.6× |
| grid: fps · p99 frame | **60.0** · 21.2 ms | 18.8 · 91.5 ms | MyGo drops frames |
| grid: peak memory | **78 MB** | 147 MB | |
| **document** (20k chars rewrapped every frame): CPU/frame | **0.52 ms** | 35.9 ms | 69× |
| document: fps · p99 frame | **60.0** · 20.0 ms | 21.0 · 89.7 ms | MyGo drops frames |
| document: peak memory | **83 MB** | 144 MB | |

How to read it:

- **Same machine, same scenes, both on the GPU** — but one laptop. On its own
  site (Apple M5, macOS) MyGo's grid is 5.7 ms/frame and its document
  4.9 ms; here its pure-Go text layout (`layout 34–42 ms` in its frame
  stats, 600 text layouts a frame) is what it spends. mdk's text is
  HarfBuzz-shaped once and cached; a changed string is re-shaped, an
  unchanged one only re-broken into lines over cached advances.
- **Memory floor**: an empty SDL3+GL window on this Mesa stack is already
  66 MB, so mdk's own share at rest is ~11 MB; MyGo on Linux also carries
  GTK.
- **p99 ~20 ms** on both is this X11 session's compositor jitter at 60 Hz
  (frame intervals are timed at frame start, as MyGo's recorder does).
- **What this doesn't measure**: breadth. MyGo has ~80 widgets,
  accessibility, IME, RTL, menus/tray/dialogs, a webview mode, installers
  and auto-update; mdk M2 has boxes, flex, text, wrapping, buttons and a
  virtual list. The point here is the engine: GPUI-style batching driven by
  compiled MFL.

Reproduce:

```
# MyGo's app, from a clone of github.com/egoist/mygo
CGO_ENABLED=0 go build -trimpath -tags mygo_noinspector \
  -ldflags "-s -w -X github.com/egoist/mygo.production=1" -o /tmp/bench-mygo ./benchmarks/mygo
./mdk build examples/bench -o build/bench-release
DISPLAY=:0 python3 bench/run.py --mygo /tmp/bench-mygo --mdk build/bench-release --runs 3
```

## Verify

```
DISPLAY=:0 ./mdk shot examples/counter /tmp/c.ppm --click 198 236   # real SDL click events
kill -USR1 <host-pid>     # screenshot a running dev window to $MDK_SHOT (/tmp/mdk-shot.ppm)
```

## Requirements

machin ≥ 0.142, a C compiler, cmake (SDL3 3.4.20 is vendored and built
statically on first run), FreeType + HarfBuzz dev packages, OpenGL 3.3.

## Roadmap

- **M2** — done: MyGo's benchmark scenes measured side by side (above);
  shaping cache, word wrap, bold, virtual list, culling, zero-wake idle.
- **M2.5** — text input + IME, scroll views with wheel/drag, clipping.
- **M3** — inspector: click an element, get its MFL file:line (agent-ready);
  `machin check` gate before every reload.
- **M4** — webview windows (machweb + reactive wasm), menus/tray/dialogs,
  `SDL_gpu` backend, macOS/Windows packaging.
