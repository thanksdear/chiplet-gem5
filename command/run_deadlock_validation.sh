#!/bin/bash

set -uo pipefail

# Run this script from the gem5 repository root:
#   bash command/run_deadlock_validation.sh
#
# This is a deterministic, single-run validation for the opt-in UHAF
# deadlock_ring workload. It does not perform a traffic-rate sweep.

GEM5_BIN="./build/Garnet_standalone/gem5.opt"
CONFIG="configs/example/garnet_synth_traffic.py"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="data/deadlock/deadlock_validation_${TIMESTAMP}"

# ===== Reproducible simulation configuration =====
TOPOLOGY="Chiplet2_5D"
NUM_CHIPLETS=4
CHIPLET_MESH_ROWS=2
CHIPLET_MESH_COLS=2
NUM_CPUS=64
NUM_DIRS=64

ROUTING_ALGORITHM=4
SYNTHETIC="deadlock_ring"
VCS_PER_VNET=4
BUFFERS_PER_DATA_VC=5
SIM_CYCLES=3000
STALL_THRESHOLD=100
GARNET_DEADLOCK_THRESHOLD=50000
ESCAPE_BUFFER_DEPTH=5

# Explicitly record the UHAF settings instead of relying on parser defaults.
HEALTH_BITS=3
UP_HEALTH_MONITOR=1
HEALTH_ALPHA=0.5
HEALTH_SEVERE_BIAS=1
HEALTH_MODERATE_BIAS=2

if [[ ! -f "${CONFIG}" || ! -d "src/mem/ruby/network/garnet" ]]; then
    echo "ERROR: run this script from the gem5 repository root." >&2
    exit 2
fi

if [[ ! -x "${GEM5_BIN}" ]]; then
    echo "ERROR: ${GEM5_BIN} does not exist or is not executable." >&2
    echo "Build it first with:" >&2
    echo "  scons build/Garnet_standalone/gem5.opt -j\$(nproc)" >&2
    exit 2
fi

mkdir -p "${RESULT_DIR}" m5out

# The implementation writes these validation logs directly under m5out.
# Remove old files so records from different runs cannot be mixed.
rm -f m5out/deadlock.log \
      m5out/deadlock_injection.log \
      m5out/stats.txt

RUN_LOG="${RESULT_DIR}/gem5.log"
SUMMARY_FILE="${RESULT_DIR}/summary.txt"
CONFIG_FILE="${RESULT_DIR}/run_config.txt"

GIT_BRANCH=$(git branch --show-current 2>/dev/null || true)
GIT_COMMIT=$(git rev-parse HEAD 2>/dev/null || true)

cat > "${CONFIG_FILE}" <<EOF
timestamp: ${TIMESTAMP}
git_branch: ${GIT_BRANCH:-unknown}
git_commit: ${GIT_COMMIT:-unknown}
workload: deterministic (random seed not applicable)
topology: ${TOPOLOGY}
num_chiplets: ${NUM_CHIPLETS}
chiplet_mesh: ${CHIPLET_MESH_ROWS}x${CHIPLET_MESH_COLS}
num_cpus: ${NUM_CPUS}
num_dirs: ${NUM_DIRS}
routing_algorithm: ${ROUTING_ALGORITHM} (UHAF)
synthetic: ${SYNTHETIC}
vcs_per_vnet: ${VCS_PER_VNET}
buffers_per_data_vc: ${BUFFERS_PER_DATA_VC}
injection_vnet: 2 (forced by deadlock_ring)
injection_rate: 1.0 (forced by deadlock_ring)
packets_per_participating_source: ${VCS_PER_VNET}
sim_cycles: ${SIM_CYCLES}
interposer_stall_threshold: ${STALL_THRESHOLD}
garnet_deadlock_threshold: ${GARNET_DEADLOCK_THRESHOLD}
escape_buffer_depth: ${ESCAPE_BUFFER_DEPTH}
health_score_bits: ${HEALTH_BITS}
up_health_monitor: ${UP_HEALTH_MONITOR}
health_monitor_alpha: ${HEALTH_ALPHA}
health_severe_bias: ${HEALTH_SEVERE_BIAS}
health_moderate_bias: ${HEALTH_MODERATE_BIAS}
EOF

echo "Running deterministic UHAF deadlock validation"
echo "Results will be stored in ${RESULT_DIR}"

