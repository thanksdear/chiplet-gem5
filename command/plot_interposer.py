"""
plot_interposer.py
解析 gem5 仿真输出中的 [Interposer VC] 数据，生成两张图：
  1. 中介层拓扑热力图（16 个路由器按物理位置排布，颜色 = 总 VC 激活周期）
  2. 每个路由器各端口 VC 激活周期柱状图

用法：
    python3 command/plot_interposer.py [log_file]
    log_file 默认为 interposer_vc.log（由 lab.sh 重定向生成）
    若未提供文件，则直接解析 stdin。

在 lab.sh 末尾加一行：
    ./build/.../gem5.debug ... 2>&1 | tee interposer_vc.log
然后运行本脚本。
"""

import sys
import re
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.colors import Normalize
from matplotlib.cm import ScalarMappable
import numpy as np

# ─────────────────────────────────────────────
# 1. 解析
# ─────────────────────────────────────────────
def parse_interposer_vc(text):
    """
    返回 dict:
      router_id -> {
          'total': int,
          'ports': int,
          'total_in_vcs': int,
          'port_data': {direction: vc_active_cycles}
      }
    """
    routers = {}
    # 匹配路由器头行
    header_re = re.compile(
        r"\[Interposer VC\] Router (\d+)\s+total_vc_active_cycles=(\d+)\s+"
        r"ports=(\d+)\s+total_in_vcs=(\d+)"
    )
    # 匹配端口行（一行可能有多个 portN(Dir) vc_active_cycles=V）
    port_re = re.compile(r"port\d+\((\w+)\)\s+vc_active_cycles=(\d+)")

    current = None
    for line in text.splitlines():
        m = header_re.search(line)
        if m:
            rid = int(m.group(1))
            current = {
                'total': int(m.group(2)),
                'ports': int(m.group(3)),
                'total_in_vcs': int(m.group(4)),
                'port_data': {}
            }
            routers[rid] = current
            # 端口数据可能在同一行
            for pm in port_re.finditer(line):
                current['port_data'][pm.group(1)] = int(pm.group(2))
            continue
        if current is not None:
            for pm in port_re.finditer(line):
                current['port_data'][pm.group(1)] = int(pm.group(2))

    return routers


# ─────────────────────────────────────────────
# 2. 拓扑布局信息
# ─────────────────────────────────────────────
#  Chiplet 排列 (2×2):
#    C0(IR64-67)  C1(IR68-71)
#    C2(IR72-75)  C3(IR76-79)
#
#  每个 chiplet 内 4 个 interposer 路由器 (TL/TR/BL/BR):
#    TL TR
#    BL BR
#
#  物理坐标 (col, row): row 0 = top
ROUTER_POS = {}   # router_id -> (col, row) in a 4×4 grid of interposer routers
ROUTER_LABEL = {} # router_id -> "CX\nIRYY\n(pos)"

for chiplet_id in range(4):
    cx = chiplet_id % 2   # chiplet column (0 or 1)
    cy = chiplet_id // 2  # chiplet row (0 or 1)
    base = 64 + chiplet_id * 4
    # offsets within chiplet: 0=TL,1=TR,2=BL,3=BR
    sub_positions = [(0, 0), (1, 0), (0, 1), (1, 1)]  # (dc, dr)
    pos_names = ["TL", "TR", "BL", "BR"]
    for idx, (dc, dr) in enumerate(sub_positions):
        rid = base + idx
        col = cx * 2 + dc
        row = cy * 2 + dr
        ROUTER_POS[rid] = (col, row)
        ROUTER_LABEL[rid] = f"C{chiplet_id}\nIR{rid}\n({pos_names[idx]})"

# 链路列表 (src, dst, direction)：仅用于画连接线
LINKS = [
    # 片内 interposer 方形 (每个 chiplet)
    *[(64+c*4+0, 64+c*4+1) for c in range(4)],  # TL-TR
    *[(64+c*4+2, 64+c*4+3) for c in range(4)],  # BL-BR
    *[(64+c*4+0, 64+c*4+2) for c in range(4)],  # TL-BL
    *[(64+c*4+1, 64+c*4+3) for c in range(4)],  # TR-BR
    # 跨 chiplet E-W: C0.TR↔C1.TL, C0.BR↔C1.BL, C2.TR↔C3.TL, C2.BR↔C3.BL
    (65, 68), (67, 70), (73, 76), (75, 78),
    # 跨 chiplet N-S: C0.BL↔C2.TL, C0.BR↔C2.TR, C1.BL↔C3.TL, C1.BR↔C3.TR
    (66, 72), (67, 73), (70, 76), (71, 77),
]


