"""
Chiplet2_5D Topology Visualization
- 4 chiplets (2x2 arrangement), each chiplet: 4x4 mesh (16 routers)
- Interposer layer: 4 corner gateway routers per chiplet (16 total)
- TSV links: each chiplet's 4 corner routers <-> corresponding interposer routers
"""

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

fig, axes = plt.subplots(1, 2, figsize=(18, 9))
fig.patch.set_facecolor('#1a1a2e')

# Colors
CHIPLET_BG     = ['#ff6b6b22', '#4ecdc422', '#45b7d122', '#96ceb422']
CHIPLET_BORDER = ['#ff6b6b', '#4ecdc4', '#45b7d1', '#96ceb4']
INTERPOSER_COLOR = '#ffd700'
TSV_COLOR        = '#ff69b4'
INTRA_COLOR      = '#aaaaaa'
INTER_IP_COLOR   = '#00ff88'
LABEL_COLOR      = '#ffffff'
BG_COLOR         = '#16213e'

# ════════════════════════════════════════════════════════════
# Left: Overall topology view
# ════════════════════════════════════════════════════════════
ax = axes[0]
ax.set_facecolor(BG_COLOR)
ax.set_title('Chiplet2_5D - Router-level View', color=LABEL_COLOR, fontsize=14, pad=12)
ax.set_xlim(-1, 23)
ax.set_ylim(-4.5, 23)
ax.set_aspect('equal')
ax.axis('off')

chiplet_rows, chiplet_cols = 4, 4
chiplet_mesh_rows, chiplet_mesh_cols = 2, 2
spacing    = 1.2
chiplet_gap = 3.0
interposer_y = -3.2

chiplet_origins = []
for cr in range(chiplet_mesh_rows):
    for cc in range(chiplet_mesh_cols):
        ox = cc * (chiplet_cols * spacing + chiplet_gap)
        oy = (chiplet_mesh_rows - 1 - cr) * (chiplet_rows * spacing + chiplet_gap)
        chiplet_origins.append((ox, oy))

chiplet_names = ['Chiplet 0\n(CPU/GPU)', 'Chiplet 1\n(CPU/GPU)',
                 'Chiplet 2\n(HBM)',     'Chiplet 3\n(HBM)']

router_pos = {}

# Draw each chiplet
for cid, (ox, oy) in enumerate(chiplet_origins):
    color = CHIPLET_BORDER[cid]
    bgcol = CHIPLET_BG[cid % len(CHIPLET_BG)]

    pad = 0.35
    rect = mpatches.FancyBboxPatch(
        (ox - pad, oy - pad),
        chiplet_cols * spacing - spacing + 2*pad,
        chiplet_rows * spacing - spacing + 2*pad,
        boxstyle="round,pad=0.15", linewidth=2,
        edgecolor=color, facecolor=bgcol, zorder=1)
    ax.add_patch(rect)

    cx = ox + (chiplet_cols - 1) * spacing / 2
    ax.text(cx, oy + (chiplet_rows - 1)*spacing + 0.65,
            chiplet_names[cid], color=color,
            fontsize=7.5, ha='center', va='bottom', fontweight='bold', zorder=5)

    base = cid * chiplet_rows * chiplet_cols

    for row in range(chiplet_rows):
        for col in range(chiplet_cols):
            rid = base + row * chiplet_cols + col
            x = ox + col * spacing
            y = oy + (chiplet_rows - 1 - row) * spacing
            router_pos[rid] = (x, y)

            is_gateway = (row, col) in [(0,0),(0,3),(3,0),(3,3)]
            fc = color if is_gateway else '#2a2a4a'
            size = 120 if is_gateway else 60
            ax.scatter(x, y, s=size, c=fc, edgecolors=color,
                       linewidths=1.5, zorder=4)
            ax.text(x, y, str(rid), color=LABEL_COLOR,
                    fontsize=4, ha='center', va='center', zorder=5)

    # Intra-chiplet mesh links
    for row in range(chiplet_rows):
        for col in range(chiplet_cols):
            rid = base + row * chiplet_cols + col
            x, y = router_pos[rid]
            if col + 1 < chiplet_cols:
                nx, ny = router_pos[base + row*chiplet_cols + col+1]
                ax.plot([x, nx], [y, ny], color=INTRA_COLOR, lw=0.8, alpha=0.6, zorder=2)
            if row + 1 < chiplet_rows:
                nx, ny = router_pos[base + (row+1)*chiplet_cols + col]
                ax.plot([x, nx], [y, ny], color=INTRA_COLOR, lw=0.8, alpha=0.6, zorder=2)

