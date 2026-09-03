# NavigaMer 完整世界构建与严格包含查询报告

日期：2026-09-03
代码基线：`c0a1184` 之后的 complete-world 修改
数据：E. coli，150 bp 滑窗，stride 1
参数：世界半径 `{55,35,15}`，构建 containment tolerance `3`，4 beacons
机器：AMD EPYC 9654，96 个 online CPU，62 GiB RAM

## 1. 结果摘要

这次修改把终端小世界从“每条序列只有一个 owner 的桶”改成了真正的完整编辑距离球。每个小世界 `W(C,R)` 现在物理保存所有满足 `ED(C,S)<=R` 的参考窗口。查询只有在严格证明

```text
ED(Q,C) + tolerance <= R
```

后才独占进入该小世界。证明不成立时仍执行全部相交世界的保守搜索，因此 fast path 不会改变结果集合。

完整 E. coli 构建耗时 163.44 s，生成 405 MiB 索引。100,000-query 的三次正式重复为 230,614、229,322、229,692 query/s，中位数 229,692 query/s（worker wall 0.4354 s）。每条 query 平均 57.44 次编辑距离，但 P50/P95/P99 只有 7/8/8 次；平均值由 23 个并行 block 的冷启动拉高。99,977 条热 query 平均仅 6.93 次编辑距离。另一次保存完整逐条 timing 的回归受运行时CPU频率波动影响为169,046 query/s，其热查询平均engine latency为15.37 us；报告不把wall吞吐与逐worker延迟混成同一个指标。

正确性证据包括：完整 E. coli 上 1,000 条 query 对全部 4,641,503 个窗口的穷举比较，FN=0、FP=0；新旧精确索引的 100,000-query hits TSV 逐字节一致；Release 与 ASan/UBSan 测试全部通过。

## 2. 构建算法

### 2.1 tolerance 安全内核

终端世界半径为 `R=15`，构建保证半径 `R-2T=9` 的中心覆盖，其中 `T=3`。对任意真实答案 `S` 和 query `Q`：

```text
ED(Q,S) <= T
ED(S,C) <= R-2T
=> ED(Q,C)+T <= ED(Q,S)+ED(S,C)+T <= R
```

所以每个 tolerance 不超过3且至少有一个答案的 query，都至少存在一个完整包含 query 球的小世界。

### 2.2 完整小世界成员

第一遍在线构建只产生中心和唯一-owner骨架。第二遍为每个小世界回填半径15内的全部参考窗口，重叠区域允许重复成员。

完整成员连接在构建期使用 q-gram count lemma 产生无漏候选：长度 `L`、q-gram长度 `q`、编辑距离上限 `R` 满足

```text
shared_qgrams >= (L-q+1) - qR
```

候选使用全局Levenshtein逐一验证后才写入。q-gram只用于一次性的构建连接，不出现在查询路径，也不是seed-and-extend。100 kbp实验中，该实现生成的索引与纯全局BK范围连接索引SHA-256完全一致（见`construction_membership_sha256.txt`）；构建从24.89 s降至3.04 s，编辑距离调用从539,470,723降至10,983,619。

滑窗posting按contig occurrence保存，而不是为每个150 bp窗口重复保存；一个occurrence对应一段可能包含它的窗口区间，使用排序event sweep累计共同q-gram数量。这使构建时间随基因组规模保持可用。

### 2.3 上层世界

完整子球先通过top-down repair得到至少一个严格包含父亲：

```text
ED(parent.center, child.center) + child.radius <= parent.radius
```

随后按中心SequenceId排序，在cache-local邻域内补充所有精确验证通过的额外父边，方便边界接力。每个子球始终保留一条全局正确的骨架父边；查询的strict shortcut只对完整终端球启用，因此远端冗余父边是否物化不影响正确性。

原论文的全 overlap DAG 条件是 `ED(P,C)<=Rp+Rc`。对当前 `{55,35,15}`，抽样大/中中心的平均距离为81.92，99.08%的随机配对满足 `ED<=90`；完整物化预计产生数百亿条边。因此这里使用“严格包含骨架 + 局部多父边 + 完整叶直接跳转”的压缩表示。

### 2.4 内存布局与并行

索引仍使用pointer-free flat arrays：40-byte `WorldNode`、分层连续nodes、CSR children、连续beacons、固定宽度child×beacon距离、局部dense matrices和持久metric目录。大根节点固定使用连续MBB扫描，让硬件预取器顺序读取，而不再构建一个查询不会使用的巨大BK树。

中心选择的三个层任务并发；owner验证、完整成员回填、局部多父连接、dense矩阵和beacon距离均按范围使用多CPU。局部父连接、缓存邻叶和叶成员循环会软件prefetch后续连续项。大根顺序扫描也测试过显式prefetch，但在本机反而降低吞吐，因此最终保留连续布局并交给硬件预取器。

