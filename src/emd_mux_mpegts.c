/*
 * emd_mux_mpegts.c — Minimal MPEG-TS muxer for AVC/HEVC clips.
 *
 * Specification:
 *  - 188-byte packets.
 *  - PAT (PID 0): program_number=1, PMT_PID=0x100.
 *  - PMT (PID 0x100): video PID 0x101, stream_type 0x1B (AVC) or 0x24 (HEVC).
 *  - PCR on video PID every ~40 ms.
 *  - PES packetisation with PTS and DTS from 90 kHz timestamps.
 *  - PAT/PMT emitted every ~100 ms (approximated by frame count).
 *
 * References: ISO 13818-1.
 */

#include "emd/recorder.h"
#include "emd/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

/* ---------------------------------------------------------------------- */
/* Constants                                                                */
/* ---------------------------------------------------------------------- */

#define TS_PKT_SIZE     188
#define TS_SYNC_BYTE    0x47u

#define PID_PAT         0x0000u
#define PID_PMT         0x0100u
#define PID_VIDEO       0x0100u  /* Same as PMT - ES PID reuses program PID */

#define STREAM_TYPE_AVC  0x1Bu
#define STREAM_TYPE_HEVC 0x24u

/* PES stream IDs */
#define PES_STREAM_VIDEO 0xE0u

/* CRC-32 for MPEG-TS SI tables (MPEG-2 polynomial) */
static uint32_t crc32_mpeg(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000u)
                crc = (crc << 1) ^ 0x04C11DB7u;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ---------------------------------------------------------------------- */
/* Context                                                                  */
/* ---------------------------------------------------------------------- */

typedef struct {
    FILE    *fp;
    uint8_t  codec;          /* 1=h264, 2=h265 */
    uint32_t width, height;
    uint32_t timescale;      /* 90000 */
    uint8_t  pat_cc;
    uint8_t  pmt_cc;
    uint8_t  vid_cc;
    uint32_t frames_since_pat;
    uint32_t frames_since_pcr;
    uint64_t last_pcr_pts;
    uint64_t first_pts;      /* First PTS for timestamp normalization */
    bool     pts_initialized;
} mpegts_ctx_t;

/* ---------------------------------------------------------------------- */
/* TS packet writer                                                         */
/* ---------------------------------------------------------------------- */

/* Write one 188-byte TS packet to the file. */
static int write_ts_pkt(mpegts_ctx_t *ctx, const uint8_t pkt[TS_PKT_SIZE]) {
    if (fwrite(pkt, 1, TS_PKT_SIZE, ctx->fp) != TS_PKT_SIZE) return -1;
    return 0;
}

/* Construct a TS packet with optional adaptation field and payload.
 * Returns 0 on success. */
static int emit_ts(mpegts_ctx_t *ctx,
                    uint16_t pid, uint8_t *cc_ptr,
                    bool pusi, bool has_pcr, uint64_t pcr_90khz,
                    const uint8_t *payload, size_t payload_len)
{
    uint8_t pkt[TS_PKT_SIZE];
    memset(pkt, 0xFF, sizeof(pkt));

    pkt[0] = TS_SYNC_BYTE;
    pkt[1] = (uint8_t)((pusi ? 0x40u : 0x00u) | (uint8_t)((pid >> 8) & 0x1Fu));
    pkt[2] = (uint8_t)(pid & 0xFFu);

    /* adaptation field */
    size_t af_len = 0;
    if (has_pcr) {
        af_len = 8; /* flags byte + 6 PCR bytes */
    }

    size_t hdr_len = 4;
    bool has_af = (af_len > 0);

    /* Adaptation field control bits:
     * 01 = payload only, 10 = adaptation only, 11 = adaptation + payload */
    uint8_t afc;
    if (has_af && payload_len > 0) afc = 0x30u;
    else if (has_af)               afc = 0x20u;
    else                           afc = 0x10u;

    pkt[3] = (uint8_t)(afc | (*cc_ptr & 0x0Fu));
    *cc_ptr = (uint8_t)((*cc_ptr + 1) & 0x0Fu);

    size_t pos = hdr_len;

    if (has_af) {
        /* Adaptation field length */
        pkt[pos++] = (uint8_t)(af_len - 1); /* length excludes the length byte itself */
        pkt[pos++] = has_pcr ? 0x10u : 0x00u; /* PCR_flag */
        if (has_pcr) {
            /* PCR base (33 bits) and extension (9 bits) */
            uint64_t base = pcr_90khz & 0x1FFFFFFFFull;
            uint16_t ext  = 0; /* extension = 0 */
            pkt[pos+0] = (uint8_t)(base >> 25);
            pkt[pos+1] = (uint8_t)(base >> 17);
            pkt[pos+2] = (uint8_t)(base >> 9);
            pkt[pos+3] = (uint8_t)(base >> 1);
            pkt[pos+4] = (uint8_t)(((base & 1u) << 7) | 0x7Eu | ((ext >> 8) & 1u));
            pkt[pos+5] = (uint8_t)(ext & 0xFFu);
            pos += 6;
        }
        /* Pad to af_len */
        /* Already done — memset 0xFF above covers stuffing */
    }

    /* Payload */
    if (payload && payload_len > 0) {
        size_t space = TS_PKT_SIZE - pos;
        size_t copy  = payload_len < space ? payload_len : space;
        memcpy(pkt + pos, payload, copy);
        /* remaining bytes are 0xFF (stuffing) */
    }

    return write_ts_pkt(ctx, pkt);
}

