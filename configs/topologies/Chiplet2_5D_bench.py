# 2.5D Chiplet Topology for Benchmark (Full-System / SE) Simulation
#
# Differences from Chiplet2_5D:
#   - 64 CPU/Cache nodes on chiplet routers (0-63)
#   - 4 Directory/MC nodes on interposer corners (IR64, IR69, IR74, IR79)
#   - All memory traffic must traverse interposer
#
# Usage: --num-cpus=64 --num-l2caches=64 --num-dirs=4
#        --topology=Chiplet2_5D_bench

from topologies.BaseTopology import SimpleTopology
from m5.objects import *
from m5.params import *


class Chiplet2_5D_bench(SimpleTopology):
    description = "Chiplet2_5D_bench"

    def __init__(self, controllers):
        self.nodes = controllers

    def makeTopology(self, options, network, IntLink, ExtLink, Router):
        nodes = self.nodes

        # --- Configuration ---
        num_chiplets = 4
        chiplet_mesh_rows = 2
        chiplet_mesh_cols = 2
        chiplet_rows = 4
        chiplet_cols = 4
        routers_per_chiplet = chiplet_rows * chiplet_cols  # 16
        num_gateways_per_chiplet = 4
        num_chiplet_routers = num_chiplets * routers_per_chiplet  # 64
        num_interposer_routers = num_chiplets * num_gateways_per_chiplet  # 16
        total_routers = num_chiplet_routers + num_interposer_routers  # 80

        # Latency (cycles)
        chiplet_link_latency = 1
        tsv_latency = 5
        interposer_link_latency = 2
        router_latency = options.router_latency

        # Interposer corner router IDs for Dir/MC placement
        # (0,0)=IR64  (3,0)=IR69  (0,3)=IR74  (3,3)=IR79
        dir_router_ids = [
            num_chiplet_routers + 0 * num_gateways_per_chiplet + 0,  # C0-TL = 64
            num_chiplet_routers + 1 * num_gateways_per_chiplet + 1,  # C1-TR = 69
            num_chiplet_routers + 2 * num_gateways_per_chiplet + 2,  # C2-BL = 74
            num_chiplet_routers + 3 * num_gateways_per_chiplet + 3,  # C3-BR = 79
        ]

        # Separate controllers by type (robust against controller ordering)
        dir_nodes = [n for n in nodes if 'Directory' in n.__class__.__name__]
        dma_nodes = [n for n in nodes if 'DMA' in n.__class__.__name__]
        chiplet_nodes = [n for n in nodes
                         if n not in dir_nodes and n not in dma_nodes]
        num_cpus = getattr(options, 'num_cpus', 64)
        assert len(dir_nodes) == 4, \
            f"Expected 4 Directory controllers, got {len(dir_nodes)}"
        assert len(chiplet_nodes) >= num_cpus, \
            f"Expected at least {num_cpus} chiplet-side controllers, " \
            f"got {len(chiplet_nodes)}"

        print(f"[Chiplet2_5D_bench] {len(chiplet_nodes)} chiplet controllers "
              f"(L1+L2), {len(dir_nodes)} Dirs, {len(dma_nodes)} DMAs")
        print(f"[Chiplet2_5D_bench] Dir routers: {dir_router_ids}")

        # --- Create routers ---
        routers = []
        for i in range(total_routers):
            routers.append(Router(router_id=i, latency=router_latency))
        network.routers = routers

        # --- ExtLinks ---
        ext_links = []
        link_count = 0

        # CPU nodes -> chiplet routers (0-63)
        for i, n in enumerate(chiplet_nodes):
            router_id = i % num_chiplet_routers
            ext_links.append(ExtLink(
                link_id=link_count, ext_node=n,
                int_node=routers[router_id],
                latency=chiplet_link_latency))
            link_count += 1

        # Dir nodes -> interposer corner routers
        for i, n in enumerate(dir_nodes):
            rid = dir_router_ids[i % len(dir_router_ids)]
            print(f"[Chiplet2_5D_bench] Dir {i} -> Router {rid}")
            ext_links.append(ExtLink(
                link_id=link_count, ext_node=n,
                int_node=routers[rid],
                latency=interposer_link_latency))
            link_count += 1

        # DMA nodes -> interposer corner routers (shared with Dir)
        for i, n in enumerate(dma_nodes):
            rid = dir_router_ids[i % len(dir_router_ids)]
            print(f"[Chiplet2_5D_bench] DMA {i} -> Router {rid}")
            ext_links.append(ExtLink(
                link_id=link_count, ext_node=n,
                int_node=routers[rid],
                latency=interposer_link_latency))
            link_count += 1

        network.ext_links = ext_links

        # --- IntLinks ---
        int_links = []

        # Part 1: Intra-chiplet mesh (same as Chiplet2_5D)
        for chiplet_id in range(num_chiplets):
            base = chiplet_id * routers_per_chiplet
            for row in range(chiplet_rows):
                for col in range(chiplet_cols):
                    src_id = base + row * chiplet_cols + col

                    # East
                    if col + 1 < chiplet_cols:
                        dst_id = base + row * chiplet_cols + (col + 1)
                        int_links.append(IntLink(
                            link_id=link_count,
                            src_node=routers[src_id],
                            dst_node=routers[dst_id],
                            src_outport="East", dst_inport="West",
                            latency=chiplet_link_latency, weight=1))
                        link_count += 1
                        int_links.append(IntLink(
                            link_id=link_count,
                            src_node=routers[dst_id],
                            dst_node=routers[src_id],
                            src_outport="West", dst_inport="East",
                            latency=chiplet_link_latency, weight=1))
                        link_count += 1

                    # South
                    if row + 1 < chiplet_rows:
                        dst_id = base + (row + 1) * chiplet_cols + col
                        int_links.append(IntLink(
                            link_id=link_count,
                            src_node=routers[src_id],
                            dst_node=routers[dst_id],
                            src_outport="South", dst_inport="North",
                            latency=chiplet_link_latency, weight=2))
                        link_count += 1
                        int_links.append(IntLink(
                            link_id=link_count,
                            src_node=routers[dst_id],
                            dst_node=routers[src_id],
                            src_outport="North", dst_inport="South",
                            latency=chiplet_link_latency, weight=2))
                        link_count += 1

        # Part 2: TSV links (same as Chiplet2_5D)
        for chiplet_id in range(num_chiplets):
            base = chiplet_id * routers_per_chiplet
            gateway_offsets = [0, chiplet_cols - 1,
                               (chiplet_rows - 1) * chiplet_cols,
                               routers_per_chiplet - 1]

            for i, gw_off in enumerate(gateway_offsets):
                chip_router = base + gw_off
                interp_router = (num_chiplet_routers +
                                 chiplet_id * num_gateways_per_chiplet + i)

                # Chiplet -> Interposer
                int_links.append(IntLink(
                    link_id=link_count,
                    src_node=routers[chip_router],
                    dst_node=routers[interp_router],
                    src_outport="Down", dst_inport="Up",
                    latency=tsv_latency, weight=5))
                link_count += 1

                # Interposer -> Chiplet
                int_links.append(IntLink(
                    link_id=link_count,
                    src_node=routers[interp_router],
                    dst_node=routers[chip_router],
                    src_outport="Up", dst_inport="Down",
                    latency=tsv_latency, weight=5))
                link_count += 1

        # Part 3: Interposer mesh (same as Chiplet2_5D)
        # 3a: Intra-chiplet interposer 2x2 squares
        square_edges = [
            (0, 1, "East",  "West"),
            (2, 3, "East",  "West"),
            (0, 2, "South", "North"),
            (1, 3, "South", "North"),
        ]
        for chiplet_id in range(num_chiplets):
            base_ir = num_chiplet_routers + chiplet_id * num_gateways_per_chiplet
            for a, b, fwd_out, fwd_in in square_edges:
                src_id = base_ir + a
                dst_id = base_ir + b
                rev_out = fwd_in
                rev_in = fwd_out
                int_links.append(IntLink(
                    link_id=link_count,
                    src_node=routers[src_id], dst_node=routers[dst_id],
                    src_outport=fwd_out, dst_inport=fwd_in,
                    latency=interposer_link_latency, weight=3))
                link_count += 1
                int_links.append(IntLink(
                    link_id=link_count,
                    src_node=routers[dst_id], dst_node=routers[src_id],
                    src_outport=rev_out, dst_inport=rev_in,
                    latency=interposer_link_latency, weight=3))
                link_count += 1

        # 3b: Inter-chiplet interposer connections
        for chiplet_row in range(chiplet_mesh_rows):
            for chiplet_col in range(chiplet_mesh_cols):
                chiplet_id = chiplet_row * chiplet_mesh_cols + chiplet_col

                # East neighbor
                if chiplet_col + 1 < chiplet_mesh_cols:
                    nb = chiplet_row * chiplet_mesh_cols + (chiplet_col + 1)
                    for src_idx, dst_idx in [(1, 0), (3, 2)]:
                        src_ir = (num_chiplet_routers +
                                  chiplet_id * num_gateways_per_chiplet +
                                  src_idx)
                        dst_ir = (num_chiplet_routers +
                                  nb * num_gateways_per_chiplet + dst_idx)
                        int_links.append(IntLink(
                            link_id=link_count,
                            src_node=routers[src_ir],
                            dst_node=routers[dst_ir],
                            src_outport="East", dst_inport="West",
                            latency=interposer_link_latency, weight=3))
                        link_count += 1
                        int_links.append(IntLink(
                            link_id=link_count,
                            src_node=routers[dst_ir],
                            dst_node=routers[src_ir],
                            src_outport="West", dst_inport="East",
                            latency=interposer_link_latency, weight=3))
                        link_count += 1

                # South neighbor
                if chiplet_row + 1 < chiplet_mesh_rows:
                    nb = (chiplet_row + 1) * chiplet_mesh_cols + chiplet_col
                    for src_idx, dst_idx in [(2, 0), (3, 1)]:
                        src_ir = (num_chiplet_routers +
                                  chiplet_id * num_gateways_per_chiplet +
                                  src_idx)
                        dst_ir = (num_chiplet_routers +
                                  nb * num_gateways_per_chiplet + dst_idx)
                        int_links.append(IntLink(
                            link_id=link_count,
                            src_node=routers[src_ir],
                            dst_node=routers[dst_ir],
                            src_outport="South", dst_inport="North",
                            latency=interposer_link_latency, weight=3))
                        link_count += 1
                        int_links.append(IntLink(
                            link_id=link_count,
                            src_node=routers[dst_ir],
                            dst_node=routers[src_ir],
                            src_outport="North", dst_inport="South",
                            latency=interposer_link_latency, weight=3))
                        link_count += 1

        network.int_links = int_links
        print(f"[Chiplet2_5D_bench] Total routers: {total_routers}, "
              f"links: {link_count}")
