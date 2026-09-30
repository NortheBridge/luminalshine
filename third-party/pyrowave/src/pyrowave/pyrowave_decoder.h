// Copyright (c) 2025 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <map>
#include <span>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_raii.hpp>

#include "pyrowave_common.h"

namespace PyroWave
{

class Decoder;

class DecoderInput
{
	friend class Decoder;

	const Decoder & decoder;

	buffer_allocation dequant_offset_buffer;
	buffer_allocation dequant_staging;
	std::span<uint32_t> dequant_data;
	buffer_allocation payload_data;
	buffer_allocation payload_staging;
	uint8_t * payload = nullptr;
	size_t payload_size = 0;

	vk::raii::BufferView u32_view = nullptr;
	vk::raii::BufferView u16_view = nullptr;
	vk::raii::BufferView u8_view = nullptr;

	vk::raii::Image r32_image = nullptr;
	vk::raii::Image r16_image = nullptr;
	vk::raii::Image r8_image = nullptr;
	vk::raii::ImageView r32_imageview = nullptr;
	vk::raii::ImageView r16_imageview = nullptr;
	vk::raii::ImageView r8_imageview = nullptr;
	bool need_image_transition = true;

	BitstreamHeader header{};
	size_t header_size = 0;
	size_t packet_size = 0;

	int decoded_blocks = 0;
	uint32_t last_seq = UINT32_MAX;
	int total_blocks_in_sequence = 0;

	// Fork extensions (conditional replenishment / aligned packetization) ---
	size_t skip_size = 0;  ///< remaining bytes of an in-band padding record to discard
	uint32_t pending_block_index = UINT32_MAX;  ///< block registered only once fully received
	uint32_t pending_offset_u32 = 0;
	bool keep_frame = true;  ///< current frame uses keep-previous semantics (seq code 1)
	bool seen_full_frame = false;  ///< a code-0 (full) frame initialized the coefficient state
	uint32_t anomaly_count = 0;  ///< see parse_anomalies()

public:
	DecoderInput(const Decoder &);
	bool push_data(std::span<const uint8_t> data);
	void clear();

	// Fork extension: true when the current frame was coded with keep-previous
	// semantics (conditional replenishment). Always safe to honour: the
	// decoder clears the wavelet images on the first frame, so blocks that
	// were never received decode as zero until their first delivery.
	bool keep_previous_frame() const
	{
		return keep_frame;
	}

	// Fork extension: true when keep-previous (code-1) frames are arriving but
	// no full (code-0) frame has initialized the coefficient state yet. The
	// picture is degraded (absent blocks decode to zero) until the host sends
	// a full frame; callers should keep requesting an IDR while this holds.
	bool awaiting_state_init() const
	{
		return keep_frame && !seen_full_frame;
	}

	// Fork extension: running count of bitstream parse anomalies (backwards
	// sequence, out-of-bounds or duplicate block index, unknown sequence
	// code, dimension mismatch). Never reset; callers diff between frames
	// and surface it in their own logs (the std::cerr messages are invisible
	// in most client log captures).
	uint32_t parse_anomalies() const
	{
		return anomaly_count;
	}

	// Fork extension: call at each RTP-payload boundary. The host aligns block
	// packets to payload boundaries, so a parser that is mid-record here lost
	// the previous payload; this drops the partial record and resyncs.
	void resync_at_packet_boundary();
	// Flush the host-written bitstream/offset buffers so the GPU decode reads the
	// current frame instead of stale device memory (required on non-coherent
	// HOST_CACHED memory, e.g. Tegra X1). No-op on coherent memory.
	void flush();

private:
	void push_raw(const void * data, size_t size);
	void check_linear_texture_support();
};

class Decoder : public WaveletBuffers
{
	friend class DecoderInput;

	bool use_readonly_texel_buffer = false;
	bool fragment_path;
	// Fork extension: wavelet images hold valid contents from a previous frame
	// (switches the per-frame acquire barrier from discard to preserve).
	bool wavelet_images_valid = false;
	vk::raii::PhysicalDevice & phys_dev;
	vk::raii::DescriptorPool ds_pool;

	using key_render_pass = std::tuple<std::array<vk::Format, 3>, std::array<vk::ImageLayout, 3>>;
	using sp = std::tuple<
	        VkBool32, // vertical
	        VkBool32, // final_y
	        VkBool32, // final_cbcr
	        int32_t   // edge_condition (-1, 0, 1)
	        >;
	using key_pipeline = std::tuple<vk::RenderPass, vk::PipelineLayout, sp>;

	// For fragment based iDWT.
	struct
	{
		struct pipeline_t
		{
			vk::PipelineLayout layout{};
			vk::RenderPass rp{};
			vk::DescriptorSet ds{};
			vk::raii::Framebuffer fb = nullptr;
			vk::Extent2D fb_extent{};
			vk::Pipeline pipeline[3];
		};
		vk::raii::DescriptorSetLayout ds_layout[3] = {nullptr, nullptr, nullptr};
		vk::raii::PipelineLayout layout[3] = {nullptr, nullptr, nullptr};
		std::map<key_render_pass, vk::raii::RenderPass> render_pass;
		std::map<key_pipeline, vk::raii::Pipeline> pipelines;
		std::map<vk::ImageView, vk::raii::Framebuffer> framebuffers;
		struct
		{
			image_allocation vert[2 /*even odd*/][2 /*luma  chroma*/];
			vk::raii::ImageView vert_views[2][2] = {
			        {nullptr, nullptr},
			        {nullptr, nullptr},
			};
			image_allocation horiz[NumComponents];
			vk::raii::ImageView horiz_views[NumComponents] = {nullptr, nullptr, nullptr};
			vk::raii::ImageView decoded[NumComponents][NumFrequencyBandsPerLevel] = {
			        {nullptr, nullptr, nullptr, nullptr},
			        {nullptr, nullptr, nullptr, nullptr},
			        {nullptr, nullptr, nullptr, nullptr},
			};
			vk::Extent2D decoded_dim;
			pipeline_t vertical[2];
			pipeline_t horizontal;
		} levels[DecompositionLevels];
		pipeline_t level0_420;
	} fragment;

	// NB: named compute_pipeline (not pipeline) so its `pipeline` member does not
	// collide with the injected-class-name; MSVC otherwise resolves `x.pipeline`
	// to the type rather than the member. (pipeline_t above is the fragment path.)
	struct compute_pipeline
	{
		vk::raii::DescriptorSetLayout ds_layout = nullptr;
		vk::DescriptorSet ds[NumComponents][DecompositionLevels];
		vk::raii::PipelineLayout layout = nullptr;
		vk::raii::Pipeline pipeline = nullptr;
	};

	compute_pipeline dequant_[3];
	compute_pipeline idwt_;
	vk::raii::Pipeline idwt_dcshift = nullptr;

public:
	using ViewBuffers = std::array<vk::ImageView, 3>;

	Decoder(vk::raii::PhysicalDevice & phys_dev, vk::raii::Device & device, int width, int height, ChromaSubsampling chroma, bool fragment_path = false);
	~Decoder();

	bool decode(vk::raii::CommandBuffer & cmd, DecoderInput & input, const ViewBuffers & views);

private:
	bool dequant(vk::raii::CommandBuffer & cmd, size_t storage_mode, bool keep_previous);
	bool idwt(vk::raii::CommandBuffer & cmd, const ViewBuffers & views);
	bool idwt_fragment(vk::raii::CommandBuffer & cmd, const ViewBuffers & views);
	vk::DescriptorSet allocate_descriptor_set(vk::DescriptorSetLayout);
};
} // namespace PyroWave
