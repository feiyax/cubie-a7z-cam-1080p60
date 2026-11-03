/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include "vencoder.h"
#include <pthread.h>
#include <sys/syscall.h>
static pthread_mutex_t enc_mutex = PTHREAD_MUTEX_INITIALIZER;
struct queued {
    long long pts;
    unsigned long id;
};

static struct queued q[256];
static unsigned qhead, qtail;
static unsigned input_width, input_height;
#include "dynamic_overlay.h"
static struct ts_overlay osd;
static int osdmode, osdready;
static unsigned maxqueued;

struct range {
    uintptr_t base;
    size_t len;
    int fd;
};

static struct range ranges[64];
static atomic_int nranges, nframes, nimports, nframecopies, nalloc, nmaps, nlarge;
static atomic_ulong camera_copy_bytes, large_copy_bytes;
static int exported[1024];
static __thread int resolving;
static void *vendor_symbol(const char *name)
{
    void *h = dlopen("libvencoder.so", RTLD_NOW | RTLD_NOLOAD);
    void *p = h ? dlsym(h, name) : NULL;
    if (!p) {
        fprintf(stderr, "AUDIT unresolved %s\n", name);
        _exit(125);
    }
    return p;
}

/* Evidence mapping only; never maps or touches camera pixel memory. */
static pthread_mutex_t evidence_mutex = PTHREAD_MUTEX_INITIALIZER;
struct fd_evidence {
    int valid, video_fd;
    unsigned index, plane;
    uintptr_t iommu;
};

struct capture_evidence {
    int valid, video_fd;
    unsigned index, seq, flags;
    unsigned long long host_ns;
    long long driver_us;
};

static struct fd_evidence fd_evidence[1024];
static struct capture_evidence capture_evidence[128];
static unsigned long long mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static void log_input_link(unsigned n, VencInputBuffer *b)
{
    pthread_mutex_lock(&evidence_mutex);
    int found = -1;
    for (int f = 0; f < 1024; f++)
        if (fd_evidence[f].valid && fd_evidence[f].iommu &&
            fd_evidence[f].iommu == (uintptr_t)b->pAddrPhyY) {
            if (found != -1) {
                found = -2;
                break;
            }
            found = f;
        }
    if (found >= 0) {
        struct fd_evidence *f = &fd_evidence[found];
        struct capture_evidence *c =
            f->index < 128 ? &capture_evidence[f->index] : NULL;
        if (c && c->valid && c->video_fd == f->video_fd)
            fprintf(stderr,
                    "CAM_LINK input_n=%u input_id=%lu pts_us=%lld source_seq=%u "
                    "index=%u dma_fd=%d iommu=0x%lx driver_us=%lld "
                    "capture_host_ns=%llu link_host_ns=%llu\n",
                    n, b->nID, b->nPts, c->seq, c->index, found,
                    (unsigned long)f->iommu, c->driver_us, c->host_ns, mono_ns());
        else
            fprintf(stderr, "CAM_LINK_MISSING input_n=%u reason=no_capture dma_fd=%d\n",
                    n, found);
    } else
        fprintf(stderr, "CAM_LINK_MISSING input_n=%u reason=no_unique_iommu phyY=%p\n",
                n, b->pAddrPhyY);
    pthread_mutex_unlock(&evidence_mutex);
}

static int camera_fd(int fd)
{
    if (fd >= 0 && fd < 1024 && exported[fd])
        return 1;
    char p[64], b[128];
    snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, b, sizeof(b) - 1);
    if (n < 0)
        return 0;
    b[n] = 0;
    return strstr(b, "/dev/video0") != NULL;
}

/* Inspect only ioctl structures supplied by the application; no new ioctls. */
static void log_camera_ioctl(const char *stage, int fd, unsigned long req, void *arg,
                             int result)
{
    if (!arg)
        return;
    if (req == VIDIOC_S_PARM || req == VIDIOC_G_PARM) {
        struct v4l2_streamparm *p = arg;
        if (p->type == V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            p->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            struct v4l2_captureparm *c = &p->parm.capture;
            fprintf(stderr,
                    "CAM_PARM stage=%s op=%s fd=%d rc=%d type=%u numerator=%u "
                    "denominator=%u capturemode=%u reserved0=%u reserved1=%u "
                    "reserved2=%u host_mono_ns=%llu\n",
                    stage, req == VIDIOC_S_PARM ? "S_PARM" : "G_PARM", fd, result,
                    p->type, c->timeperframe.numerator, c->timeperframe.denominator,
                    c->capturemode, c->reserved[0], c->reserved[1], c->reserved[2],
                    mono_ns());
        } else
            fprintf(stderr,
                    "CAM_PARM stage=%s op=%s fd=%d rc=%d type=%u unsupported_type=1 "
                    "host_mono_ns=%llu\n",
                    stage, req == VIDIOC_S_PARM ? "S_PARM" : "G_PARM", fd, result,
                    p->type, mono_ns());
    }
    if (req == VIDIOC_S_FMT) {
        struct v4l2_format *f = arg;
        unsigned w = 0, h = 0, pix = 0;
        if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            w = f->fmt.pix_mp.width;
            h = f->fmt.pix_mp.height;
            pix = f->fmt.pix_mp.pixelformat;
        } else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            w = f->fmt.pix.width;
            h = f->fmt.pix.height;
            pix = f->fmt.pix.pixelformat;
        }
        fprintf(stderr,
                "CAM_FMT stage=%s op=S_FMT fd=%d rc=%d type=%u width=%u height=%u "
                "pixelformat=0x%x host_mono_ns=%llu\n",
                stage, fd, result, f->type, w, h, pix, mono_ns());
    }
}

