/*
 * Copyright (c) 2024 All rights reserved.
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

#include "mem/ruby/network/garnet/EscapeBuffer.hh"

#include <cassert>
#include <fstream>

#include "base/logging.hh"
#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/garnet/InputUnit.hh"
#include "mem/ruby/network/garnet/Router.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

EscapeBuffer::EscapeBuffer(Router *router, int inport, int capacity)
    : m_router(router),
      m_inport(inport),
      m_capacity(capacity),
      m_occupied(false),
      m_absorbing(false),
      m_source_vc(-1),
      m_source_vnet(-1),
      m_absorb_time(0),
      m_reinject_vc(-1),
      m_absorb_count(0),
      m_reinject_count(0),
      m_force_reinject_count(0)
{
}

// ================================================================
// Phase 1: Absorb flits from blocked VC into escape buffer
// ================================================================

bool
EscapeBuffer::startAbsorb(InputUnit *input_unit, int vc, Tick curTick)
{
    // Cannot absorb if buffer already occupied
    if (m_occupied)
        return false;

    // VC must have at least one flit
    if (input_unit->getVcBufferSize(vc) == 0)
        return false;

    // Peek to verify it's a HEAD or HEAD_TAIL flit
    flit *head = input_unit->peekTopFlit(vc);
    assert(head->get_type() == HEAD_ || head->get_type() == HEAD_TAIL_);

    m_source_vc = vc;
    m_source_vnet = head->get_vnet();
    m_absorb_time = curTick;
    m_occupied = true;
    m_reinject_vc = -1;

    // Extract all flits currently in this VC that belong to the head packet
    int absorbed = 0;
    bool got_tail = false;

    while (input_unit->getVcBufferSize(vc) > 0) {
        flit *f = input_unit->getTopFlit(vc);
        m_buffer.push_back(f);
        absorbed++;

        if (f->get_type() == TAIL_ || f->get_type() == HEAD_TAIL_) {
            got_tail = true;
            break;
        }

        if (absorbed >= m_capacity)
            break;
    }

    // Send credits back upstream for each absorbed flit
    sendCreditsUpstream(input_unit, vc, absorbed, got_tail, curTick);

    if (got_tail) {
        // Full packet absorbed in one shot
        m_absorbing = false;
        // Free the source VC
        input_unit->set_vc_idle(vc, curTick);
    } else {
        // Partial absorption — need to continue in next cycles
        m_absorbing = true;
    }

    m_absorb_count++;

    {
        std::ofstream log("m5out/deadlock.log", std::ios::app);
        log << "[ESCAPE ABSORB START] tick=" << curTick
            << " Router " << m_router->get_id()
            << " inport=" << m_inport
            << " vc=" << vc
            << " flits=" << absorbed
            << " complete=" << got_tail << std::endl;
        if (got_tail) {
            log << "[ESCAPE ABSORB COMPLETE] tick=" << curTick
                << " Router " << m_router->get_id()
                << " flits=" << m_buffer.size() << std::endl;
        }
    }

    DPRINTF(RubyNetwork,
            "EscapeBuffer Router %d inport %d: started absorb from vc %d, "
            "absorbed %d flits, got_tail=%d\n",
            m_router->get_id(), m_inport, vc, absorbed, got_tail);

    return true;
}

bool
EscapeBuffer::continueAbsorb(InputUnit *input_unit, Tick curTick)
{
    if (!m_absorbing)
        return true;  // already done

    int vc = m_source_vc;

    // Extract flits that arrived this cycle
    while (input_unit->getVcBufferSize(vc) > 0 &&
           (int)m_buffer.size() < m_capacity) {
        flit *f = input_unit->getTopFlit(vc);
        m_buffer.push_back(f);

        // Send credit back for this flit
        bool is_tail = (f->get_type() == TAIL_ ||
                        f->get_type() == HEAD_TAIL_);
        sendCreditsUpstream(input_unit, vc, 1, is_tail, curTick);

        if (is_tail) {
            m_absorbing = false;
            // Free the source VC
            input_unit->set_vc_idle(vc, curTick);

            std::ofstream log("m5out/deadlock.log", std::ios::app);
            log << "[ESCAPE ABSORB COMPLETE] tick=" << curTick
                << " Router " << m_router->get_id()
                << " flits=" << m_buffer.size() << std::endl;

            DPRINTF(RubyNetwork,
                    "EscapeBuffer Router %d inport %d: absorb complete, "
                    "total %d flits\n",
                    m_router->get_id(), m_inport,
                    (int)m_buffer.size());
            return true;
        }
    }

    return false;  // still absorbing
}

// ================================================================
// Phase 2: Re-inject buffered flits into a free VC
// ================================================================

bool
EscapeBuffer::tryReinject(InputUnit *input_unit, Router *router, Tick curTick)
{
    if (!m_occupied || m_absorbing || m_buffer.empty())
        return false;

    // Continue a packet-level reinjection started in a prior cycle.
    if (m_reinject_vc >= 0) {
        doReinject(input_unit, router, m_reinject_vc, curTick);
        return true;
    }

    // Find a free VC in the same vnet on this input port
    int target_vc = findFreeVc(input_unit, m_source_vnet);
    if (target_vc < 0)
        return false;  // no free VC available

    doReinject(input_unit, router, target_vc, curTick);
    return true;
}

bool
EscapeBuffer::forceReinject(InputUnit *input_unit, Router *router,
                            Tick curTick)
{
    // Never overwrite an ACTIVE VC.  The former fallback could merge two
    // packets in one VC and invalidate both ordering and credit accounting.
    // A watchdog may report the wait, but reinjection remains legal only
    // when an IDLE VC exists.
    bool reinjected = tryReinject(input_unit, router, curTick);
    if (reinjected)
        m_force_reinject_count++;
    return reinjected;
}

// ================================================================
// Helpers
// ================================================================

void
EscapeBuffer::sendCreditsUpstream(InputUnit *input_unit, int vc,
                                  int num_flits, bool last_is_tail,
                                  Tick curTick)
{
    for (int i = 0; i < num_flits; i++) {
        bool free_signal = (i == num_flits - 1) && last_is_tail;
        input_unit->increment_credit(vc, free_signal, curTick);
    }
}

int
EscapeBuffer::findFreeVc(InputUnit *input_unit, int vnet)
{
    int vc_per_vnet = m_router->get_vc_per_vnet();
    int vc_base = vnet * vc_per_vnet;

    for (int v = vc_base; v < vc_base + vc_per_vnet; v++) {
        if (input_unit->get_vc_state(v) == IDLE_)
            return v;
    }
    return -1;
}

void
EscapeBuffer::doReinject(InputUnit *input_unit, Router *router,
                         int target_vc, Tick curTick)
{
    if (m_reinject_vc < 0) {
        fatal_if(m_buffer.empty(),
                 "EscapeBuffer Router %d attempted empty reinjection",
                 m_router->get_id());

        const int packet_flits = m_buffer.front()->get_size();
        int expected_id = 0;
        for (const flit *f : m_buffer) {
            fatal_if(f->get_id() != expected_id,
                     "EscapeBuffer Router %d flit ordering error: "
                     "expected id %d, got %d",
                     m_router->get_id(), expected_id, f->get_id());
            expected_id++;
        }
        fatal_if(expected_id != packet_flits ||
                 (m_buffer.back()->get_type() != TAIL_ &&
                  m_buffer.back()->get_type() != HEAD_TAIL_),
                 "EscapeBuffer Router %d incomplete packet: stored %d of "
                 "%d flits",
                 m_router->get_id(), expected_id, packet_flits);

        m_reinject_vc = target_vc;
        input_unit->set_vc_active(target_vc, curTick);

        flit *head = m_buffer.front();
        int outport = router->route_compute(head->get_route(),
                                            m_inport,
                                            input_unit->get_direction(),
                                            head);
        input_unit->grant_outport(target_vc, outport);
    }

    const int vc_capacity =
        router->get_net_ptr()->getBuffersPerDataVC();
    int free_slots = vc_capacity - input_unit->getVcBufferSize(target_vc);
    while (!m_buffer.empty() && free_slots > 0) {
        flit *f = m_buffer.front();
        m_buffer.pop_front();

        // Update flit's VC to the new target
        f->set_vc(target_vc);
        // Credits for this physical input were already returned while the
        // packet was absorbed.  Suppress exactly one second credit when this
        // flit leaves the reinjection router.
        f->set_escape_reinjected(true);
        f->set_recovery(true);

        // Re-enter SA stage
        Cycles pipe_stages = router->get_pipe_stages();
        if (pipe_stages == Cycles(1)) {
            f->advance_stage(SA_, curTick);
        } else {
            Cycles wait_time = pipe_stages - Cycles(1);
            f->advance_stage(SA_, router->clockEdge(wait_time));
        }

        input_unit->insertFlit(target_vc, f);
        free_slots--;
    }

    if (m_buffer.empty()) {
        const int outport = input_unit->get_outport(target_vc);
        std::ofstream log("m5out/deadlock.log", std::ios::app);
        log << "[ESCAPE REINJECTED] tick=" << curTick
            << " Router " << m_router->get_id()
            << " target_vc=" << target_vc
            << " outport=" << outport << std::endl;

        DPRINTF(RubyNetwork,
                "EscapeBuffer Router %d inport %d: reinjected into vc %d, "
                "outport %d\n",
                m_router->get_id(), m_inport, target_vc, outport);

        m_occupied = false;
        m_absorbing = false;
        m_source_vc = -1;
        m_reinject_vc = -1;
        m_reinject_count++;
    }

    // Process transferred flits and, if needed, continue streaming later.
    router->schedule_wakeup(Cycles(1));
}

bool
EscapeBuffer::isWaitExpired(Tick curTick, Tick max_wait) const
{
    if (!m_occupied || m_absorbing)
        return false;
    return (curTick - m_absorb_time) > max_wait;
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
