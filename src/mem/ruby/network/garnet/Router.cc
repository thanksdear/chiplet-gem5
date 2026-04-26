/*
 * Copyright (c) 2020 Advanced Micro Devices, Inc.
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


#include "mem/ruby/network/garnet/Router.hh"

#include <fstream>
#include <iomanip>

#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/garnet/CreditLink.hh"
#include "mem/ruby/network/garnet/GarnetNetwork.hh"
#include "mem/ruby/network/garnet/InputUnit.hh"
#include "mem/ruby/network/garnet/NetworkLink.hh"
#include "mem/ruby/network/garnet/OutputUnit.hh"
#include "mem/ruby/network/garnet/EscapeBuffer.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

Router::Router(const Params &p)
  : BasicRouter(p), Consumer(this), m_latency(p.latency),
    m_virtual_networks(p.virt_nets), m_vc_per_vnet(p.vcs_per_vnet),
    m_num_vcs(m_virtual_networks * m_vc_per_vnet), m_bit_width(p.width),
    m_network_ptr(nullptr), routingUnit(this), switchAllocator(this),
    crossbarSwitch(this), m_is_interposer(false),
    interposerStats(this)   // 传 this 作为父 Group，自动建立层级关系
{
    m_input_unit.clear();
    m_output_unit.clear();
}

// InterposerStats 构造函数：ADD_STAT 在初始化列表里完成注册，不需要 regStats()
Router::InterposerStats::InterposerStats(statistics::Group *parent)
    : statistics::Group(parent, "interposer"),
      ADD_STAT(vc_active_cycles, statistics::units::Cycle::get(),
               "Total VC-cycles: sum of ACTIVE_ input VCs each wakeup "
               "(interposer routers only)"),
      ADD_STAT(port_vc_active_cycles, statistics::units::Cycle::get(),
               "Per-input-port VC-cycles (interposer routers only)")
{}

void
Router::init()
{
    BasicRouter::init();

    switchAllocator.init();
    crossbarSwitch.init();

    // Detect interposer router: any output port with direction "Up"
    // (interposer 向上发往 chiplet) marks this as an interposer node.
    m_is_interposer = false;
    for (auto &ou : m_output_unit) {
        if (ou->get_direction() == "Up") {
            m_is_interposer = true;
            break;
        }
    }

    // Initialize health monitors for Up output ports on interposer routers
    if (m_is_interposer) {
        // Convert cycle-based threshold to ticks
        Tick stall_threshold =
            m_network_ptr->getInterposerStallThreshold() * clockPeriod();
        if (m_id == 64) {
            std::cout << "[DEBUG] Router 64: stall_threshold_cycles="
                      << m_network_ptr->getInterposerStallThreshold()
                      << " clockPeriod=" << clockPeriod()
                      << " stall_threshold_ticks=" << stall_threshold
                      << std::endl;
        }
        for (auto &ou : m_output_unit) {
            if (ou->get_direction() == "Up") {
                // Only count vnet 2 (data) VCs for health monitoring
                // vnet 0/1 are 1-flit control packets, always flowing
                int data_vnet = 2;
                int vc_base = data_vnet * ou->getVcsPerVnet();
                int vc_end = vc_base + ou->getVcsPerVnet();
                ou->initHealthMonitor(stall_threshold,
                                      vc_base, vc_end);
            }
        }
    }

    // Compute chiplet ID and build peer list for health propagation
    // Interposer routers 64-79: every 4 routers belong to one chiplet
    //   C0: 64-67, C1: 68-71, C2: 72-75, C3: 76-79
    if (m_is_interposer) {
        int base_interposer_id = 64; // first interposer router ID
        int routers_per_chiplet = 4;
        m_chiplet_id = (m_id - base_interposer_id) / routers_per_chiplet;

        // Build peer list (deferred to after all routers are created)
        // We do this by iterating all routers in the network
        int chiplet_start = base_interposer_id
                            + m_chiplet_id * routers_per_chiplet;
        int chiplet_end = chiplet_start + routers_per_chiplet;
        for (auto *r : m_network_ptr->getRouters()) {
            int rid = r->get_id();
            if (rid >= chiplet_start && rid < chiplet_end && rid != m_id) {
                m_chiplet_peers.push_back(r);
            }
        }

        // Build direct neighbor list by matching outport links
        // to other interposer routers' input links
        // Note: use ID range (64-79) instead of isInterposer() because
        // other routers' init() may not have run yet.
        for (int p = 0; p < (int)m_output_unit.size(); p++) {
            std::string dir = m_output_unit[p]->get_direction();
            if (dir == "East" || dir == "West" ||
                dir == "North" || dir == "South") {
                NetworkLink *link = m_output_unit[p]->get_out_link();
                for (auto *r : m_network_ptr->getRouters()) {
                    int rid = r->get_id();
                    if (rid < base_interposer_id ||
                        rid >= base_interposer_id + 16 ||
                        rid == m_id)
                        continue;
                    for (int ip = 0; ip < r->get_num_inports(); ip++) {
                        if (r->getInputUnit(ip)->get_in_link() == link) {
                            m_direct_neighbors.push_back(r);
                        }
                    }
                }
            }
        }
    }

    // Create escape buffers for "Down" input ports (chiplet → interposer)
    if (m_is_interposer) {
        int buf_depth = m_network_ptr->getBuffersPerDataVC();
        for (int i = 0; i < (int)m_input_unit.size(); i++) {
            if (m_input_unit[i]->get_direction() == "Down") {
                m_escape_buffers[i] = std::make_unique<EscapeBuffer>(
                    this, i, buf_depth);
            }
        }
    }

    // Vector stats need a runtime size; set it here after ports are connected.
    // Also apply nozero so non-interposer routers don't clutter stats.txt.
    interposerStats.vc_active_cycles.flags(statistics::nozero);
    interposerStats.port_vc_active_cycles
        .init(m_input_unit.size())
        .flags(statistics::nozero);
}

void
Router::wakeup()
{
    DPRINTF(RubyNetwork, "Router %d woke up\n", m_id);
    assert(clockEdge() == curTick());

    // check for incoming flits
    for (int inport = 0; inport < m_input_unit.size(); inport++) {
        m_input_unit[inport]->wakeup();
    }

    // Escape buffer: continue absorb / re-inject (before SA)
    if (m_is_interposer)
        escapeBufferTick();

    // check for incoming credits
    // Note: the credit update is happening before SA
    // buffer turnaround time =
    //     credit traversal (1-cycle) + SA (1-cycle) + Link Traversal (1-cycle)
    // if we want the credit update to take place after SA, this loop should
    // be moved after the SA request
    for (int outport = 0; outport < m_output_unit.size(); outport++) {
        m_output_unit[outport]->wakeup();
    }

    // Switch Allocation
    switchAllocator.wakeup();

    // Switch Traversal
    crossbarSwitch.wakeup();

    // ---------------------------------------------------------------
    // Interposer VC monitoring: sample active VCs every wakeup cycle
    // ---------------------------------------------------------------
    if (m_is_interposer) {
        Tick STALL_THRESHOLD =
            m_network_ptr->getInterposerStallThreshold() * clockPeriod();

        // --- Step 1: Check Up InputUnit (chiplet → interposer) ---
        bool any_input_stall = false;
        for (int i = 0; i < (int)m_input_unit.size(); i++) {
            int active = m_input_unit[i]->get_num_active_vcs();
            interposerStats.vc_active_cycles += active;
            interposerStats.port_vc_active_cycles[i] += active;

            if (m_input_unit[i]->get_direction() == "Up") {
                m_input_unit[i]->sampleVcStall();
                int num_vcs = m_input_unit[i]->get_num_vcs();
                for (int v = 0; v < num_vcs; v++) {
                    if (m_input_unit[i]->getVcStallCycles(v)
                            >= STALL_THRESHOLD) {
                        any_input_stall = true;
                        break;
                    }
                }
            }
        }
        m_up_input_stall = any_input_stall;

        // --- Step 2: Check Up OutputUnit (interposer → chiplet) ---
        bool any_output_stall = false;
        int broadcast_interval =
            m_network_ptr->getHealthMonitorBroadcastInterval();
        int change_threshold =
            m_network_ptr->getHealthMonitorChangeThreshold();

        for (int o = 0; o < (int)m_output_unit.size(); o++) {
            if (m_output_unit[o]->get_direction() == "Up") {
                // Update health monitor and check broadcast
                if (m_output_unit[o]->hasHealthMonitor()) {
                    // Sync free credits FIRST, then compute score
                    m_output_unit[o]->syncHealthCredits(curTick());

                    // Sample health score into histogram every cycle
                    m_output_unit[o]->sampleHealthScore(curTick());

                    // Worst score: for deadlock detection
                    int worst = m_output_unit[o]
                        ->getWorstQuantizedScore(curTick());
                    if (worst == 0)
                        any_output_stall = true;
                    // Average score: for routing optimization
                    int avg = m_output_unit[o]
                        ->getAverageQuantizedScore(curTick());

                    // S=0 (any VC dead): immediately broadcast
                    // worst score to all same-chiplet peers
                    if (worst == 0) {
                        for (auto *peer : m_chiplet_peers) {
                            peer->receiveHealthScore(
                                m_id, worst);
                        }
                        DPRINTF(RubyNetwork,
                            "Router %d OutPort %d(Up) S=0 "
                            "immediate broadcast to %d "
                            "chiplet-%d peers\n",
                            m_id, o,
                            (int)m_chiplet_peers.size(),
                            m_chiplet_id);
                    }

                    // S!=0: broadcast average score to direct
                    // neighbors for routing optimization
                    if (worst != 0) {
                        bool need_broadcast =
                            m_output_unit[o]->updateHealthMonitor(
                                curTick(), broadcast_interval,
                                change_threshold);

                        if (need_broadcast) {
                            int sent = 0;
                            m_output_unit[o]
                                ->recordHealthBroadcast(
                                    curTick());
                            for (auto *nb : m_direct_neighbors) {
                                if (nb->getChipletId()
                                        == m_chiplet_id) {
                                    nb->receiveHealthScore(
                                        m_id, avg);
                                    sent++;
                                }
                            }
                            DPRINTF(RubyNetwork,
                                "Router %d OutPort %d(Up) "
                                "worst=%d avg=%d sent to %d "
                                "same-chiplet neighbors\n",
                                m_id, o, worst, avg, sent);
                        }
                    }
                }
            }
        }
        m_up_output_stall = any_output_stall;

        // --- IPDR deadlock detection (routing algorithm 7) ---
        if (m_network_ptr->getRoutingAlgorithm() == IPDR_) {
            int ipdr_idx = m_id - 64; // boundary index 0-15
            if (ipdr_idx >= 0 && ipdr_idx < 16) {
                if (any_output_stall) {
                    // Up output congested: increment dd_counter
                    m_network_ptr->ipdrIncrementDd(ipdr_idx);
                    // Check if threshold reached → enter recovery
                    if (m_network_ptr->ipdrGetState(ipdr_idx)
                            == GarnetNetwork::IPDR_DD) {
                        m_network_ptr->ipdrEnterRecovery(ipdr_idx);
                        DPRINTF(RubyNetwork,
                            "IPDR: Router %d boundary %d "
                            "entering recovery at tick %lld\n",
                            m_id, ipdr_idx, curTick());
                    }
                } else {
                    // Not congested: reset counter
                    m_network_ptr->ipdrResetDd(ipdr_idx);
                }
                // During recovery: drain IPDR buffer
                if (m_network_ptr->ipdrGetState(ipdr_idx)
                        == GarnetNetwork::IPDR_RECOVERY) {
                    // Attempt to drain: if Up output has free VC,
                    // decrement buffer used count
                    if (m_network_ptr->ipdrBufferUsed(ipdr_idx) > 0
                            && !any_output_stall) {
                        m_network_ptr->ipdrBufferRemove(ipdr_idx);
                    }
                    // If buffer fully drained, finish recovery
                    if (m_network_ptr->ipdrBufferUsed(ipdr_idx) == 0) {
                        m_network_ptr->ipdrFinishRecovery(ipdr_idx);
                        DPRINTF(RubyNetwork,
                            "IPDR: Router %d boundary %d "
                            "recovery finished at tick %lld\n",
                            m_id, ipdr_idx, curTick());
                    }
                }
            }
        }

        // --- Step 3: Global deadlock check across ALL interposer routers ---
        // (commented out: replaced by distributed detection via health
        //  propagation, kept for future reference)
        // Any router with input stall + any router with output stall = deadlock
        /*
        bool global_input_stall = false;
        bool global_output_stall = false;
        for (auto *r : m_network_ptr->getRouters()) {
            if (r->getUpInputStall())  global_input_stall = true;
            if (r->getUpOutputStall()) global_output_stall = true;
            if (global_input_stall && global_output_stall) break;
        }

        if (global_input_stall && global_output_stall) {
            std::ofstream logf("m5out/deadlock.log", std::ios::app);
            logf << "[DEADLOCK DETECTED] tick=" << curTick()
                 << " (detected by Router " << m_id << ")"
                 << std::endl;

            // Log ALL interposer routers with stalls
            for (auto *r : m_network_ptr->getRouters()) {
                if (!r->getUpInputStall() && !r->getUpOutputStall())
                    continue;
                logf << " Router " << r->get_id();
                if (r->getUpInputStall())
                    logf << " [input STALL]";
                if (r->getUpOutputStall())
                    logf << " [output STALL]";
                logf << std::endl;
            }
            logf.close();

            exitSimLoop("Deadlock detected: interposer TSV "
                        "input and output both stalled globally", 1);
            return;
        }
        */

        // --- Step 4: Distributed deadlock detection ---
        // Condition: received S=0 from any same-chiplet peer
        //   (peer's Up output channel is dead)
        // AND local Up InputUnit has timed out
        //   (down channel blocked: flits from chiplet stuck)
        // Together these form a circular dependency → deadlock.
        // Skip if in recovery cooldown period
        if (m_recovery_cooldown > 0 && curTick() < m_recovery_cooldown) {
            // Still cooling down from last recovery
        } else {

        bool peer_dead = false;
        int dead_peer_id = -1;
        for (auto &entry : m_neighbor_health_table) {
            if (entry.second == 0) {
                // Check if this source is a same-chiplet peer
                for (auto *p : m_chiplet_peers) {
                    if (p->get_id() == entry.first) {
                        peer_dead = true;
                        dead_peer_id = entry.first;
                        break;
                    }
                }
                if (peer_dead) break;
            }
        }

        if (peer_dead && m_up_input_stall) {
            // --- Log deadlock detection (preserved) ---
            std::ofstream logf("m5out/deadlock.log", std::ios::app);
            logf << "[DEADLOCK DETECTED] tick=" << curTick()
                 << " Router " << m_id
                 << " (chiplet " << m_chiplet_id << ")"
                 << std::endl;
            logf << "  Peer Router " << dead_peer_id
                 << " Up channel dead (S=0)" << std::endl;
            Tick stall_threshold =
                m_network_ptr->getInterposerStallThreshold();
            for (int i = 0; i < (int)m_input_unit.size(); i++) {
                if (m_input_unit[i]->get_direction() != "Up")
                    continue;
                int num_vcs = m_input_unit[i]->get_num_vcs();
                for (int v = 0; v < num_vcs; v++) {
                    Tick stall = m_input_unit[i]->getVcStallCycles(v);
                    if (stall >= stall_threshold) {
                        logf << "  Local Router " << m_id
                             << " Up input vc" << v
                             << " stall=" << stall
                             << " (threshold=" << stall_threshold
                             << ")" << std::endl;
                    }
                }
            }

            // Log full chiplet health state
            logf << "  Chiplet " << m_chiplet_id
                 << " health table:" << std::endl;
            for (auto &entry : m_neighbor_health_table) {
                logf << "    Router " << entry.first
                     << " S=" << entry.second << std::endl;
            }

            // --- Escape buffer recovery ---
            logf << "[DEADLOCK RECOVERY] tick=" << curTick()
                 << " Router " << m_id << std::endl;

            for (auto &[inport, esc] : m_escape_buffers) {
                if (esc->isOccupied())
                    continue;

                InputUnit *iu = m_input_unit[inport].get();
                int num_vcs = iu->get_num_vcs();

                // Find the most stalled VC with a head flit
                int worst_vc = -1;
                Tick worst_stall = 0;
                for (int v = 0; v < num_vcs; v++) {
                    if (iu->get_vc_state(v) != ACTIVE_)
                        continue;
                    if (iu->getVcBufferSize(v) == 0)
                        continue;
                    flit *top = iu->peekTopFlit(v);
                    if (top->get_type() != HEAD_ &&
                        top->get_type() != HEAD_TAIL_)
                        continue;
                    Tick stall = iu->getVcStallCycles(v);
                    if (stall > worst_stall) {
                        worst_stall = stall;
                        worst_vc = v;
                    }
                }

                if (worst_vc >= 0) {
                    esc->startAbsorb(iu, worst_vc, curTick());
                    logf << "  Absorbed vc " << worst_vc
                         << " from inport " << inport
                         << " (stall=" << worst_stall << ")"
                         << std::endl;
                } else {
                    logf << "  No absorbable VC on inport "
                         << inport << std::endl;
                }
            }

            logf.close();

            // Reset peer's health score to avoid re-triggering next cycle
            m_neighbor_health_table[dead_peer_id] = 7;

            // Set cooldown: skip deadlock detection for a while
            m_recovery_cooldown = curTick()
                + m_network_ptr->getInterposerStallThreshold()
                  * clockPeriod();
        }
        } // end cooldown else
    }
}

