/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <vector>
#include <vulkan/vulkan_raii.hpp>

#include "video_encoder.h"
#include "vk/allocation.h"

class slot_info
{
	std::vector<int32_t> frames;

public:
	slot_info(size_t size) :
	        frames(size, -1) {}

	std::pair<size_t, std::optional<size_t>> add_frame(int32_t frame);
	size_t get_slot();

	int32_t & operator[](size_t slot)
	{
		return frames[slot];
	}

	std::optional<size_t> get_ref();
};

class video_encoder_vulkan : public xrt::drivers::wivrn::VideoEncoder
{
	wivrn_vk_bundle & vk;
	const vk::VideoEncodeCapabilitiesKHR encode_caps;

	vk::raii::Semaphore semaphore = nullptr;
	vk::raii::Fence fence = nullptr;

	vk::raii::VideoSessionKHR video_session = nullptr;
	vk::raii::VideoSessionParametersKHR video_session_parameters = nullptr;

	vk::raii::QueryPool query_pool = nullptr;
	vk::raii::CommandPool command_pool = nullptr;

	vk::raii::CommandBuffer command_buffer = nullptr;

	buffer_allocation output_buffer;
	size_t output_buffer_size;

	image_allocation input_image;

	vk::raii::ImageView input_image_view = nullptr;

	slot_info dpb_status = slot_info(0);

	image_allocation dpb_image;
	std::vector<vk::raii::ImageView> dpb_image_views;
	std::vector<vk::VideoPictureResourceInfoKHR> dpb_resource;
	std::vector<vk::VideoReferenceSlotInfoKHR> dpb_slots;

	std::vector<vk::raii::DeviceMemory> mem;

	vk::VideoFormatPropertiesKHR select_video_format(
	        vk::raii::PhysicalDevice & physical_device,
	        const vk::PhysicalDeviceVideoFormatInfoKHR &);

	uint32_t frame_num = 0;
	const vk::Rect2D rect;
	const float fps;
	const uint64_t bitrate;

protected:
	video_encoder_vulkan(wivrn_vk_bundle & vk, vk::Rect2D rect, vk::VideoEncodeCapabilitiesKHR encode_caps, float fps, uint64_t bitrate) :
	        vk(vk), rect(rect), encode_caps(encode_caps), fps(fps), bitrate(bitrate) {}

	void init(const vk::VideoCapabilitiesKHR & video_caps,
	          const vk::VideoProfileInfoKHR & video_profile,
	          void * video_session_create_next,
	          void * session_params_next);

	virtual ~video_encoder_vulkan();

	std::vector<uint8_t> get_encoded_parameters(void * next);

	virtual void send_idr_data() = 0;

	virtual std::vector<void *> setup_slot_info(size_t dpb_size) = 0;
	virtual void * encode_info_next(uint32_t frame_num, size_t slot, std::optional<size_t> ref) = 0;
	virtual vk::ExtensionProperties std_header_version() = 0;

	void PostSubmit() override;

public:
	void Encode(bool idr, std::chrono::steady_clock::time_point target_timestamp) override;
	vk::Semaphore PresentImage(yuv_converter & src_yuv, vk::raii::CommandBuffer & cmd_buf) override;
};
