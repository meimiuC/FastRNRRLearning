# FastRNRR：一个具体 RNRR 配准案例的完整流程（含 Anderson 细节与代码映射）

> 说明  
> 1) 本文基于当前仓库代码静态阅读整理，不修改任何算法代码。  
> 2) 代码路径主要是 `src/main.cpp`、`src/Registration.*`、`src/NonRigidreg.*`、`src/tools/nodeSampler.*`。  
> 3) 下面给出一个**具体但可泛化**的配准例子：把源三角网格（中性人脸）配准到目标网格（微笑人脸）。

---

## 1. 具体案例设定

假设输入如下：

- 源网格：`data/face_neutral.obj`
- 目标网格：`data/face_smile.obj`
- 输出前缀：`out/face_`
- 不使用 landmark（先走纯最近点 + 鲁棒核）

命令形态（与 `README`/`main.cpp` 一致）：

```bash
./Fast_RNRR data/face_neutral.obj data/face_smile.obj out/face_
```

---

## 2. 先给结论：这套 RNRR 在该仓库里的“真实执行骨架”

完整链路是：

1. `main.cpp` 解析参数 + 设置超参数  
2. 读网格并做统一尺度归一化（`mesh_scaling`）  
3. 刚性初始化（构建目标 KD 树 + 初始对应）  
4. “刚性 ICP”阶段（但默认 `rigid_iters=0`，通常会被跳过）  
5. 非刚性初始化（对应关系、`Data_nu`、测地采样节点、构建稀疏矩阵）  
6. 非刚性主循环（Welsch + GNC + 线性求解/Anderson/L-BFGS 分支）  
7. 回写变形后源网格并输出

---

## 3. 入口与参数：流程是如何被驱动的

### 3.1 命令行与可选 landmark

代码：`src/main.cpp`（约 L44-L66）

```cpp
if (argc == 4) {
  src_file = argv[1];
  tar_file = argv[2];
  outpath = argv[3];
} else if (argc == 5) {
  src_file = argv[1];
  tar_file = argv[2];
  outpath = argv[3];
  landmark_file = argv[4];
  paras.use_landmark = true;
}
```

含义：  
- 3 参数：纯自动对应。  
- 4 参数：额外读取 landmark 强约束。

### 3.2 当前版本默认“优化分支”非常关键

代码：`src/main.cpp`（约 L67-L110）

```cpp
paras.use_Dynamic_nu = true;
paras.use_adaptive_gnc = true;
paras.use_anderson = true;
paras.anderson_m = 5;
paras.use_lbfgs = false;
paras.max_inner_iters = 1;
```

这意味着在当前默认配置下：

- 启用动态 GNC（`nu` 分阶段衰减）；  
[?]GNC的原理是什么
- 启用 Anderson 加速；  
- **不走 L-BFGS 分支**（`use_lbfgs=false`）；  
- 内层每次实际上是“加权线性系统 + Anderson 外推”。

[?]
> 深度观察：`RegParas` 默认 `rigid_iters = 0`（`src/tools/tools.h` 约 L145），而 `main.cpp` 未覆盖该值；所以“刚性配准阶段”常被实际跳过，仅做初始化。

---

## 4. 预处理：读入 + 统一尺度

### 4.1 读网格

代码：`src/main.cpp`（约 L120-L123）、`src/tools/io_mesh.h` 的 `read_data`

```cpp
read_data(src_file, src_mesh);
read_data(tar_file, tar_mesh);
if (src_mesh.n_vertices() == 0 || tar_mesh.n_vertices() == 0) exit(0);
```

### 4.2 统一尺度归一化（非常重要）

代码：`src/tools/tools.cpp`（约 L59-L109）

```cpp
Scalar scale = (max-min).norm();
// 所有源/目标顶点都除以同一个 scale
p = p / scale;
```

意义：  
- 让阈值（如 `distance_threshold=0.05`）在不同模型尺寸下可复用；  
- 输出时再乘回 `scale`（`write_data`）。

---

## 5. 刚性阶段（RNRR 前粗对齐）

[?]可以在初始变量中指定一些点，使得它们，能够进行初步的刚性配准
### 5.1 刚性初始化：构建目标 KD 树 + 初始对应

[!] 每一次变形，都要重新调用KDTree
        对的，点在变形之后，就要拿着新的坐标找KDTree确认自己现在距离Target mesh上
        的哪个点最近，这个距离（也就是残差），会传到Welsch函数中，让Welsch函数为这
        个点在变形图中赋予新的权重
[!] 在KDTree中，使用L2距离（不开根号），而不是欧氏距离，以增加运算速度
代码：`src/Registration.cpp::rigid_init`（约 L76-L102）

