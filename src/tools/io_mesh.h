/**
 * @file io_mesh.h
 * @brief 网格IO工具 - 读取、写入、检查网格文件
 *
 * 【文件作用】
 *   提供网格文件的读写和基本信息输出功能：
 *   1. read_data()：读取网格文件（支持 .obj, .ply, .off 等格式）
 *   2. write_data()：将配准结果写入网格文件（恢复原始缩放）
 *   3. checkMeshMode()：检查网格类型（三角形/四边形）
 *   4. printBasicMeshInfo()：打印网格基本信息
 *   5. num2str()：整数转字符串（用于文件命名）
 *
 * 【与其他文件的关系】
 *   - 被 main.cpp 包含：调用 read_data 和 write_data
 *   - 依赖 Types.h：Mesh 类型和 OpenMesh IO
 *
 * 【注意】
 *   所有函数都在头文件中实现（inline），因为它们较短且只被 main.cpp 包含一次
 */

#pragma once
#include <iostream>
#include <fstream>
#include "Types.h"

// 网格类型枚举
enum { TRIANGLE = 0, QUAD, N_MESH_MODES};

/**
 * @brief 检查网格类型（三角形/四边形/混合）
 *
 * 遍历所有面，统计每个面的边数来判断网格类型。
 * @param mesh 输入网格
 * @return TRIANGLE=三角形网格, QUAD=四边形网格, N_MESH_MODES=混合网格
 */
int checkMeshMode(Mesh& mesh)
{
    int mesh_mode;
    Mesh::FaceIter fIt = mesh.faces_begin();
    Mesh::FaceIter fEnd = mesh.faces_end();
    Mesh::FaceEdgeIter fe_it;
    int count = 1;
    int meshType[3] = {0};
    for(fIt; fIt != fEnd; ++fIt)
    {
        fe_it = mesh.fe_iter(*fIt);
        while(--fe_it)
        {
            ++count;
        }
        if(count == 4)
        {
            meshType[1]++;
        }
        else if(count == 3)
        {
            meshType[0]++;
        }
        else
        {
            meshType[2]++;
        }
        count = 1;
    }
    size_t faceNum = mesh.n_faces();
    if(meshType[0] == faceNum)//triangle
    {
        mesh_mode = TRIANGLE;
    }
    else if(meshType[1] == faceNum)//no
    {
        mesh_mode = QUAD;
    }
    else
    {
        mesh_mode = N_MESH_MODES;
    }
    return mesh_mode;
}

/**
 * @brief 打印网格基本信息（类型、顶点数、面数、边数）
 */
void printBasicMeshInfo(Mesh& mesh)
{
    if (mesh.n_vertices() == 0)
        printf("No Mesh\n");

    switch(checkMeshMode(mesh))
    {
    case TRIANGLE:
        printf("Triangle Mesh.\n");
        break;
    case QUAD:
        printf("Quadrilateral Mesh.\n");
        break;
    default:
        printf("General Mesh.\n");
        break;
    }

    printf("Information of the input mesh:\nVertex : %d;\nFace : %d;\nEdge : %d, HalfEdge : %d\n\n",
        mesh.n_vertices(),mesh.n_faces(),mesh.n_edges(),mesh.n_halfedges());
}

/**
 * @brief 读取网格文件
 *
 * 使用 OpenMesh 库读取网格文件，支持 .obj, .ply, .off 等格式。
 * 读取后会：
 * - 请求顶点/边/面状态属性
 * - 更新面法向量和顶点法向量
 * - 打印网格基本信息
 *
 * @param filename 网格文件路径
 * @param mesh 输出的网格对象
 * @return 是否读取成功
 */
bool read_data(const std::string filename, Mesh& mesh)
{
    OpenMesh::IO::Options opt_read = OpenMesh::IO::Options::VertexNormal;
    mesh.request_vertex_normals();
    bool read_OK = OpenMesh::IO::read_mesh(mesh, filename,opt_read);

	std::cout << "filename = " << filename << std::endl;
    if (read_OK)
    {
        // 在openmesh中，请求顶点/边/面状态属性后，才能分配到内存，这段代码显示请求分配内存
        mesh.request_vertex_status();
        mesh.request_edge_status();
        mesh.request_face_status();

        mesh.request_face_normals();
        printBasicMeshInfo(mesh);

        mesh.update_face_normals();
        if(mesh.n_faces()>0)
            mesh.update_vertex_normals();

        Vec3 MeshScales;
        MeshScales[0] = 0.0; MeshScales[1] = 0.0; MeshScales[2] = 0.0;
        for (Mesh::VertexIter v_it = mesh.vertices_begin(); v_it != mesh.vertices_end(); ++v_it)
        {
            MeshScales += mesh.point(*v_it);
        }
        MeshScales /= mesh.n_vertices();
        return true;
    }
    std::cout << "#vertices = " << mesh.n_vertices() << std::endl;
    return false;
}

/**
 * @brief 将配准结果写入网格文件
 *
 * 在写入前会将网格顶点坐标乘以 scale 因子，
 * 恢复到原始尺度（之前 mesh_scaling 对网格做了归一化缩放）。
 *
 * @param filename 输出文件路径
 * @param mesh 要写入的网格
 * @param scale 缩放因子（由 mesh_scaling 返回）
 * @return 是否写入成功
 */
bool write_data(const char* filename, Mesh& mesh, Scalar scale)
{
    for (Mesh::VertexIter v_it = mesh.vertices_begin(); v_it != mesh.vertices_end(); ++v_it)
    {

        mesh.point(*v_it) = mesh.point(*v_it)*scale;
    }
    bool ok = OpenMesh::IO::write_mesh(mesh, filename);
	return ok;
}



/**
 * @brief 将整数转换为指定长度的字符串
 *
 * @param num 要转换的整数
 * @param size 字符串长度
 * @param is_add0 是否在前面补零
 * @return 转换后的字符串
 */
std::string num2str(int num, const int size, bool is_add0)
{
    char s_id[10]={0};
    s_id[size] = '\0';
    int pos = 0;
    for (int i = size-1;i >= 0;--i)
    {
        s_id[i] = num % 10 + '0';
        num /= 10;
        if(!is_add0)
        {
            if(num==0)
            {
                pos = i;
                is_add0 = true;
            }
        }
    }
    std::string s_num(s_id);
    return s_num.substr(pos, size-pos);
}
