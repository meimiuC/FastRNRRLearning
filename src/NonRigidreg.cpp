/**
 * @file NonRigidreg.cpp
 * @brief 非刚性配准实现 - Welsch 鲁棒核函数 + L-BFGS 拟牛顿优化
 *
 * 【文件作用】
 *   实现 NonRigidreg 类的所有方法，这是整个配准算法的核心计算模块。
 *   主要包含：
 *   1. Initialize()：初始化采样节点、构建数据/光滑/正交项矩阵
 *   2. DoNonRigid()：非刚性配准主循环（外层 GNC + 内层 QN 优化）
 *   3. QNSolver()：L-BFGS 拟牛顿求解器（含线搜索）
 *   4. LBFGS()：L-BFGS 两循环递归算法
 *   5. welsch_weight/welsch_energy：Welsch 鲁棒核函数
 *   6. sample_energy/sample_gradient：能量和梯度计算
 *
 * 【与其他文件的关系】
 *   - 实现 NonRigidreg.h 中声明的类方法
 *   - 调用 Registration 基类方法：FindClosestPoints, SimplePruning, CalcEdgelength 等
 *   - 调用 nodeSampler：控制节点采样和权重初始化
 *
 * 【核心算法流程】
 *   DoNonRigid() 主循环：
 *   while (nu 未收敛):
 *     for 每个外层迭代:
 *       1. 更新对应关系 → 计算 Welsch 权重
 *       2. 更新加权矩阵 A0, VU
 *       3. 调用 QNSolver()（L-BFGS + 线搜索）优化变换矩阵 X
 *       4. 更新网格顶点位置
 *       5. 重新查找对应点并剪枝
 *       6. 检查收敛条件
 *     减小 nu1, nu2（从粗到精）
 */

#pragma once
#include "NonRigidreg.h"

typedef Eigen::Triplet<Scalar> Triplet;  // 稀疏矩阵三元组 (行, 列, 值)

NonRigidreg::NonRigidreg() {
};

NonRigidreg::~NonRigidreg()
{
}

/**
 * @brief 非刚性配准初始化
 *
 * 该函数是非刚性配准的核心初始化步骤，包括：
 * 1. 创建 Cholesky 分解求解器
 * 2. 调用基类的 nonrigid_init() 初始化对应点和 Welsch 参数
 * 3. 在源网格上基于测地距离进行均匀采样，生成控制节点图
 * 4. 初始化所有矩阵变量（变换矩阵 X、旋转矩阵 R、权重矩阵等）
 * 5. 通过 nodeSampler 计算数据项、光滑项的系数矩阵
 * 6. 如果有标记点，构建标记点约束矩阵
 * 7. 根据采样节点数和边数对权重参数 alpha, beta 进行归一化
 */
