/**
 * @file
 *
 * @brief Contains the @ref launch
 *
 * @note Launch configurations are  used mostly in @ref kernel_launch.hpp . 
 */

#pragma once
#ifndef CUDA_API_WRAPPERS_LAUNCH_CONFIG_BUILDER_CUH_
#define CUDA_API_WRAPPERS_LAUNCH_CONFIG_BUILDER_CUH_

#include "launch_configuration.hpp"
#include "kernel.hpp"
#include "device.hpp"
#include "types.hpp"

namespace cuda {

namespace grid {

namespace detail_ {

inline dimension_t div_rounding_up(overall_dimension_t dividend, block_dimension_t divisor)
{
	dimension_t quotient = static_cast<dimension_t>(dividend / divisor);
		// It is up to the caller to ensure we don't overflow the dimension_t type
	return (divisor * quotient == dividend) ? quotient : quotient + 1;
}

inline dimensions_t div_rounding_up(overall_dimensions_t overall_dims, block_dimensions_t block_dims)
{
	return {
		div_rounding_up(overall_dims.x, block_dims.x),
		div_rounding_up(overall_dims.y, block_dims.y),
		div_rounding_up(overall_dims.z, block_dims.z)
	};
}

// Note: We're not implementing a grid-to-block rounding up here, since - currently -
// block_dimensions_t is the same as grid_dimensions_t.

} // namespace detail_

} // namespace grid

namespace detail_ {

static void validate_all_dimension_compatibility(
	grid::block_dimensions_t   block,
	grid::dimensions_t         grid,
	grid::overall_dimensions_t overall)
{
	if (grid * block != overall) {
		throw ::std::invalid_argument("specified block, grid and overall dimensions do not agree");
	}
}

} // namespace detail_

class launch_config_builder_t {
public:
	void resolve_dimensions()  {
		grid::composite_dimensions_t cd = get_composite_dimensions();
		dimensions_.block = cd.block;
		dimensions_.grid = cd.grid;
		if (not dimensions_.overall) {
			dimensions_.overall = cd.grid * cd.block;
		}
	}

protected:
	memory::shared::size_t  get_dynamic_shared_memory_size(grid::block_dimensions_t block_dims) const
	{
		return static_cast<memory::shared::size_t>((dynamic_shared_memory_size_determiner_ == nullptr) ?
			dynamic_shared_memory_size_ :
			dynamic_shared_memory_size_determiner_(static_cast<int>(block_dims.volume())));
			// Q: Why the need for type conversion?
			// A: MSVC is being a bit finicky here for some reason
	}

#ifndef NDEBUG
	grid::composite_dimensions_t get_unvalidated_composite_dimensions() const noexcept(false)
#else
	grid::composite_dimensions_t get_composite_dimensions() const noexcept(false)
#endif
	{
		grid::composite_dimensions_t result;
		if (saturate_with_active_blocks_) {
#if CUDA_VERSION >= 10000
			if (use_min_params_for_max_occupancy_) {
				throw ::std::logic_error(
					"Cannot both use the minimum grid parameters for achieving maximum occupancy, _and_ saturate "
					"the grid with fixed-size cubs.");
			}
#endif
			if (not (kernel_)) {
				throw ::std::logic_error("A kernel must be set to determine how many blocks are required to saturate the device");
			}
			if (not (dimensions_.block)) {
				throw ::std::logic_error("The block dimensions must be known to determine how many of them one needs for saturating a device");
			}
			if (dimensions_.grid or dimensions_.overall) {
				throw ::std::logic_error("Conflicting specifications: Grid or overall dimensions specified, but requested to saturate kernels with active blocks");
			}

			result.block = dimensions_.block.value();
			auto dshmem_size = get_dynamic_shared_memory_size(dimensions_.block.value());
			auto num_block_threads = static_cast<grid::block_dimension_t>(dimensions_.block.value().volume());
			auto blocks_per_multiprocessor = kernel_->max_active_blocks_per_multiprocessor(num_block_threads, dshmem_size);
			auto num_multiprocessors = device().get_attribute(CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT);
			result.grid = blocks_per_multiprocessor * num_multiprocessors;
			return result;
		}
#if CUDA_VERSION >= 10000
		if (use_min_params_for_max_occupancy_) {
			if (not (kernel_)) {
				throw ::std::logic_error("A kernel must be set to determine the minimum grid parameter sfor m");
			}
			if (dimensions_.block or dimensions_.grid or dimensions_.overall) {
				throw ::std::logic_error("Conflicting specifications: Grid or overall dimensions specified, but requested to saturate kernels with active blocks");
			}
			auto composite_dims = dynamic_shared_memory_size_determiner_ ?
								  kernel_->min_grid_params_for_max_occupancy(dynamic_shared_memory_size_determiner_) :
								  kernel_->min_grid_params_for_max_occupancy(dynamic_shared_memory_size_);
			result.block = composite_dims.block;
			result.grid = composite_dims.grid;
			return result;
		}
#endif
		if (dimensions_.block and dimensions_.overall) {
			result.grid = grid::detail_::div_rounding_up(dimensions_.overall.value(), dimensions_.block.value());
			result.block = dimensions_.block.value();
			return result;
		}
		if (dimensions_.grid and dimensions_.overall) {
			result.block = grid::detail_::div_rounding_up(dimensions_.overall.value(), dimensions_.grid.value());
			result.grid = dimensions_.grid.value();
			return result;
		}
		if (dimensions_.grid and dimensions_.block) {
			result.block = dimensions_.block.value();
			result.grid = dimensions_.grid.value();
			return result;
		}

		if (not dimensions_.block and not dimensions_.grid) {
			throw ::std::logic_error(
				"Neither block nor grid dimensions have been specified");
		} else if (not dimensions_.block and not dimensions_.overall) {
			throw ::std::logic_error(
				"Attempt to obtain the composite grid dimensions, while the grid dimensions have only bee specified "
				"in terms of blocks, not threads, with no block dimensions specified");
		} else { // it must be the case that (not dimensions_.block and not dimensions_.overall)
			throw ::std::logic_error(
				"Only block dimensions have been specified - cannot resolve launch grid dimensions");
		}
	}

#ifndef NDEBUG
	grid::composite_dimensions_t get_composite_dimensions() const noexcept(false)
	{
		auto result = get_unvalidated_composite_dimensions();
		validate_composite_dimensions(result);
		return result;
	}
#endif

public:
	launch_configuration_t build() const
	{
		auto composite_dims = get_composite_dimensions();
		auto dynamic_shmem_size = get_dynamic_shared_memory_size(composite_dims.block);

		return launch_configuration_t{composite_dims, dynamic_shmem_size, thread_block_cooperation};
	}

protected:

	struct {
		optional<grid::block_dimensions_t  > block;
		optional<grid::dimensions_t        > grid;
		optional<grid::overall_dimensions_t> overall;
	} dimensions_;

	bool thread_block_cooperation { false };

	// Note: We could have used a variant between these two;
	// but the semantic is that if the determiner is not null, we use it;
	// and if you want to force a concrete apriori value, then you nullify
	// the determiner
	kernel::shared_memory_size_determiner_t dynamic_shared_memory_size_determiner_ {nullptr };
	memory::shared::size_t dynamic_shared_memory_size_ { 0 };

	const kernel_t* kernel_ { nullptr };
	optional<device::id_t> device_;
	bool saturate_with_active_blocks_ { false };
#if CUDA_VERSION >= 10000
	bool use_min_params_for_max_occupancy_ { false };
#endif

	static cuda::device_t device(optional<device::id_t> maybe_id)
	{
		return cuda::device::get(maybe_id.value());
	}

	cuda::device_t device() const { return device(device_.value()); }

	launch_config_builder_t& operator=(launch_configuration_t config)
	{
#ifndef NDEBUG
		detail_::validate(config);
		if (kernel_) { detail_::validate_compatibility(*kernel_, config); }
		if (device_) { detail_::validate_compatibility(device(), config); }
#endif
		thread_block_cooperation = config.block_cooperation;
		dynamic_shared_memory_size_ = config.dynamic_shared_memory_size;
		dimensions(config.dimensions);
		return *this;
	}

#ifndef NDEBUG
	static void validate_compatibility(
		const kernel_t*         kernel_ptr,
		memory::shared::size_t  shared_mem_size)
	{
		if (kernel_ptr == nullptr) { return; }
		detail_::validate_compatibility(*kernel_ptr, shared_mem_size);
	}

	static void validate_compatibility(
		optional<device::id_t> maybe_device_id,
		memory::shared::size_t shared_mem_size)
	{
		if (not maybe_device_id) { return; }
		detail_::validate_compatibility(device(maybe_device_id), shared_mem_size);
	}

	void validate_dynamic_shared_memory_size(memory::shared::size_t size)
	{
		validate_compatibility(kernel_, size);
		validate_compatibility(device_, size);
	}

	static void validate_block_dimension_compatibility(
		const kernel_t*          kernel_ptr,
		grid::block_dimensions_t block_dims)
	{
		if (kernel_ptr == nullptr) { return; }
		return detail_::validate_block_dimension_compatibility(*kernel_ptr, block_dims);
	}

