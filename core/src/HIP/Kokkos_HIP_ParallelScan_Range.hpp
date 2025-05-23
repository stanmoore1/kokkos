//@HEADER
// ************************************************************************
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
// Part of Kokkos, under the Apache License v2.0 with LLVM Exceptions.
// See https://kokkos.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//@HEADER

#ifndef KOKKOS_HIP_PARALLEL_SCAN_RANGE_HPP
#define KOKKOS_HIP_PARALLEL_SCAN_RANGE_HPP

#include <Kokkos_Parallel.hpp>

#include <HIP/Kokkos_HIP_BlockSize_Deduction.hpp>
#include <HIP/Kokkos_HIP_KernelLaunch.hpp>
#include <HIP/Kokkos_HIP_ReduceScan.hpp>

namespace Kokkos {
namespace Impl {

template <class FunctorType, class ValueType, class... Traits>
class ParallelScanHIPBase {
 public:
  using Policy = Kokkos::RangePolicy<Traits...>;

 protected:
  using Member       = typename Policy::member_type;
  using WorkTag      = typename Policy::work_tag;
  using WorkRange    = typename Policy::WorkRange;
  using LaunchBounds = typename Policy::launch_bounds;

  using Analysis =
      Kokkos::Impl::FunctorAnalysis<FunctorPatternInterface::SCAN, Policy,
                                    FunctorType, ValueType>;

 public:
  using value_type     = typename Analysis::value_type;
  using pointer_type   = typename Analysis::pointer_type;
  using reference_type = typename Analysis::reference_type;
  using functor_type   = FunctorType;
  using size_type      = HIP::size_type;
  using index_type     = typename Policy::index_type;
  // Conditionally set word_size_type to int16_t or int8_t if value_type is
  // smaller than int32_t (Kokkos::HIP::size_type)
  // word_size_type is used to determine the word count, shared memory buffer
  // size, and global memory buffer size before the scan is performed.
  // Within the scan, the word count is recomputed based on word_size_type
  // and when calculating indexes into the shared/global memory buffers for
  // performing the scan, word_size_type is used again.
  // For scalars > 4 bytes in size, indexing into shared/global memory relies
  // on the block and grid dimensions to ensure that we index at the correct
  // offset rather than at every 4 byte word; such that, when the join is
  // performed, we have the correct data that was copied over in chunks of 4
  // bytes.
  using word_size_type = std::conditional_t<
      sizeof(value_type) < sizeof(size_type),
      std::conditional_t<sizeof(value_type) == 2, int16_t, int8_t>, size_type>;

 protected:
  // Algorithmic constraints:
  //  (a) blockDim.y is a power of two
  //  (b) blockDim.x == blockDim.z == 1
  //  (c) gridDim.x  <= blockDim.y * blockDim.y
  //  (d) gridDim.y  == gridDim.z == 1

  const CombinedFunctorReducer<FunctorType, typename Analysis::Reducer>
      m_functor_reducer;
  const Policy m_policy;
  const pointer_type m_result_ptr;
  const bool m_result_ptr_device_accessible;
  word_size_type* m_scratch_space = nullptr;
  size_type* m_scratch_flags      = nullptr;
  size_type m_final               = false;
  int m_grid_x                    = 0;

 private:
  template <class TagType>
  __device__ inline std::enable_if_t<std::is_void_v<TagType>> exec_range(
      const Member& i, reference_type update, const bool final_result) const {
    m_functor_reducer.get_functor()(i, update, final_result);
  }

  template <class TagType>
  __device__ inline std::enable_if_t<!std::is_void_v<TagType>> exec_range(
      const Member& i, reference_type update, const bool final_result) const {
    m_functor_reducer.get_functor()(TagType(), i, update, final_result);
  }

  //----------------------------------------

  __device__ inline void initial() const {
    const typename Analysis::Reducer& final_reducer =
        m_functor_reducer.get_reducer();

    const integral_nonzero_constant<word_size_type, Analysis::StaticValueSize /
                                                        sizeof(word_size_type)>
        word_count(final_reducer.value_size() / sizeof(word_size_type));

    pointer_type const shared_value = reinterpret_cast<pointer_type>(
        kokkos_impl_hip_shared_memory<word_size_type>() +
        word_count.value * threadIdx.y);

    final_reducer.init(shared_value);

    // Number of blocks is bounded so that the reduction can be limited to two
    // passes. Each thread block is given an approximately equal amount of work
    // to perform. Accumulate the values for this block. The accumulation
    // ordering does not match the final pass, but is arithmetically equivalent.

    const WorkRange range(m_policy, blockIdx.x, gridDim.x);

    for (Member iwork = range.begin() + threadIdx.y, iwork_end = range.end();
         iwork < iwork_end; iwork += blockDim.y) {
      this->template exec_range<WorkTag>(
          iwork, final_reducer.reference(shared_value), false);
    }

    // Reduce and scan, writing out scan of blocks' totals and block-groups'
    // totals. Blocks' scan values are written to 'blockIdx.x' location.
    // Block-groups' scan values are at: i = ( j * blockDim.y - 1 ) for i <
    // gridDim.x
    hip_single_inter_block_reduce_scan<true>(
        final_reducer, blockIdx.x, gridDim.x,
        kokkos_impl_hip_shared_memory<word_size_type>(), m_scratch_space,
        m_scratch_flags);
  }

