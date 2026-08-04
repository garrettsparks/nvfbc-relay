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
             <log> samples/NvFBC/NvFBCR/testdata/<name>.trace "<description>" [--skip-us N]
  then add <name>.trace to index.txt and run the suite: it prints the bounds.

--skip-us drops everything before an absolute log time, for a fixture that must isolate one
regime. A capture whose early minutes carry source hitches reports the hitch recovery as its
headline synth share, which buries a steady-state regression: measured on the 60x2 walk,
whole-log synth is 9.1% against 1.1% past the hitches, so a steady-state doubling would sit
inside any bound loose enough to hold the whole log. Trim only when another fixture already
covers the regime being cut; stall recovery lives in the map-cycle fixtures.
"""
import re, sys, os

CAP  = re.compile(r"capture #\d+ arr=(\d+)us")
PRE  = re.compile(r"temporal dl=(-?\d+)us")
# lag= is optional: logs recorded before it existed still carry usable scanout times, they
# just cannot say when the relay could first have known them.
FLIP = re.compile(r"flip disp=(-?\d+)us evt=(-?\d+)us(?: lag=(-?\d+)us)? head=(\d+)")
WARMUP = 200          # must match the test: skips the lock's cold-start acquisition

def main():
    argv = [a for a in sys.argv[1:]]
    skip_us = 0
    if "--skip-us" in argv:
        i = argv.index("--skip-us")
        skip_us = int(argv[i + 1])
        del argv[i:i + 2]
    if len(argv) < 2:
        print(__doc__); return 2
    src, out = argv[0], argv[1]
    desc = argv[2] if len(argv) > 2 else os.path.basename(src)
    arr, pres, synth = [], [], []
    # Head 0 only: head 1 is the relay's own output, which no pairing rule reads. Kept in
    # DELIVERY order rather than sorted by scanout time, because the replay has to model
    # when each flip became knowable, and sorting would erase that.
    flips, delays = [], []
    have_lag = True
    for line in open(src, errors="replace"):
        m = CAP.search(line)
        if m:
            v = int(m.group(1))
            if v >= skip_us: arr.append(v)
            continue
        m = PRE.search(line)
        if m:
            v = int(m.group(1))
            if v >= skip_us:
                pres.append(v)
                synth.append("op=synth" in line)
            continue
        m = FLIP.search(line)
        if m and m.group(4) == "0":
            disp, evt, lag = int(m.group(1)), int(m.group(2)), m.group(3)
            if disp < skip_us:
                continue
            flips.append(disp)
            if lag is None:
                have_lag = False
            else:
                # When the relay could FIRST have known this flip, relative to the flip
                # itself. Drives the coherence rule offline: an upgrade that arrives after
                # the policy already bracketed the slot has to be declined, and that is
                # only testable if the fixture records arrival as well as occurrence.
                delays.append(int(evt) + int(lag) - disp)
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
        if flips:
            f.write("# Head-0 scanout times in delivery order, and how long after each flip\n")
            f.write("# the relay could first have known it. A fixture without flips_h0_delay\n")
            f.write("# predates the lag= field and can drive pairing but not the timing rule.\n")
            f.write(f"flips_h0 {len(flips)}\n{enc(flips)}\n")
            if have_lag and len(delays) == len(flips):
                f.write(f"flips_h0_delay {len(delays)}\n{' '.join(str(d) for d in delays)}\n")
    print(f"{out}: {len(arr)} arrivals, {len(pres)} presents, "
          f"{len(flips)} head-0 flips{'' if have_lag and flips else ' (no lag= in log)'}, "
          f"{os.path.getsize(out)/1e6:.2f} MB")
    print(f"  field behaviour past warmup {WARMUP}: synth {pct:.1f}%, "
          f"runs>=50 {longRuns}, worst {worst}")
    return 0

sys.exit(main())