// Escape buffer per-cycle tick: continue absorbing + attempt re-injection
void
Router::escapeBufferTick()
{
    Tick max_wait = m_network_ptr->getInterposerStallThreshold()
                    * clockPeriod();

    for (auto &[inport, esc] : m_escape_buffers) {
        if (!esc->isOccupied())
            continue;

        InputUnit *iu = m_input_unit[inport].get();

        // Phase 1: continue absorbing if partial packet
        if (esc->isAbsorbing()) {
            esc->continueAbsorb(iu, curTick());
            continue;  // don't try re-inject same cycle
        }

        // Phase 2: try re-injection into a free VC
        if (!esc->isEmpty()) {
            bool ok = esc->tryReinject(iu, this, curTick());
            if (!ok && esc->isWaitExpired(curTick(), max_wait)) {
                // Waited too long — force re-inject
                esc->forceReinject(iu, this, curTick());

                std::ofstream logf("m5out/deadlock.log", std::ios::app);
                logf << "[ESCAPE FORCE REINJECT] tick=" << curTick()
                     << " Router " << m_id
                     << " inport " << inport << std::endl;
                logf.close();
            }
        }
    }
}

void
Router::addInPort(PortDirection inport_dirn,
                  NetworkLink *in_link, CreditLink *credit_link)
{
    fatal_if(in_link->bitWidth != m_bit_width, "Widths of link %s(%d)does"
            " not match that of Router%d(%d). Consider inserting SerDes "
            "Units.", in_link->name(), in_link->bitWidth, m_id, m_bit_width);

    int port_num = m_input_unit.size();
    InputUnit *input_unit = new InputUnit(port_num, inport_dirn, this);

    input_unit->set_in_link(in_link);
    input_unit->set_credit_link(credit_link);
    in_link->setLinkConsumer(this);
    in_link->setVcsPerVnet(get_vc_per_vnet());
    credit_link->setSourceQueue(input_unit->getCreditQueue(), this);
    credit_link->setVcsPerVnet(get_vc_per_vnet());

    m_input_unit.push_back(std::shared_ptr<InputUnit>(input_unit));

    routingUnit.addInDirection(inport_dirn, port_num);
}

