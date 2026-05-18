#include "emd/h265_parse.h"
#include "emd/h264_parse.h"  /* reuse bitreader and emulation removal */
#include "emd/log.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Emulation prevention (same logic as H.264)
 * ---------------------------------------------------------------------- */
size_t emd_h265_remove_emulation(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_len)
{
    return emd_h264_remove_emulation(src, src_len, dst, dst_len);
}

/* -------------------------------------------------------------------------
 * VPS
 * ---------------------------------------------------------------------- */
int emd_h265_parse_vps(const uint8_t *rbsp, size_t len, emd_h265_vps_t *vps) {
    if (!rbsp || len < 3 || !vps) return EMD_H265_ERR;
    memset(vps, 0, sizeof(*vps));

    emd_bitreader_t br;
    emd_bitreader_init(&br, rbsp, len);

    vps->vps_id              = (uint8_t)emd_br_read_bits(&br, 4);
    emd_br_read_bit(&br); /* base_layer_internal_flag */
    emd_br_read_bit(&br); /* base_layer_available_flag */
    vps->max_layers_minus1   = (uint8_t)emd_br_read_bits(&br, 6);
    vps->max_sub_layers_minus1 = (uint8_t)emd_br_read_bits(&br, 3);
    /* rest not needed */
    return EMD_H265_OK;
}

/* -------------------------------------------------------------------------
 * SPS
 * ---------------------------------------------------------------------- */
int emd_h265_parse_sps(const uint8_t *rbsp, size_t len, emd_h265_sps_t *sps) {
    if (!rbsp || len < 4 || !sps) return EMD_H265_ERR;
    memset(sps, 0, sizeof(*sps));

    emd_bitreader_t br;
    emd_bitreader_init(&br, rbsp, len);

    sps->vps_id               = (uint8_t)emd_br_read_bits(&br, 4);
    sps->max_sub_layers_minus1= (uint8_t)emd_br_read_bits(&br, 3);
    emd_br_read_bit(&br); /* temporal_id_nesting */

    /* profile_tier_level */
    emd_br_read_bits(&br, 2); /* general_profile_space */
    emd_br_read_bit(&br);     /* general_tier_flag */
    emd_br_read_bits(&br, 5); /* general_profile_idc */
    emd_br_read_bits(&br, 32); /* general_profile_compatibility_flags */
    emd_br_read_bits(&br, 16); /* constraints */
    emd_br_read_bits(&br, 16); /* more constraints */
    emd_br_read_bits(&br, 8);  /* general_level_idc */

    /* sub-layer profile/level flags (simplified: skip them) */
    for (uint8_t i = 0; i < sps->max_sub_layers_minus1; i++) {
        emd_br_read_bit(&br); /* profile_present */
        emd_br_read_bit(&br); /* level_present */
    }
    /* alignment bits */
    if (sps->max_sub_layers_minus1 > 0) {
        for (uint8_t i = sps->max_sub_layers_minus1; i < 8; i++) {
            emd_br_read_bits(&br, 2); /* reserved */
        }
    }

    sps->sps_id = (uint8_t)emd_br_read_ue(&br);
    if (sps->sps_id >= 16) return EMD_H265_ERR;

    sps->chroma_format_idc = (uint8_t)emd_br_read_ue(&br);
    if (sps->chroma_format_idc == 3) emd_br_read_bit(&br); /* separate_colour */

    sps->pic_width_in_luma_samples  = emd_br_read_ue(&br);
    sps->pic_height_in_luma_samples = emd_br_read_ue(&br);
    sps->pic_width_px  = sps->pic_width_in_luma_samples;
    sps->pic_height_px = sps->pic_height_in_luma_samples;

    if (emd_br_read_bit(&br)) { /* conformance window */
        sps->conf_win_left_offset   = emd_br_read_ue(&br);
        sps->conf_win_right_offset  = emd_br_read_ue(&br);
        sps->conf_win_top_offset    = emd_br_read_ue(&br);
        sps->conf_win_bottom_offset = emd_br_read_ue(&br);
    }

    sps->bit_depth_luma_minus8 = (uint8_t)emd_br_read_ue(&br);
    emd_br_read_ue(&br); /* bit_depth_chroma */
    emd_br_read_ue(&br); /* log2_max_pic_order_cnt_lsb */

    uint8_t sub_layer_ordering_info = (uint8_t)emd_br_read_bit(&br);
    for (uint8_t i = sub_layer_ordering_info ? 0 : sps->max_sub_layers_minus1;
         i <= sps->max_sub_layers_minus1; i++) {
        emd_br_read_ue(&br); emd_br_read_ue(&br); emd_br_read_ue(&br);
    }

    sps->log2_min_luma_coding_block_size_minus3       = (uint8_t)emd_br_read_ue(&br);
    sps->log2_diff_max_min_luma_coding_block_size     = (uint8_t)emd_br_read_ue(&br);

    /* CTU size */
    uint32_t log2_ctu = 3 + sps->log2_min_luma_coding_block_size_minus3
                          + sps->log2_diff_max_min_luma_coding_block_size;
    sps->ctu_width  = 1u << log2_ctu;
    sps->ctu_height = sps->ctu_width;

    /* Skip rest for now */
    return EMD_H265_OK;
}

