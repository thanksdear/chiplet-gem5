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

#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/garnet/CreditLink.hh"
#include "mem/ruby/network/garnet/GarnetNetwork.hh"
#include "mem/ruby/network/garnet/InputUnit.hh"
#include "mem/ruby/network/garnet/NetworkLink.hh"
#include "mem/ruby/network/garnet/OutputUnit.hh"
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
        for (int i = 0; i < (int)m_input_unit.size(); i++) {
            int active = m_input_unit[i]->get_num_active_vcs();
            interposerStats.vc_active_cycles += active;
            interposerStats.port_vc_active_cycles[i] += active;
            // Only sample stall on "Up" input ports (TSV from chiplet)
            if (m_input_unit[i]->get_direction() == "Up") {
                m_input_unit[i]->sampleVcStall();

                // Deadlock early exit: if any VC stalls beyond threshold
                static const Tick STALL_THRESHOLD = 10000;
                int num_vcs = m_input_unit[i]->get_num_vcs();
                for (int v = 0; v < num_vcs; v++) {
                    if (m_input_unit[i]->getVcStallCycles(v)
                            >= STALL_THRESHOLD) {
                        // Write deadlock info to log file
                        {
                            std::ofstream logf("m5out/deadlock.log",
                                               std::ios::app);
                            logf << "[DEADLOCK DETECTED] tick="
                                 << curTick()
                                 << " Router " << m_id
                                 << " port" << i << "(Up) vc" << v
                                 << " sz="
                                 << m_input_unit[i]->getVcBufferSize(v)
                                 << " stall="
                                 << m_input_unit[i]->getVcStallCycles(v)
                                 << " cycles (threshold="
                                 << STALL_THRESHOLD << ")"
                                 << std::endl;
                            logf.close();
                        }
                        // Trigger normal stats dump before exit
                        exitSimLoop("Deadlock detected: interposer VC "
                                    "queue stalled", 1);
                        return;
                    }
                }
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
Router::route_compute(RouteInfo route, int inport, PortDirection inport_dirn)
{
    return routingUnit.outportCompute(route, inport, inport_dirn);
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

} // namespace garnet
} // namespace ruby
} // namespace gem5