void NonRigidreg::Initialize()
{
    // 创建 Cholesky 分解求解器，用于求解 A*x=b 形式的线性方程组
    ldlt_ = new Eigen::SimplicialCholesky<SparseMatrix>;
    // 调用基类的非刚性初始化（初始化对应点、Welsch 参数等）
    nonrigid_init();
    // 初始化 L-BFGS 迭代计数器
    iter_ = 0;
    col_idx_ = 0;

    // 初始化权重向量为全1（所有点等权）
    weight_d_.setOnes();
    weight_s_.setOnes();
    Scalar sample_radius;

    // ===========================================================
    // 第一步：在源网格上采样控制节点并构建节点图
    // ===========================================================
    // sampleAndconstuct 使用测地距离进行均匀采样：
    //   - 采样半径 = 平均边长 × uni_sample_radio
    //   - 沿 X 轴排序顶点，贪心采样保证节点间距离不小于采样半径
    //   - 同时构建顶点-节点关联图和节点-节点邻接图
    Timer timer;
    Timer::EventID time1, time2;
    time1 = timer.get_time();
    sample_radius = src_sample_nodes.sampleAndconstuct(*src_mesh_, pars_.uni_sample_radio, svr::nodeSampler::X_AXIS);
    time2 = timer.get_time();

    // 调试输出：将采样节点和节点图保存为 OBJ 文件
    if(pars_.print_each_step_info)
    {
        std::string out_node = pars_.out_each_step_info + "_init.obj";
        src_sample_nodes.print_nodes(*src_mesh_, out_node);
    }

    num_sample_nodes = src_sample_nodes.nodeSize();  // 采样节点总数 r
    pars_.num_sample_nodes = num_sample_nodes;

    // ===========================================================
    // 第二步：初始化 L-BFGS 历史记录矩阵
    // ===========================================================
    // all_s_ 和 all_t_ 是环形缓冲区，存储最近 m 步的步长差和梯度差
    all_s_.resize(4 * num_sample_nodes * 3, pars_.lbfgs_m); all_s_.setZero();
    all_t_.resize(4 * num_sample_nodes * 3, pars_.lbfgs_m); all_t_.setZero();

    // ===========================================================
    // 第三步：初始化变换矩阵和系数矩阵
    // ===========================================================
    // Smat_X_ (4r×3)：每个控制节点的仿射变换矩阵，初始为单位变换
    Smat_X_.resize(4 * num_sample_nodes, 3); Smat_X_.setZero();
    // Weight_PV_ (n×4r)：顶点到控制节点的插值权重矩阵
    Weight_PV_.resize(n_src_vertex_, 4 * num_sample_nodes);
    // Smat_P_ (n×3)：权重加权的控制节点坐标和
    Smat_P_.resize(n_src_vertex_, 3);

    // Smat_R_ (3r×3)：每个控制节点的最近旋转矩阵，初始为单位矩阵
    Smat_R_.resize(3 * num_sample_nodes, 3); Smat_R_.setZero();
    // Smat_L_ (4r×4r)：正交项系数矩阵，提取线性变换部分
    Smat_L_.resize(4 * num_sample_nodes, 4 * num_sample_nodes);
    // Smat_J_ (4r×3r)：正交项辅助矩阵
    Smat_J_.resize(4 * num_sample_nodes, 3 * num_sample_nodes);

    // 构建稀疏矩阵 Smat_L_ 和 Smat_J_
    // Smat_L_ 的对角块：每个节点的前3行为 1（提取线性变换部分），第4行为 0
    // Smat_J_ 将 4r×3 矩阵映射到 3r×3 旋转矩阵空间
    std::vector<Triplet> coeffv(4 * num_sample_nodes);
    std::vector<Triplet> coeffL(3 * num_sample_nodes);
    std::vector<Triplet> coeffJ(3 * num_sample_nodes);
    for (int i = 0; i < num_sample_nodes; i++)
    {
        // 初始化 Smat_X_ 为单位变换（前3行为 I_3，第4行为 0）
        Smat_X_.block(4 * i, 0, 3, 3) = MatrixXX::Identity(3, 3);

        // 初始化 Smat_R_ 为单位旋转矩阵
        Smat_R_.block(3 * i, 0, 3, 3) = MatrixXX::Identity(3, 3);

        // 构建 Smat_L_ 的稀疏三元组
        coeffL[3 * i] = Triplet(4 * i, 4 * i, 1.0);
        coeffL[3 * i + 1] = Triplet(4 * i + 1, 4 * i + 1, 1.0);
        coeffL[3 * i + 2] = Triplet(4 * i + 2, 4 * i + 2, 1.0);

        // 构建 Smat_J_ 的稀疏三元组
        coeffJ[3 * i] = Triplet(4 * i, 3 * i, 1.0);
        coeffJ[3 * i + 1] = Triplet(4 * i + 1, 3 * i + 1, 1.0);
        coeffJ[3 * i + 2] = Triplet(4 * i + 2, 3 * i + 2, 1.0);

    }
    Smat_L_.setFromTriplets(coeffL.begin(), coeffL.end());
    Smat_J_.setFromTriplets(coeffJ.begin(), coeffJ.end());
    direction_.resize(4*num_sample_nodes, 3);  // 下降方向矩阵

    // ===========================================================
    // 第四步：通过 nodeSampler 初始化数据项和光滑项的系数矩阵
    // ===========================================================
    // initWeight 计算：
    //   Weight_PV_：顶点-节点插值权重矩阵
    //   Smat_P_：权重加权的节点坐标
    //   Smat_B_：节点间光滑约束矩阵
    //   Smat_D_：节点间坐标差
    //   Sweight_s_：光滑权重
    src_sample_nodes.initWeight(Weight_PV_, Smat_P_, Smat_B_, Smat_D_, Sweight_s_);

    // ===========================================================
    // 第五步：如果有标记点，构建标记点约束矩阵
    // ===========================================================
    if(pars_.use_landmark && pars_.landmark_src.size() > 0)
    {
        size_t n_landmarks = pars_.landmark_src.size();
        Sub_PV_.resize(n_landmarks, 4*num_sample_nodes);  // 标记点权重子矩阵
        Sub_UP_.resize(n_landmarks, 3);                    // 标记点目标位移
        if(pars_.landmark_tar.size() != n_landmarks)
        {
            std::cout << "Error: The source and target points do not match!" << std::endl;
            exit(1);
        }
        // 对每个标记点，提取其在 Weight_PV_ 中的行作为 Sub_PV_ 的行
        for(size_t i = 0; i < n_landmarks; i++)
        {
            size_t src_idx = pars_.landmark_src[i];
            size_t tar_idx = pars_.landmark_tar[i];
            VectorX row_val = Weight_PV_.row(src_idx);
            for(int j = 0; j < 4*num_sample_nodes; j++) {
                if(fabs(row_val[j])>1e-12) {
                    Sub_PV_.insert(i, j) =  row_val[j];
                }
            }
            // 标记点的目标位移 = 目标点坐标 - 源点在控制节点下的参考坐标
            Sub_UP_.row(i) = tar_points_.col(tar_idx).transpose() - Smat_P_.row(src_idx);
        }
        // 根据标记点数量对 gamma 进行归一化
        pars_.gamma = pars_.gamma * n_landmarks / n_src_vertex_;
    }

    // 根据节点图边数和节点数对 alpha, beta 进行归一化
    // 使得不同规模的网格使用相同的参数时能获得相似的效果
    pars_.alpha = pars_.alpha * Smat_B_.rows()/ n_src_vertex_;  // 光滑项权重归一化
    pars_.beta = pars_.beta *  num_sample_nodes / n_src_vertex_; // 正交项权重归一化
}

