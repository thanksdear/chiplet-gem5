# 基于通道健康度感知的 Interposer 自适应路由优化

## X.1 问题分析

在 2.5D chiplet 架构中，chiplet 与 interposer 之间通过垂直 TSV 链路（Up/Down）连接。每个 chiplet 仅有 4 个网关路由器与 interposer 相连，这些网关的 Up 链路成为跨 chiplet 通信的关键瓶颈。当流量分布不均匀时（如热点流量），部分网关的 Up 链路承受远高于其他网关的负载，导致局部拥塞甚至死锁，而同一 chiplet 的相邻网关可能仍处于空闲状态。

传统的确定性 XY 路由算法为每个 flit 分配固定的目标网关（基于目的节点的位置选择最近角落），无法根据网络运行时状态动态调整路径。因此，需要一种能够感知通道健康状态、在 interposer 层进行自适应路由的机制，将拥塞网关的流量分担至空闲邻居网关。

## X.2 通道健康度量化模型

### X.2.1 健康度评分公式

本文为每个 interposer 网关路由器的 Up 方向输出端口建立了基于 credit 的通道健康度监控器。每个虚拟通道（VC）独立计算健康分数：

$$S_{vc} = \alpha \cdot \frac{C_{free}}{C_{total}} + (1 - \alpha) \cdot \left(1 - \frac{T_{stall}}{T_{max}}\right)$$

其中：
- $C_{free}$ 为当前空闲 credit 数量，$C_{total}$ 为该 VC 的总 credit 数量，反映缓冲区占用率
- $T_{stall}$ 为该 VC 当前连续无 credit 归还的停滞时间，$T_{max}$ 为归一化参考阈值
- $\alpha$ 为权重因子（默认 0.5），平衡瞬时容量与持续停滞两个维度

该公式综合了两个互补的拥塞信号：credit 占用率捕捉瞬时缓冲区压力，停滞时间捕捉持续性阻塞。当 VC 空闲时 $S_{vc} \to 1$，当 VC 完全阻塞且长时间无 credit 归还时 $S_{vc} \to 0$。

### X.2.2 量化与聚合

连续的健康分数 $S_{vc} \in [0, 1]$ 被量化为 8 个离散等级 $Q \in \{0, 1, ..., 7\}$：

$$Q_{vc} = \lfloor S_{vc} \times 7 \rfloor$$

对同一输出端口的所有受监控 VC 取平均，得到该端口的综合健康分数：

$$Q_{avg} = \left\lfloor \frac{1}{N_{vc}} \sum_{i=1}^{N_{vc}} Q_{vc_i} \right\rfloor$$

采用平均值而非最差值的原因在于：单个 VC 的拥塞并不代表整个端口不可用，其他空闲 VC 仍可承载新的 flit。最差值会导致过于悲观的判断，频繁触发不必要的重定向。

### X.2.3 健康信息传播

健康分数通过两种机制在同一 chiplet 的 interposer 网关之间传播：

1. **周期性广播**：每 $T_{broadcast}$（默认 50）个周期，各网关向同 chiplet 的直接邻居广播当前健康分数
2. **变化触发广播**：当量化分数变化超过阈值 $\Delta Q_{threshold}$（默认 1 级）时，立即广播

每个网关路由器维护一张邻居健康表（neighbor health table），记录邻居最近一次广播的健康分数。该表用于路由决策时评估候选邻居的拥塞状况。

## X.3 自适应路由算法

### X.3.1 算法概述

本文提出的自适应路由算法（ADAPTIVE_CHIPLET_XY）在确定性 chiplet XY 路由的基础上，仅在 interposer 层的路由计算（RC）阶段引入健康度感知的自适应决策。对于 chiplet 内部的路由以及 interposer 上的横向路由，仍采用确定性 XY 路由，保证最小路径和无死锁特性。

自适应决策仅在以下条件同时满足时触发：
- 当前路由器为 interposer 网关路由器
- 当前处理的 flit 为 HEAD 或 HEAD_TAIL 类型（BODY/TAIL flit 跟随 HEAD 的路径）
- flit 已到达其确定性路由计算的目标网关
- flit 尚未被重定向过（防活锁约束）

### X.3.2 拥塞严重程度分级

算法根据当前网关的健康分数将拥塞分为三个级别，并采用不同的重定向策略：

| 健康分数 $Q_{avg}$ | 拥塞级别 | 偏置值 bias | 重定向策略 |
|:---:|:---:|:---:|---|
| $Q > 4$ | 健康 | — | 直接走 Up 链路上行，不做重定向 |
| $3 \le Q \le 4$ | 中度拥塞 | 2 | 保守重定向：仅当邻居健康分数高出 2 级以上时才重定向 |
| $Q \le 2$ | 严重拥塞 | 1 | 积极重定向：邻居健康分数高出 1 级即触发重定向 |

