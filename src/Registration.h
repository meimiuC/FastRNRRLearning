/**
 * @file Registration.h
 * @brief 配准基类 - 提供刚性配准和对应点管理等基础功能
 *
 * 【文件作用】
 *   定义了 Registration 基类，它是整个配准算法的基础框架，负责：
 *   1. 管理源网格和目标网格的数据
 *   2. 提供基于 SVD 的点对点刚性配准（ICP）
 *   3. 利用 KD 树进行最近点查找，建立对应关系
 *   4. 对应点剪枝（基于距离和法向量过滤错误匹配）
 *   5. 标记点（landmark）约束管理
 *   6. 为非刚性配准子类提供公共数据成员和接口
 *
 * 【与其他文件的关系】
 *   - 被 NonRigidreg 类继承：NonRigidreg 重写 DoNonRigid() 和 Initialize() 实现非刚性配准
 *   - 依赖 tools/nanoflann.h：KD 树实现，用于快速最近邻搜索
 *   - 依赖 tools/tools.h：RegParas 参数结构体、工具函数
 *   - 依赖 tools/Types.h：Eigen 矩阵类型定义、OpenMesh 网格类型
 *   - 依赖 tools/OmpHelper.h：OpenMP 并行计算和计时器
 *   - 被 main.cpp 通过 NonRigidreg 间接使用
 *
 * 【类层次结构】
 *   Registration (基类)
 *       └── NonRigidreg (子类，在 NonRigidreg.h 中定义)
 */

#ifndef REGISTRATION_H_
#define REGISTRATION_H_
#include "tools/nanoflann.h"
#include "tools/tools.h"
#include "tools/Types.h"
#include "tools/OmpHelper.h"

/**
 * @class Registration
 * @brief 配准基类，封装刚性配准、对应点管理等公共功能
 *
 * 该类提供了配准算法的基础框架：
 * - rigid_init() + DoRigid()：执行刚性 ICP 配准
 * - FindClosestPoints()：利用 KD 树查找对应点
 * - SimplePruning()：对应点剪枝过滤
 * - nonrigid_init()：非刚性配准的数据初始化
 * - 虚函数 DoNonRigid() 和 Initialize() 由子类实现
 */
class Registration
{
public:
    Registration();
    virtual ~Registration();

    // ===== 网格数据 =====
    Mesh* src_mesh_;        // 源网格指针（待变形的网格）
    Mesh* tar_mesh_;        // 目标网格指针（目标形状）
    int n_src_vertex_;      // 源网格顶点数量
    int n_tar_vertex_;      // 目标网格顶点数量
    int n_landmark_nodes_;  // 标记点数量

    /**
     * @struct Closest
     * @brief 对应点对结构体，存储源网格顶点到目标网格最近点的映射信息
     */
    struct Closest{
        int src_idx;        // 源网格中的顶点索引
        int tar_idx;        // 目标网格中对应的面/顶点索引
        Vector3 position;   // 目标网格上最近点的三维坐标
        Vector3 normal;     // 目标网格上最近点的法向量
    };
    typedef std::vector<Closest> VPairs;  // 对应点对集合

protected:
    // ===== 非刚性配准能量函数相关变量 =====
    VectorX weight_d_;	 // 数据项权重向量：n×1 对角矩阵 diag(w1, w2, ... wn)，每个源顶点的匹配可信度
    VectorX weight_s_;	 // 光滑项权重向量：|e|×1 对角矩阵，每条边的光滑约束权重
    VectorX weight_4o_;	 // 正交项权重向量（旋转约束）

    MatrixXX grad_X_;	 // 能量函数关于变换矩阵 X 的梯度；维度 4r×3（r为采样节点数）
    Eigen::SparseMatrix<Scalar> mat_A0_;	// Hessian 矩阵近似；维度 4r×4r，用于 L-BFGS 的初始近似
    MatrixXX direction_;  // 下降方向；维度 4r×3
    MatrixXX tar_points_; // 目标网格顶点坐标矩阵；3×m（m为目标顶点数，每列一个点）
    MatrixXX mat_VU_;     // 辅助矩阵 U'V；维度 4r×3，用于构建线性方程组右端
    MatrixXX mat_U0_;     // 对应点目标位置矩阵；3×n（n为源顶点数）

    KDtree* target_tree;  // 目标网格的 KD 树，用于快速最近邻搜索

    // ===== 刚性配准参数 =====
    Affine3 rigid_T_;  // 刚性配准变换矩阵（4×4 仿射变换：旋转 + 平移）

