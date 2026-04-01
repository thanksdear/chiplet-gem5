#!/bin/bash
# =============================================================================
# run_garnet_chiplet2_5d.sh
# 使用 Garnet3.0 + Chiplet2_5D 拓扑运行 gem5 NoC 仿真
#
# 拓扑结构:
#   - 4 个 chiplet (2×2 mesh 排列)
#   - 每个 chiplet: 4×4 = 16 个路由器 → 共 64 个 chiplet 路由器
#   - 每个 chiplet 通过 4 个角网关连接到 interposer
#   - Interposer: 4×4 = 16 个路由器
#   - 总计: 80 个路由器
#
# 节点约束:
#   num_cpus + num_dirs 必须能被 64 整除 (平均分配到 chiplet 路由器)
# =============================================================================

set -e

# ----------------------------- 路径配置 -------------------------------------
GEM5_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GEM5_BIN="${GEM5_ROOT}/build/Garnet_standalone/gem5.debug"
GEM5_SCRIPT="${GEM5_ROOT}/configs/example/garnet_synth_traffic.py"

# ----------------------------- 仿真参数 -------------------------------------
NUM_CPUS=64           # CPU 节点数 (对应 64 个 chiplet 路由器，每路由器 1 个 CPU)
NUM_DIRS=64           # 目录节点数 (num_cpus + num_dirs = 128 = 64*2)
SIM_CYCLES=10000      # 仿真周期数 (garnet_synth_traffic.py 已改为 1ns tick 精度，1GHz 下 1 tick=1 cycle，直接填周期数)
INJECTION_RATE=0.1    # 注入率 (packets/cycle/node, 范围 0~1)
SYNTHETIC=uniform_random  # 流量模式
ROUTER_LATENCY=1      # 路由器延迟 (cycles)
LINK_LATENCY=1        # 默认链路延迟 (cycles)
VCS_PER_VNET=4        # 每个虚拟网络的虚拟通道数

# ----------------------------- 输出目录 -------------------------------------
OUTPUT_DIR="${GEM5_ROOT}/m5out/chiplet2_5d_${SYNTHETIC}_inj${INJECTION_RATE}"
mkdir -p "${OUTPUT_DIR}"

# ----------------------------- 参数解析 -------------------------------------
# 允许通过命令行覆盖默认参数
while [[ $# -gt 0 ]]; do
    case "$1" in
        --synthetic=*)     SYNTHETIC="${1#*=}"      ;;
        --injectionrate=*) INJECTION_RATE="${1#*=}" ;;
        --sim-cycles=*)    SIM_CYCLES="${1#*=}"     ;;
        --num-cpus=*)      NUM_CPUS="${1#*=}"        ;;
        --num-dirs=*)      NUM_DIRS="${1#*=}"        ;;
        --outdir=*)        OUTPUT_DIR="${1#*=}"     ;;
        -h|--help)
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --synthetic=<模式>       流量模式 (默认: uniform_random)"
            echo "    可选值: uniform_random, tornado, bit_complement,"
            echo "            bit_reverse, bit_rotation, neighbor, shuffle, transpose"
            echo "  --injectionrate=<率>     注入率 0~1 (默认: 0.1)"
            echo "  --sim-cycles=<周期>      仿真周期数，1ns tick 精度下直接等于 cycles (默认: 10000)"
            echo "  --num-cpus=<数量>        CPU 数量,需与 num_dirs 之和能被 64 整除 (默认: 64)"
            echo "  --num-dirs=<数量>        目录控制器数量 (默认: 64)"
            echo "  --outdir=<路径>          输出目录"
            echo ""
            echo "拓扑说明:"
            echo "  Chiplet2_5D: 4 chiplets (2×2), 每个 chiplet 4×4 mesh"
            echo "  共 80 个路由器 (64 chiplet + 16 interposer)"
            echo "  片内链路延迟: 1 cycle, TSV 延迟: 5 cycles, Interposer: 1 cycle"
            exit 0
            ;;
        *)
            echo "未知参数: $1，使用 -h 查看帮助"
            exit 1
            ;;
    esac
    shift
done

