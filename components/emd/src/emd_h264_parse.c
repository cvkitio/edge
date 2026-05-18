#include "emd/h264_parse.h"
#include "emd/log.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

/* -------------------------------------------------------------------------
 * Bit reader
 * ---------------------------------------------------------------------- */
void emd_bitreader_init(emd_bitreader_t *br, const uint8_t *data, size_t len) {
    br->data     = data;
    br->len      = len;
    br->byte_pos = 0;
    br->bit_pos  = 7;
}

bool emd_br_eof(const emd_bitreader_t *br) {
    return br->byte_pos >= br->len;
}

int emd_br_read_bit(emd_bitreader_t *br) {
    if (br->byte_pos >= br->len) return 0;
    int bit = (br->data[br->byte_pos] >> br->bit_pos) & 1;
    if (br->bit_pos == 0) {
        br->byte_pos++;
        br->bit_pos = 7;
    } else {
        br->bit_pos--;
    }
    return bit;
}

uint32_t emd_br_read_bits(emd_bitreader_t *br, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
        val = (val << 1) | (uint32_t)emd_br_read_bit(br);
    }
    return val;
}

/* Unsigned Exp-Golomb (ue(v)) */
uint32_t emd_br_read_ue(emd_bitreader_t *br) {
    int leading_zeros = 0;
    while (emd_br_read_bit(br) == 0 && leading_zeros < 31) {
        leading_zeros++;
    }
    if (leading_zeros == 0) return 0;
    uint32_t suffix = emd_br_read_bits(br, leading_zeros);
    return (uint32_t)((1 << leading_zeros) - 1 + suffix);
}

/* Signed Exp-Golomb (se(v))
 * Mapping: codeNum k → value:  k=0 → 0, k odd → -(k+1)/2, k even → k/2
 * (i.e. 1→-1, 2→+1, 3→-2, 4→+2, ...) */
int32_t emd_br_read_se(emd_bitreader_t *br) {
    uint32_t ue = emd_br_read_ue(br);
    if (ue == 0) return 0;
    int32_t val = (int32_t)((ue + 1) / 2);
    if ((ue & 1u) != 0) val = -val; /* odd codeNum → negative */
    return val;
}

/* -------------------------------------------------------------------------
 * Emulation prevention byte removal
 * ---------------------------------------------------------------------- */
size_t emd_h264_remove_emulation(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_len)
{
    size_t out = 0;
    for (size_t i = 0; i < src_len && out < dst_len; i++) {
        /* Remove 0x03 between 0x00 0x00 0x03 {0x00,0x01,0x02,0x03} */
        if (i >= 2 && src[i-2] == 0x00 && src[i-1] == 0x00 && src[i] == 0x03
            && i + 1 < src_len && src[i+1] <= 0x03) {
            /* skip the 0x03 */
            continue;
        }
        dst[out++] = src[i];
    }
    return out;
}

/* -------------------------------------------------------------------------
 * SPS parsing
 * ---------------------------------------------------------------------- */
