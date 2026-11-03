/* SPDX-License-Identifier: MIT */
/* Minimal dynamic GStreamer API client: no ABI-private structures accessed. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s EXISTING_OUTPUT_DIRECTORY\n", argv[0]);
        return 2;
    }
    void *lib = dlopen("libgstreamer-1.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        fprintf(stderr, "%s\n", dlerror());
        return 2;
    }
    void (*init)(int *, char ***) = dlsym(lib, "gst_init");
    void *(*parse)(const char *, void **) = dlsym(lib, "gst_parse_launch");
    int (*state)(void *, int) = dlsym(lib, "gst_element_set_state");
    int (*getstate)(void *, int *, int *, uint64_t) =
        dlsym(lib, "gst_element_get_state");
    void *(*getbus)(void *) = dlsym(lib, "gst_element_get_bus");
    void *(*pop)(void *, uint64_t, unsigned) = dlsym(lib, "gst_bus_timed_pop_filtered");
    void (*unref)(void *) = dlsym(lib, "gst_object_unref");
    void (*munref)(void *) = dlsym(lib, "gst_mini_object_unref");
    if (!init || !parse || !state || !getstate || !getbus || !pop || !unref || !munref)
        return 2;
    init(NULL, NULL);
    int counts[] = {1, 7, 180, 7};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        char description[2048];
        snprintf(
            description, sizeof(description),
            "v4l2src device=/dev/video0 en-awisp=1 en-largemode=0 io-mode=dmabuf "
            "num-buffers=%d ! "
            "video/x-raw,format=NV12,width=1920,height=1080,framerate=60/1 ! queue "
            "max-size-buffers=4 max-size-bytes=0 max-size-time=0 ! omxh264videoenc "
            "target-bitrate=12000000 ! filesink location=%s/repeat-%u-%d.h264",
            counts[i], argv[1], i, counts[i]);
        void *error = NULL, *pipeline = parse(description, &error);
        if (!pipeline || error) {
            fprintf(stderr, "parse failed\n");
            return 3;
        }
        void *bus = getbus(pipeline);
        int ok = state(pipeline, 4) != 0; /* GST_STATE_PLAYING */
        void *eos = ok ? pop(bus, 10000000000ULL, 1)
                       : NULL; /* GST_MESSAGE_EOS only; bounded on error */
        state(pipeline, 1);
        getstate(pipeline, NULL, NULL, 5000000000ULL); /* GST_STATE_NULL */
        if (eos) {
            munref(eos);
        }
        unref(bus);
        unref(pipeline);
        printf("REPEAT completed=%u frames=%d eos=%d\n", i, counts[i], eos != NULL);
        fflush(stdout);
        if (!eos)
            return 4;
    }
    return 0;
}
