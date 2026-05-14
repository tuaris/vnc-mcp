/*
 * winmcp-native.c — WinMCP native DLL
 *
 * Implements DXGI Desktop Duplication screenshot capture with JPEG
 * encoding via stb_image_write. OCR is stubbed pending WinRT (Phase 5).
 *
 * Cross-compiles from FreeBSD using Zig's mingw-w64 headers:
 *   zig cc -shared -target x86_64-windows-gnu -O2 native/winmcp-native.c
 *          native/stb_impl.c -ld3d11 -ldxgi -lole32
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#define WINMCP_NATIVE_EXPORTS
#include "winmcp-native.h"

#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>

#include "stb_image_write.h"

#define DLL_VERSION "0.3.0"

/* ================================================================
 * JPEG write-to-memory callback for stb_image_write
 * ================================================================ */

typedef struct {
    unsigned char *buf;
    int            max;
    int            pos;
    int            overflow;
} JpegWriteCtx;

static void jpeg_write_func(void *context, void *data, int size)
{
    JpegWriteCtx *ctx = (JpegWriteCtx *)context;
    if (ctx->overflow) return;
    if (ctx->pos + size > ctx->max) {
        ctx->overflow = 1;
        return;
    }
    memcpy(ctx->buf + ctx->pos, data, size);
    ctx->pos += size;
}

/* ================================================================
 * DXGI Desktop Duplication — capture one frame
 * ================================================================ */

