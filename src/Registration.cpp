/**
 * @file Registration.cpp
 * @brief 配准基类实现 - 刚性配准（ICP）、对应点管理、工具函数
 *
 * 【文件作用】
 *   实现 Registration 基类中声明的所有方法，包括：
 *   1. rigid_init()：初始化源/目标网格数据、构建 KD 树
 *   2. DoRigid()：执行基于 SVD 的刚性 ICP 配准迭代
 *   3. nonrigid_init()：为非刚性配准准备初始数据
 *   4. FindClosestPoints()：利用 KD 树进行最近点搜索
 *   5. SimplePruning()：对应点剪枝过滤不可靠的匹配
 *   6. point_to_point()：SVD 分解求解最优刚性变换
 *   7. CalcEdgelength()：计算网格边长统计
 *
 * 【与其他文件的关系】
 *   - 实现 Registration.h 中声明的类方法
 *   - 被 NonRigidreg.cpp 通过继承使用（NonRigidreg 调用 nonrigid_init, FindClosestPoints 等）
 *   - 依赖 median.h：中位数计算（用于鲁棒统计）
 *   - 依赖 tools/nanoflann.h：KD 树最近邻搜索
 *
 * 【刚性配准流程】
 *   rigid_init() → DoRigid() 中循环：
 *     FindClosestPoints() → point_to_point(SVD) → 更新网格 → SimplePruning()
 */

#include "Registration.h"
#include <median.h>


// make_unique 兼容性宏：C++14 以上使用标准库的 make_unique，否则使用自定义版本
#if (__cplusplus >= 201402L) || (defined(_MSC_VER) && _MSC_VER >= 1800)
#define MAKE_UNIQUE std::make_unique
#else
#define MAKE_UNIQUE company::make_unique
#endif


/**
 * @brief 构造函数：初始化所有指针和标志位
 */
Registration::Registration() {
    target_tree = NULL;             // KD 树指针初始为空
    src_mesh_ = NULL;               // 源网格指针初始为空
    tar_mesh_ = NULL;               // 目标网格指针初始为空
    use_cholesky_solver_ = true;    // 默认使用 Cholesky 分解
    use_pardiso_ = true;            // 默认使用 PARDISO 求解器
    update_tarGeotree = true;       // 默认更新目标测地树
};

/**
 * @brief 析构函数：释放 KD 树内存
 */
Registration::~Registration()
{
    if (target_tree != NULL)
    {
        delete target_tree;
        target_tree = NULL;
    }
}

/**
 * @brief 刚性配准初始化
 *
 * 在执行刚性/非刚性配准之前必须先调用此函数。它完成以下工作：
 * 1. 保存源网格和目标网格的指针
 * 2. 记录源/目标网格的顶点数量
 * 3. 将目标网格顶点坐标提取到 Eigen 矩阵 tar_points_ 中（3×m 矩阵）
 * 4. 用目标网格顶点构建 KD 树，用于后续的最近邻搜索
 * 5. 查找初始对应关系
 *
 * @param src_mesh 源网格引用
 * @param tar_mesh 目标网格引用
 * @param paras 配准参数
 */
void Registration::rigid_init(Mesh & src_mesh, Mesh & tar_mesh, RegParas& paras)
{
    src_mesh_ = new Mesh;
    tar_mesh_ = new Mesh;
    src_mesh_ = &src_mesh;      // 保存源网格指针
    tar_mesh_ = &tar_mesh;      // 保存目标网格指针
    pars_ = paras;              // 复制配准参数
    n_src_vertex_ = src_mesh_->n_vertices();  // 源网格顶点数
    n_tar_vertex_ = tar_mesh_->n_vertices();  // 目标网格顶点数
    corres_pair_ids_.resize(n_src_vertex_);   // 对应标记向量

    // 将目标网格顶点坐标提取到 3×m 的 Eigen 矩阵中
    // 每列存储一个顶点的 (x, y, z) 坐标
    tar_points_.resize(3, n_tar_vertex_);
    #pragma omp parallel for
    for (int i = 0; i < n_tar_vertex_; i++)
    {
        tar_points_(0, i) = tar_mesh_->point(tar_mesh_->vertex_handle(i))[0];
        tar_points_(1, i) = tar_mesh_->point(tar_mesh_->vertex_handle(i))[1];
        tar_points_(2, i) = tar_mesh_->point(tar_mesh_->vertex_handle(i))[2];
    }

    // 用目标网格顶点坐标构建 KD 树，加速后续的最近邻搜索
    target_tree = new KDtree(tar_points_);
    // 查找初始对应关系（最近点搜索 + 剪枝）
    InitCorrespondence(correspondence_pairs_);
}

