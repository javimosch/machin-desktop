#!/usr/bin/env python3
"""Build the mdk docs site: docs/<page>/index.html + docs/<page>/llms.txt.

Every page is a body fragment (docs/_src/<slug>.html) and its agent twin
(docs/_src/<slug>.llms.txt, markdown). This script wraps the fragments in the
shared nav/footer and writes, per page:

  docs/<slug>/index.html   (docs/index.html for the home page)
  docs/<slug>/llms.txt     (docs/llms.txt for the home page — the site index)

The root llms.txt lists every page's llms.txt, so an agent can start at
https://javimosch.github.io/machin-desktop/llms.txt and fetch only what it
needs. Run it after editing anything under docs/_src/:

  python3 docs/build.py
"""
import html, json, os, re

DOCS = os.path.dirname(os.path.abspath(__file__))
SITE = "https://javimosch.github.io/machin-desktop"
REPO = "https://github.com/javimosch/machin-desktop"

# slug, nav label, page title, one-line description (used in nav, <meta>, llms.txt index)
PAGES = [
    ("", "Home", "mdk — native desktop apps in machin", "Native desktop apps in machin (MFL): GPUI-style GPU UI in one draw call per frame, hot reload that keeps state."),
    ("guide", "Guide", "Getting started", "Install, first app, state, dev loop, release builds, project layout."),
    ("layout", "Layout", "Layout & styling", "Elements, modifiers, the flexbox rules, wrapping text, virtual lists, scrolling."),
    ("hot-reload", "Hot reload", "Hot reload that keeps state", "How dev reload works, what survives it, the arena rule, the error overlay, its limits."),
    ("benchmarks", "Benchmarks", "Benchmarks: mdk vs MyGo", "MyGo's own scenes, same machine, both on the GPU — numbers, method, caveats, how to reproduce."),
    ("api", "API", "API reference", "Every framework function: containers, content, widgets, layout, style, state, input, format."),
    ("internals", "Internals", "Internals", "Host ABI, element records, the one-call renderer, text shaping cache, memory model."),
    ("agents", "Agents", "Building mdk apps with an agent", "llms.txt, SKILL.md and `mdk guide`: everything a coding agent needs to build an mdk app first try."),
]


GUIDE = json.load(open(os.path.join(DOCS, "..", "framework", "guide.json")))


def esc(t):
    return html.escape(t)


def code_html(t):
    return re.sub(r"`([^`]+)`", lambda m: "<code>" + m.group(1) + "</code>", esc(t))


def generated():
    """sections rendered from framework/guide.json — the one source of truth
    shared by the HTML, the llms.txt files and `./mdk guide`"""
    api_h, api_m = [], []
    for cat in GUIDE["api"]:
        api_h.append(f'<h3 id="{re.sub(r"[^a-z]+", "-", cat["category"].lower()).strip("-")}">{esc(cat["category"])}</h3>')
        api_h.append("<table><tr><th>signature</th><th>what it does</th></tr>")
        api_m.append(f"### {cat['category']}\n")
        for f in cat["fns"]:
            api_h.append(f"<tr><td><code>{esc(f['sig'])}</code></td><td>{esc(f['summary'])}</td></tr>")
            api_m.append(f"- `{f['sig']}` — {f['summary']}")
        api_h.append("</table>")
        api_m.append("")
    cmds_h = "<table><tr><th>command</th><th>what it does</th></tr>" + "".join(
        f"<tr><td><code>{esc(c['cmd'])}</code></td><td>{esc(c['does'])}</td></tr>" for c in GUIDE["commands"]) + "</table>"
    cmds_m = "\n".join(f"- `{c['cmd']}` — {c['does']}" for c in GUIDE["commands"])
    contract_h = "<ol>" + "".join(f"<li>{code_html(r)}</li>" for r in GUIDE["app_contract"]) + "</ol>"
    contract_m = "\n".join(f"{i+1}. {r}" for i, r in enumerate(GUIDE["app_contract"]))
    gotchas_h = "<ul>" + "".join(f"<li>{code_html(r)}</li>" for r in GUIDE["gotchas"]) + "</ul>"
    gotchas_m = "\n".join(f"- {r}" for r in GUIDE["gotchas"])
    req_m = "\n".join(f"- {r}" for r in GUIDE["requires"])
    return {
        "{{api_html}}": "\n".join(api_h), "{{api_md}}": "\n".join(api_m).rstrip(),
        "{{commands_html}}": cmds_h, "{{commands_md}}": cmds_m,
        "{{contract_html}}": contract_h, "{{contract_md}}": contract_m,
        "{{gotchas_html}}": gotchas_h, "{{gotchas_md}}": gotchas_m,
        "{{requires_md}}": req_m,
        "{{example}}": GUIDE["example"].rstrip(), "{{example_html}}": esc(GUIDE["example"].rstrip()),
        "{{version}}": GUIDE["version"],
    }


