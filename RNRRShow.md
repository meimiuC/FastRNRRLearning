# FastRNRR

---

## 1. 具体使用方法

假设输入如下：

使用.obj .ply

- 源网格：`data/source_mesh.obj`
- 目标网格：`data/target_mesh.obj`
- 输出前缀：`mesh_res`
- 不使用 landmark（先走纯最近点 + 鲁棒核）

命令行使用方法：

- 注意：因为Fast_RNRR.exe 运行是需要同时传入3/4个参数，所以要在命令行中执行

```bash
./Fast_RNRR data/source_mesh.obj data/target_mesh.obj mesh_res
```

---

## 2. RNRR 执行过程 

完整流程：

1. `main.cpp` 解析参数 + 设置超参数
2. 读网格并做统一尺度归一化（`mesh_scaling`）  
3. 刚性初始化（构建目标 KD 树 + 初始对应）  
4. “刚性 ICP”阶段（但默认 `rigid_iters=0`，跳过）  
5. 非刚性初始化（对应关系、`Data_nu`、测地采样节点、构建稀疏矩阵）  
6. 非刚性主循环（Welsch + GNC + Anderson）  
7. 回写变形后源网格并输出

[?] 问题：

1. 初步配准包括旋转，整体缩放，平移，如果打印了较厚的底座，是否有必要开启刚性ICP
以及指定landmark
2. 
---

## 3. 入口与参数：流程是如何被驱动的

### 3.1 命令行与可选 landmark


含义：  
- 3 参数：纯自动对应。  
- 4 参数：额外读取 landmark 强约束。

### 3.2 默认使用GNC Anderson


- 启用动态 GNC（`nu` 分阶段衰减）；  
- 启用 Anderson 加速；  
- **不走 L-BFGS 分支**（`use_lbfgs=false`）；  

[?]
> 深度观察：`RegParas` 默认 `rigid_iters = 0`（`src/tools/tools.h` 约 L145），而 `main.cpp` 未覆盖该值；所以“刚性配准阶段”常被实际跳过，仅做初始化。

---

## 4. 预处理：读入 + 统一尺度

### 4.1 读网格


### 4.2 统一尺度归一化
- 注意：在使用Fast_RNRR.exe 之前，要现在polyworks中进行初步的刚性配置
        而对于缩放大小，因为模型和扫描的参数设置均为mm，所以没有放缩必要

- 输出时再乘回 `scale`（`write_data`）。

---

## 5. 刚性阶段（RNRR 前粗对齐）


### 5.1 刚性初始化：构建目标 KD 树 + 初始对应

[!] 每一次变形，都要重新调用KDTree
        对的，点在变形之后，就要拿着新的坐标找KDTree确认自己现在距离Target mesh上
        的哪个点最近，这个距离（也就是残差），会传到Welsch函数中，让Welsch函数为这
        个点在变形图中赋予新的权重
[!] 在KDTree中，使用L2距离（不开根号），而不是欧氏距离，以增加运算速度
代码：`src/Registration.cpp::rigid_init`（约 L76-L102）

作用：  
- `tar_points_` 提供目标点集矩阵；  
- `target_tree` 供最近邻查询；  
- 建立第一版源-目标对应。
### 5.2 对应关系建立与剪枝
说明：  
- 先最近点匹配，再按距离/法向剪掉不可信匹配。  
    通过剪切功能，更加适合薄壁类型
- 若 `use_landmark=true`，`InitCorrespondence` 会覆盖为 landmark 对应（`Registration.cpp` 约 L391-L402）。

### 5.3 刚性 ICP 主循环（SVD 求 `R,t`）

代码：`src/Registration.cpp::DoRigid`（约 L197-L240）

SVD求解
## 6. 非刚性初始化：RNRR 开始

非刚性初始化在 `NonRigidreg::Initialize()`，其中先调用基类 `nonrigid_init()`。

### 6.1 初始化 `Data_nu`（Welsch 数据项尺度）

代码：`src/Registration.cpp::nonrigid_init`（约 L149-L170）
直观上：`Data_nu` 来自“当前对应误差的中位数”，是鲁棒核起始尺度。