/**
 * @brief 查找每个点的 K 近邻距离的中位数
 *
 * 对点集 X 中的每个点，使用 KD 树查找其 K 个最近邻点，
 * 取这些距离的中位数作为该点的特征距离，
 * 最后返回所有点特征距离的总体中位数。
 * 用于估计点云密度，辅助设定 Welsch 函数的鲁棒参数 nu。
 *
 * @param X 点集矩阵（3×n，每列一个点）
 * @param nk 近邻数量 K
 * @return 所有点 K 近邻距离中位数的总体中位数
 */
template<typename Derived1>
Scalar Registration::FindKnearestMed(Eigen::MatrixBase<Derived1>& X, int nk)
{
    // 构建 KD 树用于自身的近邻搜索
    nanoflann::KDTreeAdaptor<Eigen::MatrixBase<Derived1>, 3, nanoflann::metric_L2_Simple> kdtree(X);
    VectorX X_nearest(X.cols());
#pragma omp parallel for
    for (int i = 0; i<X.cols(); i++)
    {
        int* id = new int[nk];           // 近邻索引
        Scalar *dist = new Scalar[nk];   // 近邻距离
        kdtree.query(X.col(i).data(), nk, id, dist);  // 查询 K 个最近邻
        VectorX k_dist = Eigen::Map<VectorX>(dist, nk);
        // 取第 2~K 个近邻距离的中位数（排除自身，距离为0）
        igl::median(k_dist.tail(nk - 1), X_nearest[i]);
        delete[]id;
        delete[]dist;
    }
    Scalar med;
    igl::median(X_nearest, med);  // 取所有点的中位数
    return med;
}


/**
 * @brief 非刚性配准数据初始化
 *
 * 在刚性配准完成后、非刚性配准开始前调用。它完成以下工作：
 * 1. 分配对应点目标位置矩阵 mat_U0_ 和权重向量
 * 2. 查找初始对应关系并计算 Welsch 鲁棒参数 Data_nu 的初始值
 *    Data_nu 设为源网格顶点到其最近目标点距离的中位数
 * 3. 如果源目标顶点数相同，计算初始真值误差（用于评估精度）
 */
void Registration::nonrigid_init()
{
    mat_U0_.resize(3, n_src_vertex_);  // 3×n 矩阵：存储每个源顶点对应的目标位置

    // 初始化 Welsch 权重向量
    weight_d_.resize(n_src_vertex_);              // 数据项权重：每个顶点一个
    weight_s_.resize(src_mesh_->n_halfedges());   // 光滑项权重：每条半边一个

    // 查找初始对应关系（最近点搜索 + 剪枝 / 标记点对应）
    InitCorrespondence(correspondence_pairs_);

    // 计算 Welsch 鲁棒核函数的初始参数 Data_nu
    // Data_nu = 初始对应点对距离的中位数，反映了当前匹配的典型偏差
    VectorX init_nus(correspondence_pairs_.size());
    for(size_t i = 0; i < correspondence_pairs_.size(); i++)
    {
        Vector3 closet = correspondence_pairs_[i].position;
        init_nus[i] = (src_mesh_->point(src_mesh_->vertex_handle(correspondence_pairs_[i].src_idx))
                    - Vec3(closet[0], closet[1], closet[2])).norm();
    }
    igl::median(init_nus, pars_.Data_nu);

    // 如果源目标顶点数相同且需要计算真值误差，则计算初始对齐误差
    if(pars_.calc_gt_err&&n_src_vertex_ == n_tar_vertex_)
    {
        VectorX gt_err(n_src_vertex_);
        for(int i = 0; i < n_src_vertex_; i++)
        {
            gt_err[i] = (src_mesh_->point(src_mesh_->vertex_handle(i)) - tar_mesh_->point(tar_mesh_->vertex_handle(i))).norm();
        }
        pars_.init_gt_mean_errs = std::sqrt(gt_err.squaredNorm()/n_src_vertex_);  // RMS 误差
        pars_.init_gt_max_errs = gt_err.maxCoeff();  // 最大误差
    }
}

/**
 * @brief 执行刚性 ICP 配准
 *
 * 使用迭代最近点（ICP）算法将源网格刚性对齐到目标网格。
 * 每次迭代：
 *   1. 从对应点对中提取源点和目标点坐标
 *   2. 调用 point_to_point() 使用 SVD 分解计算最优旋转+平移变换
 *   3. 将变换应用到源网格的所有顶点
 *   4. 重新查找最近对应点并剪枝
 *   5. 如果变换矩阵变化小于阈值则提前终止
 *
 * @return 配准误差（当前返回0）
 */