  //----------------------------------------

  __device__ inline void final() const {
    const typename Analysis::Reducer& final_reducer =
        m_functor_reducer.get_reducer();

    const integral_nonzero_constant<word_size_type, Analysis::StaticValueSize /
                                                        sizeof(word_size_type)>
        word_count(final_reducer.value_size() / sizeof(word_size_type));

    // Use shared memory as an exclusive scan: { 0 , value[0] , value[1] ,
    // value[2] , ... }
    word_size_type* const shared_data =
        kokkos_impl_hip_shared_memory<word_size_type>();
    word_size_type* const shared_prefix =
        shared_data + word_count.value * threadIdx.y;
    word_size_type* const shared_accum =
        shared_data + word_count.value * (blockDim.y + 1);

    // Starting value for this thread block is the previous block's total.
    if (blockIdx.x) {
      word_size_type* const block_total =
          m_scratch_space + word_count.value * (blockIdx.x - 1);
      for (unsigned i = threadIdx.y; i < word_count.value; ++i) {
        shared_accum[i] = block_total[i];
      }
    } else if (0 == threadIdx.y) {
      final_reducer.init(reinterpret_cast<pointer_type>(shared_accum));
    }

    const WorkRange range(m_policy, blockIdx.x, gridDim.x);

    for (typename Policy::member_type iwork_base = range.begin();
         iwork_base < range.end(); iwork_base += blockDim.y) {
      const typename Policy::member_type iwork = iwork_base + threadIdx.y;

      __syncthreads();  // Don't overwrite previous iteration values until they
                        // are used

      final_reducer.init(
          reinterpret_cast<pointer_type>(shared_prefix + word_count.value));

      // Copy previous block's accumulation total into thread[0] prefix and
      // inclusive scan value of this block
      for (unsigned i = threadIdx.y; i < word_count.value; ++i) {
        shared_data[i + word_count.value] = shared_data[i] = shared_accum[i];
      }

      // Make sure the write is seen by all threads
      __threadfence_block();

      // Call functor to accumulate inclusive scan value for this work item
      const bool doWork = (iwork < range.end());
      if (doWork) {
        this->template exec_range<WorkTag>(
            iwork,
            final_reducer.reference(reinterpret_cast<pointer_type>(
                shared_prefix + word_count.value)),
            false);
      }

      // Scan block values into locations shared_data[1..blockDim.y]
      hip_intra_block_reduce_scan<true>(
          final_reducer,
          typename Analysis::pointer_type(shared_data + word_count.value));

      {
        word_size_type* const block_total =
            shared_data + word_count.value * blockDim.y;
        for (unsigned i = threadIdx.y; i < word_count.value; ++i) {
          shared_accum[i] = block_total[i];
        }
      }

      // Call functor with exclusive scan value
      if (doWork) {
        this->template exec_range<WorkTag>(
            iwork,
            final_reducer.reference(
                reinterpret_cast<pointer_type>(shared_prefix)),
            true);
      }
      if (iwork + 1 == m_policy.end() && m_policy.end() == range.end() &&
          m_result_ptr_device_accessible)
        *m_result_ptr = *reinterpret_cast<pointer_type>(shared_prefix);
    }
  }

 public:
  //----------------------------------------

  __device__ inline void operator()() const {
    if (!m_final) {
      initial();
    } else {
      final();
    }
  }

  inline void impl_execute(int block_size) {
    const index_type nwork = m_policy.end() - m_policy.begin();
    if (nwork) {
      // FIXME_HIP we cannot choose it larger for large work sizes to work
      // correctly, the unit tests fail with wrong results
      const int gridMaxComputeCapability_2x = 0x01fff;

      const int grid_max =
          std::min(block_size * block_size, gridMaxComputeCapability_2x);

      // At most 'max_grid' blocks:
      const int max_grid =
          std::min<int>(grid_max, (nwork + block_size - 1) / block_size);

      // How much work per block:
      const int work_per_block = (nwork + max_grid - 1) / max_grid;

      // How many block are really needed for this much work:
      m_grid_x = (nwork + work_per_block - 1) / work_per_block;

      const typename Analysis::Reducer& final_reducer =
          m_functor_reducer.get_reducer();
      m_scratch_space =
          reinterpret_cast<word_size_type*>(Impl::hip_internal_scratch_space(
              m_policy.space(), final_reducer.value_size() * m_grid_x));
      m_scratch_flags = Impl::hip_internal_scratch_flags(m_policy.space(),
                                                         sizeof(size_type) * 1);

      dim3 grid(m_grid_x, 1, 1);
      dim3 block(1, block_size, 1);  // REQUIRED DIMENSIONS ( 1 , N , 1 )
      const int shmem = final_reducer.value_size() * (block_size + 2);

      m_final = false;
      // these ones are OK to be just the base because the specializations
      // do not modify the kernel at all
      Impl::hip_parallel_launch<ParallelScanHIPBase, LaunchBounds>(
          *this, grid, block, shmem,
          m_policy.space().impl_internal_space_instance(),
          false);  // copy to device and execute

      m_final = true;
      Impl::hip_parallel_launch<ParallelScanHIPBase, LaunchBounds>(
          *this, grid, block, shmem,
          m_policy.space().impl_internal_space_instance(),
          false);  // copy to device and execute
    }
  }