void
Router::addOutPort(PortDirection outport_dirn,
                   NetworkLink *out_link,
                   std::vector<NetDest>& routing_table_entry, int link_weight,
                   CreditLink *credit_link, uint32_t consumerVcs)
{
    fatal_if(out_link->bitWidth != m_bit_width, "Widths of units do not match."
            " Consider inserting SerDes Units");

    int port_num = m_output_unit.size();
    OutputUnit *output_unit = new OutputUnit(port_num, outport_dirn, this,
                                             consumerVcs);

    output_unit->set_out_link(out_link);
    output_unit->set_credit_link(credit_link);
    credit_link->setLinkConsumer(this);
    credit_link->setVcsPerVnet(consumerVcs);
    out_link->setSourceQueue(output_unit->getOutQueue(), this);
    out_link->setVcsPerVnet(consumerVcs);

    m_output_unit.push_back(std::shared_ptr<OutputUnit>(output_unit));

    routingUnit.addRoute(routing_table_entry);
    routingUnit.addWeight(link_weight);
    routingUnit.addOutDirection(outport_dirn, port_num);
}

PortDirection
Router::getOutportDirection(int outport)
{
    return m_output_unit[outport]->get_direction();
}

PortDirection
Router::getInportDirection(int inport)
{
    return m_input_unit[inport]->get_direction();
}

