#include "StreamCudaDecoder.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>


StreamCudaDecoder::StreamCudaDecoder(void): m_videoParser(NULL), m_videoDecoder(NULL),
    m_ctxLock(NULL), m_decodedFrames(0), m_bFinish(false)
{
}

StreamCudaDecoder::~StreamCudaDecoder(void)
{
	if(m_videoDecoder) cuvidDestroyDecoder(m_videoDecoder);
    if(m_videoParser)  cuvidDestroyVideoParser(m_videoParser);
	if(m_Input) destroyVideoSource(m_Input);
}

void StreamCudaDecoder::InitVideoDecoder( CUvideoctxlock ctxLock, FrameQueue* pFrameQueue, int targetWidth, int targetHeight)
{
	assert(ctxLock);
    assert(pFrameQueue);

	m_pFrameQueue = pFrameQueue;

	CUresult oResult;
    m_ctxLock = ctxLock;

	m_Input = freopen(NULL, "rb", stdin);
    if (m_Input == NULL)
    {
        fprintf(stderr, "StreamCudaDecoder::InitVideoDecoder Failed to open stdin\n");
        exit(EXIT_FAILURE);
    }

	//init video decoder
    CUVIDEOFORMAT oFormat;
    cuvidGetSourceVideoFormat(m_videoSource, &oFormat, 0);

    if (oFormat.codec != cudaVideoCodec_H264) {
        fprintf(stderr, "The sample only supports H264 input video!\n");
        exit(-1);
    }

    if (oFormat.chroma_format != cudaVideoChromaFormat_420) {
        fprintf(stderr, "The sample only supports 4:2:0 chroma!\n");
        exit(-1);
    }

    CUVIDDECODECREATEINFO oVideoDecodeCreateInfo;
    memset(&oVideoDecodeCreateInfo, 0, sizeof(CUVIDDECODECREATEINFO));
    oVideoDecodeCreateInfo.CodecType = oFormat.codec;
    oVideoDecodeCreateInfo.ulWidth   = oFormat.coded_width;
    oVideoDecodeCreateInfo.ulHeight  = oFormat.coded_height;
    oVideoDecodeCreateInfo.ulNumDecodeSurfaces = FrameQueue::cnMaximumSize;

    // Limit decode memory to 24MB (16M pixels at 4:2:0 = 24M bytes)
    // Keep atleast 6 DecodeSurfaces
    while (oVideoDecodeCreateInfo.ulNumDecodeSurfaces > 6 && 
        oVideoDecodeCreateInfo.ulNumDecodeSurfaces * oFormat.coded_width * oFormat.coded_height > 16 * 1024 * 1024)
    {
        oVideoDecodeCreateInfo.ulNumDecodeSurfaces--;
    }

    oVideoDecodeCreateInfo.ChromaFormat = oFormat.chroma_format;
    oVideoDecodeCreateInfo.OutputFormat = cudaVideoSurfaceFormat_NV12;
    oVideoDecodeCreateInfo.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;

    if (targetWidth <= 0 || targetHeight <= 0) {
        oVideoDecodeCreateInfo.ulTargetWidth  = oFormat.display_area.right - oFormat.display_area.left;
        oVideoDecodeCreateInfo.ulTargetHeight = oFormat.display_area.bottom - oFormat.display_area.top;
    }
    else {
        oVideoDecodeCreateInfo.ulTargetWidth  = targetWidth;
        oVideoDecodeCreateInfo.ulTargetHeight = targetHeight;
    }
    oVideoDecodeCreateInfo.display_area.left   = 0;
    oVideoDecodeCreateInfo.display_area.right  = oVideoDecodeCreateInfo.ulTargetWidth;
    oVideoDecodeCreateInfo.display_area.top    = 0;
    oVideoDecodeCreateInfo.display_area.bottom = oVideoDecodeCreateInfo.ulTargetHeight;

    oVideoDecodeCreateInfo.ulNumOutputSurfaces = 2;
    oVideoDecodeCreateInfo.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    oVideoDecodeCreateInfo.vidLock = m_ctxLock;

    oResult = cuvidCreateDecoder(&m_videoDecoder, &oVideoDecodeCreateInfo);
    if (oResult != CUDA_SUCCESS) {
        fprintf(stderr, "cuvidCreateDecoder() failed, error code: %d\n", oResult);
        exit(-1);
    }

    m_oVideoDecodeCreateInfo = oVideoDecodeCreateInfo;

    //init video parser
    CUVIDPARSERPARAMS oVideoParserParameters;
    memset(&oVideoParserParameters, 0, sizeof(CUVIDPARSERPARAMS));
    oVideoParserParameters.CodecType = oVideoDecodeCreateInfo.CodecType;
    oVideoParserParameters.ulMaxNumDecodeSurfaces = oVideoDecodeCreateInfo.ulNumDecodeSurfaces;
    oVideoParserParameters.ulMaxDisplayDelay = 1;
    oVideoParserParameters.pUserData = this;
    oVideoParserParameters.pfnSequenceCallback = HandleVideoSequence;
    oVideoParserParameters.pfnDecodePicture = HandlePictureDecode;
    oVideoParserParameters.pfnDisplayPicture = HandlePictureDisplay;

    oResult = cuvidCreateVideoParser(&m_videoParser, &oVideoParserParameters);
    if (oResult != CUDA_SUCCESS) {
        fprintf(stderr, "cuvidCreateVideoParser failed, error code: %d\n", oResult);
        exit(-1);
    }


}


void StreamCudaDecoder::destroyVideoSource(HANDLE hFileHandle)
{
	fclose((FILE *)hFileHandle);
}

