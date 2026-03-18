/**
 * @file nodeSampler.cpp
 * @brief 控制节点采样器实现 - 测地距离采样、节点图构建、权重矩阵初始化
 *
 * 【文件作用】
 *   实现 nodeSampler 类的所有方法，是非刚性配准中变形驱动的核心。
 *   主要包括：
 *   1. sampleAndconstuct()：基于测地距离的贪心采样 + 构建节点图
 *   2. initWeight()：初始化数据项和光滑项的系数矩阵
 *   3. print_nodes()：调试输出节点图
 *
 * 【与其他文件的关系】
 *   - 实现 nodeSampler.h 中声明的类方法
 *   - 被 NonRigidreg::Initialize() 调用
 *   - 依赖 geodesic/geodesic_algorithm_exact.h：精确测地距离算法
 *   - 依赖 tools.h：基础工具函数
 *
 * 【采样算法详细流程】
 *   1. 计算网格平均边长，确定采样半径
 *   2. 沿指定轴排序所有顶点
 *   3. 贪心遍历：对每个顶点，如果不在已选节点的覆盖范围内，则选为新节点
 *   4. 选为新节点后，用精确测地距离算法计算该节点到周围顶点的距离
 *   5. 距离小于采样半径的顶点被标记为"已覆盖"
 *   6. 计算顶点-节点权重：w = (1 - (d/R)^2)^3
 *   7. 构建节点邻接图：如果两个节点共同覆盖了某个顶点，则它们相邻
 *   8. 归一化顶点-节点权重
 */

//#pragma once
#include "nodeSampler.h"
#include "tools.h"

#include "geodesic/geodesic_algorithm_exact.h"

namespace svr
{
    // 辅助 lambda 函数
    static auto square = [](const Scalar argu) { return argu * argu; };     // 平方
    static auto cube = [](const Scalar argu) { return argu * argu * argu; };// 立方
    static auto max = [](const Scalar lhs, const Scalar rhs) { return lhs > rhs ? lhs : rhs; };  // 最大值

    //------------------------------------------------------------------------
    //	Node Sampling based on geodesic distance metric
    //
    //	Note that this member function samples nodes along some axis.
    //	Each node is not covered by any other node. And distance between each
    //	pair of nodes is at least sampling radius.
    //------------------------------------------------------------------------


