#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#include "../third_party/qrcodegen/qrcodegen.c"

extern "C" int webbridge_make_qr(const char *text, unsigned char *modules, int capacity)
{
    unsigned char temp[qrcodegen_BUFFER_LEN_MAX];
    unsigned char qr[qrcodegen_BUFFER_LEN_MAX];
    int x, y, size;
    if (!qrcodegen_encodeText(text, temp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true))
        return 0;
    size = qrcodegen_getSize(qr);
    if (size * size > capacity)
        return 0;
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
            modules[y * size + x] = qrcodegen_getModule(qr, x, y) ? 1 : 0;
    return size;
}
