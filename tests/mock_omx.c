/* SPDX-License-Identifier: MIT */
#include <string.h>
#include "mock_omx.h"
static OMX_ERRORTYPE send_cmd(OMX_HANDLETYPE h,OMX_COMMANDTYPE cmd,OMX_U32 p,OMX_PTR data){struct fake *f=h;(void)data;return f->callbacks.EventHandler(h,f->app,OMX_EventCmdComplete,cmd,p,NULL);}
static OMX_ERRORTYPE deinit(OMX_HANDLETYPE h){(void)h;return OMX_ErrorNone;}
OMX_ERRORTYPE AwOmxComponentInit(OMX_HANDLETYPE h,OMX_STRING name){struct fake *f=h;(void)name;memset(f,0,sizeof(*f));f->component.SendCommand=send_cmd;f->component.ComponentDeInit=deinit;return OMX_ErrorNone;}
OMX_ERRORTYPE AwOmxComponentSetCallbacks(OMX_HANDLETYPE h,OMX_CALLBACKTYPE *c,OMX_PTR app){struct fake *f=h;f->callbacks=*c;f->app=app;return OMX_ErrorNone;}
