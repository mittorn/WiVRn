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
#include "video_encoder_vulkan.h"

#include "encoder/yuv_converter.h"
#include "utils/wivrn_vk_bundle.h"
#include <iostream>
#include <stdexcept>

static uint32_t align(uint32_t value, uint32_t alignment)
{
	if (alignment == 0)
		return value;
	return alignment * (1 + (value - 1) / alignment);
}

size_t slot_info::get_slot()
{
	auto i = std::ranges::min_element(frames);
	return i - std::begin(frames);
}

std::optional<size_t> slot_info::get_ref()
{
	auto i = std::ranges::max_element(frames);
	if (*i >= 0)
		return i - std::begin(frames);
	return {};
}

vk::VideoFormatPropertiesKHR video_encoder_vulkan::select_video_format(
        vk::raii::PhysicalDevice & physical_device,
        const vk::PhysicalDeviceVideoFormatInfoKHR & format_info)
{
	for (const auto & video_fmt_prop: physical_device.getVideoFormatPropertiesKHR(format_info))
	{
		// TODO: do something smart if there is more than one
		return video_fmt_prop;
	}
	throw std::runtime_error("No suitable image format found");
}

void video_encoder_vulkan::init(const vk::VideoCapabilitiesKHR & video_caps,
                                const vk::VideoEncodeCapabilitiesKHR & encode_caps,
                                const vk::VideoProfileInfoKHR & video_profile,
                                void * video_session_create_next,
                                void * session_params_next)
{
	static const uint32_t num_dpb_slots = 4;

	fence = vk.device.createFence({});
	semaphore = vk.device.createSemaphore({});

	vk::VideoProfileListInfoKHR video_profile_list{
	        .profileCount = 1,
	        .pProfiles = &video_profile,
	};

	// Input image
	vk::VideoFormatPropertiesKHR picture_format;
	{
		vk::PhysicalDeviceVideoFormatInfoKHR video_fmt{
		        .pNext = &video_profile_list,
		        .imageUsage = vk::ImageUsageFlagBits::eVideoEncodeSrcKHR,
		};

		picture_format = select_video_format(vk.physical_device, video_fmt);

		if (picture_format.format != vk::Format::eG8B8R82Plane420Unorm)
		{
			throw std::runtime_error("Unsupported format " +
			                         vk::to_string(picture_format.format) +
			                         " for encoder input image");
		}

		vk::Extent3D aligned_extent{
		        .width = align(extent.width, encode_caps.encodeInputPictureGranularity.width),
		        .height = align(extent.height, encode_caps.encodeInputPictureGranularity.height),
		        .depth = 1,
		};

		// TODO: check format capabilities
		//
		vk::ImageCreateInfo img_create_info{
		        .pNext = &video_profile_list,
		        .flags = picture_format.imageCreateFlags,
		        .imageType = picture_format.imageType,
		        .format = picture_format.format,
		        .extent = aligned_extent,
		        .mipLevels = 1,
		        .arrayLayers = 1,
		        .samples = vk::SampleCountFlagBits::e1,
		        .tiling = picture_format.imageTiling,
		        .usage = vk::ImageUsageFlagBits::eTransferDst |
		                 picture_format.imageUsageFlags,
		        .sharingMode = vk::SharingMode::eExclusive,
		};

		input_image = image_allocation(
		        vk.device,
		        img_create_info,
		        {
		                .usage = VMA_MEMORY_USAGE_AUTO,
		        });
	}

	// Decode picture buffer (DPB) images
	vk::VideoFormatPropertiesKHR reference_picture_format;
	{
		vk::PhysicalDeviceVideoFormatInfoKHR video_fmt{
		        .pNext = &video_profile_list,
		        .imageUsage = vk::ImageUsageFlagBits::eVideoEncodeDpbKHR,
		};

		reference_picture_format = select_video_format(vk.physical_device, video_fmt);

		// TODO: check format capabilities
		// TODO: use multiple images if array levels are not supported

		vk::Extent3D aligned_extent{
		        .width = align(extent.width, video_caps.pictureAccessGranularity.width),
		        .height = align(extent.height, video_caps.pictureAccessGranularity.height),
		        .depth = 1,
		};

		vk::ImageCreateInfo img_create_info{
		        .pNext = &video_profile_list,
		        .flags = reference_picture_format.imageCreateFlags,
		        .imageType = reference_picture_format.imageType,
		        .format = reference_picture_format.format,
		        .extent = aligned_extent,
		        .mipLevels = 1,
		        .arrayLayers = num_dpb_slots,
		        .samples = vk::SampleCountFlagBits::e1,
		        .tiling = reference_picture_format.imageTiling,
		        .usage = reference_picture_format.imageUsageFlags,
		        .sharingMode = vk::SharingMode::eExclusive,
		};

		dpb_image = image_allocation(
		        vk.device,
		        img_create_info,
		        {
		                .usage = VMA_MEMORY_USAGE_AUTO,
		        });
	}

	// Output buffer
	{
		// very conservative bound
		output_buffer_size = extent.width * extent.height * 3;
		output_buffer_size = align(output_buffer_size, video_caps.minBitstreamBufferSizeAlignment);
		output_buffer = buffer_allocation(
		        vk.device,
		        {.pNext = &video_profile_list,
		         .size = output_buffer_size,
		         .usage = vk::BufferUsageFlagBits::eVideoEncodeDstKHR,
		         .sharingMode = vk::SharingMode::eExclusive},
		        {
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO,
		        });
	}

	// video session
	{
		vk::ExtensionProperties std_header_version = this->std_header_version();

		video_session =
		        vk.device.createVideoSessionKHR(vk::VideoSessionCreateInfoKHR{
		                .pNext = video_session_create_next,
		                .queueFamilyIndex = vk.encode_queue_family_index,
		                //.flags = vk::VideoSessionCreateFlagBitsKHR::eAllowEncodeParameterOptimizations,
		                .pVideoProfile = &video_profile,
		                .pictureFormat = picture_format.format,
		                .maxCodedExtent = extent,
		                .referencePictureFormat = reference_picture_format.format,
		                .maxDpbSlots = num_dpb_slots,
		                .maxActiveReferencePictures = 1,
		                .pStdHeaderVersion = &std_header_version,
		        });

		auto video_req = video_session.getMemoryRequirements();
		// FIXME: allocating on a single device memory seems to fail
		std::vector<vk::BindVideoSessionMemoryInfoKHR> video_session_bind;
		for (const auto & req: video_req)
		{
			vk::MemoryAllocateInfo alloc_info{
			        .allocationSize = req.memoryRequirements.size,
			        .memoryTypeIndex = vk.get_memory_type(req.memoryRequirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)};

			const auto & mem_item = mem.emplace_back(vk.device.allocateMemory(alloc_info));
			video_session_bind.push_back({
			        .memoryBindIndex = req.memoryBindIndex,
			        .memory = *mem_item,
			        .memoryOffset = 0,
			        .memorySize = alloc_info.allocationSize,
			});
		}
		video_session.bindMemory(video_session_bind);
	}

	// input image view
	{
		vk::ImageViewCreateInfo img_view_create_info{
		        .image = input_image,
		        .viewType = vk::ImageViewType::e2D,
		        .format = picture_format.format,
		        .components = picture_format.componentMapping,
		        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
		                             .baseMipLevel = 0,
		                             .levelCount = 1,
		                             .baseArrayLayer = 0,
		                             .layerCount = 1},
		};
		input_image_view = vk.device.createImageView(img_view_create_info);
	}

	// DPB image views
	{
		vk::ImageViewCreateInfo img_view_create_info{
		        .image = dpb_image,
		        .viewType = vk::ImageViewType::e2D,
		        .format = reference_picture_format.format,
		        .components = reference_picture_format.componentMapping,
		        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
		                             .baseMipLevel = 0,
		                             .levelCount = 1,
		                             .layerCount = 1},
		};
		for (size_t i = 0; i < num_dpb_slots; ++i)
		{
			img_view_create_info.subresourceRange.baseArrayLayer = i;
			dpb_image_views.push_back(vk.device.createImageView(img_view_create_info));
		}
	}

	// DPB video picture resource info
	{
		for (auto & dpb_image_view: dpb_image_views)
		{
			dpb_resource.push_back(
			        {
			                .codedExtent = extent,
			                .imageViewBinding = *dpb_image_view,
			        });
		}
	}

	// DPB slot info
	{
		auto std_slots = setup_slot_info(num_dpb_slots);
		assert(std_slots.size() == num_dpb_slots);
		for (size_t i = 0; i < num_dpb_slots; ++i)
		{
			dpb_slots.push_back({
			        .pNext = std_slots[i],
			        .slotIndex = -1,
			        .pPictureResource = nullptr,
			});
		}

		dpb_status = slot_info(num_dpb_slots);
	}

	// video session parameters
	{
		video_session_parameters = vk.device.createVideoSessionParametersKHR({
		        .pNext = session_params_next,
		        .videoSession = *video_session,
		});
	}

	// query pool
	{
		vk::StructureChain query_pool_create = {
		        vk::QueryPoolCreateInfo{
		                .queryType = vk::QueryType::eVideoEncodeFeedbackKHR,
		                .queryCount = 1,

		        },
		        vk::QueryPoolVideoEncodeFeedbackCreateInfoKHR{
		                .pNext = &video_profile,
		                .encodeFeedbackFlags =
		                        vk::VideoEncodeFeedbackFlagBitsKHR::estreamBufferOffsetBit |
		                        vk::VideoEncodeFeedbackFlagBitsKHR::estreamBytesWrittenBit,
		        },
		};

		query_pool = vk.device.createQueryPool(query_pool_create.get());
	}

	// command pool and buffer
	{
		command_pool = vk.device.createCommandPool({
		        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		        .queueFamilyIndex = vk.encode_queue_family_index,
		});

		command_buffer = std::move(vk.device.allocateCommandBuffers({.commandPool = *command_pool,
		                                                             .commandBufferCount = 1})[0]);
	}
}

