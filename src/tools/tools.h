/**
 * @file tools.h
 * @brief 工具函数和参数结构体定义
 *
 * 【文件作用】
 *   定义了配准算法的参数结构体 RegParas 和各种工具函数，包括：
 *   1. RegParas：配准算法的所有超参数和运行时记录
 *   2. mesh_scaling()：网格归一化缩放
 *   3. Mesh2VF()：OpenMesh 转 libigl 格式
 *   4. Eigen2Vec / Vec2Eigen：OpenMesh 和 Eigen 向量类型转换
 *   5. read_landmark / read_fixedvex：读取标记点和固定顶点文件
 *
 * 【与其他文件的关系】
 *   - 被 Registration.h 包含：使用 RegParas 结构体和工具函数
 *   - 被 main.cpp 间接使用：mesh_scaling, read_landmark 等
 *   - 依赖 Types.h：所有类型定义
 *
 * 【RegParas 结构体说明】
 *   RegParas 包含了配准算法的完整参数配置，分为：
 *   - 优化控制参数（迭代次数、收敛阈值）
 *   - 能量函数权重（alpha, beta, gamma）
 *   - 鲁棒函数参数（Welsch nu, 动态 nu 策略）
 *   - 对应点管理参数（距离/法向量剪枝阈值）
 *   - 采样参数（采样半径比率）
 *   - 运行时统计记录（每步能量、误差、耗时）
 */

#ifndef TOOL_H_
#define TOOL_H_
#include "Types.h"

// ===== 对应关系类型 =====
enum CorresType {CLOSEST, LANDMARK};  // 最近点 / 标记点

// ===== 剪枝类型 =====
enum PruningType {SIMPLE, NONE};      // 简单剪枝 / 不剪枝

/**
 * @struct RegParas
 * @brief 配准算法参数结构体，包含所有可调节的超参数和运行时统计
 */
struct RegParas
{
	// ===== 优化控制参数 =====
	int		max_outer_iters;    // 非刚性配准外层最大迭代次数
	int		max_inner_iters;    // 非刚性配准内层（L-BFGS）最大迭代次数

	// ===== 能量函数权重 =====
	Scalar	alpha;              // 光滑项权重：约束相邻控制节点变换的一致性
	Scalar	beta;               // 正交项权重：约束变换矩阵接近旋转矩阵
	Scalar  gamma;              // 标记点项权重：强制标记点对齐

	// ===== 优化方法选项 =====
	bool	use_lbfgs;          // 是否使用 L-BFGS 加速（true=更快收敛）
	int		lbfgs_m;            // L-BFGS 历史记录步数（通常 3~10）
	bool    use_anderson;      // 是否启用 Anderson 加速外层迭代
	int     anderson_m;        // Anderson 历史窗口大小
	bool    anderson_safeguard;// 是否启用 Anderson 安全回退
	double  anderson_safeguard_ratio; // Anderson 外推偏差阈值

	// ===== 对应点剪枝参数 =====
	bool	use_normal_reject;  // 是否使用法向量夹角剪枝
	bool	use_distance_reject;// 是否使用距离剪枝
	Scalar	normal_threshold;   // 法向量夹角阈值（弧度）
	Scalar	distance_threshold; // 距离阈值

	// ===== 刚性配准参数 =====
	int     rigid_iters;         // 刚性 ICP 最大迭代次数

	// ===== 约束选项 =====
	bool	use_landmark;        // 是否使用标记点约束
	bool    use_fixedvex;        // 是否使用固定顶点（不参与变形的顶点）
	bool    calc_gt_err;         // 是否计算真值误差（需要源目标顶点数相同）
	bool    data_use_welsch;     // 数据项是否使用 Welsch 鲁棒核（false=L2范数）
	bool    smooth_use_welsch;   // 光滑项是否使用 Welsch 鲁棒核（false=L2范数）

	// ===== 约束数据 =====
	std::vector<int> landmark_src;   // 源网格标记点顶点索引列表
	std::vector<int> landmark_tar;   // 目标网格标记点顶点索引列表（与 landmark_src 一一对应）
	std::vector<int> fixed_vertices; // 固定顶点索引列表