### 6.2 采样控制节点（测地距离）与图构建

代码：`src/NonRigidreg.cpp::Initialize`（约 L87）

核心算法在 `src/tools/nodeSampler.cpp::sampleAndconstuct`（约 L68-L167）：


1. 用网格平均边长确定采样半径；  
2. 以测地距离传播方式选控制节点；  
3. 构建顶点-节点影响权重和节点邻接图。

- 为什么要使用测地距离：
    
    1. 欧氏距离是一条直线，测地距离在模型表面
    2. 使用欧氏距离忽略了模型的真实情况，只能考虑到最近的直线距离

- 使用测地距离的优势：
    
    1. 均匀分布控制节点：
        构建变形图时，要从节点{i}的周围找到控制节点，依靠测地距离找到
        的控制节点，不会集中在一起，而是符合物体表面情况
    2. 测地距离也用来计算权重

### 6.3 构造稀疏系统矩阵（数据项 + 光滑项）

代码：`src/tools/nodeSampler.cpp::initWeight`（约 L185-L274）

在 `Initialize` 中接收为：

并设置初始变换：

- `Smat_X_`：每个节点一个 `4x3` 仿射块，初值单位；  
- `Smat_R_`：每节点近旋转矩阵初值单位；  
- `Smat_L_`,`Smat_J_`：正交项稀疏算子。

- 什么是稀疏矩阵，位深么要使用稀疏矩阵：
    
    1. 稠密矩阵：
        ```cpp
        Eigen::MatrixXd
        ```
        必须为矩阵中的每个数字分配内存空间，计算时要遍历每个数字
    2. 稀疏矩阵：
        ```cpp
        Eigen::SparseMatrix<double>
        ```
        只记录非零数字
        在RNRR中定义
        ```cpp
        // NonRigidreg.h 中的核心变量
        Eigen::SparseMatrix<double> Weight_PV_; // 顶点与控制节点之间的蒙皮权重矩阵
        Eigen::SparseMatrix<double> Smat_B_;    // 控制节点之间的相邻光滑约束矩阵
        ```
        1. 蒙皮权重矩阵：
            对于用变形图进行变形，控制节点的权重有很多近似于0,这是在稀
            疏矩阵中可以加快计算
        2. 光滑约束矩阵：
            所有控制节点连接成一张图，每个节点只与临近节点有约束关系

---

## 7. 非刚性主循环（核心）：Welsch + GNC + 线性解 + Anderson

主函数：`src/NonRigidreg.cpp::DoNonRigid`（约 L252-L570）

### 7.1 外层阶段：初始化 `nu1, nu2`

其中耦合关系在 `UpdateNuCoupledWeights`（约 L209-L214）：


这代表：`nu` 变化会同步改变平滑/正交项权重，保持阶段性平衡。

### 7.2 每个 outer iteration 的计算流

#### 概念
1. 迭代：
   在RNRR中，主要进行Welsch中GNC迭代，控制参数$\nu$从大到小衰减 
    1. 外层迭代：
        在一个$\nu$下，不断寻找最近点，再重新计算权重，构造代理函数求解
    2. 内层迭代：
        Anderson中，多步残差的迭代
2. 目标'mat_U0_':
    变形通过计算误差不断迭代，目标项中储存源点云和离源点云最近的目标点的最近坐标
3. 加权系统：
    最终的计算为解方程组$Ax=b$，引入的Welsch函数带有权重
4. Cholesky分解：
    解正定矩阵的方法
    

#### 具体迭代流程

1) 由当前对应关系装配目标项 `mat_U0_`。  
    来自于上一轮的变形，顶点位置改变后，再次调用KDTree，让每个源顶点在 Target 
    表面重新寻找最近，点找到的 3D 坐标被逐行装入 mat_U0_
    保证了当前目标的正确
2) 计算 Welsch 权重（数据项、平滑项）：
    提取`mat_U0_` 中点与源点的距离$d$
    Welsch 函数定义（`NonRigidreg.cpp` 约 L896-L922）：
    $weight = exp(-d^2 / (2 * nu^2))$
    
    - 核心：
        当$d$ 特别大时，`weight` 接近于0,权重低；反之，高
        体现了Welsch的自适应

