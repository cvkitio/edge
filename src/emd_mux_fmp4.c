/*
 * emd_mux_fmp4.c — Minimal fragmented MP4 muxer.
 *
 * Produces:
 *  - ftyp box (major_brand=isom)
 *  - moov: mvhd, trak(tkhd, mdia{mdhd, hdlr, minf{vmhd, dinf, stbl{stsd{avc1/hvc1}, stts, stsc, stsz, stco}}})
 *  - moof + mdat per fragment
 *
 * References: ISO 14496-12.
 */

#include "emd/recorder.h"
#include "emd/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ---------------------------------------------------------------------- */
/* Box writing helpers                                                      */
/* ---------------------------------------------------------------------- */

/* Dynamic buffer for building boxes in memory before writing to file.    */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} buf_t;

static int buf_grow(buf_t *b, size_t need) {
    if (b->len + need <= b->cap) return 0;
    size_t new_cap = b->cap * 2;
    if (new_cap < b->len + need) new_cap = b->len + need + 4096;
    uint8_t *p = realloc(b->data, new_cap);
    if (!p) return -1;
    b->data = p;
    b->cap  = new_cap;
    return 0;
}

static void buf_u8(buf_t *b, uint8_t v) {
    if (buf_grow(b, 1) < 0) return;
    b->data[b->len++] = v;
}

static void buf_u16be(buf_t *b, uint16_t v) {
    buf_u8(b, (uint8_t)(v >> 8));
    buf_u8(b, (uint8_t)(v));
}

static void buf_u32be(buf_t *b, uint32_t v) {
    buf_u8(b, (uint8_t)(v >> 24));
    buf_u8(b, (uint8_t)(v >> 16));
    buf_u8(b, (uint8_t)(v >> 8));
    buf_u8(b, (uint8_t)(v));
}

static void buf_u64be(buf_t *b, uint64_t v) {
    buf_u32be(b, (uint32_t)(v >> 32));
    buf_u32be(b, (uint32_t)(v));
}

