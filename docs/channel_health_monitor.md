# Garnet Interposer 通道健康度监控机制

## 1. 概述

在 Chiplet 2.5D 拓扑中，interposer 路由器（Router 64–79）通过 TSV（硅通孔）连接各 chiplet。为了检测 interposer 层面的死锁，我们为每个 interposer 路由器的 **Up 方向输出端口** 引入了通道健康度监控（`ChannelHealthMonitor`），实时评估数据通道的拥塞程度并在路由器之间传播健康信息。

## 2. 核心文件

| 文件 | 作用 |
|------|------|
| `src/mem/ruby/network/garnet/ChannelHealthMonitor.hh/cc` | 健康度计算核心类 |
| `src/mem/ruby/network/garnet/OutputUnit.hh/cc` | 持有 ChannelHealthMonitor 实例，提供上层接口 |
| `src/mem/ruby/network/garnet/Router.hh/cc` | 初始化、每周期调用、死锁判定、健康分数广播 |

## 3. 健康度计算公式

```
S = α × (C_free / C_total) + (1 - α) × (1 - T_stall / T_max)
```

| 符号 | 含义 | 说明 |
|------|------|------|
| `C_free` | 当前空闲 credit 数 | 仅统计 vnet2（数据网络）的 VC |
| `C_total` | 总 credit 数 | 初始化时确定，固定不变 |
| `T_stall` | 当前停滞时间 | `current_tick - last_credit_change_time` |
| `T_max` | 最大停滞阈值 | 由 `--interposer-stall-threshold` × `clockPeriod()` 得到（单位：tick） |
| `α` | credit 占用率权重 | 默认 0.80（定义在 `OutputUnit.hh:initHealthMonitor`） |

- 连续分数 S ∈ [0.0, 1.0]，量化为整数 Q ∈ [0, 15]
- **Q = 0 专门保留给 channel dead 状态**（非 dead 的通道最低为 1）

### 为什么只监控 vnet2

Garnet 使用 3 个虚拟网络：
- vnet 0/1：控制消息（request/response），1-flit 小包，流量轻、几乎不堵塞
- vnet 2：数据消息，多 flit 大包，容易拥塞

只监控 vnet2 的 VC 才能真实反映数据通道的拥塞状况，避免控制消息的 credit 变化掩盖数据通道的停滞。

## 4. 初始化流程

在 `Router::init()` 中（Router.cc:96–120）：

```cpp
if (m_is_interposer) {
    Tick stall_threshold =
        m_network_ptr->getInterposerStallThreshold() * clockPeriod();
    for (auto &ou : m_output_unit) {
        if (ou->get_direction() == "Up") {
            int data_vnet = 2;
            int vc_base = data_vnet * ou->getVcsPerVnet();
            int vc_end = vc_base + ou->getVcsPerVnet();
            int total_credits = 0;
            for (int vc = vc_base; vc < vc_end; vc++)
                total_credits += ou->get_credit_count(vc);
            ou->initHealthMonitor(total_credits, stall_threshold,
                                  vc_base, vc_end);
        }
    }
}
```

关键参数：
- `total_credits`：vnet2 所有 VC 的初始 credit 之和
- `stall_threshold`：`--interposer-stall-threshold`（默认 100 周期）× `clockPeriod()`，转换为 tick
- `alpha`：默认 0.80f（OutputUnit.hh 中定义）

> **注意**：`ChannelHealthMonitor.hh` 构造函数中也有一个默认值 `alpha = 0.0f`，但它是死代码——所有调用方都通过 `OutputUnit::initHealthMonitor` 显式传入了 0.80f。

## 5. 每周期更新流程

在 `Router::cycle()` 中（Router.cc:230–310），对每个 Up 方向的输出端口：

