/*
 * Copyright (c) 2008 Princeton University
 * Copyright (c) 2016 Georgia Institute of Technology
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


#include "mem/ruby/network/garnet/RoutingUnit.hh"

#include "base/cast.hh"
#include "base/compiler.hh"
#include "sim/cur_tick.hh"
#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/garnet/InputUnit.hh"
#include "mem/ruby/network/garnet/OutputUnit.hh"
#include "mem/ruby/network/garnet/Router.hh"
#include "mem/ruby/slicc_interface/Message.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

RoutingUnit::RoutingUnit(Router *router)
{
    m_router = router;
    m_routing_table.clear();
    m_weight_table.clear();
}

void
RoutingUnit::addRoute(std::vector<NetDest>& routing_table_entry)
{
    if (routing_table_entry.size() > m_routing_table.size()) {
        m_routing_table.resize(routing_table_entry.size());
    }
    for (int v = 0; v < routing_table_entry.size(); v++) {
        m_routing_table[v].push_back(routing_table_entry[v]);
    }
}

void
RoutingUnit::addWeight(int link_weight)
{
    m_weight_table.push_back(link_weight);
}

bool
RoutingUnit::supportsVnet(int vnet, std::vector<int> sVnets)
{
    // If all vnets are supported, return true
    if (sVnets.size() == 0) {
        return true;
    }

    // Find the vnet in the vector, return true
    if (std::find(sVnets.begin(), sVnets.end(), vnet) != sVnets.end()) {
        return true;
    }

    // Not supported vnet
    return false;
}

/*
 * This is the default routing algorithm in garnet.
 * The routing table is populated during topology creation.
 * Routes can be biased via weight assignments in the topology file.
 * Correct weight assignments are critical to provide deadlock avoidance.
 */
int
RoutingUnit::lookupRoutingTable(int vnet, NetDest msg_destination)
{
    // First find all possible output link candidates
    // For ordered vnet, just choose the first
    // (to make sure different packets don't choose different routes)
    // For unordered vnet, randomly choose any of the links
    // To have a strict ordering between links, they should be given
    // different weights in the topology file

    int output_link = -1;
    int min_weight = INFINITE_;
    std::vector<int> output_link_candidates;
    int num_candidates = 0;

    // Identify the minimum weight among the candidate output links
    for (int link = 0; link < m_routing_table[vnet].size(); link++) {
        if (msg_destination.intersectionIsNotEmpty(
            m_routing_table[vnet][link])) {

        if (m_weight_table[link] <= min_weight)
            min_weight = m_weight_table[link];
        }
    }

    // Collect all candidate output links with this minimum weight
    for (int link = 0; link < m_routing_table[vnet].size(); link++) {
        if (msg_destination.intersectionIsNotEmpty(
            m_routing_table[vnet][link])) {

            if (m_weight_table[link] == min_weight) {
                num_candidates++;
                output_link_candidates.push_back(link);
            }
        }
    }

    if (output_link_candidates.size() == 0) {
        fatal("Fatal Error:: No Route exists from this Router.");
        exit(0);
    }

    // Randomly select any candidate output link
    int candidate = 0;
    if (!(m_router->get_net_ptr())->isVNetOrdered(vnet))
        candidate = rand() % num_candidates;

    output_link = output_link_candidates.at(candidate);
    return output_link;
}


void
RoutingUnit::addInDirection(PortDirection inport_dirn, int inport_idx)
{
    m_inports_dirn2idx[inport_dirn] = inport_idx;
    m_inports_idx2dirn[inport_idx]  = inport_dirn;
}

void
RoutingUnit::addOutDirection(PortDirection outport_dirn, int outport_idx)
{
    m_outports_dirn2idx[outport_dirn] = outport_idx;
    m_outports_idx2dirn[outport_idx]  = outport_dirn;
}

// outportCompute() is called by the InputUnit
// It calls the routing table by default.
// A template for adaptive topology-specific routing algorithm
// implementations using port directions rather than a static routing
// table is provided here.