int ioctl(int fd, unsigned long req, ...)
{
    static int (*real)(int, unsigned long, ...);
    if (!real)
        real = dlsym(RTLD_NEXT, "ioctl");
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    log_camera_ioctl("pre", fd, req, arg, 0);
    int r = real(fd, req, arg);
    int saved_errno = errno;
    log_camera_ioctl("post", fd, req, arg, r);
    if (req == VIDIOC_EXPBUF && r == 0) {
        struct v4l2_exportbuffer *b = arg;
        if (b->fd >= 0 && b->fd < 1024) {
            exported[b->fd] = 1;
            pthread_mutex_lock(&evidence_mutex);
            fd_evidence[b->fd] = (struct fd_evidence){
                .valid = 1, .video_fd = fd, .index = b->index, .plane = b->plane};
            pthread_mutex_unlock(&evidence_mutex);
        }
        fprintf(stderr, "AUDIT EXPBUF video_fd=%d index=%u plane=%u dma_fd=%d\n", fd,
                b->index, b->plane, b->fd);
    }
    if (req == VIDIOC_DQBUF && r == 0) {
        struct v4l2_buffer *b = arg;
        if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
            b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            unsigned long long ns = mono_ns(), bytes = b->bytesused;
            if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                bytes = 0;
                for (unsigned k = 0; k < b->length; k++)
                    bytes += b->m.planes[k].bytesused;
            }
            long long us =
                (long long)b->timestamp.tv_sec * 1000000 + b->timestamp.tv_usec;
            pthread_mutex_lock(&evidence_mutex);
            if (b->index < 128)
                capture_evidence[b->index] = (struct capture_evidence){
                    1, fd, b->index, b->sequence, b->flags, ns, us};
            pthread_mutex_unlock(&evidence_mutex);
            fprintf(stderr,
                    "BENCH CAPTURE seq=%u index=%u sec=%ld usec=%ld flags=%u "
                    "host_mono_ns=%llu bytesused=%llu\n",
                    b->sequence, b->index, (long)b->timestamp.tv_sec,
                    (long)b->timestamp.tv_usec, b->flags, ns, bytes);
            fprintf(stderr,
                    "CAM_CAPTURE source_seq=%u video_fd=%d index=%u driver_s=%.9f "
                    "host_s=%.9f host_mono_ns=%llu flags=%u bytesused=%llu error=%u\n",
                    b->sequence, fd, b->index, us / 1e6, ns / 1e9, ns, b->flags, bytes,
                    !!(b->flags & V4L2_BUF_FLAG_ERROR));
        }
    }
    errno = saved_errno;
    return r;
}

void *mmap(void *a, size_t n, int prot, int flags, int fd, off_t off)
{
    static void *(*real)(void *, size_t, int, int, int, off_t);
    if (!real)
        real = dlsym(RTLD_NEXT, "mmap");
    void *p = real(a, n, prot, flags, fd, off);
    if (p != MAP_FAILED && fd >= 0 && camera_fd(fd)) {
        int i = atomic_fetch_add(&nranges, 1);
        if (i < 64)
            ranges[i] = (struct range){(uintptr_t)p, n, fd};
        fprintf(stderr, "AUDIT CAMERA_MMAP fd=%d addr=%p len=%zu\n", fd, p, n);
    }
    return p;
}

void *mmap64(void *a, size_t n, int p, int f, int fd, off64_t o)
{
    return mmap(a, n, p, f, fd, o);
}

