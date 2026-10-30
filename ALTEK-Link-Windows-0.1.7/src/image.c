#include "link.h"
#include <wincodec.h>
int image_load(const WCHAR *path, unsigned char **rgb, int *w, int *h)
{
    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *d = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT width = 0, height = 0;
    HRESULT hr;
    IWICStream *rs = NULL;
    int ok = 0;
    unsigned char *b = NULL;
    *rgb = NULL;
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&f);
    if (SUCCEEDED(hr) && path[0] == L'#')
    {
        HRSRC resource = FindResourceW(app.instance, MAKEINTRESOURCEW(_wtoi(path + 1)), RT_RCDATA);
        HGLOBAL handle = resource ? LoadResource(app.instance, resource) : NULL;
        if (!handle)
        {
            hr = E_FAIL;
        }
        else
        {
            hr = IWICImagingFactory_CreateStream(f, &rs);
            if (SUCCEEDED(hr))
            {
                hr = IWICStream_InitializeFromMemory(rs, (BYTE *)LockResource(handle),
                                                     SizeofResource(app.instance, resource));
            }
            if (SUCCEEDED(hr))
            {
                hr = IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)rs, NULL,
                                                                WICDecodeMetadataCacheOnLoad, &d);
            }
        }
    }
    else if (SUCCEEDED(hr))
    {
        hr = IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnLoad, &d);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapDecoder_GetFrame(d, 0, &frame);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapFrameDecode_GetSize(frame, &width, &height);
    }
    if (SUCCEEDED(hr) && width && height && width <= 8192 && height <= 8192 &&
        width * height <= 24000000)
    {
        hr = IWICImagingFactory_CreateFormatConverter(f, &conv);
        if (SUCCEEDED(hr))
        {
            hr = IWICFormatConverter_Initialize(
                conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom);
        }
        b = (unsigned char *)malloc(width * height * 4);
        if (b && SUCCEEDED(hr))
        {
            hr = IWICFormatConverter_CopyPixels(conv, NULL, width * 4, width * height * 4, b);
        }
        else
        {
            hr = E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr))
        {
            *rgb = b;
            *w = width;
            *h = height;
            ok = 1;
            b = NULL;
        }
    }
    free(b);
    if (conv)
    {
        IWICFormatConverter_Release(conv);
    }
    if (frame)
    {
        IWICBitmapFrameDecode_Release(frame);
    }
    if (d)
    {
        IWICBitmapDecoder_Release(d);
    }
    if (rs)
    {
        IWICStream_Release(rs);
    }
    if (f)
    {
        IWICImagingFactory_Release(f);
    }
    return ok;
}
int image_avatar(const WCHAR *source, const WCHAR *dest)
{
    IWICImagingFactory *f = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IWICStream *stream = NULL;
    IPropertyBag2 *props = NULL;
    unsigned char *rgb = NULL, *thumb = NULL;
    int w, h, x, y, side, left, top, ok = 0;
    HRESULT hr;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    PROPBAG2 prop;
    VARIANT quality;
    if (!image_load(source, &rgb, &w, &h))
    {
        return 0;
    }
    side = w < h ? w : h;
    left = (w - side) / 2;
    top = (h - side) / 2;
    thumb = (unsigned char *)malloc(256 * 256 * 3);
    if (!thumb)
    {
        free(rgb);
        return 0;
    }
    for (y = 0; y < 256; y++)
    {
        for (x = 0; x < 256; x++)
        {
            memcpy(thumb + 3 * (y * 256 + x),
                   rgb + 4 * ((top + y * side / 256) * w + left + x * side / 256), 3);
        }
    }
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&f);
    if (SUCCEEDED(hr))
    {
        hr = IWICImagingFactory_CreateStream(f, &stream);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICStream_InitializeFromFilename(stream, dest, GENERIC_WRITE);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatJpeg, NULL, &encoder);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &props);
    }
    memset(&prop, 0, sizeof(prop));
    prop.pstrName = L"ImageQuality";
    VariantInit(&quality);
    quality.vt = VT_R4;
    quality.fltVal = 0.75f;
    if (SUCCEEDED(hr))
    {
        hr = IPropertyBag2_Write(props, 1, &prop, &quality);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapFrameEncode_Initialize(frame, props);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapFrameEncode_SetSize(frame, 256, 256);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    }
    if (SUCCEEDED(hr) && IsEqualGUID(&format, &GUID_WICPixelFormat24bppBGR))
    {
        hr = IWICBitmapFrameEncode_WritePixels(frame, 256, 256 * 3, 256 * 256 * 3, thumb);
    }
    else
    {
        hr = E_FAIL;
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapFrameEncode_Commit(frame);
    }
    if (SUCCEEDED(hr))
    {
        hr = IWICBitmapEncoder_Commit(encoder);
    }
    ok = SUCCEEDED(hr);
    if (props)
    {
        IPropertyBag2_Release(props);
    }
    if (frame)
    {
        IWICBitmapFrameEncode_Release(frame);
    }
    if (encoder)
    {
        IWICBitmapEncoder_Release(encoder);
    }
    if (stream)
    {
        IWICStream_Release(stream);
    }
    if (f)
    {
        IWICImagingFactory_Release(f);
    }
    free(rgb);
    free(thumb);
    return ok;
}
