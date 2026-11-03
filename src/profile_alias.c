/* SPDX-License-Identifier: MIT */
/* Process-local ISP lookup alias. Does not change sensor timing or pixel data.
 * Verified against installed ISP602 parser_ini_info ABI and vendor source.
 * Explicit opt-in: A733_IMX214_60_PROFILE=1080p30; otherwise logging only.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
static atomic_int mode60[4];
typedef int (*parser_fn)(void *, char *, char *, int, int, int, int, int, int, int);
static parser_fn real_parser;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void resolve(void)
{
    /* Vendor GStreamer dlopens ISP in a local scope: RTLD_NEXT is insufficient.
     * NOLOAD prevents this probe from loading or initializing another library.
     * Keep its handle open for the process lifetime so the pointer stays valid.
     */
    void *h = dlopen("libisp.so", RTLD_LAZY | RTLD_NOLOAD);
    if (h) *(void **)(&real_parser) = dlsym(h, "parser_ini_info");
}
int a733_profile_resolver_ready(void)
{
    pthread_once(&once, resolve);
    return real_parser != NULL;
}
int parser_ini_info(void *param, char *name, char *sensor,
                    int width, int height, int fps, int wdr, int ir, int sync, int id)
{
    const char *opt = getenv("A733_IMX214_60_PROFILE");
    int lookup = fps, ret;
    if (id >= 0 && id < 4) atomic_store(&mode60[id], sensor && !strcmp(sensor,"imx214") && width==1920 && height==1080 && fps==60 && !wdr && !ir);
    pthread_once(&once, resolve);
    if (!real_parser) { fprintf(stderr, "ISP_ALIAS missing original parser_ini_info\n"); abort(); }
    if (opt && strcmp(opt, "1080p30") == 0 && sensor && strcmp(sensor, "imx214") == 0 &&
        width == 1920 && height == 1080 && fps == 60 && wdr == 0 && ir == 0)
        lookup = 30;
    fprintf(stderr, "ISP_ALIAS sensor=%s size=%dx%d actual_fps=%d lookup_fps=%d wdr=%d ir=%d isp=%d\n",
            sensor ? sensor : "NULL", width, height, fps, lookup, wdr, ir, id);
    ret = real_parser(param, name, sensor, width, height, lookup, wdr, ir, sync, id);
    fprintf(stderr, "ISP_ALIAS returned=%d profile=%.100s\n", ret, name ? name : "NULL");
    return ret;
}

/* Read-only tracing of the existing ISP sensor-update path. Layout verified
 * against installed ioctl size0x20 and gain offset12 in ARM64 disassembly.
 * isp_get_attr_cfg id7 copies8bytes (WB gains); id12 copies4bytes (temperature).
 */
#include <stdint.h>
#include <stdatomic.h>
struct sensor_exp_gain_602 { int exp, mid_exp, short_exp, gain, mid_gain, short_gain, r_gain, b_gain; };
static int (*real_exp_gain)(void *, struct sensor_exp_gain_602 *);
static int (*get_attr)(int, unsigned, void *);
static pthread_once_t trace_once = PTHREAD_ONCE_INIT;
static atomic_uint trace_count;
static void resolve_trace(void) {
    void *h = dlopen("libisp.so", RTLD_LAZY | RTLD_NOLOAD);
    if (h) {
        *(void **)(&real_exp_gain) = dlsym(h, "isp_sensor_set_exp_gain");
        *(void **)(&get_attr) = dlsym(h, "isp_get_attr_cfg");
    }
}
int isp_sensor_set_exp_gain(void *isp, struct sensor_exp_gain_602 *eg) {
    pthread_once(&trace_once, resolve_trace);
    if (!real_exp_gain) { fprintf(stderr,"ISP_TRACE original sensor updater unresolved\n"); abort(); }
    int ret = real_exp_gain(isp,eg);
    unsigned n = atomic_fetch_add(&trace_count,1);
    if (getenv("A733_ISP_TRACE") && n % 30 == 0) {
        uint16_t wb[4] = {0}; int temp=0, id; memcpy(&id,isp,sizeof(id));
        int wr = get_attr ? get_attr(id,7,wb) : -1;
        int tr = get_attr ? get_attr(id,12,&temp) : -1;
        fprintf(stderr,"ISP_TRACE n=%u isp=%d exp_raw=%d gain_raw=%d wb=%u,%u,%u,%u wb_rc=%d temp=%d temp_rc=%d update_rc=%d\n",
                n,id,eg->exp,eg->gain,wb[0],wb[1],wb[2],wb[3],wr,temp,tr,ret);
    }
    return ret;
}

/* Same AE-table policy invoked by vendor isp_set_fps, without its unsupported
 * sensor SET_FPS ioctl. Initialization is complete when isp_run is called;
 * update before the worker starts. No sensor clock/register mutation here.
 */
int isp_run(int id) {
    void *h = dlopen("libisp.so", RTLD_LAZY | RTLD_NOLOAD);
    int (*run)(int) = NULL, (*update)(void *, int) = NULL;
    void *(*getctx)(int) = NULL;
    if (h) {
        *(void **)(&run)=dlsym(h,"isp_run");
        *(void **)(&update)=dlsym(h,"isp_ctx_update_ae_tbl");
        *(void **)(&getctx)=dlsym(h,"isp_get_ctx_addr");
    }
    if (!run) { fprintf(stderr,"ISP_POLICY original isp_run missing\n"); abort(); }
    const char *opt=getenv("A733_IMX214_AE_FPS");
    if (opt && !strcmp(opt,"60") && id>=0 && id<4 && atomic_load(&mode60[id])) {
        if (!update || !getctx) { fprintf(stderr,"ISP_POLICY required API missing\n"); abort(); }
        void *ctx=getctx(id);
        if (!ctx) return -EINVAL;
        int ret=update(ctx,60);
        fprintf(stderr,"ISP_POLICY isp=%d AE minimum_fps=60 update_rc=%d before_worker_start\n",id,ret);
        if (ret<0) return ret;
    }
    return run(id);
}