int emd_h264_parse_sps(const uint8_t *rbsp, size_t len, emd_h264_sps_t *sps) {
    if (!rbsp || len < 4 || !sps) return EMD_H264_ERR;
    memset(sps, 0, sizeof(*sps));

    emd_bitreader_t br;
    emd_bitreader_init(&br, rbsp, len);

    /* Skip NAL header byte if present (nal_unit_type in low 5 bits) */
    /* Caller is expected to pass RBSP without NAL header */

    sps->profile_idc     = (uint8_t)emd_br_read_bits(&br, 8);
    sps->constraint_flags= (uint8_t)emd_br_read_bits(&br, 8);
    sps->level_idc       = (uint8_t)emd_br_read_bits(&br, 8);
    sps->seq_parameter_set_id = emd_br_read_ue(&br);

    if (sps->seq_parameter_set_id >= 32) return EMD_H264_ERR;

    /* chroma_format_idc etc. for extended profiles */
    if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
        sps->profile_idc == 122 || sps->profile_idc == 244 ||
        sps->profile_idc ==  44 || sps->profile_idc ==  83 ||
        sps->profile_idc ==  86 || sps->profile_idc == 118 ||
        sps->profile_idc == 128 || sps->profile_idc == 138 ||
        sps->profile_idc == 139 || sps->profile_idc == 134 ||
        sps->profile_idc == 135) {
        sps->chroma_format_idc = emd_br_read_ue(&br);
        if (sps->chroma_format_idc == 3) emd_br_read_bit(&br); /* separate_colour_plane */
        sps->bit_depth_luma_minus8   = emd_br_read_ue(&br);
        sps->bit_depth_chroma_minus8 = emd_br_read_ue(&br);
        emd_br_read_bit(&br); /* qpprime_y_zero_transform_bypass */
        int seq_scaling = emd_br_read_bit(&br); /* seq_scaling_matrix_present */
        if (seq_scaling) {
            int n_lists = (sps->chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < n_lists; i++) {
                if (emd_br_read_bit(&br)) { /* present */
                    /* skip scaling list */
                    int size = (i < 6) ? 16 : 64;
                    int next = 8, last = 8;
                    for (int j = 0; j < size; j++) {
                        if (next != 0) {
                            int delta = emd_br_read_se(&br);
                            next = (last + delta + 256) % 256;
                        }
                        last = (next == 0) ? last : next;
                    }
                }
            }
        }
    } else {
        sps->chroma_format_idc = 1;
    }

    emd_br_read_ue(&br); /* log2_max_frame_num_minus4 */
    uint32_t poc_type = emd_br_read_ue(&br);
    if (poc_type == 0) {
        emd_br_read_ue(&br); /* log2_max_pic_order_cnt_lsb_minus4 */
    } else if (poc_type == 1) {
        emd_br_read_bit(&br);
        emd_br_read_se(&br);
        emd_br_read_se(&br);
        uint32_t num_ref = emd_br_read_ue(&br);
        for (uint32_t i = 0; i < num_ref; i++) emd_br_read_se(&br);
    }
    emd_br_read_ue(&br); /* num_ref_frames */
    emd_br_read_bit(&br); /* gaps_in_frame_num_value_allowed */

    sps->pic_width_in_mbs_minus1         = emd_br_read_ue(&br);
    sps->pic_height_in_map_units_minus1  = emd_br_read_ue(&br);
    sps->frame_mbs_only_flag             = (bool)emd_br_read_bit(&br);

    if (!sps->frame_mbs_only_flag) emd_br_read_bit(&br); /* mb_adaptive */

    emd_br_read_bit(&br); /* direct_8x8 */

    sps->frame_cropping_flag = (bool)emd_br_read_bit(&br);
    if (sps->frame_cropping_flag) {
        sps->frame_crop_left_offset   = emd_br_read_ue(&br);
        sps->frame_crop_right_offset  = emd_br_read_ue(&br);
        sps->frame_crop_top_offset    = emd_br_read_ue(&br);
        sps->frame_crop_bottom_offset = emd_br_read_ue(&br);
    }

    sps->vui_present = (bool)emd_br_read_bit(&br);
    if (sps->vui_present) {
        /* aspect_ratio */
        if (emd_br_read_bit(&br)) {
            uint8_t ar_idc = (uint8_t)emd_br_read_bits(&br, 8);
            if (ar_idc == 255) { emd_br_read_bits(&br, 16); emd_br_read_bits(&br, 16); }
        }
        if (emd_br_read_bit(&br)) emd_br_read_bit(&br); /* overscan */
        if (emd_br_read_bit(&br)) { emd_br_read_bits(&br, 4); emd_br_read_bit(&br); if (emd_br_read_bit(&br)) emd_br_read_bits(&br, 24); }
        if (emd_br_read_bit(&br)) { emd_br_read_ue(&br); emd_br_read_ue(&br); } /* chroma_loc */
        sps->timing_info_present = (bool)emd_br_read_bit(&br);
        if (sps->timing_info_present) {
            sps->num_units_in_tick  = emd_br_read_bits(&br, 32);
            sps->time_scale         = emd_br_read_bits(&br, 32);
            sps->fixed_frame_rate_flag = (bool)emd_br_read_bit(&br);
        }
    }

    /* Derived dimensions */
    sps->mb_width  = sps->pic_width_in_mbs_minus1 + 1;
    sps->mb_height = (sps->pic_height_in_map_units_minus1 + 1) *
                     (sps->frame_mbs_only_flag ? 1u : 2u);
    sps->pic_width_px  = sps->mb_width * 16
                         - (sps->frame_crop_left_offset + sps->frame_crop_right_offset) * 2;
    sps->pic_height_px = sps->mb_height * 16
                         - (sps->frame_crop_top_offset + sps->frame_crop_bottom_offset) * 2;

    return EMD_H264_OK;
}

/* -------------------------------------------------------------------------
 * PPS parsing
 * ---------------------------------------------------------------------- */