int
Router::route_compute(RouteInfo route, int inport, PortDirection inport_dirn,
                      flit *t_flit)
{
    return routingUnit.outportCompute(route, inport, inport_dirn, t_flit);
}

void
Router::grant_switch(int inport, flit *t_flit)
{
    crossbarSwitch.update_sw_winner(inport, t_flit);
}

void
Router::schedule_wakeup(Cycles time)
{
    // wake up after time cycles
    scheduleEvent(time);
}

std::string
Router::getPortDirectionName(PortDirection direction)
{
    // PortDirection is actually a string
    // If not, then this function should add a switch
    // statement to convert direction to a string
    // that can be printed out
    return direction;
}

void
Router::regStats()
{
    BasicRouter::regStats();

    m_buffer_reads
        .name(name() + ".buffer_reads")
        .flags(statistics::nozero)
    ;

    m_buffer_writes
        .name(name() + ".buffer_writes")
        .flags(statistics::nozero)
    ;

    m_crossbar_activity
        .name(name() + ".crossbar_activity")
        .flags(statistics::nozero)
    ;

    m_sw_input_arbiter_activity
        .name(name() + ".sw_input_arbiter_activity")
        .flags(statistics::nozero)
    ;

    m_sw_output_arbiter_activity
        .name(name() + ".sw_output_arbiter_activity")
        .flags(statistics::nozero)
    ;

}