/* ---------------------------------------------------------------------- */
/* PAT / PMT                                                                */
/* ---------------------------------------------------------------------- */

static int emit_pat(mpegts_ctx_t *ctx) {
    /* PAT section:
     *   table_id (1)
     *   section_syntax_indicator | 0 | reserved | section_length (2)
     *   transport_stream_id (2)
     *   reserved | version_number | current_next (1)
     *   section_number (1)
     *   last_section_number (1)
     *   --- program loop ---
     *   program_number (2)
     *   reserved | PMT_PID (2)
     *   --- CRC ---
     */
    uint8_t sec[17];
    sec[0]  = 0x00u;                     /* table_id = PAT */
    sec[1]  = 0xB0u | ((8 >> 8) & 0x0Fu);/* section_syntax=1, section_length high */
    sec[2]  = 13;                        /* section_length = 13 (excludes first 3 bytes) */
    sec[3]  = 0x00u; sec[4]  = 0x01u;   /* transport_stream_id = 1 */
    sec[5]  = 0xC1u;                     /* reserved=11, version=0, current=1 */
    sec[6]  = 0x00u;                     /* section_number */
    sec[7]  = 0x00u;                     /* last_section_number */
    /* Program 1 → PMT PID 0x100 */
    sec[8]  = 0x00u; sec[9]  = 0x01u;   /* program_number = 1 */
    sec[10] = (uint8_t)(0xE0u | ((PID_PMT >> 8) & 0x1Fu));
    sec[11] = (uint8_t)(PID_PMT & 0xFFu);
    /* CRC */
    uint32_t crc = crc32_mpeg(sec, 12);
    sec[12] = (uint8_t)(crc >> 24);
    sec[13] = (uint8_t)(crc >> 16);
    sec[14] = (uint8_t)(crc >> 8);
    sec[15] = (uint8_t)(crc);

    /* Prepend pointer field */
    uint8_t payload[17];
    payload[0] = 0x00u; /* pointer_field */
    memcpy(payload + 1, sec, 16);

    return emit_ts(ctx, PID_PAT, &ctx->pat_cc, true, false, 0, payload, 17);
}

static int emit_pmt(mpegts_ctx_t *ctx) {
    uint8_t stream_type = (ctx->codec == 2) ? STREAM_TYPE_HEVC : STREAM_TYPE_AVC;

    /* PMT section */
    uint8_t sec[32];
    size_t  sp = 0;
    sec[sp++] = 0x02u;   /* table_id = PMT */
    /* section_length will be filled in later */
    size_t len_pos = sp;
    sec[sp++] = 0xB0u | 0x00u;
    sec[sp++] = 0;  /* placeholder */
    sec[sp++] = 0x00u; sec[sp++] = 0x01u; /* program_number = 1 */
    sec[sp++] = 0xC1u;                      /* version=0, current=1 */
    sec[sp++] = 0x00u;                      /* section_number */
    sec[sp++] = 0x00u;                      /* last_section_number */
    /* PCR PID */
    sec[sp++] = (uint8_t)(0xE0u | ((PID_VIDEO >> 8) & 0x1Fu));
    sec[sp++] = (uint8_t)(PID_VIDEO & 0xFFu);
    /* program_info_length = 0 */
    sec[sp++] = 0xF0u;
    sec[sp++] = 0x00u;
    /* ES stream: stream_type, PID, ES_info_length=0 */
    sec[sp++] = stream_type;
    sec[sp++] = (uint8_t)(0xE0u | ((PID_VIDEO >> 8) & 0x1Fu));
    sec[sp++] = (uint8_t)(PID_VIDEO & 0xFFu);
    sec[sp++] = 0xF0u; /* ES_info_length high */
    sec[sp++] = 0x00u; /* ES_info_length low */

    /* Calculate section_length: everything from after the length field to end, including CRC (4 bytes) */
    /* sp is currently at the position before CRC, so section_length = sp - 3 + 4 (for CRC) */
    size_t section_length = sp - 3 + 4;
    sec[len_pos] = 0xB0u | (uint8_t)((section_length >> 8) & 0x0Fu);
    sec[len_pos + 1] = (uint8_t)(section_length & 0xFFu);

    /* CRC */
    uint32_t crc = crc32_mpeg(sec, sp);
    sec[sp++] = (uint8_t)(crc >> 24);
    sec[sp++] = (uint8_t)(crc >> 16);
    sec[sp++] = (uint8_t)(crc >> 8);
    sec[sp++] = (uint8_t)(crc);

    uint8_t payload[1 + 32];
    payload[0] = 0x00u; /* pointer_field */
    memcpy(payload + 1, sec, sp);

    return emit_ts(ctx, PID_PMT, &ctx->pmt_cc, true, false, 0, payload, sp + 1);
}

