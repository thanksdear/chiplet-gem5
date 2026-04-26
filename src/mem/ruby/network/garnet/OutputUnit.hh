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


#ifndef __MEM_RUBY_NETWORK_GARNET_0_OUTPUTUNIT_HH__
#define __MEM_RUBY_NETWORK_GARNET_0_OUTPUTUNIT_HH__

#include <iostream>
#include <vector>

#include <memory>

#include "base/compiler.hh"
#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/network/garnet/ChannelHealthMonitor.hh"
#include "mem/ruby/network/garnet/CommonTypes.hh"
#include "mem/ruby/network/garnet/NetworkLink.hh"
#include "mem/ruby/network/garnet/OutVcState.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

class CreditLink;
class Router;

class OutputUnit : public Consumer
{
  public:
    OutputUnit(int id, PortDirection direction, Router *router,
               uint32_t consumerVcs);
    ~OutputUnit() = default;
    void set_out_link(NetworkLink *link);
    NetworkLink* get_out_link() { return m_out_link; }
    void set_credit_link(CreditLink *credit_link);
    void wakeup();
    flitBuffer* getOutQueue();
    void print(std::ostream& out) const {};
    void decrement_credit(int out_vc);
    void increment_credit(int out_vc);
    bool has_credit(int out_vc);
    bool has_free_vc(int vnet);
    int select_free_vc(int vnet);

    inline PortDirection get_direction() { return m_direction; }

    int
    get_credit_count(int vc)
    {
        return outVcState[vc].get_credit_count();
    }

    inline int
    get_outlink_id()
    {
        return m_out_link->get_id();
    }

    inline void
    set_vc_state(VC_state_type state, int vc, Tick curTime)
    {
      outVcState[vc].setState(state, curTime);
    }

    inline bool
    is_vc_idle(int vc, Tick curTime)
    {
        return (outVcState[vc].isInState(IDLE_, curTime));
    }

    void insert_flit(flit *t_flit);

    inline int
    getVcsPerVnet()
    {
        return m_vc_per_vnet;
    }

    uint32_t functionalWrite(Packet *pkt);

    // Initialize per-VC health monitors (called after port is fully connected)
    // vc_start/vc_end: only monitor VCs in [vc_start, vc_end) range (vnet 2)
    void initHealthMonitor(Tick max_stall_threshold,
                           int vc_start, int vc_end,
                           float alpha = 0.50f);

    // Update health monitor each cycle; returns true if broadcast needed
    bool updateHealthMonitor(Tick current_tick, int broadcast_interval,
                             int change_threshold);

    // Sync free credits into health monitors (call before computing score)
    void syncHealthCredits(Tick current_tick);

    // Get worst (min) quantized score across all VCs — for deadlock detection
    int getWorstQuantizedScore(Tick current_tick);

    // Get average quantized score across all VCs — for routing optimization
    int getAverageQuantizedScore(Tick current_tick);

    // Check deadlock via health monitor (any VC dead → deadlocked)
    bool isHealthDeadlocked(Tick current_tick);

    // Record that a broadcast was sent
    void recordHealthBroadcast(Tick current_tick);

    // Check if health monitor is initialized
    bool hasHealthMonitor() const;

    // Sample health score into histogram (call once per cycle)
    void sampleHealthScore(Tick current_tick);

    // Get histogram: count of each quantized score [0..7]
    const std::vector<uint64_t>& getHealthScoreHistogram() const
    { return m_health_histogram; }

    // Get per-VC quantized score (idx = vc - vc_start)
    int getVcQuantizedScore(int idx, Tick current_tick);

    // Get per-VC max stall ticks (idx = vc - vc_start)
    Tick getVcMaxStallTicks(int idx) const;

    // Get number of monitored VCs
    int getNumMonitoredVcs() const
    { return m_monitor_vc_end - m_monitor_vc_start; }

    // Get monitored VC start index
    int getMonitorVcStart() const { return m_monitor_vc_start; }


  private:
    Router *m_router;
    GEM5_CLASS_VAR_USED int m_id;
    PortDirection m_direction;
    int m_vc_per_vnet;
    NetworkLink *m_out_link;
    CreditLink *m_credit_link;

    // This is for the network link to consume
    flitBuffer outBuffer;
    // vc state of downstream router
    std::vector<OutVcState> outVcState;

    // Per-VC health monitors (created for Up output ports on interposer)
    std::vector<std::unique_ptr<ChannelHealthMonitor>> m_vc_health_monitors;
    Tick m_last_periodic_broadcast = 0;
    int m_monitor_vc_start = 0;  // first VC to monitor (vnet 2 start)
    int m_monitor_vc_end = 0;    // one past last VC to monitor
    int m_monitor_total_credits = 0; // total credits for monitored VCs

    // Histogram of quantized health scores [0..7], index = score
    std::vector<uint64_t> m_health_histogram = std::vector<uint64_t>(8, 0);
};

} // namespace garnet
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_GARNET_0_OUTPUTUNIT_HH__
