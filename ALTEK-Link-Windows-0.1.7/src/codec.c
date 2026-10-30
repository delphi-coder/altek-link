#include "link.h"
#if defined(_MSC_VER) && _MSC_VER < 1800
/* OpenH264's C ABI uses one-byte bool. VS2010 has no stdbool.h. */
#define __bool_true_false_are_defined 1
#endif
#include "codec_api.h"
struct codec
{
    HMODULE dll;
    ISVCEncoder *enc;
    ISVCDecoder *dec;
    void (*destroy_enc)(ISVCEncoder *);
    void (*destroy_dec)(ISVCDecoder *);
    int w, h;
    unsigned char *output, *rgb;
    int capacity;
    uint64_t frame;
};
static HMODULE load_codec(void)
{
    WCHAR path[MAX_PATH];
    HMODULE dll;
    void (*version)(OpenH264Version *);
    OpenH264Version v;
    path_join(path, app.exe_dir, L"openh264.dll");
    dll = LoadLibraryW(path);
    if (!dll)
    {
        return NULL;
    }
    version = (void (*)(OpenH264Version *))GetProcAddress(dll, "WelsGetCodecVersionEx");
    memset(&v, 0, sizeof(v));
    if (version)
    {
        version(&v);
    }
    if (!version || v.uMajor != 2 || v.uMinor != 4 || v.uRevision != 1)
    {
        FreeLibrary(dll);
        return NULL;
    }
    return dll;
}
int codec_available(void)
{
    HMODULE dll = load_codec();
    if (!dll)
    {
        return 0;
    }
    FreeLibrary(dll);
    return 1;
}
struct codec *codec_open(int encode, int w, int h, int fps, int bitrate,
                         const struct codec_encoder_config *config)
{
    struct codec *c = (struct codec *)calloc(1, sizeof(*c));
    int (*create_enc)(ISVCEncoder **);
    long (*create_dec)(ISVCDecoder **);
    if (!c)
    {
        return NULL;
    }
    c->dll = load_codec();
    if (!c->dll)
    {
        goto fail;
    }
    create_enc = (int (*)(ISVCEncoder **))GetProcAddress(c->dll, "WelsCreateSVCEncoder");
    create_dec = (long (*)(ISVCDecoder **))GetProcAddress(c->dll, "WelsCreateDecoder");
    c->destroy_enc = (void (*)(ISVCEncoder *))GetProcAddress(c->dll, "WelsDestroySVCEncoder");
    c->destroy_dec = (void (*)(ISVCDecoder *))GetProcAddress(c->dll, "WelsDestroyDecoder");
    if (!create_enc || !create_dec || !c->destroy_enc || !c->destroy_dec)
    {
        goto fail;
    }
    if (encode)
    {
        SEncParamExt p;
        int format = videoFormatI420;
        if (create_enc(&c->enc) || !c->enc)
        {
            goto fail;
        }
        memset(&p, 0, sizeof(p));
        if ((*c->enc)->GetDefaultParams(c->enc, &p))
        {
            goto fail;
        }
        p.iUsageType = config && config->screen_content ? SCREEN_CONTENT_REAL_TIME
                                                       : CAMERA_VIDEO_REAL_TIME;
        p.iPicWidth = w;
        p.iPicHeight = h;
        p.iTargetBitrate = bitrate;
        p.iRCMode = config && config->quality_mode ? RC_QUALITY_MODE : RC_BITRATE_MODE;
        p.fMaxFrameRate = (float)fps;
        p.iTemporalLayerNum = 1;
        p.iSpatialLayerNum = 1;
        p.uiIntraPeriod = config && config->intra_period > 0
                              ? (unsigned int)config->intra_period
                              : (unsigned int)fps;
        p.iMultipleThreadIdc = config ? config->threads : 1;
        p.bEnableFrameSkip = !config || config->frame_skip ? 1 : 0;
        p.sSpatialLayers[0].iVideoWidth = w;
        p.sSpatialLayers[0].iVideoHeight = h;
        p.sSpatialLayers[0].fFrameRate = (float)fps;
        p.sSpatialLayers[0].iSpatialBitrate = bitrate;
        p.sSpatialLayers[0].iMaxSpatialBitrate = bitrate;
        p.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
        p.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
        if ((*c->enc)->InitializeExt(c->enc, &p) ||
            (*c->enc)->SetOption(c->enc, ENCODER_OPTION_DATAFORMAT, &format))
        {
            goto fail;
        }
        c->w = w;
        c->h = h;
    }
    else
    {
        SDecodingParam p;
        memset(&p, 0, sizeof(p));
        p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        p.eEcActiveIdc = ERROR_CON_SLICE_COPY;
        if (create_dec(&c->dec) || !c->dec || (*c->dec)->Initialize(c->dec, &p))
        {
            goto fail;
        }
    }
    return c;
fail:
    codec_close(c);
    return NULL;
}
void codec_close(struct codec *c)
{
    if (!c)
    {
        return;
    }
    if (c->enc)
    {
        (*c->enc)->Uninitialize(c->enc);
        c->destroy_enc(c->enc);
    }
    if (c->dec)
    {
        (*c->dec)->Uninitialize(c->dec);
        c->destroy_dec(c->dec);
    }
    if (c->dll)
    {
        FreeLibrary(c->dll);
    }
    free(c->output);
    free(c->rgb);
    free(c);
}
static int collect(struct codec *c, SFrameBSInfo *info, unsigned char **bytes, int *len)
{
    int i, j, n, total = 0, pos = 0;
    unsigned char *b;
    for (i = 0; i < info->iLayerNum; i++)
    {
        for (j = 0; j < info->sLayerInfo[i].iNalCount; j++)
        {
            total += info->sLayerInfo[i].pNalLengthInByte[j];
        }
    }
    if (total <= 0 || total > MAX_MEDIA - 13)
    {
        return 0;
    }
    if (total > c->capacity)
    {
        b = (unsigned char *)realloc(c->output, total);
        if (!b)
        {
            return 0;
        }
        c->output = b;
        c->capacity = total;
    }
    for (i = 0; i < info->iLayerNum; i++)
    {
        n = 0;
        for (j = 0; j < info->sLayerInfo[i].iNalCount; j++)
        {
            n += info->sLayerInfo[i].pNalLengthInByte[j];
        }
        memcpy(c->output + pos, info->sLayerInfo[i].pBsBuf, n);
        pos += n;
    }
    *bytes = c->output;
    *len = total;
    return 1;
}
int codec_headers(struct codec *c, unsigned char **bytes, int *len)
{
    SFrameBSInfo info;
    memset(&info, 0, sizeof(info));
    if ((*c->enc)->EncodeParameterSets(c->enc, &info))
    {
        return 0;
    }
    return collect(c, &info, bytes, len);
}
int codec_encode(struct codec *c, const unsigned char *i420, unsigned char **bytes, int *len,
                 int *key)
{
    SSourcePicture p;
    SFrameBSInfo info;
    memset(&p, 0, sizeof(p));
    memset(&info, 0, sizeof(info));
    p.iColorFormat = videoFormatI420;
    p.iPicWidth = c->w;
    p.iPicHeight = c->h;
    p.iStride[0] = c->w;
    p.iStride[1] = p.iStride[2] = c->w / 2;
    p.pData[0] = (unsigned char *)i420;
    p.pData[1] = p.pData[0] + c->w * c->h;
    p.pData[2] = p.pData[1] + c->w * c->h / 4;
    p.uiTimeStamp = GetTickCount64();
    if ((*c->enc)->EncodeFrame(c->enc, &p, &info))
    {
        return 0;
    }
    *key = info.eFrameType == videoFrameTypeIDR;
    return collect(c, &info, bytes, len);
}
static unsigned char clip(int x)
{
    return (unsigned char)(x < 0 ? 0 : x > 255 ? 255 : x);
}
int codec_decode(struct codec *c, const unsigned char *bytes, int len, unsigned char **rgb, int *w,
                 int *h)
{
    unsigned char *planes[3], *b;
    SBufferInfo info;
    int x, y, Y, U, V, W, H, sy, suv;
    DECODING_STATE state;
    memset(&info, 0, sizeof(info));
    memset(planes, 0, sizeof(planes));
    state = (*c->dec)->DecodeFrameNoDelay(c->dec, bytes, len, planes, &info);
    if (state & dsOutOfMemory)
    {
        return 0;
    }
    if (!info.iBufferStatus)
    {
        return 0;
    }
    W = info.UsrData.sSystemBuffer.iWidth;
    H = info.UsrData.sSystemBuffer.iHeight;
    sy = info.UsrData.sSystemBuffer.iStride[0];
    suv = info.UsrData.sSystemBuffer.iStride[1];
    if (W < 1 || H < 1 || W > 1920 || H > 1920)
    {
        return 0;
    }
    b = (unsigned char *)realloc(c->rgb, W * H * 4);
    if (!b)
    {
        return 0;
    }
    c->rgb = b;
    for (y = 0; y < H; y++)
    {
        for (x = 0; x < W; x++)
        {
            Y = planes[0][y * sy + x] - 16;
            U = planes[1][(y / 2) * suv + x / 2] - 128;
            V = planes[2][(y / 2) * suv + x / 2] - 128;
            b[4 * (y * W + x)] = clip((298 * Y + 516 * U + 128) >> 8);
            b[4 * (y * W + x) + 1] = clip((298 * Y - 100 * U - 208 * V + 128) >> 8);
            b[4 * (y * W + x) + 2] = clip((298 * Y + 409 * V + 128) >> 8);
            b[4 * (y * W + x) + 3] = 255;
        }
    }
    *rgb = b;
    *w = W;
    *h = H;
    return 1;
}