static int capture_desktop(int req_x, int req_y, int req_w, int req_h,
                           unsigned char *jpeg_out, int jpeg_max,
                           int *jpeg_len, int quality)
{
    HRESULT hr;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGIDevice *dxgiDevice = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIOutput *output = NULL;
    IDXGIOutput1 *output1 = NULL;
    IDXGIOutputDuplication *dupl = NULL;
    ID3D11Texture2D *staging = NULL;
    IDXGIResource *frameRes = NULL;
    ID3D11Texture2D *frameTex = NULL;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    int result = WMCP_ERR_CAPTURE;

    /* Create D3D11 device */
    D3D_FEATURE_LEVEL featureLevel;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           0, NULL, 0, D3D11_SDK_VERSION,
                           &device, &featureLevel, &ctx);
    if (FAILED(hr)) {
        /* Try WARP (software) if hardware fails */
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
                               0, NULL, 0, D3D11_SDK_VERSION,
                               &device, &featureLevel, &ctx);
        if (FAILED(hr)) goto cleanup;
    }

    /* Get DXGI adapter and output */
    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgiDevice);
    if (FAILED(hr)) goto cleanup;

    hr = IDXGIDevice_GetAdapter(dxgiDevice, &adapter);
    if (FAILED(hr)) goto cleanup;

    hr = IDXGIAdapter_EnumOutputs(adapter, 0, &output);
    if (FAILED(hr)) goto cleanup;

    hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1);
    if (FAILED(hr)) goto cleanup;

    /* Create output duplication */
    hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)device, &dupl);
    if (FAILED(hr)) goto cleanup;

    /* Get desktop dimensions */
    DXGI_OUTDUPL_DESC duplDesc;
    IDXGIOutputDuplication_GetDesc(dupl, &duplDesc);
    int desk_w = (int)duplDesc.ModeDesc.Width;
    int desk_h = (int)duplDesc.ModeDesc.Height;

    /* Resolve capture region (0,0,0,0 = full screen) */
    if (req_w <= 0 || req_h <= 0) {
        req_x = 0; req_y = 0;
        req_w = desk_w; req_h = desk_h;
    }

    /* Clamp to desktop bounds */
    if (req_x < 0) req_x = 0;
    if (req_y < 0) req_y = 0;
    if (req_x + req_w > desk_w) req_w = desk_w - req_x;
    if (req_y + req_h > desk_h) req_h = desk_h - req_y;
    if (req_w <= 0 || req_h <= 0) goto cleanup;

    /* Acquire a frame (try a few times — first call may return empty) */
    hr = E_FAIL;
    for (int attempt = 0; attempt < 4; attempt++) {
        hr = IDXGIOutputDuplication_AcquireNextFrame(dupl, 500, &frameInfo, &frameRes);
        if (SUCCEEDED(hr)) break;
        Sleep(50);
    }
    if (FAILED(hr)) goto cleanup;

    /* Get the texture from the frame */
    hr = IDXGIResource_QueryInterface(frameRes, &IID_ID3D11Texture2D, (void **)&frameTex);
    if (FAILED(hr)) goto cleanup_frame;

    /* Create a CPU-accessible staging texture */
    D3D11_TEXTURE2D_DESC texDesc;
    ID3D11Texture2D_GetDesc(frameTex, &texDesc);
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texDesc.MiscFlags = 0;

    hr = ID3D11Device_CreateTexture2D(device, &texDesc, NULL, &staging);
    if (FAILED(hr)) goto cleanup_frame;

    /* Copy frame to staging */
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging,
                                     (ID3D11Resource *)frameTex);

    /* Map staging texture for CPU read */
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0,
                                 D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) goto cleanup_frame;

    /* Extract the requested region into an RGB buffer.
     * Desktop texture is BGRA, stb needs RGB. */
    unsigned char *rgb = (unsigned char *)malloc(req_w * req_h * 3);
    if (!rgb) {
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
        goto cleanup_frame;
    }

    unsigned char *src_base = (unsigned char *)mapped.pData;
    for (int row = 0; row < req_h; row++) {
        unsigned char *src_row = src_base + (req_y + row) * mapped.RowPitch + req_x * 4;
        unsigned char *dst_row = rgb + row * req_w * 3;
        for (int col = 0; col < req_w; col++) {
            dst_row[col * 3 + 0] = src_row[col * 4 + 2]; /* R (from B) */
            dst_row[col * 3 + 1] = src_row[col * 4 + 1]; /* G */
            dst_row[col * 3 + 2] = src_row[col * 4 + 0]; /* B (from R) */
        }
    }

    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);

    /* Encode to JPEG */
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    JpegWriteCtx jctx = { jpeg_out, jpeg_max, 0, 0 };
    int ok = stbi_write_jpg_to_func(jpeg_write_func, &jctx,
                                    req_w, req_h, 3, rgb, quality);
    free(rgb);

    if (!ok || jctx.overflow) {
        result = WMCP_ERR_BUFFER;
        goto cleanup_frame;
    }

    *jpeg_len = jctx.pos;
    result = WMCP_OK;

cleanup_frame:
    IDXGIOutputDuplication_ReleaseFrame(dupl);
    if (frameTex) ID3D11Texture2D_Release(frameTex);
    if (frameRes) IDXGIResource_Release(frameRes);

cleanup:
    if (staging) ID3D11Texture2D_Release(staging);
    if (dupl)    IDXGIOutputDuplication_Release(dupl);
    if (output1) IDXGIOutput1_Release(output1);
    if (output)  IDXGIOutput_Release(output);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgiDevice) IDXGIDevice_Release(dxgiDevice);
    if (ctx)     ID3D11DeviceContext_Release(ctx);
    if (device)  ID3D11Device_Release(device);

    return result;
}

/* ================================================================
 * Exported Functions
 * ================================================================ */

WMCP_API int wmcp_version(char *out, int out_max)
{
    const char *json = "{\"version\":\"" DLL_VERSION "\","
                       "\"capabilities\":[\"screenshot\"]}";
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
    if (!jpeg_out || jpeg_max < 1024 || !jpeg_len)
        return WMCP_ERR_GENERIC;
    *jpeg_len = 0;
    return capture_desktop(x, y, w, h, jpeg_out, jpeg_max, jpeg_len, quality);
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
