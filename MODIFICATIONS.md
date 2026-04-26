# gem5 2.5D Chiplet NoC 修改清单

基于原始 gem5 Garnet 网络模拟器，为 2.5D chiplet 片间互连网络新增了健康度监控、自适应路由和死锁检测/恢复机制。

---

## 一、新增文件

### 1.1 核心类

| 文件 | 说明 |
|------|------|
| `src/mem/ruby/network/garnet/ChannelHealthMonitor.hh/.cc` | 基于 credit 的通道健康度监控器，量化评分 S∈[0,7] |
| `src/mem/ruby/network/garnet/EscapeBuffer.hh/.cc` | 死锁恢复用逃逸缓冲区，支持吸收-重注入两阶段恢复 |

### 1.2 拓扑定义

| 文件 | 说明 |
|------|------|
| `configs/topologies/Chiplet2_5D.py` | 2.5D chiplet 拓扑：4 chiplet（2×2）×16 router（4×4 mesh）+ 16 interposer 网关，共 80 router |
| `configs/topologies/Chiplet2_5D_8TSV.py` | 8-TSV 变体拓扑：每 chiplet 8 个边界网关，共 96 router |

### 1.3 脚本与工具

| 文件 | 说明 |
|------|------|
| `command/lab.sh` | 参数扫描主脚本，支持多注入率、多流量模式批量仿真 |
| `command/run_garnet_chiplet2_5d.sh` | 单次仿真运行脚本，集成所有配置参数 |
| `command/plot.py` | 延迟-注入率曲线绘图 |
| `command/draw_topology.py` | Chiplet2_5D 拓扑可视化 |
| `command/draw_topology_3d.py` | 3D chiplet 拓扑可视化 |
| `command/plot_interposer.py` | Interposer VC 监控数据可视化 |

---

## 二、修改文件

### 2.1 路由算法（RoutingUnit）

**文件**: `RoutingUnit.hh`, `RoutingUnit.cc`

新增两种路由算法：

- **Algorithm 3 (`CHIPLET_XY_`)**：chiplet 内 XY 路由 + interposer XY 路由，跨 chiplet 流量通过最近角落网关下行到 interposer
- **Algorithm 4 (`ADAPTIVE_CHIPLET_XY_`)**：在 Algorithm 3 基础上，interposer 层引入健康度感知自适应路由

  **核心思路**：当 flit 到达目标 interposer 网关准备走 Up 链路上行至 chiplet 时，如果该 Up 链路拥塞，则将 flit 横向重定向到同 chiplet 的相邻网关，从邻居网关的 Up 链路上行。

  **决策流程（RC 阶段，仅 interposer router）**：
  ```
  flit 到达目标网关（target_ir == my_id）
    │
    ├─ 该 flit 已被重定向过（redirect_count > 0）？
    │   └─ 是 → 无条件走 Up（防活锁）
    │
    ├─ 检查本网关 Up outport 的 avg health score
    │   ├─ S > 4（健康）→ 直接走 Up
    │   ├─ S = 3-4（中度拥塞）→ bias = 2，寻找邻居
    │   └─ S ≤ 2（严重拥塞）→ bias = 1，积极寻找邻居
    │
    ├─ 遍历同 chiplet 的直接邻居（m_direct_neighbors）
    │   ├─ 从 neighbor_health_table 获取邻居 Up 的健康分数
    │   ├─ 邻居 score >= self_score + bias？
    │   └─ 横向链路有空闲 VC（has_free_vc）？
    │
    ├─ 找到更好的邻居 → 横向重定向，flit.redirect_count++
    └─ 未找到 → 仍从本地走 Up
  ```

  **重定向 flit 的处理**：被重定向的 flit 到达邻居网关后，检测到 `redirect_count > 0` 且位于目标 chiplet 的网关，直接走 Up 上行，不再二次重定向（防活锁）。

  **与旧版 optimizeUpRoute 的区别**：
  | | 旧版（optimizeUpRoute） | 新版（Algorithm 4） |
  |---|---|---|
  | 决策位置 | RC 之后覆盖 outport | RC 阶段内直接决策 |
  | 目标锁定 | routing_target 锁定，一路走到黑 | 每跳独立决策 + redirect_count 防活锁 |
  | 评分方式 | average score | average score（保留，worst 过于悲观） |
  | 触发条件 | self_score ≤ 3 固定阈值 | 严重程度分级：S≤2 aggressive / S=3-4 conservative |
  | 邻居过滤 | 需手动过滤跨 chiplet | 仅遍历同 chiplet 邻居 |