int
RoutingUnit::outportCompute(RouteInfo route, int inport,
                            PortDirection inport_dirn, flit *t_flit)
{
    int outport = -1;

    if (route.dest_router == m_router->get_id()) {

        // Multiple NIs may be connected to this router,
        // all with output port direction = "Local"
        // Get exact outport id from table
        outport = lookupRoutingTable(route.vnet, route.net_dest);

        return outport;
    }

    // Routing Algorithm set in GarnetNetwork.py
    // Can be over-ridden from command line using --routing-algorithm = 1
    RoutingAlgorithm routing_algorithm =
        (RoutingAlgorithm) m_router->get_net_ptr()->getRoutingAlgorithm();

    switch (routing_algorithm) {
        case TABLE_:  outport =
            lookupRoutingTable(route.vnet, route.net_dest); break;
        case XY_:     outport =
            outportComputeXY(route, inport, inport_dirn); break;
        // any custom algorithm
        case CUSTOM_: outport =
            outportComputeCustom(route, inport, inport_dirn); break;
        case CHIPLET_XY_: outport =
            outportComputeChipletXY(route, inport, inport_dirn); break;
        case ADAPTIVE_CHIPLET_XY_: outport =
            outportComputeAdaptiveChipletXY(route, inport, inport_dirn, t_flit); break;
        case MTR_: outport =
            outportComputeMTR(route, inport, inport_dirn); break;
        case RC_: outport =
            outportComputeChipletXY(route, inport, inport_dirn); break;
        case IPDR_: outport =
            outportComputeChipletXY(route, inport, inport_dirn); break;
        default: outport =
            lookupRoutingTable(route.vnet, route.net_dest); break;
    }

    assert(outport != -1);
    return outport;
}

// XY routing implemented using port directions
// Only for reference purpose in a Mesh
// By default Garnet uses the routing table
int
RoutingUnit::outportComputeXY(RouteInfo route,
                              int inport,
                              PortDirection inport_dirn)
{
    PortDirection outport_dirn = "Unknown";

    [[maybe_unused]] int num_rows = m_router->get_net_ptr()->getNumRows();
    int num_cols = m_router->get_net_ptr()->getNumCols();
    assert(num_rows > 0 && num_cols > 0);

    int my_id = m_router->get_id();
    int my_x = my_id % num_cols;
    int my_y = my_id / num_cols;

    int dest_id = route.dest_router;
    int dest_x = dest_id % num_cols;
    int dest_y = dest_id / num_cols;

    int x_hops = abs(dest_x - my_x);
    int y_hops = abs(dest_y - my_y);

    bool x_dirn = (dest_x >= my_x);
    bool y_dirn = (dest_y >= my_y);

    // already checked that in outportCompute() function
    assert(!(x_hops == 0 && y_hops == 0));

    if (x_hops > 0) {
        if (x_dirn) {
            assert(inport_dirn == "Local" || inport_dirn == "West");
            outport_dirn = "East";
        } else {
            assert(inport_dirn == "Local" || inport_dirn == "East");
            outport_dirn = "West";
        }
    } else if (y_hops > 0) {
        if (y_dirn) {
            // "Local" or "South" or "West" or "East"
            assert(inport_dirn != "North");
            outport_dirn = "North";
        } else {
            // "Local" or "North" or "West" or "East"
            assert(inport_dirn != "South");
            outport_dirn = "South";
        }
    } else {
        // x_hops == 0 and y_hops == 0
        // this is not possible
        // already checked that in outportCompute() function
        panic("x_hops == y_hops == 0");
    }

    return m_outports_dirn2idx[outport_dirn];
}

