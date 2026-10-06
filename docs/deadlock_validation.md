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
sources per ring, and `vcs_per_vnet - 1` packets per source.  The single-ring
case uses R0 through R5; each additional ring uses the corresponding local
R0-through-R5 positions of another chiplet.  In test mode the final vnet-2 VC
is reserved for recovered packets; ordinary packets cannot allocate it.
Each data VC must have at least five slots so a complete five-flit packet can
be staged without leaving a tail flit in an upstream dependency resource.

### Simultaneous independent rings

The same local cross-layer ring can be instantiated on multiple chiplets at
the same simulated time.  The default command above uses one ring.  Use the
multi-ring wrapper for two or four simultaneous rings:

```bash
bash command/run_multi_deadlock_validation.sh 2
bash command/run_multi_deadlock_validation.sh 4
```

Each ring has six participating sources and four blocked data VCs per source.
Thus the 2-ring case injects 48 packets (240 flits), while the 4-ring case
injects 96 packets (480 flits).  A single global formation barrier releases
all staged heads together, but each ring has its own dependency-cycle record,
detector, and escape-buffer recovery event.

### Partial-packet recovery

Run the dedicated scenario with:

```bash
bash command/run_partial_packet_validation.sh
```

The victim source releases each packet head normally but holds its four body
and tail flits in the source NI until after the dependency cycle has formed
and UHAF has detected it.  Thus the recovery input VC contains fewer than the
five packet flits when absorption starts.  Returning the head credit allows
the upstream body and tail to advance, and the single-ported escape buffer
must wait for and absorb flit IDs 0 through 4 in order.

The validation fails unless the injection log proves that all expected body
and tail flits were held, the absorb-start record reports
`initial_available_flits < packet_flits`, at least one empty absorb-wait cycle
occurs, five ordered absorb-flit records are present, and every packet and
flit is ultimately delivered.

## Evidence and pass criteria

For `N` configured simultaneous rings,
`m5out/deadlock_injection.log` must contain:

- `24*N` `DEADLOCK STAGE` records (six channels with four VCs per ring);
- one `DEADLOCK BARRIER RELEASED` record;
- `N` explicit `VC DEPENDENCY CYCLE` records.

`m5out/deadlock.log` must contain:

- `DEADLOCK DETECTED` and a non-negative `detection_latency_cycles`;
- `HEALTH SCORE RECEIVED` after the configured registered sideband delay;
- `ESCAPE ABSORB START` and `ESCAPE ABSORB COMPLETE`;
- `ESCAPE REINJECTED`;
- `RECOVERED PACKET DELIVERED`;
- `DEADLOCK FULL RECOVERY COMPLETE` and `recovery_latency_cycles`.
- `ESCAPE BUFFER CONTENTION`, including the number of eligible blocked VCs,
  the selected victim VC, and the deferred VC count.

The escape buffer is single-ported in the timing model: absorption and local
reinjection each transfer at most one flit per cycle.  Health scores become
visible to a peer only after
`shortest_path_hops * health_propagation_cycles` registered sideband cycles.
The four gateways belonging to one chiplet form a 2x2 square, so a peer is
one hop away when adjacent and two hops away when diagonal.  Consequently,
with a 100-cycle stall threshold and the default one-cycle-per-hop setting,
detection cannot occur earlier than 101 cycles for an adjacent detector or
102 cycles for a diagonal detector.  A five-flit packet also requires at
least five cycles for absorption and five cycles for local reinjection.

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
