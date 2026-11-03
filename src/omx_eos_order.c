/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "OMX_Component.h"
/* Compatibility adapter for the verified Allwinner AVC component only.
 * The vendor signals EventBufferFlag(EOS) when it consumes INPUT EOS, before
 * its final asynchronous output completes. Its output EOS buffer is correctly
 * ordered. Delay only that event until the real EOS output callback returns.
 */
struct pending_event { OMX_U32 port, flags; OMX_PTR data; struct pending_event *next; };
struct context {
 OMX_HANDLETYPE handle; OMX_CALLBACKTYPE original, wrapped; OMX_PTR app;
 OMX_ERRORTYPE (*send)(OMX_HANDLETYPE,OMX_COMMANDTYPE,OMX_U32,OMX_PTR);
 OMX_ERRORTYPE (*deinit)(OMX_HANDLETYPE);
 pthread_mutex_t lock; unsigned long epoch, deferred, delivered;
 int output_eos, installed; atomic_int retired;
 struct pending_event *head, *tail;
 struct context *next;
};
static pthread_mutex_t contexts_lock=PTHREAD_MUTEX_INITIALIZER;
static struct context *contexts;
static void *vendor(const char *name) {
 void *h=dlopen("libOmxCore.so",RTLD_NOW|RTLD_NOLOAD);
 void *p=h?dlsym(h,name):NULL;
 if(!p){fprintf(stderr,"OMX_EOS_FIX unresolved %s\n",name);abort();}
 return p;
}
static struct context *lookup(OMX_HANDLETYPE h) {
 pthread_mutex_lock(&contexts_lock);struct context *c=contexts;
 while(c && (c->handle!=h || atomic_load(&c->retired)))c=c->next;
 pthread_mutex_unlock(&contexts_lock);return c;
}
static void reset_locked(struct context *c) {
 struct pending_event *e=c->head;
 while(e){struct pending_event *n=e->next;free(e);e=n;}
 c->head=c->tail=NULL;c->output_eos=0;c->epoch++;
}
static OMX_ERRORTYPE event_cb(OMX_HANDLETYPE h, OMX_PTR app, OMX_EVENTTYPE event, OMX_U32 d1, OMX_U32 d2, OMX_PTR data) {
 struct context *c=app;
 if(atomic_load(&c->retired))return OMX_ErrorInvalidState;
 pthread_mutex_lock(&c->lock);
 if(event==OMX_EventCmdComplete && (d1==OMX_CommandFlush ||
    (d1==OMX_CommandStateSet && (d2==OMX_StateIdle || d2==OMX_StateLoaded)))) reset_locked(c);
 if(event==OMX_EventBufferFlag && d1==1 && d2==OMX_BUFFERFLAG_EOS && data==NULL && !c->output_eos) {
  struct pending_event *e=calloc(1,sizeof(*e));
  if(!e){pthread_mutex_unlock(&c->lock);return OMX_ErrorInsufficientResources;}
  e->port=d1;e->flags=d2;e->data=data;
  if(c->tail)c->tail->next=e;else c->head=e;c->tail=e;c->deferred++;
  pthread_mutex_unlock(&c->lock);
  fprintf(stderr,"OMX_EOS_FIX deferred input-side EOS event handle=%p port=%lu flags=%lu\n",h,d1,d2);
  return OMX_ErrorNone;
 }
 pthread_mutex_unlock(&c->lock);
 return c->original.EventHandler(h,c->app,event,d1,d2,data);
}
static OMX_ERRORTYPE empty_cb(OMX_HANDLETYPE h, OMX_PTR app, OMX_BUFFERHEADERTYPE *b) {
 struct context *c=app;if(atomic_load(&c->retired))return OMX_ErrorInvalidState;return c->original.EmptyBufferDone(h,c->app,b);
}
static OMX_ERRORTYPE fill_cb(OMX_HANDLETYPE h, OMX_PTR app, OMX_BUFFERHEADERTYPE *b) {
 struct context *c=app;
 if(atomic_load(&c->retired))return OMX_ErrorInvalidState;
 /* Snapshot before the client may recycle/change the returned header. */
 int eos=(b->nOutputPortIndex==1 && (b->nFlags&OMX_BUFFERFLAG_EOS));
 pthread_mutex_lock(&c->lock);unsigned long epoch=c->epoch;pthread_mutex_unlock(&c->lock);
 OMX_ERRORTYPE rc=c->original.FillBufferDone(h,c->app,b);
 if(!eos || rc!=OMX_ErrorNone)return rc;
 pthread_mutex_lock(&c->lock);
 struct pending_event *e=NULL;
 if(c->epoch==epoch){c->output_eos=1;e=c->head;c->head=c->tail=NULL;}
 pthread_mutex_unlock(&c->lock);
 while(e){
  struct pending_event *n=e->next;
  /* Never hold an adapter mutex while calling client code. */
  pthread_mutex_lock(&c->lock);
  int current=(c->epoch==epoch && !atomic_load(&c->retired));
  pthread_mutex_unlock(&c->lock);
  if(current){
   OMX_ERRORTYPE er=c->original.EventHandler(h,c->app,OMX_EventBufferFlag,e->port,e->flags,e->data);
   if(rc==OMX_ErrorNone)rc=er;
   pthread_mutex_lock(&c->lock);c->delivered++;pthread_mutex_unlock(&c->lock);
  }
  free(e);e=n;
 }
 fprintf(stderr,"OMX_EOS_FIX output EOS delivered after payloads handle=%p\n",h);
 return rc;
}
static OMX_ERRORTYPE send_cmd(OMX_HANDLETYPE h, OMX_COMMANDTYPE cmd, OMX_U32 param, OMX_PTR data) {
 struct context *c=lookup(h);if(!c)return OMX_ErrorInvalidComponent;
 if(cmd==OMX_CommandFlush || (cmd==OMX_CommandStateSet && (param==OMX_StateIdle || param==OMX_StateLoaded))) {
  pthread_mutex_lock(&c->lock);reset_locked(c);pthread_mutex_unlock(&c->lock);
 }
 return c->send(h,cmd,param,data);
}
static OMX_ERRORTYPE deinit(OMX_HANDLETYPE h) {
 struct context *c=lookup(h);if(!c)return OMX_ErrorInvalidComponent;
 pthread_mutex_lock(&c->lock);reset_locked(c);pthread_mutex_unlock(&c->lock);
 OMX_ERRORTYPE rc=c->deinit(h);
 pthread_mutex_lock(&c->lock);reset_locked(c);
 fprintf(stderr,"OMX_EOS_FIX summary handle=%p deferred=%lu delivered=%lu\n",h,c->deferred,c->delivered);
 pthread_mutex_unlock(&c->lock);
 if(rc==OMX_ErrorNone)atomic_store(&c->retired,1);
 /* Retain context until process exit: late vendor callbacks cannot dereference
  * freed adapter memory, and reused handles receive a new context. */
 return rc;
}
OMX_ERRORTYPE AwOmxComponentInit(OMX_HANDLETYPE h, OMX_STRING name) {
 OMX_ERRORTYPE (*real)(OMX_HANDLETYPE,OMX_STRING)=vendor("AwOmxComponentInit");
 OMX_ERRORTYPE rc=real(h,name);
 if(rc!=OMX_ErrorNone || strcmp(name,"OMX.allwinner.video.encoder.avc"))return rc;
 struct context *c=calloc(1,sizeof(*c));if(!c)return OMX_ErrorInsufficientResources;
 c->handle=h;pthread_mutex_init(&c->lock,NULL);
 pthread_mutex_lock(&contexts_lock);c->next=contexts;contexts=c;pthread_mutex_unlock(&contexts_lock);
 return rc;
}
OMX_ERRORTYPE AwOmxComponentSetCallbacks(OMX_HANDLETYPE h, OMX_CALLBACKTYPE *cb, OMX_PTR app) {
 OMX_ERRORTYPE (*real)(OMX_HANDLETYPE,OMX_CALLBACKTYPE*,OMX_PTR)=vendor("AwOmxComponentSetCallbacks");
 struct context *c=lookup(h);if(!c)return real(h,cb,app);
 OMX_COMPONENTTYPE *component=h;
 c->original=*cb;c->app=app;c->wrapped=(OMX_CALLBACKTYPE){event_cb,empty_cb,fill_cb};
 if(!c->installed){c->send=component->SendCommand;c->deinit=component->ComponentDeInit;}
 OMX_ERRORTYPE rc=real(h,&c->wrapped,c);
 if(rc==OMX_ErrorNone){c->installed=1;component->SendCommand=send_cmd;component->ComponentDeInit=deinit;
  fprintf(stderr,"OMX_EOS_FIX active component=OMX.allwinner.video.encoder.avc output_port=1 handle=%p\n",h);}
 return rc;
}