偏置值（bias）的设计目的是防止在轻度拥塞时过度反应。中度拥塞时要求邻居显著优于自身才重定向，避免因瞬时波动导致不必要的横向跳数增加；严重拥塞时降低门槛，优先疏散流量。

### X.3.3 邻居选择与重定向

当触发重定向时，算法在候选邻居中选择最优目标：

**候选邻居筛选条件**（必须全部满足）：
1. **同 chiplet 约束**：候选邻居必须属于同一 chiplet 的 interposer 网关。跨 chiplet 的邻居虽然物理相邻，但其 Up 链路连接到不同的 chiplet，flit 上行后将到达错误的目的 chiplet
2. **健康度约束**：候选邻居的健康分数 $Q_{neighbor} \ge Q_{self} + bias$
3. **横向链路可用性**：通往候选邻居方向的横向链路必须有空闲的虚拟通道（VC），避免将拥塞从垂直链路转移到横向链路

在满足条件的候选中，选择健康分数最高的邻居作为重定向目标。若存在多个同分候选，采用轮询（Round-Robin）策略均衡选择。

### X.3.4 防活锁机制：基于输入方向的隐式重定向检测

自适应路由引入了路径非确定性，可能导致活锁：flit 在网关 A 和网关 B 之间反复横向跳转，永远无法上行。本文提出一种**零 flit 开销**的隐式检测方法，无需修改 flit 格式或添加任何标志位。

**核心观察**：在确定性 XY 路由下，interposer 层的 flit 始终沿单调方向朝目标网关移动。若一个 flit 到达某个同 chiplet 网关时，其输入方向（inport direction）恰好指向目标网关所在的方向（direction to target），则意味着该 flit 正在**远离**其目标网关——这在正常 XY 路由下不可能发生，唯一的解释是该 flit 被目标网关重定向至此。

**检测条件**（三个条件同时满足）：

$$\text{is\_red irected} = \begin{cases} \text{true}, & \text{if } d_{inport} = d_{to\_target} \land C_{cur} = C_{dest} \land R_{cur} \neq R_{target} \\ \text{false}, & \text{otherwise} \end{cases}$$

其中：
- $d_{inport}$：flit 进入当前路由器的输入端口方向
- $d_{to\_target}$：从当前路由器到确定性目标网关的 XY 方向
- $C_{cur}$：当前网关所属 chiplet ID
- $C_{dest}$：目的节点所属 chiplet ID
- $R_{cur}$：当前路由器 ID，$R_{target}$：确定性目标网关 ID

检测到重定向后，flit 无条件走 Up 链路上行，不再参与自适应决策，从而保证每个 flit 最多被重定向 1 次。

**硬件优势**：该方法利用 RC 阶段已有的计算结果（$R_{target}$ 和 $d_{to\_target}$ 是正常 XY 路由的中间值），仅需增加一个方向比较器，不修改 flit 头部格式、不增加缓冲区宽度。chiplet 内部和 interposer 上的 flit 格式完全不受影响。

**正确性论证**：

| 场景 | $d_{inport}$ | $d_{to\_target}$ | 匹配？ | 判断 |
|:---|:---:|:---:|:---:|:---|
| 正常路由途经（从远端来，朝 target 前进） | East | West | 否 | 继续 XY ✓ |
| 重定向 flit（从 target 方向来，远离 target） | West | West | 是 | 检测到重定向，走 Up ✓ |
| 到达目标网关 | — | — | $R_{cur} = R_{target}$ | 进入自适应决策 ✓ |

### X.3.5 算法伪代码

```
Algorithm: ADAPTIVE_CHIPLET_XY Route Computation (Interposer Layer)
Input:  flit f, current router R_cur, destination node d, inport_dirn
Output: output port selection

1:  target_gw ← dest_target_interposer(d)       // 从 dest_id 计算确定性目标网关（位提取）
2:  dir_to_target ← XY_direction(R_cur, target_gw)  // RC 已有的中间计算结果
3:
4:  // ---- 隐式重定向检测（零 flit 开销） ----
5:  if R_cur ≠ target_gw then
6:      if inport_dirn = dir_to_target               // flit 从 target 方向来 → 远离 target
7:         AND chiplet(R_cur) = chiplet(d) then       // 且位于目的 chiplet
8:          return Up                                  // 检测到重定向，无条件上行
9:      end if
10:     return XY_route(R_cur, target_gw)             // 正常路由，XY 前进
11: end if
12:
13: // ---- 已到达目标网关，健康度自适应决策 ----
14: Q_self ← avg_health_score(R_cur.Up_outport)
15: if Q_self > 4 then
16:     return Up                                      // 健康，直接上行
17: end if
18:
19: bias ← (Q_self ≤ 2) ? 1 : 2                      // 拥塞严重程度分级
20: best ← NULL
21:
22: for each neighbor N in same_chiplet_neighbors(R_cur) do
23:     Q_n ← neighbor_health_table[N]
24:     if Q_n ≥ Q_self + bias then
25:         dir ← lateral_direction(R_cur, N)
26:         if has_free_vc(dir, f.vnet) then           // 横向链路可用性检查
27:             if Q_n > best.score then
28:                 best ← (N, Q_n, dir)
29:             end if
30:         end if
31:     end if
32: end for
33:
34: if best ≠ NULL then
35:     return best.dir                                // 横向重定向至更优邻居
36: else
37:     return Up                                      // 无更优邻居，仍从本地上行
38: end if
```