/**
 * @brief 执行非刚性配准主流程
 *
 * 采用 Graduated Non-Convexity (GNC) 策略：
 * - 外层循环：逐步减小 Welsch 函数参数 nu1, nu2，从粗到精求解
 *   - nu1 控制数据项的鲁棒性（大nu→接近L2，小nu→更鲁棒）
 *   - nu2 控制光滑项的鲁棒性
 * - 内层循环：对每个固定的 nu，迭代执行：
 *   1. 更新对应关系和 Welsch 权重
 *   2. 使用 L-BFGS 优化变换矩阵
 *   3. 更新网格顶点位置
 *   4. 重新查找对应点
 *   5. 检查收敛条件
 *
 * @return 配准误差
 */
Scalar NonRigidreg::DoNonRigid()
{
    Scalar data_err, smooth_err, orth_err;

    // 当前和前一次迭代的顶点位置，用于判断收敛
    MatrixXX curV =  MatrixXX::Zero(n_src_vertex_, 3);
    MatrixXX prevV = MatrixXX::Zero(n_src_vertex_, 3);

    // 保存原始矩阵，因为每次迭代会对它们重新加权
    SparseMatrix Weight_PV0 = Weight_PV_;  // 原始数据项权重矩阵
    SparseMatrix Smat_B0 = Smat_B_;        // 原始光滑项矩阵
    MatrixXX	 Smat_D0 = Smat_D_;          // 原始光滑项坐标差
    // Welsch 光滑权重
    VectorX welsch_weight_s = VectorX::Ones(Sweight_s_.rows(), 1);
    bool run_once = true;  // Cholesky 符号分析只需做一次

    Timer time;
    Timer::EventID begin_time, run_time;
    // 清空每步统计信息
    pars_.each_energys.clear();
    pars_.each_gt_mean_errs.clear();
    pars_.each_gt_max_errs.clear();
    pars_.each_times.clear();
    pars_.each_iters.clear();
    pars_.each_term_energy.clear();

    // 记录初始状态的统计信息
    pars_.each_energys.push_back(0.0);
    pars_.each_gt_max_errs.push_back(pars_.init_gt_max_errs);
    pars_.each_gt_mean_errs.push_back(pars_.init_gt_mean_errs);
    pars_.each_iters.push_back(0);
    pars_.each_times.push_back(pars_.non_rigid_init_time);
    pars_.each_term_energy.push_back(Vector3(0,0,0));

    // ===========================================================
    // 初始化 Welsch 参数
    // ===========================================================
    // nu1：数据项的 Welsch 参数，初始值 = Data_initk × Data_nu
    //   Data_nu 是初始对应点距离的中位数，Data_initk 是放大倍数
    Scalar nu1 = pars_.Data_initk * pars_.Data_nu;
    // 计算网格平均边长，用于设置 nu 的终止值
    Scalar average_len = CalcEdgelength(src_mesh_, 1);
    // end_nu1：nu1 的终止值 = Data_endk × 平均边长
    Scalar end_nu1 = pars_.Data_endk * average_len;
    // nu2：光滑项的 Welsch 参数
    Scalar nu2 = pars_.Smooth_nu * average_len;

    // 根据当前 nu 值调整光滑项和正交项权重
    // 当 nu1 减小时，alpha 和 beta 也相应调整
    ori_alpha = pars_.alpha;
    ori_beta = pars_.beta;
    pars_.alpha = ori_alpha * nu1 * nu1 / (nu2 * nu2);
    pars_.beta = ori_beta * 2.0 * nu1 * nu1;

    Scalar gt_err;

    bool dynamic_stop = false;   // 外层循环停止标志
    int accumulate_iter = 0;      // 累计迭代次数

    begin_time = time.get_time();

    // ===========================================================
    // 外层循环：Graduated Non-Convexity，逐步减小 nu
    // ===========================================================
    while (!dynamic_stop)
    {
        // 内层循环：在固定 nu 下迭代优化
        for(int out_iter = 0; out_iter < pars_.max_outer_iters; out_iter++)
        {
            // ----- 步骤 1：根据对应点对更新目标位置矩阵 mat_U0_ -----
            mat_U0_.setZero();
            corres_pair_ids_.setZero();
#pragma omp parallel for
            for (size_t i = 0; i < correspondence_pairs_.size(); i++)
            {
                // 将对应点的目标位置存入 mat_U0_（3×n 矩阵，每列一个目标点）
                mat_U0_.col(correspondence_pairs_[i].src_idx) = correspondence_pairs_[i].position;
                corres_pair_ids_[correspondence_pairs_[i].src_idx] = 1;  // 标记该顶点有对应
            }

            // ----- 步骤 2：计算 Welsch 权重 -----
            // 数据项 Welsch 权重：w_i = exp(-||PV*X + P - U_i||^2 / (2*nu1^2))
            if(pars_.data_use_welsch)
            {
                // 计算每个顶点的变形后位置与对应目标点的距离
                weight_d_ = (Weight_PV0 * Smat_X_ + Smat_P_ - mat_U0_.transpose()).rowwise().norm();
                // 将距离转换为 Welsch 权重
                welsch_weight(weight_d_, nu1);
            }
            else
                weight_d_.setOnes();  // 不使用 Welsch 时权重全为 1（等价于 L2 范数）

            // 光滑项 Welsch 权重
            if(pars_.smooth_use_welsch)
            {
                welsch_weight_s = ((Smat_B0 * Smat_X_ - Smat_D0)).rowwise().norm();
                welsch_weight(welsch_weight_s, nu2);
            }
            else
                welsch_weight_s.setOnes();

            int total_inner_iters = 0;
            MatrixXX old_X = Smat_X_;  // 保存当前变换矩阵

            // ----- 步骤 3：更新加权矩阵 -----
            // 将 Welsch 权重平方根与对应标记相乘，然后应用到原始矩阵
            weight_d_ = weight_d_.cwiseSqrt().cwiseProduct(corres_pair_ids_);
            Weight_PV_ = weight_d_.asDiagonal() * Weight_PV0;  // 加权数据项矩阵
            welsch_weight_s = welsch_weight_s.cwiseSqrt().cwiseProduct(Sweight_s_);
            Smat_B_ = welsch_weight_s.asDiagonal() * Smat_B0;  // 加权光滑项矩阵

            // 通过 SVD 从当前仿射变换中提取最近旋转矩阵
            update_R();

            // 更新加权光滑项坐标差和数据项目标位移
            Smat_D_ = welsch_weight_s.asDiagonal() * Smat_D0;
            Smat_UP_ = weight_d_.asDiagonal() * (mat_U0_.transpose() - Smat_P_);

            // ----- 步骤 4：构建线性方程组并预分解 -----
            // mat_A0_ = PV^T*PV + alpha*B^T*B + beta*L (Hessian 近似)
            mat_A0_ = Weight_PV_.transpose() * Weight_PV_
                    + pars_.alpha * Smat_B_.transpose() * Smat_B_
                    + pars_.beta * Smat_L_;
            // mat_VU_ = PV^T * UP + alpha * B^T * D (右端向量)
            mat_VU_ = (Weight_PV_).transpose() * Smat_UP_
                    + pars_.alpha * (Smat_B_).transpose() * Smat_D_;

            // 如果有标记点，加入标记点约束
            if(pars_.use_landmark)
            {
                mat_A0_ += pars_.gamma * Sub_PV_.transpose() * Sub_PV_;
                mat_VU_ += pars_.gamma * Sub_PV_.transpose() * Sub_UP_;
            }

            // Cholesky 符号分析只需做一次（矩阵稀疏模式不变）
            if(run_once)
            {
                ldlt_->analyzePattern(mat_A0_);
                run_once = false;
            }
            // Cholesky 数值分解（每次迭代都需要重新分解）
            ldlt_->factorize(mat_A0_);

            // ----- 步骤 5：求解最优变换矩阵 -----
            if(!pars_.use_lbfgs)
            {
                // 直接求解：A0 * X = beta * J * R + VU
                MatrixXX b = pars_.beta * Smat_J_ * Smat_R_ + mat_VU_;
#pragma omp parallel for
                for (int col_id = 0; col_id < 3; col_id++)
                {
                    Smat_X_.col(col_id) = ldlt_->solve(b.col(col_id));
                }
                total_inner_iters += 1;
            }
            else
            {
                // 使用 L-BFGS 拟牛顿法求解（更快收敛）
                total_inner_iters += QNSolver(data_err, smooth_err, orth_err);
            }

            // ----- 步骤 6：更新网格顶点位置 -----
            // 变形后的顶点位置 = Weight_PV0 * X + P
            MatrixXX target = Weight_PV0 * Smat_X_ + Smat_P_;
            gt_err = SetMeshPoints(src_mesh_, target, curV);
            run_time = time.get_time();

            // 记录本步统计信息
            pars_.each_gt_mean_errs.push_back(gt_err);
            pars_.each_gt_max_errs.push_back(0);
            pars_.each_energys.push_back(0.0);
            double eps_time = time.elapsed_time(begin_time, run_time);
            pars_.each_times.push_back(eps_time);
            pars_.each_iters.push_back(total_inner_iters);
            pars_.each_term_energy.push_back(Vector3(data_err, smooth_err, orth_err));

            // 调试输出：保存每步的中间结果
            if(pars_.print_each_step_info)
            {
                std::string out_obj = pars_.out_each_step_info + "/iter_obj_" + std::to_string(accumulate_iter) + ".ply";
                OpenMesh::IO::write_mesh( *src_mesh_, out_obj.c_str() );
                std::cout << "iter = " << out_iter << " time = " << eps_time
                          << "  inner iter = " << total_inner_iters << std::endl;
            }

            // ----- 步骤 7：重新查找对应点并剪枝 -----
            FindClosestPoints(correspondence_pairs_);
            accumulate_iter++;
            SimplePruning(correspondence_pairs_, pars_.use_distance_reject, pars_.use_normal_reject);

            // ----- 步骤 8：收敛判断 -----
            // 如果顶点位置变化的最大值小于阈值，认为已收敛
            if((curV - prevV).rowwise().norm().maxCoeff() < pars_.stop)
            {
                break;
            }
            prevV = curV;
        }

        // ===========================================================
        // 外层循环：更新 nu 参数
        // ===========================================================
        // 如果 nu1 已达到终止值，或不使用动态 nu，则停止
        if(fabs(nu1-end_nu1)<1e-8 || !pars_.use_Dynamic_nu || !pars_.data_use_welsch)
            dynamic_stop = true;
        // nu1 减半，但不小于终止值
        nu1 = (0.5*nu1> end_nu1)?0.5*nu1:end_nu1;
        nu2 *= 0.5;  // nu2 也减半
        // 根据新的 nu 重新计算 alpha 和 beta
        pars_.alpha = ori_alpha * nu1 * nu1 / (nu2 * nu2);
        pars_.beta = ori_beta * 2 * nu1 * nu1;
    }
    return 0;
}