    /**
     * @brief 基于测地距离的贪心采样和节点图构建
     *
     * 详细步骤：
     * 1. 计算平均边长，确定采样半径 = 平均边长 × sampleRadiusRatio
     * 2. 沿指定轴对顶点排序（保证采样的空间均匀性）
     * 3. 贪心遍历：对每个未被覆盖的顶点，将其选为控制节点
     * 4. 对每个新节点，使用精确测地距离算法计算其到周围顶点的距离
     * 5. 距离小于采样半径的顶点被标记为“已覆盖”，并计算权重
     * 6. 构建节点邻接图：如果两个节点都覆盖了同一个顶点，则它们相邻
     * 7. 归一化顶点-节点权重
     *
     * @param mesh 输入网格
     * @param sampleRadiusRatio 采样半径与平均边长的比率
     * @param axis 排序轴（X_AXIS/Y_AXIS/Z_AXIS）
     * @return 实际的采样半径
     */
    Scalar nodeSampler::sampleAndconstuct(Mesh &mesh, Scalar sampleRadiusRatio, sampleAxis axis)
    {
        // 保存网格基本信息
        m_meshVertexNum = mesh.n_vertices();
        m_meshEdgeNum = mesh.n_edges();
        m_mesh = & mesh;

        // 计算平均边长
        for (size_t i = 0; i < m_meshEdgeNum; ++i)
        {
            OpenMesh::EdgeHandle eh = mesh.edge_handle(i);
            Scalar edgeLen = mesh.calc_edge_length(eh);
            m_averageEdgeLen += edgeLen;
        }
        m_averageEdgeLen /= m_meshEdgeNum;

        // 采样半径 = 平均边长 × 比率参数
        m_sampleRadius = sampleRadiusRatio * m_averageEdgeLen;

        // 沿指定坐标轴对顶点排序（降序）
        // 这样贪心采样会从该轴的最大值开始，产生更均匀的采样
        std::vector<size_t> vertexReorderedAlongAxis(m_meshVertexNum);
        size_t vertexIdx = 0;
        std::generate(vertexReorderedAlongAxis.begin(), vertexReorderedAlongAxis.end(), [&vertexIdx]() -> size_t { return vertexIdx++; });
        std::sort(vertexReorderedAlongAxis.begin(), vertexReorderedAlongAxis.end(), [&mesh, axis](const size_t &lhs, const size_t &rhs) -> bool {
            size_t lhsIdx = lhs;
            size_t rhsIdx = rhs;
            OpenMesh::VertexHandle vhl = mesh.vertex_handle(lhsIdx);
            OpenMesh::VertexHandle vhr = mesh.vertex_handle(rhsIdx);
            Mesh::Point vl = mesh.point(vhl);
            Mesh::Point vr = mesh.point(vhr);
            return vl[axis] > vr[axis];  // 降序排列
        });

        // 初始化采样数据结构
        size_t firstVertexIdx = vertexReorderedAlongAxis[0];  // 第一个节点
        VertexNodeIdx.resize(m_meshVertexNum);
        VertexNodeIdx.setConstant(-1);  // -1 表示未分配节点
        VertexNodeIdx[firstVertexIdx] = 0;
        size_t cur_node_idx = 0;

        m_vertexGraph.resize(m_meshVertexNum);
        VectorX weight_sum = VectorX::Zero(m_meshVertexNum);  // 每个顶点的权重和（用于归一化）

        // 贪心采样循环
        for (auto &vertexIdx : vertexReorderedAlongAxis)
        {
            // 只处理未被任何节点覆盖的顶点
            if(VertexNodeIdx[vertexIdx] < 0 && m_vertexGraph.at(vertexIdx).empty())
            {
                // 将该顶点选为新的控制节点
                m_nodeContainer.emplace_back(cur_node_idx, vertexIdx);
                VertexNodeIdx[vertexIdx] = cur_node_idx;

                // 使用精确测地距离算法计算该节点到周围顶点的距离
                std::vector<size_t> neighbor_verts;
                geodesic::GeodesicAlgorithmExact geoalg(&mesh, vertexIdx, m_sampleRadius);
                geoalg.propagate(vertexIdx, neighbor_verts);  // 传播测地距离

                // 对采样半径内的顶点计算权重
                for(size_t i = 0; i < neighbor_verts.size(); i++)
                {
                    int neighIdx = neighbor_verts[i];
                    Scalar geodist = mesh.data(mesh.vertex_handle(neighIdx)).geodesic_distance;
                    if(geodist < m_sampleRadius)
                    {
                        // 权重函数：w = (1 - (d/R)^2)^3
                        // 这是一个紧支撑径向基函数，距离越近权重越大
                        Scalar weight = std::pow(1-std::pow(geodist/m_sampleRadius, 2), 3);
                        m_vertexGraph.at(neighIdx).emplace(std::pair<int, Scalar>(cur_node_idx, weight));
                        weight_sum[neighIdx] += weight;
                    }
                }
                cur_node_idx++;
            }
        }

        // 构建节点邻接图
        // 如果两个节点都影响了同一个顶点，则它们之间有一条边
        m_nodeGraph.resize(cur_node_idx);
        for (auto &vertexIdx : vertexReorderedAlongAxis)
        {
            for(auto &node: m_vertexGraph[vertexIdx])
            {
                size_t nodeIdx = node.first;
                for(auto &neighNode: m_vertexGraph[vertexIdx])
                {
                    size_t neighNodeIdx = neighNode.first;
                    if(nodeIdx != neighNodeIdx)
                    {
                        // 在节点图中添加边
                        m_nodeGraph.at(nodeIdx).emplace(std::pair<int, Scalar>(neighNodeIdx, 1.0));
                    }
                }
                // 归一化顶点-节点权重（使每个顶点的所有节点权重之和为1）
                m_vertexGraph.at(vertexIdx).at(nodeIdx) /= weight_sum[vertexIdx];
            }
        }
        return m_sampleRadius;
    }



