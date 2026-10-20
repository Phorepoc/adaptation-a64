/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-1.0 */

// ve_h264enc.h -- OURS (VE encoder one-frame probe): the CPU half of the A64 Video
// Engine H.264 encoder -- the NAL headers (SPS / PPS / IDR slice header) pushed bit by bit through the hardware bit writer
// (AVC_BASIC_BITS + AVC_TRIGGER), and the register offsets of the AVC encoder block and the ISP input block.
// Register facts from jemk/cedrus h264enc (the A64 map is unproven until the probe runs).
// Written from H.264 (08/2021) sections 7.3.2.1.1, 7.3.2.2, 7.3.3. Plain C++, no Symbian dependency.
#ifndef VE_H264ENC_H
#define VE_H264ENC_H

#include <stdint.h>

namespace h264enc
{

enum
	{
	KVeCtrl = 0x000, KVeVersion = 0x0F0,
	// ISP input block
	KIspSize = 0xA00, KIspStride = 0xA04, KIspCtrl = 0xA08, KIspLuma = 0xA78, KIspChroma = 0xA7C,
	// AVC encoder block
	KAvcParam = 0xB04, KAvcQp = 0xB08, KAvcMotionEst = 0xB10, KAvcCtrl = 0xB14, KAvcTrigger = 0xB18, KAvcStatus = 0xB1C,
	KAvcBasicBits = 0xB20, KAvcUnkBuf = 0xB60, KAvcVleAddr = 0xB80, KAvcVleEnd = 0xB84, KAvcVleOffset = 0xB88,
	KAvcVleMax = 0xB8C, KAvcVleLength = 0xB90, KAvcRefLuma = 0xBA0, KAvcRefChroma = 0xBA4, KAvcRecLuma = 0xBB0,
	KAvcRecChroma = 0xBB4, KAvcRefSluma = 0xBB8, KAvcRecSluma = 0xBBC, KAvcMbInfo = 0xBC0
	};

// where the header bits go: the hardware bit writer (the test) or a memory buffer (a host test)
class MBitSink
	{
public:
	virtual void Put(uint32_t aValue, int aBits) = 0;	// 1..31 bits, MSB first
	virtual void NoEmulationPrevention(bool aOff) = 0;	// AVC_PARAM bit 31 around a start code
	};

// the headers of ONE intra picture: 4:2:0 8-bit, CAVLC, Main profile level 4.1, POC type 2, one slice
class THeaders
	{
public:
	THeaders(MBitSink& aSink, int aMbW, int aMbH, int aQp) : iSink(aSink), iMbW(aMbW), iMbH(aMbH), iQp(aQp), iBits(0) {}
	void Sps()
		{
		Start(3, 7);
		Bits(77, 8);					// profile_idc: Main
		Bits(0, 8);						// constraint flags + reserved
		Bits(41, 8);					// level_idc 4.1
		Ue(0);							// sps_id
		Ue(0);							// log2_max_frame_num_minus4 (frame_num is 4 bits)
		Ue(2);							// pic_order_cnt_type 2: output order = decode order
		Ue(1);							// max_num_ref_frames
		Bits(0, 1);						// gaps_in_frame_num_value_allowed_flag
		Ue(iMbW - 1);					// pic_width_in_mbs_minus1
		Ue(iMbH - 1);					// pic_height_in_map_units_minus1
		Bits(1, 1);						// frame_mbs_only_flag
		Bits(1, 1);						// direct_8x8_inference_flag
		Bits(0, 1);						// frame_cropping_flag
		Bits(0, 1);						// vui_parameters_present_flag
		Trailing();
		}
	void Pps()
		{
		Start(3, 8);
		Ue(0);							// pps_id
		Ue(0);							// sps_id
		Bits(0, 1);						// entropy_coding_mode_flag: CAVLC
		Bits(0, 1);						// bottom_field_pic_order_in_frame_present_flag
		Ue(0);							// num_slice_groups_minus1
		Ue(0);							// num_ref_idx_l0_default_active_minus1
		Ue(0);							// num_ref_idx_l1_default_active_minus1
		Bits(0, 1);						// weighted_pred_flag
		Bits(0, 2);						// weighted_bipred_idc
		Se(iQp - 26);					// pic_init_qp_minus26
		Se(0);							// pic_init_qs_minus26
		Se(4);							// chroma_qp_index_offset (the 4 in AVC_QP)
		Bits(1, 1);						// deblocking_filter_control_present_flag
		Bits(0, 1);						// constrained_intra_pred_flag
		Bits(0, 1);						// redundant_pic_cnt_present_flag
		Trailing();
		}
	// the IDR slice header; the hardware appends the macroblock data at this bit position (no trailing bits here)
	void IdrSliceHeader()
		{
		Start(3, 5);
		Ue(0);							// first_mb_in_slice
		Ue(7);							// slice_type: I (all slices of the picture)
		Ue(0);							// pps_id
		Bits(0, 4);						// frame_num
		Ue(0);							// idr_pic_id
		Bits(0, 1);						// no_output_of_prior_pics_flag
		Bits(0, 1);						// long_term_reference_flag
		Se(0);							// slice_qp_delta
		Ue(0);							// disable_deblocking_filter_idc: filter on
		Se(0);							// slice_alpha_c0_offset_div2
		Se(0);							// slice_beta_offset_div2
		}
	// phevenctest2: the slice header of an IDR (aIdr) or a non-IDR P picture (all slices P, one reference frame, POC type 2);
	// frame_num is 4 bits (SPS log2_max_frame_num_minus4 = 0); aQpDelta = slice QP - PPS pic_init_qp.
	// Syntax H.264 7.3.3; the P-only elements as Bootlin cedrus_enc_h264.c lines 1021-1041.
	void SliceHeader(bool aIdr, int aFrameNum, int aQpDelta, int aDeblockIdc = 0)
		{
		Start(3, aIdr ? 5 : 1);
		Ue(0);							// first_mb_in_slice
		Ue(aIdr ? 7 : 5);				// slice_type: I / P (all slices of the picture)
		Ue(0);							// pps_id
		Bits(aFrameNum & 15, 4);		// frame_num
		if (aIdr)
			Ue(0);						// idr_pic_id
		else
			{
			Bits(0, 1);					// num_ref_idx_active_override_flag
			Bits(0, 1);					// ref_pic_list_modification_flag_l0
			}
		if (aIdr)
			{
			Bits(0, 1);					// no_output_of_prior_pics_flag
			Bits(0, 1);					// long_term_reference_flag
			}
		else
			Bits(0, 1);					// adaptive_ref_pic_marking_mode_flag (sliding window)
		Se(aQpDelta);					// slice_qp_delta
		Ue(aDeblockIdc);				// disable_deblocking_filter_idc (0 = filter on, 1 = off)
		if (aDeblockIdc != 1)
			{
			Se(0);
			Se(0);
			}
		}
	int BitCount() const { return iBits; }	// header bits since the last Start
private:
	void Start(int aRefIdc, int aType)
		{
		iSink.NoEmulationPrevention(true);
		Bits(0, 24);					// 00 00 00
		Bits(0x100 | (aRefIdc << 5) | aType, 16);	// 01 + the NAL header byte
		iSink.NoEmulationPrevention(false);
		iBits = 0;
		}
	void Bits(uint32_t aV, int aN)
		{
		iSink.Put(aV, aN);
		iBits += aN;
		}
	void Ue(uint32_t aV)
		{
		const uint32_t x = aV + 1;
		int len = 0;
		while ((x >> len) > 1)
			++len;
		if (len)
			Bits(0, len);				// len leading zeros, then x in len+1 bits
		Bits(x, len + 1);
		}
	void Se(int32_t aV) { Ue(aV > 0 ? 2 * aV - 1 : -2 * aV); }
	void Trailing()
		{
		Bits(1, 1);						// rbsp_stop_one_bit
		if (iBits & 7)
			Bits(0, 8 - (iBits & 7));	// alignment zeros (iBits counts the payload bits of this NAL, from the NAL header)
		}
	MBitSink& iSink;
	int iMbW, iMbH, iQp, iBits;
	};

// CRC-32 (IEEE, reflected) over a byte range
inline uint32_t Crc32(const uint8_t* aP, int aN)
	{
	uint32_t c = 0xFFFFFFFFu;
	for (int i = 0; i < aN; ++i)
		{
		c ^= aP[i];
		for (int k = 0; k < 8; ++k)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
		}
	return ~c;
	}

} // namespace h264enc

#endif
