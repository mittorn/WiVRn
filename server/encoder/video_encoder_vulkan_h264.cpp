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
#include "video_encoder_vulkan_h264.h"
#include "utils/wivrn_vk_bundle.h"

video_encoder_vulkan_h264::video_encoder_vulkan_h264(wivrn_vk_bundle & vk, vk::Extent2D extent) :
        video_encoder_vulkan(vk, extent),
        sps{
                .flags =
                        {
                                .constraint_set0_flag = 0,
                                .constraint_set1_flag = 0,
                                .constraint_set2_flag = 0,
                                .constraint_set3_flag = 0,
                                .constraint_set4_flag = 0,
                                .constraint_set5_flag = 0,
                                .direct_8x8_inference_flag = 0,
                                .mb_adaptive_frame_field_flag = 0,
                                .frame_mbs_only_flag = 1,
                                .delta_pic_order_always_zero_flag = 1,
                                .separate_colour_plane_flag = 0,
                                .gaps_in_frame_num_value_allowed_flag = 1,
                                .qpprime_y_zero_transform_bypass_flag = 0,
                                .frame_cropping_flag = (extent.width % 16) || (extent.height) % 16,
                                .seq_scaling_matrix_present_flag = 0,
                                .vui_parameters_present_flag = 0,
                        },
                .profile_idc = STD_VIDEO_H264_PROFILE_IDC_MAIN,
                .level_idc = STD_VIDEO_H264_LEVEL_IDC_5_0,
                .chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420,
                .seq_parameter_set_id = 0,
                .bit_depth_luma_minus8 = 0,
                .bit_depth_chroma_minus8 = 0,
                .log2_max_frame_num_minus4 = 12,
                .pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2,
                .offset_for_non_ref_pic = 0,
                .offset_for_top_to_bottom_field = 0,
                .log2_max_pic_order_cnt_lsb_minus4 = 0,
                .num_ref_frames_in_pic_order_cnt_cycle = 0,
                .max_num_ref_frames = 1,
                .reserved1 = 0,
                .pic_width_in_mbs_minus1 = (extent.width - 1) / 16,
                .pic_height_in_map_units_minus1 = (extent.height - 1) / 16,
                .frame_crop_left_offset = 0,
                .frame_crop_right_offset = (extent.width % 16) / 2,
                .frame_crop_top_offset = 0,
                .frame_crop_bottom_offset = (extent.height % 16) / 2,
                .reserved2 = 0,
                .pOffsetForRefFrame = nullptr,
                .pScalingLists = nullptr,
                .pSequenceParameterSetVui = nullptr,
        },
        pps{
                .flags =
                        {
                                .transform_8x8_mode_flag = 0,
                                .redundant_pic_cnt_present_flag = 1,
                                .constrained_intra_pred_flag = 0,
                                .deblocking_filter_control_present_flag = 0,
                                .weighted_pred_flag = 0,
                                .bottom_field_pic_order_in_frame_present_flag = 0,
                                .entropy_coding_mode_flag = 1,
                                .pic_scaling_matrix_present_flag = 0,
                        },
                .seq_parameter_set_id = 0,
                .pic_parameter_set_id = 0,
                .num_ref_idx_l0_default_active_minus1 = 0,
                .num_ref_idx_l1_default_active_minus1 = 0,
                .weighted_bipred_idc = STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_DEFAULT,
                .pic_init_qp_minus26 = 0,
                .pic_init_qs_minus26 = 0,
                .chroma_qp_index_offset = 0,
                .second_chroma_qp_index_offset = 0,
                .pScalingLists = nullptr,
        }
{}

std::vector<void *> video_encoder_vulkan_h264::setup_slot_info(size_t dpb_size)
{
	dpb_std_info.resize(dpb_size, {});
	dpb_std_slots.reserve(dpb_size);
	std::vector<void *> res;
	for (size_t i = 0; i < dpb_size; ++i)
	{
		dpb_std_slots.push_back({
		        .pStdReferenceInfo = &dpb_std_info[i],
		});
		res.push_back(&dpb_std_slots[i]);
	}

	return res;
}

std::unique_ptr<video_encoder_vulkan_h264> video_encoder_vulkan_h264::create(
        wivrn_vk_bundle & vk,
        const vk::Extent2D & extent)
{
	std::unique_ptr<video_encoder_vulkan_h264> self(new video_encoder_vulkan_h264(vk, extent));

	vk::StructureChain video_profile_info{
	        vk::VideoProfileInfoKHR{
	                .videoCodecOperation =
	                        vk::VideoCodecOperationFlagBitsKHR::eEncodeH264,
	                .chromaSubsampling = vk::VideoChromaSubsamplingFlagBitsKHR::e420,
	                .lumaBitDepth = vk::VideoComponentBitDepthFlagBitsKHR::e8,
	                .chromaBitDepth = vk::VideoComponentBitDepthFlagBitsKHR::e8,
	        },
	        vk::VideoEncodeH264ProfileInfoKHR{
	                .stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN,
	        },
	        vk::VideoEncodeUsageInfoKHR{
	                .videoUsageHints = vk::VideoEncodeUsageFlagBitsKHR::eStreaming,
	                .videoContentHints = vk::VideoEncodeContentFlagBitsKHR::eRendered,
	                .tuningMode = vk::VideoEncodeTuningModeKHR::eUltraLowLatency,
	        }};

	vk::VideoEncodeH264SessionParametersAddInfoKHR h264_add_info{};
	h264_add_info.setStdSPSs(self->sps);
	h264_add_info.setStdPPSs(self->pps);

	vk::VideoEncodeH264SessionParametersCreateInfoKHR h264_session_params{
	        .maxStdSPSCount = 1,
	        .maxStdPPSCount = 1,
	        .pParametersAddInfo = &h264_add_info,
	};

	auto [video_caps, encode_caps, encode_h264_caps] =
	        vk.physical_device.getVideoCapabilitiesKHR<
	                vk::VideoCapabilitiesKHR,
	                vk::VideoEncodeCapabilitiesKHR,
	                vk::VideoEncodeH264CapabilitiesKHR>(video_profile_info.get());

	vk::VideoEncodeH264SessionCreateInfoKHR session_create_info{
	        .useMaxLevelIdc = false,
	};

	self->init(video_caps, encode_caps, video_profile_info.get(), &session_create_info, &h264_session_params);

	return self;
}

