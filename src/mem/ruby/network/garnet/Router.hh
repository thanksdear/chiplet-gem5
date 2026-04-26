/*
 * Copyright (c) 2020 Inria
 * Copyright (c) 2016 Georgia Institute of Technology
 * Copyright (c) 2008 Princeton University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#ifndef __MEM_RUBY_NETWORK_GARNET_0_ROUTER_HH__
#define __MEM_RUBY_NETWORK_GARNET_0_ROUTER_HH__

#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/common/NetDest.hh"
#include "mem/ruby/network/BasicRouter.hh"
#include "mem/ruby/network/garnet/CommonTypes.hh"
#include "mem/ruby/network/garnet/CrossbarSwitch.hh"
#include "mem/ruby/network/garnet/GarnetNetwork.hh"
#include "mem/ruby/network/garnet/RoutingUnit.hh"
#include "mem/ruby/network/garnet/SwitchAllocator.hh"
#include "mem/ruby/network/garnet/EscapeBuffer.hh"
#include "mem/ruby/network/garnet/flit.hh"
#include "params/GarnetRouter.hh"

namespace gem5
{

namespace ruby
{

class FaultModel;

namespace garnet
{

class NetworkLink;
class CreditLink;
class InputUnit;
class OutputUnit;

class Router : public BasicRouter, public Consumer
{
  public:
    typedef GarnetRouterParams Params;
    Router(const Params &p);

    ~Router() = default;

    void wakeup();
    void print(std::ostream& out) const {};

    void init();
    void addInPort(PortDirection inport_dirn, NetworkLink *link,
                   CreditLink *credit_link);
    void addOutPort(PortDirection outport_dirn, NetworkLink *link,
                    std::vector<NetDest>& routing_table_entry,
                    int link_weight, CreditLink *credit_link,
                    uint32_t consumerVcs);

    Cycles get_pipe_stages(){ return m_latency; }
    uint32_t get_num_vcs()       { return m_num_vcs; }
    uint32_t get_num_vnets()     { return m_virtual_networks; }
    uint32_t get_vc_per_vnet()   { return m_vc_per_vnet; }
    int get_num_inports()   { return m_input_unit.size(); }
    int get_num_outports()  { return m_output_unit.size(); }
    int get_id()            { return m_id; }

    void init_net_ptr(GarnetNetwork* net_ptr)
    {
        m_network_ptr = net_ptr;
    }

    GarnetNetwork* get_net_ptr()                    { return m_network_ptr; }

    InputUnit*
    getInputUnit(unsigned port)
    {
        assert(port < m_input_unit.size());
        return m_input_unit[port].get();
    }

    OutputUnit*
    getOutputUnit(unsigned port)
    {
        assert(port < m_output_unit.size());
        return m_output_unit[port].get();
    }

    int getBitWidth() { return m_bit_width; }

    PortDirection getOutportDirection(int outport);
    PortDirection getInportDirection(int inport);

    int route_compute(RouteInfo route, int inport, PortDirection direction,
                      flit *t_flit = nullptr);
    // Health-based routing optimization for interposer Up direction
    int optimizeUpRoute(int outport, flit *t_flit);
    void grant_switch(int inport, flit *t_flit);
    void schedule_wakeup(Cycles time);

    std::string getPortDirectionName(PortDirection direction);
    void printFaultVector(std::ostream& out);
    void printAggregateFaultProbability(std::ostream& out);

    void regStats();
    void collateStats();
    void resetStats();

    // For Fault Model:
    bool get_fault_vector(int temperature, float fault_vector[]) {
        return m_network_ptr->fault_model->fault_vector(m_id, temperature,
                                                        fault_vector);
    }
    bool get_aggregate_fault_probability(int temperature,
                                         float *aggregate_fault_prob) {
        return m_network_ptr->fault_model->fault_prob(m_id, temperature,
                                                      aggregate_fault_prob);
    }

    uint32_t functionalWrite(Packet *);

  private:
    Cycles m_latency;
    uint32_t m_virtual_networks, m_vc_per_vnet, m_num_vcs;
    uint32_t m_bit_width;
    GarnetNetwork *m_network_ptr;

    RoutingUnit routingUnit;
    SwitchAllocator switchAllocator;
    CrossbarSwitch crossbarSwitch;

    std::vector<std::shared_ptr<InputUnit>> m_input_unit;
    std::vector<std::shared_ptr<OutputUnit>> m_output_unit;

    // Statistical variables required for power computations
    statistics::Scalar m_buffer_reads;
    statistics::Scalar m_buffer_writes;

    statistics::Scalar m_sw_input_arbiter_activity;
    statistics::Scalar m_sw_output_arbiter_activity;

    statistics::Scalar m_crossbar_activity;

    // ---------------------------------------------------------------
    // Interposer VC monitoring
    // m_is_interposer is set in init() by detecting a "Down" outport.
    // InterposerStats uses the new statistics::Group API:
    //   - ADD_STAT auto-registers each stat at construction time
    //   - port_vc_active_cycles.init() is called in Router::init()
    //     after ports are connected (init() runs before regStats())
    // ---------------------------------------------------------------
    bool m_is_interposer;

    // Deadlock detection flags (set per-cycle, checked globally)
    bool m_up_input_stall = false;   // Up InputUnit stalled
    bool m_up_output_stall = false;  // Up OutputUnit stalled
    uint64_t m_up_flit_count = 0;    // flits sent toward chiplet (Up)

  public:
    bool getUpInputStall() const { return m_up_input_stall; }
    bool getUpOutputStall() const { return m_up_output_stall; }
    void incrementUpFlitCount() { m_up_flit_count++; }
    uint64_t getUpFlitCount() const { return m_up_flit_count; }

    // ----- Health score propagation interface -----
    int getChipletId() const { return m_chiplet_id; }
    bool isInterposer() const { return m_is_interposer; }

    // Called by neighbor routers to push their health score to us
    void receiveHealthScore(int source_router_id, int score);

    // Query neighbor health table (for routing decisions)
    int getNeighborHealth(int router_id) const;
    bool hasNeighborHealth(int router_id) const;
    const std::map<int, int>& getNeighborHealthTable() const
    { return m_neighbor_health_table; }
    const std::vector<Router*>& getDirectNeighbors() const
    { return m_direct_neighbors; }
    void incrOptRedirected() { m_opt_redirected++; }

    // Adaptive RC statistics incrementors
    void incrArcAtTarget() { m_arc_at_target++; }
    void incrArcHealthy() { m_arc_healthy++; }
    void incrArcCongested() { m_arc_congested++; }
    void incrArcRedirected() { m_arc_redirected++; }
    void incrArcNoBetter() { m_arc_no_better++; }
    void incrArcAntiLivelock() { m_arc_anti_livelock++; }

    // Get the list of same-chiplet interposer router pointers
    const std::vector<Router*>& getChipletPeers() const
    { return m_chiplet_peers; }

  private:
    // ----- Chiplet grouping & health propagation -----
    int m_chiplet_id = -1;               // which chiplet this router serves
    std::vector<Router*> m_chiplet_peers; // same-chiplet interposer routers (excluding self)
    std::vector<Router*> m_direct_neighbors; // adjacent interposer routers (E/W/N/S)
    std::map<int, int> m_neighbor_health_table; // router_id → quantized health score [0,7]

    // ----- Routing optimization statistics -----
    int m_opt_called = 0;          // optimizeUpRoute called (is Up direction)
    int m_opt_congested = 0;       // self_score <= threshold
    int m_opt_neighbor_better = 0; // neighbor score satisfies LOCAL_BIAS
    int m_opt_lateral_blocked = 0; // lateral link has no free VC
    int m_opt_redirected = 0;      // actually redirected

    // ----- Adaptive RC statistics (algorithm 4) -----
    int m_arc_at_target = 0;       // flit arrived at its target gateway
    int m_arc_healthy = 0;         // target healthy, go Up directly
    int m_arc_congested = 0;       // target congested, try redirect
    int m_arc_redirected = 0;      // actually redirected to neighbor
    int m_arc_no_better = 0;       // no better neighbor, go Up anyway
    int m_arc_anti_livelock = 0;   // redirected flit went Up (anti-livelock)

    // ----- Recovery cooldown -----
    Tick m_recovery_cooldown = 0;  // skip deadlock detection until this tick

    // ----- Escape buffer for deadlock recovery -----
    // One EscapeBuffer per "Down" input port (chiplet → interposer)
    // Key: inport index, Value: escape buffer
    std::map<int, std::unique_ptr<EscapeBuffer>> m_escape_buffers;

    // Escape buffer deadlock recovery logic (called from wakeup)
    void escapeBufferTick();

    struct InterposerStats : public statistics::Group
    {
        InterposerStats(statistics::Group *parent);

        statistics::Scalar vc_active_cycles;
        statistics::Vector port_vc_active_cycles;
    } interposerStats;
};

} // namespace garnet
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_GARNET_0_ROUTER_HH__