```
1. sampleCreditStall()       — 采样 credit 停滞（仅 vnet2 VC）
2. syncHealthCredits()       — 同步当前 vnet2 空闲 credit 到 monitor
3. sampleHealthScore()       — 计算量化分数并记入直方图
4. getQuantizedHealthScore() — 获取当前量化分数 Q
5. 广播判定：
   - Q == 0 → 立即广播给同 chiplet 所有 peer（每周期）
   - Q != 0 → 满足周期性间隔或分数变化阈值时广播给直接邻居
```

## 6. Channel Dead 判定（S = 0）

`ChannelHealthMonitor::isChannelDead()`（ChannelHealthMonitor.cc:150–167）：

```cpp
bool isChannelDead(Tick current_tick) {
    // 所有 credit 都空闲 → 通道空闲，不是死锁
    if (m_free_credits == m_total_credits) return false;
    // 从未有过 credit 活动 → 通道未启动
    if (!m_has_credit_activity) return false;
    // 停滞时间超过阈值 → channel dead
    Tick stall_time = current_tick - m_last_credit_change_time;
    return stall_time > m_max_stall_threshold;
}
```

三个条件同时满足时 S = 0：
1. 有 flit 在途中（credit 未全部归还）
2. 曾经有过 credit 变化（通道不是从未使用）
3. vnet2 的 credit 已经连续 `T_max` tick 没有变化

## 7. 分布式死锁检测

在 `Router::cycle()` 的 Step 4（Router.cc:363–424）：

**触发条件**（两者同时满足）：
1. 收到同 chiplet peer 广播的 **S = 0**（peer 的 Up 输出通道 dead）
2. 本地 Up 输入端口有 VC 停滞超过阈值（下行通道也阻塞）

这两个条件共同形成**环形依赖**，判定为死锁，调用 `exitSimLoop()` 终止仿真。

### Chiplet 分组

Interposer 路由器 64–79，每 4 个为一组：
- Chiplet 0: Router 64–67
- Chiplet 1: Router 68–71
- Chiplet 2: Router 72–75
- Chiplet 3: Router 76–79

## 8. 统计输出

仿真结束时 `Router::collateStats()` 打印（Router.cc:604–651）：

```
outport0(Up) credit=12 credit_stall=2 max_stall=45
outport0(Up) health histogram [total=26229]:
    S0=1  S1=0  ...  S14=10004  S15=339
    S0=0.0%  ...  S14=38.1%  S15=1.3%
```

| 字段 | 含义 |
|------|------|
| `credit` | 仿真结束时 vnet2 VC 的空闲 credit 数 |
| `credit_stall` | 当前连续停滞周期数（仅 vnet2 VC） |
| `max_stall` | 历史最长连续停滞周期数（仅 vnet2 VC） |
| `health histogram` | 量化分数 S0–S15 的采样次数分布 |

> **注意**：`credit_stall` 和 `max_stall` 是仿真结束时的快照值。如果仿真因死锁终止，此时流量已停止、credit 可能已回归，因此这些值不代表触发 S=0 时的状态。

## 9. 调试方法

如需查看每周期的实时健康分数，取消注释 Router.cc:254–273 的 health.log 输出块：

```cpp
std::ofstream hlog("m5out/health.log", std::ios::app);
hlog << "[Health] tick=" << curTick()
     << " Router " << m_id
     << " OutPort " << o << "(Up)"
     << " S=" << score
     << " input_stall=" << m_up_input_stall;
```

## 10. 运行方式

使用 `command/lab.sh` 运行仿真：

```bash
cd /path/to/gem5
bash command/lab.sh
```

关键参数在脚本顶部配置：
- `STALL_THRESHOLD=100`：停滞阈值（周期数）
- `ROUTING_ALG=3`：XY_CHIPLET 路由算法
- `SYNTHETIC=hotspot`：流量模式
- `TOPOLOGY=Chiplet2_5D`：2.5D Chiplet 拓扑

死锁日志输出到 `m5out/deadlock_<timestamp>.log`。
