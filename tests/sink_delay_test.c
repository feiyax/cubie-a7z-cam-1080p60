/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/uio.h>
#include <stdatomic.h>
/* TEST ONLY. Never loaded by the production launcher. One selected filesink
 * writev is delayed650ms after49 successful selected calls. */
static atomic_uint calls;
ssize_t writev(int fd,const struct iovec *iov,int count){
 static ssize_t(*real)(int,const struct iovec*,int);if(!real)real=dlsym(RTLD_NEXT,"writev");
 int saved=errno;const char *enable=getenv("A733_SINK_TEST_DELAY_MS"),*want=getenv("A733_SINK_TRACE_PATH");
 if(enable && !strcmp(enable,"650") && want){
  char link[64],path[4096];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);ssize_t n=readlink(link,path,sizeof(path)-1);
  if(n>=0){path[n]=0;if(!strcmp(path,want) && atomic_fetch_add(&calls,1)==49){
   struct timespec delay={.tv_sec=0,.tv_nsec=650000000};
   fprintf(stderr,"SINK_TEST_DELAY begin fd=%d milliseconds=650 selected_call=50\n",fd);
   while(nanosleep(&delay,&delay)<0 && errno==EINTR){}
   fprintf(stderr,"SINK_TEST_DELAY end fd=%d\n",fd);
  }}
 }
 errno=saved;return real(fd,iov,count);
}
