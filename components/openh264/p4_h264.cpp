#include "p4_h264.h"
#include "wels/codec_api.h"

int p4_h264_create(p4_h264_decoder *handle)
{
    ISVCDecoder *decoder;
    int result=WelsCreateDecoder(&decoder);
    if(result)return result;
    SDecodingParam parameters={};
    parameters.eEcActiveIdc=ERROR_CON_DISABLE;
    parameters.sVideoProperty.size=sizeof(parameters.sVideoProperty);
    parameters.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;
    result=decoder->Initialize(&parameters);
    if(result){WelsDestroyDecoder(decoder);return result;}
    // The caller runs decode synchronously on the camera worker.
    int threads=0;
    result=decoder->SetOption(DECODER_OPTION_NUM_OF_THREADS,&threads);
    if(result){decoder->Uninitialize();WelsDestroyDecoder(decoder);return result;}
    *handle=decoder;
    return 0;
}
int p4_h264_decode(p4_h264_decoder handle,const uint8_t *data,size_t size,p4_h264_frame *frame)
{
    auto decoder=static_cast<ISVCDecoder *>(handle);
    unsigned char *planes[3];
    SBufferInfo info={};
    int result=decoder->DecodeFrameNoDelay(data,static_cast<int>(size),planes,&info);
    // High/Main retain a decoded picture in the display-order queue. Each input
    // is a complete independent IDR, so drain that picture immediately.
    if(result==0 && info.iBufferStatus==0)
        result=decoder->FlushFrame(planes,&info);
    frame->ready=info.iBufferStatus;
    if(info.iBufferStatus==1){
        for(int i=0;i<3;i++)frame->plane[i]=planes[i];
        frame->width=info.UsrData.sSystemBuffer.iWidth;
        frame->height=info.UsrData.sSystemBuffer.iHeight;
        frame->stride_y=info.UsrData.sSystemBuffer.iStride[0];
        frame->stride_uv=info.UsrData.sSystemBuffer.iStride[1];
    }
    return result;
}
void p4_h264_destroy(p4_h264_decoder handle)
{
    auto decoder=static_cast<ISVCDecoder *>(handle);
    decoder->Uninitialize();
    WelsDestroyDecoder(decoder);
}
