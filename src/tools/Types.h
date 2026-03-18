// typedef <origin type> <new type>;
// typedef used in define eigen matrix and vector types
// original:Eigen::Matrix<float,3,3,Eigen::RowMajor>
// use 'using' to define type aliases

/**
 * @file Types.h
 * @brief 全局类型定义文件 - 定义整个项目使用的基础数据类型
 *
 * 【文件作用】
 *   定义了整个项目中使用的所有基础类型，是最底层的头文件，被几乎所有其他文件包含。
 *   主要包括：
 *   1. 标量类型 Scalar（可选 float/double）
 *   2. Eigen 矩阵和向量类型别名（Vector3, Matrix33, MatrixXX 等）
 *   3. OpenMesh 网格类型定义（Mesh, Vec3）
 *   4. OpenMesh 的自定义 Traits（顶点/面/边的额外属性）
 *   5. 四阶张量类 Matrix3333, Matrix2222
 *   6. 类型转换工具函数
 *
 * 【与其他文件的关系】
 *   - 被 tools.h, nanoflann.h, nodeSampler.h 等所有工具文件包含
 *   - 被 Registration.h, NonRigidreg.h 等核心算法文件间接包含
 *   - 提供了项目中统一的数据类型，确保类型一致性
 *
 * 【OpenMesh Traits 说明】
 *   TriTraits 为 OpenMesh 网格添加了额外的顶点属性：
 *   - geodesic_distance：测地距离，用于节点采样
 *   - state：顶点状态（OUTSIDE/FRONT/INSIDE），用于测地算法的 Dijkstra 式传播
 *   - incident_face, incident_point：测地距离计算中的入射信息
 *   - saddle_or_boundary：是否为鞍点或边界点
 */

#ifndef TYPES_H
#define TYPES_H

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <OpenMesh/Core/Geometry/VectorT.hh>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Core/IO/MeshIO.hh>

#include <iostream>
#include <fstream>
#include <iomanip>

#include <stdlib.h>
#include <stdio.h>
#include <cstdio>

#include <algorithm>
#include <math.h>
#include <cmath>

#include <vector>
#include <list>
#include <set>
#include <queue>
#include <map>
#include <string>

#include <time.h>
#include <assert.h>
#include <cstddef>
#include <limits>

// ===== 标量类型定义 =====
// 通过宏控制使用 float 或 double。
// 使用 float 可以节省内存并加速计算，但精度较低。
#define USE_FLOAT_SCALAR

#ifdef USE_FLOAT_SCALAR
typedef float Scalar;    // 使用单精度浮点数
#else
typedef double Scalar;   // 使用双精度浮点数
#endif

// ===== 稀疏矩阵类型 =====
typedef Eigen::SparseMatrix<Scalar, Eigen::ColMajor> ColMajorSparseMatrix;  // 列主序稀疏矩阵
typedef Eigen::SparseMatrix<Scalar, Eigen::RowMajor> RowMajorSparseMatrix;  // 行主序稀疏矩阵
//typedef Eigen::Triplet<Scalar> Triplet;
//typedef std::vector<std::pair<int, int>> VPairs;


#ifdef EIGEN_DONT_ALIGN
#define EIGEN_ALIGNMENT Eigen::DontAlign
#else
#define EIGEN_ALIGNMENT Eigen::AutoAlign
#endif


