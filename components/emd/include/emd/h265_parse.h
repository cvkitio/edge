#pragma once
#ifndef EMD_H265_PARSE_H
#define EMD_H265_PARSE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMD_H265_OK         0
#define EMD_H265_ERR       -1
#define EMD_H265_NEED_MORE -2

/* NAL unit types (ITU-T H.265, Table 7-1) */
typedef enum {
    H265_NAL_TRAIL_N    = 0,
    H265_NAL_TRAIL_R    = 1,
    H265_NAL_TSA_N      = 2,
    H265_NAL_TSA_R      = 3,
    H265_NAL_STSA_N     = 4,
    H265_NAL_STSA_R     = 5,
    H265_NAL_RADL_N     = 6,
    H265_NAL_RADL_R     = 7,
    H265_NAL_RASL_N     = 8,
    H265_NAL_RASL_R     = 9,
    /* 10..15 reserved */
    H265_NAL_BLA_W_LP   = 16,
    H265_NAL_BLA_W_RADL = 17,
    H265_NAL_BLA_N_LP   = 18,
    H265_NAL_IDR_W_RADL = 19,
    H265_NAL_IDR_N_LP   = 20,
    H265_NAL_CRA        = 21,
    /* 22..31 reserved */
    H265_NAL_VPS        = 32,
    H265_NAL_SPS        = 33,
    H265_NAL_PPS        = 34,
    H265_NAL_AUD        = 35,
    H265_NAL_EOS        = 36,
    H265_NAL_EOB        = 37,
    H265_NAL_FD         = 38,
    H265_NAL_SEI_PREFIX = 39,
    H265_NAL_SEI_SUFFIX = 40,
} emd_h265_nal_type_t;

/* H.265 VPS (abbreviated) */
typedef struct {
    uint8_t  vps_id;
    uint8_t  max_layers_minus1;
    uint8_t  max_sub_layers_minus1;
} emd_h265_vps_t;

/* H.265 SPS (abbreviated - fields used by inspector) */
typedef struct {
    uint8_t  sps_id;
    uint8_t  vps_id;
    uint8_t  max_sub_layers_minus1;
    uint8_t  chroma_format_idc;
    uint32_t pic_width_in_luma_samples;
    uint32_t pic_height_in_luma_samples;
    uint32_t conf_win_left_offset;
    uint32_t conf_win_right_offset;
    uint32_t conf_win_top_offset;
    uint32_t conf_win_bottom_offset;
    uint8_t  bit_depth_luma_minus8;
    uint8_t  log2_min_luma_coding_block_size_minus3;
    uint8_t  log2_diff_max_min_luma_coding_block_size;
    /* Derived */
    uint32_t ctu_width;   /* in luma samples */
    uint32_t ctu_height;
    uint32_t pic_width_px;
    uint32_t pic_height_px;
    /* timing */
    bool     timing_present;
    uint32_t num_units_in_tick;
    uint32_t time_scale;
} emd_h265_sps_t;

/* H.265 PPS (abbreviated) */
typedef struct {
    uint8_t  pps_id;
    uint8_t  sps_id;
    int32_t  init_qp_minus26;
} emd_h265_pps_t;

/* H.265 slice header (abbreviated) */
typedef struct {
    emd_h265_nal_type_t nal_type;
    uint8_t             pps_id;
    bool                first_slice_in_pic;
    uint32_t            pic_order_cnt_lsb;
    int32_t             slice_qp_delta;
} emd_h265_slice_hdr_t;

/* Parameter set cache */
typedef struct {
    emd_h265_vps_t vps[16];
    bool           vps_valid[16];
    emd_h265_sps_t sps[16];
    bool           sps_valid[16];
    emd_h265_pps_t pps[64];
    bool           pps_valid[64];
} emd_h265_param_cache_t;

void emd_h265_param_cache_init(emd_h265_param_cache_t *cache);

/* Parse functions (raw RBSP, without 2-byte NAL header) */
int emd_h265_parse_vps(const uint8_t *rbsp, size_t len, emd_h265_vps_t *vps);
int emd_h265_parse_sps(const uint8_t *rbsp, size_t len, emd_h265_sps_t *sps);
int emd_h265_parse_pps(const uint8_t *rbsp, size_t len, emd_h265_pps_t *pps);
int emd_h265_parse_slice_header(const uint8_t *rbsp, size_t len,
                                 emd_h265_nal_type_t nal_type,
                                 const emd_h265_param_cache_t *cache,
                                 emd_h265_slice_hdr_t *hdr);

/* Feed a raw NAL into the cache (handles VPS/SPS/PPS) */
int emd_h265_cache_nal(emd_h265_param_cache_t *cache,
                        const uint8_t *nal, size_t len);

/* Emulation prevention removal */
size_t emd_h265_remove_emulation(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_len);

/* NAL type from a 2-byte header */
static inline emd_h265_nal_type_t emd_h265_nal_type(const uint8_t *nal, size_t len) {
    if (len < 2) return H265_NAL_TRAIL_N;
    return (emd_h265_nal_type_t)((nal[0] >> 1) & 0x3fu);
}

static inline bool emd_h265_is_keyframe(emd_h265_nal_type_t t) {
    return t == H265_NAL_IDR_W_RADL || t == H265_NAL_IDR_N_LP ||
           t == H265_NAL_CRA ||
           t == H265_NAL_BLA_W_LP || t == H265_NAL_BLA_W_RADL ||
           t == H265_NAL_BLA_N_LP;
}

static inline bool emd_h265_is_paramset(emd_h265_nal_type_t t) {
    return t == H265_NAL_VPS || t == H265_NAL_SPS || t == H265_NAL_PPS;
}

#ifdef __cplusplus
}
#endif

#endif /* EMD_H265_PARSE_H */