/**
 * @brief 计算当前能量值（数据项 + 光滑项 + 正交项）
 *
 * E_total = ||PV*X - UP||^2 + α*||B*X - D||^2 + β*∑||X_i - R_i||^2
 *
 * 同时通过 SVD 分解更新每个节点的最近旋转矩阵 R_i。
 * SVD 分解的作用：将 3×3 线性变换分解为 U*S*V^T，
 * 最近旋转 R = U*V^T（如果 det < 0 则修正反射）
 *
 * @param data_err   输出：数据项误差
 * @param smooth_err 输出：光滑项误差
 * @param orth_err   输出：正交项误差
 * @return 总能量值
 */
Scalar NonRigidreg::sample_energy(Scalar& data_err, Scalar& smooth_err, Scalar& orth_err)
{
    data_err =  ( Weight_PV_ * Smat_X_  - Smat_UP_).squaredNorm();
    smooth_err = ( (Smat_B_ * Smat_X_ - Smat_D_)).squaredNorm();
    VectorX orth_errs(num_sample_nodes);
#pragma omp parallel for
    for (int i = 0; i < num_sample_nodes; i++)
    {
        Eigen::JacobiSVD<MatrixXX> svd(Smat_X_.block(4 * i, 0, 3, 3), Eigen::ComputeThinU | Eigen::ComputeThinV);

        if (svd.matrixU().determinant()*svd.matrixV().determinant() < 0.0) {
            Vector3 S = Vector3::Ones(); S(2) = -1.0;
            Smat_R_.block(3 * i, 0, 3, 3) = svd.matrixU()*S.asDiagonal()*svd.matrixV().transpose();
        }
        else {
            Smat_R_.block(3 * i, 0, 3, 3) = svd.matrixU()*svd.matrixV().transpose();
        }
        orth_errs[i] = (Smat_X_.block(4 * i, 0, 3, 3) - Smat_R_.block(3 * i, 0, 3, 3)).squaredNorm();
    }
    orth_err = orth_errs.sum();
    Scalar total_err = data_err + pars_.alpha * smooth_err + pars_.beta * orth_err;
    if(pars_.use_landmark)
        total_err += pars_.gamma * ( Sub_PV_ * Smat_X_  - Sub_UP_).squaredNorm();
    return total_err;
}