  ParallelScanHIPBase(const FunctorType& arg_functor, const Policy& arg_policy,
                      pointer_type arg_result_ptr,
                      bool arg_result_ptr_device_accessible)
      : m_functor_reducer(arg_functor, typename Analysis::Reducer{arg_functor}),
        m_policy(arg_policy),
        m_result_ptr(arg_result_ptr),
        m_result_ptr_device_accessible(arg_result_ptr_device_accessible) {}
};

template <class FunctorType, class... Traits>
class ParallelScan<FunctorType, Kokkos::RangePolicy<Traits...>, HIP>
    : public ParallelScanHIPBase<FunctorType, void, Traits...> {
 public:
  using Base = ParallelScanHIPBase<FunctorType, void, Traits...>;
  using Base::operator();

  inline void execute() {
    const int block_size = static_cast<int>(
        local_block_size(Base::m_functor_reducer.get_functor()));
    if (block_size == 0) {
      Kokkos::Impl::throw_runtime_exception(
          std::string("Kokkos::Impl::ParallelScan< HIP > could not find a "
                      "valid execution configuration."));
    }

    Base::impl_execute(block_size);
  }

  ParallelScan(const FunctorType& arg_functor,
               const typename Base::Policy& arg_policy)
      : Base(arg_functor, arg_policy, nullptr, false) {}

  inline unsigned local_block_size(const FunctorType& f) {
    // blockDim.y must be power of two = 128 (2 warps) or 256 (4 warps) or
    // 512 (8 warps) gridDim.x <= blockDim.y * blockDim.y

    const auto& instance =
        Base::m_policy.space().impl_internal_space_instance();
    auto shmem_functor = [&f](unsigned n) {
      return hip_single_inter_block_reduce_scan_shmem<
          true, typename Base::WorkTag, void>(f, n);
    };
    using DriverType = ParallelScan<FunctorType, typename Base::Policy, HIP>;
    return Impl::hip_get_preferred_blocksize<DriverType,
                                             typename Base::LaunchBounds>(
        instance, shmem_functor);
  }
};

//----------------------------------------------------------------------------

template <class FunctorType, class ReturnType, class... Traits>
class ParallelScanWithTotal<FunctorType, Kokkos::RangePolicy<Traits...>,
                            ReturnType, HIP>
    : public ParallelScanHIPBase<FunctorType, ReturnType, Traits...> {
 public:
  using Base = ParallelScanHIPBase<FunctorType, ReturnType, Traits...>;
  using Base::operator();

  inline void execute() {
    const int block_size = static_cast<int>(
        local_block_size(Base::m_functor_reducer.get_functor()));
    if (block_size == 0) {
      Kokkos::Impl::throw_runtime_exception(
          std::string("Kokkos::Impl::ParallelScan< HIP > could not find a "
                      "valid execution configuration."));
    }

    Base::impl_execute(block_size);

    const auto nwork = Base::m_policy.end() - Base::m_policy.begin();
    if (nwork && !Base::m_result_ptr_device_accessible) {
      const int size =
          Base::Analysis::value_size(Base::m_functor_reducer.get_functor());
      DeepCopy<HostSpace, HIPSpace, HIP>(
          Base::m_policy.space(), Base::m_result_ptr,
          Base::m_scratch_space + (Base::m_grid_x - 1) * size /
                                      sizeof(typename Base::word_size_type),
          size);
    }
  }

  template <class ViewType>
  ParallelScanWithTotal(const FunctorType& arg_functor,
                        const typename Base::Policy& arg_policy,
                        const ViewType& arg_result_view)
      : Base(arg_functor, arg_policy, arg_result_view.data(),
             MemorySpaceAccess<HIPSpace,
                               typename ViewType::memory_space>::accessible) {}

  inline unsigned local_block_size(const FunctorType& f) {
    // blockDim.y must be power of two = 128 (2 warps) or 256 (4 warps) or
    // 512 (8 warps) gridDim.x <= blockDim.y * blockDim.y

    const auto& instance =
        Base::m_policy.space().impl_internal_space_instance();
    auto shmem_functor = [&f](unsigned n) {
      return hip_single_inter_block_reduce_scan_shmem<
          true, typename Base::WorkTag, ReturnType>(f, n);
    };
    using DriverType = ParallelScanWithTotal<FunctorType, typename Base::Policy,
                                             ReturnType, HIP>;
    return hip_get_preferred_blocksize<DriverType, typename Base::LaunchBounds>(
        instance, shmem_functor);
  }
};

}  // namespace Impl
}  // namespace Kokkos

#endif