void *memcpy(void *d, const void *s, size_t n)
{
    static void *(*real)(void *, const void *, size_t);
    if (!real) {
        if (resolving) {
            volatile unsigned char *dd = d;
            const volatile unsigned char *ss = s;
            for (size_t i = 0; i < n; i++)
                dd[i] = ss[i];
            return d;
        }
        resolving = 1;
        real = dlsym(RTLD_NEXT, "memcpy");
        resolving = 0;
    }
    int lim = atomic_load(&nranges);
    if (lim > 64)
        lim = 64;
    for (int i = 0; i < lim; i++)
        if ((uintptr_t)s < ranges[i].base + ranges[i].len &&
            (uintptr_t)s + n > ranges[i].base) {
            atomic_fetch_add(&camera_copy_bytes, n);
            break;
        }
    if (n >= 65536) {
        atomic_fetch_add(&large_copy_bytes, n);
        int i = atomic_fetch_add(&nlarge, 1);
        if (i < 5) {
            Dl_info di = {0};
            dladdr(__builtin_return_address(0), &di);
            fprintf(stderr, "AUDIT LARGE_MEMCPY bytes=%zu caller=%s\n", n,
                    di.dli_fname ? di.dli_fname : "?");
        }
    }
    return real(d, s, n);
}

int gst_video_frame_copy(void *d, const void *s)
{
    static int (*real)(void *, const void *);
    if (!real) {
        void *h = dlopen("libgstvideo-1.0.so.0", RTLD_NOW | RTLD_NOLOAD);
        real = h ? dlsym(h, "gst_video_frame_copy") : NULL;
        if (!real)
            _exit(119);
    }
    atomic_fetch_add(&nframecopies, 1);
    return real(d, s);
}

int gst_memory_map(void *m, void *info, int flags)
{
    static int (*real)(void *, void *, int);
    static int (*isdma)(void *);
    static int (*getfd)(void *);
    if (!real) {
        real = dlsym(RTLD_NEXT, "gst_memory_map");
        isdma = dlsym(RTLD_NEXT, "gst_is_dmabuf_memory");
        getfd = dlsym(RTLD_NEXT, "gst_dmabuf_memory_get_fd");
    }
    if (isdma && isdma(m)) {
        int k = atomic_fetch_add(&nmaps, 1);
        if (k < 8)
            fprintf(stderr, "AUDIT GST_DMABUF_MAP fd=%d flags=%d\n", getfd(m), flags);
    }
    return real(m, info, flags);
}

void VideoEncoderGetVeIommuAddr(VideoEncoder *e, struct user_iommu_param *p)
{
    static void (*real)(VideoEncoder *, struct user_iommu_param *);
    if (!real)
        real = vendor_symbol("VideoEncoderGetVeIommuAddr");
    real(e, p);
    pthread_mutex_lock(&evidence_mutex);
    if (p->fd >= 0 && p->fd < 1024 && fd_evidence[p->fd].valid)
        fd_evidence[p->fd].iommu = p->iommu_addr;
    pthread_mutex_unlock(&evidence_mutex);
    atomic_fetch_add(&nimports, 1);
    fprintf(stderr, "AUDIT VE_IMPORT dma_fd=%d iommu=0x%x camera_export=%d\n", p->fd,
            p->iommu_addr, p->fd >= 0 && p->fd < 1024 ? exported[p->fd] : 0);
}

int AddOneInputBuffer(VideoEncoder *e, VencInputBuffer *b)
{
    static int (*real)(VideoEncoder *, VencInputBuffer *);
    if (!real)
        real = vendor_symbol("AddOneInputBuffer");
    pthread_mutex_lock(&enc_mutex);
    int r = real(e, b);
    if (r == 0) {
        unsigned n = atomic_fetch_add(&nframes, 1);
        log_input_link(n, b);
        q[qtail++ % 256] = (struct queued){b->nPts, b->nID};
        if (qtail - qhead > maxqueued)
            maxqueued = qtail - qhead;
        if (qtail - qhead > 250) {
            fprintf(stderr, "BENCH QUEUE_OVERFLOW\n");
            _exit(123);
        }
        fprintf(stderr, "BENCH INPUT n=%u id=%lu pts=%lld phyY=%p phyC=%p virY=%p\n", n,
                b->nID, b->nPts, b->pAddrPhyY, b->pAddrPhyC, b->pAddrVirY);
    }
    pthread_mutex_unlock(&enc_mutex);
    return r;
}

int AllocInputBuffer(VideoEncoder *e, VencAllocateBufferParam *p)
{
    static int (*real)(VideoEncoder *, VencAllocateBufferParam *);
    if (!real)
        real = vendor_symbol("AllocInputBuffer");
    atomic_fetch_add(&nalloc, 1);
    return real(e, p);
}

__attribute__((destructor)) static void summary(void)
{
    fprintf(
        stderr,
        "AUDIT SUMMARY encoder_inputs=%d ve_imports=%d encoder_alloc_input_calls=%d "
        "gst_frame_copy_calls=%d gst_dmabuf_maps=%d camera_mappings=%d "
        "camera_memcpy_bytes=%lu large_memcpy_calls=%d large_memcpy_bytes=%lu\n",
        nframes, nimports, nalloc, nframecopies, nmaps, nranges, camera_copy_bytes,
        nlarge, large_copy_bytes);
}