# ─────────────────────────────────────────────
# 3. 图 1：拓扑热力图
# ─────────────────────────────────────────────
def plot_heatmap(routers, ax, title="中介层路由器 VC 激活热力图"):
    if not routers:
        ax.text(0.5, 0.5, "无数据", ha="center", va="center")
        return

    totals = [routers[rid]['total'] for rid in sorted(routers)]
    vmin, vmax = 0, max(totals) if totals else 1
    cmap = plt.cm.YlOrRd
    norm = Normalize(vmin=vmin, vmax=vmax)

    # 画连接线
    for (a, b) in LINKS:
        if a in ROUTER_POS and b in ROUTER_POS:
            ax_col = [ROUTER_POS[a][0], ROUTER_POS[b][0]]
            ax_row = [ROUTER_POS[a][1], ROUTER_POS[b][1]]
            ax.plot(ax_col, ax_row, color="#aaaaaa", linewidth=1.2, zorder=1)

    # 画节点
    for rid, info in routers.items():
        if rid not in ROUTER_POS:
            continue
        col, row = ROUTER_POS[rid]
        color = cmap(norm(info['total']))
        circle = plt.Circle((col, row), 0.38, color=color, zorder=2)
        ax.add_patch(circle)
        # 路由器 ID 和总激活数
        ax.text(col, row + 0.12, f"IR{rid}", ha="center", va="center",
                fontsize=7, fontweight="bold", zorder=3)
        ax.text(col, row - 0.15, f"{info['total']:,}", ha="center", va="center",
                fontsize=6.5, color="#333333", zorder=3)

    # chiplet 边框
    for cx in range(2):
        for cy in range(2):
            cid = cy * 2 + cx
            x0 = cx * 2 - 0.55
            y0 = cy * 2 - 0.55
            rect = mpatches.FancyBboxPatch(
                (x0, y0), 1.1, 1.1,
                boxstyle="round,pad=0.05",
                linewidth=1.5, edgecolor="#4488cc",
                facecolor="#e8f0ff", alpha=0.3, zorder=0
            )
            ax.add_patch(rect)
            ax.text(x0 + 0.55, y0 - 0.35, f"Chiplet {cid}",
                    ha="center", fontsize=8, color="#4488cc")

    ax.set_xlim(-0.8, 2.8)
    ax.set_ylim(-0.8, 3.4)
    ax.set_aspect("equal")
    ax.axis("off")
    ax.set_title(title, fontsize=11, pad=10)

    sm = ScalarMappable(cmap=cmap, norm=norm)
    sm.set_array([])
    plt.colorbar(sm, ax=ax, fraction=0.03, pad=0.02, label="VC 激活周期数")


# ─────────────────────────────────────────────
# 4. 图 2：各路由器端口柱状图
# ─────────────────────────────────────────────
DIRECTION_COLORS = {
    "Down":  "#e74c3c",   # 红：来自 chiplet（TSV 上行）
    "North": "#3498db",
    "South": "#2ecc71",
    "East":  "#f39c12",
    "West":  "#9b59b6",
    "Local": "#95a5a6",
}

def plot_port_bars(routers, ax, title="各路由器端口 VC 激活明细"):
    if not routers:
        ax.text(0.5, 0.5, "无数据", ha="center", va="center")
        return

    rids = sorted(routers.keys())
    all_dirs = ["Down", "North", "South", "East", "West"]

    x = np.arange(len(rids))
    width = 0.13
    offsets = np.linspace(-(len(all_dirs)-1)/2, (len(all_dirs)-1)/2, len(all_dirs)) * width

    for i, dirn in enumerate(all_dirs):
        vals = [routers[rid]['port_data'].get(dirn, 0) for rid in rids]
        bars = ax.bar(x + offsets[i], vals, width,
                      label=dirn,
                      color=DIRECTION_COLORS.get(dirn, "#cccccc"),
                      edgecolor="white", linewidth=0.3)

    ax.set_xticks(x)
    ax.set_xticklabels([f"IR{r}" for r in rids], rotation=45, ha="right", fontsize=7)
    ax.set_ylabel("VC 激活周期数", fontsize=9)
    ax.set_title(title, fontsize=11)
    ax.legend(title="端口方向", fontsize=8, title_fontsize=8,
              loc="upper right", ncol=2)
    ax.grid(axis="y", linestyle="--", alpha=0.4)

    # 标注 chiplet 分组
    for cid in range(4):
        base = 64 + cid * 4
        idxs = [rids.index(r) for r in range(base, base+4) if r in rids]
        if not idxs:
            continue
        xmin, xmax = min(idxs) - 0.45, max(idxs) + 0.45
        ax.axvspan(xmin, xmax,
                   alpha=0.06,
                   color=["#3498db","#e74c3c","#2ecc71","#f39c12"][cid])
        ax.text((xmin+xmax)/2, ax.get_ylim()[1]*0.97,
                f"C{cid}", ha="center", fontsize=8,
                color=["#3498db","#e74c3c","#2ecc71","#f39c12"][cid],
                fontweight="bold")


