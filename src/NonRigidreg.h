/**
 * @file NonRigidreg.h
 * @brief 非刚性配准子类 - 基于 Welsch 鲁棒核函数和 L-BFGS 优化的非刚性配准
 *
 * 【文件作用】
 *   定义 NonRigidreg 类，继承自 Registration 基类，实现非刚性配准的核心算法。
 *   该算法将源网格变形到目标网格，通过在稀疏控制节点上求解仿射变换来驱动变形。
 *
 * 【算法核心思想】
 *   1. 在源网格上基于测地距离均匀采样 r 个控制节点（nodeSampler）
 *   2. 每个控制节点有一个 4×3 的仿射变换矩阵（3×3 线性变换 + 1×3 平移）
 *   3. 通过最小化以下能量函数求解最优变换：
 *      E_total = E_data + α * E_smooth + β * E_orth + γ * E_landmark
 *      - E_data：数据项，使变形后的网格接近目标网格
 *      - E_smooth：光滑项，使相邻节点的变换保持一致
 *      - E_orth：正交项，约束变换矩阵接近旋转矩阵（防止剪切/缩放）
 *      - E_landmark：标记点项，强制标记点对齐（可选）
 *   4. 使用 Welsch 鲁棒核函数替代 L2 范数，提高对异常值的鲁棒性
 *   5. 使用 L-BFGS 拟牛顿法加速优化收敛
 *   6. 动态调整 nu 参数（graduated non-convexity），从粗到精求解
 *
 * 【与其他文件的关系】
 *   - 继承自 Registration（Registration.h）：复用刚性配准、对应点管理等功能
 *   - 依赖 tools/nodeSampler.h：控制节点采样和图结构构建
 *   - 被 main.cpp 创建和调用：main 中调用 Initialize() 和 DoNonRigid()
 *
 * 【矩阵符号说明】
 *   n = 源网格顶点数, r = 采样节点数, |e| = 节点图边数, k = 标记点数
 *   Smat_X_   (4r × 3)：所有控制节点的仿射变换矩阵（每个节点 4×3）
 *   Weight_PV_ (n × 4r)：顶点到控制节点的权重矩阵（稀疏）
 *   Smat_B_   (2|e| × 4r)：光滑项矩阵
 *   Smat_R_   (3r × 3)：旋转矩阵近似（通过 SVD 从 X 中提取）
 */

#ifndef QN_WELSCH_H_
#define QN_WELSCH_H_
#include "Registration.h"
#include "tools/nodeSampler.h"

typedef Eigen::SparseMatrix<Scalar> SparseMatrix;

/**
 * @class NonRigidreg
 * @brief 非刚性配准类，实现基于 Welsch + L-BFGS 的鲁棒非刚性配准算法
 *
 * 主要方法：
 * - Initialize()：初始化采样节点、权重矩阵、变换矩阵等
 * - DoNonRigid()：执行非刚性配准主循环（外层调整 nu + 内层 L-BFGS 优化）
 */
class NonRigidreg : public Registration
{
public:
	NonRigidreg();
	~NonRigidreg();

	/**
	 * @brief 执行非刚性配准主流程
	 * 外层循环：逐步减小 Welsch 函数参数 nu（graduated non-convexity）
	 * 内层循环：对每个 nu 值，迭代更新对应关系和变换矩阵
	 */
	virtual Scalar DoNonRigid();

	/**
	 * @brief 非刚性配准初始化
	 * 采样控制节点、构建节点图、初始化所有矩阵和变量
	 */
	virtual void Initialize();

private:
	// ===== Welsch 鲁棒核函数相关 =====
	/** @brief 计算 Welsch 鲁棒误差 */
	Scalar welsch_error(Scalar nu1, Scalar nu2);
	/** @brief 计算 Welsch 能量：E = sum(1 - exp(-r_i^2 / (2*p^2))) */
	Scalar welsch_energy(VectorX& r, Scalar p);
	/** @brief 计算 Welsch 权重：w_i = exp(-r_i^2 / (2*p^2)) */
	void welsch_weight(VectorX& r, Scalar p);

	// ===== L-BFGS 优化相关 =====
	/**
	 * @brief L-BFGS 两循环递归算法
	 * 利用历史梯度差和步长信息近似 Hessian 逆矩阵，计算下降方向
	 * @param iter 当前迭代次数
	 * @param dir 输出的下降方向
	 */
	void LBFGS(int iter, MatrixXX & dir) const;