void
Router::collateStats()
{
    for (int j = 0; j < m_virtual_networks; j++) {
        for (int i = 0; i < m_input_unit.size(); i++) {
            m_buffer_reads += m_input_unit[i]->get_buf_read_activity(j);
            m_buffer_writes += m_input_unit[i]->get_buf_write_activity(j);
        }
    }

    m_sw_input_arbiter_activity = switchAllocator.get_input_arbiter_activity();
    m_sw_output_arbiter_activity =
        switchAllocator.get_output_arbiter_activity();
    m_crossbar_activity = crossbarSwitch.get_crossbar_activity();

    // Print per-port VC utilisation summary for interposer routers
    if (m_is_interposer) {
        int total_in_vcs = 0;
        for (int i = 0; i < (int)m_input_unit.size(); i++)
            total_in_vcs += m_input_unit[i]->get_num_vcs();

        std::cout << "[Interposer VC] Router " << m_id
                  << "  total_vc_active_cycles="
                  << interposerStats.vc_active_cycles.value()
                  << "  ports=" << m_input_unit.size()
                  << "  total_in_vcs=" << total_in_vcs
                  << "  up_flits=" << m_up_flit_count
                  << std::endl;

        for (int i = 0; i < (int)m_input_unit.size(); i++) {
            // Only print detail for "Up" ports (TSV from chiplet)
            if (m_input_unit[i]->get_direction() != "Up")
                continue;

            std::cout << "  port" << i << "(Up)"
                      << "  vc_active_cycles="
                      << interposerStats.port_vc_active_cycles[i].value();

            int num_vcs = m_input_unit[i]->get_num_vcs();
            for (int v = 0; v < num_vcs; v++) {
                Tick cur_stall = m_input_unit[i]->getVcStallCycles(v);
                Tick max_stall = m_input_unit[i]->getVcMaxStall(v);
                int buf_size = m_input_unit[i]->getVcBufferSize(v);
                if (cur_stall > 0 || max_stall > 0) {
                    std::cout << "  vc" << v
                              << "[sz=" << buf_size
                              << ",stall=" << cur_stall
                              << ",max=" << max_stall << "]";
                }
            }
            std::cout << std::endl;
        }

        // Up output ports: per-VC credit & health info + histogram
        for (int o = 0; o < (int)m_output_unit.size(); o++) {
            if (m_output_unit[o]->get_direction() != "Up")
                continue;

            // Per-VC credit and health score
            if (m_output_unit[o]->hasHealthMonitor()) {
                int num_vcs = m_output_unit[o]->getNumMonitoredVcs();
                int vc_base = m_output_unit[o]->getMonitorVcStart();
                std::cout << "  outport" << o << "(Up) per-VC health:";
                for (int v = 0; v < num_vcs; v++) {
                    int vc = vc_base + v;
                    int cr = m_output_unit[o]->get_credit_count(vc);
                    int sc = m_output_unit[o]->getVcQuantizedScore(
                                 v, curTick());
                    Tick ms = m_output_unit[o]->getVcMaxStallTicks(v);
                    std::cout << " vc" << vc
                              << "[cr=" << cr
                              << ",S=" << sc
                              << ",max_stall=" << ms << "]";
                }
                std::cout << std::endl;

                // Aggregated: worst and average
                int worst = m_output_unit[o]
                    ->getWorstQuantizedScore(curTick());
                int avg = m_output_unit[o]
                    ->getAverageQuantizedScore(curTick());
                std::cout << "  outport" << o << "(Up)"
                          << " worst_S=" << worst
                          << " avg_S=" << avg
                          << std::endl;

                // Health score histogram (worst score)
                const auto &hist =
                    m_output_unit[o]->getHealthScoreHistogram();
                uint64_t total_samples = 0;
                for (int s = 0; s < 8; s++)
                    total_samples += hist[s];

                std::cout << "  outport" << o
                          << "(Up) health histogram(average)"
                          << " [total=" << total_samples << "]:"
                          << std::endl << "    ";
                for (int s = 0; s < 8; s++) {
                    std::cout << "S" << s << "=" << hist[s];
                    if (s < 7) std::cout << "  ";
                }
                std::cout << std::endl;

                // Print percentage distribution
                if (total_samples > 0) {
                    std::cout << "    ";
                    for (int s = 0; s < 8; s++) {
                        float pct = 100.0f * hist[s] / total_samples;
                        if (hist[s] > 0) {
                            std::cout << "S" << s << "="
                                      << std::fixed
                                      << std::setprecision(1)
                                      << pct << "%  ";
                        }
                    }
                    std::cout << std::endl;
                }
            }
        }

        // Adaptive RC statistics (algorithm 4)
        if (m_arc_at_target > 0) {
            std::cout << "  [AdaptiveRC] at_target=" << m_arc_at_target
                      << " healthy=" << m_arc_healthy
                      << " congested=" << m_arc_congested
                      << " redirected=" << m_arc_redirected
                      << " no_better=" << m_arc_no_better
                      << " anti_livelock=" << m_arc_anti_livelock
                      << std::endl;
        }
    }
}

