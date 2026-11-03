/* SPDX-License-Identifier: MIT */
/* Native CedarC OSD bitmap helper, one context per encoder.
 * Caller MUST serialize update + synchronous VideoEncodeOneFrame for the same
 * encoder. Call update only for the input at the head of its accepted FIFO.
 * Bitmap remains alive until encoder destruction; do not update during encode.
 * Coordinates/layout shared with validate_dynamic.py. No camera pixels touched. */
#ifndef A733_DYNAMIC_OVERLAY_H
#define A733_DYNAMIC_OVERLAY_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <float.h>
#include "vencoder.h"
#define TS_W 320
#define TS_H 96
#define TS_X 32
#define TS_Y 32
#define TS_CW 24
#define TS_CH 32
struct ts_stat { double wall_sum,cpu_sum,wall_min,wall_max,cpu_min,cpu_max; };
struct ts_overlay {
    uint32_t *bitmap;
    uint32_t atlas[12][TS_CW*TS_CH];
    char previous[2][12];
    VencOverlayInfoS info;
    uint64_t calls,failures,cells_updated;
    struct ts_stat render,set;
};
static double ts_clock(clockid_t id) {struct timespec t;clock_gettime(id,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static void ts_stat_add(struct ts_stat *s,double w,double c) {
    s->wall_sum+=w;s->cpu_sum+=c;
    if(w<s->wall_min)s->wall_min=w;
    if(w>s->wall_max)s->wall_max=w;
    if(c<s->cpu_min)s->cpu_min=c;
    if(c>s->cpu_max)s->cpu_max=c;
}
static int ts_overlay_init(struct ts_overlay *s,int width,int height) {
    static const unsigned char font[12][7]={
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14},{14,17,17,15,1,1,14},
        {31,16,16,30,16,16,16}, /* F */
        {31,4,4,4,4,4,4}       /* T: stream PTS milliseconds */
    };
    if(width<TS_X+TS_W || height<TS_Y+TS_H)return -1;
    double w0=ts_clock(CLOCK_MONOTONIC),c0=ts_clock(CLOCK_THREAD_CPUTIME_ID);
    memset(s,0,sizeof(*s));
    s->render.wall_min=s->render.cpu_min=s->set.wall_min=s->set.cpu_min=DBL_MAX;
    s->bitmap=malloc(TS_W*TS_H*4);if(!s->bitmap)return -1;
    for(int y=0;y<TS_H;y++)for(int x=0;x<TS_W;x++)
        s->bitmap[y*TS_W+x]=(x<3||x>=TS_W-3||y<3||y>=TS_H-3)?0xffffffff:0xff101010;
    for(int k=0;k<12;k++) {
        for(int i=0;i<TS_CW*TS_CH;i++)s->atlas[k][i]=0xff101010;
        for(int y=0;y<7;y++)for(int x=0;x<5;x++)if(font[k][y]&(1<<(4-x)))
            for(int dy=0;dy<4;dy++)for(int dx=0;dx<4;dx++)
                s->atlas[k][(y*4+dy)*TS_CW+x*4+dx]=0xffffffff;
    }
    s->info.blk_num=1;s->info.argb_type=VENC_OVERLAY_ARGB8888;
    VencOverlayHeaderS *h=&s->info.overlayHeaderList[0];
    h->start_mb_x=TS_X/16;h->end_mb_x=(TS_X+TS_W)/16-1;
    h->start_mb_y=TS_Y/16;h->end_mb_y=(TS_Y+TS_H)/16-1;
    h->overlay_type=NORMAL_OVERLAY;h->overlay_blk_addr=(unsigned char*)s->bitmap;
    h->bitmap_size=TS_W*TS_H*4;
    fprintf(stderr,"TS_INIT bitmap_bytes=%u wall_us=%.3f cpu_us=%.3f\n",h->bitmap_size,
      (ts_clock(CLOCK_MONOTONIC)-w0)*1e6,(ts_clock(CLOCK_THREAD_CPUTIME_ID)-c0)*1e6);
    return 0;
}
static int ts_overlay_update(struct ts_overlay *s,VideoEncoder *enc,int64_t pts_us,uint64_t sequence) {
    /* Fail instead of wrapping counters silently and invalidating uniqueness. */
    if(!s->bitmap||pts_us<0||pts_us/1000>9999999999LL||sequence>999999)return -1;
    double w0=ts_clock(CLOCK_MONOTONIC),c0=ts_clock(CLOCK_THREAD_CPUTIME_ID);
    char text[2][12];
    snprintf(text[0],sizeof(text[0]),"F%06llu",(unsigned long long)sequence);
    snprintf(text[1],sizeof(text[1]),"T%010llu",(unsigned long long)(pts_us/1000));
    for(int row=0;row<2;row++)for(int col=0;text[row][col];col++) {
        char c=text[row][col];if(c==s->previous[row][col])continue;
        int k=(c=='F')?10:(c=='T'?11:c-'0');
        for(int y=0;y<TS_CH;y++)memcpy(s->bitmap+(12+row*40+y)*TS_W+12+col*TS_CW,
             s->atlas[k]+y*TS_CW,TS_CW*4);
        s->previous[row][col]=c;s->cells_updated++;
    }
    double c1=ts_clock(CLOCK_THREAD_CPUTIME_ID),w1=ts_clock(CLOCK_MONOTONIC);
    int rc=VideoEncSetParameter(enc,VENC_IndexParamSetOverlay,&s->info);
    double c2=ts_clock(CLOCK_THREAD_CPUTIME_ID),w2=ts_clock(CLOCK_MONOTONIC);
    ts_stat_add(&s->render,w1-w0,c1-c0);ts_stat_add(&s->set,w2-w1,c2-c1);
    s->calls++;if(rc)s->failures++;
    /* Interposer records sequence/PTS and encode return separately, identically
       for off/static/dynamic modes. No per-frame printing within timed helper. */
    return rc;
}
static void ts_overlay_report(struct ts_overlay *s) {
    fprintf(stderr,"TS_SUMMARY updates=%llu failures=%llu changed_cells=%llu render_wall_us=%.3f render_cpu_us=%.3f render_wall_min_us=%.3f render_wall_max_us=%.3f render_cpu_min_us=%.3f render_cpu_max_us=%.3f set_wall_us=%.3f set_cpu_us=%.3f set_wall_min_us=%.3f set_wall_max_us=%.3f set_cpu_min_us=%.3f set_cpu_max_us=%.3f\n",
    (unsigned long long)s->calls,(unsigned long long)s->failures,(unsigned long long)s->cells_updated,
    s->render.wall_sum*1e6,s->render.cpu_sum*1e6,s->calls?s->render.wall_min*1e6:0,s->render.wall_max*1e6,
    s->calls?s->render.cpu_min*1e6:0,s->render.cpu_max*1e6,
    s->set.wall_sum*1e6,s->set.cpu_sum*1e6,s->calls?s->set.wall_min*1e6:0,s->set.wall_max*1e6,
    s->calls?s->set.cpu_min*1e6:0,s->set.cpu_max*1e6);
}
static void ts_overlay_free(struct ts_overlay *s) {free(s->bitmap);s->bitmap=NULL;}
#endif
