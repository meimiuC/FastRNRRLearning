/**
 * @file nodeSampler.h
 * @brief 控制节点采样器 - 基于测地距离的均匀采样和节点图构建
 *
 * 【文件作用】
 *   定义了节点采样器 nodeSampler 类，它是非刚性配准中的关键组件。
 *   负责在源网格上采样稀疏的控制节点，并构建节点之间的连接图。
 *   这些控制节点用于驱动网格变形：每个控制节点有独立的仿射变换，
 *   网格上的其他顶点通过插值控制节点的变换来获得自己的变形。
 *
 * 【算法原理】
 *   1. 采样过程：
 *      - 沿指定坐标轴排序所有顶点
 *      - 贪心选择：依次遍历顶点，如果该顶点不在已选节点的测地距离覆盖范围内，
 *        则选为新的控制节点
 *      - 采样半径 = 平均边长 × sampleRadiusRatio
 *   2. 权重计算：
 *      - 每个顶点受附近控制节点的影响，权重由测地距离决定
 *      - 权重函数：w = (1 - (d/R)^2)^3（紧支撑径向基函数）
 *   3. 节点图：
 *      - 如果两个控制节点都影响同一个顶点，则它们之间有一条边
 *      - 节点图用于构建光滑项矩阵
 *
 * 【与其他文件的关系】
 *   - 被 NonRigidreg.h 包含：NonRigidreg 拥有一个 nodeSampler 成员
 *   - 在 NonRigidreg::Initialize() 中调用采样和权重初始化
 *   - 依赖 geodesic/ 目录：精确测地距离算法
 *   - 依赖 Types.h：数据类型定义
 *
 * 【输出矩阵说明】
 *   initWeight() 函数初始化以下矩阵：
 *   - matPV (n×4r)：顶点到控制节点的权重矩阵
 *   - matP  (n×3)：权重加权的控制节点坐标
 *   - matB  (|e|×4r)：光滑项系数矩阵
 *   - matD  (|e|×3)：相邻节点间的坐标差
 *   - smoothw (|e|)：光滑项权重
 */

#ifndef NODESAMPLER_H
#define NODESAMPLER_H
#include "Types.h"

namespace svr
{
    /**
     * @class neighborIter
     * @brief 邻居节点迭代器，用于遍历某个节点的所有邻居
     *
     * 封装 std::map 迭代器，提供简洁的遍历接口。
     * 每个邻居存储为 (节点索引, 权重) 对。
     */
    class neighborIter
    {
    public:
        neighborIter(const std::map<size_t, Scalar> &nodeNeighbors)
        {
            m_neighborIter = nodeNeighbors.begin();
            m_neighborEnd = nodeNeighbors.end();
        }

        neighborIter& operator++()
        {
            if (m_neighborIter != m_neighborEnd)
                ++m_neighborIter;
            return *this;
        }

        neighborIter operator++(int)
        {
            neighborIter tempIter(*this);
            ++*this;
            return tempIter;
        }

        const std::pair<const size_t, Scalar>& operator*() { return *m_neighborIter; }
        std::map<size_t, Scalar>::const_iterator operator->() { return m_neighborIter; }
        bool is_valid() { return m_neighborIter != m_neighborEnd; }
        size_t getIndex() { return m_neighborIter->first; }     // 获取邻居节点索引
        Scalar getWeight() { return m_neighborIter->second; }   // 获取邻居边权重

    private:
        std::map<size_t, Scalar>::const_iterator m_neighborIter;
        std::map<size_t, Scalar>::const_iterator m_neighborEnd;
    };

    /**
     * @class nodeSampler
     * @brief 控制节点采样器
     *
     * 主要功能：
     * - sampleAndconstuct()：在网格上采样控制节点并构建节点图
     * - initWeight()：初始化数据项和光滑项的系数矩阵
     * - print_nodes()：将节点图输出为 OBJ 文件（调试用）
     */
    class nodeSampler
    {
    public:
        // 采样轴：决定贪心选择的遍历顺序
        enum sampleAxis { X_AXIS, Y_AXIS, Z_AXIS };
        nodeSampler() {};

        /**
         * @brief 在网格上采样控制节点并构建节点图
         * @param mesh 输入网格
         * @param sampleRadiusRatio 采样半径与平均边长的比率
         * @param axis 贪心选择的排序轴
         * @return 实际的采样半径
         */
        Scalar sampleAndconstuct(Mesh &mesh, Scalar sampleRadiusRatio, sampleAxis axis);

        void updateWeight(Mesh &mesh);
        void constructGraph(bool is_uniform);

        /** @brief 获取采样节点总数 */
        size_t nodeSize() const { return m_nodeContainer.size(); }

        /** @brief 获取指定节点的邻居节点迭代器 */
        neighborIter getNodeNodeIter(size_t nodeIdx) const { return neighborIter(m_nodeGraph[nodeIdx]); }
        /** @brief 获取指定顶点关联的控制节点迭代器 */
        neighborIter getVertexNodeIter(size_t vertexIdx) const { return neighborIter(m_vertexGraph[vertexIdx]); }
        /** @brief 获取指定节点对应的网格顶点索引 */
        size_t getNodeVertexIdx(size_t nodeIdx) const { return m_nodeContainer.at(nodeIdx).second; }

        size_t getVertexNeighborSize(size_t vertexIdx) const { return m_vertexGraph.at(vertexIdx).size(); }
        size_t getNodeNeighborSize(size_t nodeIdx) const { return m_nodeGraph.at(nodeIdx).size(); }

        /**
         * @brief 初始化数据项和光滑项的系数矩阵
         *
         * @param matPV   输出：顶点-节点权重矩阵 (n×4r)
         * @param matP    输出：权重加权的节点坐标 (n×3)
         * @param matB    输出：光滑项系数矩阵 (|e|×4r)
         * @param matD    输出：相邻节点坐标差 (|e|×3)
         * @param smoothw 输出：光滑项权重 (|e|)
         */
        void initWeight(Eigen::SparseMatrix<Scalar>& matPV, MatrixXX & matP,
                        Eigen::SparseMatrix<Scalar>& matB, MatrixXX& matD, VectorX& smoothw);

        /** @brief 将节点图输出为 OBJ 文件（调试用） */
        void print_nodes(Mesh & mesh, std::string file_path);

    private:
        size_t m_meshVertexNum = 0;       // 网格顶点总数
        size_t m_meshEdgeNum = 0;         // 网格边总数
        Scalar m_averageEdgeLen = 0.0f;   // 网格平均边长
        Scalar m_sampleRadius = 0.0f;     // 采样半径
        std::vector<Scalar> non_unisamples_Radius;  // 非均匀采样半径（预留）
        Mesh * m_mesh;                    // 网格指针

        // m_nodeContainer[i] = (节点索引i, 对应的网格顶点索引)
        std::vector<std::pair<size_t, size_t>> m_nodeContainer;
        // m_vertexGraph[顶点索引] = {(影响该顶点的节点索引, 权重), ...}
        std::vector<std::map<size_t, Scalar>> m_vertexGraph;
        // m_nodeGraph[节点索引] = {(邻居节点索引, 连接权重), ...}
        std::vector<std::map<size_t, Scalar>> m_nodeGraph;
        // m_nodeVertexGraph[节点索引] = {(关联的顶点索引, 权重), ...}
        std::vector<std::map<size_t, Scalar>> m_nodeVertexGraph;
        // 测地距离容器（预留）
        std::vector<VectorX> m_geoDistContainer;
        // 每个顶点所属的节点索引（-1 表示还不属于任何节点）
        Eigen::VectorXi      VertexNodeIdx;
    };
}
#endif