video_encoder_vulkan::~video_encoder_vulkan()
{
}

std::vector<uint8_t> video_encoder_vulkan::get_encoded_parameters(void * next)
{
	auto [feedback, encoded] = vk.device.getEncodedVideoSessionParametersKHR({
	        .pNext = next,
	        .videoSessionParameters = *video_session_parameters,
	});
	return encoded;
}

void video_encoder_vulkan::PostSubmit()
{
	command_buffer.reset();
	command_buffer.begin(vk::CommandBufferBeginInfo{});
	vk::ImageMemoryBarrier2 barrier{
	        .srcStageMask = vk::PipelineStageFlagBits2KHR::eVideoEncodeKHR,
	        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
	        .dstStageMask = vk::PipelineStageFlagBits2KHR::eVideoEncodeKHR,
	        .dstAccessMask = vk::AccessFlagBits2::eVideoEncodeReadKHR,
	        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
	        .newLayout = vk::ImageLayout::eVideoEncodeSrcKHR,
	        .srcQueueFamilyIndex = vk.queue_family_index,
	        .dstQueueFamilyIndex = vk.encode_queue_family_index,
	        .image = input_image,
	        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
	                             .baseMipLevel = 0,
	                             .levelCount = 1,
	                             .baseArrayLayer = 0,
	                             .layerCount = 1},
	};
	vk::DependencyInfo dep_info{
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &barrier,
	};
	command_buffer.pipelineBarrier2(dep_info);
	command_buffer.resetQueryPool(*query_pool, 0, 1);

	// slot: where the encoded picture will be stored in DPB
	size_t slot = dpb_status.get_slot();
	// ref_slot: which image to use as reference
	auto ref_slot = dpb_status.get_ref();
	assert(not(ref_slot and (*ref_slot == slot)));
	dpb_status[slot] = frame_num;

	dpb_slots[slot].slotIndex = -1;
	dpb_slots[slot].pPictureResource = &dpb_resource[slot];

	{
		vk::VideoBeginCodingInfoKHR video_coding_begin_info{
		        .videoSession = *video_session,
		        .videoSessionParameters = *video_session_parameters,
		};
		video_coding_begin_info.setReferenceSlots(dpb_slots);
		command_buffer.beginVideoCodingKHR(video_coding_begin_info);
	}

	if (frame_num == 0)
	{
		command_buffer.controlVideoCodingKHR({.flags = vk::VideoCodingControlFlagBitsKHR::eReset});
		vk::ImageMemoryBarrier2 dpb_barrier{
		        .srcStageMask = vk::PipelineStageFlagBits2KHR::eNone,
		        .srcAccessMask = vk::AccessFlagBits2::eNone,
		        .dstStageMask = vk::PipelineStageFlagBits2KHR::eVideoEncodeKHR,
		        .dstAccessMask = vk::AccessFlagBits2::eVideoEncodeReadKHR | vk::AccessFlagBits2::eVideoEncodeWriteKHR,
		        .oldLayout = vk::ImageLayout::eUndefined,
		        .newLayout = vk::ImageLayout::eVideoEncodeDpbKHR,
		        .image = dpb_image,
		        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
		                             .baseMipLevel = 0,
		                             .levelCount = 1,
		                             .baseArrayLayer = 0,
		                             .layerCount = 1},
		};
		command_buffer.pipelineBarrier2({
		        .imageMemoryBarrierCount = 1,
		        .pImageMemoryBarriers = &dpb_barrier,
		});
	}

	dpb_slots[slot].slotIndex = slot;
	vk::VideoEncodeInfoKHR encode_info{
	        .pNext = encode_info_next(frame_num, slot, ref_slot),
	        .dstBuffer = output_buffer,
	        .dstBufferOffset = 0,
	        .dstBufferRange = output_buffer_size,
	        .srcPictureResource = {.codedExtent = extent,
	                               .baseArrayLayer = 0,
	                               .imageViewBinding = *input_image_view},
	        .pSetupReferenceSlot = &dpb_slots[slot],
	};
	if (ref_slot)
		encode_info.setReferenceSlots(dpb_slots[*ref_slot]);

	command_buffer.beginQuery(*query_pool, 0, {});
	command_buffer.encodeVideoKHR(encode_info);
	command_buffer.endQuery(*query_pool, 0);
	command_buffer.endVideoCodingKHR(vk::VideoEndCodingInfoKHR{});
	command_buffer.end();

	vk::SubmitInfo2 submit{};
	vk::CommandBufferSubmitInfo cmd_info{
	        .commandBuffer = *command_buffer,
	};
	submit.setCommandBufferInfos(cmd_info);
	vk::SemaphoreSubmitInfo sem_info{
	        .semaphore = *semaphore,
	        .stageMask = vk::PipelineStageFlagBits2::eVideoEncodeKHR,
	};
	submit.setWaitSemaphoreInfos(sem_info);
	// FIXME: allow sequencing submits with multiple vulkan encode groups
	vk.encode_queue.submit2(submit, *fence);
	++frame_num;
}