Scalar Registration::DoRigid()
{
	Matrix3X rig_tar_v = Matrix3X::Zero(3, n_src_vertex_); // 目标点坐标矩阵 3×n
	Matrix3X rig_src_v = Matrix3X::Zero(3, n_src_vertex_); // 源点坐标矩阵 3×n
	Affine3 old_T;  // 上一次迭代的变换矩阵，用于判断收敛

	corres_pair_ids_.setZero();
	for(int iter = 0; iter < pars_.rigid_iters; iter++)
	{
		// 从对应点对中提取源点和目标点的坐标
		for (size_t i = 0; i < correspondence_pairs_.size(); i++)
		{
			rig_src_v.col(i) = Eigen::Map<Vector3>(src_mesh_->point(src_mesh_->vertex_handle(correspondence_pairs_[i].src_idx)).data(), 3, 1);
			rig_tar_v.col(i) = correspondence_pairs_[i].position;
			corres_pair_ids_[correspondence_pairs_[i].src_idx] = 1;  // 标记该顶点有对应
		}

		// 使用 SVD 分解计算最优刚性变换（旋转 + 平移）
		old_T = rigid_T_;
		rigid_T_ = point_to_point(rig_src_v, rig_tar_v, corres_pair_ids_);

		// 收敛判断：变换矩阵变化小于阈值则停止
		if((old_T.matrix() - rigid_T_.matrix()).norm() < 1e-3)
		{
			break;
		}

		// 将刚性变换应用到源网格的所有顶点
		#pragma omp parallel for
		for (int i = 0; i < n_src_vertex_; i++)
		{
			Vec3 p = src_mesh_->point(src_mesh_->vertex_handle(i));
			Vector3 temp = rigid_T_ * Eigen::Map<Vector3>(p.data(), 3);  // 应用仿射变换
			p[0] = temp[0];
			p[1] = temp[1];
			p[2] = temp[2];
			src_mesh_->set_point(src_mesh_->vertex_handle(i), p);
		}

		// 重新查找最近对应点并进行剪枝
		FindClosestPoints(correspondence_pairs_);
		SimplePruning(correspondence_pairs_, pars_.use_distance_reject, pars_.use_normal_reject);
	}

	return 0;
}

/**
 * @brief 查找最近对应点
 *
 * 对源网格中的每个顶点，使用 KD 树在目标网格中查找最近的顶点，
 * 并记录目标点的位置和法向量。
 * 使用 OpenMP 并行加速。
 *
 * @param corres 输出的对应点对集合
 */
void Registration::FindClosestPoints(VPairs & corres)
{
    corres.resize(n_src_vertex_);

    #pragma omp parallel for
    for (int i = 0; i < n_src_vertex_; i++)
    {
        Scalar mini_dist;
        // 使用 KD 树查找目标网格中距离源顶点最近的点
        int idx = target_tree->closest(src_mesh_->point(src_mesh_->vertex_handle(i)).data(), mini_dist);
        Closest c;
        c.src_idx = i;                                                        // 源顶点索引
        c.position = tar_points_.col(idx);                                    // 最近目标点坐标
        c.normal = Vec2Eigen(tar_mesh_->normal(tar_mesh_->vertex_handle(idx))); // 最近目标点法向量
        corres[i] = c;
    }
}

/**
 * @brief 对应点简单剪枝
 *
 * 根据距离阈值和法向量夹角阈值，过滤掉不可靠的对应点对。
 * 剪枝条件：
 *   - 距离剪枝：源顶点到对应目标点的距离超过 distance_threshold
 *   - 法向量剪枝：源顶点和目标点的法向量夹角超过 normal_threshold
 *   - 固定顶点：标记为固定的顶点不参与配准
 *
 * @param corres 输入/输出的对应点对集合
 * @param use_distance 是否启用距离剪枝
 * @param use_normal 是否启用法向量剪枝
 */
