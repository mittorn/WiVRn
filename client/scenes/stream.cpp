/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_enums.hpp>
#include <vulkan/vulkan_raii.hpp>
#define GLM_FORCE_RADIANS

#include "stream.h"

#include "application.h"
#include "decoder/shard_accumulator.h"
#include "magic_enum.hpp"
#include "glm/fwd.hpp"
#include "spdlog/spdlog.h"
#include "utils/check.h"
#include "utils/ranges.h"
#include "utils/sync_queue.h"
#include "vk/device_memory.h"
#include "wivrn_packets.h"
#include "xr/details/enumerate.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vulkan/vulkan_core.h>
#include "audio/audio.h"

using namespace xrt::drivers::wivrn;

// clang-format off
static const std::unordered_map<std::string, device_id> device_ids = {
	{"/user/hand/left/input/x/click",           device_id::X_CLICK},
	{"/user/hand/left/input/x/touch",           device_id::X_TOUCH},
	{"/user/hand/left/input/y/click",           device_id::Y_CLICK},
	{"/user/hand/left/input/y/touch",           device_id::Y_TOUCH},
	{"/user/hand/left/input/menu/click",        device_id::MENU_CLICK},
	{"/user/hand/left/input/squeeze/value",     device_id::LEFT_SQUEEZE_VALUE},
	{"/user/hand/left/input/trigger/value",     device_id::LEFT_TRIGGER_VALUE},
	{"/user/hand/left/input/trigger/touch",     device_id::LEFT_TRIGGER_TOUCH},
	{"/user/hand/left/input/thumbstick",        device_id::LEFT_THUMBSTICK_X},
	{"/user/hand/left/input/thumbstick/click",  device_id::LEFT_THUMBSTICK_CLICK},
	{"/user/hand/left/input/thumbstick/touch",  device_id::LEFT_THUMBSTICK_TOUCH},
	{"/user/hand/left/input/thumbrest/touch",   device_id::LEFT_THUMBREST_TOUCH},
	{"/user/hand/right/input/a/click",          device_id::A_CLICK},
	{"/user/hand/right/input/a/touch",          device_id::A_TOUCH},
	{"/user/hand/right/input/b/click",          device_id::B_CLICK},
	{"/user/hand/right/input/b/touch",          device_id::B_TOUCH},
	{"/user/hand/right/input/system/click",     device_id::SYSTEM_CLICK},
	{"/user/hand/right/input/squeeze/value",    device_id::RIGHT_SQUEEZE_VALUE},
	{"/user/hand/right/input/trigger/value",    device_id::RIGHT_TRIGGER_VALUE},
	{"/user/hand/right/input/trigger/touch",    device_id::RIGHT_TRIGGER_TOUCH},
	{"/user/hand/right/input/thumbstick",       device_id::RIGHT_THUMBSTICK_X},
	{"/user/hand/right/input/thumbstick/click", device_id::RIGHT_THUMBSTICK_CLICK},
	{"/user/hand/right/input/thumbstick/touch", device_id::RIGHT_THUMBSTICK_TOUCH},
	{"/user/hand/right/input/thumbrest/touch",  device_id::RIGHT_THUMBREST_TOUCH},
};
// clang-format on