	static void validate_block_dimension_compatibility(
		optional<device::id_t>    maybe_device_id,
		grid::block_dimensions_t  block_dims)
	{
		if (not maybe_device_id) { return; }
		detail_::validate_block_dimension_compatibility(device(maybe_device_id), block_dims);
	}

	void validate_block_dimensions(grid::block_dimensions_t block_dims) const
	{
		detail_::validate_block_dimensions(block_dims);
		if (dimensions_.grid and dimensions_.overall) {
			detail_::validate_all_dimension_compatibility(
				block_dims, dimensions_.grid.value(), dimensions_.overall.value());
		}
		// TODO: Check divisibility
		validate_block_dimension_compatibility(kernel_, block_dims);
		validate_block_dimension_compatibility(device_, block_dims);
	}

	void validate_grid_dimensions(grid::dimensions_t grid_dims) const
	{
		detail_::validate_grid_dimensions(grid_dims);
		if (dimensions_.block and dimensions_.overall) {
			detail_::validate_all_dimension_compatibility(
				dimensions_.block.value(), grid_dims, dimensions_.overall.value());
		}
		// TODO: Check divisibility
	}

	void validate_overall_dimensions(grid::overall_dimensions_t overall_dims) const
	{
		if (dimensions_.block and dimensions_.grid) {
			if (dimensions_.grid.value() * dimensions_.block.value() != overall_dims) {
				throw ::std::invalid_argument(
				"specified overall dimensions conflict with the already-specified "
				"block and grid dimensions");
			}
		}
	}

	void validate_kernel(const kernel_t* kernel_ptr) const
	{
		if (dimensions_.block or (dimensions_.grid and dimensions_.overall)) {
			auto block_dims = dimensions_.block ?
						dimensions_.block.value() :
						get_composite_dimensions().block;
			validate_block_dimension_compatibility(kernel_ptr, block_dims);
		}
		validate_compatibility(kernel_ptr, dynamic_shared_memory_size_);
	}

	void validate_device(device::id_t device_id) const
	{
		if (dimensions_.block or (dimensions_.grid and dimensions_.overall)) {
			auto block_dims = dimensions_.block ?
				dimensions_.block.value() :
				get_composite_dimensions().block;
			validate_block_dimension_compatibility(device_id, block_dims);
		}
		validate_compatibility(device_id, dynamic_shared_memory_size_);
	}

	void validate_composite_dimensions(grid::composite_dimensions_t composite_dims) const
	{
		validate_block_dimension_compatibility(kernel_, composite_dims.block);
		validate_block_dimension_compatibility(device_, composite_dims.block);

		// Is there anything to validate regarding the grid dims?
		validate_block_dimension_compatibility(device_, composite_dims.grid);
	}
#endif // ifndef NDEBUG

public:
	launch_config_builder_t& dimensions(grid::composite_dimensions_t composite_dims)
	{
#ifndef NDEBUG
		validate_composite_dimensions(composite_dims);
#endif
		dimensions_.overall = nullopt;
		dimensions_.grid = composite_dims.grid;
		dimensions_.block = composite_dims.block;
		return *this;
	}

	launch_config_builder_t& block_dimensions(grid::block_dimensions_t dims)
	{
#ifndef NDEBUG
		validate_block_dimensions(dims);
#endif
		dimensions_.block = dims;
		if (dimensions_.grid) {
			dimensions_.overall = nullopt;
		}
		return *this;

	}

	launch_config_builder_t& block_dimensions(
		grid::block_dimension_t x,
		grid::block_dimension_t y = 1,
		grid::block_dimension_t z = 1)
	{
		return block_dimensions(grid::block_dimensions_t{x, y, z});
	}

	launch_config_builder_t& block_size(grid::block_dimension_t size) { return block_dimensions(size, 1, 1); }

	launch_config_builder_t& use_maximum_linear_block()
	{
		grid::block_dimension_t max_size;
		if (kernel_) {
			max_size = kernel_->maximum_threads_per_block();
		}
		else if (device_) {
			max_size = device().maximum_threads_per_block();
		}
		else {
			throw ::std::logic_error("Request to use the maximum-size linear block, with no device or kernel specified");
		}
		auto block_dims = grid::block_dimensions_t { max_size, 1, 1 };

		if (dimensions_.grid and dimensions_.overall) {
			dimensions_.overall = nullopt;
		}
		dimensions_.block = block_dims;
		return *this;
	}

	launch_config_builder_t& grid_dimensions(grid::dimensions_t dims)
	{
#ifndef NDEBUG
		validate_grid_dimensions(dims);
#endif
		if (dimensions_.block) {
			dimensions_.overall = nullopt;
		}
		dimensions_.grid = dims;
		saturate_with_active_blocks_ = false;
		return *this;
	}

