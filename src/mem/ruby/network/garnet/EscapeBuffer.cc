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
      m_absorb_time(0),
      m_reinject_vc(-1),
      m_reinject_time(0),
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
    m_absorb_time = curTick;
    m_occupied = true;
    m_absorbing = true;
    m_reinject_vc = -1;
    fatal_if(!m_buffer.empty(),
             "EscapeBuffer Router %d started with stale flits",
             m_router->get_id());

    m_absorb_count++;

    {
        std::ofstream log("m5out/deadlock.log", std::ios::app);
        log << "[ESCAPE ABSORB START] tick=" << curTick
            << " Router " << m_router->get_id()
            << " inport=" << m_inport
            << " vc=" << vc
            << " flits=0 complete=0 bandwidth_flits_per_cycle=1"
            << std::endl;
    }

    DPRINTF(RubyNetwork,
            "EscapeBuffer Router %d inport %d: started absorb from vc %d\n",
            m_router->get_id(), m_inport, vc);

    m_router->schedule_wakeup(Cycles(1));

    return true;
}

bool
EscapeBuffer::continueAbsorb(InputUnit *input_unit, Tick curTick)
{
    if (!m_absorbing)
        return true;  // already done

    int vc = m_source_vc;

    // A single-ported escape buffer accepts at most one flit per cycle.
    if (input_unit->getVcBufferSize(vc) == 0) {
        m_router->schedule_wakeup(Cycles(1));
        return false;
    }

    fatal_if((int)m_buffer.size() >= m_capacity,
             "EscapeBuffer Router %d overflow while absorbing vc %d",
             m_router->get_id(), vc);

    flit *f = input_unit->getTopFlit(vc);
    m_buffer.push_back(f);
    bool is_tail = (f->get_type() == TAIL_ ||
                    f->get_type() == HEAD_TAIL_);
    if (!f->is_escape_reinjected())
        input_unit->increment_credit(vc, is_tail, curTick);

    if (is_tail) {
        m_absorbing = false;
        input_unit->set_vc_idle(vc, curTick);

        std::ofstream log("m5out/deadlock.log", std::ios::app);
        log << "[ESCAPE ABSORB COMPLETE] tick=" << curTick
            << " Router " << m_router->get_id()
            << " flits=" << m_buffer.size()
            << " absorption_cycles="
            << (curTick - m_absorb_time) / m_router->clockPeriod()
            << std::endl;

        DPRINTF(RubyNetwork,
                "EscapeBuffer Router %d inport %d: absorb complete, "
                "total %d flits\n",
                m_router->get_id(), m_inport,
                (int)m_buffer.size());
        // Ensure reinjection begins next cycle even if no unrelated link
        // event happens to wake this router.
        m_router->schedule_wakeup(Cycles(1));
        return true;
    }

    m_router->schedule_wakeup(Cycles(1));
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

    // The final data VC is reserved end-to-end for escape traffic.  Ordinary
    // packets never allocate it, so local reinjection cannot collide with a
    // physical-link head arriving from upstream.
    int target_vc = m_source_vc - (m_source_vc %
        m_router->get_vc_per_vnet()) + m_router->get_vc_per_vnet() - 1;
    if (target_vc < 0 || input_unit->get_vc_state(target_vc) != IDLE_)
        return false;

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
        m_reinject_time = curTick;
        input_unit->set_vc_active(target_vc, curTick);

        flit *head = m_buffer.front();
        head->set_recovery(true);
        int outport = router->route_compute(head->get_route(),
                                            m_inport,
                                            input_unit->get_direction(),
                                            head);
        input_unit->grant_outport(target_vc, outport);

        std::ofstream log("m5out/deadlock.log", std::ios::app);
        log << "[ESCAPE REINJECT START] tick=" << curTick
            << " Router " << m_router->get_id()
            << " target_vc=" << target_vc
            << " bandwidth_flits_per_cycle=1" << std::endl;

        router->schedule_wakeup(Cycles(1));
        return;
    }

    const int vc_capacity =
        router->get_net_ptr()->getBuffersPerDataVC();
    int free_slots = vc_capacity - input_unit->getVcBufferSize(target_vc);
    if (!m_buffer.empty() && free_slots > 0) {
        flit *f = m_buffer.front();
        m_buffer.pop_front();

        // Update flit's VC to the new target
        f->set_vc(target_vc);
        // Credits for the original physical input were already returned
        // while the packet was absorbed.  Suppress the local reinjection
        // credit when this flit leaves the recovery router.
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
    }

    if (m_buffer.empty()) {
        const int outport = input_unit->get_outport(target_vc);
        std::ofstream log("m5out/deadlock.log", std::ios::app);
        log << "[ESCAPE REINJECTED] tick=" << curTick
            << " Router " << m_router->get_id()
            << " target_vc=" << target_vc
            << " outport=" << outport
            << " reinjection_cycles="
            << (curTick - m_reinject_time) / m_router->clockPeriod()
            << std::endl;

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