std::shared_ptr<scenes::stream> scenes::stream::create(std::unique_ptr<wivrn_session> network_session)
{
	std::shared_ptr<stream> self{new stream};

	self->network_session = std::move(network_session);

	from_headset::headset_info_packet info{};

	info.recommended_eye_width = self->swapchains[0].width();
	info.recommended_eye_height = self->swapchains[0].height();

	auto [flags, views] = self->session.locate_views(
	        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	        self->instance.now(),
	        application::view());
	assert(views.size() == info.fov.size());
	for (auto [i, j]: utils::zip(views, info.fov))
	{
		j = i.fov;
	}

	if (self->instance.has_extension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
	{
		info.available_refresh_rates = self->session.get_refresh_rates();
		info.preferred_refresh_rate = self->session.get_current_refresh_rate();
	}

	if (info.available_refresh_rates.empty())
		spdlog::warn("Unable to detect refresh rates");

	audio::get_audio_description(info);

	self->network_session->send_control(info);

	self->network_thread = std::thread(&stream::process_packets, self.get());
	pthread_setname_np(self->network_thread.native_handle(), "network_thread");

	self->video_thread = std::thread(&stream::video, self.get());
	pthread_setname_np(self->video_thread.native_handle(), "video_thread");

	self->command_buffer = std::move(self->device.allocateCommandBuffers({
		.commandPool = *self->commandpool,
		.level = vk::CommandBufferLevel::ePrimary,
		.commandBufferCount = 1})[0]);
	self->fence = self->create_fence(false);

	// Look up the XrActions for haptics
	self->haptics_actions[0].first = application::get_action("/user/hand/left/output/haptic").first;
	self->haptics_actions[0].second = application::string_to_path("/user/hand/left");

	self->haptics_actions[1].first = application::get_action("/user/hand/right/output/haptic").first;
	self->haptics_actions[1].second = application::string_to_path("/user/hand/right");

	// Look up the XrActions for input
	for (const auto & [action, action_type, name]: application::inputs())
	{
		auto it = device_ids.find(name);

		if (it == device_ids.end())
			continue;

		self->input_actions.emplace_back(it->second, action, action_type);
	}

	return self;
}

scenes::stream::~stream()
{
	cleanup();
	exit();

	video_thread.join();
	if (tracking_thread)
		tracking_thread->join();
	network_thread.join();
}

void scenes::stream::push_blit_handle(shard_accumulator * decoder, std::shared_ptr<shard_accumulator::blit_handle> handle)
{
	std::lock_guard lock(decoder_mutex);

	if (!application::is_visible())
		return;

	for (auto & i: decoders)
	{
		if (i.decoder.get() == decoder)
		{
			static_assert(std::tuple_size_v<decltype(i.latest_frames)> == 2);
			std::swap(i.latest_frames[0], i.latest_frames[1]);
			i.latest_frames[1] = handle;
			break;
		}
	}

	if (!ready_ && std::all_of(decoders.begin(), decoders.end(), [](accumulator_images & i) {
		    return i.latest_frames.back();
	    }))
	{
		ready_ = true;
		first_frame_time = application::now();
		spdlog::info("Stream scene ready at t={}", first_frame_time);
	}
}

std::vector<uint64_t> scenes::stream::accumulator_images::frames() const
{
	std::vector<uint64_t> result;
	for (const auto & frame: latest_frames)
	{
		if (frame)
			result.push_back(frame->feedback.frame_index);
	}
	return result;
}

std::optional<uint64_t> scenes::stream::accumulator_images::common_frame(const std::vector<accumulator_images> & sets)
{
	if (sets.empty())
		return {};
	auto common_frames = sets[0].frames();
	for (const auto & set: sets)
	{
		std::vector<uint64_t> tmp;
		auto x = set.frames();
		std::set_intersection(
		        x.begin(), x.end(), common_frames.begin(), common_frames.end(), std::back_inserter(tmp));
		common_frames = tmp;
		if (common_frames.empty())
			return {};
	}
	assert(not common_frames.empty());
	static uint64_t frame_index;

	// next frame if availiable
	for (auto it = common_frames.begin(); it != common_frames.end(); ++it)
		if(*it == frame_index + 1)
			return ++frame_index;

	// too slow? keep on current frame
	for (auto it = common_frames.begin(); it != common_frames.end(); ++it)
		if(*it == frame_index)
			return frame_index;

	// select oldest frame to allow it go smoother
	return (frame_index = common_frames.front());
}

std::shared_ptr<shard_accumulator::blit_handle> scenes::stream::accumulator_images::frame(std::optional<uint64_t> id)
{
	for (auto it = latest_frames.rbegin(); it != latest_frames.rend(); ++it)
	{
		if (not *it)
			continue;
		if (id and (*it)->feedback.frame_index != *id)
			continue;
		return *it;
	}
	return nullptr;
//	if(latest_frames[0])
//		frame_index = latest_frames[0]->feedback.frame_index;
//	return latest_frames[3];//nullptr;//latest_frames[0];
}
static std::array<XrPosef, 2> g_last_pose;
static bool g_pose_present;

void scenes::stream::render()
{
	if (exiting)
		application::pop_scene();

	XrFrameState framestate = session.wait_frame();

	if (decoders.empty())
		framestate.shouldRender = false;

	if (!framestate.shouldRender)
	{
		// TODO: stop/restart video stream
		session.begin_frame();
		session.end_frame(framestate.predictedDisplayTime, {});

		std::unique_lock lock(decoder_mutex);
		for (auto & i: decoders)
		{
			for (auto & frame: i.latest_frames)
				frame.reset();
		}

		return;
	}

	session.begin_frame();

	auto [flags, views] = session.locate_views(viewconfig, framestate.predictedDisplayTime, world_space);
	assert(views.size() == swapchains.size());

	std::array<int, view_count> image_indices;
	assert(views.size() == view_count);
	for (size_t swapchain_index = 0; swapchain_index < views.size(); swapchain_index++)
	{
		int image_index = swapchains[swapchain_index].acquire();
		swapchains[swapchain_index].wait();

		image_indices[swapchain_index] = image_index;
	}

	command_buffer.reset();

	vk::CommandBufferBeginInfo begin_info;
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	command_buffer.begin(begin_info);

	// Transition the layout of the decoder framebuffer to the one the decoders expect
	std::vector<vk::ImageMemoryBarrier> image_barriers;
	for (size_t i = 0; i < decoder_output.size(); i++)
	{
		vk::ImageMemoryBarrier barrier;
		barrier.srcAccessMask = vk::AccessFlagBits::eNone;
		barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
		barrier.oldLayout = vk::ImageLayout::eUndefined;
		barrier.newLayout = shard_accumulator::framebuffer_expected_layout;
		barrier.image = (vk::Image)decoder_output[i].image;
		barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		image_barriers.push_back(barrier);
	}
	command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands, vk::DependencyFlags{}, {}, {}, image_barriers);

	// Keep a reference to the resources needed to blit the images until vkWaitForFences
	std::vector<std::shared_ptr<shard_accumulator::blit_handle>> current_blit_handles;

	std::array<XrPosef, 2> pose = {views[0].pose, views[1].pose};
	if(!g_pose_present) g_last_pose = pose;
	std::array<XrFovf, 2> fov = {views[0].fov, views[1].fov};
	{
		std::lock_guard lock(decoder_mutex);
		// Search for the most recent frame available on all decoders.
		// If no such frame exists, use the most latest frame for each decoder
		auto common_frame = accumulator_images::common_frame(decoders);
		// Blit images from the decoders
		// TODO be smarter: group blits per eye, so that the framebuffer can use OP_DONT_CARE instead of OP_LOAD and use the same renderpass if possible
		for (auto & i: decoders)
		{
			auto blit_handle = i.frame(common_frame);
			if (not blit_handle)
				continue;

			current_blit_handles.push_back(blit_handle);

			blit_handle->feedback.blitted = application::now();
			blit_handle->feedback.displayed = framestate.predictedDisplayTime;
			blit_handle->feedback.real_pose[0] = views[0].pose;
			blit_handle->feedback.real_pose[1] = views[1].pose;

			pose = blit_handle->view_info.pose;
			g_last_pose = blit_handle->view_info.pose;
			g_pose_present = true;
			fov = blit_handle->view_info.fov;

			send_feedback(blit_handle->feedback);

			int indices[] = {0, 1};
			i.decoder->blit(command_buffer, *blit_handle, indices); // TODO blit indices no longer needed here
		}
	}

	// Transition the output of the decoder to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	image_barriers.clear();
	for (size_t i = 0; i < decoder_output.size(); i++)
	{
		vk::ImageMemoryBarrier barrier;
		barrier.srcAccessMask = vk::AccessFlagBits::eNone;
		barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
		barrier.oldLayout = shard_accumulator::framebuffer_expected_layout;
		barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		barrier.image = (vk::Image)decoder_output[i].image;
		barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		image_barriers.push_back(barrier);
	}
	command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader, vk::DependencyFlags{}, {}, {}, image_barriers);

	// Reproject the image to the real pose
	for (size_t view = 0; view < view_count; view++)
	{
		size_t destination_index = view * swapchains[0].images().size() + image_indices[view];
		reprojector->reproject(command_buffer, view, destination_index, pose[view].orientation, fov[view], pose[view].orientation, views[view].fov);
//		reprojector->reproject(command_buffer, view, destination_index, pose[view].orientation, fov[view], views[view].pose.orientation, views[view].fov);
	}

	command_buffer.end();
	vk::SubmitInfo submit_info;
	submit_info.setCommandBuffers(*command_buffer);
	queue.submit(submit_info, *fence);

	std::vector<XrCompositionLayerBaseHeader *> layers_base;
	std::vector<XrCompositionLayerProjectionView> layer_view;
	layer_view.resize(views.size());

	for (size_t swapchain_index = 0; swapchain_index < views.size(); swapchain_index++)
	{
		swapchains[swapchain_index].release();

		layer_view[swapchain_index].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;

//		layer_view[swapchain_index].pose = views[swapchain_index].pose;
		layer_view[swapchain_index].pose = g_last_pose[swapchain_index];

		layer_view[swapchain_index].fov = views[swapchain_index].fov;

		layer_view[swapchain_index].subImage.swapchain = swapchains[swapchain_index];
		layer_view[swapchain_index].subImage.imageRect.offset = {0, 0};
		layer_view[swapchain_index].subImage.imageRect.extent.width = swapchains[swapchain_index].width();
		layer_view[swapchain_index].subImage.imageRect.extent.height = swapchains[swapchain_index].height();
	}

	float brightness = std::clamp<float>(dbrightness * (framestate.predictedDisplayTime - first_frame_time) / 1.e9, 0, 1);

	XrCompositionLayerColorScaleBiasKHR color_scale_bias{
	        .type = XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR,
	        .colorScale = {brightness, brightness, brightness, 1},
	        .colorBias = {}};

	XrCompositionLayerProjection layer{
	        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
	        .layerFlags = 0,
	        .space = world_space,
	        .viewCount = (uint32_t)layer_view.size(),
	        .views = layer_view.data(),
	};

	if (instance.has_extension(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME))
		layer.next = &color_scale_bias;

	layers_base.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&layer));
	session.end_frame(/*timestamp*/ framestate.predictedDisplayTime, layers_base);

	if (device.waitForFences(*fence, VK_TRUE, UINT64_MAX) == vk::Result::eTimeout)
		throw std::runtime_error("Vulkan fence timeout");
	device.resetFences(*fence);

	// We don't need those after vkWaitForFences
	current_blit_handles.clear();

	read_actions();
}