	/**
	 * @brief 拟牛顿求解器（L-BFGS + 线搜索）
	 * 在固定 Welsch 权重下，使用 L-BFGS 方法求解最优变换矩阵
	 * @return 实际执行的迭代次数
	 */
	int QNSolver(Scalar& data_err, Scalar& smooth_err, Scalar& orth_err);

	// ===== 能量和梯度计算 =====
	/** @brief 计算当前能量值（数据项 + 光滑项 + 正交项），同时通过 SVD 更新旋转矩阵 */
	Scalar sample_energy(Scalar& data_err, Scalar& smooth_err, Scalar& orth_err);
	/** @brief 计算能量函数的梯度 */
	void   sample_gradient();
	/** @brief 通过 SVD 分解从仿射变换中提取最近旋转矩阵 */
	void   update_R();
	/** @brief 将变换结果应用到网格顶点，并计算真值误差 */
	Scalar SetMeshPoints(Mesh* mesh, const MatrixXX & target, MatrixXX& cur_v);

	/** @brief Cholesky 分解求解器，用于求解线性方程组 A*x = b */
	Eigen::SimplicialCholesky<SparseMatrix>* ldlt_;

private:
	// ===== L-BFGS 历史记录 =====
	MatrixXX all_s_; // si = X_{i+1} - X_i；维度 (12r) × lbfgs_m，存储最近 m 步的步长差
	MatrixXX all_t_; // ti = ∇E(X_{i+1}) - ∇E(X_i)；维度 (12r) × lbfgs_m，存储最近 m 步的梯度差
	int iter_;       // 全局迭代计数器
	int col_idx_;    // 环形缓冲区当前写入位置

	// ===== 采样节点相关 =====
	int				num_sample_nodes;  // (r) 采样控制节点数量
	svr::nodeSampler src_sample_nodes; // 采样节点管理器（负责采样、构建节点图、计算权重）

	// ===== 变换变量 =====
	MatrixXX		Smat_X_;	// (4r × 3) 所有控制节点的仿射变换矩阵
								//   每个节点占4行：前3行是3×3线性变换，第4行是1×3平移

	// ===== 数据项矩阵 =====
	//   E_data = ||Weight_PV_ * Smat_X_ + Smat_P_ - U||^2
	SparseMatrix	Weight_PV_; // (n × 4r) 顶点-控制节点权重矩阵（稀疏）
								//   将控制节点的变换插值到所有网格顶点
	MatrixXX		Smat_P_;    // (n × 3) 权重加权的控制节点坐标和

	// ===== 光滑项矩阵 =====
	//   E_smooth = α * ||Smat_B_ * Smat_X_ - Smat_D_||^2
	SparseMatrix	Smat_B_;	// (2|e| × 4r) 节点间光滑约束矩阵
								//   |e| 是控制节点图的边数
	MatrixXX		Smat_D_;	// (2|e| × 3) 相邻控制节点间的坐标差
	VectorX			Sweight_s_; // (2|e|) 光滑项权重（与节点间距离成反比）

	// ===== 正交项矩阵 =====
	//   E_orth = β * ||L * Smat_X_ - J * Smat_R_||^2
	MatrixXX		Smat_R_;	// (3r × 3) 每个控制节点的最近旋转矩阵
	SparseMatrix	Smat_L_;	// (4r × 4r) 正交项系数矩阵（提取线性变换部分）
	SparseMatrix	Smat_J_;	// (4r × 3r) 正交项辅助矩阵
	MatrixXX		Smat_UP_;   // 辅助矩阵：加权后的目标位移

	// ===== 标记点项矩阵 =====
	//   E_landmark = γ * ||Sub_PV_ * Smat_X_ - Sub_UP_||^2
	SparseMatrix    Sub_PV_;    // (k × 4r) 标记点的权重子矩阵
	MatrixXX        Sub_UP_;    // (k × 3) 标记点的目标位移

	// ===== 原始参数备份 =====
	Scalar          ori_alpha;  // 原始 alpha 值（在动态调整 nu 时需要恢复）
	Scalar          ori_beta;   // 原始 beta 值
};
#endif
