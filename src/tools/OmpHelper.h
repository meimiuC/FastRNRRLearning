/**
 * @file OmpHelper.h
 * @brief OpenMP 并行计算辅助和计时器工具
 *
 * 【文件作用】
 *   1. 定义 OpenMP 并行指令的跨平台宏（MSVC 和 GCC/Clang 语法不同）
 *   2. 提供 Timer 计时器类，用于精确测量算法各阶段的耗时
 *
 * 【与其他文件的关系】
 *   - 被 Registration.h 包含：提供 OMP 宏
 *   - 被 main.cpp 包含：使用 Timer 类测量配准耗时
 *   - 在整个项目的 #pragma omp parallel for 中使用
 *
 * 【OpenMP 使用说明】
 *   项目中广泛使用 OpenMP 并行化以下操作：
 *   - 最近点搜索（FindClosestPoints）
 *   - SVD 分解（update_R, sample_energy）
 *   - 梯度计算和矩阵运算
 *   - 刚性变换应用（DoRigid）
 */

#ifndef OMPHELPER_H_
#define OMPHELPER_H_

#define USE_OPENMP  // 启用 OpenMP 并行计算

#ifdef USE_OPENMP
#include <omp.h>
// MSVC 和 GCC/Clang 的 pragma 语法不同，需要分别处理
#ifdef USE_MSVC
#define OMP_PARALLEL __pragma(omp parallel)
#define OMP_FOR __pragma(omp for)
#define OMP_SINGLE __pragma(omp single)
#define OMP_SECTIONS __pragma(omp sections)
#define OMP_SECTION __pragma(omp section)
#else
#define OMP_PARALLEL _Pragma("omp parallel")
#define OMP_FOR _Pragma("omp for")
#define OMP_SINGLE _Pragma("omp single")
#define OMP_SECTIONS _Pragma("omp sections")
#define OMP_SECTION _Pragma("omp section")
#endif
#else
// 未启用 OpenMP 时，所有宏为空
#include <ctime>
#define OMP_PARALLEL
#define OMP_FOR
#define OMP_SINGLE
#define OMP_SECTIONS
#define OMP_SECTION
#endif

#include <cassert>
#include <vector>

/**
 * @class Timer
 * @brief 高精度计时器，用于测量算法各阶段的耗时
 *
 * 使用方法：
 *   Timer timer;
 *   Timer::EventID t1 = timer.get_time();  // 记录起始时间
 *   // ... 执行操作 ...
 *   Timer::EventID t2 = timer.get_time();  // 记录结束时间
 *   double elapsed = timer.elapsed_time(t1, t2);  // 计算耗时（秒）
 *
 * 当启用 OpenMP 时使用 omp_get_wtime()（高精度墙钟时间）；
 * 否则使用 clock()（CPU 时间）。
 */
class Timer
{
public:
	typedef int EventID;  // 时间事件ID

	/** @brief 记录当前时间点，返回事件ID */
	EventID get_time()
	{
		EventID id = time_values_.size();

#ifdef USE_OPENMP
		time_values_.push_back(omp_get_wtime());   // 使用 OpenMP 的高精度计时
#else
		time_values_.push_back(clock());             // 使用标准库计时
#endif

		return id;
	}

	/**
	 * @brief 计算两个时间事件之间的耗时
	 * @param event1 起始事件ID
	 * @param event2 结束事件ID
	 * @return 耗时（秒）
	 */
	double elapsed_time(EventID event1, EventID event2)
	{
		assert(event1 >= 0 && event1 < static_cast<EventID>(time_values_.size()));
		assert(event2 >= 0 && event2 < static_cast<EventID>(time_values_.size()));

#ifdef USE_OPENMP
		return time_values_[event2] - time_values_[event1];
#else
		return double(time_values_[event2] - time_values_[event1]) / CLOCKS_PER_SEC;
#endif
	}

	/** @brief 清空所有时间记录 */
	void reset()
	{
		time_values_.clear();
	}

private:
#ifdef USE_OPENMP
	std::vector<double> time_values_;    // OpenMP 模式：存储 double 类型的墙钟时间
#else
	std::vector<clock_t> time_values_;   // 标准模式：存储 clock_t 类型的 CPU 时间
#endif
};

#endif /* OMPHELPER_H_ */
