import re
import csv
import matplotlib.pyplot as plt

# 1. 读取并解析文件
file_path = './network_stats.txt'
data = []

with open(file_path, 'r') as f:
    content = f.read()
    
    # 使用正则表达式提取 注入率 和 平均延迟
    # 匹配 injectionrate:0.01
    rates = re.findall(r"injectionrate:([\d.]+)", content)
    # 匹配 system.ruby.network.average_flit_latency    26.714308
    latencies = re.findall(r"average_flit_latency\s+([\d.]+)", content)

    # 合并数据
    for r, l in zip(rates, latencies):
        data.append([float(r), float(l)])

# 2. 导出为 CSV 文件
csv_file = 'noc_performance_results.csv'
with open(csv_file, 'w', newline='') as f:
    writer = csv.writer(f)
    writer.writerow(['injection_rate', 'average_latency']) # 写入表头
    writer.writerows(data)

print(f"成功导出 CSV: {csv_file}")

# 3. 生成图像
# 提取绘图数据
x = [item[0] for item in data]
y = [item[1] for item in data]

plt.figure(figsize=(10, 6))
plt.plot(x, y, marker='o', color='r', linewidth=2, markersize=8, label='NoC Latency')

# 添加标注 (在饱和点附近标注数值)
for i, txt in enumerate(y):
    plt.annotate(f"{txt:.1f}", (x[i], y[i]), textcoords="offset points", xytext=(0,10), ha='center')

# 装饰图表
plt.title('NoC Performance: Injection Rate vs Latency', fontsize=14)
plt.xlabel('Injection Rate (pkts/node/cycle)', fontsize=12)
plt.ylabel('Average Flit Latency (cycles)', fontsize=12)
plt.grid(True, which="both", ls="--", alpha=0.7)
plt.legend()

# 如果延迟跨度过大（如你的数据从26跳到1572），建议开启对数坐标查看趋势
# plt.yscale('log') 

plt.savefig('noc_latency_chart.png', dpi=300)
print("成功生成图像: noc_latency_chart.png")
plt.show()