	launch_config_builder_t& grid_dimensions(
		grid::dimension_t x,
		grid::dimension_t y = 1,
		grid::dimension_t z = 1)
	{
		return grid_dimensions(grid::dimensions_t{x, y, z});
	}

	launch_config_builder_t& grid_size(grid::dimension_t size) {return grid_dimensions(size, 1, 1); }
	launch_config_builder_t& num_blocks(grid::dimension_t size) {return grid_size(size); }

	launch_config_builder_t& overall_dimensions(grid::overall_dimensions_t dims)
	{
#ifndef NDEBUG
		validate_overall_dimensions(dims);
#endif
		dimensions_.overall = dims;
		saturate_with_active_blocks_ = false;
		return *this;
	}
	launch_config_builder_t& overall_dimensions(
		grid::overall_dimension_t x,
		grid::overall_dimension_t y = 1,
		grid::overall_dimension_t z = 1)
	{
		return overall_dimensions(grid::overall_dimensions_t{x, y, z});
	}

	launch_config_builder_t& overall_size(grid::overall_dimension_t size) { return overall_dimensions(size, 1, 1); }

	launch_config_builder_t& block_cooperation(bool cooperation)
	{
		thread_block_cooperation = cooperation;
		return *this;
	}

	launch_config_builder_t& blocks_may_cooperate() { return block_cooperation(true); }
	launch_config_builder_t& blocks_dont_cooperate() { return block_cooperation(false); }

	launch_config_builder_t& dynamic_shared_memory_size(
		kernel::shared_memory_size_determiner_t shared_mem_size_determiner)
	{
		dynamic_shared_memory_size_determiner_ = shared_mem_size_determiner;
		return *this;
	}

	launch_config_builder_t& no_dynamic_shared_memory()
	{
		return dynamic_shared_memory_size(memory::shared::size_t(0));
	}

	launch_config_builder_t& dynamic_shared_memory_size(memory::shared::size_t size)
	{
#ifndef NDEBUG
		validate_dynamic_shared_memory_size(size);
#endif
		dynamic_shared_memory_size_ = size;
		dynamic_shared_memory_size_determiner_ = nullptr;
		return *this;
	}

	launch_config_builder_t& dynamic_shared_memory(memory::shared::size_t size)
	{
		return dynamic_shared_memory_size(size);
	}

	launch_config_builder_t& dynamic_shared_memory(
		kernel::shared_memory_size_determiner_t shared_mem_size_determiner)
	{
		return dynamic_shared_memory_size(shared_mem_size_determiner);
	}

	launch_config_builder_t& kernel(const kernel_t* wrapped_kernel_ptr)
	{
#ifndef NDEBUG
		validate_kernel(wrapped_kernel_ptr);
#endif
		kernel_ = wrapped_kernel_ptr;
		return *this;
	}

	launch_config_builder_t& kernel_independent()
	{
		kernel_ = nullptr;
		return *this;
	}
	launch_config_builder_t& no_kernel()
	{
		kernel_ = nullptr;
		return *this;
	}

	/**
	 * @brief THis will use information about the kernel, the already-set block size,
	 * and the device to create a unidimensional grid of blocks to exactly saturate
	 * the CUDA device's capacity for simultaneous active blocks.
	 *
	 * @note This will _not_ set the block size - unlike
	 */
	launch_config_builder_t& saturate_with_active_blocks()
	{
		if (not (kernel_)) {
			throw ::std::logic_error("A kernel must be set to determine how many blocks are required to saturate the device");
		}
		if (not (dimensions_.block)) {
			throw ::std::logic_error("The block dimensions must be known to determine how many of them one needs for saturating a device");
		}
		dimensions_.grid = nullopt;
		dimensions_.overall = nullopt;
#if CUDA_VERSION >= 10000
		use_min_params_for_max_occupancy_ = false;
#endif
		saturate_with_active_blocks_ = true;
		return *this;
	}

	launch_config_builder_t& min_params_for_max_occupancy()
	{
		if (not (kernel_)) {
			throw ::std::logic_error("A kernel must be set to determine how many blocks are required to saturate the device");
		}
		dimensions_.block = nullopt;
		dimensions_.grid = nullopt;
		dimensions_.overall = nullopt;
#if CUDA_VERSION >= 10000
		use_min_params_for_max_occupancy_ = true;
#endif
		saturate_with_active_blocks_ = false;
		return *this;
	}


}; // launch_config_builder_t

inline launch_config_builder_t launch_config_builder() { return {}; }

} // namespace cuda

#endif // CUDA_API_WRAPPERS_LAUNCH_CONFIG_BUILDER_CUH_
