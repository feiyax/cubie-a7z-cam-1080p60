/* SPDX-License-Identifier: MIT */
/* IMX214 VIN ioctl regression test. No STREAMON/QBUF or ISP activity.
 * Run exclusively under the camera lock. Leaves normal 1080p30 selected.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define VIDEO_MODE 2 /* Allwinner UAPI V4L2_MODE_VIDEO */
static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

static int set_rate(int fd, unsigned num, unsigned den, int want_errno)
{
    struct v4l2_streamparm p = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    p.parm.capture.capturemode = VIDEO_MODE;
    p.parm.capture.timeperframe.numerator = num;
    p.parm.capture.timeperframe.denominator = den;
    errno = 0;
    int r = xioctl(fd, VIDIOC_S_PARM, &p), e = errno;
    printf("S_PARM %u/%u: rc=%d errno=%d (%s), returned=%u/%u\n", num, den, r,
           r < 0 ? e : 0, r < 0 ? strerror(e) : "success",
           p.parm.capture.timeperframe.numerator,
           p.parm.capture.timeperframe.denominator);
    if ((want_errno && (r != -1 || e != want_errno)) || (!want_errno && r < 0)) {
        fprintf(stderr, "FAIL: expected %s\n",
                want_errno ? strerror(want_errno) : "success");
        return -1;
    }
    return 0;
}

static int expect_rate(int fd, unsigned fps)
{
    struct v4l2_streamparm p = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    if (xioctl(fd, VIDIOC_G_PARM, &p) < 0) {
        perror("G_PARM");
        return -1;
    }
    printf("G_PARM: %u/%u (expected 1/%u)\n", p.parm.capture.timeperframe.numerator,
           p.parm.capture.timeperframe.denominator, fps);
    if (p.parm.capture.timeperframe.numerator != 1 ||
        p.parm.capture.timeperframe.denominator != fps) {
        fprintf(stderr, "FAIL: selected interval changed unexpectedly\n");
        return -1;
    }
    return 0;
}

static int request_buffers(int fd, unsigned count)
{
    struct v4l2_requestbuffers q = {.count = count,
                                    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                    .memory = V4L2_MEMORY_MMAP};
    if (xioctl(fd, VIDIOC_REQBUFS, &q) < 0) {
        perror("REQBUFS");
        return -1;
    }
    printf("REQBUFS requested=%u returned=%u\n", count, q.count);
    if (count && !q.count) {
        fprintf(stderr, "FAIL: no buffers allocated\n");
        return -1;
    }
    return 0;
}

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if ((expr) < 0)                                                                \
            goto cleanup;                                                              \
    } while (0)
int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/video0";
    int fd, input = 0, result = 1;
    bool selected = false, buffers = false;
    struct v4l2_capability cap = {0};
    struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [/dev/video0]\n", argv[0]);
        return 2;
    }
    fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror(dev);
        return 1;
    }
    CHECK(xioctl(fd, VIDIOC_QUERYCAP, &cap));
    printf("device=%s driver=%.16s card=%.32s\n", dev, cap.driver, cap.card);
    CHECK(xioctl(fd, VIDIOC_S_INPUT, &input));
    selected = true;
    CHECK(set_rate(fd, 1, 30, 0));
    f.fmt.pix_mp.width = 1920;
    f.fmt.pix_mp.height = 1080;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    CHECK(xioctl(fd, VIDIOC_S_FMT, &f));
    printf("S_FMT returned %ux%u fourcc=0x%08x planes=%u\n", f.fmt.pix_mp.width,
           f.fmt.pix_mp.height, f.fmt.pix_mp.pixelformat, f.fmt.pix_mp.num_planes);
    if (f.fmt.pix_mp.width != 1920 || f.fmt.pix_mp.height != 1080 ||
        f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M) {
        fprintf(stderr, "FAIL: requested format was not selected\n");
        goto cleanup;
    }
    CHECK(expect_rate(fd, 30));
    CHECK(set_rate(fd, 0, 0, 0));
    CHECK(expect_rate(fd, 30));
    CHECK(set_rate(fd, 0, 60, EINVAL));
    CHECK(expect_rate(fd, 30));
    CHECK(set_rate(fd, 1, 0, EINVAL));
    CHECK(expect_rate(fd, 30));
    /* Set before call so cleanup also handles a partially successful allocation. */
    buffers = true;
    CHECK(request_buffers(fd, 3));
    CHECK(set_rate(fd, 1, 60, EBUSY));
    CHECK(expect_rate(fd, 30));
    CHECK(request_buffers(fd, 0));
    buffers = false;
    CHECK(set_rate(fd, 1, 60, 0));
    CHECK(expect_rate(fd, 60));
    CHECK(set_rate(fd, 0, 0, 0));
    CHECK(expect_rate(fd, 60));
    CHECK(set_rate(fd, 0, 30, EINVAL));
    CHECK(expect_rate(fd, 60));
    CHECK(set_rate(fd, 1, 0, EINVAL));
    CHECK(expect_rate(fd, 60));
    CHECK(set_rate(fd, 1, 30, 0));
    CHECK(expect_rate(fd, 30));
    result = 0;
cleanup:
    if (buffers && request_buffers(fd, 0) < 0)
        result = 1;
    if (selected) {
        if (set_rate(fd, 1, 30, 0) < 0 || expect_rate(fd, 30) < 0) {
            fprintf(stderr, "FAIL: cleanup could not restore 30 fps\n");
            result = 1;
        }
    }
    if (close(fd) < 0) {
        perror("close");
        result = 1;
    }
    puts(result ? "FAIL: rate ioctl regression"
                : "PASS: rate ioctl regression; buffers released; selected 1080p30; no "
                  "streaming performed");
    return result;
}
