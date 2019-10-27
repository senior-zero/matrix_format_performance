#include <cuda_runtime.h>
#include <unordered_map>
#include <optional>
#include <iostream>
#include <fstream>

#include "matrix_market_reader.h"
#include "resizable_gpu_memory.h"
#include "matrix_converter.h"

#include "gpu_matrix_multiplier.h"
#include "cpu_matrix_multiplier.h"

#include "cpp_itt.h"

#include "fmt/format.h"
#include "fmt/color.h"
#include "fmt/core.h"

using namespace std;

class time_printer
{
  double reference {};
  optional<double> parallel_reference;

  /// Settings
  const unsigned int time_width = 20;
  const unsigned int time_precision = 6;

public:
  explicit time_printer (
      double reference_time,
      std::optional<double> parallel_ref = nullopt)
   : reference (reference_time)
   , parallel_reference (move (parallel_ref))
  {
  }

  void add_time (double time, fmt::color color) const
  {
    fmt::print (fmt::fg (color), "{2:<{0}.{1}g}   ", time_width, time_precision, time);
  }

  void print_time (const std::string &label, double time) const
  {
    fmt::print (fmt::fg (fmt::color::yellow), "{0:<25}", label);
    fmt::print (":  ");
    add_time (time, fmt::color::white);
    add_time (speedup (time), fmt::color::green);
    if (parallel_reference)
      add_time (parallel_speedup (time), fmt::color::green_yellow);
    fmt::print ("\n");
  }

  double speedup (double time) const
  {
    return reference / time;
  }

  double parallel_speedup (double time) const
  {
    return *parallel_reference / time;
  }
};