### X.3.6 重定向 flit 的上行处理

被重定向的 flit 经过横向链路到达邻居网关后，其确定性目标网关（target_gw）仍指向原网关。若按常规逻辑，flit 会被 XY 路由送回原网关，导致无效环路。

本文通过隐式重定向检测（伪代码第 5-9 行）解决此问题：当 flit 的输入方向与目标网关方向一致时，判定为重定向 flit，直接走 Up 上行。该判断完全基于 RC 阶段已有的计算结果（target_gw 和 dir_to_target），无需在 flit 中携带任何额外标志位。flit 上行到 chiplet 后，由 chiplet 内部的 XY 路由将其送达最终目的节点。

## X.4 正确性分析

### X.4.1 无死锁保证

本算法的自适应决策仅改变 flit 在 interposer 层的上行网关选择，不改变以下确定性路由路径：
- chiplet 内部：XY 路由（无环）
- interposer 横向：XY 路由（无环）
- 垂直方向：单向 Up 或 Down（无环）

重定向引入的额外横向路径同样遵循 XY 路由，且每个 flit 最多重定向 1 次，不会形成环路依赖。因此算法不引入新的死锁可能。

### X.4.2 无活锁保证

隐式重定向检测机制保证每个 flit 最多经历 1 次重定向。当 flit 被重定向至邻居网关后，其输入方向必然与目标网关方向一致（因为它从目标网关方向移动过来），触发检测条件后无条件上行。因此 flit 不可能被二次重定向，确保在有限步内离开 interposer 层。

### X.4.3 横向开销上界

每次重定向增加的横向跳数为 1（直接邻居）。在最坏情况下，flit 的 interposer 层路径增加 1 跳，对应 1 个周期的额外链路延迟。

## X.5 实验验证

### X.5.1 实验设置

在 hotspot_single 流量模式下验证算法效果。该模式中所有节点以 60% 概率向 node 0 发送数据包，导致 node 0 对应的网关路由器（Router 64）的 Up 链路成为严重瓶颈。

### X.5.2 流量分担效果

| 指标 | 优化前（Algorithm 3） | 优化后（Algorithm 4） | 变化 |
|:---|:---:|:---:|:---:|
| Router 64 上行 flit 数 | 74,350 | 43,058 | **-42%** |
| Router 65 上行 flit 数 | 3,045 | 19,667 | +6.5× |
| Router 66 上行 flit 数 | 3,045 | 12,449 | +4.1× |
| Router 64 成功重定向次数 | 0 | 11,443 | — |
| Router 64 健康度 S≥5 占比 | 10.6% | 59.4% | +48.8pp |

实验数据表明，自适应路由成功将 Router 64 约 42% 的上行流量分担至相邻的 Router 65 和 Router 66，热点网关的健康状况显著改善。邻居网关虽然负载增加，但仍维持较高的健康水平（Router 65 avg_S=7，Router 66 avg_S=7），说明分担后的负载在邻居的承受范围内。

### X.5.3 统计分析

Router 64 的自适应路由决策分布：

| 决策类型 | 次数 | 占比 |
|:---|:---:|:---:|
| 到达目标网关（at_target） | 29,688 | 100% |
| 健康直接上行（healthy） | 18,009 | 60.7% |
| 检测到拥塞（congested） | 11,679 | 39.3% |
| 成功重定向（redirected） | 11,443 | 38.5% |
| 未找到更优邻居（no_better） | 236 | 0.8% |
| 防活锁上行（anti_livelock） | 0 | 0% |

重定向成功率为 97.98%（11,443/11,679），仅有 236 次因邻居健康度不足或横向链路无空闲 VC 而放弃重定向。防活锁计数为 0 说明本场景下不存在二次重定向需求，redirect_count 机制作为安全保障未被触发。