3) 形成加权系统：

    有 landmark 时再加 `gamma * Sub_PV_^T Sub_PV_`。
    在MM算法中，能量函数替换为二次代理函数


4) Cholesky 分解：
    最消耗算力的一步
5) 进入求解分支（当前默认 Anderson 分支）：

- `use_anderson=true`：`DirectSolve` + `AndersonStep`  
    使用Anderson 加速求解
    
    - 为什么使用Anderson：
        
        1. 牛顿法：
            一阶导数    梯度    在最陡峭的地方下降
            二阶导数Hessian矩阵 可以找到最佳的方向
            缺点：N个变量，计算过于复杂
        2. Anderson：
            
            1. 不动点：
                求解不动点：算法收敛时，状态不再改变，即达到了不动点 (Fixed-Point)：$x^* = G(x^*)$
                定义残差：$$f_k = G(x_k) - x_k$$
                残差为零，则找到了不动点，即满足了优化的目标（找到了
                函数的极小值点）
            2. 优势：
                
                - 现在的问题： 计算复制、离散
                - Anderson 的好处：
                    对于Anderson，现在有$G$，$G$进行
                    如果当前的变量$x_{k}$在$G(x_{k})$得到$x_{k+1} = G(x_k)$
                    则说明找到了不动点
            3. 例子：
                对于函数$x=cos(x)$：
                
                 1. 普通做法：
                    随便猜一个初始值，比如 $x_0 = 0.5$。$x_1 = \cos(0.5) \approx 0.877$$x_2 = \cos(0.877) \approx 0.639$$x_3 = \cos(0.639) \approx 0.802$$x_4 = \cos(0.802) \approx 0.695$...（你会发现它在 0.739 左右来回震荡，收敛非常缓慢）。

                 2. Anderson：
                    记录：$x_1, x_2, x_3, x_4$

                    算出残差：$f_1 = x_2 - x_1$, $f_2 = x_3 - x_2$, $f_3 = x_4 - x_3$

                    解残差的最小二乘问题，例如：如果把 $x_3$ 赋予 60% 的权重，把 $x_4$ 赋予 40% 的权重，它们产生的残差混合后最接近 0

                    则下一次计算的起点改为：
                    $x_{new} = 0.6 \times G(x_3) + 0.4 \times G(x_4)$
                    $x_{new}$被用来计算$cos(x)$
            4. 在RNRR中：
                对一个配准问题，目标是两个mesh的点的距离和最
                小定义$G$：

                1. KD-Tree，找到目标点最近的点
                2. 计算变换矩阵
                3. 挪动mesh

                $G$包括这三步

- `use_anderson=false && !use_lbfgs`：直接线性解，不使用  

6) 更新网格顶点并重找对应：

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

最终返回 `X_aa`（reshape 回 `4r x 3`）。

### 8.3 历史缓存与稳定性

主循环中历史记录：


数值保护：

- 若 `X_anderson` 非有限值，回退到 `X_new`；  
- 可选 safeguard（当前默认关）：
  `||X_anderson-X_new||` 过大则回退。

### 8.4 与 GNC 的交互

当 `nu` 阶段变化时，代码会重置 Anderson 历史：

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
`update_R`/`sample_energy` 中每节点做 SVD，把 `3x3` 线性块投影到最近旋转。

---
<!---->
<!-- ## 11. 与“注释叙述”相比，当前版本运行时应特别注意的点 -->
<!---->
<!-- 1. `main.cpp` 注释写有“刚性配准”，但默认 `rigid_iters=0`，通常不迭代。   -->
<!-- 2. 注释常提到 L-BFGS，但默认 `use_lbfgs=false`，实际以 Anderson 分支为主。   -->
<!-- 3. GNC 策略是自适应的（并支持环境变量 `FASTRNRR_GNC_MODE` 切 fixed/adaptive）。 -->
<!---->
---

## 11. 按功能的分类

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