std::vector<uint8_t> video_encoder_vulkan_h264::get_sps_pps()
{
	vk::VideoEncodeH264SessionParametersGetInfoKHR next{
	        .writeStdSPS = true,
	        .writeStdPPS = true,
	};
	return get_encoded_parameters(&next);
}

void video_encoder_vulkan_h264::send_idr_data()
{
	auto data = get_sps_pps();
	SendData(data, false);
}

void * video_encoder_vulkan_h264::encode_info_next(uint32_t frame_num, size_t slot, std::optional<size_t> ref)
{
	slice_header = {
	        .flags =
	                {
	                        .direct_spatial_mv_pred_flag = 0, //?
	                        .num_ref_idx_active_override_flag = 0,
	                        .reserved = 0,
	                },
	        .first_mb_in_slice = 0,
	        .slice_type = ref ? STD_VIDEO_H264_SLICE_TYPE_P
	                          : STD_VIDEO_H264_SLICE_TYPE_I,
	        .slice_alpha_c0_offset_div2 = 0,
	        .slice_beta_offset_div2 = 0,
	        .slice_qp_delta = 0,
	        .reserved1 = 0,
	        .cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0,
	        .disable_deblocking_filter_idc =
	                STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED,
	        .pWeightTable = nullptr,
	};
	nalu_slice_info = vk::VideoEncodeH264NaluSliceInfoKHR{
	        .pStdSliceHeader = &slice_header,
	};
	reference_lists_info = {
	        .flags =
	                {
	                        .ref_pic_list_modification_flag_l0 = 0,
	                        .ref_pic_list_modification_flag_l1 = 0,
	                        .reserved = 0,
	                },
	        .num_ref_idx_l0_active_minus1 = 0,
	        .num_ref_idx_l1_active_minus1 = 0,
	        .RefPicList0 = {},
	        .RefPicList1 = {},
	        .refList0ModOpCount = 0,
	        .refList1ModOpCount = 0,
	        .refPicMarkingOpCount = 0,
	        .reserved1 = {},
	        .pRefList0ModOperations = nullptr,
	        .pRefList1ModOperations = nullptr,
	        .pRefPicMarkingOperations = nullptr,
	};
	std::fill(reference_lists_info.RefPicList0,
	          reference_lists_info.RefPicList0 + sizeof(reference_lists_info.RefPicList0),
	          STD_VIDEO_H264_NO_REFERENCE_PICTURE);
	std::fill(reference_lists_info.RefPicList1,
	          reference_lists_info.RefPicList1 + sizeof(reference_lists_info.RefPicList1),
	          STD_VIDEO_H264_NO_REFERENCE_PICTURE);
	if (ref)
	{
		reference_lists_info.RefPicList0[0] = *ref;
	}

	std_picture_info = {
	        .flags =
	                {
	                        .IdrPicFlag = uint32_t(ref ? 0 : 1),
	                        .is_reference = 1,
	                        .no_output_of_prior_pics_flag = 0,
	                        .long_term_reference_flag = 0,
	                        .adaptive_ref_pic_marking_mode_flag = 0,
	                        .reserved = 0,
	                },
	        .seq_parameter_set_id = 0,
	        .pic_parameter_set_id = 0,
	        .idr_pic_id = idr_id,
	        .primary_pic_type = ref ? STD_VIDEO_H264_PICTURE_TYPE_P
	                                : STD_VIDEO_H264_PICTURE_TYPE_IDR,
	        .frame_num = frame_num,
	        .PicOrderCnt = 0,
	        .temporal_id = 0,
	        .reserved1 = {},
	        .pRefLists = &reference_lists_info,
	};
	picture_info = vk::VideoEncodeH264PictureInfoKHR{
	        .naluSliceEntryCount = 1,
	        .pNaluSliceEntries = &nalu_slice_info,
	        .pStdPictureInfo = &std_picture_info,
	        .generatePrefixNalu = false, // check if useful, check if supported
	};

	dpb_std_info[slot].primary_pic_type = std_picture_info.primary_pic_type;
	dpb_std_info[slot].FrameNum = frame_num;

	if (not ref)
		++idr_id;

	return &picture_info;
}
vk::ExtensionProperties video_encoder_vulkan_h264::std_header_version()
{
	// FIXME: update to version 1.0
	vk::ExtensionProperties std_header_version{
	        .specVersion = 0x0000900b, // VK_MAKE_VIDEO_STD_VERSION(1, 0, 0),
	};
	strcpy(std_header_version.extensionName,
	       VK_STD_VULKAN_VIDEO_CODEC_H264_ENCODE_EXTENSION_NAME);
	return std_header_version;
}