template <typename data_type>
unordered_map<string, double> perform_measurement (const matrix_market::reader &reader, size_t free_memory)
{
  unordered_map<string, double> measurements;

  unique_ptr<csr_matrix_class<data_type>> csr_matrix;
  unique_ptr<ell_matrix_class<data_type>> ell_matrix;
  unique_ptr<coo_matrix_class<data_type>> coo_matrix;

  {
    cpp_itt::quiet_region region;

    csr_matrix = make_unique<csr_matrix_class<data_type>> (reader.matrix ());
    cout << "Complete converting to CSR" << endl;

    ell_matrix = make_unique<ell_matrix_class<data_type>> (*csr_matrix);
    cout << "Complete converting to ELL" << endl;

    coo_matrix = make_unique<coo_matrix_class<data_type>> (*csr_matrix);
    cout << "Complete converting to COO" << endl;

    if (   csr_matrix->get_matrix_size () * sizeof (double) > free_memory * 0.9
        || ell_matrix->get_matrix_size () * sizeof (double) > free_memory * 0.9)
      return {};
  }

  // CPU
  std::unique_ptr<data_type[]> reference_answer (new data_type[csr_matrix->meta.rows_count]);
  std::unique_ptr<data_type[]> cpu_y (new data_type[csr_matrix->meta.rows_count]);
  std::unique_ptr<data_type[]> x (new data_type[std::max (csr_matrix->meta.rows_count, csr_matrix->meta.cols_count)]);

  double cpu_naive_time {};

  {
    auto duration = cpp_itt::create_event_duration ("cpu_csr_spmv_single_thread_naive");
    cpu_naive_time = cpu_csr_spmv_single_thread_naive (*csr_matrix, x.get (), reference_answer.get ());
  }

  time_printer single_core_timer (cpu_naive_time);
  single_core_timer.print_time ("CPU CSR", cpu_naive_time);
  measurements["CPU CSR"] = cpu_naive_time;

  double cpu_parallel_naive_time {};

  {
    auto duration = cpp_itt::create_event_duration ("cpu_csr_spmv_multi_thread_naive");
    cpu_parallel_naive_time = cpu_csr_spmv_multi_thread_naive (*csr_matrix, x.get (), cpu_y.get ());
    single_core_timer.print_time ("CPU CSR Parallel", cpu_parallel_naive_time);
    measurements["CPU CSR Parallel"] = cpu_parallel_naive_time;
  }

  time_printer multi_core_timer (cpu_naive_time, cpu_parallel_naive_time);

  if (0)
  {
    auto duration = cpp_itt::create_event_duration ("cpu_ell_spmv_multi_thread_naive");
    auto cpu_parallel_naive_ell_time = cpu_ell_spmv_multi_thread_naive (*ell_matrix, x.get (), cpu_y.get ());
    cout << "CPU Parallel ELL: " << cpu_parallel_naive_ell_time << " (SSCPU = " << cpu_naive_time / cpu_parallel_naive_ell_time << ")" << endl;
  }

  if (0)
  {
    auto duration = cpp_itt::create_event_duration ("cpu_ell_spmv_multi_thread_naive");
    auto cpu_parallel_naive_ell_time = cpu_ell_spmv_multi_thread_avx2 (*ell_matrix, x.get (), cpu_y.get (), reference_answer.get ());
    cout << "CPU Parallel ELL (AVX2): " << cpu_parallel_naive_ell_time << " (SSCPU = " << cpu_naive_time / cpu_parallel_naive_ell_time << ")" << endl;
  }

  /// GPU Reusable memory
  resizable_gpu_memory<data_type> A, x_gpu, y;
  resizable_gpu_memory<unsigned int> col_ids, row_ptr;

  /// GPU
  {
    cpp_itt::quiet_region region;

    {
      auto gpu_time = gpu_csr_spmv<data_type> (*csr_matrix, A, col_ids, row_ptr, x_gpu, y, x.get (), reference_answer.get ());
      multi_core_timer.print_time ("GPU CSR", gpu_time);
      measurements["GPU CSR"] = gpu_time;
    }

    {
      auto gpu_time = gpu_csr_vector_spmv<data_type> (*csr_matrix, A, col_ids, row_ptr, x_gpu, y, x.get (), reference_answer.get ());
      multi_core_timer.print_time ("GPU CSR (vector)", gpu_time);
      measurements["GPU CSR (vector)"] = gpu_time;
    }

    {
      auto gpu_time = gpu_ell_spmv<data_type> (*ell_matrix, A, col_ids, x_gpu, y, x.get (), reference_answer.get ());
      multi_core_timer.print_time ("GPU ELL", gpu_time);
      measurements["GPU ELL"] = gpu_time;
    }

    {
      auto gpu_time = gpu_coo_spmv<data_type> (*coo_matrix, A, col_ids, row_ptr, x_gpu, y, x.get (), reference_answer.get ());
      multi_core_timer.print_time ("GPU COO", gpu_time);
      measurements["GPU COO"] = gpu_time;
    }

    // if (0)
    // {
    //   scoo_matrix_class scoo_matrix (*coo_matrix);
    // }

    resizable_gpu_memory<data_type> A_coo;
    resizable_gpu_memory<unsigned int> col_ids_coo;

    hybrid_matrix_class<data_type> hybrid_matrix (*csr_matrix);

    for (double percent = 0.0; percent <= 1.0; percent += 0.35)
    {
      hybrid_matrix.allocate(*csr_matrix, percent);

      std::string percent_str = std::to_string ((int)(percent * 100));

      {
        const string label = "GPU HYBRID " + percent_str;
        auto gpu_time = gpu_hybrid_spmv<data_type> (hybrid_matrix, A, A_coo, col_ids, col_ids_coo, row_ptr, x_gpu, y, x.get (), reference_answer.get ());
        multi_core_timer.print_time (label, gpu_time);
        measurements[label] = gpu_time;
      }

      {
        const string label = "GPU HYBRID ATOMIC " + percent_str;
        auto gpu_time = gpu_hybrid_atomic_spmv<data_type> (hybrid_matrix, A, A_coo, col_ids, col_ids_coo, row_ptr, x_gpu, y, x.get (), reference_answer.get ());
        multi_core_timer.print_time (label, gpu_time);
        measurements[label] = gpu_time;
      }

      {
        const string label = "GPU HYBRID CPU COO " + percent_str;
        resizable_gpu_memory<data_type> tmp;
        auto gpu_time = gpu_hybrid_cpu_coo_spmv<data_type> (hybrid_matrix, A, col_ids, x_gpu, y, tmp, cpu_y.get (), x.get (), reference_answer.get ());
        multi_core_timer.print_time (label, gpu_time);
        measurements[label] = gpu_time;
      }
    }
  }

  return measurements;
}

int main(int argc, char *argv[])
{
  if (argc != 2)
  {
    cerr << "Usage: " << argv[0] << " /path/to/mtx_list" << endl;
    return 1;
  }

  cudaSetDevice (0);

  size_t free_gpu_mem, total_gpu_mem;
  cudaMemGetInfo (&free_gpu_mem, &total_gpu_mem);

  ifstream list (argv[1]);

  string mtx;
  unordered_map<string, unordered_map<string, unordered_map<string, double>>> measurements;

  while (getline (list, mtx))
  {
    fmt::print ("Start loading matrix {}\n", mtx);

    ifstream is (mtx);
    matrix_market::reader reader (is);
    cout << "Complete loading" << endl;

    auto float_result = perform_measurement<float> (reader, free_gpu_mem);
    auto double_result = perform_measurement<double> (reader, free_gpu_mem);

    if (float_result.empty () || double_result.empty ())
      continue; // Don't store result for matrices that couldn't be computed on GPU
    measurements[mtx]["float"] = float_result;
    measurements[mtx]["double"] = double_result;
  }

  return 0;
}