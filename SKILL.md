---
name: mdk
description: Build native desktop GUI apps in machin (MFL) with mdk (machin desktop kit) — a GPUI-style immediate-mode UI drawn on the GPU in one draw call per frame, with hot reload that keeps state. Use when asked to write, run, debug, screenshot or ship a desktop window/app/tool in machin, or to extend mdk itself (framework/ui.src, host/host.c, the mdk CLI).
---

# mdk — machin desktop kit

Docs: https://javimosch.github.io/machin-desktop/ (every page has an `llms.txt` twin;
index at `/llms.txt`). Machine-readable API: `./mdk guide` (JSON, `mdk.guide/v1`).

## Workflow (do all of it — especially the screenshot)

```bash
./mdk guide                                 # the API, contract and gotchas as JSON
./mdk new apps/thing                        # scaffold apps/thing/app.src
# edit apps/thing/app.src — define func view()
./mdk check apps/thing                      # typecheck only, no window
./mdk shot apps/thing /tmp/t.ppm --size 480 360 --click 120 80
convert /tmp/t.ppm /tmp/t.png               # then LOOK at /tmp/t.png
./mdk build apps/thing -o thing             # release binary (~2.6 MB)
```

- `--click X Y` (repeatable) sends real SDL mouse events, one click per two frames, so a
  screenshot after clicks verifies the interaction, not just the first frame.
- A display is required (`DISPLAY=:0` on this machine). The first run builds the vendored
  SDL3 3.4.20 (~1 min).
- `./mdk dev apps/thing` is for humans: it reloads on save, keeps state, and draws build
  errors over the last good frame. While it runs, `kill -USR1 <mdk-host pid>` saves the
  frame to `$MDK_SHOT` (default `/tmp/mdk-shot.ppm`). Find the pid with `pgrep -x mdk-host`
  (never `pkill -f build/mdk-host`: it matches your own shell's command line).

## The app contract

1. An app is a directory of `.src` files; mdk prepends `framework/*.src`.
2. Define exactly one `func view()`; never `main()`.
3. Build the UI top-down every frame: `col()`/`row()` … `end()`. **Modifiers apply to the
   last created element** — or, right after `end()`, to the container it closed.
4. Widgets return this frame's event: `if button("Save") { ... }`.
5. State goes in the host store — `state_int/set_int`, `state_f/set_f`,
   `state_str/set_str`. It survives hot reloads; MFL globals reset on reload.
6. **Never reassign an MFL global every frame.** Frames run inside `arena { }`; assigning
   a global promotes the value out of the arena → a leak per frame. Locals are free.
   Filling a global once (first-frame cache) is fine.

```go
func view() {
    n := state_int("count", 0)
    col()
    fill()
    center()
    gap(16.0)
        text(str(n), 56.0)
        bold()
        row()
        gap(10.0)
            if button("Increment") { set_int("count", n + 1) }
            if button_ghost("Reset") { set_int("count", 0) }
        end()
    end()
}
```

## API at a glance

containers `col row end spacer vlist scroll_view(key) card` · content `text para label
md_view(src)` · widgets `button button_ghost button_styled text_field(key,ph)
text_area(key,ph) nav_item list_item avatar badge divider` · layout `fill fillw fillh width
height size pad padxy gap grow align(A_*) justify(J_*) center clip at(x,y) scroll_y` · style
`bg fg font bold radius border shadow` (colors `0xRRGGBBAA`) · state `state_int set_int
state_f set_f state_str set_str` · keyboard `key(K_*) ctrl shift focus focused blur` ·
input `hovered pressed clicked frame_no animate` · window `window_title cursor` · format
`fmt2`. A real app to copy from: `apps/notes` (dashboard, editor, settings, `.md` storage). Full signatures: `./mdk guide` or
https://javimosch.github.io/machin-desktop/api/llms.txt

## MFL reminders that bite in UI code

- Loops are `while cond { }` (no C-style `for`). Floats need a dot: `16.0`. `float(i)`
  converts an int.
- `type T struct {` with one field per line.
- machin typechecks reachably — an error in a function `view()` never calls won't fail
  the build.
- Alpha `00` is invisible: `bg(0x6E56CF00)` draws nothing.
- Text input exists (`text_field`, `text_area`) but no selection yet, and `text_area`
  doesn't soft-wrap. Long content: `scroll_view(key)` or `vlist`.
- `state_str` returns a per-frame copy; `set_str(k, state_str(k, ""))` is safe.
- Script input for verification: `./mdk shot app out.ppm --click X Y --type "text" --key Return
  --key ctrl+a --wheel -3` (window pixels at scale 1 — don't use coordinates read off a
  downscaled screenshot).

## Working on mdk itself

| file | what | after editing |
|---|---|---|
| `framework/ui.src` + `framework/layout.src` | MFL core: element records (raw memory, `peek`/`poke`), `measure(i, maxw)`, `place`, `paint`, widgets | `./mdk check examples/counter`, then screenshot `examples/counter` and all four `MDK_SCENE`s of `examples/bench` |
| `framework/guide.json` | API source of truth (`mdk guide`, API page, llms.txt) | `python3 docs/build.py` |
| `host/host.c` | SDL3/GL window, one-call SDF renderer, HarfBuzz shaping cache, atlas, state store, reload, test hooks | `mdk dev`/`shot` rebuild it automatically; release: `./mdk build` |
| `docs/_src/*.html` + `*.llms.txt` | site pages | `python3 docs/build.py` (it also checks relative links) |
| `bench/run.py` | mdk vs MyGo runner | see https://javimosch.github.io/machin-desktop/benchmarks/ |

New host function: define it in `host.c` with a C ABI (`int64_t`, `float`, `double`,
`const char*`), declare it in the `extern "mdk" { }` block of `framework/ui.src`, wrap it
in a framework function, add it to `framework/guide.json`, rebuild the docs.
GLSL gotcha: `half` is a reserved word.