### 2.2 路由类型枚举（CommonTypes）

**文件**: `CommonTypes.hh`

```cpp
CHIPLET_XY_ = 3,
ADAPTIVE_CHIPLET_XY_ = 4,
NUM_ROUTING_ALGORITHM_ = 5
```

### 2.3 Flit 类

**文件**: `flit.hh`, `flit.cc`

新增字段：
- `int m_routing_target = -1`：路由优化目标 router ID
- `int m_redirect_count = 0`：重定向计数（防活锁）

### 2.4 Router（核心基础设施）

**文件**: `Router.hh`, `Router.cc`

**Interposer 检测与 chiplet 分组：**
- `m_is_interposer`：通过检测 "Down" outport 自动识别 interposer router
- `m_chiplet_id`：所属 chiplet 编号
- `m_chiplet_peers`：同 chiplet 的其他 interposer router
- `m_direct_neighbors`：物理相邻的 interposer router（E/W/N/S）

**健康分数传播：**
- `m_neighbor_health_table`：router ID → 量化健康分数 [0-7] 映射表
- `receiveHealthScore()`：接收邻居广播的健康分数

**死锁检测（Up 方向）：**
- `m_up_input_stall` / `m_up_output_stall`：Up 端口阻塞状态
- 逃逸缓冲区管理：`escapeBufferTick()` 每周期执行死锁恢复逻辑

**自适应路由统计：**
- `m_arc_at_target`、`m_arc_healthy`、`m_arc_congested`、`m_arc_redirected`、`m_arc_no_better`、`m_arc_anti_livelock`

**旧版路由优化（已被 Algorithm 4 替代）：**
- `optimizeUpRoute()`：基于健康分数的 Up 方向路由优化

### 2.5 OutputUnit（健康度监控集成）

**文件**: `OutputUnit.hh`, `OutputUnit.cc`

**Per-VC 健康度监控（interposer Up 方向）：**
- `m_vc_health_monitors`：每个 VC 独立的 ChannelHealthMonitor
- 监控范围：vnet 2（response）的 VC
- 支持周期性广播（默认 50 周期）和分数变化时立即广播

**健康度评估方法：**
- `getWorstQuantizedScore()`：所有 VC 中最低分（用于死锁检测）
- `getAverageQuantizedScore()`：所有 VC 平均分（用于路由决策）
- `isHealthDeadlocked()`：基于健康分数的死锁判断

**统计直方图：**
- `m_health_histogram[0..7]`：记录各分数等级出现次数

### 2.6 InputUnit（VC 阻塞监控）

**文件**: `InputUnit.hh`, `InputUnit.cc`

- `m_vc_stall_cycles`：每个 VC 当前连续阻塞周期数
- `m_vc_max_stall`：每个 VC 历史最大阻塞周期
- `sampleVcStall()`：每个 wakeup 周期采样阻塞状态

### 2.7 GarnetNetwork（网络配置）

**文件**: `GarnetNetwork.hh`, `GarnetNetwork.cc`, `GarnetNetwork.py`

新增参数：
| 参数 | 默认值 | 说明 |
|------|--------|------|
| `interposer_stall_threshold` | 10000 | VC 阻塞阈值（早期死锁退出） |
| `health_monitor_broadcast_interval` | 50 | 健康分数周期性广播间隔（周期） |
| `health_monitor_change_threshold` | 1 | 触发立即广播的分数变化阈值 |
| `enable_routing_optimization` | False | 启用旧版路由优化（optimizeUpRoute） |

### 2.8 流量模式

**文件**: `GarnetSyntheticTraffic.hh`, `GarnetSyntheticTraffic.cc`

