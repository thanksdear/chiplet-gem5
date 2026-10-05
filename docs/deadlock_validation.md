# UHAF deterministic deadlock validation

The `deadlock_ring` workload is an opt-in test.  With the option disabled,
normal Garnet traffic and routing do not use the scripted route or formation
barrier.

## Scenario

Six groups of five-flit vnet-2 packets are injected in path-dependency order
(`R1`, `R0`, `R2`, `R3`, `R4`, then `R5`).  Each group contains
four packets and occupies the four normal VCs of one directed channel while
requesting the next occupied channel.  A fifth VC is reserved end-to-end for
escape traffic.  The resulting closed dependency cycle is:

```text
R0.Down -> IR0.East -> IR1.Up -> R3.West -> R2.West -> R1.West -> R0.Down
```

The formation barrier only synchronizes the packet heads.  It is permanently
removed when all 24 normal-VC heads have reached their wait points.
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
  --vcs-per-vnet=5 \
  --buffers-per-data-vc=5 \
  --interposer-stall-threshold=100 \
  --escape-buffer-depth=5 \
  --health-propagation-cycles=1 \
  --sim-cycles=3000
```

The test automatically selects vnet 2, injection rate 1.0, six participating
sources, and `vcs_per_vnet - 1` packets per source.  Only sources R0 through
R5 participate in this workload.  In test mode the final vnet-2 VC is
reserved for recovered packets; ordinary packets cannot allocate it.
Each data VC must have at least five slots so a complete five-flit packet can
be staged without leaving a tail flit in an upstream dependency resource.

## Evidence and pass criteria

`m5out/deadlock_injection.log` must contain:

- 24 `DEADLOCK STAGE` records (six channels with four VCs each);
- one `DEADLOCK BARRIER RELEASED` record;
- the explicit `VC DEPENDENCY CYCLE` record.

`m5out/deadlock.log` must contain:

- `DEADLOCK DETECTED` and a non-negative `detection_latency_cycles`;
- `HEALTH SCORE RECEIVED` after the configured registered sideband delay;
- `ESCAPE ABSORB START` and `ESCAPE ABSORB COMPLETE`;
- `ESCAPE REINJECTED`;
- `RECOVERED PACKET DELIVERED`;
- `DEADLOCK FULL RECOVERY COMPLETE` and `recovery_latency_cycles`.

The escape buffer is single-ported in the timing model: absorption and local
reinjection each transfer at most one flit per cycle.  Health scores become
visible to a peer only after `health_propagation_cycles` registered sideband
cycles.  Consequently, with a 100-cycle stall threshold and the default
one-cycle registered propagation, detection cannot occur earlier than 101
cycles after cycle formation.  A five-flit packet also requires at least five
cycles for absorption and five cycles for local reinjection.

The final statistics must also report all 24 packets (120 flits) injected and
received.  This guards against declaring success after only a partial
recovery.

The run fails immediately if an escape buffer contains missing, duplicated,
or out-of-order flits, or if the configured escape depth cannot hold the
five-flit data packet.  Reinjection only uses an IDLE VC; it never overwrites
an ACTIVE VC.

For a no-injection regression, run the original workload without
`--synthetic=deadlock_ring`.  Both deadlock-test log files should remain free
of formation records.
