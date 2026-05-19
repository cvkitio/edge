/*
 * emd_recorder.c — Clip writer pool.
 *
 * Workers pull events from the event bus, take snapshots from the
 * camera ring buffers, and write clips to disk via the mux backend.
 * Atomic rename: .part → final path.
 */

#include "emd/recorder.h"
#include "emd/event.h"
#include "emd/ringbuf.h"
#include "emd/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>

/* ---------------------------------------------------------------------- */
/* SHA-256 (minimal, for sidecar hash)                                     */
/* ---------------------------------------------------------------------- */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t bpos; } sha256_t;

static const uint32_t K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
    0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
    0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
    0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
    0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
    0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
};

#define ROR32(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_t *s, const uint8_t *blk) {
    uint32_t w[64]; uint32_t a,b,c,d,e,f,g,h,t1,t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)|
               ((uint32_t)blk[i*4+2]<<8)|(uint32_t)blk[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i-15],7)^ROR32(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = ROR32(w[i-2],17)^ROR32(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3];
    e=s->h[4]; f=s->h[5]; g=s->h[6]; h=s->h[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + (ROR32(e,6)^ROR32(e,11)^ROR32(e,25)) +
             ((e&f)^(~e&g)) + K256[i] + w[i];
        t2 = (ROR32(a,2)^ROR32(a,13)^ROR32(a,22)) + ((a&b)^(a&c)^(b&c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void sha256_init(sha256_t *s) {
    s->h[0]=0x6a09e667u; s->h[1]=0xbb67ae85u; s->h[2]=0x3c6ef372u; s->h[3]=0xa54ff53au;
    s->h[4]=0x510e527fu; s->h[5]=0x9b05688cu; s->h[6]=0x1f83d9abu; s->h[7]=0x5be0cd19u;
    s->len=0; s->bpos=0;
}

static void sha256_update(sha256_t *s, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        s->buf[s->bpos++] = data[i];
        if (s->bpos == 64) { sha256_block(s, s->buf); s->bpos = 0; }
        s->len++;
    }
}

static void sha256_final(sha256_t *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    s->buf[s->bpos++] = 0x80u;
    if (s->bpos > 56) { while (s->bpos < 64) s->buf[s->bpos++] = 0; sha256_block(s, s->buf); s->bpos = 0; }
    while (s->bpos < 56) s->buf[s->bpos++] = 0;
    for (int i = 7; i >= 0; i--) { s->buf[s->bpos++] = (uint8_t)(bits >> (i*8)); }
    sha256_block(s, s->buf);
    for (int i = 0; i < 8; i++) {
        out[i*4+0]=(uint8_t)(s->h[i]>>24); out[i*4+1]=(uint8_t)(s->h[i]>>16);
        out[i*4+2]=(uint8_t)(s->h[i]>>8);  out[i*4+3]=(uint8_t)(s->h[i]);
    }
}

static void sha256_file(const char *path, char hex[65]) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { hex[0] = '\0'; return; }
    sha256_t s; sha256_init(&s);
    uint8_t buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        sha256_update(&s, buf, n);
    fclose(fp);
    uint8_t dig[32]; sha256_final(&s, dig);
    for (int i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", dig[i]);
    hex[64] = '\0';
}

/* ---------------------------------------------------------------------- */
/* Path construction                                                        */
/* ---------------------------------------------------------------------- */

static void make_dirs(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void pts_to_wall(uint64_t pts_90khz, char out[32]) {
    /* Best effort: use current time */
    time_t t = time(NULL);
    struct tm tm_info;
    gmtime_r(&t, &tm_info);
    strftime(out, 32, "%Y%m%dT%H%M%SZ", &tm_info);
    (void)pts_90khz;
}

/* ---------------------------------------------------------------------- */
/* Clip writing                                                             */
/* ---------------------------------------------------------------------- */

int emd_recorder_write_clip(const emd_recorder_cfg_t *cfg,
                             const emd_event_t *ev,
                             emd_ringbuf_snap_t *snap,
                             const emd_mux_backend_t *mux,
                             emd_clip_header_t *hdr_out)
{
    if (!cfg || !ev || !snap || !mux || !hdr_out) return -1;

    /* Determine container extension */
    const char *ext = (cfg->container == EMD_CONTAINER_FMP4) ? "mp4" : "ts";
    const char *container_name = (cfg->container == EMD_CONTAINER_FMP4) ? "fmp4" : "mpegts";

    /* Inflight path: {inflight_root}/{cam_name}/{event_id}.{nonce}.{ext}.part */
    char ts_str[32];
    pts_to_wall(ev->started_pts_90khz, ts_str);

    char inflight_dir[512];
    snprintf(inflight_dir, sizeof(inflight_dir), "%s/%s",
             cfg->inflight_root, ev->cam_name[0] ? ev->cam_name : "cam0");
    make_dirs(inflight_dir);

    char part_path[1024];
    snprintf(part_path, sizeof(part_path), "%s/%s.%s.part",
             inflight_dir, ev->event_id, ext);

    /* Open muxer */
    void *mx = mux->open(part_path, ev->codec, 1920, 1080, 90000);
    if (!mx) {
        EMD_LOGE("recorder", "mux open failed");
        return -1;
    }

    /* Write NAL units from snapshot */
    uint64_t first_pts = 0, last_pts = 0;
    bool first = true;

    for (uint32_t i = 0; i < snap->count; i++) {
        const emd_nal_record_t *rec = &snap->records[i];
        const uint8_t *nal_data = emd_ringbuf_snap_data(snap, i);
        if (!nal_data) continue;

        bool is_kf = !!(rec->flags & EMD_NAL_KEYFRAME);

        if (first) {
            first_pts = rec->pts_90khz;
            first = false;
        }
        last_pts = rec->pts_90khz;

        if (mux->write_nal(mx, nal_data, rec->length,
                            rec->pts_90khz, rec->pts_90khz, is_kf) < 0) {
            EMD_LOGE("recorder", "write_nal failed");
            mux->close(mx);
            remove(part_path);
            return -1;
        }
    }

    if (mux->close(mx) < 0) {
        EMD_LOGE("recorder", "mux close failed");
        remove(part_path);
        return -1;
    }

    /* fsync */
    if (cfg->fsync_policy == EMD_FSYNC_ON_CLOSE || cfg->fsync_policy == EMD_FSYNC_ALWAYS) {
        int fd2 = open(part_path, O_RDONLY);
        if (fd2 >= 0) { fsync(fd2); close(fd2); }
    }

    /* Final path: {clip_root}/{cam_name}/{YYYY}/{MM}/{DD}/{ts}-{reason}.{ext} */
    time_t t = time(NULL);
    struct tm tm_info;
    gmtime_r(&t, &tm_info);

    char final_dir[512];
    snprintf(final_dir, sizeof(final_dir), "%s/%s/%04d/%02d/%02d",
             cfg->clip_root, ev->cam_name[0] ? ev->cam_name : "cam0",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday);
    make_dirs(final_dir);

    const char *type_str = (ev->type == EMD_EVENT_SCENE_CHANGE) ? "scene" : "motion";
    char final_path[1024];
    snprintf(final_path, sizeof(final_path), "%s/%s-%s.%s",
             final_dir, ts_str, type_str, ext);

    if (rename(part_path, final_path) < 0) {
        EMD_LOGE("recorder", "rename failed");
        remove(part_path);
        return -1;
    }

    /* Compute size and sha256 */
    struct stat st;
    uint64_t file_size = 0;
    if (stat(final_path, &st) == 0) file_size = (uint64_t)st.st_size;

    char sha_hex[65];
    sha256_file(final_path, sha_hex);

    /* Fill header */
    memset(hdr_out, 0, sizeof(*hdr_out));
    strncpy(hdr_out->event_id, ev->event_id, sizeof(hdr_out->event_id) - 1);
    strncpy(hdr_out->cam_id_str, ev->cam_name, sizeof(hdr_out->cam_id_str) - 1);
    strncpy(hdr_out->container, container_name, sizeof(hdr_out->container) - 1);
    strncpy(hdr_out->codec, emd_codec_name(ev->codec), sizeof(hdr_out->codec) - 1);
    strncpy(hdr_out->path, final_path, sizeof(hdr_out->path) - 1);
    strncpy(hdr_out->sha256, sha_hex, sizeof(hdr_out->sha256) - 1);
    hdr_out->size_bytes  = file_size;
    uint64_t dur_pts = (last_pts > first_pts) ? (last_pts - first_pts) : 0;
    hdr_out->duration_ms = dur_pts / 90; /* 90 kHz → ms */
    hdr_out->pre_roll_ms  = ev->pre_roll_pts  / 90;
    hdr_out->post_roll_ms = ev->post_roll_pts  / 90;

    return 0;
}

int emd_recorder_write_sidecar(const emd_clip_header_t *hdr) {
    if (!hdr || hdr->path[0] == '\0') return -1;

    char json_path[1024];
    snprintf(json_path, sizeof(json_path), "%s.json", hdr->path);

    FILE *fp = fopen(json_path, "w");
    if (!fp) return -1;

    fprintf(fp,
            "{"
            "\"event_id\":\"%s\","
            "\"cam_id\":\"%s\","
            "\"container\":\"%s\","
            "\"codec\":\"%s\","
            "\"path\":\"%s\","
            "\"size_bytes\":%" PRIu64 ","
            "\"duration_ms\":%" PRIu64 ","
            "\"pre_roll_ms\":%" PRIu64 ","
            "\"post_roll_ms\":%" PRIu64 ","
            "\"sha256\":\"%s\""
            "}\n",
            hdr->event_id, hdr->cam_id_str, hdr->container, hdr->codec,
            hdr->path, hdr->size_bytes, hdr->duration_ms,
            hdr->pre_roll_ms, hdr->post_roll_ms, hdr->sha256);

    fclose(fp);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Recorder pool                                                            */
/* ---------------------------------------------------------------------- */

struct emd_recorder_pool {
    emd_recorder_cfg_t    cfg;
    emd_ringbuf_t       **ring_map;
    uint32_t              ring_map_count;
    emd_event_bus_t      *bus;

    pthread_t            *threads;
    uint32_t              thread_count;
    volatile bool         running;
};

typedef struct {
    struct emd_recorder_pool *pool;
    uint32_t                   worker_id;
} worker_arg_t;

static void *recorder_worker(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    struct emd_recorder_pool *pool = wa->pool;
    free(wa);

    const emd_mux_backend_t *mux =
        (pool->cfg.container == EMD_CONTAINER_FMP4) ? &emd_mux_fmp4 : &emd_mux_mpegts;

    while (pool->running) {
        emd_event_t ev;
        if (emd_event_bus_pop(pool->bus, &ev) < 0) {
            /* Nothing to do — yield */
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
            nanosleep(&ts, NULL);
            continue;
        }

        if (ev.cam_id >= pool->ring_map_count) {
            EMD_LOGE("recorder", "cam_id out of range");
            continue;
        }

        emd_ringbuf_t *rb = pool->ring_map[ev.cam_id];
        if (!rb) continue;

        emd_ringbuf_snap_t snap;
        if (emd_ringbuf_snapshot(rb, ev.pre_roll_pts, ev.post_roll_pts, &snap) < 0) {
            EMD_LOGW("recorder", "no data in snapshot range");
            continue;
        }

        emd_clip_header_t hdr;
        int r = emd_recorder_write_clip(&pool->cfg, &ev, &snap, mux, &hdr);
        emd_ringbuf_snapshot_release(&snap);

        if (r == 0) {
            emd_recorder_write_sidecar(&hdr);
        } else {
            EMD_LOGE("recorder", "write_clip failed");
        }
    }

    return NULL;
}

emd_recorder_pool_t *emd_recorder_pool_new(const emd_recorder_cfg_t *cfg,
                                            emd_ringbuf_t **ring_map,
                                            uint32_t ring_map_count)
{
    emd_recorder_pool_t *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->cfg = *cfg;
    pool->ring_map       = ring_map;
    pool->ring_map_count = ring_map_count;

    uint32_t tc = cfg->thread_count;
    if (tc == 0) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        tc = (uint32_t)(ncpu > 2 ? 2 : 1);
    }
    pool->thread_count = tc;
    pool->threads = calloc(tc, sizeof(pthread_t));
    if (!pool->threads) { free(pool); return NULL; }

    return pool;
}

int emd_recorder_pool_start(emd_recorder_pool_t *pool, emd_event_bus_t *bus) {
    if (!pool || !bus) return -1;
    pool->bus     = bus;
    pool->running = true;

    for (uint32_t i = 0; i < pool->thread_count; i++) {
        worker_arg_t *wa = calloc(1, sizeof(*wa));
        if (!wa) return -1;
        wa->pool      = pool;
        wa->worker_id = i;
        if (pthread_create(&pool->threads[i], NULL, recorder_worker, wa) != 0) {
            free(wa);
            return -1;
        }
    }
    return 0;
}

void emd_recorder_pool_stop(emd_recorder_pool_t *pool) {
    if (!pool) return;
    pool->running = false;
    for (uint32_t i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
}

void emd_recorder_pool_free(emd_recorder_pool_t *pool) {
    if (!pool) return;
    free(pool->threads);
    free(pool);
}