// Composable routing for chiplet-based systems (ISCA 2018)
// - Intra-chiplet traffic  : XY routing (local chiplet coordinates)
// - Interposer / ingress / egress : routing table
// - Boundary router turn restriction: prohibit Down(in) -> Down(out)
int
RoutingUnit::outportComputeCustom(RouteInfo route,
                                 int inport,
                                 PortDirection inport_dirn)
{
    static const int CHIPLET_COLS        = 4;
    static const int ROUTERS_PER_CHIPLET = 16; // 4x4
    static const int NUM_CHIPLET_ROUTERS = 64; // 4 chiplets x 16

    int my_id   = m_router->get_id();
    int dest_id = route.dest_router;

    // 物理方向: interposer 在下，chiplet 在上
    // interposer 路由器有 "Up" 出口（向上发往 chiplet）
    // chiplet 边界路由器有 "Down" 出口（向下发往 interposer）
    bool is_interposer       = (m_outports_dirn2idx.count("Up")   > 0);
    bool is_chiplet_boundary = (m_outports_dirn2idx.count("Down") > 0);


    // ------------------------------------------------------------------
    // Case 1: Interposer router -> routing table
    // ------------------------------------------------------------------
    if (is_interposer) {
        return lookupRoutingTable(route.vnet, route.net_dest);
    }

    // ------------------------------------------------------------------
    // Case 2: Chiplet boundary router
    // ------------------------------------------------------------------
    if (is_chiplet_boundary) {
        int my_chiplet  = my_id / ROUTERS_PER_CHIPLET;
        bool same_chiplet = (dest_id < NUM_CHIPLET_ROUTERS) &&
                            (dest_id / ROUTERS_PER_CHIPLET == my_chiplet);


        // 2a: destination on same chiplet -> intra-chiplet XY
        if (same_chiplet) {
            int my_local   = my_id   % ROUTERS_PER_CHIPLET;
            int dest_local = dest_id % ROUTERS_PER_CHIPLET;
            int my_lx   = my_local   % CHIPLET_COLS;
            int my_ly   = my_local   / CHIPLET_COLS;
            int dest_lx = dest_local % CHIPLET_COLS;
            int dest_ly = dest_local / CHIPLET_COLS;

            PortDirection dirn;
            if (dest_lx != my_lx)
                dirn = (dest_lx > my_lx) ? "East" : "West";
            else
                dirn = (dest_ly > my_ly) ? "South" : "North";
            return m_outports_dirn2idx.at(dirn);
        }

        // 2b: destination on another chiplet (egress/ingress path)
        //     Turn restriction: Down(in) -> Down(out) is PROHIBITED
        //     to break cyclic resource dependencies across chiplets.
        if (inport_dirn == "Down") {
            // Packet arrived from interposer (从下方上来); must NOT go back down.
            // Use routing table to reach destination within this chiplet.
            return lookupRoutingTable(route.vnet, route.net_dest);
        }
        // Packet came from within the chiplet: send down to interposer.
        return m_outports_dirn2idx.at("Down");
    }

    // ------------------------------------------------------------------
    // Case 3: Normal intra-chiplet router -> XY routing
    // ------------------------------------------------------------------
    int my_chiplet  = my_id / ROUTERS_PER_CHIPLET;
    bool same_chiplet = (dest_id < NUM_CHIPLET_ROUTERS) &&
                        (dest_id / ROUTERS_PER_CHIPLET == my_chiplet);

    if (same_chiplet) {
        // Intra-chiplet XY
        int my_local   = my_id   % ROUTERS_PER_CHIPLET;
        int dest_local = dest_id % ROUTERS_PER_CHIPLET;
        int my_lx   = my_local   % CHIPLET_COLS;
        int my_ly   = my_local   / CHIPLET_COLS;
        int dest_lx = dest_local % CHIPLET_COLS;
        int dest_ly = dest_local / CHIPLET_COLS;

        PortDirection dirn;
        if (dest_lx != my_lx)
            dirn = (dest_lx > my_lx) ? "East" : "West";
        else
            dirn = (dest_ly > my_ly) ? "South" : "North";
        return m_outports_dirn2idx.at(dirn);
    }

    // Inter-chiplet: XY toward nearest corner boundary router,
    // then the boundary router handles the Down egress (toward interposer).
    int my_local = my_id % ROUTERS_PER_CHIPLET;
    int my_lx    = my_local % CHIPLET_COLS;
    int my_ly    = my_local / CHIPLET_COLS;
    int tgt_lx   = (my_lx < CHIPLET_COLS / 2) ? 0 : (CHIPLET_COLS - 1);
    int tgt_ly   = (my_ly < CHIPLET_COLS / 2) ? 0 : (CHIPLET_COLS - 1);

    PortDirection dirn;
    if (tgt_lx != my_lx)
        dirn = (tgt_lx > my_lx) ? "East" : "West";
    else
        dirn = (tgt_ly > my_ly) ? "South" : "North";
    return m_outports_dirn2idx.at(dirn);
}

