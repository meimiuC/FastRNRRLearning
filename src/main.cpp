/**
程序入口文件 - Fast RNRR（快速鲁棒非刚性配准）

  【文件作用】
   本文件是整个程序的入口点，负责：
   1. 解析命令行参数（源网格、目标网格、输出路径、可选的标记点文件）
   2. 设置配准算法的各项超参数
   3. 读取网格数据并进行归一化缩放
   4. 依次执行刚性配准（粗对齐）和非刚性配准（精细变形）
   5. 输出配准结果和耗时统计


  【程序整体执行流程】
    命令行输入 → 参数设置 → 读取网格 → 归一化缩放 →
    刚性配准初始化 → 刚性配准（ICP）→ 非刚性配准初始化 →
    非刚性配准（Welsch + L-BFGS）→ 输出结果
 */

// 改进方向：
// 使用Anderson替换L - BFGS，加快求解速度
// Sparse ，不再使用点对点的数据对齐
// 自适应参数调整，nu1参数的调整现在是制定了一个参数的
// 变形图细化，层次化
// GPU并行加速计算
// 初对齐改进

#include "NonRigidreg.h"
#include "tools/OmpHelper.h"
#include "tools/io_mesh.h"

int main(int argc, char **argv) {
  // =============================================
  // 第一步：声明变量并解析命令行参数
  // =============================================
  Mesh src_mesh;                 // 源网格（待变形的网格）
  Mesh tar_mesh;                 // 目标网格（变形的目标形状）
  std::string src_file;          // 源网格文件路径
  std::string tar_file;          // 目标网格文件路径
  std::string out_file, outpath; // 输出文件路径
  std::string landmark_file;     // 标记点文件路径（可选）
  RegParas paras; // 配准算法参数结构体（定义在 tools/tools.h 中），实例化结构体

  // 解析命令行参数：支持3个或4个参数
  // 是否可以通过指定刚性点，增加准确度？
  if (argc == 4) {
    // 模式1：<源网格> <目标网格> <输出路径>
    src_file = argv[1];
    tar_file = argv[2];
    outpath = argv[3];
  } else if (argc == 5) {
    // 模式2：<源网格> <目标网格> <输出路径> <标记点文件>
    // 标记点文件包含源网格和目标网格上已知对应关系的顶点索引对
    src_file = argv[1];
    tar_file = argv[2];
    outpath = argv[3];
    landmark_file = argv[4];
    paras.use_landmark = true; // 启用标记点约束
  } else {
    // 参数数量不正确，输出使用说明并退出
    std::cout << "Usage: <srcFile> <tarFile> <outPath>\n    or <srcFile> "
                 "<tarFile> <outPath> <landmarkFile>"
              << std::endl;
    exit(0);
  }

  // =============================================
  // 第二步：设置配准算法的超参数，定义在tools/tools.h文件的RegParas结构体中
  // =============================================
  paras.alpha = 100.0; // 光滑项权重：控制相邻节点变换的一致性
  paras.beta = 100.0;  // 正交项权重：约束变换矩阵接近旋转矩阵
  paras.gamma = 1e8;   // 标记点项权重：强制标记点对齐
  paras.uni_sample_radio =
      5.0; // 均匀采样半径比率：采样半径 = 平均边长 × 此比率

  paras.use_distance_reject = true;  // 启用基于距离的对应点剪枝
  paras.distance_threshold = 0.05;   // 距离阈值：超过此距离的对应点对将被剔除
  paras.use_normal_reject = false;   // 不启用基于法向量的对应点剪枝
  paras.normal_threshold = M_PI / 3; // 法向量夹角阈值（60度）
  paras.use_Dynamic_nu =
      true; // 启用动态 nu 参数（Welsch 函数的鲁棒性参数逐步缩小）
  paras.use_anderson = true;
  paras.anderson_m = 5;
  paras.use_lbfgs = false;
  paras.max_inner_iters = 1;
  paras.anderson_safeguard = false;
  paras.anderson_safeguard_ratio = 10.0;

  // 设置输出文件路径
  paras.out_gt_file = outpath + "_res.txt"; // 真实误差输出文件
  out_file = outpath + "res.obj";           // 配准结果网格文件

  // =============================================
  // 第三步：读取网格数据
  // =============================================
  // read_data 定义在 io_mesh.h 中，使用 OpenMesh 库读取网格文件
  // 支持 .obj, .ply, .off 等常见三维网格格式
  read_data(src_file, src_mesh);
  read_data(tar_file, tar_mesh);
  if (src_mesh.n_vertices() == 0 || tar_mesh.n_vertices() == 0)
    exit(0); // 网格为空则退出

  // 如果源网格和目标网格顶点数不同，则无法逐点计算真值误差
  // 这个问题比较关键，因为扫描数据和sw文件转化得来的数据顶点数不尽相同？
  if (src_mesh.n_vertices() != tar_mesh.n_vertices())
    paras.calc_gt_err = false;

  // 如果使用标记点，从文件中读取标记点对应关系
  if (paras.use_landmark)
    read_landmark(landmark_file.c_str(), paras.landmark_src,
                  paras.landmark_tar);

  // 对源网格和目标网格进行归一化缩放（除以包围盒对角线长度）
  // 使两个网格在统一的尺度下进行配准，返回缩放因子以便最终恢复尺度
  double scale = mesh_scaling(src_mesh, tar_mesh);

  // =============================================
  // 第四步：创建配准对象并执行配准流程
  // =============================================
  // NonRigidreg 继承自 Registration，实现了完整的非刚性配准算法
  NonRigidreg *reg;
  reg = new NonRigidreg;

  Timer time; // 计时器对象（定义在 OmpHelper.h 中）

  // --- 阶段1：刚性配准（粗对齐）---
  // 使用基于 SVD 的点对点 ICP 算法，通过旋转和平移将源网格粗略对齐到目标网格
  std::cout << "\nrigid registration to initial..." << std::endl;
  Timer::EventID time1 = time.get_time();
  reg->rigid_init(src_mesh, tar_mesh,
                  paras); // 初始化：构建KD树、查找初始对应关系
  reg->DoRigid();         // 执行刚性配准迭代
  Timer::EventID time2 = time.get_time();
  std::cout << "rgid registration... " << std::endl;

  // --- 阶段2：非刚性配准初始化 ---
  // 包括：节点采样（构建稀疏控制节点图）、初始化变换矩阵、构建权重矩阵
  std::cout << "non-rigid registration to initial..." << std::endl;
  Timer::EventID time3 = time.get_time();
  reg->Initialize(); // 调用 NonRigidreg::Initialize()
  Timer::EventID time4 = time.get_time();
  reg->pars_.non_rigid_init_time = time.elapsed_time(time3, time4);

  // --- 阶段3：非刚性配准（精细变形）---
  // 通过优化带有 Welsch 鲁棒核函数的能量函数，迭代求解每个控制节点的仿射变换
  // 使用 L-BFGS 拟牛顿法加速求解
  std::cout << "non-rigid registration... " << std::endl;
  reg->DoNonRigid(); // 调用 NonRigidreg::DoNonRigid()
  Timer::EventID time5 = time.get_time();

  // =============================================
  // 第五步：输出结果和耗时统计
  // =============================================
  std::cout << "Registration done!\nrigid_init time : "
            << time.elapsed_time(time1, time2)
            << " s \trigid-reg run time = " << time.elapsed_time(time2, time3)
            << " s \nnon-rigid init time = " << time.elapsed_time(time3, time4)
            << " s \tnon-rigid run time = " << time.elapsed_time(time4, time5)
            << " s\n"
            << std::endl;

  // 将配准结果写入文件（恢复原始缩放比例后保存）
  write_data(out_file.c_str(), src_mesh, scale);
  std::cout << "write result to " << out_file << std::endl;

  // 清理内存
  delete reg;

  return 0;
}
