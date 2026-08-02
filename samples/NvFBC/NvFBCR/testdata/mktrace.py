# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Turn an NvFBCR.log into a PolicyTests replay fixture.

The fixture holds the capture timeline (NvFBC wake times, relay present deadlines) plus
what the relay ACTUALLY DID with it. The timeline is the test input; the recorded
behaviour is how the replay model's fidelity gets checked for this specific capture,
rather than assumed from whichever capture the model was first tuned on.

Bounds are deliberately NOT written here: they describe the model, which only the test
can measure. Run the suite once and it prints the lines to paste.

  usage: uv run samples/NvFBC/NvFBCR/testdata/mktrace.py \
             <log> samples/NvFBC/NvFBCR/testdata/<name>.trace "<description>"
  then add <name>.trace to index.txt and run the suite: it prints the bounds.
"""
import re, sys, os

CAP  = re.compile(r"capture #\d+ arr=(\d+)us")
PRE  = re.compile(r"temporal dl=(-?\d+)us")
WARMUP = 200          # must match the test: skips the lock's cold-start acquisition

def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    src, out = sys.argv[1], sys.argv[2]
    desc = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(src)
    arr, pres, synth = [], [], []
    for line in open(src, errors="replace"):
        m = CAP.search(line)
        if m:
            arr.append(int(m.group(1)))
            continue
        m = PRE.search(line)
        if m:
            pres.append(int(m.group(1)))
            synth.append("op=synth" in line)
    if not arr or not pres:
        print("no capture/temporal lines found"); return 1

    # What the relay really did, measured the same way the replay test measures itself.
    run = worst = longRuns = nsynth = 0
    for v in synth[WARMUP:]:
        if v:
            nsynth += 1; run += 1; worst = max(worst, run)
        else:
            if run >= 50: longRuns += 1
            run = 0
    if run >= 50: longRuns += 1
    pct = 100.0 * nsynth / max(1, len(synth) - WARMUP)

    def enc(v):
        return " ".join([str(v[0])] + [str(v[i] - v[i-1]) for i in range(1, len(v))])
    with open(out, "w") as f:
        f.write("# nvfbc-relay policy replay fixture\n")
        f.write(f"# {desc}\n")
        f.write("# units: microseconds. first value absolute, rest are deltas.\n")
        f.write(f"description {desc}\n")
        f.write("# What the RELAY did on this capture. The replay model is only trustworthy\n")
        f.write("# for a configuration where it reproduces these; the test checks that.\n")
        f.write(f"field_worst_run {worst}\n")
        f.write(f"field_long_runs {longRuns}\n")
        f.write(f"field_synth_pct {pct:.1f}\n")
        f.write(f"arrivals {len(arr)}\n{enc(arr)}\n")
        f.write(f"presents {len(pres)}\n{enc(pres)}\n")
    print(f"{out}: {len(arr)} arrivals, {len(pres)} presents, {os.path.getsize(out)/1e6:.2f} MB")
    print(f"  field behaviour past warmup {WARMUP}: synth {pct:.1f}%, "
          f"runs>=50 {longRuns}, worst {worst}")
    return 0

sys.exit(main())