// Chiplet XY routing: XY within chiplet, XY on interposer, nearest gateway
// --routing-algorithm=3
int
RoutingUnit::outportComputeChipletXY(RouteInfo route,
                                     int inport,
                                     PortDirection inport_dirn)
{
    static const int CHIPLET_COLS        = 4;
    static const int ROUTERS_PER_CHIPLET = 16; // 4x4
    static const int NUM_CHIPLET_ROUTERS = 64; // 4 chiplets x 16
    static const int NUM_GATEWAYS        = 4;  // per chiplet
    static const int CHIPLET_MESH_COLS   = 2;  // 2x2 chiplet grid
    static const int INTERPOSER_COLS     = 4;  // 4x4 interposer grid

    int my_id   = m_router->get_id();
    int dest_id = route.dest_router;

    bool is_interposer       = (m_outports_dirn2idx.count("Up")   > 0);
    bool is_chiplet_boundary = (m_outports_dirn2idx.count("Down") > 0);

    // Helper: compute interposer global (x,y) from interposer router id
    auto interposer_xy = [&](int ir_id, int &ix, int &iy) {
        int local_ir    = ir_id - NUM_CHIPLET_ROUTERS;
        int chiplet_id  = local_ir / NUM_GATEWAYS;
        int gw_idx      = local_ir % NUM_GATEWAYS;
        int chip_col    = chiplet_id % CHIPLET_MESH_COLS;
        int chip_row    = chiplet_id / CHIPLET_MESH_COLS;
        int gw_lx       = gw_idx % 2;  // 0=TL/BL, 1=TR/BR
        int gw_ly       = gw_idx / 2;  // 0=TL/TR, 1=BL/BR
        ix = chip_col * 2 + gw_lx;
        iy = chip_row * 2 + gw_ly;
    };

    // Helper: for a destination chiplet router, find the nearest gateway's
    // interposer router id
    auto dest_target_interposer = [&](int dst_id) -> int {
        int dst_chiplet = dst_id / ROUTERS_PER_CHIPLET;
        int dst_local   = dst_id % ROUTERS_PER_CHIPLET;
        int dst_lx      = dst_local % CHIPLET_COLS;
        int dst_ly      = dst_local / CHIPLET_COLS;
        // Nearest gateway corner
        int gw_lx = (dst_lx < CHIPLET_COLS / 2) ? 0 : 1;
        int gw_ly = (dst_ly < CHIPLET_COLS / 2) ? 0 : 1;
        int gw_idx = gw_ly * 2 + gw_lx;
        return NUM_CHIPLET_ROUTERS + dst_chiplet * NUM_GATEWAYS + gw_idx;
    };

    // XY routing helper: returns port direction given current and dest coords
    auto xy_dirn = [](int cx, int cy, int dx, int dy) -> PortDirection {
        if (dx != cx)
            return (dx > cx) ? "East" : "West";
        else
            return (dy > cy) ? "South" : "North";
    };

    // ------------------------------------------------------------------
    // Case 1: Interposer router -> XY on interposer grid
    // ------------------------------------------------------------------
    if (is_interposer) {
        int my_ix, my_iy;
        interposer_xy(my_id, my_ix, my_iy);

        // Destination is on a chiplet; find the target interposer router
        int target_ir = dest_target_interposer(dest_id);

        if (target_ir == my_id) {
            // Already at the target interposer router, send Up to chiplet
            return m_outports_dirn2idx.at("Up");
        }

        int tgt_ix, tgt_iy;
        interposer_xy(target_ir, tgt_ix, tgt_iy);

        PortDirection dirn = xy_dirn(my_ix, my_iy, tgt_ix, tgt_iy);
        return m_outports_dirn2idx.at(dirn);
    }

    // ------------------------------------------------------------------
    // Case 2: Chiplet boundary router
    // ------------------------------------------------------------------
    if (is_chiplet_boundary) {
        int my_chiplet  = my_id / ROUTERS_PER_CHIPLET;
        bool same_chiplet = (dest_id < NUM_CHIPLET_ROUTERS) &&
                            (dest_id / ROUTERS_PER_CHIPLET == my_chiplet);

        // 2a: same chiplet -> intra-chiplet XY
        if (same_chiplet) {
            int my_local   = my_id   % ROUTERS_PER_CHIPLET;
            int dest_local = dest_id % ROUTERS_PER_CHIPLET;
            PortDirection dirn = xy_dirn(
                my_local % CHIPLET_COLS, my_local / CHIPLET_COLS,
                dest_local % CHIPLET_COLS, dest_local / CHIPLET_COLS);
            return m_outports_dirn2idx.at(dirn);
        }

        // 2b: different chiplet
        // Turn restriction: Down(in) -> Down(out) prohibited
        if (inport_dirn == "Down") {
            // Arrived from interposer, XY within chiplet to destination
            int my_local   = my_id   % ROUTERS_PER_CHIPLET;
            int dest_local = dest_id % ROUTERS_PER_CHIPLET;
            PortDirection dirn = xy_dirn(
                my_local % CHIPLET_COLS, my_local / CHIPLET_COLS,
                dest_local % CHIPLET_COLS, dest_local / CHIPLET_COLS);
            return m_outports_dirn2idx.at(dirn);
        }
        // From chiplet interior: send Down to interposer
        return m_outports_dirn2idx.at("Down");
    }

    // ------------------------------------------------------------------
    // Case 3: Normal intra-chiplet router
    // ------------------------------------------------------------------
    int my_chiplet  = my_id / ROUTERS_PER_CHIPLET;
    bool same_chiplet = (dest_id < NUM_CHIPLET_ROUTERS) &&
                        (dest_id / ROUTERS_PER_CHIPLET == my_chiplet);

    if (same_chiplet) {
        // Intra-chiplet XY
        int my_local   = my_id   % ROUTERS_PER_CHIPLET;
        int dest_local = dest_id % ROUTERS_PER_CHIPLET;
        PortDirection dirn = xy_dirn(
            my_local % CHIPLET_COLS, my_local / CHIPLET_COLS,
            dest_local % CHIPLET_COLS, dest_local / CHIPLET_COLS);
        return m_outports_dirn2idx.at(dirn);
    }

    // Inter-chiplet: XY toward nearest corner boundary router
    int my_local = my_id % ROUTERS_PER_CHIPLET;
    int my_lx    = my_local % CHIPLET_COLS;
    int my_ly    = my_local / CHIPLET_COLS;
    int tgt_lx   = (my_lx < CHIPLET_COLS / 2) ? 0 : (CHIPLET_COLS - 1);
    int tgt_ly   = (my_ly < CHIPLET_COLS / 2) ? 0 : (CHIPLET_COLS - 1);

    PortDirection dirn = xy_dirn(my_lx, my_ly, tgt_lx, tgt_ly);
    return m_outports_dirn2idx.at(dirn);
}