int VideoEncSetParameter(VideoEncoder *e, VENC_INDEXTYPE i, void *p)
{
    static int (*real)(VideoEncoder *, VENC_INDEXTYPE, void *);
    if (!real)
        real = vendor_symbol("VideoEncSetParameter");
    return real(e, i, p);
}

int VideoEncInit(VideoEncoder *e, VencBaseConfig *c)
{
    static int (*real)(VideoEncoder *, VencBaseConfig *);
    if (!real)
        real = vendor_symbol("VideoEncInit");
    int r = real(e, c);
    input_width = c->nDstWidth;
    input_height = c->nDstHeight;
    const char *m = getenv("A733_TIMESTAMP_MODE");
    osdmode = m && !strcmp(m, "dynamic") ? 2 : (m && !strcmp(m, "static") ? 1 : 0);
    if (r == 0 && osdmode) {
        if (ts_overlay_init(&osd, input_width, input_height) != 0)
            _exit(122);
        osdready = 1;
    }
    return r;
}

int VideoEncodeOneFrame(VideoEncoder *e)
{
    static int (*real)(VideoEncoder *);
    if (!real)
        real = vendor_symbol("VideoEncodeOneFrame");
    pthread_mutex_lock(&enc_mutex);
    unsigned n = qhead;
    int have = qhead != qtail;
    struct queued v = have ? q[qhead % 256] : (struct queued){0};
    if (have && osdready && (osdmode == 2 || (osdmode == 1 && n == 0))) {
        if (ts_overlay_update(&osd, e, osdmode == 2 ? v.pts : 0,
                              osdmode == 2 ? n : 0) != 0)
            _exit(121);
    }
    int r = real(e);
    if (have) {
        fprintf(stderr, "TS_FRAME seq=%u pts=%lld rc=%d\n", n, v.pts, r);
        fprintf(stderr, "BENCH ENCODE n=%u id=%lu pts=%lld rc=%d tid=%ld\n", n, v.id,
                v.pts, r, syscall(SYS_gettid));
        qhead++;
        if (r != 0) {
            fprintf(stderr, "BENCH ENCODE_FAIL_FAST rc=%d\n", r);
            _exit(120);
        }
    }
    pthread_mutex_unlock(&enc_mutex);
    return r;
}

void VideoEncDestroy(VideoEncoder *e)
{
    static void (*real)(VideoEncoder *);
    if (!real)
        real = vendor_symbol("VideoEncDestroy");
    real(e);
    if (osdready) {
        ts_overlay_report(&osd);
        ts_overlay_free(&osd);
        osdready = 0;
    }
    fprintf(stderr, "BENCH QUEUE max=%u pending=%u\n", maxqueued, qtail - qhead);
}

int AlreadyUsedInputBuffer(VideoEncoder *e, VencInputBuffer *b)
{
    static int (*real)(VideoEncoder *, VencInputBuffer *);
    if (!real)
        real = vendor_symbol("AlreadyUsedInputBuffer");
    int r = real(e, b);
    if (r == 0)
        fprintf(stderr, "BENCH USED id=%lu pts=%lld\n", b->nID, b->nPts);
    return r;
}

/* Retrieval completion is NOT file persistence or downstream sink completion. */
int GetOneBitstreamFrame(VideoEncoder *e, VencOutputBuffer *b)
{
    static int (*real)(VideoEncoder *, VencOutputBuffer *);
    if (!real)
        real = vendor_symbol("GetOneBitstreamFrame");
    int r = real(e, b);
    unsigned long long ns = mono_ns();
    if (r == 0) {
        pthread_mutex_lock(&enc_mutex);
        long long n = -1;
        unsigned matches = 0;
        unsigned start = qtail > 256 ? qtail - 256 : 0;
        for (unsigned k = start; k < qtail; k++)
            if (q[k % 256].pts == b->nPts) {
                n = k;
                matches++;
            }
        if (matches == 1)
            fprintf(stderr,
                    "CAM_OUTPUT input_n=%lld pts_us=%lld done_s=%.9f host_mono_ns=%llu "
                    "bytes=%llu stage=get_bitstream\n",
                    n, b->nPts, ns / 1e9, ns,
                    (unsigned long long)b->nSize0 + b->nSize1 + b->nSize2);
        else
            fprintf(stderr,
                    "CAM_OUTPUT_MISSING pts_us=%lld matches=%u host_mono_ns=%llu\n",
                    b->nPts, matches, ns);
        pthread_mutex_unlock(&enc_mutex);
    }
    return r;
}
