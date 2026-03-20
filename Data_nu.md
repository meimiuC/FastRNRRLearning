
  1) Data_nu 如何设定

  在 src/Registration.cpp 的 Registration::nonrigid_init() 中：

   - 先建立初始对应（InitCorrespondence，内部会走最近点+剪枝）
   - 计算每个对应对的距离[
   d_i = |x_i - y_i|
   ]
   - 用中位数设定[
   \text{Data_nu} = \mathrm{median}(d_i)
   ]

  对应代码位置：Registration.cpp 约 157–169 行。

  2) 它如何“迭代”（准确说是 nu1/nu2 迭代）

  在 src/NonRigidreg.cpp::DoNonRigid()：

   - 初值：
   [
   \text{base_nu}=\max(\text{Data_nu},,10^{-4}\cdot \text{avgEdgeLen})
   ]
   [
   \nu_1^{(0)}=\text{Data_initk}\cdot \text{base_nu},\quad
   \nu_{1,\text{end}}=\text{Data_endk}\cdot \text{avgEdgeLen}
   ]
   [
   \nu_2^{(0)}=\text{Smooth_nu}\cdot \text{avgEdgeLen}
   ]
   （约 288–292 行）
   - 每个 stage 后根据收敛/梯度/Hessian代理决定是否缩小（约 470–530 行）：
    - 自适应快缩 / 慢缩（gnc_decay_fast/slow）
    - 停滞时可触发固定
    
    0.5 衰减回退
    - 非自适应模式直接固定
    
    0.5 衰减
   - 停止条件：nu1 到终值、或关闭动态nu等（约 479 行）。

  3) 这套方法实现了什么功能

  通过 Welsch 权重（welsch_weight，约 917 行）： [ w_i=\exp\left(-\frac{r_i^2}{2\nu^2}\right) ] nu 大时接近 
  L2（更平滑、易收敛），nu
  小时更鲁棒（强抑制离群点）。
  所以 GNC 实现的是从“粗鲁棒”到“精鲁棒”的逐步非凸优化，避免一开始就被噪声/错误对应拖偏。

  4) 与其他模块的联系

   - src/tools/tools.h：参数定义与默认值（Data_nu, Data_initk, Data_endk, 自适应GNC阈值）。
   - src/main.cpp：运行时配置（use_Dynamic_nu, use_adaptive_gnc，以及 FASTRNRR_GNC_MODE）。
   - src/Registration.cpp：提供对应关系与 Data_nu 统计基线。
   - src/NonRigidreg.cpp：用 nu1/nu2 更新数据项/光滑项权重，构建线性系统并求解。
   - Anderson 联动：当 nu 改变时会重置 Anderson 历史，避免跨目标函数混用历史。

  5) 初值+迭代策略的优势

   - 中位数初始化稳健：对离群对应不敏感，比均值更可靠。
   - 尺度自适应：结合平均边长，跨模型尺度更稳定。
   - 先易后难：先大 nu 保证优化稳定，再减小 nu 提升抗离群能力与细节拟合。
   - 自适应调度更安全：通过梯度/Hessian代理控制衰减速度，减少“过早变尖锐”导致的震荡。

  如果你愿意，我可以下一步把你当前一次运行日志里的 policy_code 序列逐行解读，告诉你每个 stage 为什么“hold / fast / 
  fallback”。
