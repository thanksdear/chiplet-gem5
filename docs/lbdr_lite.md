# LBDR-lite reproduction

This implementation reproduces the central offline binding idea in:

> Cao et al., "LBDR: A load-balanced deadlock-free routing strategy for
> chiplet systems," *Integration*, vol. 96, 2024, 102149.

It is intentionally a **simplified reproduction**, because the local
`Chiplet2_5D` topology differs from the paper. The paper searches candidate
boundary-node locations and turn restrictions. This project already fixes
four vertical links at the four corners of every 4x4 chiplet, so LBDR-lite:

1. keeps those four physical gateways;
2. computes a static, injection-weight-aware node-to-gateway binding offline;
3. routes source-to-gateway with XY, across the interposer with XY, and from
   the destination gateway to the destination with XY.

The full CDG enumeration and turn-restriction optimization in the paper are
not reproduced. Consequently, results must be labelled `LBDR-lite`; the code
does not claim a complete reproduction of the paper's deadlock proof.

## Generate a binding

For uniform injection:

```bash
python3 util/generate_lbdr_binding.py
```

For a measured injection distribution, pass the 16 source-router weights in
local-router order (row-major, routers 0 through 15):

```bash
python3 util/generate_lbdr_binding.py \
  --weights 9,4,3,2,8,5,2,1,7,4,2,1,6,3,2,1
```

The first output line can be copied directly into a gem5 command. Gateway
indices are `0=top-left`, `1=top-right`, `2=bottom-left`, and
`3=bottom-right`. One 16-entry map is repeated for all four chiplets.

## Run

Use the existing Chiplet2_5D command and select routing algorithm 8:

```bash
build/NULL/gem5.opt configs/example/garnet_synth_traffic.py \
  --network=garnet --topology=Chiplet2_5D \
  --num-cpus=64 --num-dirs=64 \
  --routing-algorithm=8 \
  --lbdr-gateway-map=0,0,1,1,0,0,1,1,2,2,3,3,2,2,3,3
```

The default map is the equal-size nearest-corner partition. To expose LBDR's
benefit, generate the map from a non-uniform traffic profile and compare:

- algorithm 3: nearest-corner Chiplet XY;
- algorithm 5: the existing MTR baseline;
- algorithm 8: LBDR-lite with the generated binding.

Keep topology, seed, traffic trace/pattern, injection rate, VC count, buffer
depth, and simulation duration identical. Report at least average packet
latency, throughput/received flits, and per-Down-link traffic. The last metric
directly tests whether the offline binding balances the vertical-link load.
