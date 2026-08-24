#!/bin/bash

set -uo pipefail

# Run from the gem5 repository root.
# Traffic types: uniform_random, bit_reverse, hotspot_multi, transpose

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_DIR="data/extradata"
mkdir -p "$OUTPUT_DIR"

# ===== Simulation Parameters =====
VCS_PER_VNET=4
ROUTING_ALGS=(4 6 7 8)
declare -A ROUTING_NAMES=(
    [4]="UHAF"
    [5]="MTR"
    [6]="RC"
    [7]="IPDR"
    [8]="LBDR"
)

STALL_THRESHOLD=20
SYNTHETIC=bit_reverse
SIM_CYCLES=100000
NUM_CHIPLETS=4
CHIPLET_MESH_ROWS=2
CHIPLET_MESH_COLS=2
NUM_CPUS=64
NUM_DIRS=64
TOPOLOGY=Chiplet2_5D

# UHAF health-score sensitivity sweep.  The baseline algorithms do not use
# this health score, so they remain at 3 bits to avoid redundant runs.
UHAF_HEALTH_BITS=(2 3 4)
BASELINE_HEALTH_BITS=3

# Injection Rate Sweep
start=0.025
step=0.002
end=0.025

# Alpha Sweep
ALPHA_START=0.6
ALPHA_STEP=0.1
ALPHA_END=0.6

# Bias Sweep: "severe moderate"
BIAS_PAIRS=(
    "1 2"
)

STATS_FILE="${OUTPUT_DIR}/routing_sweep_${NUM_CHIPLETS}chiplet_${SYNTHETIC}_${TIMESTAMP}.txt"
DEADLOCK_LOG="${OUTPUT_DIR}/deadlock_${NUM_CHIPLETS}chiplet_${SYNTHETIC}_${TIMESTAMP}.log"

cat > "$STATS_FILE" <<EOF
===== Simulation Config =====
timestamp: ${TIMESTAMP}
topology: ${TOPOLOGY}
num_chiplets: ${NUM_CHIPLETS}
chiplet_mesh: ${CHIPLET_MESH_ROWS}x${CHIPLET_MESH_COLS}
num_cpus: ${NUM_CPUS}
num_dirs: ${NUM_DIRS}
vcs_per_vnet: ${VCS_PER_VNET}
routing_algorithms: 4(UHAF) 6(RC) 7(IPDR) 8(LBDR)
stall_threshold: ${STALL_THRESHOLD}
synthetic: ${SYNTHETIC}
sim_cycles: ${SIM_CYCLES}
uhaf_health_bits: ${UHAF_HEALTH_BITS[*]}
baseline_health_bits: ${BASELINE_HEALTH_BITS}
bias_pairs: (1,2)
alpha_sweep: ${ALPHA_START} ~ ${ALPHA_END} (step=${ALPHA_STEP})
injection_rate: ${start} ~ ${end} (step=${step})
=============================
EOF