"${GEM5_BIN}" \
    "${CONFIG}" \
    --network=garnet \
    --topology="${TOPOLOGY}" \
    --num-cpus="${NUM_CPUS}" \
    --num-dirs="${NUM_DIRS}" \
    --num-chiplets="${NUM_CHIPLETS}" \
    --chiplet-mesh-rows="${CHIPLET_MESH_ROWS}" \
    --chiplet-mesh-cols="${CHIPLET_MESH_COLS}" \
    --routing-algorithm="${ROUTING_ALGORITHM}" \
    --synthetic="${SYNTHETIC}" \
    --vcs-per-vnet="${VCS_PER_VNET}" \
    --buffers-per-data-vc="${BUFFERS_PER_DATA_VC}" \
    --sim-cycles="${SIM_CYCLES}" \
    --interposer-stall-threshold="${STALL_THRESHOLD}" \
    --garnet-deadlock-threshold="${GARNET_DEADLOCK_THRESHOLD}" \
    --escape-buffer-depth="${ESCAPE_BUFFER_DEPTH}" \
    --health-score-bits="${HEALTH_BITS}" \
    --up-health-monitor="${UP_HEALTH_MONITOR}" \
    --health-monitor-alpha="${HEALTH_ALPHA}" \
    --health-severe-bias="${HEALTH_SEVERE_BIAS}" \
    --health-moderate-bias="${HEALTH_MODERATE_BIAS}" \
    2>&1 | tee "${RUN_LOG}"

GEM5_STATUS=${PIPESTATUS[0]}

# Preserve all evidence before evaluating the result.
for artifact in \
    stats.txt \
    deadlock.log \
    deadlock_injection.log \
    config.ini \
    config.json
do
    if [[ -f "m5out/${artifact}" ]]; then
        cp "m5out/${artifact}" "${RESULT_DIR}/${artifact}"
    fi
done

count_record()
{
    local pattern=$1
    local file=$2

    if [[ -f "${file}" ]]; then
        grep -c "${pattern}" "${file}" 2>/dev/null || true
    else
        echo 0
    fi
}

EXPECTED_STAGES=$((4 * VCS_PER_VNET))
STAGE_COUNT=$(count_record "DEADLOCK STAGE" m5out/deadlock_injection.log)
BARRIER_COUNT=$(count_record "DEADLOCK BARRIER RELEASED" m5out/deadlock_injection.log)
CYCLE_COUNT=$(count_record "VC DEPENDENCY CYCLE" m5out/deadlock_injection.log)
DETECTED_COUNT=$(count_record "DEADLOCK DETECTED" m5out/deadlock.log)
LATENCY_COUNT=$(count_record "detection_latency_cycles=" m5out/deadlock.log)
ABSORB_START_COUNT=$(count_record "ESCAPE ABSORB START" m5out/deadlock.log)
ABSORB_COMPLETE_COUNT=$(count_record "ESCAPE ABSORB COMPLETE" m5out/deadlock.log)
REINJECTED_COUNT=$(count_record "ESCAPE REINJECTED" m5out/deadlock.log)
DELIVERED_COUNT=$(count_record "RECOVERED PACKET DELIVERED" m5out/deadlock.log)

VALIDATION_STATUS="FAIL"
if (( GEM5_STATUS == 0 &&
      STAGE_COUNT == EXPECTED_STAGES &&
      BARRIER_COUNT == 1 &&
      CYCLE_COUNT == 1 &&
      DETECTED_COUNT >= 1 &&
      LATENCY_COUNT >= 1 &&
      ABSORB_START_COUNT >= 1 &&
      ABSORB_COMPLETE_COUNT >= 1 &&
      REINJECTED_COUNT >= 1 &&
      DELIVERED_COUNT >= 1 )); then
    VALIDATION_STATUS="PASS"
fi

cat > "${SUMMARY_FILE}" <<EOF
UHAF deterministic deadlock validation: ${VALIDATION_STATUS}
gem5_exit_status: ${GEM5_STATUS}
expected_deadlock_stages: ${EXPECTED_STAGES}
deadlock_stage_count: ${STAGE_COUNT}
barrier_release_count: ${BARRIER_COUNT}
dependency_cycle_count: ${CYCLE_COUNT}
deadlock_detected_count: ${DETECTED_COUNT}
detection_latency_record_count: ${LATENCY_COUNT}
escape_absorb_start_count: ${ABSORB_START_COUNT}
escape_absorb_complete_count: ${ABSORB_COMPLETE_COUNT}
escape_reinjected_count: ${REINJECTED_COUNT}
recovered_packet_delivered_count: ${DELIVERED_COUNT}
EOF

if [[ -f m5out/deadlock.log ]]; then
    {
        echo
        echo "Detection latency records:"
        grep "detection_latency_cycles=" m5out/deadlock.log || true
    } >> "${SUMMARY_FILE}"
fi

echo
cat "${SUMMARY_FILE}"
echo "Evidence directory: ${RESULT_DIR}"

if [[ "${VALIDATION_STATUS}" != "PASS" ]]; then
    echo "Validation failed; inspect ${RUN_LOG} and the copied deadlock logs." >&2
    exit 1
fi

exit 0