void scenes::stream::exit()
{
	exiting = true;
	audio_handle.reset();
	shard_queue.close();
}

void scenes::stream::cleanup()
{
	// Assumes decoder_mutex is locked
	ready_ = false;
	decoders.clear();
}

void scenes::stream::setup(const to_headset::video_stream_description & description)
{
	std::unique_lock lock(decoder_mutex);

	cleanup();

	if (description.items.empty())
	{
		spdlog::info("Stopping video stream");
		return;
	}

	// Create outputs for the decoders
	const uint32_t width = description.width / view_count;
	const uint32_t height = description.height;

	VkExtent3D decoder_out_size{width, height, 1};
	for (size_t i = 0; i < view_count; i++)
	{
		decoder_output[i].format = vk::Format::eA8B8G8R8SrgbPack32;
		decoder_output[i].size.width = width;
		decoder_output[i].size.height = height;

		vk::ImageCreateInfo image_info;
		image_info.flags = vk::ImageCreateFlags{};
		image_info.imageType = vk::ImageType::e2D;
		image_info.format = vk::Format::eA8B8G8R8SrgbPack32;
		image_info.extent = decoder_out_size;
		image_info.mipLevels = 1;
		image_info.arrayLayers = 1;
		image_info.samples = vk::SampleCountFlagBits::e1;
		image_info.tiling = vk::ImageTiling::eOptimal;
		image_info.usage = vk::ImageUsageFlagBits::eSampled | shard_accumulator::framebuffer_usage;
		image_info.sharingMode = vk::SharingMode::eExclusive;
		image_info.initialLayout = vk::ImageLayout::eUndefined;

		VmaAllocationCreateInfo alloc_info{};
		alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

		decoder_output[i].image = image_allocation{image_info, alloc_info};

		vk::ImageViewCreateInfo image_view_info;

		image_view_info.image = (VkImage)decoder_output[i].image;
		image_view_info.viewType = vk::ImageViewType::e2D;
		image_view_info.format = vk::Format::eA8B8G8R8SrgbPack32;
		image_view_info.components.r = vk::ComponentSwizzle::eIdentity;
		image_view_info.components.g = vk::ComponentSwizzle::eIdentity;
		image_view_info.components.b = vk::ComponentSwizzle::eIdentity;
		image_view_info.components.a = vk::ComponentSwizzle::eIdentity;
		image_view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		image_view_info.subresourceRange.baseMipLevel = 0;
		image_view_info.subresourceRange.levelCount = 1;
		image_view_info.subresourceRange.baseArrayLayer = 0;
		image_view_info.subresourceRange.layerCount = 1;

		decoder_output[i].image_view = vk::raii::ImageView(device, image_view_info);
	}

	std::vector<shard_accumulator::blit_target> blit_targets;
	blit_targets.resize(view_count);

	for (size_t i = 0; i < view_count; i++)
	{
		blit_targets[i].image = (VkImage)decoder_output[i].image;
		blit_targets[i].image_view = *decoder_output[i].image_view;
		blit_targets[i].extent.width = width;
		blit_targets[i].extent.height = height;
		blit_targets[i].offset.x = width * i;
		blit_targets[i].offset.y = 0;
	}

	for (const auto & [stream_index, item]: utils::enumerate(description.items))
	{
		spdlog::info("Creating decoder size {}x{} offset {},{}", item.width, item.height, item.offset_x, item.offset_y);

		accumulator_images dec;
		dec.decoder = std::make_unique<shard_accumulator>(device, physical_device, item, description.fps, shared_from_this(), stream_index);
		dec.decoder->set_blit_targets(blit_targets, vk::Format::eA8B8G8R8SrgbPack32);

		decoders.push_back(std::move(dec));
	}

	spdlog::info("Initializing reprojector");
	vk::Extent2D extent = {swapchains[0].width(), swapchains[0].height()};
	std::vector<vk::Image> swapchain_images;
	for (auto & swapchain: swapchains)
	{
		for (auto & image: swapchain.images())
			swapchain_images.push_back(image.image);
	}

	std::vector<vk::Image> images;
	for (renderpass_output & i: decoder_output)
	{
		images.push_back((vk::Image)i.image);
	}

	reprojector.emplace(device, physical_device, images, swapchain_images, extent, swapchains[0].format(), description);
}

void scenes::stream::video()
{
#ifdef XR_USE_PLATFORM_ANDROID
	application::instance().setup_jni();
#endif

	while (not exiting)
	{
		try
		{
			auto shard = shard_queue.pop();

			if (shard.stream_item_idx >= decoders.size())
			{
				// We don't know (yet?) about this stream, ignore packet
				return;
			}
			auto idx = shard.stream_item_idx;
			decoders[idx].decoder->push_shard(std::move(shard));
		}
		catch (utils::sync_queue_closed &)
		{
			break;
		}
		catch (std::exception & e)
		{
			spdlog::error("Exception in video thread: {}", e.what());
			exit();
		}
	}
}
