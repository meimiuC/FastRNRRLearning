/**
 * @file tools.cpp
 * @brief 工具函数实现 - 网格缩放、类型转换、文件读取等
 *
 * 【文件作用】
 *   实现 tools.h 中声明的各种工具函数，包括：
 *   1. Eigen2Vec / Vec2Eigen：OpenMesh 和 Eigen 向量类型互转
 *   2. mesh_scaling()：网格归一化缩放（使两个网格在统一尺度下配准）
 *   3. Mesh2VF()：OpenMesh 网格转 libigl 格式（顶点+面矩阵）
 *   4. read_landmark()：读取标记点文件
 *   5. read_fixedvex()：读取固定顶点文件
 *   6. my_mkdir()：创建目录（Linux 专用）
 *
 * 【与其他文件的关系】
 *   - 实现 tools.h 中声明的函数
 *   - 被 main.cpp 调用：mesh_scaling, read_landmark
 *   - 被 Registration.cpp 间接使用：Eigen2Vec, Vec2Eigen
 */

#include "tools.h"
#include <iostream>

#ifdef __linux__
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

const float SHRINKING_FACTOR = 7.5f;               // 收缩因子（未使用）
const int NO_PROGRESS_STREAK_THRESHOLD = 100;       // 无进展阈值（未使用）

/**
 * @brief 将 Eigen::Vector3 转换为 OpenMesh::Vec3
 */
Vec3 Eigen2Vec(Vector3 s)
{
    return Vec3(s[0], s[1], s[2]);
}

/**
 * @brief 将 OpenMesh::Vec3 转换为 Eigen::Vector3
 */
Vector3 Vec2Eigen(Vec3 s)
{
    return Vector3(s[0], s[1], s[2]);
}

/**
 * @brief 归一化缩放：将源网格和目标网格的坐标归一化到统一尺度
 *
 * 计算两个网格的联合包围盒对角线长度 scale，
 * 然后将所有顶点坐标除以 scale，使得网格在 [0,1] 量级的空间中。
 * 这样可以使距离阈值等参数与网格实际大小无关。
 *
 * @param src_mesh 源网格（会被修改）
 * @param tar_mesh 目标网格（会被修改）
 * @return 缩放因子 scale（输出结果时需要乘回这个值以恢复原始尺度）
 */
Scalar mesh_scaling(Mesh& src_mesh, Mesh& tar_mesh)
{
    Vec3 max(-1e12, -1e12, -1e12);
    Vec3 min(1e12, 1e12, 1e12);
    for(auto it = src_mesh.vertices_begin(); it != src_mesh.vertices_end(); it++)
    {
        for(int j = 0; j < 3; j++)
        {
            if(src_mesh.point(*it)[j] > max[j])
            {
                max[j] = src_mesh.point(*it)[j];
            }
            if(src_mesh.point(*it)[j] < min[j])
            {
                min[j] = src_mesh.point(*it)[j];
            }
        }
    }

    for(auto it = tar_mesh.vertices_begin(); it != tar_mesh.vertices_end(); it++)
    {
        for(int j = 0; j < 3; j++)
        {
            if(tar_mesh.point(*it)[j] > max[j])
            {
                max[j] = tar_mesh.point(*it)[j];
            }
            if(tar_mesh.point(*it)[j] < min[j])
            {
                min[j] = tar_mesh.point(*it)[j];
            }
        }
    }
    Scalar scale = (max-min).norm();        // 计算对角线长度

    for(auto it = src_mesh.vertices_begin(); it != src_mesh.vertices_end(); it++)
    {
        Vec3 p = src_mesh.point(*it);
        p = p/scale;
        src_mesh.set_point(*it, p);
    }

    for(auto it = tar_mesh.vertices_begin(); it != tar_mesh.vertices_end(); it++)
    {
        Vec3 p = tar_mesh.point(*it);
        p = p/scale;
        tar_mesh.set_point(*it, p);
    }

    return scale;
}