    // ===== 对应点管理 =====
    VectorX corres_pair_ids_;                  // 标记向量：标记每个源顶点是否有有效对应点
    VPairs correspondence_pairs_;              // 当前迭代的对应点对集合
    Eigen::SparseMatrix<Scalar> sub_V_;        // 子集选择矩阵
    MatrixXX sub_U_;                           // 子集目标矩阵
    int current_n_;                            // 当前有效对应点数量

    // ===== Welsch 鲁棒函数动态参数 =====
    bool init_nu;       // nu 参数是否已初始化
    Scalar end_nu;      // nu 参数的终止值
    Scalar nu;          // 当前 nu 参数值

    bool update_tarGeotree;  // 是否需要更新目标网格的测地距离树

public:
    // ===== 可调节参数 =====
    bool use_cholesky_solver_;  // 是否使用 Cholesky 分解求解器
    bool use_pardiso_;          // 是否使用 PARDISO 并行稀疏求解器
    RegParas pars_;             // 配准算法参数结构体（定义在 tools/tools.h 中）

public:
    /**
     * @brief 非刚性配准数据初始化
     * 初始化对应点查找、Welsch 鲁棒参数 nu、初始误差统计等
     */
    void nonrigid_init();

    /**
     * @brief 执行非刚性配准（虚函数，由子类 NonRigidreg 重写实现）
     * @return 配准误差
     */
    virtual Scalar DoNonRigid() { return 0.0; }

    /**
     * @brief 执行刚性配准（ICP）
     * 迭代执行：查找对应点 → SVD求解最优刚性变换 → 更新源网格 → 剪枝
     * @return 配准误差
     */
    Scalar DoRigid();

    /**
     * @brief 刚性配准初始化
     * 分配内存、构建 KD 树、查找初始对应关系
     * @param src_mesh 源网格
     * @param tar_mesh 目标网格
     * @param paras 配准参数
     */
    void rigid_init(Mesh& src_mesh, Mesh& tar_mesh, RegParas& paras);

    /**
     * @brief 非刚性配准初始化（虚函数，由子类重写）
     * 在子类 NonRigidreg 中实现：采样控制节点、构建权重矩阵等
     */
    virtual void Initialize(){}

private:
    Eigen::VectorXi  init_geo_pairs;  // 初始测地距离对应关系

protected:
    /**
     * @brief 基于 SVD 的点对点刚性配准
     * 使用加权 SVD 分解计算最优旋转和平移，使得 ||R*X + t - Y||^2 最小
     * @param X 源点集（3×n，每列一个点）
     * @param Y 目标点集（3×n，每列一个点）
     * @param w 每个点对的置信度权重
     * @return 最优仿射变换矩阵（旋转+平移）
     */
    template <typename Derived1, typename Derived2, typename Derived3>
    Affine3 point_to_point(Eigen::MatrixBase<Derived1>& X,
        Eigen::MatrixBase<Derived2>& Y, const Eigen::MatrixBase<Derived3>& w);

    /**
     * @brief 初始化对应关系
     * 调用 FindClosestPoints + SimplePruning，或使用标记点对应
     */
    void InitCorrespondence(VPairs & corres);

    /**
     * @brief 查找最近对应点
     * 对每个源顶点，使用 KD 树在目标网格中查找最近点
     */
    void FindClosestPoints(VPairs & corres);

    /**
     * @brief 对应点简单剪枝
     * 基于距离阈值和法向量夹角阈值剔除不可靠的对应点对
     * @param use_distance 是否使用距离剪枝
     * @param use_normal 是否使用法向量剪枝
     */
    void SimplePruning(VPairs & corres, bool use_distance, bool use_normal);

    /**
     * @brief 设置标记点对应关系
     * 用预定义的标记点对替代最近点搜索得到的对应关系
     */
    void LandMarkCorres(VPairs & correspondence_pairs);

    /**
     * @brief 计算网格的边长统计值
     * @param mesh 输入网格
     * @param type 0=中位数, 1=平均值
     * @return 边长的中位数或平均值
     */
    Scalar CalcEdgelength(Mesh* mesh, int type);

    /**
     * @brief 查找 K 近邻距离的中位数
     * 用于估计点云密度，辅助设置 Welsch 函数的鲁棒参数
     */
    template<typename Derived1>
    Scalar FindKnearestMed(Eigen::MatrixBase<Derived1>& X, int nk);
};
#endif