// ===== Eigen 矩阵和向量类型别名 =====
// MatrixT 是模板别名，封装 Eigen::Matrix 的标量类型和对齐方式
template < int Rows, int Cols, int Options = (Eigen::ColMajor | EIGEN_ALIGNMENT) >
using MatrixT = Eigen::Matrix<Scalar, Rows, Cols, Options>;
typedef MatrixT<2, 1> Vector2;								///< 2维列向量
typedef MatrixT<2, 2> Matrix22;								///< 2×2 矩阵
typedef MatrixT<2, 3> Matrix23;								///< 2×3 矩阵
typedef MatrixT<3, 1> Vector3;								///< 3维列向量（广泛用于三维坐标、法向量等）
typedef MatrixT<3, 2> Matrix32;								///< 3×2 矩阵
typedef MatrixT<3, 3> Matrix33;								///< 3×3 矩阵（旋转矩阵、协方差矩阵等）
typedef MatrixT<3, 4> Matrix34;								///< 3×4 矩阵
typedef MatrixT<4, 1> Vector4;								///< 4维列向量
typedef MatrixT<4, 4> Matrix44;								///< 4×4 矩阵（仿射变换矩阵）
typedef MatrixT<4, Eigen::Dynamic> Matrix4X;				///< 4×n 动态矩阵
typedef MatrixT<3, Eigen::Dynamic> Matrix3X;				///< 3×n 动态矩阵（点集矩阵，每列一个点）
typedef MatrixT<Eigen::Dynamic, 3> MatrixX3;				///< n×3 动态矩阵
typedef MatrixT<2, Eigen::Dynamic> Matrix2X;				///< 2×n 动态矩阵
typedef MatrixT<Eigen::Dynamic, 2> MatrixX2;				///< n×2 动态矩阵
typedef MatrixT<Eigen::Dynamic, 1> VectorX;					///< n维列向量（动态大小）
typedef MatrixT<Eigen::Dynamic, Eigen::Dynamic> MatrixXX;	///< n×m 动态矩阵（通用矩阵）
typedef Eigen::Matrix<Scalar, 12, 12, 0, 12, 12> EigenMatrix12;  ///< 12×12 固定大小矩阵
typedef Eigen::Transform<Scalar, 3, Eigen::Affine> Affine3;      ///< 3D 仿射变换（4×4 矩阵：旋转+平移+缩放）

// ===== 四元数和角轴旋转 =====
typedef Eigen::AngleAxis<Scalar> EigenAngleAxis;                         ///< 角轴旋转表示
typedef Eigen::Quaternion<Scalar, Eigen::DontAlign> EigenQuaternion;     ///< 四元数旋转表示

// ===== 向量类型转换工具 =====
/**
 * @brief 将任意 3D 向量类型转换为 Eigen::Vector3
 */
template<typename Vec_T>
inline Vector3 to_eigen_vec3(const Vec_T &vec)
{
    return Vector3(vec[0], vec[1], vec[2]);
}

/**
 * @brief 将 Eigen::Vector3 转换为指定的 3D 向量类型
 */
template<typename Vec_T>
inline Vec_T from_eigen_vec3(const Vector3 &vec)
{
    Vec_T v;
    v[0] = vec(0);
    v[1] = vec(1);
    v[2] = vec(2);
    return v;
}

// ===== 顶点状态枚举（用于测地距离计算） =====
// 在测地距离的 Dijkstra 式传播算法中，标记顶点的处理状态
enum VertexState
{
    OUTSIDE,  // 未处理：不在传播前沿内
    FRONT,    // 前沿：当前正在处理的边界
    INSIDE    // 已处理：测地距离已确定
};

/**
 * @brief OpenMesh 自定义 Traits，为网格元素添加额外属性
 *
 * 顶点属性：
 *   - geodesic_distance：从源点到该顶点的测地距离
 *   - state：当前顶点的传播状态
 *   - incident_face, incident_point：测地距离计算中的辅助信息
 *   - saddle_or_boundary：是否为鞍点/边界点
 *
 * 面属性：
 *   - corner_angles：三角形三个角的角度
 *
 * 边属性：
 *   - length：边长
 */
struct TriTraits : public OpenMesh::DefaultTraits {
    #ifdef USE_FLOAT_SCALAR
    typedef OpenMesh::Vec3f Point;
    typedef OpenMesh::Vec3f Normal;
#else
    typedef OpenMesh::Vec3d Point;
    typedef OpenMesh::Vec3d Normal;
#endif
    typedef OpenMesh::Vec4f Color;



    VertexAttributes(OpenMesh::Attributes::Status);
    FaceAttributes(OpenMesh::Attributes::Status);
    EdgeAttributes(OpenMesh::Attributes::Status);
    HalfedgeAttributes(OpenMesh::Attributes::Status);

