#include "StreamCudaDecoder.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>

static const char* getProfileName(int profile)
{
	switch (profile) {
	case 66:    return "Baseline";
	case 77:    return "Main";
	case 88:    return "Extended";
	case 100:   return "High";
	case 110:   return "High 10";
	case 122:   return "High 4:2:2";
	case 244:   return "High 4:4:4";
	case 44:    return "CAVLC 4:4:4";
	}

	return "Unknown Profile";
}

static int CUDAAPI HandleVideoData(void* pUserData, CUVIDSOURCEDATAPACKET* pPacket)
{
	assert(pUserData);
	StreamCudaDecoder* pDecoder = (StreamCudaDecoder*)pUserData;

	CUresult oResult = cuvidParseVideoData(pDecoder->m_videoParser, pPacket);
	if (oResult != CUDA_SUCCESS) {
		printf("error!\n");
	}

	return 1;
}

static int CUDAAPI HandleVideoSequence(void* pUserData, CUVIDEOFORMAT* pFormat)
{
	assert(pUserData);
	StreamCudaDecoder* pDecoder = (StreamCudaDecoder*)pUserData;

	if ((pFormat->codec != pDecoder->m_oVideoDecodeCreateInfo.CodecType) ||         // codec-type
		(pFormat->coded_width != pDecoder->m_oVideoDecodeCreateInfo.ulWidth) ||
		(pFormat->coded_height != pDecoder->m_oVideoDecodeCreateInfo.ulHeight) ||
		(pFormat->chroma_format != pDecoder->m_oVideoDecodeCreateInfo.ChromaFormat))
	{
		fprintf(stderr, "NvTranscoder doesn't deal with dynamic video format changing\n");
		return 0;
	}

	return 1;
}

static int CUDAAPI HandlePictureDecode(void* pUserData, CUVIDPICPARAMS* pPicParams)
{
	assert(pUserData);
	StreamCudaDecoder* pDecoder = (StreamCudaDecoder*)pUserData;
	pDecoder->m_pFrameQueue->waitUntilFrameAvailable(pPicParams->CurrPicIdx);
	assert(CUDA_SUCCESS == cuvidDecodePicture(pDecoder->m_videoDecoder, pPicParams));
	return 1;
}

static int CUDAAPI HandlePictureDisplay(void* pUserData, CUVIDPARSERDISPINFO* pPicParams)
{
	assert(pUserData);
	StreamCudaDecoder* pDecoder = (StreamCudaDecoder*)pUserData;
	pDecoder->m_pFrameQueue->enqueue(pPicParams);
	pDecoder->m_decodedFrames++;

	return 1;
}

StreamCudaDecoder::StreamCudaDecoder(void): m_Input(NULL), m_videoParser(NULL), m_videoDecoder(NULL),
                                            m_ctxLock(NULL), m_pFrameQueue(nullptr), m_decodedFrames(0), m_bFinish(false)
{
}

StreamCudaDecoder::~StreamCudaDecoder(void)
{
	if(m_videoDecoder) cuvidDestroyDecoder(m_videoDecoder);
    if(m_videoParser)  cuvidDestroyVideoParser(m_videoParser);
	if(m_Input) destroyVideoSource(m_Input);
}

void StreamCudaDecoder::InitVideoDecoder(CUVIDEOFORMAT* oFormat, CUvideoctxlock ctxLock, FrameQueue* pFrameQueue, int targetWidth, int targetHeight)
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
	if (oFormat->codec != cudaVideoCodec_H264 && oFormat->codec != cudaVideoCodec_HEVC) {
		fprintf(stderr, "The sample only supports H264/HEVC input video!\n");
		exit(-1);
	}

	if (oFormat->chroma_format != cudaVideoChromaFormat_420) {
		fprintf(stderr, "The sample only supports 4:2:0 chroma!\n");
		exit(-1);
	}

	CUVIDDECODECREATEINFO oVideoDecodeCreateInfo;
	memset(&oVideoDecodeCreateInfo, 0, sizeof(CUVIDDECODECREATEINFO));
	oVideoDecodeCreateInfo.CodecType = oFormat->codec;
	oVideoDecodeCreateInfo.ulWidth = oFormat->coded_width;
	oVideoDecodeCreateInfo.ulHeight = oFormat->coded_height;
	oVideoDecodeCreateInfo.ulNumDecodeSurfaces = 8;
	if ((oVideoDecodeCreateInfo.CodecType == cudaVideoCodec_H264) ||
		(oVideoDecodeCreateInfo.CodecType == cudaVideoCodec_H264_SVC) ||
		(oVideoDecodeCreateInfo.CodecType == cudaVideoCodec_H264_MVC))
	{
		// assume worst-case of 20 decode surfaces for H264
		oVideoDecodeCreateInfo.ulNumDecodeSurfaces = 20;
	}
	if (oVideoDecodeCreateInfo.CodecType == cudaVideoCodec_VP9)
		oVideoDecodeCreateInfo.ulNumDecodeSurfaces = 12;
	if (oVideoDecodeCreateInfo.CodecType == cudaVideoCodec_HEVC)
	{
		// ref HEVC spec: A.4.1 General tier and level limits
		int MaxLumaPS = 35651584; // currently assuming level 6.2, 8Kx4K
		int MaxDpbPicBuf = 6;
		int PicSizeInSamplesY = oVideoDecodeCreateInfo.ulWidth * oVideoDecodeCreateInfo.ulHeight;
		int MaxDpbSize;
		if (PicSizeInSamplesY <= (MaxLumaPS >> 2))
			MaxDpbSize = MaxDpbPicBuf * 4;
		else if (PicSizeInSamplesY <= (MaxLumaPS >> 1))
			MaxDpbSize = MaxDpbPicBuf * 2;
		else if (PicSizeInSamplesY <= ((3 * MaxLumaPS) >> 2))
			MaxDpbSize = (MaxDpbPicBuf * 4) / 3;
		else
			MaxDpbSize = MaxDpbPicBuf;
		MaxDpbSize = MaxDpbSize < 16 ? MaxDpbSize : 16;
		oVideoDecodeCreateInfo.ulNumDecodeSurfaces = MaxDpbSize + 4;
	}
	oVideoDecodeCreateInfo.ChromaFormat = oFormat->chroma_format;
	oVideoDecodeCreateInfo.OutputFormat = cudaVideoSurfaceFormat_NV12;
	oVideoDecodeCreateInfo.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;

	if (targetWidth <= 0 || targetHeight <= 0) {
		oVideoDecodeCreateInfo.ulTargetWidth = oFormat->display_area.right - oFormat->display_area.left;
		oVideoDecodeCreateInfo.ulTargetHeight = oFormat->display_area.bottom - oFormat->display_area.top;
	}
	else {
		oVideoDecodeCreateInfo.ulTargetWidth = targetWidth;
		oVideoDecodeCreateInfo.ulTargetHeight = targetHeight;
	}
	oVideoDecodeCreateInfo.display_area.left = 0;
	oVideoDecodeCreateInfo.display_area.right = oVideoDecodeCreateInfo.ulTargetWidth;
	oVideoDecodeCreateInfo.display_area.top = 0;
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

void StreamCudaDecoder::Start()
{
	
}

void StreamCudaDecoder::destroyVideoSource(HANDLE hFileHandle)
{
	fclose((FILE *)hFileHandle);
}

