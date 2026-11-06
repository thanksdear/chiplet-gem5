# IPDR + HAR 路由对照

在现有 gem5 IPDR 模型上增加 HAR 上行口选择，不修改 RTL。

## 服务器获取代码

在服务器已有仓库的根目录执行。先确认 `git status`，保留服务器上尚未提交的修改：

```bash
git checkout main
git pull --ff-only origin main
scons build/Garnet_standalone/gem5.opt -j2
```

代码更新在原仓库 `main` 分支。必须重新编译，旧二进制不包含新增参数和统计。

## 开启方式

编译：

```bash
scons build/Garnet_standalone/gem5.opt -j2
```

原 IPDR 使用 `--routing-algorithm=7`；IPDR+HAR 使用
`--routing-algorithm=7 --ipdr-har`。UHAF 仍为 `--routing-algorithm=4`。
`--ipdr-har` 不接受其他 routing algorithm，也不接受关闭 Up health monitor。
原有的健康分数位宽、alpha、严重/中度重定向偏置和传播延迟配置仍然生效。

IPDR+HAR 在目的 chiplet 的原上行网关处执行与 UHAF 相同的健康感知决策：
本地健康时直接 Up；拥塞时尝试同 chiplet 的直连更健康网关；使用原有
额外跳转限制。其余路径继续使用现有 HAR 函数中的 Chiplet XY 逻辑。
包头决定输出端口，BODY/TAIL 沿已分配路径转发。

IPDR 的跨 chiplet 优先仲裁和恢复状态机仍由 algorithm 7 控制。
**修复了原 IPDR 同时能触发 UHAF escape recovery 的混用问题：**
IPDR 和 IPDR+HAR 都不创建/吸收/重注入 UHAF escape buffer。
这个修复会影响旧 IPDR 在出现停顿报警时的行为，因此应配对重跑 IPDR
与 IPDR+HAR，不能直接拼接旧 CSV。其他路由模式的恢复条件保持原样。

## 配对运行

从仓库根目录运行，输出目录必须尚不存在：

```bash
python3 command/run_ipdr_har_comparison.py \
  --output data/ipdr_har/new_sweep \
  --schemes IPDR IPDR+HAR UHAF \
  --vcs 2 4 --seeds 3000 \
  --traffic uniform_random bit_reverse transpose hotspot_multi \
  --rates 0.003 0.006 0.009 0.012 0.015 \
  --sim-cycles 100000 --stall-threshold 20 --alpha 0.6
```

上面的注入率只是命令示例，不代表这些点已经覆盖饱和区间。
按现有实验拓扑和饱和区间指定完整采样点。默认 4 chiplets、2×2 排列；
2×4 使用 `--num-chiplets 8 --mesh-rows 2 --mesh-cols 4`，
4×4 使用 `--num-chiplets 16 --mesh-rows 4 --mesh-cols 4`。
对照使用相同 seed、注入率、VC/缓冲容量和健康信息配置。
脚本使用正常连续流量，不重置 warmup 统计；需要预热时应进一步统一统计窗口。

脚本保存每次运行命令、config.ini、stats.txt、完整日志及源文件/二进制哈希；
`results.csv` 含原始延迟、收发计数和机制统计，另有 `manifest.json`。
不同 VC 数和随机种子的行需要分别处理，不能直接当作同一条曲线连接。
可以用 `--packets-per-source N` 进行有限注入/排空验证，该模式会检查
所有源均注入指定包数且包/flit 收发总数一致；它不替代完整包身份和顺序审计。
持续注入的 PASS 只表示基本运行检查通过，不表示截至结束时所有包均已送达。

新增 `har_decisions`、`har_redirects`、`ipdr_priority_grants`、
`ipdr_recovery_entries`、`ipdr_recovery_finishes` 和
`uhaf_escape_absorptions` 统计。前两项应在 IPDR 为零，
IPDR+HAR 在有候选和足够拥塞时应实际重定向；两个 IPDR 模式最后一项均应为零。
恢复 entries/finishes 只表示状态机转换次数，不是检测正确率或成功恢复次数。

绘图样式增加 `IPDR+HAR`，分组名为 `ipdr_har`，不改变已有论文图的数据或分组。

## 验证范围

这是**现有简化 IPDR 模型上的 HAR 组合实验**。当前 IPDR 恢复缓冲只维护
使用数量，没有真的吸收、保存和重新注入包；恢复结束也可能因为计数本来为零。
现有 allocator 的 IPDR recovery guard 检查 interposer 的 `Down` 输入，
而当前 Chiplet2_5D 中来自 chiplet 的输入实际叫 `Up`；这次没有重写该旧模型。
因此这次组合不能用于声称实现了论文完整 IPDR，不能从状态机计数推断真实
死锁已被解除，也不能把 UHAF 的专用 forced-deadlock 场景直接当作 IPDR 验证。
HAR 改变了通道依赖，完整 IPDR 的 VN/路径约束与组合后的恢复正确性仍需另验。