/**
 * @brief 通过 SVD 分解更新每个控制节点的最近旋转矩阵 R_i
 *
 * 对每个节点的 3×3 线性变换部分 X_i 进行 SVD 分解：
 *   X_i = U * S * V^T
 * 最近旋转矩阵：R_i = U * V^T
 * 如果 det(U)*det(V) < 0，说明产生反射，需翻转最后一个奇异值对应的列
 */
void NonRigidreg::update_R()
{
#pragma omp parallel for
    for (int i = 0; i < num_sample_nodes; i++)
    {
        Eigen::JacobiSVD<MatrixXX> svd(Smat_X_.block(4 * i, 0, 3, 3), Eigen::ComputeThinU | Eigen::ComputeThinV);
        if (svd.matrixU().determinant()*svd.matrixV().determinant() < 0.0) {
            Vector3 S = Vector3::Ones(); S(2) = -1.0;
            Smat_R_.block(3 * i, 0, 3, 3) = svd.matrixU()*S.asDiagonal()*svd.matrixV().transpose();
        }
        else {
            Smat_R_.block(3 * i, 0, 3, 3) = svd.matrixU()*svd.matrixV().transpose();
        }
    }
}

/**
 * @brief 计算能量函数关于变换矩阵 X 的梯度
 *
 * 梯度公式：∇E = 2 * (A0 * X - VU - β * J * R)
 * 其中 A0 是 Hessian 近似，VU 是右端向量，J*R 是正交约束项
 */
