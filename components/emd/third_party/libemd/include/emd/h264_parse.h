#pragma once
#ifndef EMD_H264_PARSE_H
#define EMD_H264_PARSE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define EMD_H264_OK          0
#define EMD_H264_ERR        -1
#define EMD_H264_NEED_MORE  -2  /* truncated input */

/* NAL unit types (ITU-T H.264, Table 7-1) */
typedef enum {
    H264_NAL_UNSPEC       = 0,
    H264_NAL_SLICE        = 1,   /* non-IDR slice */
    H264_NAL_DPA          = 2,
    H264_NAL_DPB          = 3,
    H264_NAL_DPC          = 4,
    H264_NAL_IDR_SLICE    = 5,
    H264_NAL_SEI          = 6,
    H264_NAL_SPS          = 7,
    H264_NAL_PPS          = 8,
    H264_NAL_AUD          = 9,
    H264_NAL_END_SEQ      = 10,
    H264_NAL_END_STREAM   = 11,
    H264_NAL_FILLER       = 12,
    H264_NAL_SPS_EXT      = 13,
    H264_NAL_PREFIX       = 14,
    H264_NAL_SUB_SPS      = 15,
    H264_NAL_SLICE_AUX    = 19,
    H264_NAL_SLICE_EXT    = 20,
} emd_h264_nal_type_t;

/* Slice types */
typedef enum {
    H264_SLICE_P  = 0,
    H264_SLICE_B  = 1,
    H264_SLICE_I  = 2,
    H264_SLICE_SP = 3,
    H264_SLICE_SI = 4,
    /* mod-5 variants */
    H264_SLICE_P2  = 5,
    H264_SLICE_B2  = 6,
    H264_SLICE_I2  = 7,
    H264_SLICE_SP2 = 8,
    H264_SLICE_SI2 = 9,
} emd_h264_slice_type_t;

/* Exp-Golomb bitstream reader */
typedef struct {
    const uint8_t *data;
    size_t         len;      /* total bytes */
    size_t         byte_pos;
    int            bit_pos;  /* 7..0 within current byte */
} emd_bitreader_t;

void     emd_bitreader_init(emd_bitreader_t *br, const uint8_t *data, size_t len);
int      emd_br_read_bit(emd_bitreader_t *br);
uint32_t emd_br_read_bits(emd_bitreader_t *br, int n);
uint32_t emd_br_read_ue(emd_bitreader_t *br);    /* unsigned exp-Golomb */
int32_t  emd_br_read_se(emd_bitreader_t *br);    /* signed exp-Golomb */
bool     emd_br_eof(const emd_bitreader_t *br);

/* H.264 SPS */
typedef struct {
    uint8_t  profile_idc;
    uint8_t  level_idc;
    uint8_t  constraint_flags;
    uint32_t seq_parameter_set_id;
    uint32_t chroma_format_idc;
    uint32_t bit_depth_luma_minus8;
    uint32_t bit_depth_chroma_minus8;
    bool     frame_mbs_only_flag;
    uint32_t pic_width_in_mbs_minus1;
    uint32_t pic_height_in_map_units_minus1;
    uint32_t frame_crop_left_offset;
    uint32_t frame_crop_right_offset;
    uint32_t frame_crop_top_offset;
    uint32_t frame_crop_bottom_offset;
    bool     frame_cropping_flag;
    /* timing (optional) */
    bool     timing_info_present;
    uint32_t num_units_in_tick;
    uint32_t time_scale;
    bool     fixed_frame_rate_flag;
    /* VUI */
    bool     vui_present;
    /* entropy coding */
    uint8_t  entropy_coding_mode_flag; /* in PPS, cached here */

    /* Derived */
    uint32_t pic_width_px;
    uint32_t pic_height_px;
    uint32_t mb_width;
    uint32_t mb_height;
} emd_h264_sps_t;

/* H.264 PPS */
typedef struct {
    uint32_t pic_parameter_set_id;
    uint32_t seq_parameter_set_id;
    uint8_t  entropy_coding_mode_flag;
    int32_t  pic_init_qp_minus26;
} emd_h264_pps_t;

/* H.264 slice header (abbreviated — only fields needed for the inspector) */
typedef struct {
    emd_h264_slice_type_t slice_type;
    uint32_t pic_parameter_set_id;
    uint32_t frame_num;
    bool     idr_flag;
    int32_t  slice_qp_delta;
    uint32_t mb_skip_run;          /* leading mb_skip_run from CAVLC, 0 for CABAC */
    bool     cabac;
} emd_h264_slice_hdr_t;

/* SPS/PPS cache (per stream, supports up to 32 of each) */
typedef struct {
    emd_h264_sps_t sps[32];
    bool           sps_valid[32];
    emd_h264_pps_t pps[256];
    bool           pps_valid[256];
} emd_h264_param_cache_t;

void emd_h264_param_cache_init(emd_h264_param_cache_t *cache);

/* Parse a raw NAL unit (RBSP, without start code or length prefix).
 * nal_type is the NAL type byte (masked to low 5 bits).
 */
int emd_h264_parse_sps(const uint8_t *rbsp, size_t len, emd_h264_sps_t *sps);
int emd_h264_parse_pps(const uint8_t *rbsp, size_t len, emd_h264_pps_t *pps);
int emd_h264_parse_slice_header(const uint8_t *rbsp, size_t len,
                                 const emd_h264_param_cache_t *cache,
                                 emd_h264_slice_hdr_t *hdr);

/* Feed a NAL unit into the cache (handles SPS/PPS automatically). */
int emd_h264_cache_nal(emd_h264_param_cache_t *cache,
                        const uint8_t *nal, size_t len);

/* Byte-de-emulation (remove emulation prevention bytes 0x03). */
size_t emd_h264_remove_emulation(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_len);

/* Return the NAL type of a raw NAL unit (first byte, bits [4:0]). */
static inline uint8_t emd_h264_nal_type(const uint8_t *nal, size_t len) {
    if (len < 1) return 0;
    return nal[0] & 0x1fu;
}

static inline bool emd_h264_is_keyframe(uint8_t nal_type_val) {
    return nal_type_val == H264_NAL_IDR_SLICE;
}

static inline bool emd_h264_is_paramset(uint8_t nal_type_val) {
    return nal_type_val == H264_NAL_SPS || nal_type_val == H264_NAL_PPS;
}

#ifdef __cplusplus
}
#endif

#endif /* EMD_H264_PARSE_H */