# Interposer routers
num_chiplet_routers = 64
gateway_offsets = [0, 3, 12, 15]
ip_pos = {}

for cid, (ox, oy) in enumerate(chiplet_origins):
    base_ip = num_chiplet_routers + cid * 4
    ip_xs = [ox + col * spacing for col in [0, 1, 2, 3]]

    for i in range(4):
        irid = base_ip + i
        ix, iy = ip_xs[i], interposer_y
        ip_pos[irid] = (ix, iy)
        ax.scatter(ix, iy, s=120, c='#1a1a2e', edgecolors=INTERPOSER_COLOR,
                   linewidths=2, zorder=4, marker='D')
        ax.text(ix, iy, str(irid), color=INTERPOSER_COLOR,
                fontsize=4.5, ha='center', va='center', zorder=5)

    # Intra-chiplet interposer connections
    for i in range(3):
        x1, y1 = ip_pos[base_ip + i]
        x2, y2 = ip_pos[base_ip + i + 1]
        ax.plot([x1, x2], [y1, y2], color=INTER_IP_COLOR, lw=1.5, alpha=0.8, zorder=3)

    # TSV links: chiplet corner <-> interposer
    for i, goff in enumerate(gateway_offsets):
        cx2, cy2 = router_pos[cid * 16 + goff]
        ix2, iy2 = ip_pos[base_ip + i]
        ax.plot([cx2, ix2], [cy2, iy2], color=TSV_COLOR,
                lw=1.2, alpha=0.7, linestyle='--', zorder=3)

# Inter-chiplet interposer mesh (2x2)
for cr in range(chiplet_mesh_rows):
    for cc in range(chiplet_mesh_cols):
        cid = cr * chiplet_mesh_cols + cc
        base_ip = num_chiplet_routers + cid * 4

        if cc + 1 < chiplet_mesh_cols:
            nbase = num_chiplet_routers + (cr * chiplet_mesh_cols + cc + 1) * 4
            for gi in range(4):
                x1, y1 = ip_pos[base_ip + gi]
                x2, y2 = ip_pos[nbase + gi]
                ax.plot([x1, x2], [y1, y2], color=INTER_IP_COLOR,
                        lw=1.5, alpha=0.6, zorder=3, linestyle=':')

        if cr + 1 < chiplet_mesh_rows:
            nbase = num_chiplet_routers + ((cr+1) * chiplet_mesh_cols + cc) * 4
            for gi in range(4):
                x1, y1 = ip_pos[base_ip + gi]
                x2, y2 = ip_pos[nbase + gi]
                ax.plot([x1, x2], [y1, y2], color=INTER_IP_COLOR,
                        lw=1.5, alpha=0.6, zorder=3, linestyle=':')

ax.text(10.5, interposer_y - 0.75, 'Interposer Layer (16 routers)',
        color=INTERPOSER_COLOR, fontsize=9, ha='center', va='top',
        bbox=dict(boxstyle='round', facecolor='#1a1a2e', edgecolor=INTERPOSER_COLOR, alpha=0.8))