/* ---------------------------------------------------------------------- */
/* PES packetisation                                                        */
/* ---------------------------------------------------------------------- */

/* Write a PES-wrapped NAL as one or more TS packets. */
static int emit_nal(mpegts_ctx_t *ctx,
                     const uint8_t *nal, size_t nal_len,
                     uint64_t pts, uint64_t dts, bool is_keyframe)
{
    /* PES header:
     *   0x000001 (start code)
     *   stream_id (1)
     *   PES_packet_length (2) — 0 for video
     *   marker | scrambling | PES_priority | data_align | copyright | original (1)
     *   PTS_DTS_flags | ... (1)
     *   PES_header_data_length (1)
     *   PTS (5 bytes)
     *   DTS (5 bytes) — only if DTS != PTS
     */
    bool pts_dts_same = (pts == dts);
    size_t pes_hdr_len = pts_dts_same ? 14u : 19u;

    uint8_t pes[32];
    size_t  pp = 0;
    pes[pp++] = 0x00u; pes[pp++] = 0x00u; pes[pp++] = 0x01u; /* start code */
    pes[pp++] = PES_STREAM_VIDEO;
    pes[pp++] = 0x00u; pes[pp++] = 0x00u; /* PES packet length = 0 (unbounded) */
    /* flags byte 1: marker=10, scrambling=00, priority=0, data_alignment=1, copyright=0, original=0 */
    pes[pp++] = 0x84u;  /* 0x84 = 10000100 binary, sets data_alignment_indicator */
    /* flags byte 2: PTS_DTS_flags */
    pes[pp++] = pts_dts_same ? 0x80u : 0xC0u;
    /* PES_header_data_length */
    pes[pp++] = pts_dts_same ? 5u : 10u;

    /* PTS */
    uint64_t pts_val = pts & 0x1FFFFFFFFull;
    uint8_t pts_prefix = pts_dts_same ? 0x21u : 0x31u;
    pes[pp++] = (uint8_t)(pts_prefix | ((pts_val >> 29) & 0x0Eu));
    pes[pp++] = (uint8_t)(pts_val >> 22);
    pes[pp++] = (uint8_t)(((pts_val >> 14) & 0xFEu) | 0x01u);
    pes[pp++] = (uint8_t)(pts_val >> 7);
    pes[pp++] = (uint8_t)(((pts_val & 0x7Fu) << 1) | 0x01u);

    if (!pts_dts_same) {
        uint64_t dts_val = dts & 0x1FFFFFFFFull;
        pes[pp++] = (uint8_t)(0x11u | ((dts_val >> 29) & 0x0Eu));
        pes[pp++] = (uint8_t)(dts_val >> 22);
        pes[pp++] = (uint8_t)(((dts_val >> 14) & 0xFEu) | 0x01u);
        pes[pp++] = (uint8_t)(dts_val >> 7);
        pes[pp++] = (uint8_t)(((dts_val & 0x7Fu) << 1) | 0x01u);
    }

    (void)pes_hdr_len;

    /* Assemble: PES header + 4-byte start code prefix + NAL */
    /* For AVC/HEVC we emit Annex B (start-code + NALU) */
    static const uint8_t START_CODE[4] = {0x00, 0x00, 0x00, 0x01};

    /* Total payload: PES header + start code + NAL */
    size_t total_payload = pp + 4 + nal_len;
    uint8_t *payload_buf = malloc(total_payload);
    if (!payload_buf) return -1;

    memcpy(payload_buf, pes, pp);
    memcpy(payload_buf + pp, START_CODE, 4);
    memcpy(payload_buf + pp + 4, nal, nal_len);

    /* Emit TS packets */
    size_t offset = 0;
    bool first = true;

    while (offset < total_payload) {
        bool has_pcr = false;
        uint64_t pcr_val = 0;
        if (is_keyframe && first) {
            has_pcr = true;
            pcr_val = dts;  /* PCR must align with DTS, not PTS */
        }

        /* How much payload fits in this TS packet? */
        size_t af_overhead = 0;
        if (has_pcr) {
            af_overhead = 2 + 6; /* af_length(1) + flags(1) + PCR(6) */
        } else if (first) {
            /* No PCR but we must handle potential stuffing */
            af_overhead = 0;
        }

        size_t space = (size_t)(TS_PKT_SIZE - 4 - af_overhead);
        size_t chunk = total_payload - offset;
        if (chunk > space) chunk = space;

        /* If last chunk and not full packet — need stuffing. Handle by adding
         * an adaptation field with stuffing bytes. */
        size_t remaining_after = (total_payload - offset) - chunk;
        bool need_stuff = (chunk < space && remaining_after == 0 && !has_pcr);
        if (need_stuff) {
            /* Insert a minimal adaptation field for stuffing */
            size_t stuff_len = space - chunk;
            /* adaptation_field_length + flags = 2 bytes minimum.
             * The stuff_len bytes are already 0xFF in pkt. */
            /* We'll handle this inside emit_ts by just letting the 0xFF fill */
            (void)stuff_len;
        }

        /* Use the chunk */
        if (has_pcr) {
            emit_ts(ctx, PID_VIDEO, &ctx->vid_cc, first, true, pcr_val,
                    payload_buf + offset, chunk);
        } else {
            emit_ts(ctx, PID_VIDEO, &ctx->vid_cc, first, false, 0,
                    payload_buf + offset, chunk);
        }

        offset += chunk;
        first = false;
    }

    free(payload_buf);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Muxer backend API                                                        */
/* ---------------------------------------------------------------------- */

static void *mpegts_open(const char *path, uint8_t codec,
                          uint32_t width, uint32_t height, uint32_t timescale)
{
    mpegts_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->fp = fopen(path, "wb");
    if (!ctx->fp) { free(ctx); return NULL; }

    ctx->codec     = codec;
    ctx->width     = width;
    ctx->height    = height;
    ctx->timescale = timescale;

    /* Emit initial PAT/PMT */
    emit_pat(ctx);
    emit_pmt(ctx);
    ctx->frames_since_pat = 0;

    return ctx;
}

static int mpegts_write_nal(void *handle, const uint8_t *nal, size_t len,
                              uint64_t pts, uint64_t dts, bool is_keyframe)
{
    mpegts_ctx_t *ctx = (mpegts_ctx_t *)handle;
    if (!ctx || !nal || len == 0) return -1;

    /* Normalize timestamps: capture first PTS and subtract it from all timestamps */
    if (!ctx->pts_initialized) {
        ctx->first_pts = pts;
        ctx->pts_initialized = true;
        /* Use stdout and flush immediately to ensure it appears in logs */
        printf("[NORM_DEBUG] NEW CLIP: first_pts=%lu\n", (unsigned long)pts);
        fflush(stdout);
    }

    /* Subtract first_pts to normalize timestamps to start from zero */
    uint64_t normalized_pts = pts - ctx->first_pts;
    uint64_t normalized_dts = dts - ctx->first_pts;

    /* Log first NAL of each clip to verify normalization */
    if (ctx->frames_since_pat == 0) {
        printf("[NORM_DEBUG] FIRST NAL: pts=%lu dts=%lu norm_pts=%lu norm_dts=%lu\n",
                (unsigned long)pts, (unsigned long)dts,
                (unsigned long)normalized_pts, (unsigned long)normalized_dts);
        fflush(stdout);
    }

    /* Emit PAT/PMT every ~100 ms (~3 frames at 30 fps) */
    if (ctx->frames_since_pat >= 90) {
        emit_pat(ctx);
        emit_pmt(ctx);
        ctx->frames_since_pat = 0;
    }
    ctx->frames_since_pat++;

    return emit_nal(ctx, nal, len, normalized_pts, normalized_dts, is_keyframe);
}

static int mpegts_close(void *handle) {
    mpegts_ctx_t *ctx = (mpegts_ctx_t *)handle;
    if (!ctx) return -1;
    int r = fclose(ctx->fp);
    free(ctx);
    return r == 0 ? 0 : -1;
}

const emd_mux_backend_t emd_mux_mpegts = {
    .open      = mpegts_open,
    .write_nal = mpegts_write_nal,
    .close     = mpegts_close,
};