    /**
     * @brief 初始化数据项和光滑项的系数矩阵
     *
     * 数据项矩阵构建：
     *   matPV (n×4r)：每个顶点 vi 的变形后坐标 = ∑_j w_j * (A_j * (vi - pj) + t_j + pj)
     *     其中 w_j 是权重，A_j 是 3×3 线性变换，t_j 是平移，pj 是节点j的坐标
     *     matPV 的每行对应一个顶点，每4列对应一个节点的 (dx, dy, dz, 1)
     *   matP (n×3)：权重加权的节点坐标和，是变形的参考点
     *
     * 光滑项矩阵构建：
     *   matB (|e|×4r)：节点间光滑约束，要求相邻节点的变换将同一点映射到相同位置
     *   matD (|e|×3)：相邻节点间的坐标差
     *   smoothw (|e|)：光滑权重（与节点间距离成反比）
     */
    void nodeSampler::initWeight(Eigen::SparseMatrix<Scalar>& matPV, MatrixXX & matP,
        Eigen::SparseMatrix<Scalar>& matB, MatrixXX& matD, VectorX& smoothw)
    {
        std::vector<Eigen::Triplet<Scalar>> coeff;
        matP.setZero();
        Eigen::VectorXi nonzero_num = Eigen::VectorXi::Zero(m_mesh->n_vertices());

        // ===== 构建数据项系数矩阵 matPV =====
        // 对每个顶点，遍历其关联的控制节点，填写 matPV 的非零元素
        for (size_t vertexIdx = 0; vertexIdx < m_meshVertexNum; ++vertexIdx)
        {
            Mesh::Point vi = m_mesh->point(m_mesh->vertex_handle(vertexIdx));  // 顶点坐标
            for (auto &eachNeighbor : m_vertexGraph[vertexIdx])
            {
                size_t nodeIdx = eachNeighbor.first;  // 控制节点索引
                Scalar weight = m_vertexGraph.at(vertexIdx).at(nodeIdx);  // 归一化后的权重
                Mesh::Point pj = m_mesh->point(m_mesh->vertex_handle(getNodeVertexIdx(nodeIdx)));  // 节点坐标

                // matPV 的每行：w * [(vi-pj)_x, (vi-pj)_y, (vi-pj)_z, 1]
                // 对应仿射变换：w * (A * (vi-pj) + t)
                coeff.push_back(Eigen::Triplet<Scalar>(vertexIdx, nodeIdx * 4, weight * (vi[0] - pj[0])));
                coeff.push_back(Eigen::Triplet<Scalar>(vertexIdx, nodeIdx * 4 + 1, weight * (vi[1] - pj[1])));
                coeff.push_back(Eigen::Triplet<Scalar>(vertexIdx, nodeIdx * 4 + 2, weight * (vi[2] - pj[2])));
                coeff.push_back(Eigen::Triplet<Scalar>(vertexIdx, nodeIdx * 4 + 3, weight * 1.0));
                // matP：累加权重加权的节点坐标
                matP(vertexIdx, 0) += weight * pj[0];
                matP(vertexIdx, 1) += weight * pj[1];
                matP(vertexIdx, 2) += weight * pj[2];
            }
            nonzero_num[vertexIdx] = m_vertexGraph[vertexIdx].size();
        }
        matPV.setFromTriplets(coeff.begin(), coeff.end());

        // ===== 构建光滑项系数矩阵 matB =====
        // 对节点图中的每条边 (nodeIdx, neighborIdx)，
        // 要求：A_neighbor * (v0-v1) + t_neighbor - t_node ≈ (v0-v1)
        coeff.clear();
        int max_edge_num = nodeSize() * (nodeSize()-1);  // 最大可能的边数
        matB.resize(max_edge_num, 4 * nodeSize());
        matD.resize(max_edge_num, 3);
        smoothw.resize(max_edge_num);
        matD.setZero();
        int edge_id = 0;
        for (size_t nodeIdx = 0; nodeIdx < m_nodeContainer.size(); ++nodeIdx)
        {
            size_t vIdx0 = getNodeVertexIdx(nodeIdx);  // 节点对应的顶点索引
            Mesh::VertexHandle vh0 = m_mesh->vertex_handle(vIdx0);
            Mesh::Point v0 = m_mesh->point(vh0);
            for (auto &eachNeighbor : m_nodeGraph[nodeIdx])
            {
                size_t neighborIdx = eachNeighbor.first;
                size_t vIdx1 = getNodeVertexIdx(neighborIdx);
                Mesh::Point v1 = m_mesh->point(m_mesh->vertex_handle(vIdx1));
                Mesh::Point dv = v0 - v1;  // 两个节点间的坐标差
                int k = edge_id;

                // matB 的每行：对应一条节点图边的光滑约束
                coeff.push_back(Eigen::Triplet<Scalar>(k, neighborIdx * 4, dv[0]));
                coeff.push_back(Eigen::Triplet<Scalar>(k, neighborIdx * 4 + 1, dv[1]));
                coeff.push_back(Eigen::Triplet<Scalar>(k, neighborIdx * 4 + 2, dv[2]));
                coeff.push_back(Eigen::Triplet<Scalar>(k, neighborIdx * 4 + 3, 1.0));
                coeff.push_back(Eigen::Triplet<Scalar>(k, nodeIdx * 4 + 3, -1.0));

                // 光滑权重：与节点间距离成反比（距离越近约束越强）
                Scalar dist = dv.norm();
                if(dist > 0)
                {
                    smoothw[k] = 1.0/ dist;
                }
                else
                {
                    smoothw[k] = 0.0;
                    std::cout << "node repeat";
                    exit(1);
                }
                // 记录节点间坐标差
                matD(k, 0) = dv[0];
                matD(k, 1) = dv[1];
                matD(k, 2) = dv[2];
                edge_id++;
            }
        }
        matB.setFromTriplets(coeff.begin(), coeff.end());
        // 调整矩阵大小为实际边数
        matD.conservativeResize(edge_id, 3);
        matB.conservativeResize(edge_id, matPV.cols());
        smoothw.conservativeResize(edge_id);
        // 归一化光滑权重（使其总和为边数）
        smoothw *= edge_id/(smoothw.sum());
    }