## 3. 查询算法

完整模式的顺序如下：

1. 预处理query，建立本条query的Myers profile和小型精确距离cache。
2. 若有上一条路径，先计算 `ED(Q,cached_leaf.center)`。若 `d+t<=15`，直接扫描该完整小世界，根/大/中层全部跳过。
3. 若缓存叶刚越界，按节点连续顺序检查 `+1,-1,+2,-2,...` 的邻近叶中心，最多64个方向步，并prefetch后续节点。一旦找到严格包含叶，同样直接进入。
4. 只有缓存和局部接力都失败时才从root冷启动。各层以beacon反三角下界剪枝，保留全部满足 `ED(Q,C)<=R+t` 的相交世界。
5. 在小世界层，一旦任何完整叶满足 `ED(Q,C)+t<=R`，就可以丢弃其他分支并独占进入该叶；否则保留全部相交叶。
6. 叶内先用最多4个beacon检查 `|ED(Q,b)-ED(S,b)|<=t`，再用bounded Edlib精确验证，SequenceId去重并排序。

旧的动态root path-pivot行在complete-world模式中完全禁用。它曾在每次刷新时计算pivot到全部大世界中心的距离，是旧查询的主要成本。

## 4. 完整 E. coli 世界结构

```text
synthetic root
└── 229,582 large worlds, R=55
    └── 257,666 large->middle edges
        └── 257,665 middle worlds, R=35
            └── 1,083,553 middle->small edges
                └── 927,924 complete small worlds, R=15
                    └── 14,754,814 terminal memberships
                        └── 4,641,503 unique reference windows
```

| 层 | 世界数 | 平均 children | P50/P95/P99 | 最大 children | beacon分布 |
|---|---:|---:|---:|---:|---|
| 大，R=55 | 229,582 | 1.122 | 1/2/2 | 3 | `{1:201517, 2:28046, 3:19}` |
| 中，R=35 | 257,665 | 4.205 | 4/5/5 | 13 | `{3:1, 4:257664}` |
| 小，R=15 | 927,924 | 15.901 | 15/15/45 | 135 | `{4:927924}` |

每个参考窗口平均属于 `14,754,814 / 4,641,503 = 3.179` 个完整小世界。逐层摘要见 `world_structure_summary.tsv`，代表节点见 `world_sample.tsv`；本地完整逐节点导出为`full_worlds.tsv`，因文件为75 MiB未计划提交Git。

需要明确：半径55在150 bp编辑空间中并不是一个能把完整E. coli压成少量顶层世界的“大半径”。55和35之间仅有20的完整子球容纳余量，所以大世界平均只有1.122个中世界。查询速度来自可证明的完整叶复用，而不是假设上层世界数量很少。

## 5. 构建性能

### 5.1 小规模，10 kbp

| 指标 | 值 |
|---|---:|
| reference windows | 9,851 |
| build wall | 0.39 s |
| complete small worlds | 1,971 |
| terminal memberships | 29,547 |
| index bytes | 1,107,963 |
| center / owner / membership | 0.129 / 0.014 / 0.070 s |
| packing / topology / beacon-MBB | 0.119 / 0.042 / 0.017 s |

### 5.2 完整基因组，4.64 Mbp

| 阶段 | 秒 | 阶段占比 |
|---|---:|---:|
| center selection | 48.106 | 29.49% |
| owner skeleton | 0.406 | 0.25% |
| complete memberships | 48.024 | 29.44% |
| containment repair/local rebinding | 51.789 | 31.75% |
| flat topology/directories | 9.450 | 5.79% |
| beacon/MBB | 5.352 | 3.28% |
| measured command wall | 163.44 | — |

峰值RSS为836,800 KiB，平均CPU利用率2444%（约24.4个满核）；在线CPU为96。最重阶段已经并行，但中心选择与拓扑仍含串行部分。

## 6. 查询性能

### 6.1 小规模100,000 queries

block=256、96 threads：worker wall 0.0373 s，2,683,471 query/s，平均8.40次编辑距离/query。99,609个有缓存前驱的query全部由缓存叶或局部叶接力严格包含。命令端到端为0.62 s，主要是FASTQ读取和timing TSV输出而非查询引擎。

### 6.2 完整索引100,000 queries

block=4352、96 threads结果：