``cpp
tar_points_.resize(3, n_tar_vertex_);
target_tree = new KDtree(tar_points_);
InitCorrespondence(correspondence_pairs_);
```

作用：  
- `tar_points_` 提供目标点集矩阵；  
- `target_tree` 供最近邻查询；  
- 建立第一版源-目标对应。

### 5.2 对应关系建立与剪枝

代码：`src/Registration.cpp::FindClosestPoints`（约 L253-L269）

```cpp
int idx = target_tree->closest(src_point, mini_dist);
c.position = tar_points_.col(idx);
c.normal = Vec2Eigen(tar_mesh_->normal(...));
```

代码：`src/Registration.cpp::SimplePruning`（约 L284-L336）

```cpp
if((!use_distance || dist < pars_.distance_threshold)
   && (!use_normal || ... || angle < pars_.normal_threshold))
    corres_idx[i] = 1;
```

说明：  
- 先最近点匹配，再按距离/法向剪掉不可信匹配。  
    通过剪切功能，更加适合薄壁类型
- 若 `use_landmark=true`，`InitCorrespondence` 会覆盖为 landmark 对应（`Registration.cpp` 约 L391-L402）。

### 5.3 刚性 ICP 主循环（SVD 求 `R,t`）

代码：`src/Registration.cpp::DoRigid`（约 L197-L240）

```cpp
for(int iter = 0; iter < pars_.rigid_iters; iter++) {
  rigid_T_ = point_to_point(rig_src_v, rig_tar_v, corres_pair_ids_);
  // 应用 rigid_T_ 到所有源顶点
  FindClosestPoints(...);
  SimplePruning(...);
}
```

SVD 求解在 `point_to_point`（`Registration.cpp` 约 L480-L518）：

```cpp
Matrix33 sigma = X * w_normalized.asDiagonal() * Y.transpose();
JacobiSVD<Matrix33> svd(sigma, ...);
R = V * U^T (带反射修正), t = Y_mean - R * X_mean;
```

---

## 6. 非刚性初始化：RNRR 真正开始

非刚性初始化在 `NonRigidreg::Initialize()`，其中先调用基类 `nonrigid_init()`。

### 6.1 初始化 `Data_nu`（Welsch 数据项尺度）

代码：`src/Registration.cpp::nonrigid_init`（约 L149-L170）

```cpp
InitCorrespondence(correspondence_pairs_);
init_nus[i] = ||src_i - closest_i||;
igl::median(init_nus, pars_.Data_nu);
```

直观上：`Data_nu` 来自“当前对应误差的中位数”，是鲁棒核起始尺度。

### 6.2 采样控制节点（测地距离）与图构建

代码：`src/NonRigidreg.cpp::Initialize`（约 L87）

```cpp
sample_radius = src_sample_nodes.sampleAndconstuct(*src_mesh_, paras_.uni_sample_radio, X_AXIS);
```

核心算法在 `src/tools/nodeSampler.cpp::sampleAndconstuct`（约 L68-L167）：

```cpp
m_sampleRadius = sampleRadiusRatio * m_averageEdgeLen;
geodesic::GeodesicAlgorithmExact geoalg(&mesh, vertexIdx, m_sampleRadius);
geoalg.propagate(vertexIdx, neighbor_verts);
weight = pow(1 - pow(geodist/m_sampleRadius,2), 3);
```

这一步做了三件事：

1. 用网格平均边长确定采样半径；  
2. 以测地距离传播方式选控制节点；  
3. 构建顶点-节点影响权重和节点邻接图。

### 6.3 构造稀疏系统矩阵（数据项 + 光滑项）

代码：`src/tools/nodeSampler.cpp::initWeight`（约 L185-L274）

```cpp
// 数据项：matPV, matP
coeff.push_back(... nodeIdx*4 ...);  // 每节点 4 个自由度块

