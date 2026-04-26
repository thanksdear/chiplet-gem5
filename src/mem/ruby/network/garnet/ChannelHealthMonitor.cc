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


#include "mem/ruby/network/garnet/ChannelHealthMonitor.hh"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace gem5
{

namespace ruby
{

namespace garnet
{

ChannelHealthMonitor::ChannelHealthMonitor(int total_credits,
                                           Tick max_stall_threshold,
                                           float alpha)
    : m_total_credits(total_credits),
      m_free_credits(total_credits),
      m_credit_changes(0),
      m_has_credit_activity(false),
      m_last_credit_change_time(0),
      m_max_stall_threshold(max_stall_threshold),
      m_alpha(alpha),
      m_last_broadcast_score(7),
      m_last_broadcast_time(0),
      m_max_stall_observed(0)
{
    assert(total_credits > 0);
    assert(max_stall_threshold > 0);
    assert(alpha >= 0.0f && alpha <= 1.0f);
}

void
ChannelHealthMonitor::recordCreditChange(Tick current_tick)
{
    // Only count stall if there were flits in flight (not idle)
    if (m_has_credit_activity && current_tick > m_last_credit_change_time
        && m_free_credits < m_total_credits) {
        Tick stall = current_tick - m_last_credit_change_time;
        if (stall > m_max_stall_observed)
            m_max_stall_observed = stall;
    }
    m_credit_changes++;
    m_has_credit_activity = true;
    m_last_credit_change_time = current_tick;
}

void
ChannelHealthMonitor::updateFreeCredits(int free_credits, Tick current_tick)
{
    assert(free_credits >= 0 && free_credits <= m_total_credits);
    if (free_credits != m_free_credits) {
        recordCreditChange(current_tick);
    }
    m_free_credits = free_credits;
}

float
ChannelHealthMonitor::computeScore(Tick current_tick)
{
    if (isChannelDead(current_tick))
        return 0.0f;

    float ratio = computeCreditRatio();
    float penalty = computeStallPenalty(current_tick);

    float score = m_alpha * ratio + (1.0f - m_alpha) * penalty;
    return std::max(0.0f, std::min(1.0f, score));
}

int
ChannelHealthMonitor::quantizeScore(Tick current_tick)
{
    if (isChannelDead(current_tick))
        return 0;

    float score = computeScore(current_tick);
    int q = (int)(score * 7.0f);
    // S=0 is reserved for isChannelDead(), non-dead channel minimum is 1
    return std::max(1, q);
}

bool
ChannelHealthMonitor::isDeadlocked(Tick current_tick)
{
    return isChannelDead(current_tick);
}

bool
ChannelHealthMonitor::needsImmediateBroadcast(Tick current_tick,
                                               int change_threshold)
{
    int current = quantizeScore(current_tick);
    if (current == 0)
        return true;
    if (std::abs(current - m_last_broadcast_score) >= change_threshold)
        return true;
    return false;
}

void
ChannelHealthMonitor::recordBroadcast(Tick current_tick)
{
    m_last_broadcast_score = quantizeScore(current_tick);
    m_last_broadcast_time = current_tick;
}

float
ChannelHealthMonitor::computeCreditRatio()
{
    if (m_total_credits == 0)
        return 0.0f;
    return (float)m_free_credits / (float)m_total_credits;
}

float
ChannelHealthMonitor::computeStallPenalty(Tick current_tick)
{
    // Fully idle channel (all credits free) is healthy, not stalled
    if (m_free_credits == m_total_credits)
        return 1.0f;

    if (current_tick <= m_last_credit_change_time)
        return 1.0f;

    Tick stall_time = current_tick - m_last_credit_change_time;
    float penalty = 1.0f - (float)stall_time / (float)m_max_stall_threshold;
    return std::max(0.0f, penalty);
}

bool
ChannelHealthMonitor::isChannelDead(Tick current_tick)
{
    // If all credits are free, the channel is idle (no flits in flight),
    // not deadlocked. Avoid false positives on low-traffic corner routers.
    if (m_free_credits == m_total_credits)
        return false;

    // No activity yet — channel hasn't started, not dead
    if (!m_has_credit_activity)
        return false;

    // Channel is dead when stall time exceeds threshold
    if (current_tick <= m_last_credit_change_time)
        return false;

    Tick stall_time = current_tick - m_last_credit_change_time;
    return stall_time > m_max_stall_threshold;
}

Tick
ChannelHealthMonitor::getMaxStallTicks() const
{
    return m_max_stall_observed;
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
