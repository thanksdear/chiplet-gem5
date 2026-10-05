# UHAF deterministic deadlock validation

The `deadlock_ring` workload is an opt-in test.  With the option disabled,
normal Garnet traffic and routing do not use the scripted route or formation
barrier.

## Scenario

Four groups of five-flit vnet-2 packets are injected in reverse dependency
order.  Each group contains `vcs_per_vnet` packets, so all data VCs of the
four resources are occupied.  The resulting dependency cycle is:

```text
R0.Down -> IR0.East -> IR1.Up -> R1.West -> R0.Down
```

The formation barrier only synchronizes the packet heads.  It is permanently
removed when all `4 * vcs_per_vnet` heads have reached their wait points.
Subsequent blocking is produced by ordinary Garnet VC allocation and credit
flow control.

## Server command

```bash
build/Garnet_standalone/gem5.opt \
  configs/example/garnet_synth_traffic.py \
  --network=garnet \
  --topology=Chiplet2_5D \
  --num-cpus=64 \
  --num-dirs=64 \
  --num-chiplets=4 \
  --chiplet-mesh-rows=2 \
  --chiplet-mesh-cols=2 \
  --routing-algorithm=4 \
  --synthetic=deadlock_ring \
  --vcs-per-vnet=4 \
  --interposer-stall-threshold=100 \
  --escape-buffer-depth=5 \
  --sim-cycles=3000
```

The test automatically selects vnet 2, injection rate 1.0, four participating
sources, and `vcs_per_vnet` packets per source.

## Evidence and pass criteria

`m5out/deadlock_injection.log` must contain:

- 16 `DEADLOCK STAGE` records for four VCs;
- one `DEADLOCK BARRIER RELEASED` record;
- the explicit `VC DEPENDENCY CYCLE` record.

`m5out/deadlock.log` must contain:

- `DEADLOCK DETECTED` and a non-negative `detection_latency_cycles`;
- `ESCAPE ABSORB START` and `ESCAPE ABSORB COMPLETE`;
- `ESCAPE REINJECTED`;
- `RECOVERED PACKET DELIVERED`.

The run fails immediately if an escape buffer contains missing, duplicated,
or out-of-order flits, or if the configured escape depth cannot hold the
five-flit data packet.  Reinjection only uses an IDLE VC; it never overwrites
an ACTIVE VC.

For a no-injection regression, run the original workload without
`--synthetic=deadlock_ring`.  Both deadlock-test log files should remain free
of formation records.