| 指标 | 值 |
|---|---:|
| 三次重复的中位worker wall | 0.435366 s |
| 三次重复的中位worker throughput | 229,691.8 query/s |
| 保存完整timing的回归worker wall | 0.591554 s |
| 保存完整timing的完整命令wall | 1.55 s |
| timing回归mean engine latency | 104.258 us/query |
| timing回归P50 / P95 / P99 engine latency | 14.752 / 24.546 / 29.003 us |
| mean edit-distance calls | 57.438/query |
| P50 / P95 / P99 calls | 7 / 8 / 8 |
| exact leaf verifications | 2.728/query |
| dynamic pivot-row calls | 0 |

缓存统计：100,000次查询中23次为block冷启动；其余99,977次全部找到严格包含小世界。邻居接力总检查20,052个中心，仅0.201次/query。

热/冷必须分开解释：

| 集合 | 数量 | mean engine | mean ED calls |
|---|---:|---:|---:|
| hot path | 99,977 | 15.373 us | 6.929 |
| cold block starts | 23 | 386.475 ms | 219,612.0 |

冷启动仍需证明性扫描229,582个顶层世界，形成长尾；block增大可以减少冷启动，但并行块太少会降低吞吐。`block_scaling.tsv`给出block变化；三个原始重复分别保存在`full_query_block4352_rep1/2/3_stats.tsv`。后续完整timing回归发生了可见的主机频率波动，因此吞吐结论采用三次连续重复的中位数，并单列timing回归的延迟分布。

### 6.3 与修改前比较

修改前相同完整E. coli、`{55,35,15}`、tolerance 3、block=256的结果为1,934.85 query/s、平均24,387.96次编辑距离/query。新实现同样block=256为46,254.29 query/s、902.32次/query，即23.91倍吞吐提升和27.03倍调用下降。调优block=4352后中位数为229,692 query/s和57.44次/query，吞吐约提升118.7倍。汇总见`baseline_comparison.tsv`。

代价是完整成员重叠：构建wall由约82.08 s增至163.44 s，索引内存计数由204,135,568 B增至424,398,579 B，约为原来的2.08倍。

## 7. 正确性验证

- CTest Release：2/2通过。
- CTest ASan+UBSan：2/2通过。
- 单元测试逐个小世界、逐个参考序列检查：成员存在当且仅当 `ED(center,sequence)<=radius`。
- 10 kbp：1,000 queries × 9,851 windows，expected=observed=1,001，FN=0，FP=0。
- 1 Mbp：1,000 queries × 999,851 windows，expected=observed=1,001，FN=0，FP=0。
- 完整4.64 Mbp：1,000 queries × 4,641,503 windows，expected=observed=1,001，FN=0，FP=0，46.4亿对比较耗时109.29 s。
- 完整索引100,000-query hits与旧精确实现逐字节一致，SHA-256均为 `e85e501924290adc36c928aced4911460014ebb45d6a378556781cdd6e68ea40`。
- 100 kbp构建期q-gram连接与纯BK精确范围连接生成的索引逐字节一致，SHA-256均为 `4bdbb5e23f86abd2977ccdd12aff769d7a7bf2a050f494b6d0a00533d283ddab`。

## 8. 可复现命令

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j 32
ctest --test-dir build --output-on-failure

./build/navigamer build \
  --reference /home/minghao/project/2026/ecoli/fasta/ecoli.fa \
  --output ecoli_full_r55_35_15.nvm \
  --window 150 --stride 1 --radii 55,35,15 \
  --containment-tolerance 3 --build-mode topdown --local-creation \
  --beacons 4 --hot-cache 32 --threads 96 \
  --stats-output full_build_stats.tsv

./build/navigamer query \
  --index ecoli_full_r55_35_15.nvm \
  --reference /home/minghao/project/2026/ecoli/fasta/ecoli.fa \
  --queries ecoli-v2/queries_1000000bp_100k_t3.fastq \
  --tolerance 3 --threads 96 --block-size 4352 \
  --output full_hits_100k.tsv --timings full_query_timings_100k.tsv \
  --stats-output full_query_100k_stats.tsv
```

核心原始文件：`full_build_stats.tsv`、`full_build_resource.txt`、`full_query_100k_best_stats.tsv`、`full_query_100k_best_resource.txt`、`full_query_timings_100k_best.tsv`、`full_verify_1000.tsv`和`full_verify_1000_resource.txt`。

## 9. 仍然存在的边界

- 性能依赖source-sorted query locality。随机query或每条query都清空cache时，必须走冷根证明，速度会显著下降，但结果仍精确。
- `{55,35,15}`的上层分支因半径差仅20而接近一对一；若目标是减少顶层世界数，需要改变半径日程，而不是放松正确性。
- `NVMIDX4`改变了终端成员语义，旧索引必须重建。
- 完整索引、hits、逐query timing和完整world表保留在本机benchmark目录；大文件不适合直接提交普通GitHub仓库，报告提交其统计、资源日志和SHA-256。
