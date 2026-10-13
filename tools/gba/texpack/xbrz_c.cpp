// C entry point for calling xBRZ from Python (ctypes) in upscalers.py.
// xBRZ itself (GPL-3) is not in this repository: build this file together
// with xbrz.cpp from https://sourceforge.net/projects/xbrz/ (README.md).
#include "xbrz.h"

extern "C" __declspec(dllexport) void xbrz_scale_argb(int factor, const uint32_t *src, uint32_t *dst, int w, int h)
{
    xbrz::scale(factor, src, dst, w, h, xbrz::ColorFormat::argb);
}