/* -------------------------------------------------------------------------
 * PPS
 * ---------------------------------------------------------------------- */
int emd_h265_parse_pps(const uint8_t *rbsp, size_t len, emd_h265_pps_t *pps) {
    if (!rbsp || len < 2 || !pps) return EMD_H265_ERR;
    memset(pps, 0, sizeof(*pps));

    emd_bitreader_t br;
    emd_bitreader_init(&br, rbsp, len);

    pps->pps_id = (uint8_t)emd_br_read_ue(&br);
    pps->sps_id = (uint8_t)emd_br_read_ue(&br);
    emd_br_read_bit(&br); /* dependent_slices_segments_enabled */
    emd_br_read_bit(&br); /* output_flag_present */
    emd_br_read_bits(&br, 3); /* num_extra_slice_header_bits */
    emd_br_read_bit(&br); /* sign_data_hiding */
    emd_br_read_bit(&br); /* cabac_init_present */
    emd_br_read_ue(&br); emd_br_read_ue(&br); /* ref_idx_l0/l1 */
    pps->init_qp_minus26 = emd_br_read_se(&br);
    /* rest not needed */
    return EMD_H265_OK;
}

/* -------------------------------------------------------------------------
 * Slice header
 * ---------------------------------------------------------------------- */
int emd_h265_parse_slice_header(const uint8_t *rbsp, size_t len,
                                 emd_h265_nal_type_t nal_type,
                                 const emd_h265_param_cache_t *cache,
                                 emd_h265_slice_hdr_t *hdr)
{
    if (!rbsp || len < 2 || !hdr) return EMD_H265_ERR;
    memset(hdr, 0, sizeof(*hdr));
    hdr->nal_type = nal_type;

    emd_bitreader_t br;
    /* Skip 2-byte NAL header */
    emd_bitreader_init(&br, rbsp + 2, len > 2 ? len - 2 : 0);

    hdr->first_slice_in_pic = (bool)emd_br_read_bit(&br);
    if (emd_h265_is_keyframe(nal_type)) emd_br_read_bit(&br); /* no_output_of_prior_pics */

    hdr->pps_id = (uint8_t)emd_br_read_ue(&br);

    if (!hdr->first_slice_in_pic) {
        /* dependent_slice_segment → skip address */
        emd_br_read_bit(&br);
    }

    /* pic_order_cnt_lsb: size = log2_max_pic_order_cnt_lsb_minus4+4 bits.
     * We don't cache that, so read 8 bits as a proxy. */
    if (!emd_h265_is_keyframe(nal_type)) {
        hdr->pic_order_cnt_lsb = emd_br_read_bits(&br, 8);
    }

    (void)cache;
    return EMD_H265_OK;
}

/* -------------------------------------------------------------------------
 * Cache
 * ---------------------------------------------------------------------- */
void emd_h265_param_cache_init(emd_h265_param_cache_t *cache) {
    memset(cache, 0, sizeof(*cache));
}

int emd_h265_cache_nal(emd_h265_param_cache_t *cache,
                        const uint8_t *nal, size_t len)
{
    if (!nal || len < 2) return EMD_H265_ERR;
    emd_h265_nal_type_t nt = emd_h265_nal_type(nal, len);

    static uint8_t rbsp[65536];
    /* Skip 2-byte NAL header */
    size_t rbsp_len = emd_h265_remove_emulation(nal + 2, len > 2 ? len - 2 : 0,
                                                 rbsp, sizeof(rbsp));

    if (nt == H265_NAL_VPS) {
        emd_h265_vps_t vps;
        if (emd_h265_parse_vps(rbsp, rbsp_len, &vps) == EMD_H265_OK) {
            if (vps.vps_id < 16) {
                cache->vps[vps.vps_id] = vps;
                cache->vps_valid[vps.vps_id] = true;
            }
        }
    } else if (nt == H265_NAL_SPS) {
        emd_h265_sps_t sps;
        if (emd_h265_parse_sps(rbsp, rbsp_len, &sps) == EMD_H265_OK) {
            if (sps.sps_id < 16) {
                cache->sps[sps.sps_id] = sps;
                cache->sps_valid[sps.sps_id] = true;
            }
        }
    } else if (nt == H265_NAL_PPS) {
        emd_h265_pps_t pps;
        if (emd_h265_parse_pps(rbsp, rbsp_len, &pps) == EMD_H265_OK) {
            if (pps.pps_id < 64) {
                cache->pps[pps.pps_id] = pps;
                cache->pps_valid[pps.pps_id] = true;
            }
        }
    }
    return EMD_H265_OK;
}
