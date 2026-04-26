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

#ifndef __MEM_RUBY_NETWORK_GARNET_0_ESCAPEBUFFER_HH__
#define __MEM_RUBY_NETWORK_GARNET_0_ESCAPEBUFFER_HH__

#include <deque>
#include <vector>

#include "base/types.hh"
#include "mem/ruby/network/garnet/CommonTypes.hh"
#include "mem/ruby/network/garnet/flit.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

class Router;
class InputUnit;

// Escape buffer for deadlock recovery on interposer routers.
//
// When a deadlock is detected (peer S=0 + local down-channel blocked),
// the escape buffer absorbs the head packet's flits from the blocked
// input VC, freeing the VC for other traffic. Later, when a free VC
// becomes available on the same input port, the flits are re-injected
// and re-routed through normal pipeline stages.
//
// Each interposer router has one EscapeBuffer per "Down" input port
// (the port receiving flits from the chiplet above).

class EscapeBuffer
{
  public:
    EscapeBuffer(Router *router, int inport, int capacity);
    ~EscapeBuffer() = default;

    // --- Phase 1: Absorb ---
    // Start absorbing the head packet from the given VC.
    // Returns true if absorption started (buffer was free and VC had flits).
    bool startAbsorb(InputUnit *input_unit, int vc, Tick curTick);

    // Continue absorbing body/tail flits that arrive in subsequent cycles.
    // Returns true when the full packet (up to TAIL) has been absorbed.
    bool continueAbsorb(InputUnit *input_unit, Tick curTick);

    // --- Phase 2: Re-inject ---
    // Try to re-inject the buffered packet into a free VC on the same
    // input port. Returns true if re-injection completed or is in progress.
    bool tryReinject(InputUnit *input_unit, Router *router, Tick curTick);

    // --- Force re-inject ---
    // Force-occupy a VC when max wait time exceeded.
    // Returns true if a VC was seized and re-injection started.
    bool forceReinject(InputUnit *input_unit, Router *router, Tick curTick);

    // --- Queries ---
    bool isOccupied() const { return m_occupied; }
    bool isEmpty() const { return m_buffer.empty(); }
    bool isAbsorbing() const { return m_absorbing; }
    int getSourceVc() const { return m_source_vc; }
    Tick getAbsorbTime() const { return m_absorb_time; }
    int getReinjectVc() const { return m_reinject_vc; }
    int getFlitCount() const { return m_buffer.size(); }

    // Check if waiting too long for re-injection
    bool isWaitExpired(Tick curTick, Tick max_wait) const;

    // Statistics
    uint64_t getAbsorbCount() const { return m_absorb_count; }
    uint64_t getReinjectCount() const { return m_reinject_count; }
    uint64_t getForceReinjectCount() const { return m_force_reinject_count; }

  private:
    // Send credits back to upstream for each absorbed flit
    void sendCreditsUpstream(InputUnit *input_unit, int vc,
                             int num_flits, bool last_is_tail, Tick curTick);

    // Find a free VC in the same vnet on the given input port
    int findFreeVc(InputUnit *input_unit, int vnet);

    // Re-insert all buffered flits into the target VC
    void doReinject(InputUnit *input_unit, Router *router,
                    int target_vc, Tick curTick);

    Router *m_router;
    int m_inport;        // which input port this escape buffer serves
    int m_capacity;      // max flits (= VC buffer depth)

    // Buffer state
    bool m_occupied;     // buffer holds a packet
    bool m_absorbing;    // still absorbing (waiting for tail)
    int m_source_vc;     // VC we absorbed from
    int m_source_vnet;   // vnet of the absorbed packet
    Tick m_absorb_time;  // tick when absorption started
    int m_reinject_vc;   // VC we are re-injecting into (-1 if not yet)

    std::deque<flit*> m_buffer;  // absorbed flits

    // Statistics
    uint64_t m_absorb_count;
    uint64_t m_reinject_count;
    uint64_t m_force_reinject_count;
};

} // namespace garnet
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_GARNET_0_ESCAPEBUFFER_HH__
