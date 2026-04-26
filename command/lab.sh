TIMESTAMP=$(date +%Y%m%d_%H%M%S)
STATS_FILE="network_stats_${TIMESTAMP}.txt"
#uniform_random
#tornado
#bit_complement
#bit_reverse
#bit_rotation
#neighbor
#shuffle
#transpose
#hotspot
#hotspot_single
# ===== Simulation Parameters =====
VCS_PER_VNET=4
ROUTING_ALG=6        # 0=TABLE, 1=XY, 2=CUSTOM, 3=XY_CHIPLET
STALL_THRESHOLD=50
ROUTING_OPT=False     # True=enable, False=disable
SYNTHETIC=transpose         
SIM_CYCLES=100000
NUM_CPUS=64
NUM_DIRS=64
TOPOLOGY=Chiplet2_5D
start=0.070
step=0.001
end=0.070

# Write header with parameters
cat > "$STATS_FILE" <<EOF
===== Simulation Config =====
timestamp: ${TIMESTAMP}
topology: ${TOPOLOGY}
num_cpus: ${NUM_CPUS}
num_dirs: ${NUM_DIRS}
vcs_per_vnet: ${VCS_PER_VNET}
routing_algorithm: ${ROUTING_ALG}
stall_threshold: ${STALL_THRESHOLD}
routing_optimization: ${ROUTING_OPT}
synthetic: ${SYNTHETIC}
sim_cycles: ${SIM_CYCLES}
injection_rate: ${start} ~ ${end} (step=${step})
=============================
EOF

DEADLOCK_LOG="m5out/deadlock_${TIMESTAMP}.log"
for rate in $(seq $start $step $end)
do
# Clear per-run deadlock log
rm -f m5out/deadlock.log
./build/Garnet_standalone/gem5.opt configs/example/garnet_synth_traffic.py \
--num-cpus=${NUM_CPUS} \
--num-dirs=${NUM_DIRS} \
--network=garnet \
--topology=${TOPOLOGY} \
--mesh-rows=8 \
--sim-cycles=${SIM_CYCLES} \
--synthetic=${SYNTHETIC} \
--inj-vnet=-1 \
--injectionrate=$rate \
--vcs-per-vnet=${VCS_PER_VNET} \
--routing-algorithm=${ROUTING_ALG} \
--interposer-stall-threshold=${STALL_THRESHOLD} \
--enable-routing-optimization=${ROUTING_OPT}
#2>&1 | tee interposer_vc.log
echo "injectionrate:$rate">> "$STATS_FILE"
# Append deadlock info with injection rate to unified log
if [ -f m5out/deadlock.log ]; then
    echo "=== injectionrate=$rate ===" >> "$DEADLOCK_LOG"
    cat m5out/deadlock.log >> "$DEADLOCK_LOG"
    echo "[DEADLOCK] rate=$rate" >> "$STATS_FILE"
fi
grep "packets_injected::total" m5out/stats.txt  >> "$STATS_FILE"
grep "packets_received::total" m5out/stats.txt  >> "$STATS_FILE"
grep "flits_injected::total" m5out/stats.txt  >> "$STATS_FILE"
grep "flits_received::total" m5out/stats.txt  >> "$STATS_FILE"
grep "average_flit_latency" m5out/stats.txt  >> "$STATS_FILE"
grep "average_flit_network_latency" m5out/stats.txt  >> "$STATS_FILE"
grep "average_flit_queueing_latency" m5out/stats.txt  >> "$STATS_FILE"
grep "average_hops" m5out/stats.txt  >> "$STATS_FILE"
echo "___________________________________________________________" >> "$STATS_FILE"
cat "$STATS_FILE" | grep "average_flit_latency"
done

ln -sf "$STATS_FILE" network_stats.txt
python3 ./command/plot.py
#python3 ./command/plot_interposer.py interposer_vc.log