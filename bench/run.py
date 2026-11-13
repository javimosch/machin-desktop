#!/usr/bin/env python3
"""Linux runner for MyGo's comparison scenes: mdk vs MyGo on the same machine.

MyGo's own runner (benchmarks/run in github.com/egoist/mygo) measures on macOS
only. This one keeps its method on Linux:
  - the scenes and their apps are MyGo's (benchmarks/mygo) and ours
    (examples/bench), speaking the same stdout protocol: "ready" once the first
    frame is on screen, "start" after the warmup frames, "done {...}" with the
    frame intervals' p50/p99/max after the measured frames;
  - CPU and memory come from outside the process (/proc: utime+stime of every
    thread, RSS, peak RSS), so framework threads and the GL driver count;
  - frameworks take turns within each round; results are medians of the runs.

  bench/run.py --mygo /tmp/bench-mygo --mdk build/bench-release [--runs 5]
"""
import argparse, json, os, platform, statistics, subprocess, sys, time

TCK = os.sysconf("SC_CLK_TCK")
SCENES = ["counter", "list", "grid", "document"]


def cpu_s(pid):
    with open(f"/proc/{pid}/stat") as f:
        parts = f.read().rsplit(")", 1)[1].split()
    return (int(parts[11]) + int(parts[12])) / TCK


def mem_kb(pid, key):
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith(key):
                return int(line.split()[1])
    return 0


def launch(fw, binary, scene, frames, warmup):
    env = dict(os.environ, BENCH_FRAMES=str(frames), BENCH_WARMUP=str(warmup))
    args = [binary, scene]
    if fw == "mdk":
        env["MDK_BENCH"] = "0" if scene == "counter" else "1"
        args += ["--size"] + (["360", "260"] if scene == "counter" else ["800", "600"])
    t0 = time.monotonic()
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, text=True, bufsize=1)
    return p, t0


def wait_line(p, prefix, timeout=60):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        line = p.stdout.readline()
        if not line:
            if p.poll() is not None:
                raise RuntimeError(f"exited before {prefix!r}")
            continue
        if line.startswith(prefix):
            return line.strip(), time.monotonic()
    raise RuntimeError(f"timeout waiting for {prefix!r}")


def stop(p):
    p.terminate()
    try:
        p.wait(3)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()


def run_counter(fw, binary, idle):
    p, t0 = launch(fw, binary, "counter", 600, 120)
    try:
        _, t_ready = wait_line(p, "ready")
        time.sleep(idle)
        rss = mem_kb(p.pid, "VmRSS")
        c0 = cpu_s(p.pid)
        time.sleep(idle)
        c1 = cpu_s(p.pid)
        return {"startupMs": (t_ready - t0) * 1000, "idleMemMB": rss / 1024,
                "idleCpuPct": (c1 - c0) / idle * 100}
    finally:
        stop(p)


def run_animated(fw, binary, scene, frames, warmup):
    p, _ = launch(fw, binary, scene, frames, warmup)
    try:
        wait_line(p, "ready")
        wait_line(p, "start")
        c0 = cpu_s(p.pid)
        line, _ = wait_line(p, "done", timeout=120)
        c1 = cpu_s(p.pid)
        d = json.loads(line.split(" ", 1)[1])
        return {"cpuMsPerFrame": (c1 - c0) * 1000 / d["frames"],
                "fps": d["frames"] / (d["wallUs"] / 1e6),
                "p99Ms": d["p99Us"] / 1000, "peakMemMB": mem_kb(p.pid, "VmHWM") / 1024}
    finally:
        stop(p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mygo", required=True)
    ap.add_argument("--mdk", required=True)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--warmup", type=int, default=120)
    ap.add_argument("--idle", type=float, default=5.0)
    ap.add_argument("--scenes", default=",".join(SCENES))
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    apps = {"mdk": a.mdk, "mygo": a.mygo}
    scenes = a.scenes.split(",")
    samples = {fw: {s: {} for s in scenes} for fw in apps}
    for fw, b in apps.items():  # first launch of each, not measured: page cache, driver shader cache
        p, _ = launch(fw, b, "counter", 600, 120)
        wait_line(p, "ready")
        stop(p)
    for r in range(a.runs):
        for scene in scenes:
            for fw, b in (apps.items() if r % 2 == 0 else reversed(list(apps.items()))):
                m = run_counter(fw, b, a.idle) if scene == "counter" else run_animated(fw, b, scene, a.frames, a.warmup)
                for k, v in m.items():
                    samples[fw][scene].setdefault(k, []).append(v)
                print(f"run {r+1}/{a.runs} {scene:8} {fw:4} " + " ".join(f"{k}={v:.2f}" for k, v in m.items()), file=sys.stderr)
    res = {"date": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "runs": a.runs, "frames": a.frames,
           "machine": {"os": platform.platform(), "cpu": open("/proc/cpuinfo").read().split("model name")[1].split(":")[1].split("\n")[0].strip(),
                       "cpus": os.cpu_count()},
           "frameworks": {fw: {"size": os.path.getsize(b), "scenes": {
               s: {k: {"median": statistics.median(v), "low": min(v), "high": max(v)} for k, v in samples[fw][s].items()}
               for s in scenes}} for fw, b in apps.items()}}
    out = json.dumps(res, indent=2)
    if a.out:
        with open(a.out, "w") as f:
            f.write(out + "\n")
    print(out)


if __name__ == "__main__":
    main()