/**
 * @brief 将 OpenMesh 网格转换为 libigl 格式（顶点矩阵 V + 面索引矩阵 F）
 *
 * @param mesh 输入的 OpenMesh 网格
 * @param V 输出：n×3 顶点坐标矩阵
 * @param F 输出：m×3 面索引矩阵（每行三个顶点索引）
 */
void Mesh2VF(Mesh & mesh, MatrixXX& V, Eigen::MatrixXi& F)
{
    V.resize(mesh.n_vertices(),3);
    F.resize(mesh.n_faces(),3);
    for (auto it = mesh.vertices_begin(); it != mesh.vertices_end(); it++)
    {
        V(it->idx(), 0) = mesh.point(*it)[0];
        V(it->idx(), 1) = mesh.point(*it)[1];
        V(it->idx(), 2) = mesh.point(*it)[2];
    }

    for (auto fit = mesh.faces_begin(); fit != mesh.faces_end(); fit++)
    {
        int i = 0;
        for (auto vit = mesh.fv_begin(*fit); vit != mesh.fv_end(*fit); vit++)
        {
            F(fit->idx(), i) = vit->idx();
            i++;
            if (i > 3)
            {
                std::cout << "Error!! one face has more than 3 points!" << std::endl;
                break;
            }
        }
    }
}

/**
 * @brief 从文件读取标记点对应关系
 *
 * 文件格式：每行两个整数，分别是源网格和目标网格的顶点索引
 * 例如：
 *   42 128    ← 源网格第42个顶点对应目标网格第128个顶点
 *   100 256   ← 源网格第100个顶点对应目标网格第256个顶点
 *
 * @param filename 标记点文件路径
 * @param landmark_src 输出：源网格标记点索引列表
 * @param landmark_tar 输出：目标网格标记点索引列表
 * @return 是否读取成功
 */
bool read_landmark(const char* filename, std::vector<int>& landmark_src, std::vector<int>& landmark_tar)
{
    std::ifstream in(filename);
    std::cout << "filename = " << filename << std::endl;
    if (!in)
    {
        std::cout << "Can't open the landmark file!!" << std::endl;
        return false;
    }
    int x, y;
    landmark_src.clear();
    landmark_tar.clear();
    while (!in.eof())
    {
        if (in >> x >> y) {
            landmark_src.push_back(x);
            landmark_tar.push_back(y);
        }
    }
    in.close();
    std::cout << "landmark_src = " << landmark_src.size() << " tar = " << landmark_tar.size() << std::endl;
    return true;
}

/**
 * @brief 从文件读取固定顶点列表
 *
 * 文件格式：每行一个整数，表示不参与变形的顶点索引。
 * 固定顶点在配准过程中保持不动。
 *
 * @param filename 固定顶点文件路径
 * @param vertices_list 输出：固定顶点索引列表
 * @return 是否读取成功
 */
bool read_fixedvex(const char* filename, std::vector<int>& vertices_list)
{
    std::ifstream in(filename);
    std::cout << "filename = " << filename << std::endl;
    if (!in)
    {
        std::cout << "Can't open the landmark file!!" << std::endl;
        return false;
    }
    int x;
    vertices_list.clear();
    while (!in.eof())
    {
        if (in >> x) {
            vertices_list.push_back(x);
        }
    }
    in.close();
    std::cout << "the number of fixed vertices = " << vertices_list.size() << std::endl;
    return true;
}

#ifdef __linux__
/**
 * @brief 创建目录（仅 Linux 系统）
 * 如果目录不存在或无写权限，则创建它。
 */
bool my_mkdir(std::string file_path)
{
    if(access(file_path.c_str(), 06))
   {
       std::cout << "file_path : (" << file_path << ") didn't exist or no write ability!!" << std::endl;
       if(mkdir(file_path.c_str(), S_IRWXU))
       {
           std::cout << "mkdir " << file_path << " is wrong! please check upper path " << std::endl;
           exit(0);
       }
       std::cout<< "mkdir " << file_path << " success!! " << std::endl;
   }
}
#endif