void NonRigidreg::sample_gradient()
{
    grad_X_ = 2 * (mat_A0_ * Smat_X_ - mat_VU_ - pars_.beta * Smat_J_ * Smat_R_);
}

/**
 * @brief L-BFGS 两循环递归算法
 *
 * 利用最近 m 步的步长差 s_i 和梯度差 t_i 来近似 Hessian 逆矩阵，
 * 计算下降方向 dir = H^{-1} * (-∇E)。
 *
 * 算法步骤：
 *   1. 前向循环：从最新到最旧，逐步计算 κ_i 并修正 q
 *   2. 中间步：用 Cholesky 分解求解 H0^{-1} * q 作为初始方向
 *   3. 后向循环：从最旧到最新，逐步修正方向
 *
 * 这里的初始 Hessian H0 使用 mat_A0_（Cholesky 分解），
 * 而不是标准 L-BFGS 中的对角近似，这是本算法的关键创新点。
 *
 * @param iter 当前迭代次数
 * @param dir 输出的下降方向
 */
void NonRigidreg::LBFGS(int iter, MatrixXX & dir) const
{
    VectorX rho(pars_.lbfgs_m);
    VectorX kersi(pars_.lbfgs_m);
    MatrixXX q(4 * num_sample_nodes, 3);
    MatrixXX temp(4 * num_sample_nodes, 3);
    int k = iter;
    q.setZero();
    dir = q;
    q = -grad_X_;
    int m_k = std::min(k, pars_.lbfgs_m);
    for (int i = k - 1; i > k - m_k - 1; i--)
    {
        int col = (pars_.lbfgs_m + col_idx_ - (k - 1 - i)) % pars_.lbfgs_m;
        rho(k - 1 - i) = all_t_.col(col).transpose().dot(all_s_.col(col));
        Scalar lbfgs_err_scalar = Eigen::Map<VectorX>(q.data(), q.size()).dot(all_s_.col(col));
        kersi(k - 1 - i) = lbfgs_err_scalar / rho(k - 1 - i);
        Eigen::Map<VectorX>(q.data(), q.size()) -= kersi(k - 1 - i) * all_t_.col(col);
    }
#pragma omp parallel for
    for(int cid = 0; cid < 3; cid++)
    {
        dir.col(cid) = ldlt_->solve(q.col(cid));
    }

    for (int i = k - m_k; i < k; i++)
    {
        int col = (pars_.lbfgs_m + col_idx_ - (k - 1 - i)) % pars_.lbfgs_m;
        Scalar lbfgs_err_scalar = all_t_.col(col).dot(Eigen::Map<VectorX>(dir.data(), dir.size()));
        Scalar eta = kersi(k - 1 - i) - lbfgs_err_scalar / rho(k - 1 - i);
        Eigen::Map<VectorX>(dir.data(), dir.size()) += all_s_.col(col) * eta;
    }

    rho.resize(0);
    kersi.resize(0);
    q.resize(0, 0);
    temp.resize(0, 0);
    return;
}


