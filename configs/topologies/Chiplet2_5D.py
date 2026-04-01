# Copyright (c) 2024
# 2.5D Chiplet Topology with Interposer Layer
# Models chiplets connected through an active interposer with routers

from topologies.BaseTopology import SimpleTopology
from m5.objects import *
from m5.params import *


class Chiplet2_5D(SimpleTopology):
    """
    2.5D Chiplet topology with active interposer layer

    Architecture:
    - Multiple chiplets (e.g., GPU, CPU, HBM)
    - Each chiplet has internal mesh routers
    - Interposer layer has its own routers
    - Chiplets connect to interposer via TSVs

    Parameters:
    - num_chiplets: Number of chiplets (default: 3)
    - chiplet_rows: Rows per chiplet mesh (default: 2)
    - chiplet_cols: Columns per chiplet mesh (default: 2)
    - interposer_routing: Interposer routing topology ("mesh" or "crossbar")
    """

    description = "Chiplet2_5D"

    def __init__(self, controllers):
        self.nodes = controllers

    def makeTopology(self, options, network, IntLink, ExtLink, Router):
        nodes = self.nodes

        # Configuration parameters
        num_chiplets = 4  # 4 chiplets in 2×2 mesh
        chiplet_mesh_rows = 2  # Chiplets arranged in 2×2 grid
        chiplet_mesh_cols = 2
        chiplet_rows = 4  # Each chiplet: 4×4 PEs
        chiplet_cols = 4
        routers_per_chiplet = chiplet_rows * chiplet_cols  # 16 routers/chiplet

        # Latency parameters (in cycles)
        chiplet_link_latency = 1      # Intra-chiplet link latency
        tsv_latency = 5              # TSV: Chiplet ↔ Interposer
        interposer_link_latency = 2   # Interposer router-to-router

        router_latency = options.router_latency

        # Calculate total routers
        # Each chiplet: 16 routers (4×4)
        # Interposer: 4 routers per chiplet (one per corner gateway)
        num_gateways_per_chiplet = 4  # 4 corner gateways per chiplet
        num_chiplet_routers = num_chiplets * routers_per_chiplet  # 4*16 = 64
        num_interposer_routers = num_chiplets * num_gateways_per_chiplet  # 4*4 = 16
        total_routers = num_chiplet_routers + num_interposer_routers  # 80

        print(f"[Chiplet2_5D] Creating {num_chiplets} chiplets")
        print(f"[Chiplet2_5D] Chiplet routers: {num_chiplet_routers}")
        print(f"[Chiplet2_5D] Interposer routers: {num_interposer_routers}")
        print(f"[Chiplet2_5D] Total routers: {total_routers}")

        # Create all routers
        routers = []

        # Chiplet routers (ID: 0-63)
        for i in range(num_chiplet_routers):
            routers.append(Router(router_id=i, latency=router_latency))

        # Interposer routers (ID: 64-79)
        for i in range(num_interposer_routers):
            router_id = num_chiplet_routers + i
            routers.append(Router(router_id=router_id, latency=router_latency))

        network.routers = routers

        # Connect nodes to chiplet routers via ExtLinks
        # Use interleaved assignment so each chiplet gets both CPU and Dir nodes.
        # e.g. node 0,1,...,63 → routers 0,1,...,63
        #      node 64,65,...,127 → routers 0,1,...,63  (wraps around)
        ext_links = []
        nodes_per_router = len(nodes) // num_chiplet_routers

        print(f"[Chiplet2_5D] Nodes per chiplet router: {nodes_per_router}")

        for i, n in enumerate(nodes):
            router_id = i % num_chiplet_routers

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

            print(f"[Chiplet2_5D] Creating {chiplet_rows}×{chiplet_cols} mesh for Chiplet {chiplet_id} "
                  f"(routers {base_router}-{base_router+routers_per_chiplet-1})")

            # Create mesh fgateway_idxor each chiplet dynamically
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
        print(f"[Chiplet2_5D] Creating TSV links (chiplet ↔ interposer)")
        print(f"[Chiplet2_5D] Using 4 corner gateways per chiplet for better load distribution")

        for chiplet_id in range(num_chiplets):
            # Each chiplet uses 4 corner routers as gateways
            # For 4×4 mesh:
            #   Top-left:     R0  = offset 0
            #   Top-right:    R3  = offset 3
            #   Bottom-left:  R12 = offset 12
            #   Bottom-right: R15 = offset 15

            base_router = chiplet_id * routers_per_chiplet

            gateway_offsets = [
                0,                                      # Top-left corner
                chiplet_cols - 1,                       # Top-right corner
                (chiplet_rows - 1) * chiplet_cols,      # Bottom-left corner
                routers_per_chiplet - 1                 # Bottom-right corner
            ]

            gateway_positions = [
                f"[0,0] top-left",
                f"[0,{chiplet_cols-1}] top-right",
                f"[{chiplet_rows-1},0] bottom-left",
                f"[{chiplet_rows-1},{chiplet_cols-1}] bottom-right"
            ]

            for i, (gateway_offset, pos_str) in enumerate(zip(gateway_offsets, gateway_positions)):
                chiplet_gateway_router = base_router + gateway_offset
                interposer_router = num_chiplet_routers + chiplet_id * num_gateways_per_chiplet + i

                print(f"[Chiplet2_5D]   Chiplet {chiplet_id} gateway {i}: Router {chiplet_gateway_router} "
                      f"{pos_str} ↔ Interposer Router {interposer_router}")

                # Downlink: Chiplet → Interposer (chiplet 在上，向下穿过 TSV 到 interposer)
                int_links.append(
                    IntLink(
                        link_id=link_count,
                        src_node=routers[chiplet_gateway_router],
                        dst_node=routers[interposer_router],
                        src_outport="Down",   # chiplet 向下发往 interposer
                        dst_inport="Up",      # interposer 从上方接收
                        latency=tsv_latency,
                        weight=5,
                    )
                )
                link_count += 1

                # Uplink: Interposer → Chiplet (interposer 在下，向上穿过 TSV 到 chiplet)
                int_links.append(
                    IntLink(
                        link_id=link_count,
                        src_node=routers[interposer_router],
                        dst_node=routers[chiplet_gateway_router],
                        src_outport="Up",     # interposer 向上发往 chiplet
                        dst_inport="Down",    # chiplet 从下方接收
                        latency=tsv_latency,
                        weight=5,
                    )
                )
                link_count += 1

        # ================================================================
        # Part 3: Interposer mesh connections (medium latency)
        # ================================================================
        print(f"[Chiplet2_5D] Creating interposer mesh")
        print(f"[Chiplet2_5D] Interposer topology: {chiplet_mesh_rows}×{chiplet_mesh_cols} chiplet mesh "
              f"× 4 gateways = 2×2×4 structure")

        # Interposer routers organized as 2×2 chiplet mesh, each with 4 gateways:
        # Chiplet layout (2×2):
        #   Chiplet 0  Chiplet 1
        #   Chiplet 2  Chiplet 3
        #
        # Each chiplet has 4 interposer routers (one per corner gateway):
        #   Chiplet 0: IR64-67   Chiplet 1: IR68-71
        #   Chiplet 2: IR72-75   Chiplet 3: IR76-79
        #
        # Create connections:
        # 1. Within each chiplet's interposer routers (4 routers in a line)
        # 2. Between adjacent chiplets in 2×2 mesh (via corresponding gateways)

        # Part 3a: Intra-chiplet interposer connections
        # 4 routers form a 2×2 square:
        #   IR0(TL) ── IR1(TR)
        #     │               │
        #   IR2(BL) ── IR3(BR)
        # Index: 0=TL, 1=TR, 2=BL, 3=BR
        print(f"[Chiplet2_5D]   Creating intra-chiplet interposer connections (square)")
        square_edges = [
            (0, 1, "East",  "West"),   # TL ── TR
            (2, 3, "East",  "West"),   # BL ── BR
            (0, 2, "South", "North"),  # TL ── BL
            (1, 3, "South", "North"),  # TR ── BR
        ]
        for chiplet_id in range(num_chiplets):
            base_interposer = num_chiplet_routers + chiplet_id * num_gateways_per_chiplet

            print(f"[Chiplet2_5D]     Chiplet {chiplet_id} interposer routers: "
                  f"{base_interposer}-{base_interposer+num_gateways_per_chiplet-1}")

            for a, b, fwd_out, fwd_in in square_edges:
                src_id = base_interposer + a
                dst_id = base_interposer + b
                # Reverse ports
                rev_out = fwd_in   # "West" or "North"
                rev_in  = fwd_out  # "East" or "South"

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
        # Connect facing sides of adjacent chiplets' interposer squares:
        #   East neighbor : left chiplet's TR(1),BR(3)  ↔  right chiplet's TL(0),BL(2)
        #   South neighbor: top chiplet's  BL(2),BR(3)  ↔  bottom chiplet's TL(0),TR(1)
        #
        # Chiplet layout:
        #   [0,0]=C0  [0,1]=C1
        #   [1,0]=C2  [1,1]=C3
        print(f"[Chiplet2_5D]   Creating inter-chiplet interposer mesh (2×2, facing sides)")

        for chiplet_row in range(chiplet_mesh_rows):
            for chiplet_col in range(chiplet_mesh_cols):
                chiplet_id = chiplet_row * chiplet_mesh_cols + chiplet_col

                # ── East neighbor ──────────────────────────────────────
                if chiplet_col + 1 < chiplet_mesh_cols:
                    neighbor_chiplet = chiplet_row * chiplet_mesh_cols + (chiplet_col + 1)
                    # right side of left chiplet → left side of right chiplet
                    ew_pairs = [(1, 0), (3, 2)]  # (TR→TL, BR→BL)
                    for src_idx, dst_idx in ew_pairs:
                        src_ir = num_chiplet_routers + chiplet_id      * num_gateways_per_chiplet + src_idx
                        dst_ir = num_chiplet_routers + neighbor_chiplet * num_gateways_per_chiplet + dst_idx
                        print(f"[Chiplet2_5D]     C{chiplet_id}[IR{src_ir}] <-E-> C{neighbor_chiplet}[IR{dst_ir}]")

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
                    # bottom of top chiplet → top of bottom chiplet
                    ns_pairs = [(2, 0), (3, 1)]  # (BL→TL, BR→TR)
                    for src_idx, dst_idx in ns_pairs:
                        src_ir = num_chiplet_routers + chiplet_id      * num_gateways_per_chiplet + src_idx
                        dst_ir = num_chiplet_routers + neighbor_chiplet * num_gateways_per_chiplet + dst_idx
                        print(f"[Chiplet2_5D]     C{chiplet_id}[IR{src_ir}] <-S-> C{neighbor_chiplet}[IR{dst_ir}]")

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

        print(f"[Chiplet2_5D] Total links created: {link_count}")
        print(f"[Chiplet2_5D] Topology creation complete")

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