// 光滑项：matB, matD, smoothw
smoothw[k] = 1.0 / dist;
```

在 `Initialize` 中接收为：

```cpp
src_sample_nodes.initWeight(Weight_PV_, Smat_P_, Smat_B_, Smat_D_, Sweight_s_);
```

并设置初始变换：

- `Smat_X_`：每个节点一个 `4x3` 仿射块，初值单位；  
- `Smat_R_`：每节点近旋转矩阵初值单位；  
- `Smat_L_`,`Smat_J_`：正交项稀疏算子。

---

## 7. 非刚性主循环（核心）：Welsch + GNC + 线性解 + Anderson

主函数：`src/NonRigidreg.cpp::DoNonRigid`（约 L252-L570）

### 7.1 外层阶段：初始化 `nu1, nu2`

```cpp
average_len = CalcEdgelength(src_mesh_, 1);
base_nu = max(pars_.Data_nu, average_len*1e-4);
nu1 = Data_initk * base_nu;
end_nu1 = Data_endk * average_len;
nu2 = Smooth_nu * average_len;
UpdateNuCoupledWeights(nu1, nu2);
```

其中耦合关系在 `UpdateNuCoupledWeights`（约 L209-L214）：

```cpp
alpha = ori_alpha * nu1^2 / nu2^2;
beta  = ori_beta  * 2 * nu1^2;
```

这代表：`nu` 变化会同步改变平滑/正交项权重，保持阶段性平衡。

### 7.2 每个 outer iteration 的计算流

1) 由当前对应关系装配目标项 `mat_U0_`。  

2) 计算 Welsch 权重（数据项、平滑项）：

```cpp
weight_d_ = (Weight_PV0 * Smat_X_ + Smat_P_ - mat_U0_^T).rowwise().norm();
welsch_weight(weight_d_, nu1);
welsch_weight_s = (Smat_B0 * Smat_X_ - Smat_D0).rowwise().norm();
welsch_weight(welsch_weight_s, nu2);
```

Welsch 函数定义（`NonRigidreg.cpp` 约 L896-L922）：

```cpp
energy: 1 - exp(-r^2/(2p^2))
weight: exp(-r^2/(2p^2))
```

3) 形成加权系统：

```cpp
mat_A0_ = W^T W + alpha * B^T B + beta * L
mat_VU_ = W^T UP + alpha * B^T D
```

有 landmark 时再加 `gamma * Sub_PV_^T Sub_PV_`。

4) Cholesky 分解：

```cpp
ldlt_->analyzePattern(mat_A0_); // 首次
ldlt_->factorize(mat_A0_);      // 每次
```

5) 进入求解分支（当前默认 Anderson 分支）：

- `use_anderson=true`：`DirectSolve` + `AndersonStep`  
- `use_anderson=false && !use_lbfgs`：直接线性解  
- `use_lbfgs=true`：`QNSolver`（L-BFGS + 线搜索）

6) 更新网格顶点并重找对应：

```cpp
target = Weight_PV0 * Smat_X_ + Smat_P_;
SetMeshPoints(src_mesh_, target, curV);
FindClosestPoints(correspondence_pairs_);
SimplePruning(...);
```

7) 计算阶段收敛（位移 max-norm）并做 GNC 策略判定。

---

## 8. Anderson 加速：在本实现里到底怎么工作

相关代码：

- 开关与参数：`main.cpp`（`use_anderson=true`, `anderson_m=5`）  
- 历史初始化：`InitAndersonHistory`（`NonRigidreg.cpp` L572-L582）  
- 固定点求解器：`DirectSolve`（L584-L594）  
- Anderson 外推：`AndersonStep`（L596-L632）  
- 主循环调用点：`DoNonRigid`（L385-L415）

### 8.1 固定点视角

当前迭代变量是节点仿射变量 `X = Smat_X_`。  
`DirectSolve()` 给出一次“基线更新” `g(X)`（求解线性系统）。  
定义残差 `f(X) = g(X) - X`。  
Anderson 用历史残差差分逼近更优更新。

### 8.2 代码级步骤（对应 `AndersonStep`）

```cpp
g_cur = vec(X_new);          // X_new = g(X_old)
x_cur = vec(X_old);
f_cur = g_cur - x_cur;

delta_f(:,i) = F_hist[idx_cur] - F_hist[idx_pre];
delta_g(:,i) = g_hist[idx_cur] - g_hist[idx_pre];

coeff = argmin ||delta_f * coeff - f_cur||   // QR solve
x_aa  = g_cur - delta_g * coeff;
```

最终返回 `X_aa`（reshape 回 `4r x 3`）。

### 8.3 历史缓存与稳定性

主循环中历史记录：

```cpp
anderson_X_hist_[buf] = vec(X_old);
anderson_g_hist_[buf] = vec(X_new);
anderson_F_hist_[buf] = vec(X_new - X_old);
```

数值保护：

- 若 `X_anderson` 非有限值，回退到 `X_new`；  
- 可选 safeguard（当前默认关）：
  `||X_anderson-X_new||` 过大则回退。

### 8.4 与 GNC 的交互

当 `nu` 阶段变化时，代码会重置 Anderson 历史：

```cpp
anderson_iter_ = 0;
anderson_buf_idx_ = 0;
InitAndersonHistory();
```

这是合理的：因为目标函数形态（权重和系数矩阵）已经改变，旧历史不再同分布。

---

## 9. GNC 自适应阶段控制（当前实现的“鲁棒调度器”）

关键代码：`NonRigidreg.cpp`（约 L470-L560）

策略核心：

1. 评估阶段指标：
   - `grad_norm = ||grad_X_||`
   - `hdiag_min = min(diag(mat_A0_))`

2. 自适应衰减判断：
   - 收敛且梯度小，允许缩小 `nu`；  
   - `hdiag_min` 较好时用快衰减 `gnc_decay_fast`，否则慢衰减 `gnc_decay_slow`；  
   - 若长期停滞且允许 fallback，则强制乘 `0.5`。

3. 停止条件：
   - `!use_Dynamic_nu`，或 `!data_use_welsch`，或 `nu1` 已达 `end_nu1`。

还会把每阶段日志写到 `out_gt_file`（表头 `#stage policy nu1 nu2 ...`）。