void Registration::SimplePruning(VPairs & corres, bool use_distance = true, bool use_normal = true)
{
    VectorX tar_min_dists(n_tar_vertex_);    // 每个目标顶点的最小距离
    tar_min_dists.setConstant(1e10);
    Eigen::VectorXi min_idxs(n_tar_vertex_); // 最小距离对应的源顶点索引
    min_idxs.setConstant(-1);
    src_mesh_->update_vertex_normals();       // 更新源网格的顶点法向量

    // 遍历所有对应点对，根据距离和法向量条件标记是否保留
    VectorX corres_idx = VectorX::Zero(n_src_vertex_);
    for(size_t i = 0; i < corres.size(); i++)
    {
        Vector3 closet = corres[i].position;
        // 计算源顶点到对应目标点的距离
        Scalar dist = (src_mesh_->point(src_mesh_->vertex_handle(corres[i].src_idx))
                       - Eigen2Vec(closet)).norm();
        // 获取源顶点和目标点的法向量
        Vec3 src_normal = src_mesh_->normal(src_mesh_->vertex_handle(corres[i].src_idx));
        Vec3 tar_normal = Eigen2Vec(corres[i].normal);

        // 计算法向量夹角
        Scalar angle = acos(src_normal | tar_normal / (src_normal.norm()*tar_normal.norm()));

        // 满足距离和法向量条件的对应点标记为有效
        if((!use_distance || dist < pars_.distance_threshold)
            && (!use_normal || src_mesh_->n_faces() == 0 || angle < pars_.normal_threshold))
        {
            corres_idx[i] = 1;
        }
    }

    // 排除固定顶点（这些顶点不参与配准变形）
    if(pars_.use_fixedvex)
    {
        for(size_t i = 0; i < pars_.fixed_vertices.size(); i++)
        {
            int idx = pars_.fixed_vertices[i];
            corres_idx[idx] = 0;
        }
    }

    // 只保留标记为有效的对应点对
    VPairs corres2;
    for (auto it = corres.begin(); it != corres.end(); it++)
    {
        if (corres_idx[(*it).src_idx] == 1)
        {
            corres2.push_back(*it);
        }
    }
    corres.clear();
    corres = corres2;
}


/**
 * @brief 设置标记点对应关系
 *
 * 使用预定义的标记点对（landmark_src, landmark_tar）建立对应关系。
 * 标记点是用户手动指定的源网格和目标网格上已知对应的顶点。
 * 这种强约束可以引导配准结果，特别适用于大变形场景。
 *
 * @param corres 输出的对应点对集合
 */
void Registration::LandMarkCorres(VPairs & corres)
{
	corres.clear();
	if (pars_.landmark_src.size() != pars_.landmark_tar.size())
	{
		std::cout << "Error: landmark data wrong!!" << std::endl;
	}
	n_landmark_nodes_ = pars_.landmark_tar.size();
	for (int i = 0; i < n_landmark_nodes_; i++)
	{
		Closest c;
		c.src_idx = pars_.landmark_src[i];  // 源网格标记点顶点索引
		OpenMesh::VertexHandle vh = tar_mesh_->vertex_handle(pars_.landmark_tar[i]);

		// 边界检查
		if (c.src_idx > n_src_vertex_ || c.src_idx < 0)
			std::cout << "Error: source index in Landmark is out of range!" << std::endl;
		if (vh.idx() < 0)
			std::cout << "Error: target index in Landmark is out of range!" << std::endl;

		c.position = Vec2Eigen(tar_mesh_->point(vh));   // 目标标记点坐标
		c.normal = Vec2Eigen(tar_mesh_->normal(vh));     // 目标标记点法向量
		corres.push_back(c);
	}
	std::cout << " use landmark and landmark is ... " << pars_.landmark_src.size() << std::endl;
}

/**
 * @brief 初始化对应关系
 *
 * 根据配置决定使用哪种方式建立初始对应关系：
 * - 无标记点：使用 FindClosestPoints + SimplePruning
 * - 有标记点：直接使用用户指定的标记点对
 *
 * @param corres 输出的对应点对集合
 */
void Registration::InitCorrespondence(VPairs & corres)
{
    // 先通过最近点搜索建立对应关系
    FindClosestPoints(corres);
    SimplePruning(corres);

    // 如果使用标记点，则覆盖为标记点对应关系
    if(pars_.use_landmark)
    {
        corres.clear();
        for(int i = 0; i < pars_.landmark_src.size(); i++)
        {
            Closest c;
            c.src_idx = pars_.landmark_src[i];
            c.tar_idx = pars_.landmark_tar[i];
            c.position = tar_points_.col(c.tar_idx);
            c.normal = Vec2Eigen(tar_mesh_->normal(tar_mesh_->vertex_handle(c.tar_idx)));
            corres.push_back(c);
        }
    }
}

/**
 * @brief 计算网格边长的统计值（中位数或平均值）
 *
 * 对于有面的网格（三角网格）：直接遍历所有边计算边长。
 * 对于纯点云（无面）：使用 KD 树查找 K=7 近邻的距离作为替代。
 * 返回的边长统计值用于设置 Welsch 函数参数 nu 的终止值。
 *
 * @param mesh 输入网格
 * @param type 统计类型：0=中位数（更鲁棒），1=平均值
 * @return 边长的中位数或平均值
 */
