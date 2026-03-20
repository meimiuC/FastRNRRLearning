# FastRNRR 中 `Data_nu` 的设定与迭代（逐行讲解）

本文只讲与 `Data_nu / nu1 / nu2` 相关的关键代码行，并按源码行号解释“如何设定、如何迭代、与哪些模块联动、最终实现什么功能”。

---

## 1) 相关文件总览

- `src/tools/tools.h`：参数定义与默认值（`Data_nu`、GNC开关和阈值）
- `src/main.cpp`：运行时参数赋值（开启动态 GNC、自适应策略、可切换 fixed/adaptive）
- `src/Registration.cpp`：`Data_nu` 的**初始化统计**（对应距离中位数）
- `src/NonRigidreg.h`：GNC 辅助函数声明
- `src/NonRigidreg.cpp`：`nu1/nu2` 初始化、每阶段更新（迭代）、Welsch 权重/能量应用

---

## 2) `Data_nu` 的参数定义与默认值

### 文件：`src/tools/tools.h`

### 行 83-96（参数语义）

- **83** `use_Dynamic_nu`：是否启用动态 `nu`（GNC 外层调度总开关）
- **84** `use_adaptive_gnc`：是否启用“自适应”GNC（而非固定 0.5 衰减）
- **85** `use_fixed_gnc_fallback`：自适应长时间不收敛时，是否回退固定衰减
- **86** `Data_nu`：数据项鲁棒核尺度基准（由初始对应距离中位数估计）
- **87** `Smooth_nu`：光滑项 `nu2` 与平均边长的倍率
- **88** `Data_initk`：初始 `nu1` 放大系数，`nu1_init = Data_initk * Data_nu`
- **89** `Data_endk`：`nu1` 终止下界比例，`nu1_end = Data_endk * avg_edge_len`
- **90** `stop`：位移收敛阈值
- **91-96** 自适应 GNC 的梯度阈值、Hessian 代理阈值、快慢衰减率、最小迭代数、耐心计数

### 行 152-165（默认值）

- **152** 默认启用动态 `nu`
- **153** 默认启用自适应 GNC
- **154** 默认启用固定衰减回退
- **155** `Data_nu = 0.0`（注意：后续在 `nonrigid_init()` 中会被真实数据覆盖）
- **156** `Smooth_nu = 40`
- **157** `Data_initk = 10`
- **158** `Data_endk = 0.5`
- **160-165** 自适应阈值默认：`gnc_grad_tol=5e-4`、`fast=0.35`、`slow=0.65` 等

---

## 3) 运行时开关如何连接到 GNC

### 文件：`src/main.cpp`，行 80-109

- **80-81** `use_Dynamic_nu = true`：开启 GNC 外层调度
- **82-89** 设置自适应调度参数（梯度阈值、衰减率、耐心）
- **96-109** 支持环境变量 `FASTRNRR_GNC_MODE`
  - `fixed`：关闭自适应，走固定衰减
  - `adaptive`：启用自适应
  - 其他值：回退 adaptive

> 结论：`main.cpp` 负责“策略层配置”，`NonRigidreg.cpp` 执行“算法层更新”。

---

## 4) `Data_nu` 是如何被设定出来的（初始化）

### 文件：`src/Registration.cpp`，`Registration::nonrigid_init()` 行 149-170

- **157-158** `InitCorrespondence(correspondence_pairs_)`：先建立初始对应关系
- **160-162** 构造向量 `init_nus` 存放所有初始对应残差
- **163-168** 遍历每个对应点，计算距离：
  - 源点：`src_mesh_` 对应顶点
  - 目标点：`correspondence_pairs_[i].position`
  - 距离：欧氏范数 `||x_i - y_i||`
- **169** `igl::median(init_nus, pars_.Data_nu)`：取中位数作为 `Data_nu`

### 为什么用中位数

- 对错误对应/离群点不敏感，比均值更稳健
- 能反映“当前对应关系的典型误差尺度”
- 很适合用作鲁棒核（Welsch）初始尺度

---

## 5) `Data_nu` 与对应模块如何联动

### 文件：`src/Registration.cpp`

### `InitCorrespondence()` 行 384-388

- **387** `FindClosestPoints(corres)`：KD树最近点搜索
- **388** `SimplePruning(corres)`：按距离/法向阈值剪枝

### `FindClosestPoints()` 行 253-268

- 每个源顶点查目标最近点，形成候选对应

### `SimplePruning()` 行 284-325（节选）

- 用距离阈值/法向阈值过滤掉不可靠对应

> 因此：`Data_nu` 统计所用的样本，来源于“最近点 + 剪枝”后的初始对应集，质量更高。

---

## 6) `Data_nu` 如何进入 GNC 迭代（`nu1/nu2` 初始化）

### 文件：`src/NonRigidreg.cpp`，行 288-296