| 流量模式 | 行为 |
|----------|------|
| `HOTSPOT_`（=8） | 60% 概率发往 chiplet 0 或 3（跨 chiplet 压力测试） |
| `HOTSPOT_SINGLE_`（=9） | 60% 概率发往 node 0（集中压力至 Router 64 的 Up 链路） |

**文件**: `configs/example/garnet_synth_traffic.py`
- `--synthetic` 参数新增 `hotspot`、`hotspot_single` 选项

**文件**: `configs/network/Network.py`
- 新增 `--interposer-stall-threshold`、`--enable-routing-optimization` 命令行参数

---

## 三、健康度监控系统

### 3.1 评分公式

```
S = α × (C_free / C_total) + (1 - α) × (1 - T_stall / T_max)
```

- `C_free`：当前空闲 credit 数
- `C_total`：总 credit 数
- `T_stall`：VC 连续阻塞时间
- `T_max`：最大阻塞阈值（归一化参考）
- `α`：权重因子（默认 0.5）

### 3.2 量化等级

| 等级 | 含义 | 路由行为 |
|------|------|----------|
| S=7 | 完全健康 | 直接上行 |
| S=5-6 | 轻度拥塞 | 直接上行 |
| S=3-4 | 中度拥塞 | 保守重定向（bias=2） |
| S=1-2 | 严重拥塞 | 积极重定向（bias=1） |
| S=0 | 疑似死锁 | 触发死锁检测 |

### 3.3 广播机制

- **周期性广播**：每 50 周期向同 chiplet 邻居广播
- **立即广播**：分数变化超过阈值时立即广播

---

## 四、架构总览

```
                    Chiplet 0 (4×4)          Chiplet 1 (4×4)
                   ┌───────────────┐        ┌───────────────┐
                   │ R0  R1  R2  R3│        │R16 R17 R18 R19│
                   │ R4  R5  R6  R7│        │R20 R21 R22 R23│
                   │ R8  R9  R10 R11│       │R24 R25 R26 R27│
                   │ R12 R13 R14 R15│       │R28 R29 R30 R31│
                   └──┬──┬──┬──┬───┘        └──┬──┬──┬──┬───┘
                      │  │  │  │  Up/Down TSV   │  │  │  │
               ┌──────┴──┴──┴──┴────────────────┴──┴──┴──┴──────┐
               │  R64 R65  ←→  R68 R69   Interposer Mesh (4×4)  │
               │  R66 R67  ←→  R70 R71                          │
               │   ↕   ↕        ↕   ↕                           │
               │  R72 R73  ←→  R76 R77                          │
               │  R74 R75  ←→  R78 R79                          │
               └──────┬──┬──┬──┬────────────────┬──┬──┬──┬──────┘
                      │  │  │  │  Up/Down TSV   │  │  │  │
                   ┌──┴──┴──┴──┴───┐        ┌──┴──┴──┴──┴───┐
                   │R32 R33 R34 R35│        │R48 R49 R50 R51│
                   │R36 R37 R38 R39│        │R52 R53 R54 R55│
                   │R40 R41 R42 R43│        │R56 R57 R58 R59│
                   │R44 R45 R46 R47│        │R60 R61 R62 R63│
                   └───────────────┘        └───────────────┘
                    Chiplet 2 (4×4)          Chiplet 3 (4×4)
```

---

## 五、统计指标

仿真结束时输出的 interposer 统计信息：

```
[Interposer VC] Router XX
  port0(Up) per-VC health: vc8[cr=N,S=X,max_stall=Y] ...
  outport0(Up) worst_S=X avg_S=Y
  outport0(Up) health histogram(average) [total=N]: S0=... S7=...
  [AdaptiveRC] at_target=N healthy=N congested=N redirected=N no_better=N anti_livelock=N
```

| 指标 | 说明 |
|------|------|
| `at_target` | flit 到达目标网关的次数 |
| `healthy` | 目标网关健康、直接上行的次数 |
| `congested` | 目标网关拥塞、尝试重定向的次数 |
| `redirected` | 成功重定向到邻居网关的次数 |
| `no_better` | 找不到更好邻居、仍在本地上行的次数 |
| `anti_livelock` | 重定向 flit 到达邻居后直接上行的次数（防活锁） |
