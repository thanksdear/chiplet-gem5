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


#ifndef __MEM_RUBY_NETWORK_GARNET_0_INPUTUNIT_HH__
#define __MEM_RUBY_NETWORK_GARNET_0_INPUTUNIT_HH__

#include <iostream>
#include <vector>

#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/network/garnet/CommonTypes.hh"
#include "mem/ruby/network/garnet/CreditLink.hh"
#include "mem/ruby/network/garnet/NetworkLink.hh"
#include "mem/ruby/network/garnet/Router.hh"
#include "mem/ruby/network/garnet/VirtualChannel.hh"
#include "mem/ruby/network/garnet/flitBuffer.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

class InputUnit : public Consumer
{
  public:
    InputUnit(int id, PortDirection direction, Router *router);
    ~InputUnit() = default;

    void wakeup();
    void print(std::ostream& out) const {};

    inline PortDirection get_direction() { return m_direction; }

    inline void
    set_vc_idle(int vc, Tick curTime)
    {
        virtualChannels[vc].set_idle(curTime);
    }

    inline void
    set_vc_active(int vc, Tick curTime)
    {
        virtualChannels[vc].set_active(curTime);
    }

    inline void
    grant_outport(int vc, int outport)
    {
        virtualChannels[vc].set_outport(outport);
    }

    inline void
    grant_outvc(int vc, int outvc)
    {
        virtualChannels[vc].set_outvc(outvc);
    }

    inline int
    get_outport(int invc)
    {
        return virtualChannels[invc].get_outport();
    }

    inline int
    get_outvc(int invc)
    {
        return virtualChannels[invc].get_outvc();
    }

    inline Tick
    get_enqueue_time(int invc)
    {
        return virtualChannels[invc].get_enqueue_time();
    }

    void increment_credit(int in_vc, bool free_signal, Tick curTime);

    inline flit*
    peekTopFlit(int vc)
    {
        return virtualChannels[vc].peekTopFlit();
    }

    inline flit*
    getTopFlit(int vc)
    {
        return virtualChannels[vc].getTopFlit();
    }

    inline bool
    need_stage(int vc, flit_stage stage, Tick time)
    {
        return virtualChannels[vc].need_stage(stage, time);
    }

    inline bool
    isReady(int invc, Tick curTime)
    {
        return virtualChannels[invc].isReady(curTime);
    }

    flitBuffer* getCreditQueue() { return &creditQueue; }

    inline void
    set_in_link(NetworkLink *link)
    {
        m_in_link = link;
    }

    inline int get_inlink_id() { return m_in_link->get_id(); }

    inline void
    set_credit_link(CreditLink *credit_link)
    {
        m_credit_link = credit_link;
    }

    double get_buf_read_activity(unsigned int vnet) const
    { return m_num_buffer_reads[vnet]; }
    double get_buf_write_activity(unsigned int vnet) const
    { return m_num_buffer_writes[vnet]; }

    uint32_t functionalWrite(Packet *pkt);
    void resetStats();

    // Returns number of VCs currently in ACTIVE_ state on this input port
    int get_num_active_vcs() {
        int count = 0;
        for (auto &vc : virtualChannels)
            if (vc.get_state() == ACTIVE_) count++;
        return count;
    }

    int get_num_vcs() { return virtualChannels.size(); }

    // VC occupancy duration queries
    VC_state_type get_vc_state(int vc) { return virtualChannels[vc].get_state(); }
    Tick get_vc_active_start(int vc) { return virtualChannels[vc].get_active_start(); }
    Tick get_vc_last_duration(int vc) { return virtualChannels[vc].get_last_occupy_duration(); }

    // VC queue stall detection: returns how many cycles the queue size
    // has been unchanged for each VC. Call sampleVcStall() every wakeup.
    void sampleVcStall() {
        int num_vcs = virtualChannels.size();
        if (m_prev_vc_size.empty()) {
            m_prev_vc_size.resize(num_vcs, 0);
            m_vc_stall_cycles.resize(num_vcs, 0);
            m_vc_max_stall.resize(num_vcs, 0);
        }
        for (int v = 0; v < num_vcs; v++) {
            int cur_size = virtualChannels[v].getBufferSize();
            if (cur_size > 0 && cur_size == m_prev_vc_size[v]) {
                m_vc_stall_cycles[v]++;
            } else {
                // queue changed or empty, reset
                if (m_vc_stall_cycles[v] > m_vc_max_stall[v])
                    m_vc_max_stall[v] = m_vc_stall_cycles[v];
                m_vc_stall_cycles[v] = 0;
            }
            m_prev_vc_size[v] = cur_size;
        }
    }
    int getVcBufferSize(int vc) { return virtualChannels[vc].getBufferSize(); }
    Tick getVcStallCycles(int vc) { return m_vc_stall_cycles.empty() ? 0 : m_vc_stall_cycles[vc]; }
    Tick getVcMaxStall(int vc) { return m_vc_max_stall.empty() ? 0 : m_vc_max_stall[vc]; }

  private:
    Router *m_router;
    int m_id;
    PortDirection m_direction;
    int m_vc_per_vnet;
    NetworkLink *m_in_link;
    CreditLink *m_credit_link;
    flitBuffer creditQueue;

    // Input Virtual channels
    std::vector<VirtualChannel> virtualChannels;

    // Statistical variables
    std::vector<double> m_num_buffer_writes;
    std::vector<double> m_num_buffer_reads;

    // VC stall detection
    std::vector<int> m_prev_vc_size;       // previous queue size per VC
    std::vector<Tick> m_vc_stall_cycles;   // current stall streak per VC
    std::vector<Tick> m_vc_max_stall;      // max stall streak per VC
};

} // namespace garnet
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_GARNET_0_INPUTUNIT_HH__