// Adaptive Chiplet XY routing: health-aware on interposer, XY elsewhere
// --routing-algorithm=4
int
RoutingUnit::outportComputeAdaptiveChipletXY(RouteInfo route,
                                              int inport,
                                              PortDirection inport_dirn,
                                              flit *t_flit)
{
    static const int CHIPLET_COLS        = 4;
    static const int ROUTERS_PER_CHIPLET = 16;
    static const int NUM_CHIPLET_ROUTERS = 64;
    static const int NUM_GATEWAYS        = 4;
    static const int CHIPLET_MESH_COLS   = 2;

    int my_id   = m_router->get_id();
    int dest_id = route.dest_router;

    bool is_interposer       = (m_outports_dirn2idx.count("Up")   > 0);
    bool is_chiplet_boundary = (m_outports_dirn2idx.count("Down") > 0);

    auto interposer_xy = [&](int ir_id, int &ix, int &iy) {
        int local_ir    = ir_id - NUM_CHIPLET_ROUTERS;
        int chiplet_id  = local_ir / NUM_GATEWAYS;
        int gw_idx      = local_ir % NUM_GATEWAYS;
        int chip_col    = chiplet_id % CHIPLET_MESH_COLS;
        int chip_row    = chiplet_id / CHIPLET_MESH_COLS;
        int gw_lx       = gw_idx % 2;
        int gw_ly       = gw_idx / 2;
        ix = chip_col * 2 + gw_lx;
        iy = chip_row * 2 + gw_ly;
    };

    auto dest_target_interposer = [&](int dst_id) -> int {
        int dst_chiplet = dst_id / ROUTERS_PER_CHIPLET;
        int dst_local   = dst_id % ROUTERS_PER_CHIPLET;
        int dst_lx      = dst_local % CHIPLET_COLS;
        int dst_ly      = dst_local / CHIPLET_COLS;
        int gw_lx = (dst_lx < CHIPLET_COLS / 2) ? 0 : 1;
        int gw_ly = (dst_ly < CHIPLET_COLS / 2) ? 0 : 1;
        int gw_idx = gw_ly * 2 + gw_lx;
        return NUM_CHIPLET_ROUTERS + dst_chiplet * NUM_GATEWAYS + gw_idx;
    };

    auto xy_dirn = [](int cx, int cy, int dx, int dy) -> PortDirection {
        if (dx != cx)
            return (dx > cx) ? "East" : "West";
        else
            return (dy > cy) ? "South" : "North";
    };

    // ------------------------------------------------------------------
    // Case 1: Interposer router — health-aware Up selection
    // ------------------------------------------------------------------
    if (is_interposer) {
        int my_ix, my_iy;
        interposer_xy(my_id, my_ix, my_iy);

        int target_ir = dest_target_interposer(dest_id);

        // Compute direction from current router to target gateway
        int tgt_ix, tgt_iy;
        interposer_xy(target_ir, tgt_ix, tgt_iy);
        PortDirection dir_to_target = xy_dirn(my_ix, my_iy, tgt_ix, tgt_iy);

        // ---- Implicit redirect detection (zero flit overhead) ----
        // If flit arrived FROM the direction of its target gateway,
        // it is moving AWAY from target — impossible under normal XY.
        // This means it was redirected by the target gateway → go Up.
        if (target_ir != my_id) {
            int dest_chiplet = dest_id / ROUTERS_PER_CHIPLET;
            int my_chiplet = (my_id - NUM_CHIPLET_ROUTERS) / NUM_GATEWAYS;
            if (inport_dirn == dir_to_target
                && my_chiplet == dest_chiplet) {
                m_router->incrArcAntiLivelock();
                return m_outports_dirn2idx.at("Up");
            }

            // Normal routing: XY toward target
            return m_outports_dirn2idx.at(dir_to_target);
        }

        if (target_ir == my_id) {
            m_router->incrArcAtTarget();

            // Check Up health using average score
            int self_score = 7;
            for (int i = 0; i < m_router->get_num_outports(); i++) {
                if (m_router->getOutportDirection(i) == "Up" &&
                    m_router->getOutputUnit(i)->hasHealthMonitor()) {
                    self_score = m_router->getOutputUnit(i)
                        ->getAverageQuantizedScore(curTick());
                    break;
                }
            }

            // Severity-based LOCAL_BIAS:
            //   self <= 2 (severe):   bias = 1, aggressive redirect
            //   self  3-4 (moderate): bias = 2, conservative redirect
            //   self  > 4 (healthy):  no redirect
            int bias;
            if (self_score <= 2) {
                bias = 1;
            } else if (self_score <= 4) {
                bias = 2;
            } else {
                // Healthy, go Up directly
                m_router->incrArcHealthy();
                return m_outports_dirn2idx.at("Up");
            }

            m_router->incrArcCongested();

            // Debug: print once per router
            static bool debug_printed = false;
            if (!debug_printed && my_id == 64) {
                debug_printed = true;
                std::cout << "[ARC-DEBUG] R" << my_id
                          << " self_score=" << self_score
                          << " bias=" << bias
                          << " neighbors=" << m_router->getDirectNeighbors().size()
                          << " health_table_size=" << m_router->getNeighborHealthTable().size();
                for (auto *peer : m_router->getDirectNeighbors()) {
                    int ps = 7;
                    if (m_router->getNeighborHealthTable().count(peer->get_id()))
                        ps = m_router->getNeighborHealthTable().at(peer->get_id());
                    std::cout << " nb_R" << peer->get_id()
                              << "[chip=" << peer->getChipletId()
                              << ",S=" << ps << "]";
                }
                std::cout << " my_chip=" << m_router->getChipletId()
                          << " vnet=" << route.vnet << std::endl;
            }

            // Congested — find a better same-chiplet neighbor
            int best_id = -1;
            int best_score = self_score;

            for (auto *peer : m_router->getDirectNeighbors()) {
                if (peer->getChipletId() != m_router->getChipletId())
                    continue;
                int ps = 7;
                if (m_router->getNeighborHealthTable().count(peer->get_id()))
                    ps = m_router->getNeighborHealthTable().at(
                        peer->get_id());
                if (ps >= self_score + bias) {
                    // Check lateral link has free VC
                    int peer_ix, peer_iy;
                    interposer_xy(peer->get_id(), peer_ix, peer_iy);
                    PortDirection dirn = xy_dirn(
                        my_ix, my_iy, peer_ix, peer_iy);
                    if (m_outports_dirn2idx.count(dirn)) {
                        int op = m_outports_dirn2idx.at(dirn);
                        if (m_router->getOutputUnit(op)->has_free_vc(
                                route.vnet)) {
                            if (ps > best_score) {
                                best_score = ps;
                                best_id = peer->get_id();
                            }
                        }
                    }
                }
            }

            if (best_id >= 0) {
                // Redirect to better neighbor
                m_router->incrArcRedirected();
                int best_ix, best_iy;
                interposer_xy(best_id, best_ix, best_iy);
                PortDirection dirn = xy_dirn(
                    my_ix, my_iy, best_ix, best_iy);
                return m_outports_dirn2idx.at(dirn);
            }

            // No better neighbor, go Up anyway
            m_router->incrArcNoBetter();
            return m_outports_dirn2idx.at("Up");
        }

    }

    // ------------------------------------------------------------------
    // Case 2: Chiplet boundary router (same as CHIPLET_XY_)
    // ------------------------------------------------------------------
    if (is_chiplet_boundary) {
        int my_chiplet  = my_id / ROUTERS_PER_CHIPLET;
        bool same_chiplet = (dest_id < NUM_CHIPLET_ROUTERS) &&
                            (dest_id / ROUTERS_PER_CHIPLET == my_chiplet);

        if (same_chiplet) {
            int my_local   = my_id   % ROUTERS_PER_CHIPLET;
            int dest_local = dest_id % ROUTERS_PER_CHIPLET;
            PortDirection dirn = xy_dirn(
                my_local % CHIPLET_COLS, my_local / CHIPLET_COLS,
                dest_local % CHIPLET_COLS, dest_local / CHIPLET_COLS);
            return m_outports_dirn2idx.at(dirn);
        }

        if (inport_dirn == "Down") {
            int my_local   = my_id   % ROUTERS_PER_CHIPLET;
            int dest_local = dest_id % ROUTERS_PER_CHIPLET;
            PortDirection dirn = xy_dirn(
                my_local % CHIPLET_COLS, my_local / CHIPLET_COLS,
                dest_local % CHIPLET_COLS, dest_local / CHIPLET_COLS);
            return m_outports_dirn2idx.at(dirn);
        }
        return m_outports_dirn2idx.at("Down");
    }

    // ------------------------------------------------------------------
    // Case 3: Normal intra-chiplet router (same as CHIPLET_XY_)
    // ------------------------------------------------------------------
    int my_chiplet  = my_id / ROUTERS_PER_CHIPLET;
    bool same_chiplet = (dest_id < NUM_CHIPLET_ROUTERS) &&
                        (dest_id / ROUTERS_PER_CHIPLET == my_chiplet);

    if (same_chiplet) {
        int my_local   = my_id   % ROUTERS_PER_CHIPLET;
        int dest_local = dest_id % ROUTERS_PER_CHIPLET;
        PortDirection dirn = xy_dirn(
            my_local % CHIPLET_COLS, my_local / CHIPLET_COLS,
            dest_local % CHIPLET_COLS, dest_local / CHIPLET_COLS);
        return m_outports_dirn2idx.at(dirn);
    }

    int my_local = my_id % ROUTERS_PER_CHIPLET;
    int my_lx    = my_local % CHIPLET_COLS;
    int my_ly    = my_local / CHIPLET_COLS;
    int tgt_lx   = (my_lx < CHIPLET_COLS / 2) ? 0 : (CHIPLET_COLS - 1);
    int tgt_ly   = (my_ly < CHIPLET_COLS / 2) ? 0 : (CHIPLET_COLS - 1);

    PortDirection dirn = xy_dirn(my_lx, my_ly, tgt_lx, tgt_ly);
    return m_outports_dirn2idx.at(dirn);
}

