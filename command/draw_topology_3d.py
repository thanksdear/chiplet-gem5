"""
Chiplet2_5D 3D Topology Visualization
Top layer: 4 chiplets (each 4x4 mesh)
Bottom layer: Interposer (16 routers)
Vertical: TSV connections
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from mpl_toolkits.mplot3d import Axes3D
from mpl_toolkits.mplot3d.art3d import Poly3DCollection
import numpy as np

fig = plt.figure(figsize=(16, 11))
fig.patch.set_facecolor('#0d1117')
ax = fig.add_subplot(111, projection='3d')
ax.set_facecolor('#0d1117')

# ── Colors ──────────────────────────────────────────────────
CHIPLET_BORDER = ['#ff6b6b', '#4ecdc4', '#45b7d1', '#96ceb4']
CHIPLET_FACE   = ['#ff6b6b18', '#4ecdc418', '#45b7d118', '#96ceb418']
INTERPOSER_COLOR = '#ffd700'
TSV_COLOR        = '#ff69b4'
INTRA_COLOR      = '#888888'
INTER_IP_COLOR   = '#00ff88'
LABEL_COLOR      = '#ffffff'

# ── Layout parameters ────────────────────────────────────────
chiplet_rows = chiplet_cols = 4
mesh_rows = mesh_cols = 2
sp  = 1.2       # intra-chiplet router spacing
gap = 3.5       # chiplet-to-chiplet gap
Z_CHIP = 3.0    # z-height of chiplet layer
Z_IP   = 0.0    # z-height of interposer layer
SLAB_H = 0.18   # thickness of drawn slab

# ── Chiplet origins (top-layer XY) ──────────────────────────
def chiplet_origin(cr, cc):
    return (cc * (chiplet_cols * sp + gap),
            cr * (chiplet_rows * sp + gap))

origins = [(r, c) for r in range(mesh_rows) for c in range(mesh_cols)]

# ── Helper: draw a flat rectangular slab ────────────────────
def draw_slab(ax, x0, y0, x1, y1, z, dz, facecolor, edgecolor, alpha=0.18, lw=1.2):
    verts = [
        # bottom face
        [(x0,y0,z),(x1,y0,z),(x1,y1,z),(x0,y1,z)],
        # top face
        [(x0,y0,z+dz),(x1,y0,z+dz),(x1,y1,z+dz),(x0,y1,z+dz)],
        # front
        [(x0,y0,z),(x1,y0,z),(x1,y0,z+dz),(x0,y0,z+dz)],
        # back
        [(x0,y1,z),(x1,y1,z),(x1,y1,z+dz),(x0,y1,z+dz)],
        # left
        [(x0,y0,z),(x0,y1,z),(x0,y1,z+dz),(x0,y0,z+dz)],
        # right
        [(x1,y0,z),(x1,y1,z),(x1,y1,z+dz),(x0,y0,z+dz)],  # note: intentional for closed box
    ]
    poly = Poly3DCollection(verts, alpha=alpha, linewidths=lw,
                            edgecolor=edgecolor, facecolor=facecolor)
    ax.add_collection3d(poly)

# ════════════════════════════════════════════════════════════
# INTERPOSER LAYER  (z = Z_IP)
# ════════════════════════════════════════════════════════════
# Full interposer slab
ip_x0 = -0.8;  ip_x1 = mesh_cols * (chiplet_cols * sp + gap) - gap + 0.8
ip_y0 = -0.8;  ip_y1 = mesh_rows * (chiplet_rows * sp + gap) - gap + 0.8
draw_slab(ax, ip_x0, ip_y0, ip_x1, ip_y1, Z_IP, SLAB_H*1.5,
          facecolor='#ffd70012', edgecolor=INTERPOSER_COLOR, alpha=0.12, lw=1.5)

ax.text((ip_x0+ip_x1)/2, ip_y0 - 0.3, Z_IP,
        'Interposer Layer', color=INTERPOSER_COLOR,
        fontsize=10, ha='center', va='top', fontweight='bold',
        zdir='y')

# Interposer routers: 4 per chiplet at corner positions
num_chiplet_routers = 64
gateway_offsets = [0, 3, 12, 15]  # TL, TR, BL, BR of 4x4

ip_router_pos = {}   # rid -> (x, y, z)
for cid, (cr, cc) in enumerate(origins):
    ox, oy = chiplet_origin(cr, cc)
    base_ip = num_chiplet_routers + cid * 4
    # Place interposer routers at the chiplet's corner positions (XY), at Z_IP
    corner_xy = [
        (ox,                          oy + (chiplet_rows-1)*sp),  # TL
        (ox + (chiplet_cols-1)*sp,    oy + (chiplet_rows-1)*sp),  # TR
        (ox,                          oy),                         # BL
        (ox + (chiplet_cols-1)*sp,    oy),                         # BR
    ]
    for i, (ix, iy) in enumerate(corner_xy):
        irid = base_ip + i
        ip_router_pos[irid] = (ix, iy, Z_IP + SLAB_H*1.5)
        ax.scatter(ix, iy, Z_IP + SLAB_H*1.5,
                   s=90, c=INTERPOSER_COLOR, edgecolors='#000',
                   linewidths=0.8, zorder=5, marker='D', depthshade=False)
        ax.text(ix, iy, Z_IP + SLAB_H*1.5 + 0.15, str(irid),
                color=INTERPOSER_COLOR, fontsize=5, ha='center', va='bottom',
                zdir='z')

# Interposer intra-chiplet links (between 4 corner routers of same chiplet)
for cid in range(4):
    base_ip = num_chiplet_routers + cid * 4
    # Connect all 4 corners: TL-TR, BL-BR, TL-BL, TR-BR
    pairs = [(0,1),(2,3),(0,2),(1,3)]
    for a, b in pairs:
        x1,y1,z1 = ip_router_pos[base_ip+a]
        x2,y2,z2 = ip_router_pos[base_ip+b]
        ax.plot([x1,x2],[y1,y2],[z1,z2], color=INTER_IP_COLOR,
                lw=1.2, alpha=0.7, zorder=3)

# Interposer inter-chiplet links (between adjacent chiplets' nearest corners)
# East neighbors: chiplet 0<->1, 2<->3  (connect TR of left to TL of right, BR to BL)
# South neighbors: chiplet 0<->2, 1<->3 (connect BL of top to TL of bottom, BR to TR)
interchiplet_links = [
    # East: chiplet_left, chiplet_right, corner_left, corner_right
    (0, 1, 1, 0),  # C0.TR <-> C1.TL
    (0, 1, 3, 2),  # C0.BR <-> C1.BL
    (2, 3, 1, 0),  # C2.TR <-> C3.TL
    (2, 3, 3, 2),  # C2.BR <-> C3.BL
    # South: chiplet_top, chiplet_bot, corner_top, corner_bot
    (0, 2, 2, 0),  # C0.BL <-> C2.TL
    (0, 2, 3, 1),  # C0.BR <-> C2.TR
    (1, 3, 2, 0),  # C1.BL <-> C3.TL
    (1, 3, 3, 1),  # C1.BR <-> C3.TR
]
for ca, cb, ia, ib in interchiplet_links:
    x1,y1,z1 = ip_router_pos[num_chiplet_routers + ca*4 + ia]
    x2,y2,z2 = ip_router_pos[num_chiplet_routers + cb*4 + ib]
    ax.plot([x1,x2],[y1,y2],[z1,z2], color=INTER_IP_COLOR,
            lw=1.8, alpha=0.5, linestyle=':', zorder=3)

# ════════════════════════════════════════════════════════════
# CHIPLET LAYER  (z = Z_CHIP)
# ════════════════════════════════════════════════════════════
chiplet_router_pos = {}  # rid -> (x, y, z)

for cid, (cr, cc) in enumerate(origins):
    ox, oy = chiplet_origin(cr, cc)
    color  = CHIPLET_BORDER[cid]

    # Chiplet slab
    pad = 0.4
    draw_slab(ax,
              ox - pad, oy - pad,
              ox + (chiplet_cols-1)*sp + pad, oy + (chiplet_rows-1)*sp + pad,
              Z_CHIP, SLAB_H,
              facecolor=color+'18', edgecolor=color, alpha=0.20, lw=1.8)

    # Chiplet label
    cx = ox + (chiplet_cols-1)*sp/2
    cy = oy + (chiplet_rows-1)*sp/2
    ax.text(cx, oy - pad - 0.05, Z_CHIP + SLAB_H,
            f'Chiplet {cid}', color=color,
            fontsize=9, ha='center', va='top', fontweight='bold', zdir='y')

    base = cid * chiplet_rows * chiplet_cols

    # Routers
    for row in range(chiplet_rows):
        for col in range(chiplet_cols):
            rid = base + row * chiplet_cols + col
            x = ox + col * sp
            y = oy + row * sp
            z = Z_CHIP + SLAB_H
            chiplet_router_pos[rid] = (x, y, z)

            is_gateway = (row, col) in [(0,0),(0,3),(3,0),(3,3)]
            fc = color if is_gateway else '#1e1e3a'
            size = 110 if is_gateway else 50
            ax.scatter(x, y, z, s=size, c=fc, edgecolors=color,
                       linewidths=1.2, zorder=6, depthshade=False)
            if is_gateway:
                ax.text(x, y, z+0.12, str(rid), color=LABEL_COLOR,
                        fontsize=4.5, ha='center', va='bottom', zdir='z')

    # Intra-chiplet mesh links
    for row in range(chiplet_rows):
        for col in range(chiplet_cols):
            rid = base + row * chiplet_cols + col
            x, y, z = chiplet_router_pos[rid]
            if col + 1 < chiplet_cols:
                x2,y2,z2 = chiplet_router_pos[base + row*chiplet_cols + col+1]
                ax.plot([x,x2],[y,y2],[z,z2], color=INTRA_COLOR,
                        lw=0.7, alpha=0.55, zorder=4)
            if row + 1 < chiplet_rows:
                x2,y2,z2 = chiplet_router_pos[base + (row+1)*chiplet_cols + col]
                ax.plot([x,x2],[y,y2],[z,z2], color=INTRA_COLOR,
                        lw=0.7, alpha=0.55, zorder=4)

# ════════════════════════════════════════════════════════════
# TSV VERTICAL LINKS  (chiplet corner <-> interposer router)
# ════════════════════════════════════════════════════════════
gateway_map = {0: 0, 3: 1, 12: 2, 15: 3}  # chiplet-local offset -> interposer corner idx

for cid in range(4):
    base = cid * 16
    base_ip = num_chiplet_routers + cid * 4
    for local_off, ip_idx in gateway_map.items():
        cx2, cy2, cz2 = chiplet_router_pos[base + local_off]
        ix2, iy2, iz2 = ip_router_pos[base_ip + ip_idx]
        # Draw TSV as dashed vertical line with midpoint glow
        ax.plot([cx2, ix2], [cy2, iy2], [cz2, iz2],
                color=TSV_COLOR, lw=1.8, alpha=0.85,
                linestyle='--', zorder=5)
        # Small tick marks at midpoint
        mx, my, mz = (cx2+ix2)/2, (cy2+iy2)/2, (cz2+iz2)/2
        ax.scatter(mx, my, mz, s=18, c=TSV_COLOR, zorder=6, depthshade=False)

# ════════════════════════════════════════════════════════════
# Axes & labels
# ════════════════════════════════════════════════════════════
ax.set_xlabel('X', color='#666', labelpad=2)
ax.set_ylabel('Y', color='#666', labelpad=2)
ax.set_zlabel('Layer', color='#666', labelpad=2)
ax.tick_params(colors='#444')
ax.xaxis.pane.fill = False
ax.yaxis.pane.fill = False
ax.zaxis.pane.fill = False
ax.xaxis.pane.set_edgecolor('#333')
ax.yaxis.pane.set_edgecolor('#333')
ax.zaxis.pane.set_edgecolor('#333')
ax.grid(True, color='#333', linewidth=0.4, alpha=0.5)
ax.set_zticks([Z_IP, Z_CHIP])
ax.set_zticklabels(['Interposer', 'Chiplet'], color='#aaa', fontsize=8)
ax.set_xlim(-1, ip_x1)
ax.set_ylim(-1, ip_y1)
ax.set_zlim(-0.5, Z_CHIP + 1.5)
ax.view_init(elev=28, azim=-55)

# ── Legend ──────────────────────────────────────────────────
legend_items = [
    mpatches.Patch(color=INTRA_COLOR,    alpha=0.7, label='Intra-chiplet Mesh (1 cycle)'),
    mpatches.Patch(color=TSV_COLOR,      alpha=0.8, label='TSV vertical link (5 cycles)'),
    mpatches.Patch(color=INTER_IP_COLOR, alpha=0.8, label='Interposer link (1 cycle)'),
    plt.scatter([],[], s=50,  c='#1e1e3a', edgecolors='white', linewidths=1, label='Router'),
    plt.scatter([],[], s=110, c='white',   edgecolors='white', linewidths=1, label='Gateway router (TSV)'),
    plt.scatter([],[], s=90,  c=INTERPOSER_COLOR, edgecolors='k', linewidths=0.8, marker='D', label='Interposer router'),
]
leg = ax.legend(handles=legend_items, loc='upper left',
                facecolor='#1a1a2e', edgecolor='#555', labelcolor=LABEL_COLOR,
                fontsize=8, framealpha=0.9, bbox_to_anchor=(0.0, 0.98))

fig.suptitle('Chiplet2_5D — 3D Architecture\n'
             '4 Chiplets (2x2)  |  Each: 4x4 Mesh  |  Interposer Layer below',
             color=LABEL_COLOR, fontsize=13, y=0.97)

plt.tight_layout()
out = '/home/thanks/gem5/command/chiplet2_5d_3d.png'
plt.savefig(out, dpi=160, bbox_inches='tight', facecolor=fig.get_facecolor())
print(f'Saved: {out}')
plt.show()