/**
 * @brief L-BFGS 拟牛顿求解器（含线搜索）
 *
 * 在固定的 Welsch 权重下，使用 L-BFGS 方法求解最优变换矩阵。
 * 每次迭代：
 *   1. 计算梯度
 *   2. 第一次迭代：使用 Cholesky 分解计算初始方向
 *      后续迭代：使用 LBFGS 两循环递归计算下降方向
 *   3. 回溯线搜索：确保 Armijo 条件满足
 *   4. 更新变换矩阵 X
 *   5. 收敛判断：能量变化小于阈值则停止
 *
 * @param data_err   输出：最终数据项误差
 * @param smooth_err 输出：最终光滑项误差
 * @param orth_err   输出：最终正交项误差
 * @return 实际执行的迭代次数
 */
int NonRigidreg::QNSolver(Scalar& data_err, Scalar& smooth_err, Scalar& orth_err)
{
    MatrixXX prev_X;           // 上一步的变换矩阵
    int count_linesearch = 0;  // 线搜索计数

    int iter;
    for (iter = 0; iter <= pars_.max_inner_iters; iter++)
    {
        // 计算当前梯度
        sample_gradient();

        // 更新下降方向
        if (iter == 0)
        {
            // 第一次迭代：使用 Cholesky 分解作为初始方向 d = A0^{-1} * (-grad)
            MatrixXX temp = -grad_X_;
#pragma omp parallel for
            for(int cid = 0; cid < 3; cid++)
            {
                direction_.col(cid) = ldlt_->solve(temp.col(cid));
            }

            // 初始化 L-BFGS 历史记录
            col_idx_ = 0;
            all_s_.col(col_idx_) = -Eigen::Map<VectorX>(Smat_X_.data(), 4 * num_sample_nodes * 3);
            all_t_.col(col_idx_) = -Eigen::Map<VectorX>(grad_X_.data(), 4 * num_sample_nodes * 3);
        }
        else
        {
            // 更新 L-BFGS 历史记录：完成 s_i = X_{i+1} - X_i 和 t_i = ∇E_{i+1} - ∇E_i
            all_s_.col(col_idx_) += Eigen::Map<VectorX>(Smat_X_.data(), 4 * num_sample_nodes * 3);
            all_t_.col(col_idx_) += Eigen::Map<VectorX>(grad_X_.data(), 4 * num_sample_nodes * 3);

            // 使用 L-BFGS 两循环递归计算下降方向
            LBFGS(iter, direction_);

            // 更新环形缓冲区指针，开始记录新一步的历史
            col_idx_ = (col_idx_ + 1) % pars_.lbfgs_m;
            all_s_.col(col_idx_) = -Eigen::Map<VectorX>(Smat_X_.data(), 4 * num_sample_nodes * 3);
            all_t_.col(col_idx_) = -Eigen::Map<VectorX>(grad_X_.data(), 4 * num_sample_nodes * 3);
        }

        // 回溯线搜索：找到满足 Armijo 条件的步长 alpha
        Scalar alpha = 2.0;     // 初始步长
        prev_X = Smat_X_;       // 保存当前 X
        Scalar new_err = sample_energy(data_err, smooth_err, orth_err);
        Scalar prev_err = new_err;
        Scalar gamma = 0.3;     // Armijo 条件系数
        // 计算方向导数 x = ∇E^T * d
        Scalar x = (grad_X_.transpose() * direction_).trace();
        do
        {
            alpha /= 2;  // 步长减半
            Smat_X_ = prev_X + alpha * direction_;  // 沿下降方向移动
            new_err = sample_energy(data_err, smooth_err, orth_err);
            count_linesearch++;
        } while (new_err > prev_err + gamma * alpha * x);  // Armijo 条件

        // 收敛判断：能量变化小于阈值则停止
        if (fabs(new_err - prev_err)< pars_.stop)
        {
            break;
        }
        iter_++;  // 全局迭代计数器
    }
    return iter;
}