# ─────────────────────────────────────────────
# 5. 图 3：Down 端口对称性检查表格
# ─────────────────────────────────────────────
def plot_down_table(routers, ax):
    ax.axis("off")
    chiplet_names = ["Chiplet 0\n(IR64-67)", "Chiplet 1\n(IR68-71)",
                     "Chiplet 2\n(IR72-75)", "Chiplet 3\n(IR76-79)"]
    pos_names = ["TL", "TR", "BL", "BR"]
    rows = []
    for cid in range(4):
        base = 64 + cid * 4
        row = []
        for idx in range(4):
            rid = base + idx
            down = routers.get(rid, {}).get('port_data', {}).get('Down', 0)
            total = routers.get(rid, {}).get('total', 1)
            pct = down / total * 100 if total > 0 else 0
            row.append(f"{down:,}\n({pct:.0f}%)")
        rows.append(row)

    col_labels = pos_names
    row_labels = chiplet_names
    table = ax.table(
        cellText=rows,
        rowLabels=row_labels,
        colLabels=col_labels,
        cellLoc="center",
        loc="center"
    )
    table.auto_set_font_size(False)
    table.set_fontsize(8.5)
    table.scale(1.4, 2.2)

    # 着色：Down=0 红色背景，否则按值深浅
    max_val = max(
        (routers.get(64+c*4+i, {}).get('port_data', {}).get('Down', 0)
         for c in range(4) for i in range(4)),
        default=1
    )
    for r_idx in range(4):
        for c_idx in range(4):
            rid = 64 + r_idx * 4 + c_idx
            down = routers.get(rid, {}).get('port_data', {}).get('Down', 0)
            if down == 0:
                table[r_idx+1, c_idx].set_facecolor("#ffcccc")
            else:
                intensity = 0.3 + 0.6 * (down / max_val)
                table[r_idx+1, c_idx].set_facecolor(
                    plt.cm.Greens(intensity))

    ax.set_title("TSV 上行（Down 端口）各网关激活周期\n红色=0（无上行流量），绿色深浅=流量多少",
                 fontsize=9, pad=12)


# ─────────────────────────────────────────────
# 6. 主程序
# ─────────────────────────────────────────────
def main():
    if len(sys.argv) > 1:
        with open(sys.argv[1]) as f:
            text = f.read()
    else:
        # 尝试从保存的 lab 输出文件读取
        default_log = os.path.join(os.path.dirname(__file__),
                                   "..", "interposer_vc.log")
        if os.path.exists(default_log):
            with open(default_log) as f:
                text = f.read()
        else:
            print("用法: python3 plot_interposer.py <log_file>")
            print("  log_file 应包含 [Interposer VC] 输出行")
            sys.exit(1)

    routers = parse_interposer_vc(text)
    if not routers:
        print("未找到 [Interposer VC] 数据，请检查输入文件。")
        sys.exit(1)

    print(f"解析到 {len(routers)} 个中介层路由器数据")

    fig = plt.figure(figsize=(18, 13))
    fig.suptitle("Chiplet2_5D 中介层虚拟通道流量分析", fontsize=14, fontweight="bold", y=0.98)

    # 布局：左上热力图，右上表格，下方柱状图
    ax_heat  = fig.add_axes([0.03, 0.38, 0.38, 0.55])
    ax_table = fig.add_axes([0.45, 0.38, 0.52, 0.55])
    ax_bar   = fig.add_axes([0.05, 0.04, 0.90, 0.30])

    plot_heatmap(routers, ax_heat)
    plot_down_table(routers, ax_table)
    plot_port_bars(routers, ax_bar)

    out_path = os.path.join(os.path.dirname(__file__),
                            "..", "command", "interposer_traffic.png")
    plt.savefig(out_path, dpi=150, bbox_inches="tight")
    print(f"已保存: {out_path}")
    plt.close()


if __name__ == "__main__":
    main()