---

## 10. 能量项与矩阵语义（把流程真正串起来）

从代码可读出的目标结构：

- 数据项：让变形后源点靠近目标对应点；  
- 光滑项：约束相邻节点变换一致；  
- 正交项：约束局部线性变换接近旋转；  
- 标记项：landmark 强约束（可选）。

在 `sample_energy`（`NonRigidreg.cpp` L648-L672）中：

```cpp
E = ||Weight_PV_*X - Smat_UP_||^2
  + alpha * ||Smat_B_*X - Smat_D_||^2
  + beta  * Σ||X_i(0:2,:) - R_i||^2
  + gamma * ||Sub_PV_*X - Sub_UP_||^2 (optional)
```

`update_R`/`sample_energy` 中每节点做 SVD，把 `3x3` 线性块投影到最近旋转。

---

## 11. 与“注释叙述”相比，当前版本运行时应特别注意的点

1. `main.cpp` 注释写有“刚性配准”，但默认 `rigid_iters=0`，通常不迭代。  
2. 注释常提到 L-BFGS，但默认 `use_lbfgs=false`，实际以 Anderson 分支为主。  
3. GNC 策略是自适应的（并支持环境变量 `FASTRNRR_GNC_MODE` 切 fixed/adaptive）。

---

## 12. 该具体例子的“逐环节代码映射清单”

| 流程环节 | 关键函数/位置 | 作用 |
|---|---|---|
| 参数解析 | `src/main.cpp` L44-L66 | 读取输入/可选 landmark |
| 参数设置 | `src/main.cpp` L67-L110 | 设置 α、β、阈值、GNC、Anderson 等 |
| 读取网格 | `src/main.cpp` L120-L123 + `io_mesh.h::read_data` | 读源/目标网格 |
| 归一化 | `tools.cpp::mesh_scaling` L59-L109 | 统一尺度，避免参数依赖模型绝对尺寸 |
| 刚性初始化 | `Registration.cpp::rigid_init` L76-L102 | 构建 `tar_points_` 与 KD 树 |
| 初始对应 | `Registration.cpp::FindClosestPoints` L253-L269 | 最近点匹配 |
| 对应剪枝 | `Registration.cpp::SimplePruning` L284-L336 | 距离/法向过滤 |
| 刚性求解 | `Registration.cpp::DoRigid` L197-L240 | ICP 外层流程 |
| 刚性 SVD | `Registration.cpp::point_to_point` L480-L518 | 求 `R,t` |
| 非刚性基础初始化 | `Registration.cpp::nonrigid_init` L149-L182 | `Data_nu` 初值 + 对应 |
| 控制节点采样 | `nodeSampler.cpp::sampleAndconstuct` L68-L167 | 测地传播 + 权重核 |
| 稀疏矩阵构建 | `nodeSampler.cpp::initWeight` L185-L274 | `Weight_PV_`,`Smat_B_`,`Smat_D_` |
| 非刚性初始化总控 | `NonRigidreg.cpp::Initialize` L62-L207 | 初始化所有优化变量 |
| 非刚性主循环 | `NonRigidreg.cpp::DoNonRigid` L252-L570 | Welsch + GNC + 求解/更新 |
| Anderson 历史 | `NonRigidreg.cpp::InitAndersonHistory` L572-L582 | 初始化缓存 |
| 线性基线解 | `NonRigidreg.cpp::DirectSolve` L584-L594 | `A^{-1}b` |
| Anderson 更新 | `NonRigidreg.cpp::AndersonStep` L596-L632 | 历史差分外推 |
| 鲁棒核 | `NonRigidreg.cpp::welsch_energy/weight` L896-L922 | 非凸鲁棒降权 |
| 阶段调度 | `NonRigidreg.cpp` L470-L560 | 自适应 GNC 衰减策略 |
| 更新网格 | `NonRigidreg.cpp::SetMeshPoints` L935-L952 | 写回变形后顶点 |
| 输出结果 | `main.cpp` L184-L186 + `io_mesh.h::write_data` | 恢复尺度并保存 |

---

## 13. 一句话总结这个案例里的 RNRR“算法本质”

在该实现中，RNRR 本质上是：  
**“以测地采样节点驱动的局部仿射变形模型 + Welsch 鲁棒重加权 + GNC 分阶段收缩 +（默认）Anderson 固定点加速”**，通过“对应更新 ↔ 变形求解”交替迭代，把源网格逐步拉向目标形状。

