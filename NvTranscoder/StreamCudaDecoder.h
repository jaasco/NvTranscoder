#pragma once
#ifndef _VIDEO_DECODER
#define _VIDEO_DECODER

#include "dynlink_nvcuvid.h" // <nvcuvid.h>
#include "dynlink_cuda.h"    // <cuda.h>
#include "FrameQueue.h"

class StreamCudaDecoder
{
public:
	StreamCudaDecoder(void);
	virtual ~StreamCudaDecoder(void);
	bool IsFinished()            { return m_bFinish; }
    virtual void InitVideoDecoder(CUVIDEOFORMAT* oFormat, CUvideoctxlock ctxLock, FrameQueue* pFrameQueue,
            int targetWidth = 0, int targetHeight = 0);
    virtual void Start();
    virtual void* GetDecoder()   { return m_videoDecoder; }

    HANDLE m_Input;
	CUvideoparser  m_videoParser;
    CUvideodecoder m_videoDecoder;
    CUvideoctxlock m_ctxLock;
    CUVIDDECODECREATEINFO m_oVideoDecodeCreateInfo;

    FrameQueue*    m_pFrameQueue;
    int            m_decodedFrames;

protected:
    void destroyVideoSource(HANDLE hFileHandle);

	bool m_bFinish;



};

#endif