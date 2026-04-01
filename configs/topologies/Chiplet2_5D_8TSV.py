# Copyright (c) 2024
# 2.5D Chiplet Topology with Interposer Layer
# Models chiplets connected through an active interposer with routers
# Modified: 8 TSV boundary routers per chiplet (2 per side)

from topologies.BaseTopology import SimpleTopology
from m5.objects import *
from m5.params import *


class Chiplet2_5D_8TSV(SimpleTopology):
    """
    2.5D Chiplet topology with active interposer layer

    Architecture:
    - Multiple chiplets (e.g., GPU, CPU, HBM)
    - Each chiplet has internal mesh routers
    - Interposer layer has its own routers
    - Chiplets connect to interposer via TSVs

    Parameters:
    - num_chiplets: Number of chiplets (default: 4, arranged in 2×2)
    - chiplet_rows: Rows per chiplet mesh (default: 4)
    - chiplet_cols: Columns per chiplet mesh (default: 4)
    - num_gateways_per_chiplet: 8 (2 per side, arranged clockwise)

    Gateway layout per chiplet (indices 0-7, clockwise from top-left):
             0(T0) 1(T1)
     7(L0) ┌───────────┐ 2(R0)
     6(L1) └───────────┘ 3(R1)
             5(B0) 4(B1)
    """

    description = "Chiplet2_5D_8TSV"

    def __init__(self, controllers):
        self.nodes = controllers

    def makeTopology(self, options, network, IntLink, ExtLink, Router):
        nodes = self.nodes

        # Configuration parameters
        num_chiplets = 4          # 4 chiplets in 2×2 mesh
        chiplet_mesh_rows = 2     # Chiplets arranged in 2×2 grid
        chiplet_mesh_cols = 2
        chiplet_rows = 4          # Each chiplet: 4×4 PEs
        chiplet_cols = 4
        routers_per_chiplet = chiplet_rows * chiplet_cols  # 16 routers/chiplet

        # Latency parameters (in cycles)
        chiplet_link_latency = 1      # Intra-chiplet link latency
        tsv_latency = 1               # TSV: Chiplet ↔ Interposer
        interposer_link_latency = 1   # Interposer router-to-router

        router_latency = options.router_latency

        # Calculate total routers
        # Each chiplet: 16 routers (4×4)
        # Interposer: 8 routers per chiplet (2 per side, clockwise)
        num_gateways_per_chiplet = 8   # 8 TSV gateways per chiplet
        num_chiplet_routers   = num_chiplets * routers_per_chiplet       # 4*16 = 64
        num_interposer_routers = num_chiplets * num_gateways_per_chiplet  # 4*8  = 32
        total_routers = num_chiplet_routers + num_interposer_routers      # 96

        print(f"[Chiplet2_5D_8TSV] Creating {num_chiplets} chiplets")
        print(f"[Chiplet2_5D_8TSV] Chiplet routers:    {num_chiplet_routers}")
        print(f"[Chiplet2_5D_8TSV] Interposer routers: {num_interposer_routers}")
        print(f"[Chiplet2_5D_8TSV] Total routers:      {total_routers}")

        # Create all routers
        routers = []

        # Chiplet routers (ID: 0-63)
        for i in range(num_chiplet_routers):
            routers.append(Router(router_id=i, latency=router_latency))

        # Interposer routers (ID: 64-95)
        for i in range(num_interposer_routers):
            router_id = num_chiplet_routers + i
            routers.append(Router(router_id=router_id, latency=router_latency))

        network.routers = routers

        # Connect nodes to chiplet routers via ExtLinks
        ext_links = []
        nodes_per_router = len(nodes) // num_chiplet_routers

        print(f"[Chiplet2_5D_8TSV] Nodes per chiplet router: {nodes_per_router}")

        for i, n in enumerate(nodes):
            router_id = i // nodes_per_router
            if router_id >= num_chiplet_routers:
                router_id = num_chiplet_routers - 1

            ext_links.append(
                ExtLink(
                    link_id=i,
                    ext_node=n,
                    int_node=routers[router_id],
                    latency=chiplet_link_latency,
                )
            )
        network.ext_links = ext_links

        # Create Internal Links
        int_links = []
        link_count = len(nodes)

        # ================================================================
        # Part 1: Intra-chiplet mesh connections (low latency)
        # ================================================================
        for chiplet_id in range(num_chiplets):
            base_router = chiplet_id * routers_per_chiplet

            print(f"[Chiplet2_5D_8TSV] Creating {chiplet_rows}×{chiplet_cols} mesh for Chiplet {chiplet_id} "
                  f"(routers {base_router}-{base_router+routers_per_chiplet-1})")

            # Router layout for 4x4:
            #   R0  - R1  - R2  - R3
            #   |     |     |     |
            #   R4  - R5  - R6  - R7
            #   |     |     |     |
            #   R8  - R9  - R10 - R11
            #   |     |     |     |
            #   R12 - R13 - R14 - R15

            # East-West connections
            for row in range(chiplet_rows):
                for col in range(chiplet_cols):
                    router_offset = row * chiplet_cols + col
                    src_id = base_router + router_offset

                    # East connection
                    if col + 1 < chiplet_cols:
                        dst_offset = row * chiplet_cols + (col + 1)
                        dst_id = base_router + dst_offset

                        # Eastbound
                        int_links.append(
                            IntLink(
                                link_id=link_count,
                                src_node=routers[src_id],
                                dst_node=routers[dst_id],
                                src_outport="East",
                                dst_inport="West",
                                latency=chiplet_link_latency,
                                weight=1,
                            )
                        )
                        link_count += 1

                        # Westbound
                        int_links.append(
                            IntLink(
                                link_id=link_count,
                                src_node=routers[dst_id],
                                dst_node=routers[src_id],
                                src_outport="West",
                                dst_inport="East",
                                latency=chiplet_link_latency,
                                weight=1,
                            )
                        )
                        link_count += 1

                    # South connection
                    if row + 1 < chiplet_rows:
                        dst_offset = (row + 1) * chiplet_cols + col
                        dst_id = base_router + dst_offset

                        # Southbound
                        int_links.append(
                            IntLink(
                                link_id=link_count,
                                src_node=routers[src_id],
                                dst_node=routers[dst_id],
                                src_outport="South",
                                dst_inport="North",
                                latency=chiplet_link_latency,
                                weight=2,  # XY routing: prefer East-West first
                            )
                        )
                        link_count += 1

                        # Northbound
                        int_links.append(
                            IntLink(
                                link_id=link_count,
                                src_node=routers[dst_id],
                                dst_node=routers[src_id],
                                src_outport="North",
                                dst_inport="South",
                                latency=chiplet_link_latency,
                                weight=2,
                            )
                        )
                        link_count += 1

        # ================================================================
        # Part 2: Chiplet-to-Interposer connections (TSV, medium latency)
        # ================================================================
        print(f"[Chiplet2_5D_8TSV] Creating TSV links (chiplet ↔ interposer)")
        print(f"[Chiplet2_5D_8TSV] Using 8 gateways per chiplet (2 per side, clockwise)")

        # Gateway layout on a 4×4 chiplet mesh (2 per side, clockwise indices 0-7):
        #
        #          col1  col2
        #   row0 [  R1 -  R2 ]          index 0 = offset  1  (Top-0)
        #   row0 [  R1 -  R2 ]          index 1 = offset  2  (Top-1)
        #   row1 [ R4        R7 ]        index 2 = offset  7  (Right-0)
        #   row2 [ R8        R11]        index 3 = offset 11  (Right-1)
        #   row3 [ R13 - R14 ]           index 4 = offset 14  (Bottom-1)
        #   row3 [ R13 - R14 ]           index 5 = offset 13  (Bottom-0)
        #   row2 [ R8        R11]        index 6 = offset  8  (Left-1)
        #   row1 [ R4        R7 ]        index 7 = offset  4  (Left-0)

        for chiplet_id in range(num_chiplets):
            base_router = chiplet_id * routers_per_chiplet

            gateway_offsets = [
                1,                                      # index 0: Top-0    [0,1]
                2,                                      # index 1: Top-1    [0,2]
                1 * chiplet_cols + (chiplet_cols - 1),  # index 2: Right-0  [1,3] = 7
                2 * chiplet_cols + (chiplet_cols - 1),  # index 3: Right-1  [2,3] = 11
                (chiplet_rows - 1) * chiplet_cols + 2,  # index 4: Bottom-1 [3,2] = 14
                (chiplet_rows - 1) * chiplet_cols + 1,  # index 5: Bottom-0 [3,1] = 13
                2 * chiplet_cols,                       # index 6: Left-1   [2,0] = 8
                1 * chiplet_cols,                       # index 7: Left-0   [1,0] = 4
            ]

            gateway_positions = [
                f"[0,1] top-0",
                f"[0,2] top-1",
                f"[1,{chiplet_cols-1}] right-0",
                f"[2,{chiplet_cols-1}] right-1",
                f"[{chiplet_rows-1},2] bottom-1",
                f"[{chiplet_rows-1},1] bottom-0",
                f"[2,0] left-1",
                f"[1,0] left-0",
            ]

            for i, (gateway_offset, pos_str) in enumerate(zip(gateway_offsets, gateway_positions)):
                chiplet_gateway_router = base_router + gateway_offset
                interposer_router = num_chiplet_routers + chiplet_id * num_gateways_per_chiplet + i

                print(f"[Chiplet2_5D_8TSV]   Chiplet {chiplet_id} gateway {i}: Router {chiplet_gateway_router} "
                      f"{pos_str} ↔ Interposer Router {interposer_router}")

                # Uplink: Chiplet → Interposer
                int_links.append(
                    IntLink(
                        link_id=link_count,
                        src_node=routers[chiplet_gateway_router],
                        dst_node=routers[interposer_router],
                        src_outport="Up",
                        dst_inport="Down",
                        latency=tsv_latency,
                        weight=5,
                    )
                )
                link_count += 1

                # Downlink: Interposer → Chiplet
                int_links.append(
                    IntLink(
                        link_id=link_count,
                        src_node=routers[interposer_router],
                        dst_node=routers[chiplet_gateway_router],
                        src_outport="Down",
                        dst_inport="Up",
                        latency=tsv_latency,
                        weight=5,
                    )
                )
                link_count += 1

        # ================================================================
        # Part 3: Interposer connections (medium latency)
        # ================================================================
        print(f"[Chiplet2_5D_8TSV] Creating interposer mesh")
        print(f"[Chiplet2_5D_8TSV] Interposer topology: {chiplet_mesh_rows}×{chiplet_mesh_cols} chiplet mesh "
              f"× 8 gateways = 2×2×8 structure")

        # Interposer routers organized as 2×2 chiplet mesh, each with 8 gateways:
        #   Chiplet 0: IR64-71    Chiplet 1: IR72-79
        #   Chiplet 2: IR80-87    Chiplet 3: IR88-95
        #
        # Gateway indices (clockwise):
        #        0(T0) 1(T1)
        # 7(L0) ┌─────────┐ 2(R0)
        # 6(L1) └─────────┘ 3(R1)
        #        5(B0) 4(B1)

        # Part 3a: Intra-chiplet interposer connections (8-node ring, clockwise)
        print(f"[Chiplet2_5D_8TSV]   Creating intra-chiplet interposer connections (ring of 8)")
        ring_edges = [
            (0, 1, "East",  "West"),   # T0  ── T1
            (1, 2, "East",  "West"),   # T1  ── R0
            (2, 3, "South", "North"),  # R0  ── R1
            (3, 4, "South", "North"),  # R1  ── B1
            (4, 5, "West",  "East"),   # B1  ── B0
            (5, 6, "West",  "East"),   # B0  ── L1
            (6, 7, "North", "South"),  # L1  ── L0
            (7, 0, "North", "South"),  # L0  ── T0
        ]

        for chiplet_id in range(num_chiplets):
            base_interposer = num_chiplet_routers + chiplet_id * num_gateways_per_chiplet

            print(f"[Chiplet2_5D_8TSV]     Chiplet {chiplet_id} interposer routers: "
                  f"{base_interposer}-{base_interposer+num_gateways_per_chiplet-1}")

            for a, b, fwd_out, fwd_in in ring_edges:
                src_id = base_interposer + a
                dst_id = base_interposer + b
                rev_out = fwd_in
                rev_in  = fwd_out

                int_links.append(IntLink(
                    link_id=link_count, src_node=routers[src_id], dst_node=routers[dst_id],
                    src_outport=fwd_out, dst_inport=fwd_in,
                    latency=interposer_link_latency, weight=3))
                link_count += 1

                int_links.append(IntLink(
                    link_id=link_count, src_node=routers[dst_id], dst_node=routers[src_id],
                    src_outport=rev_out, dst_inport=rev_in,
                    latency=interposer_link_latency, weight=3))
                link_count += 1

        # Part 3b: Inter-chiplet interposer connections (2×2 mesh)
        # Connect facing sides of adjacent chiplets' interposer rings:
        #   East neighbor : left chiplet's R0(2),R1(3)  ↔  right chiplet's L0(7),L1(6)
        #   South neighbor: top chiplet's  B0(5),B1(4)  ↔  bottom chiplet's T0(0),T1(1)
        #
        # Chiplet layout:
        #   [0,0]=C0  [0,1]=C1
        #   [1,0]=C2  [1,1]=C3
        print(f"[Chiplet2_5D_8TSV]   Creating inter-chiplet interposer mesh (2×2, facing sides)")

        for chiplet_row in range(chiplet_mesh_rows):
            for chiplet_col in range(chiplet_mesh_cols):
                chiplet_id = chiplet_row * chiplet_mesh_cols + chiplet_col

                # ── East neighbor ──────────────────────────────────────
                if chiplet_col + 1 < chiplet_mesh_cols:
                    neighbor_chiplet = chiplet_row * chiplet_mesh_cols + (chiplet_col + 1)
                    # Right side of left chiplet (R0=2, R1=3) ↔ Left side of right chiplet (L0=7, L1=6)
                    ew_pairs = [(2, 7), (3, 6)]
                    for src_idx, dst_idx in ew_pairs:
                        src_ir = num_chiplet_routers + chiplet_id      * num_gateways_per_chiplet + src_idx
                        dst_ir = num_chiplet_routers + neighbor_chiplet * num_gateways_per_chiplet + dst_idx
                        print(f"[Chiplet2_5D_8TSV]     C{chiplet_id}[IR{src_ir}] <-E-> C{neighbor_chiplet}[IR{dst_ir}]")

                        int_links.append(IntLink(
                            link_id=link_count, src_node=routers[src_ir], dst_node=routers[dst_ir],
                            src_outport="East", dst_inport="West",
                            latency=interposer_link_latency, weight=3))
                        link_count += 1

                        int_links.append(IntLink(
                            link_id=link_count, src_node=routers[dst_ir], dst_node=routers[src_ir],
                            src_outport="West", dst_inport="East",
                            latency=interposer_link_latency, weight=3))
                        link_count += 1

                # ── South neighbor ─────────────────────────────────────
                if chiplet_row + 1 < chiplet_mesh_rows:
                    neighbor_chiplet = (chiplet_row + 1) * chiplet_mesh_cols + chiplet_col
                    # Bottom of top chiplet (B0=5, B1=4) ↔ Top of bottom chiplet (T0=0, T1=1)
                    ns_pairs = [(5, 0), (4, 1)]
                    for src_idx, dst_idx in ns_pairs:
                        src_ir = num_chiplet_routers + chiplet_id      * num_gateways_per_chiplet + src_idx
                        dst_ir = num_chiplet_routers + neighbor_chiplet * num_gateways_per_chiplet + dst_idx
                        print(f"[Chiplet2_5D_8TSV]     C{chiplet_id}[IR{src_ir}] <-S-> C{neighbor_chiplet}[IR{dst_ir}]")

                        int_links.append(IntLink(
                            link_id=link_count, src_node=routers[src_ir], dst_node=routers[dst_ir],
                            src_outport="South", dst_inport="North",
                            latency=interposer_link_latency, weight=3))
                        link_count += 1

                        int_links.append(IntLink(
                            link_id=link_count, src_node=routers[dst_ir], dst_node=routers[src_ir],
                            src_outport="North", dst_inport="South",
                            latency=interposer_link_latency, weight=3))
                        link_count += 1

        network.int_links = int_links

        print(f"[Chiplet2_5D_8TSV] Total links created: {link_count}")
        print(f"[Chiplet2_5D_8TSV] Topology creation complete")

    def _opposite_direction(self, direction):
        """Return the opposite direction"""
        opposites = {
            "North": "South",
            "South": "North",
            "East": "West",
            "West": "East",
            "Up": "Down",
            "Down": "Up",
        }
        return opposites.get(direction, direction)
