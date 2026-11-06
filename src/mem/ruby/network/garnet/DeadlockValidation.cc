// Simulator-only observers for the opt-in forced VC dependency cycles.
// These checks never grant a VC, return a credit, or alter routing.
#include <fstream>

#include "base/logging.hh"
#include "mem/ruby/network/garnet/GarnetNetwork.hh"
#include "mem/ruby/network/garnet/InputUnit.hh"
#include "mem/ruby/network/garnet/OutputUnit.hh"
#include "mem/ruby/network/garnet/Router.hh"
#include "mem/ruby/network/garnet/flit.hh"

namespace gem5 { namespace ruby { namespace garnet {

void
Router::armDeadlockTest()
{
    if (!m_is_interposer)
        return;
    for (auto &input : m_input_unit)
        input->resetStallObservation();
    for (auto &output : m_output_unit)
        output->resetHealthObservation(curTick());
    m_pending_health_updates.clear();
    m_neighbor_health_table.clear();
    m_neighbor_health_sent_tick.clear();
    m_neighbor_health_path_hops.clear();
    m_up_input_stall = false;
    m_up_output_stall = false;
    m_recovery_cooldown = 0;
    m_deadlock_test_health_epoch = curTick();
    schedule_wakeup(Cycles(1));
}

bool
GarnetNetwork::verifyDeadlockTestCycle(int ring, const char *phase)
{
    const int rb = ring * 16;
    const int ir = getFirstInterposerRouterId() + ring * 4;
    const int routers[] = {ir, ir + 1, rb + 3, rb + 2, rb + 1, rb};
    const char *inputs[] = {"Up", "West", "Down", "East", "East", "East"};
    const char *outputs[] = {"East", "Up", "West", "West", "West", "Down"};
    std::vector<InputUnit *> units;
    for (int g = 0; g < 6; ++g) {
        InputUnit *iu = nullptr;
        Router *r = m_routers.at(routers[g]);
        for (int p = 0; p < r->get_num_inports(); ++p)
            if (r->getInputUnit(p)->get_direction() == inputs[g])
                iu = r->getInputUnit(p);
        if (!iu)
            return false;
        units.push_back(iu);
    }
    const int base_vc = 2 * (m_deadlock_test_vcs + 1);
    bool closed = true;
    std::ofstream log("m5out/deadlock_audit.log", std::ios::app);
    for (int g = 0; g < 6; ++g) {
        Router *r = m_routers.at(routers[g]);
        InputUnit *iu = units[g];
        for (unsigned v = 0; v < m_deadlock_test_vcs; ++v) {
            const int vc = base_vc + v;
            bool held = iu->get_vc_state(vc) == ACTIVE_ &&
                        iu->getVcBufferSize(vc) > 0;
            int out = held ? iu->get_outport(vc) : -1;
            bool edge = false;
            if (held && out >= 0 && out < r->get_num_outports()) {
                flit *f = iu->peekTopFlit(vc);
                OutputUnit *ou = r->getOutputUnit(out);
                edge = f->get_type() == HEAD_ &&
                       f->get_route().src_router == rb + g &&
                       iu->get_outvc(vc) == -1 &&
                       ou->get_direction() == outputs[g] &&
                       ou->get_out_link() ==
                           units[(g + 1) % 6]->get_in_link() &&
                       !ou->has_free_vc(2, false);
            }
            closed = closed && edge;
            log << "[DEPENDENCY EDGE] tick=" << curTick()
                << " ring=" << ring << " phase=" << phase
                << " group=" << g << " router=" << routers[g]
                << " vc=" << vc << " occupied=" << held
                << " requested=" << outputs[g] << " blocked_edge=" << edge
                << std::endl;
        }
    }
    log << "[CYCLE VERIFIED] tick=" << curTick() << " ring=" << ring
        << " phase=" << phase << " closed=" << closed
        << " blocked_vcs=" << 6 * m_deadlock_test_vcs << std::endl;
    return closed;
}

void
GarnetNetwork::observeDeadlockTest()
{
    if (!m_deadlock_test_enabled || !m_deadlock_test_formation_tick ||
        curTick() <= m_deadlock_test_formation_tick ||
        m_validation_last_sample == curTick())
        return;
    m_validation_last_sample = curTick();
    for (unsigned ring = 0; ring < m_validation_rings.size(); ++ring) {
        auto &state = m_validation_rings[ring];
        if (!state.verified) {
            state.verified = verifyDeadlockTestCycle(ring, "formed");
            fatal_if(!state.verified,
                     "Forced deadlock ring %u did not form a live VC cycle",
                     ring);
        }
    }
}

void
GarnetNetwork::recordDeadlockTestDetection(int router_id, Tick tick)
{
    if (!m_deadlock_test_enabled)
        return;
    int ring = (router_id - getFirstInterposerRouterId()) / 4;
    if (ring < 0 || ring >= static_cast<int>(m_validation_rings.size()))
        return;
    auto &state = m_validation_rings[ring];
    ++state.detection_events;
    if (state.detected)
        return;
    fatal_if(!verifyDeadlockTestCycle(ring, "detection"),
             "Detector fired without the injected VC cycle on ring %d", ring);
    state.detected = tick;
    if (!m_deadlock_test_detection_tick)
        m_deadlock_test_detection_tick = tick;
    const Tick period = m_routers.at(router_id)->clockPeriod();
    std::ofstream log("m5out/deadlock_audit.log", std::ios::app);
    log << "[RING DETECTED] ring=" << ring << " tick=" << tick
        << " detection_latency_cycles="
        << (tick - m_deadlock_test_formation_tick) / period << std::endl;
}

int
GarnetNetwork::registerDeadlockTestPacket(const RouteInfo &route, int flits)
{
    if (!m_deadlock_test_enabled)
        return -1;
    unsigned sequence = 0;
    for (const auto &p : m_validation_packets)
        if (p.route.src_ni == route.src_ni &&
            p.route.dest_ni == route.dest_ni && p.route.vnet == route.vnet)
            ++sequence;
    const int id = m_validation_packets.size();
    m_validation_packets.push_back({route, flits, sequence});
    std::ofstream log("m5out/deadlock_audit.log", std::ios::app);
    log << "[PACKET INJECTED] tick=" << curTick() << " packet=" << id
        << " ring=" << route.src_router / 16
        << " src_ni=" << route.src_ni << " dest_ni=" << route.dest_ni
        << " sequence=" << sequence << " flits=" << flits
        << " ordered_vnet=" << isVNetOrdered(route.vnet) << std::endl;
    return id;
}

void
GarnetNetwork::recordDeadlockTestFlit(flit *f, int dest_ni)
{
    if (!m_deadlock_test_enabled)
        return;
    const int id = f->get_validation_packet();
    fatal_if(id < 0 || id >= static_cast<int>(m_validation_packets.size()),
             "Unknown forced-deadlock packet id %d", id);
    auto &p = m_validation_packets[id];
    const RouteInfo route = f->get_route();
    fatal_if(p.delivered || f->get_id() != p.received ||
             f->get_size() != p.flits || dest_ni != p.route.dest_ni ||
             route.src_ni != p.route.src_ni ||
             route.dest_ni != p.route.dest_ni ||
             f->get_vnet() != p.route.vnet,
             "Forced-deadlock packet %d duplicate, misroute "
             "or flit-order error: "
             "expected flit %d, got %d", id, p.received, f->get_id());
    ++p.received;
    std::ofstream log("m5out/deadlock_audit.log", std::ios::app);
    log << "[FLIT RECEIVED] tick=" << curTick() << " packet=" << id
        << " flit_id=" << f->get_id() << " dest_ni=" << dest_ni << std::endl;
}

void
GarnetNetwork::recordDeadlockTestCycleBreak(int router_id, Tick tick)
{
    if (!m_deadlock_test_enabled)
        return;
    int ring = (router_id - getFirstInterposerRouterId()) / 4;
    if (ring < 0 || ring >= static_cast<int>(m_validation_rings.size()))
        return;
    auto &state = m_validation_rings[ring];
    if (!state.broken)
        state.broken = tick;
}

void
GarnetNetwork::recordDeadlockTestPacketDelivery(flit *f, Tick period)
{
    const Tick tick = curTick();
    std::ofstream log;
    if (f->is_recovery() || m_deadlock_test_enabled)
        log.open("m5out/deadlock.log", std::ios::app);
    const RouteInfo route = f->get_route();
    const int ring = route.src_router / 16;
    const Tick detected = m_deadlock_test_enabled ?
        m_validation_rings.at(ring).detected : 0;
    if (f->is_recovery()) {
        log << "[RECOVERED PACKET DELIVERED] tick=" << tick
            << " src_router=" << route.src_router
            << " dest_router=" << route.dest_router
            << " hops=" << route.hops_traversed;
        if (detected)
            log << " recovery_latency_cycles=" << (tick - detected) / period;
        log << std::endl;
    }
    if (!m_deadlock_test_enabled)
        return;
    auto &p = m_validation_packets.at(f->get_validation_packet());
    fatal_if(p.delivered || p.received != p.flits,
             "Incomplete or duplicate forced-deadlock packet delivery");
    unsigned overtaken = 0;
    for (const auto &older : m_validation_packets)
        if (older.route.src_ni == p.route.src_ni &&
            older.route.dest_ni == p.route.dest_ni &&
            older.route.vnet == p.route.vnet &&
            older.sequence < p.sequence && !older.delivered)
            ++overtaken;
    m_validation_flow_inversions += overtaken;
    p.delivered = true;
    std::ofstream audit("m5out/deadlock_audit.log", std::ios::app);
    audit << "[PACKET DELIVERED] tick=" << tick
          << " packet=" << f->get_validation_packet()
          << " ring=" << ring << " sequence=" << p.sequence
          << " overtaken_older_packets=" << overtaken << std::endl;
    auto &state = m_validation_rings.at(ring);
    ++state.delivered;
    ++m_deadlock_test_packets_delivered;
    if (state.delivered == 6 * m_deadlock_test_vcs) {
        state.drained = tick;
        audit << "[RING DRAINED] tick=" << tick << " ring=" << ring
              << " recovery_latency_cycles=" << (tick - detected) / period
              << std::endl;
    }
    const unsigned expected = 6 * m_deadlock_test_vcs * m_deadlock_test_rings;
    if (m_deadlock_test_packets_delivered == expected) {
        log << "[DEADLOCK FULL RECOVERY COMPLETE] tick=" << tick
            << " packets=" << expected << " recovery_latency_cycles="
            << (tick - m_deadlock_test_detection_tick) / period << std::endl;
    }
}

void
GarnetNetwork::writeDeadlockTestSummary()
{
    // Called at simulation exit, including cases with an undetected ring.
    std::ofstream csv("m5out/deadlock_rings.csv");
    csv << "ring,formed,verified,detected,missed,detect_events,formation_tick,"
           "detection_tick,cycle_break_tick,drain_tick,detection_cycles,"
           "release_cycles,recovery_cycles,packets_expected,packets_delivered,"
           "still_closed_at_exit\n";
    const Tick period = m_routers.front()->clockPeriod();
    for (unsigned ring = 0; ring < m_validation_rings.size(); ++ring) {
        const auto &s = m_validation_rings[ring];
        const bool closed = m_deadlock_test_disable_recovery &&
            m_deadlock_test_formation_tick &&
            verifyDeadlockTestCycle(ring, "exit");
        csv << ring << ',' << (m_deadlock_test_formation_tick != 0) << ','
            << s.verified << ',' << (s.detected != 0) << ','
            << (s.verified && !s.detected) << ',' << s.detection_events << ','
            << m_deadlock_test_formation_tick << ',' << s.detected << ','
            << s.broken << ',' << s.drained << ',';
        if (s.detected)
            csv << (s.detected - m_deadlock_test_formation_tick) / period;
        csv << ',';
        if (s.detected && s.broken)
            csv << (s.broken - s.detected) / period;
        csv << ',';
        if (s.detected && s.drained)
            csv << (s.drained - s.detected) / period;
        csv << ',' << 6 * m_deadlock_test_vcs << ',' << s.delivered << ','
            << closed << '\n';
    }
    std::ofstream audit("m5out/deadlock_audit.log", std::ios::app);
    unsigned received = 0;
    for (const auto &p : m_validation_packets)
        received += p.received;
    audit << "[VALIDATION TOTALS] tick=" << curTick()
          << " packets_injected=" << m_validation_packets.size()
          << " packets_delivered=" << m_deadlock_test_packets_delivered
          << " flits_received=" << received
          << " flow_inversions=" << m_validation_flow_inversions
          << " ordered_vnet=" << isVNetOrdered(2) << std::endl;
}

} } } // namespace gem5::ruby::garnet
