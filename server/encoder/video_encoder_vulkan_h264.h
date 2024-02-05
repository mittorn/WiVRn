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

#include "video_encoder_vulkan.h"

#include <memory>
#include <vector>

#include <vulkan/vulkan.hpp>

class video_encoder_vulkan_h264 : public video_encoder_vulkan
{
	uint16_t idr_id = 0;
	StdVideoH264SequenceParameterSet sps;
	StdVideoH264PictureParameterSet pps;

	StdVideoEncodeH264SliceHeader slice_header;
	vk::VideoEncodeH264NaluSliceInfoKHR nalu_slice_info;

	StdVideoEncodeH264PictureInfo std_picture_info;
	vk::VideoEncodeH264PictureInfoKHR picture_info;

	StdVideoEncodeH264ReferenceListsInfo reference_lists_info;

	std::vector<StdVideoEncodeH264ReferenceInfo> dpb_std_info;
	std::vector<vk::VideoEncodeH264DpbSlotInfoKHR> dpb_std_slots;

	video_encoder_vulkan_h264(wivrn_vk_bundle & vk, vk::Rect2D rect, vk::VideoEncodeCapabilitiesKHR encode_caps, float fps, uint64_t bitrate);

protected:
	std::vector<void *> setup_slot_info(size_t dpb_size) override;

	void * encode_info_next(uint32_t frame_num, size_t slot, std::optional<size_t> ref) override;
	virtual vk::ExtensionProperties std_header_version() override;

	void send_idr_data() override;

public:
	static std::unique_ptr<video_encoder_vulkan_h264> create(wivrn_vk_bundle & vk,
	                                                         xrt::drivers::wivrn::encoder_settings & settings,
	                                                         float fps);

	std::vector<uint8_t> get_sps_pps();
};
