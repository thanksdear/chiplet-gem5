echo "Running Simulation..." > network_stats.txt
start=0.055
step=0.001
end=0.055
DEADLOCK_LOG="m5out/deadlock_$(date +%Y%m%d_%H%M%S).log"
for rate in $(seq $start $step $end)
do
# Clear per-run deadlock log
rm -f m5out/deadlock.log
./build/Garnet_standalone/gem5.debug configs/example/garnet_synth_traffic.py \
--num-cpus=64 \
--num-dirs=64 \
--network=garnet \
--topology=Chiplet2_5D \
--mesh-rows=8 \
--sim-cycles=100000 \
--synthetic=uniform_random \
--inj-vnet=-1 \
--injectionrate=$rate \
--vcs-per-vnet=8 \
--routing-algorithm=2 \
#2>&1 | tee interposer_vc.log
echo "injectionrate:$rate">> network_stats.txt
# Append deadlock info with injection rate to unified log
if [ -f m5out/deadlock.log ]; then
    echo "=== injectionrate=$rate ===" >> "$DEADLOCK_LOG"
    cat m5out/deadlock.log >> "$DEADLOCK_LOG"
    echo "[DEADLOCK] rate=$rate" >> network_stats.txt
fi
grep "packets_injected::total" m5out/stats.txt  >> network_stats.txt
grep "packets_received::total" m5out/stats.txt  >> network_stats.txt
grep "flits_injected::total" m5out/stats.txt  >> network_stats.txt
grep "flits_received::total" m5out/stats.txt  >> network_stats.txt
grep "average_flit_latency" m5out/stats.txt  >> network_stats.txt
grep "average_flit_network_latency" m5out/stats.txt  >> network_stats.txt
grep "average_flit_queueing_latency" m5out/stats.txt  >> network_stats.txt
grep "average_hops" m5out/stats.txt  >> network_stats.txt
echo "___________________________________________________________" >> network_stats.txt
cat network_stats.txt | grep "average_flit_latency"
done

python3 ./command/plot.py
#python3 ./command/plot_interposer.py interposer_vc.log