# Outer loop: Routing Algorithm Sweep
for ROUTING_ALG in "${ROUTING_ALGS[@]}"
do
    ROUTING_NAME=${ROUTING_NAMES[$ROUTING_ALG]}

    echo "===========================================================" >> "$STATS_FILE"
    echo ">>> ROUTING: ${ROUTING_ALG} (${ROUTING_NAME})" >> "$STATS_FILE"
    echo "===========================================================" >> "$STATS_FILE"

    if (( ROUTING_ALG == 4 )); then
        CURRENT_HEALTH_BITS=("${UHAF_HEALTH_BITS[@]}")
    else
        CURRENT_HEALTH_BITS=("${BASELINE_HEALTH_BITS}")
    fi

    for HEALTH_BITS in "${CURRENT_HEALTH_BITS[@]}"
    do
        echo ">>> HEALTH_BITS: ${HEALTH_BITS}" >> "$STATS_FILE"

        for bias_pair in "${BIAS_PAIRS[@]}"
        do
            read -r SEVERE_BIAS MODERATE_BIAS <<< "$bias_pair"
            echo ">>> BIAS: (${SEVERE_BIAS},${MODERATE_BIAS})" >> "$STATS_FILE"

            for ALPHA in $(seq "$ALPHA_START" "$ALPHA_STEP" "$ALPHA_END")
            do
                echo ">>> ALPHA: ${ALPHA}" >> "$STATS_FILE"

                for rate in $(seq "$start" "$step" "$end")
                do
                    rm -f m5out/deadlock.log m5out/stats.txt

                    RUN_LOG="${OUTPUT_DIR}/${NUM_CHIPLETS}chiplet_${SYNTHETIC}_alg${ROUTING_ALG}_${ROUTING_NAME}_hb${HEALTH_BITS}_b${SEVERE_BIAS}${MODERATE_BIAS}_alpha${ALPHA}_rate${rate}.log"

                    echo "routing:${ROUTING_ALG}(${ROUTING_NAME}) | health_bits:${HEALTH_BITS} | bias:(${SEVERE_BIAS},${MODERATE_BIAS}) | alpha:${ALPHA} | injectionrate:${rate}" | tee -a "$STATS_FILE"

                    if ! ./build/Garnet_standalone/gem5.opt --dot-config= \
                    configs/example/garnet_synth_traffic.py \
                    --num-cpus=${NUM_CPUS} \
                    --num-dirs=${NUM_DIRS} \
                    --network=garnet \
                    --topology=${TOPOLOGY} \
                    --num-chiplets=${NUM_CHIPLETS} \
                    --chiplet-mesh-rows=${CHIPLET_MESH_ROWS} \
                    --chiplet-mesh-cols=${CHIPLET_MESH_COLS} \
                    --sim-cycles=${SIM_CYCLES} \
                    --synthetic=${SYNTHETIC} \
                    --inj-vnet=-1 \
                    --injectionrate=${rate} \
                    --vcs-per-vnet=${VCS_PER_VNET} \
                    --routing-algorithm=${ROUTING_ALG} \
                    --interposer-stall-threshold=${STALL_THRESHOLD} \
                    --health-score-bits=${HEALTH_BITS} \
                    --health-severe-bias=${SEVERE_BIAS} \
                    --health-moderate-bias=${MODERATE_BIAS} \
                    --health-monitor-alpha=${ALPHA} 2>&1 | tee "$RUN_LOG"
                    then
                        echo "simulation_status:FAILED (see ${RUN_LOG})" | tee -a "$STATS_FILE"
                        echo "___________________________________________________________" >> "$STATS_FILE"
                        continue
                    fi

                    if [ ! -s m5out/stats.txt ]; then
                        echo "simulation_status:FAILED (m5out/stats.txt missing)" | tee -a "$STATS_FILE"
                        echo "___________________________________________________________" >> "$STATS_FILE"
                        continue
                    fi

                    echo "simulation_status:OK" >> "$STATS_FILE"

                    if [ -f m5out/deadlock.log ]; then
                        echo "=== routing=${ROUTING_ALG}(${ROUTING_NAME}) health_bits=${HEALTH_BITS} bias=(${SEVERE_BIAS},${MODERATE_BIAS}) alpha=${ALPHA} injectionrate=${rate} ===" >> "$DEADLOCK_LOG"
                        cat m5out/deadlock.log >> "$DEADLOCK_LOG"
                        deadlock_count=$(grep -c "DEADLOCK DETECTED" m5out/deadlock.log || true)
                        echo "deadlock_count:${deadlock_count}" >> "$STATS_FILE"
                    fi

                    grep "packets_injected::total" m5out/stats.txt          >> "$STATS_FILE" || true
                    grep "packets_received::total" m5out/stats.txt          >> "$STATS_FILE" || true
                    grep "flits_injected::total" m5out/stats.txt            >> "$STATS_FILE" || true
                    grep "flits_received::total" m5out/stats.txt            >> "$STATS_FILE" || true
                    grep "average_flit_latency" m5out/stats.txt             >> "$STATS_FILE" || true
                    grep "average_flit_network_latency" m5out/stats.txt     >> "$STATS_FILE" || true
                    grep "average_flit_queueing_latency" m5out/stats.txt    >> "$STATS_FILE" || true
                    grep "average_hops" m5out/stats.txt                     >> "$STATS_FILE" || true

                    if (( ROUTING_ALG == 4 )); then
                        grep -E "health_candidate_(sets|ties|tie_rate)" \
                            m5out/stats.txt >> "$STATS_FILE" || true
                    fi
                    echo "___________________________________________________________" >> "$STATS_FILE"

                    grep "average_flit_latency" m5out/stats.txt || true
                    if (( ROUTING_ALG == 4 )); then
                        grep "health_candidate_tie_rate" \
                            m5out/stats.txt || true
                    fi
                done
            done
        done
    done
done

ln -sfn "$(basename "$STATS_FILE")" "${OUTPUT_DIR}/network_stats.txt"

echo "==================== ALL SIMULATIONS FINISHED ===================="
echo "Results: ${STATS_FILE}"
grep -E "(>>> ROUTING|>>> HEALTH_BITS|>>> BIAS|>>> ALPHA|injectionrate|simulation_status|average_flit_latency|health_candidate_)" "$STATS_FILE"