    /**
     * @brief 将节点图输出为 OBJ 文件（调试用）
     *
     * 输出两个文件：
     * - nodes.obj：包含所有控制节点的位置和节点间的连接边
     * - edges.txt：节点图的边列表（节点索引和对应的顶点索引）
     */
    void nodeSampler::print_nodes(Mesh & mesh, std::string file_path)
    {
        std::string namev = file_path + "nodes.obj";
        std::ofstream out1(namev);
        //std::cout << "print nodes to " << name << std::endl;
        for (size_t i = 0; i < m_nodeContainer.size(); i++)
        {
            int vexid = m_nodeContainer[i].second;
            out1 << "v " << mesh.point(mesh.vertex_handle(vexid))[0] << " " << mesh.point(mesh.vertex_handle(vexid))[1]
                << " " << mesh.point(mesh.vertex_handle(vexid))[2] << std::endl;
        }
        Eigen::VectorXi nonzero_num = Eigen::VectorXi::Zero(m_nodeContainer.size());
        for (size_t nodeIdx = 0; nodeIdx < m_nodeContainer.size(); ++nodeIdx)
        {
            for (auto &eachNeighbor : m_nodeGraph[nodeIdx])
            {
                size_t neighborIdx = eachNeighbor.first;
                out1 << "l " << nodeIdx+1 << " " << neighborIdx+1 << std::endl;
            }
            nonzero_num[nodeIdx] = m_nodeGraph[nodeIdx].size();
        }
        std::cout << "node neighbor min = " << nonzero_num.minCoeff() << " max = "
                  << nonzero_num.maxCoeff() << " average = " << nonzero_num.mean() << std::endl;
        out1.close();
        std::string namee = file_path + "edges.txt";
        std::ofstream out2(namee);
        for (size_t nodeIdx = 0; nodeIdx < m_nodeContainer.size(); ++nodeIdx)
        {
            size_t vIdx0 = getNodeVertexIdx(nodeIdx);
            for (auto &eachNeighbor : m_nodeGraph[nodeIdx])
            {
                size_t neighborIdx = eachNeighbor.first;
                size_t vIdx1 = getNodeVertexIdx(neighborIdx);
                out2 << nodeIdx << " " << neighborIdx << " " << vIdx0 << " " << vIdx1 << std::endl;
            }
        }
        out2.close();
    }
}
