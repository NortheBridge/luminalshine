#pragma once

#include <vulkan/vulkan_raii.hpp>

#include "pyrowave_common.h"

namespace PyroWave
{
class Encoder : public WaveletBuffers
{
	vk::raii::DescriptorPool ds_pool;
	struct pipeline
	{
		vk::raii::DescriptorSetLayout ds_layout = nullptr;
		vk::DescriptorSet ds = nullptr;
		vk::raii::PipelineLayout layout = nullptr;
		vk::raii::Pipeline pipeline = nullptr;
	};
	buffer_allocation bucket_buffer, meta_buffer, block_stat_buffer, payload_data, quant_buffer;
	uint32_t sequence_count = 0;
	int quality_bias = 0;

	pipeline block_packing_;
	pipeline resolve_rdo_;
	pipeline analyze_rdo_;
	pipeline analyze_rdo_finalize; // shares descriptor set with anaylze_rdo
	vk::DescriptorSet quant_ds[NumComponents][DecompositionLevels];
	pipeline quant_;
	vk::DescriptorSet dwt_ds[NumComponents][DecompositionLevels];
	pipeline dwt_;
	vk::raii::Pipeline dwt_dcshift = nullptr;

	/*
public:
	buffer_allocation debug_buffer;
	*/

public:
	using ViewBuffers = std::array<vk::ImageView, 3>;
	Encoder(vk::raii::PhysicalDevice & phys_dev, vk::raii::Device & device, int width, int height, ChromaSubsampling chroma);
	~Encoder();

	struct BitstreamBuffers
	{
		struct
		{
			vk::Buffer buffer;
			uint64_t offset;
			uint64_t size;
		} meta, bitstream;
		size_t target_size;
	};

	bool encode(vk::raii::CommandBuffer & cmd, const ViewBuffers & views, const BitstreamBuffers & buffers);

	// Fork extension: raise the initial (pre-RDO) quantization resolution by
	// this many bits (clamped to [0, 3]). Lets very high per-frame budgets buy
	// additional quality instead of saturating. Encoder-side only; the wire
	// format is unchanged (quant codes are carried per block).
	void set_quality_bias(int extra_bits);

	// Debug hackery
	const vk::ImageView get_wavelet_band(int component, int level);
	bool encode_pre_transformed(vk::raii::CommandBuffer & cmd, const BitstreamBuffers & buffers, float quant_scale);
	//

	uint64_t get_meta_required_size() const;

	struct Packet
	{
		size_t offset;
		size_t size;
	};

	size_t compute_num_packets(const void * mapped_meta, size_t packet_boundary) const;
	size_t packetize(Packet * packets, size_t packet_boundary, void * bitstream, size_t size, const void * mapped_meta, const void * mapped_bitstream) const;

	void report_stats(const void * mapped_meta, const void * mapped_bitstream) const;

	// Diagnostic: run the per-block self-consistency validator over a finished frame and
	// return counts the caller can route through its own logger. max_num_words tells how
	// close any block is to the 12-bit payload_words cap (4095).
	struct ValidationStats
	{
		uint32_t total_blocks = 0;
		uint32_t nonzero_blocks = 0;
		uint32_t invalid_blocks = 0;
		uint32_t max_num_words = 0;
	};
	ValidationStats validate_frame(const void * mapped_meta, const void * mapped_bitstream) const;

private:
	bool encode_quant_and_coding(vk::raii::CommandBuffer & cmd, const BitstreamBuffers & buffers, float quant_scale);

	bool dwt(vk::raii::CommandBuffer & cmd, const ViewBuffers & views);
	bool quant(vk::raii::CommandBuffer & cmd, float quant_scale);
	bool analyze_rdo(vk::raii::CommandBuffer & cmd);
	bool resolve_rdo(vk::raii::CommandBuffer & cmd, size_t target_payload_size);
	bool block_packing(vk::raii::CommandBuffer & cmd, const BitstreamBuffers & buffers, float quant_scale);

	float get_noise_power_normalized_quant_resolution(int level, int component, int band) const;
	float get_quant_resolution(int level, int component, int band) const;
	float get_quant_rdo_distortion_scale(int level, int component, int band) const;

	void analyze_alternative_packing(const void * mapped_meta, const void * mapped_bitstream) const;

	bool validate_bitstream(const uint32_t * bitstream_u32, const BitstreamPacket * meta, uint32_t block_index) const;
};
} // namespace PyroWave