legend_items = [
    mpatches.Patch(color=INTRA_COLOR,    alpha=0.6, label='Intra-chiplet Mesh link (1 cycle)'),
    mpatches.Patch(color=TSV_COLOR,      alpha=0.7, label='TSV link (5 cycles)'),
    mpatches.Patch(color=INTER_IP_COLOR, alpha=0.8, label='Interposer link (1 cycle)'),
    plt.scatter([], [], s=60,  c='#2a2a4a', edgecolors='white', linewidths=1.5, label='Regular router'),
    plt.scatter([], [], s=120, c='white',   edgecolors='white', linewidths=1.5, label='Corner gateway router (TSV)'),
    plt.scatter([], [], s=120, c='#1a1a2e', edgecolors=INTERPOSER_COLOR, linewidths=2, marker='D', label='Interposer router'),
]
ax.legend(handles=legend_items, loc='lower right',
          facecolor='#1a1a2e', edgecolor='#444', labelcolor=LABEL_COLOR,
          fontsize=7.5, framealpha=0.9)

# ════════════════════════════════════════════════════════════
# Right: Layered architecture diagram
# ════════════════════════════════════════════════════════════
ax2 = axes[1]
ax2.set_facecolor(BG_COLOR)
ax2.set_title('Chiplet2_5D - Layered Architecture', color=LABEL_COLOR, fontsize=14, pad=12)
ax2.set_xlim(0, 10)
ax2.set_ylim(0, 12)
ax2.axis('off')

def draw_layer(ax, y, h, color, label, sublabel=''):
    rect = mpatches.FancyBboxPatch((0.5, y), 9, h,
        boxstyle="round,pad=0.1", linewidth=2,
        edgecolor=color, facecolor=color+'22', zorder=1)
    ax.add_patch(rect)
    ax.text(5, y + h/2 + 0.1, label, color=color,
            fontsize=11, ha='center', va='center', fontweight='bold', zorder=3)
    if sublabel:
        ax.text(5, y + h/2 - 0.35, sublabel, color=LABEL_COLOR,
                fontsize=8.5, ha='center', va='center', alpha=0.85, zorder=3)

draw_layer(ax2, 8.5, 3.0, '#4ecdc4',
           'Chiplet Layer',
           '4 Chiplets x 4x4 Mesh = 64 routers\nIntra-link: 1 cycle  |  XY routing')

draw_layer(ax2, 4.5, 3.0, '#ffd700',
           'Interposer Layer',
           '16 routers (4 corner gateways per chiplet)\nInterposer link: 1 cycle')

draw_layer(ax2, 0.8, 3.0, '#ff6b9d',
           'TSV Vertical Interconnect',
           '4 TSV channels per chiplet (16 total)\nTSV latency: 5 cycles  |  Chiplet <-> Interposer')

# Bidirectional arrows between layers
for y_arrow in [7.9, 4.0]:
    ax2.annotate('', xy=(5, y_arrow - 0.5), xytext=(5, y_arrow),
                arrowprops=dict(arrowstyle='<->', color=TSV_COLOR, lw=2))

# Key parameters table
params = [
    ('Total routers',      '80  (64 chiplet + 16 interposer)'),
    ('Intra-chiplet topo', '4x4 Mesh per chiplet'),
    ('Chiplet arrangement','2x2 Mesh'),
    ('Intra-chiplet link', '1 cycle latency'),
    ('TSV latency',        '5 cycles'),
    ('Interposer link',    '1 cycle latency'),
    ('Router latency',     '1 cycle (configurable)'),
    ('Routing algorithm',  'XY routing (weight-based)'),
]

ax2.text(0.7, 0.65, 'Key Parameters:', color=LABEL_COLOR,
         fontsize=9, fontweight='bold', va='top')
for i, (k, v) in enumerate(params):
    y_t = 0.45 - i * 0.055
    ax2.text(0.7, y_t, f'{k}:', color='#aaaaaa', fontsize=7.5, va='top')
    ax2.text(4.2, y_t, v,       color=LABEL_COLOR, fontsize=7.5, va='top')

plt.tight_layout(pad=2.0)
out = '/home/thanks/gem5/command/chiplet2_5d_topology.png'
plt.savefig(out, dpi=150, bbox_inches='tight', facecolor=fig.get_facecolor())
print(f"Saved: {out}")
plt.show()