void video_encoder_vulkan::Encode(bool idr, std::chrono::steady_clock::time_point target_timestamp)
{
	if (idr)
	{
		send_idr_data();
	}
	if (auto res = vk.device.waitForFences(*fence, true, 1'000'000'000);
	    res != vk::Result::eSuccess)
	{
		throw std::runtime_error("wait for fences: " + vk::to_string(res));
	}

	// Feedback = offset / size / has overrides
	auto [res, feedback] = query_pool.getResults<uint32_t>(0, 1, 3 * sizeof(uint32_t), 0, vk::QueryResultFlagBits::eWait);
	if (res != vk::Result::eSuccess)
	{
		std::cerr << "device.getQueryPoolResults: " << vk::to_string(res) << std::endl;
	}

	vk.device.resetFences(*fence);

	SendData({((uint8_t *)output_buffer.map()) + feedback[0], feedback[1]}, true);
}
vk::Semaphore video_encoder_vulkan::PresentImage(yuv_converter & src_yuv, vk::raii::CommandBuffer & cmd_buf)
{
	vk::ImageMemoryBarrier2 barrier{
	        .srcStageMask = vk::PipelineStageFlagBits2KHR::eNone,
	        .srcAccessMask = vk::AccessFlagBits2::eNone,
	        .dstStageMask = vk::PipelineStageFlagBits2KHR::eTransfer,
	        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
	        .oldLayout = vk::ImageLayout::eUndefined,
	        .newLayout = vk::ImageLayout::eTransferDstOptimal,
	        .image = input_image,
	        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
	                             .baseMipLevel = 0,
	                             .levelCount = 1,
	                             .baseArrayLayer = 0,
	                             .layerCount = 1},
	};
	vk::DependencyInfo dep_info{
	        .imageMemoryBarrierCount = 1,
	        .pImageMemoryBarriers = &barrier,
	};
	cmd_buf.pipelineBarrier2(dep_info);

	cmd_buf.copyImage(
	        src_yuv.luma,
	        vk::ImageLayout::eTransferSrcOptimal,
	        input_image,
	        vk::ImageLayout::eTransferDstOptimal,
	        vk::ImageCopy{.srcSubresource = {
	                              .aspectMask = vk::ImageAspectFlagBits::eColor,
	                              .layerCount = 1,
	                      },
	                      .dstSubresource = {
	                              .aspectMask = vk::ImageAspectFlagBits::ePlane0,
	                              .layerCount = 1,
	                      },
	                      .extent = {extent.width, extent.height, 1}});
	cmd_buf.copyImage(
	        src_yuv.chroma,
	        vk::ImageLayout::eTransferSrcOptimal,
	        input_image,
	        vk::ImageLayout::eTransferDstOptimal,
	        vk::ImageCopy{.srcSubresource = {
	                              .aspectMask = vk::ImageAspectFlagBits::eColor,
	                              .layerCount = 1,
	                      },
	                      .dstSubresource = {
	                              .aspectMask = vk::ImageAspectFlagBits::ePlane1,
	                              .layerCount = 1,
	                      },
	                      .extent = {extent.width / 2, extent.height / 2, 1}});

	barrier.srcStageMask = vk::PipelineStageFlagBits2KHR::eTransfer;
	barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	barrier.dstStageMask = vk::PipelineStageFlagBits2KHR::eTopOfPipe;
	barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal,
	barrier.newLayout = vk::ImageLayout::eVideoEncodeSrcKHR,
	barrier.srcQueueFamilyIndex = vk.queue_family_index,
	barrier.dstQueueFamilyIndex = vk.encode_queue_family_index,
	cmd_buf.pipelineBarrier2(dep_info);

	return *semaphore;
}