    VertexTraits
    {
        VertexT() : geodesic_distance(1e100), state(OUTSIDE),incident_point(0.0),
          saddle_or_boundary(false)
        {};
    public:
        Scalar geodesic_distance;
        VertexState state;
        OpenMesh::FaceHandle incident_face;
        Scalar incident_point;
        bool saddle_or_boundary;
    };
    FaceTraits
    {
        FaceT() : corner_angles(0,0,0)
        {};
        public:
        Vector3 corner_angles;
    };
    EdgeTraits
    {
        EdgeT() : length(0.0)
        {};
    public:
        Scalar length;
    };
};
// ===== 网格类型定义 =====
// Mesh：基于 OpenMesh 的三角网格类型，带有自定义 Traits
typedef OpenMesh::TriMesh_ArrayKernelT<TriTraits> Mesh;
// Vec3：OpenMesh 的 3D 向量类型，与 Scalar 类型一致
#ifdef USE_FLOAT_SCALAR
typedef OpenMesh::Vec3f Vec3;
#else
typedef OpenMesh::Vec3d Vec3;
#endif


/**
 * @class Matrix3333
 * @brief 3×3 的四阶张量：每个元素是一个 3×3 矩阵
 *
 * 用于表示四阶弹性张量等高阶数学对象。
 * 支持加减乘、转置、缩并等运算。
 */
class Matrix3333
{
public:
    Matrix3333();
    Matrix3333(const Matrix3333& other);
    ~Matrix3333() {}

    void SetZero(); // [0 0 0; 0 0 0; 0 0 0]; 0 = 3x3 zeros
    void SetIdentity(); //[I 0 0; 0 I 0; 0 0 I]; 0 = 3x3 zeros, I = 3x3 identity

                        // operators
    Matrix33& operator() (int row, int col);
    Matrix3333 operator+ (const Matrix3333& plus);
    Matrix3333 operator- (const Matrix3333& minus);
    Matrix3333 operator* (const Matrix33& multi);
    friend Matrix3333 operator* (const Matrix33& multi1, Matrix3333& multi2);
    Matrix3333 operator* (Scalar multi);
    friend Matrix3333 operator* (Scalar multi1, Matrix3333& multi2);
    Matrix3333 transpose();
    Matrix33 Contract(const Matrix33& multi); // this operator is commutative
    Matrix3333 Contract(Matrix3333& multi);

    //protected:

    Matrix33 mat[3][3];
};

class Matrix2222 // 2x2 matrix: each element is a 2x2 matrix
{
public:
    Matrix2222();
    Matrix2222(const Matrix2222& other);
    ~Matrix2222() {}

    void SetZero(); // [0 0; 0 0]; 0 = 2x2 zeros
    void SetIdentity(); //[I 0; 0 I;]; 0 = 2x2 zeros, I = 2x2 identity

                        // operators and basic functions
    Matrix22& operator() (int row, int col);
    Matrix2222 operator+ (const Matrix2222& plus);
    Matrix2222 operator- (const Matrix2222& minus);
    Matrix2222 operator* (const Matrix22& multi);
    friend Matrix2222 operator* (const Matrix22& multi1, Matrix2222& multi2);
    Matrix2222 operator* (Scalar multi);
    friend Matrix2222 operator* (Scalar multi1, Matrix2222& multi2);
    Matrix2222 transpose();
    Matrix22 Contract(const Matrix22& multi); // this operator is commutative
    Matrix2222 Contract(Matrix2222& multi);

protected:

    Matrix22 mat[2][2];
};

// ===== 张量积运算 =====
// dst = src1 ⊗ src2 （Kronecker 积 / 直积）
void directProduct(Matrix3333& dst, const Matrix33& src1, const Matrix33& src2);
void directProduct(Matrix2222& dst, const Matrix22& src1, const Matrix22& src2);
#endif // TYPES_H
///////////////////////////////////////////////////////////////////////////////