void
Router::resetStats()
{
    for (int i = 0; i < m_input_unit.size(); i++) {
            m_input_unit[i]->resetStats();
    }

    crossbarSwitch.resetStats();
    switchAllocator.resetStats();
}

void
Router::printFaultVector(std::ostream& out)
{
    int temperature_celcius = BASELINE_TEMPERATURE_CELCIUS;
    int num_fault_types = m_network_ptr->fault_model->number_of_fault_types;
    float fault_vector[num_fault_types];
    get_fault_vector(temperature_celcius, fault_vector);
    out << "Router-" << m_id << " fault vector: " << std::endl;
    for (int fault_type_index = 0; fault_type_index < num_fault_types;
         fault_type_index++) {
        out << " - probability of (";
        out <<
        m_network_ptr->fault_model->fault_type_to_string(fault_type_index);
        out << ") = ";
        out << fault_vector[fault_type_index] << std::endl;
    }
}

void
Router::printAggregateFaultProbability(std::ostream& out)
{
    int temperature_celcius = BASELINE_TEMPERATURE_CELCIUS;
    float aggregate_fault_prob;
    get_aggregate_fault_probability(temperature_celcius,
                                    &aggregate_fault_prob);
    out << "Router-" << m_id << " fault probability: ";
    out << aggregate_fault_prob << std::endl;
}

uint32_t
Router::functionalWrite(Packet *pkt)
{
    uint32_t num_functional_writes = 0;
    num_functional_writes += crossbarSwitch.functionalWrite(pkt);

    for (uint32_t i = 0; i < m_input_unit.size(); i++) {
        num_functional_writes += m_input_unit[i]->functionalWrite(pkt);
    }

    for (uint32_t i = 0; i < m_output_unit.size(); i++) {
        num_functional_writes += m_output_unit[i]->functionalWrite(pkt);
    }

    return num_functional_writes;
}

void
Router::receiveHealthScore(int source_router_id, int score)
{
    m_neighbor_health_table[source_router_id] = score;
}

int
Router::getNeighborHealth(int router_id) const
{
    auto it = m_neighbor_health_table.find(router_id);
    if (it != m_neighbor_health_table.end())
        return it->second;
    return 7; // default: assume healthy if unknown
}

bool
Router::hasNeighborHealth(int router_id) const
{
    return m_neighbor_health_table.find(router_id)
           != m_neighbor_health_table.end();
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
