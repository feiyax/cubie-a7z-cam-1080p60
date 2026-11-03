/* SPDX-License-Identifier: MIT */
#include "OMX_Component.h"
struct fake {OMX_COMPONENTTYPE component;OMX_CALLBACKTYPE callbacks;OMX_PTR app;};
OMX_ERRORTYPE AwOmxComponentInit(OMX_HANDLETYPE,OMX_STRING);
OMX_ERRORTYPE AwOmxComponentSetCallbacks(OMX_HANDLETYPE,OMX_CALLBACKTYPE*,OMX_PTR);
