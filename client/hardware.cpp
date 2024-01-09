/*
 * WiVRn VR streaming
 * Copyright (C) 2023  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2023  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "hardware.h"

#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

#ifdef XR_USE_PLATFORM_ANDROID
#include "utils/wrap_lambda.h"

#include <sys/system_properties.h>

static std::string get_property(const char * property)
{
	auto info = __system_property_find(property);
	std::string result;
	wrap_lambda cb = [&result](const char * name, const char * value, uint32_t serial) {
		result = value;
	};

	__system_property_read_callback(info, cb.userdata_first(), cb);
	return result;
}
#endif

model guess_model()
{
#ifdef XR_USE_PLATFORM_ANDROID
	const auto device = get_property("ro.product.device");
	if (device == "monterey")
		return model::oculus_quest;
	if (device == "hollywood")
		return model::oculus_quest_2;
	if (device == "seacliff")
		return model::meta_quest_pro;
	if (device == "eureka")
		return model::meta_quest_3;

	const auto manufacturer = get_property("ro.product.manufacturer");
	const auto model = get_property("ro.product.model");
	if (manufacturer == "Pico")
	{
		if (model == "Pico Neo 3")
			return model::pico_neo_3;

		spdlog::info("manufacturer={}, model={}, device={} assuming Pico 4", manufacturer, model, device);
		return model::pico_4;
	}

	spdlog::info("Unknown model, manufacturer={}, model={}, device={}", manufacturer, model, device);
#endif
	return model::unknown;
}

static XrViewConfigurationView scale_view(XrViewConfigurationView view, uint32_t width)
{
	double ratio = double(width) / view.recommendedImageRectWidth;
	view.recommendedImageRectWidth = width;
	view.recommendedImageRectHeight *= ratio;
	spdlog::info("Using panel size: {}x{}", view.recommendedImageRectWidth, view.recommendedImageRectHeight);
	return view;
}

XrViewConfigurationView override_view(XrViewConfigurationView view, model m)
{
	// Standalone headsets tend to report a lower resolution
	// as the GPU can't handle full res.
	// Return the panel resolution instead.
	spdlog::debug("Recommended image size: {}x{}", view.recommendedImageRectWidth, view.recommendedImageRectHeight);
	switch (m)
	{
		case model::oculus_quest:
			return scale_view(view, 1440);
		case model::oculus_quest_2:
		case model::pico_neo_3:
			return scale_view(view, 1832);
		case model::meta_quest_pro:
			return scale_view(view, 1800);
		case model::meta_quest_3:
			return scale_view(view, 2064);
		case model::pico_4:
			return scale_view(view, 2160);
		case model::unknown:
			return view;
	}
	throw std::range_error("invalid model " + std::to_string((int)m));
}
