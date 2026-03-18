# ==========================================================
# CMakeLists.txt - 项目构建配置文件
# ==========================================================
# 本文件是整个 Fast_RNRR（快速鲁棒非刚性配准）项目的 CMake 构建脚本。
# 它负责：
#   1. 指定所需的第三方库路径（Eigen3、OpenMesh、NanoFlann）
#   2. 设置编译器选项（C++14、O3优化、OpenMP并行）
#   3. 收集所有源文件并生成可执行文件
#   4. 链接所需的库文件
#   5. 部署数据文件夹
#
# 项目源文件结构：
#   src/main.cpp          - 程序入口，解析命令行参数并调度配准流程
#   src/Registration.h/cpp - 配准基类，提供刚性配准、对应点查找、剪枝等基础功能
#   src/NonRigidreg.h/cpp  - 非刚性配准子类，实现基于 Welsch 函数和 L-BFGS 的非刚性配准
#   src/tools/             - 工具库（类型定义、IO、采样器、KD树、计时器等）
#   src/geodesic/          - 精确测地距离计算库
# ==========================================================

#cmake_minimum_required(VERSION 2.8)
cmake_minimum_required(VERSION 3.10)  # 要求 CMake 最低版本 3.10
project(Fast_RNRR)                    # 定义项目名称为 Fast_RNRR

#--- CMake 配置：设置模块搜索路径
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# ==========================================================
# 1. 手动指定你的本地第三方库路径 (Windows 环境配置)
# ==========================================================
# Eigen3：C++ 线性代数库，提供矩阵运算、SVD分解、稀疏矩阵等核心数学功能
set(EIGEN3_INCLUDE_DIRS "C:/Libs/eigen-5.0.0")

# OpenMesh 11.0：三角网格处理库，用于读写网格文件、管理网格拓扑结构（顶点/面/边/半边）
set(OPENMESH_INCLUDE_DIRS "C:/Program Files/OpenMesh 11.0/include")
set(OPENMESH_LIB_DIRS "C:/Program Files/OpenMesh 11.0/lib")

# NanoFlann：基于 FLANN 的轻量级 KD 树库，用于快速最近邻搜索
set(NANOFLANN_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")
# ==========================================================

#--- 编译器配置
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")      # 启用 O3 级别优化
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14") # 使用 C++14 标准

# 定义 _USE_MATH_DEFINES 宏，使 Windows 下可以使用 M_PI 等数学常量
add_definitions(-D_USE_MATH_DEFINES)

#--- OpenMP 并行计算支持（Clang 编译器不支持）
# OpenMP 用于加速对应点查找、SVD分解、梯度计算等可并行化操作
find_package(OpenMP QUIET)
if(OPENMP_FOUND)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp")
endif()

#--- Eigen3 线性代数库（已注释自动查找，直接使用上面手动指定的路径）
# find_package(Eigen3 REQUIRED) 
include_directories(${EIGEN3_INCLUDE_DIRS})

# Intel MKL 加速支持（仅在使用 Intel 编译器时启用）
# MKL 可以显著加速 Eigen 的矩阵运算
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Intel")
  find_package(BLAS)
  include_directories(${BLAS_INCLUDE_DIRS})
  set(PROJECT_LIBS ${BLAS_LIBRARIES})
  add_definitions(-DEIGEN_USE_MKL_ALL)
endif()

#--- NanoFlann KD 树库（已注释自动查找，直接使用上面手动指定的路径）
# find_package(NanoFlann REQUIRED) 
include_directories(${NANOFLANN_INCLUDE_DIR})

#--- 构建可执行文件
# 添加源文件目录和 OpenMesh 库目录
include_directories("./src/")              # 源代码根目录
include_directories(${OPENMESH_INCLUDE_DIRS}) # OpenMesh 头文件
link_directories(${OPENMESH_LIB_DIRS})        # OpenMesh 库文件

# 收集所有源文件和头文件
# src/         - 主程序和配准算法核心代码
# src/geodesic/ - 测地距离计算模块
# src/tools/    - 工具类（IO、类型、采样器等）
file(GLOB SOURCES "./src/*.cpp" "./src/geodesic/*.cpp" "./src/tools/*.cpp")
file(GLOB HEADERS "./src/*.h" "./src/geodesic/*.h" "./src/tools/*.h")
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

#--- 链接 OpenMesh 动态库
# OpenMeshCore：网格核心数据结构
# OpenMeshTools：网格读写等工具
target_link_libraries(${PROJECT_NAME}
    OpenMeshCore OpenMeshTools)

# 部署数据文件夹到构建目录
# data/ 目录包含测试用的源网格和目标网格文件
if(APPLE OR UNIX)
    # macOS/Linux：创建符号链接指向源数据目录
    execute_process(COMMAND ln -f -s ${CMAKE_SOURCE_DIR}/data WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    # 在 QtCreator 中显示数据文件
    add_custom_target(data SOURCES ${CMAKE_SOURCE_DIR}/data)
else()
    # Windows：直接复制数据文件夹到构建目录
    file(COPY ${CMAKE_SOURCE_DIR}/data DESTINATION ${PROJECT_BINARY_DIR})
endif()