	// ===== Welsch 鲁棒函数动态参数 =====
	bool    use_Dynamic_nu;   // 是否使用动态 nu（GNC 策略，逐步减小 nu）
	Scalar  Data_nu;          // 数据项 Welsch 参数 nu 的基准值（初始对应点距离的中位数）
	Scalar  Smooth_nu;        // 光滑项 Welsch 参数的倍率
	Scalar  Data_initk;       // 数据项 nu 初始放大倍数（nu1_init = Data_initk × Data_nu）
	Scalar  Data_endk;        // 数据项 nu 终止比率（end_nu1 = Data_endk × 平均边长）
	Scalar  stop;             // 收敛阈值（顶点位移或能量变化小于此值则停止）

	// ===== 采样参数 =====
	Scalar  uni_sample_radio;       // 均匀采样半径比率：采样半径 = 平均边长 × 此值
	bool    print_each_step_info;   // 是否输出每步的调试信息

	// ===== 输出路径 =====
	std::string out_gt_file;          // 真值误差输出文件路径
	std::string out_each_step_info;   // 每步信息输出路径
	int         num_sample_nodes;     // 实际采样节点数量

	// ===== 运行时统计记录 =====
	std::vector<Scalar> each_times;          // 每步的累计耗时
	std::vector<Scalar> each_gt_mean_errs;   // 每步的平均真值误差
	std::vector<Scalar> each_gt_max_errs;    // 每步的最大真值误差
	std::vector<Scalar> each_energys;        // 每步的总能量
	std::vector<Scalar> each_iters;          // 每步的内层迭代次数
	std::vector<Vector3> each_term_energy;   // 每步的分项能量 (data, smooth, orth)
	Scalar  non_rigid_init_time;             // 非刚性初始化耗时
	Scalar  init_gt_mean_errs;               // 初始平均真值误差
	Scalar  init_gt_max_errs;                // 初始最大真值误差

	/**
	 * @brief 默认构造函数：设置所有参数的默认值
	 */
	RegParas()
	{
		max_outer_iters = 100;
		max_inner_iters = 1;
		alpha = 100.0;
		beta = 100.0;
		gamma = 1e6;
		use_lbfgs = false;
		lbfgs_m = 5;
		use_anderson = true;
		anderson_m = 5;
		anderson_safeguard = false;
		anderson_safeguard_ratio = 10.0;
		use_normal_reject = false;
		use_distance_reject = false;
		normal_threshold = M_PI / 3;
		distance_threshold = 0.3;
		rigid_iters = 0;
		use_landmark = false;
		use_fixedvex = false;
		calc_gt_err = false;
		data_use_welsch = true;
		smooth_use_welsch = true;

		use_Dynamic_nu = true;
		Data_nu = 0.0;
		Smooth_nu = 40;
		Data_initk =10;
		Data_endk = 0.5;
		stop = 1e-3;

		uni_sample_radio = 5;
		print_each_step_info = false;

		non_rigid_init_time = .0;
		init_gt_mean_errs = .0;
	}

};

// ===== 工具函数声明 =====

/**
 * @brief 归一化缩放：将源网格和目标网格的坐标除以包围盒对角线长度
 * @return 缩放因子（用于最终恢复原始尺度）
 */
Scalar mesh_scaling(Mesh& src_mesh, Mesh& tar_mesh);

/**
 * @brief 将 OpenMesh 网格转换为 libigl 格式（顶点矩阵 V + 面矩阵 F）
 */
void Mesh2VF(Mesh & mesh, MatrixXX& V, Eigen::MatrixXi& F);

/** @brief OpenMesh Vec3 ↔ Eigen Vector3 互相转换 */
Vec3 Eigen2Vec(Vector3 s);
Vector3 Vec2Eigen(Vec3 s);

/** @brief 从文件读取标记点对应关系 */
bool read_landmark(const char* filename, std::vector<int>& landmark_src, std::vector<int>& landmark_tar);

/** @brief 从文件读取固定顶点列表 */
bool read_fixedvex(const char* filename, std::vector<int>& vertices_list);

#ifdef __linux__
/** @brief 创建目录（仅 Linux） */
bool my_mkdir(std::string file_path);
#endif

#endif
