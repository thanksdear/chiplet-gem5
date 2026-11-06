#!/usr/bin/env python3
"""Run paired IPDR/IPDR+HAR traffic experiments with isolated evidence.

This evaluates routing on the existing simplified IPDR model. It does not
validate paper-faithful IPDR packet buffering or deadlock recovery.
"""

import argparse
import csv
import hashlib
import itertools
import json
import math
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TRAFFIC = ("uniform_random", "bit_reverse", "transpose", "hotspot_multi")
METRICS = (
    "packets_injected::total", "packets_received::total",
    "flits_injected::total", "flits_received::total",
    "average_flit_latency", "average_hops", "har_decisions",
    "har_redirects", "ipdr_priority_grants", "ipdr_recovery_entries",
    "ipdr_recovery_finishes", "uhaf_escape_absorptions",
)


def read_stats(path):
    stats = {}
    if path.is_file():
        for line in path.read_text().splitlines():
            match = re.match(r"system\.ruby\.network\.(\S+)\s+(\S+)", line)
            if match:
                try:
                    stats[match[1]] = float(match[2])
                except ValueError:
                    pass
    return stats


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="new directory; existing results are never "
                             "overwritten")
    parser.add_argument("--binary", type=Path,
                        default=ROOT / "build/Garnet_standalone/gem5.opt")
    parser.add_argument("--schemes", nargs="+", default=["IPDR", "IPDR+HAR"],
                        choices=("IPDR", "IPDR+HAR", "UHAF"))
    parser.add_argument("--traffic", nargs="+", choices=TRAFFIC,
                        default=TRAFFIC)
    parser.add_argument("--rates", nargs="+", type=float,
                        default=[0.01, 0.03, 0.05])
    parser.add_argument("--vcs", nargs="+", type=int, default=[4])
    parser.add_argument("--seeds", nargs="+", type=int, default=[3000])
    parser.add_argument("--sim-cycles", type=int, default=100000)
    parser.add_argument("--packets-per-source", type=int, default=-1,
                        help="finite delivery check if positive; "
                             "-1 for continuous traffic")
    parser.add_argument("--stall-threshold", type=int, default=20)
    parser.add_argument("--alpha", type=float, default=0.6)
    parser.add_argument("--health-bits", type=int, choices=(2, 3, 4),
                        default=3)
    parser.add_argument("--num-chiplets", type=int, default=4)
    parser.add_argument("--mesh-rows", type=int, default=2)
    parser.add_argument("--mesh-cols", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    if (any(not math.isfinite(rate) or not 0 < rate <= 1
            for rate in args.rates)
            or any(vc < 1 for vc in args.vcs)
            or any(seed < 0 for seed in args.seeds)
            or not 0 <= args.alpha <= 1
            or min(args.num_chiplets, args.mesh_rows, args.mesh_cols,
                   args.sim_cycles, args.stall_threshold, args.timeout) < 1
            or args.mesh_rows * args.mesh_cols != args.num_chiplets
            or args.packets_per_source == 0 or args.packets_per_source < -1):
        parser.error("invalid rate, VC count, seed, timing, alpha, "
                     "or topology")
    args.binary = args.binary.resolve()
    if not args.binary.is_file():
        parser.error(f"missing binary: {args.binary}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sources = sorted((ROOT / "src/mem/ruby/network/garnet").glob("*.cc"))
    sources += sorted((ROOT / "src/mem/ruby/network/garnet").glob("*.hh"))
    sources += [ROOT / p for p in (
        "src/mem/ruby/network/garnet/GarnetNetwork.py",
        "configs/network/Network.py",
        "configs/example/garnet_synth_traffic.py",
        "configs/topologies/Chiplet2_5D.py",
        "command/run_ipdr_har_comparison.py")]
    manifest = {
        "binary": str(args.binary), "binary_sha256": sha256(args.binary),
        "source_sha256": {str(p.relative_to(ROOT)): sha256(p)
                          for p in sources},
        "settings": {k: str(v) if isinstance(v, Path) else v
                     for k, v in vars(args).items()},
        "scope": ("routing comparison on simplified IPDR; "
                  "no forced-deadlock claim"),
        "runs": [],
    }
    manifest_path = output / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    fields = ["routing", "traffic", "injection_rate", "vcs_per_vnet",
              "seed", "avg_latency", "status", "run_dir"] + list(METRICS)
    all_passed = True
    with (output / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        jobs = itertools.product(dict.fromkeys(args.vcs),
                                 dict.fromkeys(args.traffic),
                                 dict.fromkeys(args.rates),
                                 dict.fromkeys(args.seeds),
                                 dict.fromkeys(args.schemes))
        for vc, traffic, rate, seed, scheme in jobs:
            name = (f"{scheme.replace('+', '_')}_{traffic}_vc{vc}"
                    f"_rate{rate:g}_seed{seed}")
            run = output / name
            run.mkdir()
            cmd = [str(args.binary), "--dot-config=", "--outdir=m5out",
                   str(ROOT / "configs/example/garnet_synth_traffic.py"),
                   f"--num-cpus={args.num_chiplets * 16}",
                   f"--num-dirs={args.num_chiplets * 16}",
                   "--network=garnet", "--topology=Chiplet2_5D",
                   f"--num-chiplets={args.num_chiplets}",
                   f"--chiplet-mesh-rows={args.mesh_rows}",
                   f"--chiplet-mesh-cols={args.mesh_cols}",
                   f"--sim-cycles={args.sim_cycles}",
                   f"--synthetic={traffic}", f"--injectionrate={rate:g}",
                   "--precision=6", "--inj-vnet=-1", f"--vcs-per-vnet={vc}",
                   f"--routing-algorithm={4 if scheme == 'UHAF' else 7}",
                   f"--random-seed={seed}",
                   f"--interposer-stall-threshold={args.stall_threshold}",
                   f"--health-monitor-alpha={args.alpha}",
                   f"--health-score-bits={args.health_bits}",
                   f"--num-packets-max={args.packets_per_source}"]
            if scheme == "IPDR+HAR":
                cmd.append("--ipdr-har")
            (run / "command.json").write_text(json.dumps(cmd, indent=2) + "\n")
            problems = []
            with (run / "gem5.log").open("w") as log:
                try:
                    result = subprocess.run(cmd, cwd=run, stdout=log,
                                            stderr=subprocess.STDOUT,
                                            timeout=args.timeout)
                    if result.returncode:
                        problems.append(f"simulator exit {result.returncode}")
                except subprocess.TimeoutExpired:
                    problems.append("wall-clock timeout")
            stats = read_stats(run / "m5out/stats.txt")
            if any(key not in stats for key in METRICS):
                problems.append("missing required statistics")
            latency = stats.get("average_flit_latency", math.nan)
            if not math.isfinite(latency) or latency <= 0:
                problems.append("no finite positive latency sample")
            for unit in ("packets", "flits"):
                received = stats.get(f"{unit}_received::total", 0)
                injected = stats.get(f"{unit}_injected::total", 0)
                if received <= 0 or received > injected:
                    problems.append(f"invalid {unit} delivery counts")
                if args.packets_per_source > 0:
                    if received != injected:
                        problems.append(f"finite run did not drain all {unit}")
            if args.packets_per_source > 0:
                expected = args.num_chiplets * 16 * args.packets_per_source
                if stats.get("packets_injected::total") != expected:
                    problems.append("finite run did not inject all "
                                    "requested packets")
            if scheme.startswith("IPDR"):
                if stats.get("uhaf_escape_absorptions", -1) != 0:
                    problems.append("IPDR used UHAF escape recovery")
                deadlock_log = run / "m5out/deadlock.log"
                if (deadlock_log.exists() and
                        "[DEADLOCK RECOVERY]" in deadlock_log.read_text()):
                    problems.append("IPDR entered UHAF recovery branch")
            if scheme == "IPDR" and stats.get("har_decisions", -1) != 0:
                problems.append("plain IPDR unexpectedly used HAR")
            # Check the instantiated configuration as well as requested flags.
            config = run / "m5out/config.ini"
            if config.exists():
                actual = config.read_text()
                expected_flag = "true" if scheme == "IPDR+HAR" else "false"
                if f"ipdr_har_enabled={expected_flag}" not in actual:
                    problems.append("instantiated HAR flag differs "
                                    "from request")
            else:
                problems.append("missing instantiated configuration")
            status = "FAIL" if problems else "PASS"
            row = dict(routing=scheme, traffic=traffic, injection_rate=rate,
                       vcs_per_vnet=vc, seed=seed,
                       avg_latency=latency if math.isfinite(latency) else "",
                       status=status, run_dir=name)
            row.update({key: stats.get(key, "") for key in METRICS})
            writer.writerow(row)
            stream.flush()
            manifest["runs"].append(dict(name=name, status=status,
                                         problems=problems))
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
            all_passed = all_passed and not problems
            print(f"{status}: {name}; "
                  f"HAR redirects={stats.get('har_redirects')}; "
                  f"IPDR priority grants={stats.get('ipdr_priority_grants')}",
                  flush=True)
            if problems:
                print("  " + "; ".join(problems), flush=True)
    unchanged = all(sha256(ROOT / p) == digest
                    for p, digest in manifest["source_sha256"].items())
    unchanged = unchanged and sha256(args.binary) == manifest["binary_sha256"]
    manifest["inputs_unchanged"] = unchanged
    manifest["passed"] = all_passed and unchanged
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return 0 if manifest["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
