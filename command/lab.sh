echo "Running Simulation..." > network_stats.txt
start=0.055
step=0.001
end=0.055
for rate in $(seq $start $step $end)
do
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
--vcs-per-vnet=4 \
--routing-algorithm=2 \
#2>&1 | tee interposer_vc.log
echo "injectionrate:$rate">> network_stats.txt
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