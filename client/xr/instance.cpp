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

#include "instance.h"

#include "xr.h"
#include "xr/details/enumerate.h"
#include <cassert>
#include <cstring>
#include <spdlog/spdlog.h>
#include <utility>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>

static XrBool32 debug_callback(
        XrDebugUtilsMessageSeverityFlagsEXT messageSeverity,
        XrDebugUtilsMessageTypeFlagsEXT messageTypes,
        const XrDebugUtilsMessengerCallbackDataEXT * callbackData,
        void * userData)
{
	spdlog::info("OpenXR debug message: severity={}, type={}, function={}, {}", messageSeverity, messageTypes, callbackData->functionName, callbackData->message);

	return XR_FALSE;
}

#if defined(XR_USE_PLATFORM_ANDROID)
xr::instance::instance(std::string_view application_name, void * applicationVM, void * applicationActivity, std::vector<const char *> extensions)
#else
xr::instance::instance(std::string_view application_name, std::vector<const char *> extensions)
#endif
{
#if defined(XR_USE_GRAPHICS_API_VULKAN)
	extensions.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
#else
#error Not implemented
#endif

#if defined(XR_USE_PLATFORM_ANDROID)
	extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif

	std::vector<const char *> layers;
	// TODO: runtime switch

	spdlog::info("Available OpenXR layers:");
	for (XrApiLayerProperties & i: xr::details::enumerate<XrApiLayerProperties>(xrEnumerateApiLayerProperties))
	{
		spdlog::info("    {}", i.layerName);
#ifndef NDEBUG
//		if (!strcmp(i.layerName, "XR_APILAYER_LUNARG_core_validation"))
//		{
//			layers.push_back("XR_APILAYER_LUNARG_core_validation");
//		}
#endif
	}

	spdlog::info("Available OpenXR extensions:");
	bool debug_utils_found = false;
	for (XrExtensionProperties & i:
	     xr::details::enumerate<XrExtensionProperties>(xrEnumerateInstanceExtensionProperties, nullptr))
	{
		spdlog::info("    {}", i.extensionName);
#ifndef NDEBUG
		if (!strcmp(i.extensionName, "XR_EXT_debug_utils"))
		{
//			debug_utils_found = true;
//			extensions.push_back("XR_EXT_debug_utils");
		}
#endif
	}

	spdlog::info("Using OpenXR extensions:");
	for (auto & i: extensions)
	{
		loaded_extensions.insert(i);
		spdlog::info("    {}", i);
	}

	XrInstanceCreateInfo create_info{
	        .type = XR_TYPE_INSTANCE_CREATE_INFO,
	        .applicationInfo = {.apiVersion = XR_CURRENT_API_VERSION},
	        .enabledApiLayerCount = (uint32_t)layers.size(),
	        .enabledApiLayerNames = layers.data(),
	        .enabledExtensionCount = (uint32_t)extensions.size(),
	        .enabledExtensionNames = extensions.data(),
	};
	strncpy(create_info.applicationInfo.applicationName, application_name.data(), sizeof(create_info.applicationInfo.applicationName) - 1);

#if defined(XR_USE_PLATFORM_ANDROID)
	XrInstanceCreateInfoAndroidKHR instanceCreateInfoAndroid{
	        .type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR,
	        .applicationVM = applicationVM,
	        .applicationActivity = applicationActivity,
	};
	create_info.next = &instanceCreateInfoAndroid;
#endif

	CHECK_XR(xrCreateInstance(&create_info, &id));
	assert(id != XR_NULL_HANDLE);

	if (debug_utils_found)
	{
		XrDebugUtilsMessengerCreateInfoEXT debug_messenger_info{
		        .type = XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		        .messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		        .messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT,
		        .userCallback = debug_callback};
		auto xrCreateDebugUtilsMessengerEXT = get_proc<PFN_xrCreateDebugUtilsMessengerEXT>("xrCreateDebugUtilsMessengerEXT");
		XrDebugUtilsMessengerEXT messenger;
		CHECK_XR(xrCreateDebugUtilsMessengerEXT(id, &debug_messenger_info, &messenger));
	}

	XrInstanceProperties prop{XR_TYPE_INSTANCE_PROPERTIES};
	CHECK_XR(xrGetInstanceProperties(id, &prop));
	// TODO: exception safety
	// 	if (!XR_SUCCEEDED(result))
	// 	{
	// 		xrDestroyInstance(id);
	// 		throw error("Cannot get instance properties", result);
	// 	}

	runtime_version = to_string(prop.runtimeVersion);
	runtime_name = prop.runtimeName;
}

std::string xr::instance::path_to_string(XrPath path)
{
	if (path == XR_NULL_PATH)
		return "XR_NULL_PATH";

	uint32_t length;
	std::string s;

	CHECK_XR(xrPathToString(id, path, 0, &length, nullptr));
	s.resize(length);
	CHECK_XR(xrPathToString(id, path, length, &length, s.data()));

	return s;
}

XrPath xr::instance::string_to_path(const std::string & path)
{
	XrPath p;
	CHECK_XR(xrStringToPath(id, path.c_str(), &p));

	return p;
}

bool xr::instance::poll_event(xr::event & buffer)
{
	buffer.header.type = XR_TYPE_EVENT_DATA_BUFFER;
	buffer.header.next = nullptr;
	XrResult result = CHECK_XR(xrPollEvent(id, &buffer.header));

	return result == XR_SUCCESS;
}

void xr::instance::suggest_bindings(const std::string & interaction_profile,
                                    std::vector<XrActionSuggestedBinding> & bindings)
{
	XrInteractionProfileSuggestedBinding suggested_binding{};
	suggested_binding.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	suggested_binding.interactionProfile = string_to_path(interaction_profile);
	suggested_binding.countSuggestedBindings = bindings.size();
	suggested_binding.suggestedBindings = bindings.data();
	CHECK_XR(xrSuggestInteractionProfileBindings(id, &suggested_binding));
}

xr::instance::~instance()
{
	if (id != XR_NULL_HANDLE)
		xrDestroyInstance(id);
}

XrTime xr::instance::now()
{
	static PFN_xrConvertTimespecTimeToTimeKHR xrConvertTimespecTimeToTimeKHR =
	        get_proc<PFN_xrConvertTimespecTimeToTimeKHR>("xrConvertTimespecTimeToTimeKHR");
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	XrTime res;
	CHECK_XR(xrConvertTimespecTimeToTimeKHR(id, &ts, &res));
	return res;
}

std::vector<XrExtensionProperties> xr::instance::extensions(const char * layer_name)
{
	return xr::details::enumerate<XrExtensionProperties>(xrEnumerateInstanceExtensionProperties, layer_name);
}
