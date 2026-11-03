/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/uio.h>
#include <stdatomic.h>
/* Only compressed filesink writes to the explicit absolute trace path.
 * No payload mapping, buffering or scheduling changes. */
static atomic_ullong bytes_total, calls, max_ns;
static unsigned long long now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000ULL+t.tv_nsec;}
static int selected(int fd){
 const char *want=getenv("A733_SINK_TRACE_PATH");if(!want)return 0;
 char link[64],path[4096];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
 ssize_t n=readlink(link,path,sizeof(path)-1);if(n<0)return 0;path[n]=0;return !strcmp(path,want);
}
static void record(int fd,ssize_t n,unsigned long long start,unsigned long long done,const char *kind){
 unsigned long long total=atomic_fetch_add(&bytes_total,n>0?(unsigned long long)n:0)+(n>0?(unsigned long long)n:0);
 unsigned long long duration=done-start,old=atomic_load(&max_ns);while(duration>old&&!atomic_compare_exchange_weak(&max_ns,&old,duration)){}
 atomic_fetch_add(&calls,1);
 fprintf(stderr,"SINK_WRITE kind=%s fd=%d bytes=%zd cumulative=%llu start_ns=%llu done_ns=%llu duration_ns=%llu stage=write_return\n",kind,fd,n,total,start,done,duration);
}
ssize_t write(int fd,const void *buf,size_t size){
 static ssize_t(*real)(int,const void*,size_t);if(!real)real=dlsym(RTLD_NEXT,"write");
 int saved=errno,trace=selected(fd);errno=saved;unsigned long long start=trace?now_ns():0;
 ssize_t n=real(fd,buf,size);int result_errno=errno;if(trace)record(fd,n,start,now_ns(),"write");errno=result_errno;return n;
}
ssize_t writev(int fd,const struct iovec *iov,int count){
 static ssize_t(*real)(int,const struct iovec*,int);if(!real)real=dlsym(RTLD_NEXT,"writev");
 int saved=errno,trace=selected(fd);errno=saved;unsigned long long start=trace?now_ns():0;
 ssize_t n=real(fd,iov,count);int result_errno=errno;if(trace)record(fd,n,start,now_ns(),"writev");errno=result_errno;return n;
}
__attribute__((destructor))static void summary(void){fprintf(stderr,"SINK_SUMMARY calls=%llu bytes=%llu max_write_ns=%llu\n",atomic_load(&calls),atomic_load(&bytes_total),atomic_load(&max_ns));}