# 更新输出目录（考虑命令行覆盖参数后）
OUTPUT_DIR="${GEM5_ROOT}/m5out/chiplet2_5d_${SYNTHETIC}_inj${INJECTION_RATE}"
mkdir -p "${OUTPUT_DIR}"

# ----------------------------- 前置检查 -------------------------------------
if [ ! -f "${GEM5_BIN}" ]; then
    echo "[错误] gem5 可执行文件不存在: ${GEM5_BIN}"
    echo "请先编译 Garnet_standalone 协议:"
    echo "  cd ${GEM5_ROOT}"
    echo "  scons build/Garnet_standalone/gem5.debug -j\$(nproc)"
    exit 1
fi

TOTAL_NODES=$((NUM_CPUS + NUM_DIRS))
if (( TOTAL_NODES % 64 != 0 )); then
    echo "[错误] num_cpus(${NUM_CPUS}) + num_dirs(${NUM_DIRS}) = ${TOTAL_NODES}"
    echo "       必须能被 64 整除 (Chiplet2_5D 有 64 个 chiplet 路由器)"
    exit 1
fi

# ----------------------------- 打印配置信息 ---------------------------------
echo "============================================================"
echo "  gem5 Garnet3.0 + Chiplet2_5D 仿真"
echo "============================================================"
echo "  gem5 二进制  : ${GEM5_BIN}"
echo "  仿真脚本     : ${GEM5_SCRIPT}"
echo "  输出目录     : ${OUTPUT_DIR}"
echo ""
echo "  [拓扑] Chiplet2_5D"
echo "    - 4 个 chiplet，2×2 排列"
echo "    - 每个 chiplet: 4×4 mesh (16 路由器)"
echo "    - Interposer: 16 个路由器 (每 chiplet 4 个角网关)"
echo "    - 总路由器数: 80"
echo ""
echo "  [流量] 流量模式: ${SYNTHETIC}"
echo "  [流量] 注入率 : ${INJECTION_RATE} packets/cycle/node"
echo "  [流量] 仿真周期: ${SIM_CYCLES} cycles"
echo ""
echo "  [网络] num_cpus   : ${NUM_CPUS}"
echo "  [网络] num_dirs   : ${NUM_DIRS}"
echo "  [网络] 总节点数   : ${TOTAL_NODES} (每路由器 $((TOTAL_NODES / 64)) 个节点)"
echo "  [网络] 路由器延迟 : ${ROUTER_LATENCY} cycles"
echo "  [网络] VC 数/vnet : ${VCS_PER_VNET}"
echo "============================================================"
echo ""

# ----------------------------- 执行仿真 -------------------------------------
echo "[运行] 启动仿真..."
echo ""

"${GEM5_BIN}" \
    --outdir="${OUTPUT_DIR}" \
    "${GEM5_SCRIPT}" \
    --num-cpus=${NUM_CPUS} \
    --num-dirs=${NUM_DIRS} \
    --network=garnet \
    --topology=Chiplet2_5D \
    --router-latency=${ROUTER_LATENCY} \
    --link-latency=${LINK_LATENCY} \
    --vcs-per-vnet=${VCS_PER_VNET} \
    --sim-cycles=${SIM_CYCLES} \
    --synthetic=${SYNTHETIC} \
    --injectionrate=${INJECTION_RATE} \
    --inj-vnet=0

SIM_EXIT=$?

echo ""
if [ ${SIM_EXIT} -eq 0 ]; then
    echo "============================================================"
    echo "  仿真完成！"
    echo "  结果文件: ${OUTPUT_DIR}/stats.txt"
    echo "============================================================"
    echo ""
    # 提取关键 NoC 性能指标
    echo "[统计] 关键性能指标 (latency 单位: cycles，tick 精度 1ns，1 tick = 1 cycle @ 1GHz):"
    if [ -f "${OUTPUT_DIR}/stats.txt" ]; then
        grep -E "^system\.ruby\.network\.(average|packets_injected|packets_received|flits_injected)" \
            "${OUTPUT_DIR}/stats.txt" | grep -v "::" | head -20 || true
    fi
else
    echo "[错误] 仿真失败，退出码: ${SIM_EXIT}"
    exit ${SIM_EXIT}
fi