int emd_h264_parse_pps(const uint8_t *rbsp, size_t len, emd_h264_pps_t *pps) {
    if (!rbsp || len < 2 || !pps) return EMD_H264_ERR;
    memset(pps, 0, sizeof(*pps));

    emd_bitreader_t br;
    emd_bitreader_init(&br, rbsp, len);

    pps->pic_parameter_set_id = emd_br_read_ue(&br);
    pps->seq_parameter_set_id = emd_br_read_ue(&br);
    pps->entropy_coding_mode_flag = (uint8_t)emd_br_read_bit(&br);
    emd_br_read_bit(&br); /* bottom_field_pic_order_in_frame_present */
    uint32_t num_slg = emd_br_read_ue(&br);
    if (num_slg) {
        emd_br_read_ue(&br); /* num_ref_idx_l0_default */
        emd_br_read_ue(&br);
        emd_br_read_bit(&br);
        emd_br_read_bit(&br);
    }
    emd_br_read_bit(&br); /* weighted_pred */
    emd_br_read_bits(&br, 2);
    pps->pic_init_qp_minus26 = emd_br_read_se(&br);
    /* rest not needed */
    (void)num_slg;

    return EMD_H264_OK;
}

/* -------------------------------------------------------------------------
 * Slice header parsing
 * ---------------------------------------------------------------------- */
int emd_h264_parse_slice_header(const uint8_t *rbsp, size_t len,
                                 const emd_h264_param_cache_t *cache,
                                 emd_h264_slice_hdr_t *hdr)
{
    if (!rbsp || len < 2 || !hdr) return EMD_H264_ERR;
    memset(hdr, 0, sizeof(*hdr));

    uint8_t nal_type = rbsp[0] & 0x1Fu;
    hdr->idr_flag = (nal_type == H264_NAL_IDR_SLICE);

    emd_bitreader_t br;
    /* Skip NAL header byte */
    emd_bitreader_init(&br, rbsp + 1, len - 1);

    emd_br_read_ue(&br); /* first_mb_in_slice */
    hdr->slice_type = (emd_h264_slice_type_t)emd_br_read_ue(&br);
    hdr->pic_parameter_set_id = emd_br_read_ue(&br);

    /* frame_num — width depends on SPS.log2_max_frame_num_minus4 + 4;
     * we don't store that, so read a reasonable default of up to 16 bits. */
    if (cache && hdr->pic_parameter_set_id < 256 &&
        cache->pps_valid[hdr->pic_parameter_set_id]) {
        const emd_h264_pps_t *pps = &cache->pps[hdr->pic_parameter_set_id];
        hdr->cabac = (pps->entropy_coding_mode_flag != 0);
        hdr->slice_qp_delta = 0; /* parse further if needed */
    }

    hdr->frame_num = emd_br_read_bits(&br, 4); /* simplified */

    /* For CAVLC non-IDR, try to read mb_skip_run (leading zeros) */
    if (!hdr->idr_flag && !hdr->cabac) {
        hdr->mb_skip_run = emd_br_read_ue(&br);
    }

    return EMD_H264_OK;
}

/* -------------------------------------------------------------------------
 * Cache
 * ---------------------------------------------------------------------- */
void emd_h264_param_cache_init(emd_h264_param_cache_t *cache) {
    memset(cache, 0, sizeof(*cache));
}

int emd_h264_cache_nal(emd_h264_param_cache_t *cache,
                        const uint8_t *nal, size_t len)
{
    if (!nal || len < 1) return EMD_H264_ERR;
    uint8_t nt = nal[0] & 0x1Fu;

    /* Remove emulation prevention bytes into a local buffer */
    static uint8_t rbsp[65536];
    size_t rbsp_len = emd_h264_remove_emulation(nal + 1, len - 1, rbsp, sizeof(rbsp));

    if (nt == H264_NAL_SPS) {
        emd_h264_sps_t sps;
        if (emd_h264_parse_sps(rbsp, rbsp_len, &sps) == EMD_H264_OK) {
            uint32_t id = sps.seq_parameter_set_id;
            if (id < 32) {
                cache->sps[id] = sps;
                cache->sps_valid[id] = true;
            }
        }
    } else if (nt == H264_NAL_PPS) {
        emd_h264_pps_t pps;
        if (emd_h264_parse_pps(rbsp, rbsp_len, &pps) == EMD_H264_OK) {
            uint32_t id = pps.pic_parameter_set_id;
            if (id < 256) {
                cache->pps[id] = pps;
                cache->pps_valid[id] = true;
            }
        }
    }
    return EMD_H264_OK;
}
