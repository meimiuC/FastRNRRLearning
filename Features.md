# L-BFGS
L-BFGS是有限内存的拟牛顿算法
## 牛顿法
一阶导数    梯度    在最陡峭的地方下降
二阶导数Hessian矩阵 可以找到最佳的方向

- 缺点：
    N个变量，Hessian矩阵NxN，计算复杂
- L-BFGS

# Anderson
## 基础定义
求解不动点：算法收敛时，状态不再改变，即达到了不动点 (Fixed-Point)：$x^* = G(x^*)$

定义残差：$$f_k = G(x_k) - x_k$$
残差为零，则找到了不动点

## 为什么要求解不动点
优化算法的目标是，求解函数的极小值点$\min E(x)$
- 问题：计算复杂，离散
对于Anderson，现在有$G$，$G$进行
如果当前的变量$x_{k}$在$G(x_{k})$得到$x_{k+1} = G(x_k)$
则说明找到了不动点
### 例子
对于函数$x=cos(x)$
1. 普通做法：
    随便猜一个初始值，比如 $x_0 = 0.5$。$x_1 = \cos(0.5) \approx 0.877$$x_2 = \cos(0.877) \approx 0.639$$x_3 = \cos(0.639) \approx 0.802$$x_4 = \cos(0.802) \approx 0.695$...（你会发现它在 0.739 左右来回震荡，收敛非常缓慢）。
2. Anderson：
    记录：$x_1, x_2, x_3, x_4$

    算出残差：$f_1 = x_2 - x_1$, $f_2 = x_3 - x_2$, $f_3 = x_4 - x_3$

    解残差的最小二乘问题，例如：如果把 $x_3$ 赋予 60% 的权重，把 $x_4$ 赋予 40% 的权重，它们产生的残差混合后最接近 0

    则下一次计算的起点改为：
    $x_{new} = 0.6 \times G(x_3) + 0.4 \times G(x_4)$
    $x_{new}$被用来计算$cos(x)$
### 在图形学中
对一个配准问题，目标是两个mesh的点的距离和最小
定义$G$：

    1. KD-Tree，找到目标点最近的点
    2. 计算变换矩阵
    3. 挪动mesh
$G$包括这三步



## 原理
已知求解了m步，利用过去的最近的 $m+1$ 个历史状态 $x_i$ 和对应的残差 $f_i$。
我们希望找到一组权重系数 $\alpha = [\alpha_0, \alpha_1, \dots, \alpha_m]^T$，
使得这些历史残差的加权和最小。为了保证它还是一个合法的点，权重之和必须为 1：
$$\sum_{i=0}^m \alpha_i = 1$$
最近的 $m+1$ 个历史状态 $x_i$ 和对应的残差 $f_i$。
我们希望找到一组权重系数 
$\alpha = [\alpha_0, \alpha_1, \dots, \alpha_m]^T$，使得这些历史残差的加权和最小。
为了保证它还是一个合法的点，权重之和必须为 1：
$\sum_{i=0}^m \alpha_i = 1$

## 求解过程
为了使得残差接近0（达到不动点），

## 优势
绕开了梯度计算

# 问题与不足
1. ICP刚性配准未能起到作用
这条代码链：
   1. RegParas 默认构造把 rigid_iters = 
  0（src/tools/tools.h:145，你的 Advancing...md 里也有 
  896 行）。
   2. main.cpp 里是 RegParas 
  paras;，后续参数设置没有覆盖 
  rigid_iters（src/main.cpp:42 + 参数区）。
   3. rigid_init() 把 paras 复制到成员 
  pars_（src/Registration.cpp:82）。
   4. DoRigid() 的循环条件是for (int iter = 0; iter < 
  pars_.rigid_iters; 
  iter++)（src/Registration.cpp:204），所以 
  rigid_iters=0 时循环体完全不进。
也就是说，在刚性初始化阶段，只做了KDTree和初始点的
对应，没有索SVD的刚体旋转
现在的刚体配准都是通过手动在Polyworks中进行的

