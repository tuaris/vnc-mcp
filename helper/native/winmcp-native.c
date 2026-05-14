/*
 * winmcp-native.c — WinMCP native DLL scaffold
 *
 * Stub implementations for DXGI screenshot and WinRT OCR.
 * These will be replaced with real implementations in Phase 3 (DXGI)
 * and Phase 5 (OCR) when built with MSVC/clang-cl on Windows.
 *
 * For now this compiles as a valid DLL with correct exports,
 * allowing the agent's LoadLibrary/GetProcAddress integration to
 * be tested end-to-end.
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#define WINMCP_NATIVE_EXPORTS
#include "winmcp-native.h"

#include <stdio.h>
#include <string.h>

#define DLL_VERSION "0.2.0"

WMCP_API int wmcp_version(char *out, int out_max)
{
    const char *json = "{\"version\":\"" DLL_VERSION "\","
                       "\"capabilities\":[]}";
    int len = (int)strlen(json);
    if (len + 1 > out_max)
        return WMCP_ERR_BUFFER;
    memcpy(out, json, len + 1);
    return WMCP_OK;
}

WMCP_API int wmcp_screenshot(int x, int y, int w, int h,
                             unsigned char *jpeg_out, int jpeg_max,
                             int *jpeg_len, int quality)
{
    (void)x; (void)y; (void)w; (void)h;
    (void)jpeg_out; (void)jpeg_max; (void)jpeg_len; (void)quality;
    /* TODO: Phase 3 — DXGI Desktop Duplication */
    return WMCP_ERR_NOT_IMPL;
}

WMCP_API int wmcp_ocr_region(int x, int y, int w, int h,
                             const char *lang,
                             char *text_out, int text_max)
{
    (void)x; (void)y; (void)w; (void)h;
    (void)lang; (void)text_out; (void)text_max;
    /* TODO: Phase 5 — Windows.Media.Ocr via C++/WinRT */
    return WMCP_ERR_NOT_IMPL;
}