static void buf_bytes(buf_t *b, const uint8_t *data, size_t len) {
    if (buf_grow(b, len) < 0) return;
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void buf_fourcc(buf_t *b, const char *cc) {
    buf_u8(b, (uint8_t)cc[0]);
    buf_u8(b, (uint8_t)cc[1]);
    buf_u8(b, (uint8_t)cc[2]);
    buf_u8(b, (uint8_t)cc[3]);
}

/* Start a box: writes placeholder size (0) + fourcc.
 * Returns offset of size field for later patching. */
static size_t box_start(buf_t *b, const char *fourcc) {
    size_t off = b->len;
    buf_u32be(b, 0); /* placeholder size */
    buf_fourcc(b, fourcc);
    return off;
}

/* Patch the size field at offset 'off' to cover bytes from off to b->len. */
static void box_end(buf_t *b, size_t off) {
    uint32_t sz = (uint32_t)(b->len - off);
    b->data[off+0] = (uint8_t)(sz >> 24);
    b->data[off+1] = (uint8_t)(sz >> 16);
    b->data[off+2] = (uint8_t)(sz >> 8);
    b->data[off+3] = (uint8_t)(sz);
}

/* Full-box: version(1) + flags(3) */
static void fullbox(buf_t *b, uint8_t version, uint32_t flags) {
    buf_u8(b, version);
    buf_u8(b, (uint8_t)(flags >> 16));
    buf_u8(b, (uint8_t)(flags >> 8));
    buf_u8(b, (uint8_t)(flags));
}

/* ---------------------------------------------------------------------- */
/* Context                                                                  */
/* ---------------------------------------------------------------------- */

#define MAX_NAL_PER_FRAG  4096

typedef struct {
    uint64_t pts;
    uint64_t dts;
    uint32_t size;
    bool     is_keyframe;
} frag_sample_t;

typedef struct {
    FILE    *fp;
    uint8_t  codec;          /* 1=h264, 2=h265 */
    uint32_t width, height;
    uint32_t timescale;

    /* Cached SPS/PPS for avcC / hvcC */
    uint8_t  sps[512]; size_t sps_len;
    uint8_t  pps[256]; size_t pps_len;
    uint8_t  vps[256]; size_t vps_len;  /* HEVC */

    bool     init_written;

    /* Current fragment accumulation */
    buf_t    frag_data;       /* raw NAL bytes */
    frag_sample_t samples[MAX_NAL_PER_FRAG];
    int      nsamp;
    uint64_t frag_base_pts;
    uint32_t seq_num;
} fmp4_ctx_t;

/* ---------------------------------------------------------------------- */
/* ftyp box                                                                 */
/* ---------------------------------------------------------------------- */

static void write_ftyp(fmp4_ctx_t *ctx) {
    buf_t b = {0};
    b.data = malloc(128); b.cap = 128;

    size_t off = box_start(&b, "ftyp");
    buf_fourcc(&b, "isom");           /* major_brand */
    buf_u32be(&b, 0x00000200u);       /* minor_version */
    /* compatible_brands */
    buf_fourcc(&b, "isom");
    buf_fourcc(&b, "iso2");
    if (ctx->codec == 2) {
        buf_fourcc(&b, "hvc1");
    } else {
        buf_fourcc(&b, "avc1");
    }
    buf_fourcc(&b, "mp41");
    box_end(&b, off);

    fwrite(b.data, 1, b.len, ctx->fp);
    free(b.data);
}

/* ---------------------------------------------------------------------- */
/* moov box                                                                 */
/* ---------------------------------------------------------------------- */

/* avcC (AVCDecoderConfigurationRecord) */
static void write_avcc(buf_t *b, const uint8_t *sps, size_t sps_len,
                        const uint8_t *pps, size_t pps_len)
{
    size_t off = box_start(b, "avcC");
    buf_u8(b, 0x01u);                         /* configurationVersion */
    buf_u8(b, sps_len >= 1 ? sps[1] : 0x64u); /* AVCProfileIndication */
    buf_u8(b, sps_len >= 2 ? sps[2] : 0x00u); /* profile_compatibility */
    buf_u8(b, sps_len >= 3 ? sps[3] : 0x1Fu); /* AVCLevelIndication */
    buf_u8(b, 0xFFu);                          /* lengthSizeMinusOne = 3 (4 bytes) */
    buf_u8(b, 0xE1u);                          /* numSequenceParameterSets = 1 */
    buf_u16be(b, (uint16_t)sps_len);
    buf_bytes(b, sps, sps_len);
    buf_u8(b, 0x01u);                          /* numPictureParameterSets = 1 */
    buf_u16be(b, (uint16_t)pps_len);
    buf_bytes(b, pps, pps_len);
    box_end(b, off);
}

/* hvcC (HEVCDecoderConfigurationRecord) — simplified */
static void write_hvcc(buf_t *b, const uint8_t *vps, size_t vps_len,
                        const uint8_t *sps, size_t sps_len,
                        const uint8_t *pps, size_t pps_len)
{
    /* Build the hvcC config record inline */
    size_t off = box_start(b, "hvcC");
    buf_u8(b, 0x01u);   /* configurationVersion */
    /* general_profile_space|tier_flag|profile_idc */
    buf_u8(b, sps_len >= 2 ? (uint8_t)(sps[1] >> 3) : 0x01u);
    /* general_profile_compatibility_flags (4 bytes) */
    buf_u32be(b, 0x60000000u);
    /* general_constraint_indicator_flags (6 bytes) */
    buf_u32be(b, 0); buf_u16be(b, 0);
    /* general_level_idc */
    buf_u8(b, sps_len >= 12 ? sps[11] : 0x5Au);
    /* min_spatial_segmentation_idc (2 bytes, with reserved 0xF000) */
    buf_u16be(b, 0xF000u);
    /* parallelismType */
    buf_u8(b, 0u);
    /* chromaFormat */
    buf_u8(b, 1u);
    /* bitDepthLumaMinus8 */
    buf_u8(b, 0u);
    /* bitDepthChromaMinus8 */
    buf_u8(b, 0u);
    /* avgFrameRate */
    buf_u16be(b, 0u);
    /* constantFrameRate | numTemporalLayers | temporalIdNested | lengthSizeMinusOne */
    buf_u8(b, 0x03u); /* 4-byte NAL length */
    /* numOfArrays */
    buf_u8(b, 3u);
    /* VPS array */
    buf_u8(b, 0x20u); /* array_completeness=1, NAL_unit_type=VPS(32) */
    buf_u16be(b, 1u);
    buf_u16be(b, (uint16_t)vps_len);
    buf_bytes(b, vps, vps_len);
    /* SPS array */
    buf_u8(b, 0x21u); /* SPS(33) */
    buf_u16be(b, 1u);
    buf_u16be(b, (uint16_t)sps_len);
    buf_bytes(b, sps, sps_len);
    /* PPS array */
    buf_u8(b, 0x22u); /* PPS(34) */
    buf_u16be(b, 1u);
    buf_u16be(b, (uint16_t)pps_len);
    buf_bytes(b, pps, pps_len);

    box_end(b, off);
}

static void write_moov(fmp4_ctx_t *ctx) {
    buf_t b = {0};
    b.data = malloc(4096); b.cap = 4096;

    uint32_t ts = ctx->timescale;

    /* mvhd */
    size_t moov = box_start(&b, "moov");

    {
        size_t off = box_start(&b, "mvhd");
        fullbox(&b, 0, 0);
        buf_u32be(&b, 0); /* creation_time */
        buf_u32be(&b, 0); /* modification_time */
        buf_u32be(&b, ts);
        buf_u32be(&b, 0); /* duration */
        buf_u32be(&b, 0x00010000u); /* rate = 1.0 */
        buf_u16be(&b, 0x0100u);     /* volume = 1.0 */
        buf_u16be(&b, 0);           /* reserved */
        buf_u32be(&b, 0); buf_u32be(&b, 0); /* reserved */
        /* Unity matrix */
        buf_u32be(&b, 0x00010000u); buf_u32be(&b, 0);           buf_u32be(&b, 0);
        buf_u32be(&b, 0);           buf_u32be(&b, 0x00010000u); buf_u32be(&b, 0);
        buf_u32be(&b, 0);           buf_u32be(&b, 0);           buf_u32be(&b, 0x40000000u);
        /* pre-defined */
        for (int i = 0; i < 6; i++) buf_u32be(&b, 0);
        buf_u32be(&b, 2); /* next_track_ID */
        box_end(&b, off);
    }

    /* trak */
    size_t trak = box_start(&b, "trak");

    /* tkhd */
    {
        size_t off = box_start(&b, "tkhd");
        fullbox(&b, 0, 3); /* track_enabled | track_in_movie */
        buf_u32be(&b, 0); /* creation_time */
        buf_u32be(&b, 0); /* modification_time */
        buf_u32be(&b, 1); /* track_ID */
        buf_u32be(&b, 0); /* reserved */
        buf_u32be(&b, 0); /* duration */
        buf_u32be(&b, 0); buf_u32be(&b, 0); /* reserved */
        buf_u16be(&b, 0); /* layer */
        buf_u16be(&b, 0); /* alternate_group */
        buf_u16be(&b, 0); /* volume (video = 0) */
        buf_u16be(&b, 0); /* reserved */
        /* Unity matrix */
        buf_u32be(&b, 0x00010000u); buf_u32be(&b, 0);           buf_u32be(&b, 0);
        buf_u32be(&b, 0);           buf_u32be(&b, 0x00010000u); buf_u32be(&b, 0);
        buf_u32be(&b, 0);           buf_u32be(&b, 0);           buf_u32be(&b, 0x40000000u);
        buf_u32be(&b, ctx->width  << 16); /* width  (16.16 fixed) */
        buf_u32be(&b, ctx->height << 16); /* height */
        box_end(&b, off);
    }

    /* mdia */
    size_t mdia = box_start(&b, "mdia");

    /* mdhd */
    {
        size_t off = box_start(&b, "mdhd");
        fullbox(&b, 0, 0);
        buf_u32be(&b, 0); buf_u32be(&b, 0); /* times */
        buf_u32be(&b, ts);
        buf_u32be(&b, 0); /* duration */
        buf_u16be(&b, 0x55C4u); /* language = 'und' */
        buf_u16be(&b, 0); /* pre_defined */
        box_end(&b, off);
    }

    /* hdlr */
    {
        size_t off = box_start(&b, "hdlr");
        fullbox(&b, 0, 0);
        buf_u32be(&b, 0); /* pre_defined */
        buf_fourcc(&b, "vide");
        buf_u32be(&b, 0); buf_u32be(&b, 0); buf_u32be(&b, 0); /* reserved */
        /* name */
        buf_bytes(&b, (const uint8_t *)"VideoHandler", 13); /* includes NUL */
        box_end(&b, off);
    }

    /* minf */
    size_t minf = box_start(&b, "minf");

    /* vmhd */
    {
        size_t off = box_start(&b, "vmhd");
        fullbox(&b, 0, 1);
        buf_u16be(&b, 0); /* graphicsMode */
        buf_u16be(&b, 0); buf_u16be(&b, 0); buf_u16be(&b, 0); /* opcolor */
        box_end(&b, off);
    }

    /* dinf + dref */
    {
        size_t dinf_off = box_start(&b, "dinf");
        size_t dref_off = box_start(&b, "dref");
        fullbox(&b, 0, 0);
        buf_u32be(&b, 1); /* entry_count */
        {
            size_t url_off = box_start(&b, "url ");
            fullbox(&b, 0, 1); /* self-contained */
            box_end(&b, url_off);
        }
        box_end(&b, dref_off);
        box_end(&b, dinf_off);
    }

    /* stbl */
    size_t stbl = box_start(&b, "stbl");

    /* stsd */
    {
        size_t off = box_start(&b, "stsd");
        fullbox(&b, 0, 0);
        buf_u32be(&b, 1); /* entry_count */

        if (ctx->codec == 2) {
            /* hvc1 */
            size_t hvc1 = box_start(&b, "hvc1");
            /* SampleEntry reserved+data-reference-index */
            for (int i = 0; i < 6; i++) buf_u8(&b, 0); /* reserved */
            buf_u16be(&b, 1); /* data-reference-index */
            /* VisualSampleEntry */
            buf_u16be(&b, 0); buf_u16be(&b, 0); /* pre_defined, reserved */
            buf_u32be(&b, 0); buf_u32be(&b, 0); buf_u32be(&b, 0); /* pre_defined */
            buf_u16be(&b, (uint16_t)ctx->width);
            buf_u16be(&b, (uint16_t)ctx->height);
            buf_u32be(&b, 0x00480000u); /* horizresolution 72dpi */
            buf_u32be(&b, 0x00480000u); /* vertresolution  72dpi */
            buf_u32be(&b, 0); /* reserved */
            buf_u16be(&b, 1); /* frame_count */
            for (int i = 0; i < 32; i++) buf_u8(&b, 0); /* compressorname */
            buf_u16be(&b, 0x0018u); /* depth */
            buf_u16be(&b, (uint16_t)-1); /* pre_defined */
            /* hvcC */
            write_hvcc(&b, ctx->vps, ctx->vps_len,
                           ctx->sps, ctx->sps_len,
                           ctx->pps, ctx->pps_len);
            box_end(&b, hvc1);
        } else {
            /* avc1 */
            size_t avc1 = box_start(&b, "avc1");
            for (int i = 0; i < 6; i++) buf_u8(&b, 0);
            buf_u16be(&b, 1);
            buf_u16be(&b, 0); buf_u16be(&b, 0);
            buf_u32be(&b, 0); buf_u32be(&b, 0); buf_u32be(&b, 0);
            buf_u16be(&b, (uint16_t)ctx->width);
            buf_u16be(&b, (uint16_t)ctx->height);
            buf_u32be(&b, 0x00480000u);
            buf_u32be(&b, 0x00480000u);
            buf_u32be(&b, 0);
            buf_u16be(&b, 1);
            for (int i = 0; i < 32; i++) buf_u8(&b, 0);
            buf_u16be(&b, 0x0018u);
            buf_u16be(&b, (uint16_t)-1);
            /* avcC */
            write_avcc(&b, ctx->sps, ctx->sps_len,
                           ctx->pps, ctx->pps_len);
            box_end(&b, avc1);
        }
        box_end(&b, off);
    }

    /* stts — empty (no static samples in fragmented MP4) */
    {
        size_t off = box_start(&b, "stts");
        fullbox(&b, 0, 0);
        buf_u32be(&b, 0); /* entry_count */
        box_end(&b, off);
    }

    /* stsc — empty */
    {
        size_t off = box_start(&b, "stsc");
        fullbox(&b, 0, 0);
        buf_u32be(&b, 0);
        box_end(&b, off);
    }

    /* stsz — empty */
    {
        size_t off = box_start(&b, "stsz");
        fullbox(&b, 0, 0);
        buf_u32be(&b, 0); /* sample_size = 0 (variable) */
        buf_u32be(&b, 0); /* sample_count */
        box_end(&b, off);
    }

    /* stco — empty */
    {
        size_t off = box_start(&b, "stco");
        fullbox(&b, 0, 0);
        buf_u32be(&b, 0);
        box_end(&b, off);
    }

    box_end(&b, stbl);
    box_end(&b, minf);
    box_end(&b, mdia);
    box_end(&b, trak);
    box_end(&b, moov);

    fwrite(b.data, 1, b.len, ctx->fp);
    free(b.data);
}

/* ---------------------------------------------------------------------- */
/* Fragment: moof + mdat                                                    */
/* ---------------------------------------------------------------------- */

static void flush_fragment(fmp4_ctx_t *ctx) {
    if (ctx->nsamp == 0) return;

    buf_t b = {0};
    b.data = malloc(4096 + (size_t)ctx->nsamp * 16); b.cap = 4096 + (size_t)ctx->nsamp * 16;

    /* moof */
    size_t moof = box_start(&b, "moof");

    /* mfhd */
    {
        size_t off = box_start(&b, "mfhd");
        fullbox(&b, 0, 0);
        buf_u32be(&b, ++ctx->seq_num);
        box_end(&b, off);
    }

    /* traf */
    size_t traf = box_start(&b, "traf");

    /* tfhd */
    {
        size_t off = box_start(&b, "tfhd");
        /* flags: base-data-offset-present=0x01, default-base-is-moof=0x020000 */
        fullbox(&b, 0, 0x020000u);
        buf_u32be(&b, 1); /* track_ID */
        box_end(&b, off);
    }

    /* tfdt */
    {
        size_t off = box_start(&b, "tfdt");
        fullbox(&b, 1, 0); /* version=1 for 64-bit */
        buf_u64be(&b, ctx->frag_base_pts);
        box_end(&b, off);
    }

    /* trun */
    /* flags: data-offset-present=0x0001, first-sample-flags-present=0x0004,
     *         sample-duration-present=0x0100, sample-size-present=0x0200,
     *         sample-flags-present=0x0400 */
    uint32_t trun_flags = 0x0001u | 0x0004u | 0x0100u | 0x0200u;
    {
        size_t off = box_start(&b, "trun");
        fullbox(&b, 0, trun_flags);
        buf_u32be(&b, (uint32_t)ctx->nsamp);
        /* data_offset placeholder — we'll patch after moof is built */
        size_t data_offset_pos = b.len;
        buf_u32be(&b, 0); /* placeholder */
        /* first_sample_flags: key frame = 0x02000000 */
        buf_u32be(&b, ctx->samples[0].is_keyframe ? 0x02000000u : 0x00010000u);

        /* Per-sample: duration, size */
        for (int i = 0; i < ctx->nsamp; i++) {
            uint32_t dur;
            if (i + 1 < ctx->nsamp) {
                int64_t d = (int64_t)(ctx->samples[i+1].pts - ctx->samples[i].pts);
                dur = d > 0 ? (uint32_t)d : 3003u; /* ~29.97 fps default */
            } else {
                dur = 3003u;
            }
            buf_u32be(&b, dur);
            buf_u32be(&b, ctx->samples[i].size);
        }

        /* Patch data_offset: moof size + 8 (for mdat size+fourcc) */
        box_end(&b, traf);
        box_end(&b, moof);
        uint32_t moof_size = (uint32_t)(b.len);
        uint32_t data_off  = moof_size + 8u;
        b.data[data_offset_pos+0] = (uint8_t)(data_off >> 24);
        b.data[data_offset_pos+1] = (uint8_t)(data_off >> 16);
        b.data[data_offset_pos+2] = (uint8_t)(data_off >> 8);
        b.data[data_offset_pos+3] = (uint8_t)(data_off);

        box_end(&b, off);
    }

    /* mdat */
    {
        size_t off = box_start(&b, "mdat");
        buf_bytes(&b, ctx->frag_data.data, ctx->frag_data.len);
        box_end(&b, off);
    }

    fwrite(b.data, 1, b.len, ctx->fp);
    free(b.data);

    /* Reset fragment */
    ctx->frag_data.len = 0;
    ctx->nsamp = 0;
}

/* ---------------------------------------------------------------------- */
/* Muxer backend API                                                        */
/* ---------------------------------------------------------------------- */

static void *fmp4_open(const char *path, uint8_t codec,
                        uint32_t width, uint32_t height, uint32_t timescale)
{
    fmp4_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->fp = fopen(path, "wb");
    if (!ctx->fp) { free(ctx); return NULL; }

    ctx->codec     = codec;
    ctx->width     = width ? width : 1920;
    ctx->height    = height ? height : 1080;
    ctx->timescale = timescale ? timescale : 90000;

    ctx->frag_data.data = malloc(1024 * 1024);
    ctx->frag_data.cap  = 1024 * 1024;

    /* ftyp written immediately; moov written after first param set */
    write_ftyp(ctx);

    return ctx;
}

/* NAL type helpers */
static bool nal_is_sps_h264(const uint8_t *nal, size_t len) {
    return len > 0 && (nal[0] & 0x1Fu) == 7u;
}
static bool nal_is_pps_h264(const uint8_t *nal, size_t len) {
    return len > 0 && (nal[0] & 0x1Fu) == 8u;
}
static bool nal_is_vps_h265(const uint8_t *nal, size_t len) {
    return len >= 2 && ((nal[0] >> 1) & 0x3Fu) == 32u;
}
static bool nal_is_sps_h265(const uint8_t *nal, size_t len) {
    return len >= 2 && ((nal[0] >> 1) & 0x3Fu) == 33u;
}
static bool nal_is_pps_h265(const uint8_t *nal, size_t len) {
    return len >= 2 && ((nal[0] >> 1) & 0x3Fu) == 34u;
}

static int fmp4_write_nal(void *handle, const uint8_t *nal, size_t len,
                            uint64_t pts, uint64_t dts, bool is_keyframe)
{
    fmp4_ctx_t *ctx = (fmp4_ctx_t *)handle;
    if (!ctx || !nal || len == 0) return -1;

    /* Cache parameter sets */
    if (ctx->codec == 1) {
        if (nal_is_sps_h264(nal, len)) {
            size_t copy = len < sizeof(ctx->sps) ? len : sizeof(ctx->sps) - 1;
            memcpy(ctx->sps, nal, copy); ctx->sps_len = copy;
        } else if (nal_is_pps_h264(nal, len)) {
            size_t copy = len < sizeof(ctx->pps) ? len : sizeof(ctx->pps) - 1;
            memcpy(ctx->pps, nal, copy); ctx->pps_len = copy;
        }
    } else {
        if (nal_is_vps_h265(nal, len)) {
            size_t copy = len < sizeof(ctx->vps) ? len : sizeof(ctx->vps) - 1;
            memcpy(ctx->vps, nal, copy); ctx->vps_len = copy;
        } else if (nal_is_sps_h265(nal, len)) {
            size_t copy = len < sizeof(ctx->sps) ? len : sizeof(ctx->sps) - 1;
            memcpy(ctx->sps, nal, copy); ctx->sps_len = copy;
        } else if (nal_is_pps_h265(nal, len)) {
            size_t copy = len < sizeof(ctx->pps) ? len : sizeof(ctx->pps) - 1;
            memcpy(ctx->pps, nal, copy); ctx->pps_len = copy;
        }
    }

    /* Write moov once we have parameter sets */
    if (!ctx->init_written && ctx->sps_len > 0 && ctx->pps_len > 0) {
        if (ctx->codec != 2 || ctx->vps_len > 0) {
            write_moov(ctx);
            ctx->init_written = true;
        }
    }

    /* Flush fragment on keyframe boundary (except first) */
    if (is_keyframe && ctx->nsamp > 0) {
        flush_fragment(ctx);
    }

    /* Start new fragment */
    if (ctx->nsamp == 0) {
        ctx->frag_base_pts = pts;
    }

    /* Append NAL to fragment data as length-prefixed (4-byte big-endian) */
    uint8_t len_hdr[4];
    len_hdr[0] = (uint8_t)(len >> 24);
    len_hdr[1] = (uint8_t)(len >> 16);
    len_hdr[2] = (uint8_t)(len >> 8);
    len_hdr[3] = (uint8_t)(len);

    if (buf_grow(&ctx->frag_data, 4 + len) < 0) return -1;
    memcpy(ctx->frag_data.data + ctx->frag_data.len, len_hdr, 4);
    ctx->frag_data.len += 4;
    memcpy(ctx->frag_data.data + ctx->frag_data.len, nal, len);
    ctx->frag_data.len += len;

    if (ctx->nsamp < MAX_NAL_PER_FRAG) {
        ctx->samples[ctx->nsamp].pts          = pts;
        ctx->samples[ctx->nsamp].dts          = dts;
        ctx->samples[ctx->nsamp].size         = (uint32_t)(4 + len);
        ctx->samples[ctx->nsamp].is_keyframe  = is_keyframe;
        ctx->nsamp++;
    }

    return 0;
}

static int fmp4_close(void *handle) {
    fmp4_ctx_t *ctx = (fmp4_ctx_t *)handle;
    if (!ctx) return -1;

    /* Flush remaining fragment */
    if (ctx->nsamp > 0) flush_fragment(ctx);

    int r = fclose(ctx->fp);
    free(ctx->frag_data.data);
    free(ctx);
    return r == 0 ? 0 : -1;
}

const emd_mux_backend_t emd_mux_fmp4 = {
    .open      = fmp4_open,
    .write_nal = fmp4_write_nal,
    .close     = fmp4_close,
};