- **288** `average_len = CalcEdgelength(...)`：估计网格尺度
- **289** `base_nu = max(Data_nu, average_len * 1e-4)`：防止 `Data_nu` 过小
- **290** `nu1 = max(Data_initk * base_nu, 1e-8)`：数据项初始尺度（通常较大）
- **291** `end_nu1 = max(Data_endk * average_len, 1e-8)`：数据项最小终值
- **292** `nu2 = max(Smooth_nu * average_len, 1e-8)`：光滑项初始尺度
- **296** `UpdateNuCoupledWeights(nu1, nu2)`：同步更新 `alpha/beta`

### 文件：`src/NonRigidreg.cpp`，行 209-214（权重耦合）

- **212** `alpha = ori_alpha * nu1^2 / nu2^2`
- **213** `beta  = ori_beta  * 2 * nu1^2`

这保证 `nu` 变化时，光滑项和正交项权重随阶段联动。

---

## 7) `nu` 在每个 stage 如何迭代

### 文件：`src/NonRigidreg.cpp`，行 470-544

### A. 先计算是否可缩小 `nu`

- **474** `grad_norm = ComputeGradientNormProxy()`
- **475** `hdiag_min = ComputeHessianDiagMinProxy()`
- **493** `grad_converged = grad_norm < gnc_grad_tol`
- **494-495** `can_shrink = stage_converged && grad_converged && 达到最小外层迭代数`

### B. 自适应模式（`use_adaptive_gnc = true`）

- **499** 若曲率代理较好（`hdiag_min > eps`）选 `gnc_decay_fast`
- **499** 否则选 `gnc_decay_slow`
- **501-502** 更新：
  - `next_nu1 = max(end_nu1, nu1 * decay)`
  - `next_nu2 = max(1e-12, nu2 * decay)`
- **507-521** 若不满足缩小条件，先 `hold`；耐心超限则触发回退：
  - **512-513** 固定 0.5 衰减（policy=3）

### C. 固定模式（`use_adaptive_gnc = false`）

- **526-527** 每阶段直接 `nu1 *= 0.5`, `nu2 *= 0.5`（带下界）

### D. 停止条件

- **479-484** 任一满足即停止：
  - 关闭动态 `nu`
  - 关闭数据项 Welsch
  - `|nu1 - end_nu1| < 1e-8`

### E. 更新后联动

- **536** `UpdateNuCoupledWeights(nu1, nu2)` 更新 `alpha/beta`
- **537-542** 若用 Anderson，`nu` 改变后重置历史，避免跨阶段混用外推记忆

---

## 8) `nu1/nu2` 在优化里具体做了什么

### 文件：`src/NonRigidreg.cpp`，行 334-348

- **336-337** 数据残差先算范数，再经 `welsch_weight(..., nu1)` 转成权重
- **346-347** 光滑残差经 `welsch_weight(..., nu2)` 转成权重

### 文件：`src/NonRigidreg.cpp`，行 917-921（权重公式）

- `w = exp(-r^2/(2p^2))`，这里 `p` 就是 `nu1` 或 `nu2`

行为解释：

- `nu` 大：权重衰减慢，接近 L2，优化面更平滑
- `nu` 小：大残差权重快速趋近 0，更鲁棒，能压制离群点

### 文件：`src/NonRigidreg.cpp`，行 860-882（鲁棒总能量）

- 数据项、光滑项都可用 Welsch 能量
- 总能量用于阶段诊断和调度记录

---

## 9) 迭代日志如何输出（用于复盘）

### 文件：`src/NonRigidreg.cpp`，行 545-563

- 记录每个 stage 的：
  - `nu1`, `nu2`
  - `grad_norm`, `hdiag_min`
  - `policy_code`
  - `outer_iters`
  - `robust_energy`
- 输出到 `pars_.out_gt_file`（通常是 `*_res.txt`）

这就是你在日志里看到的 stage 表格来源。

---

## 10) 总结：`Data_nu` 设定与迭代链路（一句话）

1. **`Data_nu`** 在 `nonrigid_init()` 由“初始对应距离中位数”估计。  
2. 在 `DoNonRigid()` 中转成 **`nu1` 初值**，并设定 `nu1` 终值与 `nu2` 初值。  
3. 外层 GNC 根据收敛与曲率代理自适应（或固定）更新 `nu1/nu2`。  
4. `nu1/nu2` 直接控制 Welsch 权重，改变数据项/光滑项鲁棒性。  
5. 每次 `nu` 变化会联动 `alpha/beta`，并与 Anderson 历史重置协同。  

---

## 11) 附：policy_code 含义（对应你看到的 stage 日志）

- `0`：本阶段保持 `nu`（hold）
- `1`：自适应快速衰减（fast）
- `2`：自适应保守衰减（slow）
- `3`：耐心耗尽，触发固定 0.5 回退衰减
- `4`：终止阶段
- `5`：固定模式下的 0.5 衰减