def fill(t):
    for k, v in generated().items():
        t = t.replace(k, v)
    return t


def rel(from_slug, to_slug):
    """relative URL from one page to another (pages are dirs; home is docs/)"""
    up = "../" if from_slug else ""
    return up + (to_slug + "/" if to_slug else "")


def page(slug, label, title, desc, body):
    up = "../" if slug else ""
    on = ' class="on"'
    nav = "\n".join(
        f'        <a href="{rel(slug, s)}"{on if s == slug else ""}>{l}</a>'
        for s, l, *_ in PAGES if s)
    llms = "llms.txt"
    body = body.replace("{{up}}", up)
    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{html.escape(title)}{"" if not slug else " · mdk"}</title>
  <meta name="description" content="{html.escape(desc)}">
  <link rel="icon" href="{up}assets/favicon.png">
  <link rel="stylesheet" href="{up}assets/site.css">
  <link rel="alternate" type="text/plain" title="llms.txt" href="{llms}">
  <meta property="og:title" content="{html.escape(title)}">
  <meta property="og:description" content="{html.escape(desc)}">
  <meta property="og:image" content="{SITE}/assets/scenes.png">
</head>
<body>
  <nav class="nav">
    <div class="in">
      <a class="brand" href="{rel(slug, '')}"><img src="{up}assets/logo.png" alt="">mdk</a>
      <div class="links">
{nav}
      </div>
      <a class="gh" href="{REPO}">GitHub ↗</a>
    </div>
  </nav>
  <main>
{body}
    <div class="agentbox">For agents: <a href="{llms}">this page as llms.txt</a><a href="{up}llms.txt">site index llms.txt</a><a href="{REPO}/blob/master/SKILL.md">SKILL.md</a></div>
  </main>
  <footer>
    <div class="in">
      <span>mdk · machin desktop kit · MIT</span>
      <a href="{REPO}">source</a>
      <a href="https://github.com/javimosch/machin">machin</a>
      <a href="{REPO}/issues">issues</a>
    </div>
  </footer>
</body>
</html>
"""


def main():
    index = [f"# mdk — machin desktop kit\n",
             f"> {PAGES[0][3]}\n",
             "Each page of the docs has its own llms.txt; fetch only what the task needs.",
             f"Repository: {REPO} · agent skill: {REPO}/blob/master/SKILL.md · "
             "machine-readable API: `./mdk guide` (JSON).\n",
             "## Pages\n"]
    for slug, label, title, desc in PAGES:
        src = os.path.join(DOCS, "_src", (slug or "home") + ".html")
        with open(src) as f:
            body = fill(f.read())
        out_dir = os.path.join(DOCS, slug) if slug else DOCS
        os.makedirs(out_dir, exist_ok=True)
        with open(os.path.join(out_dir, "index.html"), "w") as f:
            f.write(page(slug, label, title, desc, body))
        url = f"{SITE}/{slug + '/' if slug else ''}"
        if slug:
            with open(os.path.join(DOCS, "_src", slug + ".llms.txt")) as f:
                txt = fill(f.read()).rstrip() + "\n"
            txt += f"\n---\nHTML: {url} · site index: {SITE}/llms.txt\n"
            with open(os.path.join(out_dir, "llms.txt"), "w") as f:
                f.write(txt)
            index.append(f"- [{title}]({url}llms.txt): {desc}")
    with open(os.path.join(DOCS, "_src", "home.llms.txt")) as f:
        home = fill(f.read()).rstrip()
    with open(os.path.join(DOCS, "llms.txt"), "w") as f:
        f.write("\n".join(index) + "\n\n" + home + "\n")
    # sanity: every relative href/src in the built pages resolves to a file
    bad = []
    for slug, *_ in PAGES:
        d = os.path.join(DOCS, slug) if slug else DOCS
        with open(os.path.join(d, "index.html")) as f:
            for ref in re.findall(r'(?:href|src)="([^"#:]+)"', f.read()):
                p = os.path.normpath(os.path.join(d, ref))
                if not (os.path.exists(p) or os.path.exists(os.path.join(p, "index.html"))):
                    bad.append(f"{slug or 'home'}: {ref}")
    if bad:
        raise SystemExit("broken links:\n  " + "\n  ".join(bad))
    print(f"built {len(PAGES)} pages + {len(PAGES)} llms.txt — links ok")


if __name__ == "__main__":
    main()