Scalar Registration::CalcEdgelength(Mesh* mesh, int type)
{
    Scalar med;
    if(mesh->n_faces() > 0)
    {
        VectorX edges_length(mesh->n_edges());
        for(size_t i = 0; i < mesh->n_edges();i++)
        {
            OpenMesh::VertexHandle vi = mesh->from_vertex_handle(mesh->halfedge_handle(mesh->edge_handle(i),0));
            OpenMesh::VertexHandle vj = mesh->to_vertex_handle(mesh->halfedge_handle(mesh->edge_handle(i),0));
            edges_length[i] = (mesh->point(vi) - mesh->point(vj)).norm();
        }
        if (type == 0)
            igl::median(edges_length, med);
        else
            med = edges_length.mean();
    }
    else
    {
        // source is mesh, target may be point cloud.
        VectorX edges_length(mesh->n_vertices());
        int nk = 7;
        for(size_t i = 0; i<mesh->n_vertices(); i++)
        {
            int* id = new int[nk];
            Scalar *dist = new Scalar[nk];
            target_tree->query(mesh->point(mesh->vertex_handle(i)).data(), nk, id, dist);
            VectorX k_dist = Eigen::Map<VectorX>(dist, nk);
            if (type == 0)
                igl::median(k_dist.tail(nk - 1), edges_length[i]);
            else
                edges_length[i] = k_dist.tail(nk - 1).mean();
            delete[]id;
            delete[]dist;
        }
        if (type == 0)
            igl::median(edges_length, med);
        else
            med = edges_length.mean();
    }
    return med;
}

/**
 * @brief 基于 SVD 的加权点对点刚性配准
 *
 * 求解最优旋转矩阵 R 和平移向量 t，使得加权误差最小：
 *   min_{R,t} sum_i w_i * ||R * X_i + t - Y_i||^2
 *
 * 算法步骤：
 *   1. 对权重向量归一化
 *   2. 计算加权质心 X_mean 和 Y_mean，对点集去中心化
 *   3. 计算协方差矩阵 sigma = X * W * Y^T
 *   4. 对 sigma 进行 SVD 分解：sigma = U * S * V^T
 *   5. 最优旋转 R = V * U^T（如果行列式为负则修正反射）
 *   6. 最优平移 t = Y_mean - R * X_mean
 *
 * @param X 源点集（3×n 矩阵，每列一个点）
 * @param Y 目标点集（3×n 矩阵，每列一个点）
 * @param w 每个点对的置信度权重（n×1 向量）
 * @return 最优仿射变换矩阵（4×4，包含旋转和平移）
 */
template <typename Derived1, typename Derived2, typename Derived3>
Affine3 Registration::point_to_point(Eigen::MatrixBase<Derived1>& X,
    Eigen::MatrixBase<Derived2>& Y,
    const Eigen::MatrixBase<Derived3>& w) {
    // 第一步：归一化权重，使权重之和为1
    VectorX w_normalized = w / w.sum();

    // 第二步：计算加权质心
    Vector3 X_mean, Y_mean;
    for (int i = 0; i<3; ++i) {
        X_mean(i) = (X.row(i).array()*w_normalized.transpose().array()).sum();
        Y_mean(i) = (Y.row(i).array()*w_normalized.transpose().array()).sum();
    }
    // 去中心化：减去质心
    X.colwise() -= X_mean;
    Y.colwise() -= Y_mean;

    // 第三步：计算 3×3 协方差矩阵并进行 SVD 分解
    Affine3 transformation;
    Matrix33 sigma = X * w_normalized.asDiagonal() * Y.transpose();
    Eigen::JacobiSVD<Matrix33> svd(sigma, Eigen::ComputeFullU | Eigen::ComputeFullV);

    // 第四步：从 SVD 结果构建旋转矩阵
    // 如果 det(U)*det(V) < 0，说明产生了反射，需要修正最后一个奇异值的符号
    if (svd.matrixU().determinant()*svd.matrixV().determinant() < 0.0) {
        Vector3 S = Vector3::Ones(); S(2) = -1.0;
        transformation.linear().noalias() = svd.matrixV()*S.asDiagonal()*svd.matrixU().transpose();
    }
    else {
        transformation.linear().noalias() = svd.matrixV()*svd.matrixU().transpose();
    }
    // 第五步：计算平移向量
    transformation.translation().noalias() = Y_mean - transformation.linear()*X_mean;

    // 恢复原始点集坐标（加回质心）
    X.colwise() += X_mean;
    Y.colwise() += Y_mean;

    return transformation;
}
