#!/bin/bash
set -uo pipefail

# Minimal 2/3/4-bit sensitivity experiment.
# Run from the gem5 repository root after rebuilding gem5.opt.
# Optional example:
#   INJECTION_RATE=0.070 SIM_CYCLES=100000 bash command/run_health_bits_sensitivity.sh

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_DIR="data/health_bits_sensitivity"
STATS_FILE="${OUTPUT_DIR}/health_bits_${TIMESTAMP}.txt"
mkdir -p "${OUTPUT_DIR}"

VCS_PER_VNET=4
ROUTING_ALG=4
STALL_THRESHOLD=20
SYNTHETIC=${SYNTHETIC:-uniform_random}
SIM_CYCLES=${SIM_CYCLES:-100000}
INJECTION_RATE=${INJECTION_RATE:-0.070}
ALPHA=${ALPHA:-0.6}
SEVERE_BIAS=${SEVERE_BIAS:-1}
MODERATE_BIAS=${MODERATE_BIAS:-2}
NUM_CPUS=${NUM_CPUS:-64}
NUM_DIRS=${NUM_DIRS:-64}

cat > "${STATS_FILE}" <<EOF
===== Health-bit Sensitivity Experiment =====
traffic: ${SYNTHETIC}
injection_rate: ${INJECTION_RATE}
sim_cycles: ${SIM_CYCLES}
alpha: ${ALPHA}
bias_3bit_equivalent: (${SEVERE_BIAS},${MODERATE_BIAS})
metrics: candidate tie rate, gateway decision flip rate
=============================================
EOF

for HEALTH_BITS in 2 3 4; do
    RUN_LOG="${OUTPUT_DIR}/${SYNTHETIC}_hb${HEALTH_BITS}_${TIMESTAMP}.log"
    rm -f m5out/stats.txt

    ./build/Garnet_standalone/gem5.opt \
        configs/example/garnet_synth_traffic.py \
        --num-cpus=${NUM_CPUS} \
        --num-dirs=${NUM_DIRS} \
        --network=garnet \
        --topology=Chiplet2_5D \
        --mesh-rows=8 \
        --sim-cycles=${SIM_CYCLES} \
        --synthetic=${SYNTHETIC} \
        --inj-vnet=-1 \
        --injectionrate=${INJECTION_RATE} \
        --vcs-per-vnet=${VCS_PER_VNET} \
        --routing-algorithm=${ROUTING_ALG} \
        --interposer-stall-threshold=${STALL_THRESHOLD} \
        --health-score-bits=${HEALTH_BITS} \
        --health-severe-bias=${SEVERE_BIAS} \
        --health-moderate-bias=${MODERATE_BIAS} \
        --health-monitor-alpha=${ALPHA} 2>&1 | tee "${RUN_LOG}"
    GEM5_STATUS=${PIPESTATUS[0]}

    echo ">>> HEALTH_BITS: ${HEALTH_BITS}" | tee -a "${STATS_FILE}"
    if (( GEM5_STATUS != 0 )) || [[ ! -f m5out/stats.txt ]]; then
        echo "simulation_status:FAILED (see ${RUN_LOG})" | \
            tee -a "${STATS_FILE}"
        continue
    fi

    echo "simulation_status:OK" | tee -a "${STATS_FILE}"
    grep -E "average_flit_latency|health_candidate_(sets|ties|tie_rate)|health_gateway_decision_(comparisons|flips|flip_rate)" \
        m5out/stats.txt | tee -a "${STATS_FILE}"
done

ln -sf "$(basename "${STATS_FILE}")" \
    "${OUTPUT_DIR}/health_bits_latest.txt"

echo "Results: ${STATS_FILE}"
