/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mock_omx.h"
static char order[64];
static unsigned used;
static OMX_U32 last_bytes, last_flags;
static OMX_ERRORTYPE event(OMX_HANDLETYPE h, OMX_PTR app, OMX_EVENTTYPE e, OMX_U32 p,
                           OMX_U32 flags, OMX_PTR data)
{
    (void)h;
    (void)app;
    (void)p;
    (void)data;
    if (e == OMX_EventBufferFlag) {
        order[used++] = 'E';
        last_flags = flags;
    }
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE empty(OMX_HANDLETYPE h, OMX_PTR app, OMX_BUFFERHEADERTYPE *b)
{
    (void)h;
    (void)app;
    (void)b;
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE fill(OMX_HANDLETYPE h, OMX_PTR app, OMX_BUFFERHEADERTYPE *b)
{
    (void)h;
    (void)app;
    order[used++] = 'F';
    last_bytes = b->nFilledLen;
    return OMX_ErrorNone;
}

static OMX_CALLBACKTYPE cb = {event, empty, fill};
static void init(struct fake *f, const char *name)
{
    assert(AwOmxComponentInit(f, (char *)name) == OMX_ErrorNone);
    assert(AwOmxComponentSetCallbacks(f, &cb, NULL) == OMX_ErrorNone);
    used = 0;
    memset(order, 0, sizeof(order));
}

static void eos(struct fake *f, OMX_U32 port, OMX_U32 flags, OMX_PTR data)
{
    assert(f->callbacks.EventHandler(f, f->app, OMX_EventBufferFlag, port, flags,
                                     data) == OMX_ErrorNone);
}

static void output(struct fake *f, OMX_U32 bytes, OMX_U32 flags)
{
    OMX_BUFFERHEADERTYPE b = {0};
    b.nFilledLen = bytes;
    b.nFlags = flags;
    b.nOutputPortIndex = 1;
    assert(f->callbacks.FillBufferDone(f, f->app, &b) == OMX_ErrorNone);
    assert(b.nFilledLen == bytes && b.nFlags == flags);
}

int main(void)
{
    struct fake f;
    const char *avc = "OMX.allwinner.video.encoder.avc";
    init(&f, avc);
    eos(&f, 1, OMX_BUFFERFLAG_EOS, NULL);
    assert(used == 0);
    output(&f, 37, OMX_BUFFERFLAG_ENDOFFRAME);
    assert(used == 1 && last_bytes == 37);
    output(&f, 0, OMX_BUFFERFLAG_EOS);
    assert(!strcmp(order, "FFE") && last_flags == OMX_BUFFERFLAG_EOS);
    /* A flush must discard pending EOS from the old generation. */
    assert(f.component.SendCommand(&f, OMX_CommandFlush, 1, NULL) == OMX_ErrorNone);
    used = 0;
    memset(order, 0, sizeof(order));
    eos(&f, 1, OMX_BUFFERFLAG_EOS, NULL);
    assert(used == 0);
    assert(f.component.SendCommand(&f, OMX_CommandFlush, 1, NULL) == OMX_ErrorNone);
    output(&f, 0, OMX_BUFFERFLAG_EOS);
    assert(!strcmp(order, "F"));
    assert(f.component.ComponentDeInit(&f) == OMX_ErrorNone);
    OMX_BUFFERHEADERTYPE b = {0};
    assert(f.callbacks.FillBufferDone(&f, f.app, &b) == OMX_ErrorInvalidState);
    /* Reuse of the same handle gets a fresh context. EOS may carry payload. */
    init(&f, avc);
    eos(&f, 1, OMX_BUFFERFLAG_EOS, NULL);
    output(&f, 99, OMX_BUFFERFLAG_EOS);
    assert(!strcmp(order, "FE") && last_bytes == 99);
    assert(f.component.SendCommand(&f, OMX_CommandStateSet, OMX_StateIdle, NULL) ==
           OMX_ErrorNone);
    used = 0;
    memset(order, 0, sizeof(order));
    eos(&f, 0, OMX_BUFFERFLAG_EOS, NULL);
    eos(&f, 1, OMX_BUFFERFLAG_ENDOFFRAME, NULL);
    eos(&f, 1, OMX_BUFFERFLAG_EOS | OMX_BUFFERFLAG_ENDOFFRAME, NULL);
    eos(&f, 1, OMX_BUFFERFLAG_EOS, &f);
    assert(!strcmp(order, "EEEE"));
    assert(f.component.ComponentDeInit(&f) == OMX_ErrorNone);
    init(&f, "OMX.allwinner.video.decoder.avc");
    eos(&f, 1, OMX_BUFFERFLAG_EOS, NULL);
    assert(!strcmp(order, "E"));
    puts("PASS: final payload before EOS; flush reset; EOS payload; unrelated events; "
         "handle reuse; late callback guard; decoder unchanged");
    return 0;
}