/**
 * @brief 计算带 Welsch 鲁棒核函数的总误差
 *
 * 与 sample_energy 类似，但数据项和光滑项可选择使用 Welsch 核或 L2 范数。
 * Welsch 核函数：ψ(r) = 1 - exp(-r^2 / (2ν^2))
 * 相比 L2 范数，Welsch 核对大残差（异常值）的惩罚有上界，
 * 因此对错误对应关系更鲁棒。
 */
Scalar NonRigidreg::welsch_error(Scalar nu1, Scalar nu2)
{
    VectorX w_data = (Weight_PV_ * Smat_X_ - Smat_UP_).rowwise().norm();
    Scalar data_err;
    if(pars_.data_use_welsch)
        data_err = welsch_energy(w_data, nu1);
    else
        data_err = w_data.squaredNorm();
    VectorX s_data = ((Smat_B_ * Smat_X_ - Smat_D_)).rowwise().norm();
    Scalar smooth_err;
    if(pars_.smooth_use_welsch)
        smooth_err = welsch_energy(s_data, nu2);
    else
        smooth_err = s_data.squaredNorm();

    VectorX orth_errs(num_sample_nodes);
#pragma omp parallel for
    for (int i = 0; i < num_sample_nodes; i++)
    {
        orth_errs[i] = (Smat_X_.block(4 * i, 0, 3, 3) - Smat_R_.block(3 * i, 0, 3, 3)).squaredNorm();
    }
    return data_err + pars_.alpha * smooth_err + pars_.beta * orth_errs.sum();
}

/**
 * @brief 计算 Welsch 能量：E = ∑(1 - exp(-r_i^2 / (2p^2)))
 *
 * Welsch 函数的特性：
 *   - 当 r 小时，接近 r^2/(2p^2)（类似 L2 范数）
 *   - 当 r 大时，趋近于 1（对异常值的惩罚有上界）
 *   - p 控制转折点：p越大越接近L2，p越小越鲁棒
 *
 * @param r 残差向量
 * @param p Welsch 参数 nu
 * @return Welsch 能量值
 */
Scalar NonRigidreg::welsch_energy(VectorX& r, Scalar p) {
    VectorX energy(r.rows());
#pragma omp parallel for
    for (int i = 0; i<r.rows(); ++i) {
        energy[i] = 1.0 - std::exp(-r(i)*r(i) / (2 * p*p));
    }
    return energy.sum();
}

/**
 * @brief 计算 Welsch 权重：w_i = exp(-r_i^2 / (2p^2))
 *
 * 这是 Welsch 核函数的一阶导数除以 r 后的结果，
 * 用于将非线性的 Welsch 问题转化为加权最小二乘问题。
 * 权重特性：
 *   - 小残差的点权重接近 1（可信）
 *   - 大残差的点权重接近 0（异常值，被自动降权）
 *
 * @param r 输入残差向量，输出时被替换为权重向量
 * @param p Welsch 参数 nu
 */
void NonRigidreg::welsch_weight(VectorX& r, Scalar p) {
#pragma omp parallel for
    for (int i = 0; i<r.rows(); ++i) {
        r(i) = std::exp(-r(i)*r(i) / (2 * p*p));
    }
}

/**
 * @brief 将变换结果应用到网格顶点，并计算真值误差
 *
 * 将优化得到的变形后顶点坐标设置到网格中，
 * 同时如果源目标顶点数相同，计算逐点的配准误差。
 *
 * @param mesh 要更新的网格
 * @param target 变形后的顶点坐标矩阵 (n×3)
 * @param cur_v 输出：当前顶点位置（用于收敛判断）
 * @return 平均真值误差，或 -1.0（如果不计算）
 */
Scalar NonRigidreg::SetMeshPoints(Mesh* mesh, const MatrixXX & target, MatrixXX& cur_v)
{
    VectorX gt_errs(n_src_vertex_);
#pragma omp parallel for
    for (int i = 0; i < n_src_vertex_; i++)
    {
        MatrixXX tar_p = target.block(i, 0, 1, 3);
        Vec3 p(tar_p(0, 0), tar_p(0, 1), tar_p(0, 2));
        mesh->set_point(mesh->vertex_handle(i), p);
        cur_v.row(i) = tar_p;
        if(pars_.calc_gt_err)
            gt_errs[i] = (tar_p.transpose() - tar_points_.col(i)).squaredNorm();
    }
    if(pars_.calc_gt_err)
        return gt_errs.sum()/n_src_vertex_;
    else
        return -1.0;
}
