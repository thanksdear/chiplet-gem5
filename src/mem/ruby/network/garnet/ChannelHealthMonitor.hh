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


#ifndef __MEM_RUBY_NETWORK_GARNET_CHANNEL_HEALTH_MONITOR_HH__
#define __MEM_RUBY_NETWORK_GARNET_CHANNEL_HEALTH_MONITOR_HH__

#include <cassert>
#include <cmath>

#include "base/types.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

class ChannelHealthMonitor
{
  public:
    // S = alpha * (C_free/C_total) + (1-alpha) * (1 - T_stall/T_max)
    ChannelHealthMonitor(int total_credits, Tick max_stall_threshold,
                         float alpha = 0.0f);

    // Called when credit count changes (increment or decrement)
    void recordCreditChange(Tick current_tick);

    // Called every cycle to sync current free credit count
    void updateFreeCredits(int free_credits, Tick current_tick);

    // Compute health score S in [0.0, 1.0]
    float computeScore(Tick current_tick);

    // Quantize score to [0, 7]
    int quantizeScore(Tick current_tick);

    // Check if S == 0 (deadlock condition)
    bool isDeadlocked(Tick current_tick);

    // Check if score change is large enough for immediate broadcast
    bool needsImmediateBroadcast(Tick current_tick, int change_threshold);

    // Record that a broadcast was sent
    void recordBroadcast(Tick current_tick);

    // Get max stall duration observed (ticks)
    Tick getMaxStallTicks() const;

  private:
    // Component 1: credit occupancy ratio C_free / C_total
    float computeCreditRatio();

    // Component 2: stall time penalty 1 - T_stall / T_max
    float computeStallPenalty(Tick current_tick);

    // Check forced-zero condition: change rate == 0 AND stall > threshold
    bool isChannelDead(Tick current_tick);

    // Total credit count (fixed at init)
    int m_total_credits;
    // Current free credits
    int m_free_credits;
    // Credit change count within current window
    int m_credit_changes;
    // Whether any credit change has been observed
    bool m_has_credit_activity;
    // Time of last credit change
    Tick m_last_credit_change_time;
    // Maximum stall threshold T_max
    Tick m_max_stall_threshold;
    // Weight for credit ratio (stall weight = 1 - alpha)
    float m_alpha;
    // Last broadcast quantized score
    int m_last_broadcast_score;
    // Last broadcast time
    Tick m_last_broadcast_time;
    // Max stall duration observed
    Tick m_max_stall_observed;
};

} // namespace garnet
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_GARNET_CHANNEL_HEALTH_MONITOR_HH__