// MTR (Modular Turn Restriction) routing baseline (ISCA 2018)
// Static turn restrictions at corner gateways create load imbalance:
//   GW0(TL): South->Down forbidden  =>  only row-0 nodes can egress here
//   GW3(BR): North->Down forbidden  =>  only row-3 nodes can egress here
//   GW1(TR), GW2(BL): unrestricted  =>  hotspots for middle-row traffic
// --routing-algorithm=5
int
RoutingUnit::outportComputeMTR(RouteInfo route,
                               int inport,
                               PortDirection inport_dirn)
{
    static const int CHIPLET_COLS        = 4;
    static const int ROUTERS_PER_CHIPLET = 16;
    static const int NUM_CHIPLET_ROUTERS = 64;
    static const int NUM_GATEWAYS        = 4;
    static const int CHIPLET_MESH_COLS   = 2;

    int my_id   = m_router->get_id();
    int dest_id = route.dest_router;

    bool is_interposer       = (m_outports_dirn2idx.count("Up")   > 0);
    bool is_chiplet_boundary = (m_outports_dirn2idx.count("Down") > 0);

    auto interposer_xy = [&](int ir_id, int &ix, int &iy) {
        int local_ir    = ir_id - NUM_CHIPLET_ROUTERS;
        int chiplet_id  = local_ir / NUM_GATEWAYS;
        int gw_idx      = local_ir % NUM_GATEWAYS;
        int chip_col    = chiplet_id % CHIPLET_MESH_COLS;
        int chip_row    = chiplet_id / CHIPLET_MESH_COLS;
        int gw_lx       = gw_idx % 2;
        int gw_ly       = gw_idx / 2;
        ix = chip_col * 2 + gw_lx;
        iy = chip_row * 2 + gw_ly;
    };

    auto dest_target_interposer = [&](int dst_id) -> int {
        int dst_chiplet = dst_id / ROUTERS_PER_CHIPLET;
        int dst_local   = dst_id % ROUTERS_PER_CHIPLET;
        int dst_lx      = dst_local % CHIPLET_COLS;
        int dst_ly      = dst_local / CHIPLET_COLS;
        int gw_lx = (dst_lx < CHIPLET_COLS / 2) ? 0 : 1;
        int gw_ly = (dst_ly < CHIPLET_COLS / 2) ? 0 : 1;
        int gw_idx = gw_ly * 2 + gw_lx;
        return NUM_CHIPLET_ROUTERS + dst_chiplet * NUM_GATEWAYS + gw_idx;
    };

    auto xy_dirn = [](int cx, int cy, int dx, int dy) -> PortDirection {
        if (dx != cx)
            return (dx > cx) ? "East" : "West";
        else
            return (dy > cy) ? "South" : "North";
    };

    // ------------------------------------------------------------------
    // Case 1: Interposer router — XY on interposer grid (same as CHIPLET_XY)
    // ------------------------------------------------------------------
    if (is_interposer) {
        int my_ix, my_iy;
        interposer_xy(my_id, my_ix, my_iy);
        int target_ir = dest_target_interposer(dest_id);
        if (target_ir == my_id)
            return m_outports_dirn2idx.at("Up");
        int tgt_ix, tgt_iy;
        interposer_xy(target_ir, tgt_ix, tgt_iy);
        return m_outports_dirn2idx.at(xy_dirn(my_ix, my_iy, tgt_ix, tgt_iy));
    }

    // ------------------------------------------------------------------
    // Case 2: Chiplet boundary router
    // ------------------------------------------------------------------
    if (is_chiplet_boundary) {
        int my_chiplet = my_id / ROUTERS_PER_CHIPLET;
        bool same_chiplet = (dest_id < NUM_CHIPLET_ROUTERS) &&
                            (dest_id / ROUTERS_PER_CHIPLET == my_chiplet);
        if (same_chiplet) {
            int my_local   = my_id   % ROUTERS_PER_CHIPLET;
            int dest_local = dest_id % ROUTERS_PER_CHIPLET;
            return m_outports_dirn2idx.at(xy_dirn(
                my_local % CHIPLET_COLS, my_local / CHIPLET_COLS,
                dest_local % CHIPLET_COLS, dest_local / CHIPLET_COLS));
        }
        if (inport_dirn == "Down") {
            int my_local   = my_id   % ROUTERS_PER_CHIPLET;
            int dest_local = dest_id % ROUTERS_PER_CHIPLET;
            return m_outports_dirn2idx.at(xy_dirn(
                my_local % CHIPLET_COLS, my_local / CHIPLET_COLS,
                dest_local % CHIPLET_COLS, dest_local / CHIPLET_COLS));
        }
        return m_outports_dirn2idx.at("Down");
    }

    // ------------------------------------------------------------------
    // Case 3: Normal intra-chiplet router
    // ------------------------------------------------------------------
    int my_chiplet = my_id / ROUTERS_PER_CHIPLET;
    bool same_chiplet = (dest_id < NUM_CHIPLET_ROUTERS) &&
                        (dest_id / ROUTERS_PER_CHIPLET == my_chiplet);

    int my_local = my_id % ROUTERS_PER_CHIPLET;
    int my_lx    = my_local % CHIPLET_COLS;
    int my_ly    = my_local / CHIPLET_COLS;

    if (same_chiplet) {
        int dest_local = dest_id % ROUTERS_PER_CHIPLET;
        return m_outports_dirn2idx.at(xy_dirn(
            my_lx, my_ly,
            dest_local % CHIPLET_COLS, dest_local / CHIPLET_COLS));
    }

    // Cross-chiplet: MTR-biased gateway selection
    // GW0(TL,0,0) and GW3(BR,3,3) have outbound turn restrictions,
    // so middle rows must use GW1(TR,0,3) or GW2(BL,3,0) only.
    int tgt_lx, tgt_ly;
    if (my_ly == 0) {
        tgt_ly = 0;
        tgt_lx = (my_lx < CHIPLET_COLS / 2) ? 0 : (CHIPLET_COLS - 1);
    } else if (my_ly == CHIPLET_COLS - 1) {
        tgt_ly = CHIPLET_COLS - 1;
        tgt_lx = (my_lx < CHIPLET_COLS / 2) ? 0 : (CHIPLET_COLS - 1);
    } else {
        // Rows 1-2: restricted to GW1(TR) or GW2(BL)
        if (my_lx >= CHIPLET_COLS / 2) {
            tgt_lx = CHIPLET_COLS - 1; tgt_ly = 0;  // GW1 (TR)
        } else {
            tgt_lx = 0; tgt_ly = CHIPLET_COLS - 1;  // GW2 (BL)
        }
    }

    return m_outports_dirn2idx.at(xy_dirn(my_lx, my_ly, tgt_lx, tgt_ly));
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
