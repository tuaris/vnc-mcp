/*
 * winmcp-native.c — WinMCP native DLL
 *
 * Implements DXGI Desktop Duplication screenshot capture with JPEG
 * encoding via stb_image_write, and Windows.Media.Ocr text recognition
 * via raw WinRT COM interfaces (no C++/WinRT, cross-compiles from FreeBSD).
 *
 * Cross-compiles from FreeBSD using Zig's mingw-w64 headers:
 *   zig cc -shared -target x86_64-windows-gnu -O2 native/winmcp-native.c
 *          native/stb_impl.c -ld3d11 -ldxgi -lole32
 *          -lapi-ms-win-core-winrt-l1-1-0
 *          -lapi-ms-win-core-winrt-string-l1-1-0
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
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "stb_image_write.h"

#define DLL_VERSION "0.4.0"

/* ================================================================
 * WinRT OCR Interface Definitions (raw COM vtables)
 *
 * These are manual definitions for the Windows.Media.Ocr and
 * Windows.Graphics.Imaging interfaces. Vtable slot order matches
 * the Windows SDK IDL exactly.
 * ================================================================ */

/* Forward declarations */
typedef struct IOcrEngine          IOcrEngine;
typedef struct IOcrEngineStatics   IOcrEngineStatics;
typedef struct IOcrResult          IOcrResult;
typedef struct ISoftwareBitmap     ISoftwareBitmap;
typedef struct ISoftwareBitmapFactory ISoftwareBitmapFactory;
typedef struct IBitmapBuffer       IBitmapBuffer;
typedef struct IMemoryBufferReference IMemoryBufferReference;
typedef struct IAsyncOperation_OcrResult IAsyncOperation_OcrResult;
typedef struct IAsyncInfo          IAsyncInfo;
typedef struct ILanguage           ILanguage;
typedef struct ILanguageFactory    ILanguageFactory;

/* IIDs (GUIDs) */
DEFINE_GUID(IID_IOcrEngine,
    0x5a14bc41, 0x5b76, 0x3140, 0xb6, 0x80, 0x88, 0x25, 0x56, 0x26, 0x83, 0xac);
DEFINE_GUID(IID_IOcrEngineStatics,
    0x5bffa85a, 0x3384, 0x3540, 0x99, 0x40, 0x69, 0x91, 0x20, 0xd4, 0x28, 0xa8);
DEFINE_GUID(IID_IOcrResult,
    0x9bd235b2, 0x175b, 0x3d6a, 0x92, 0xe2, 0x38, 0x8c, 0xe0, 0x61, 0x07, 0x24);
DEFINE_GUID(IID_ISoftwareBitmapFactory,
    0xc99feb69, 0x2d62, 0x4d47, 0xa6, 0xb3, 0x4f, 0xdb, 0x6a, 0x07, 0xfd, 0xf8);
DEFINE_GUID(IID_ISoftwareBitmap,
    0x689e0708, 0x7eef, 0x483f, 0x96, 0x3f, 0xda, 0x93, 0x88, 0x18, 0xe0, 0x73);
DEFINE_GUID(IID_IBitmapBuffer,
    0xa53e04c4, 0x399c, 0x438c, 0xb2, 0x8f, 0xa6, 0x3a, 0x6b, 0x83, 0xd1, 0xa1);
DEFINE_GUID(IID_IMemoryBufferReference,
    0xfbc4dd2b, 0x245b, 0x11e4, 0xaf, 0x98, 0x68, 0x94, 0x23, 0x26, 0x0c, 0xf8);
DEFINE_GUID(IID_IAsyncInfo,
    0x00000036, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(IID_ILanguageFactory,
    0x9b0252ac, 0x0c27, 0x44f8, 0xb7, 0x92, 0x97, 0x93, 0xfb, 0x66, 0xc6, 0x3e);

/* IMemoryBufferByteAccess — non-WinRT COM interface for raw byte access */
DEFINE_GUID(IID_IMemoryBufferByteAccess,
    0x5b0d3235, 0x4dba, 0x4d44, 0x86, 0x5e, 0x8f, 0x1d, 0x0e, 0x4f, 0xd0, 0x4d);
DEFINE_GUID(IID_IMemoryBuffer,
    0xfbc4dd2a, 0x245b, 0x11e4, 0xaf, 0x98, 0x68, 0x94, 0x23, 0x26, 0x0c, 0xf8);

typedef struct IMemoryBufferByteAccessVtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *This);
    ULONG   (STDMETHODCALLTYPE *Release)(void *This);
    /* IMemoryBufferByteAccess */
    HRESULT (STDMETHODCALLTYPE *GetBuffer)(void *This, BYTE **value, UINT32 *capacity);
} IMemoryBufferByteAccessVtbl;

typedef struct IMemoryBufferByteAccess {
    IMemoryBufferByteAccessVtbl *lpVtbl;
} IMemoryBufferByteAccess;

/* IMemoryBuffer — WinRT interface with CreateReference */
typedef struct IMemoryBuffer IMemoryBuffer;
typedef struct IMemoryBufferVtbl {
    /* IUnknown (0-2) */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IMemoryBuffer *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IMemoryBuffer *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IMemoryBuffer *This);
    /* IInspectable (3-5) */
    HRESULT (STDMETHODCALLTYPE *GetIids)(IMemoryBuffer *This, ULONG *cnt, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IMemoryBuffer *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IMemoryBuffer *This, int *level);
    /* IMemoryBuffer (6) */
    HRESULT (STDMETHODCALLTYPE *CreateReference)(IMemoryBuffer *This, IInspectable **ref);
} IMemoryBufferVtbl;

struct IMemoryBuffer { IMemoryBufferVtbl *lpVtbl; };

/* BitmapPixelFormat and BitmapAlphaMode enums */
typedef enum BitmapPixelFormat {
    BitmapPixelFormat_Unknown = 0,
    BitmapPixelFormat_Rgba16 = 12,
    BitmapPixelFormat_Rgba8 = 30,
    BitmapPixelFormat_Bgra8 = 87,
    BitmapPixelFormat_Gray16 = 57,
    BitmapPixelFormat_Gray8 = 62,
    BitmapPixelFormat_Nv12 = 103,
    BitmapPixelFormat_Yuy2 = 107
} BitmapPixelFormat;

typedef enum BitmapAlphaMode {
    BitmapAlphaMode_Premultiplied = 0,
    BitmapAlphaMode_Straight = 1,
    BitmapAlphaMode_Ignore = 2
} BitmapAlphaMode;

typedef enum BitmapBufferAccessMode {
    BitmapBufferAccessMode_Read = 0,
    BitmapBufferAccessMode_ReadWrite = 1,
    BitmapBufferAccessMode_Write = 2
} BitmapBufferAccessMode;

typedef enum AsyncStatus {
    AsyncStatus_Created    = 0,
    AsyncStatus_Started    = 0,
    AsyncStatus_Completed  = 1,
    AsyncStatus_Canceled   = 2,
    AsyncStatus_Error      = 3
} AsyncStatus;

/* IOcrEngineStatics vtable */
typedef struct IOcrEngineStaticsVtbl {
    /* IUnknown (0-2) */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IOcrEngineStatics *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IOcrEngineStatics *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IOcrEngineStatics *This);
    /* IInspectable (3-5) */
    HRESULT (STDMETHODCALLTYPE *GetIids)(IOcrEngineStatics *This, ULONG *cnt, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IOcrEngineStatics *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IOcrEngineStatics *This, int *level);
    /* IOcrEngineStatics (6-10) */
    HRESULT (STDMETHODCALLTYPE *MaxImageDimension)(IOcrEngineStatics *This, UINT32 *value);
    HRESULT (STDMETHODCALLTYPE *AvailableRecognizerLanguages)(IOcrEngineStatics *This, void **value);
    HRESULT (STDMETHODCALLTYPE *IsLanguageSupported)(IOcrEngineStatics *This, ILanguage *lang, BOOLEAN *result);
    HRESULT (STDMETHODCALLTYPE *TryCreateFromLanguage)(IOcrEngineStatics *This, ILanguage *lang, IOcrEngine **engine);
    HRESULT (STDMETHODCALLTYPE *TryCreateFromUserProfileLanguages)(IOcrEngineStatics *This, IOcrEngine **engine);
} IOcrEngineStaticsVtbl;

struct IOcrEngineStatics { IOcrEngineStaticsVtbl *lpVtbl; };

/* IOcrEngine vtable */
typedef struct IOcrEngineVtbl {
    /* IUnknown (0-2) */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IOcrEngine *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IOcrEngine *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IOcrEngine *This);
    /* IInspectable (3-5) */
    HRESULT (STDMETHODCALLTYPE *GetIids)(IOcrEngine *This, ULONG *cnt, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IOcrEngine *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IOcrEngine *This, int *level);
    /* IOcrEngine (6) */
    HRESULT (STDMETHODCALLTYPE *RecognizeAsync)(IOcrEngine *This, ISoftwareBitmap *bitmap, IAsyncOperation_OcrResult **op);
    HRESULT (STDMETHODCALLTYPE *get_RecognizerLanguage)(IOcrEngine *This, ILanguage **lang);
} IOcrEngineVtbl;

struct IOcrEngine { IOcrEngineVtbl *lpVtbl; };

/* IOcrResult vtable */
typedef struct IOcrResultVtbl {
    /* IUnknown (0-2) */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IOcrResult *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IOcrResult *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IOcrResult *This);
    /* IInspectable (3-5) */
    HRESULT (STDMETHODCALLTYPE *GetIids)(IOcrResult *This, ULONG *cnt, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IOcrResult *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IOcrResult *This, int *level);
    /* IOcrResult (6-8) */
    HRESULT (STDMETHODCALLTYPE *get_Lines)(IOcrResult *This, void **lines);
    HRESULT (STDMETHODCALLTYPE *get_TextAngle)(IOcrResult *This, void **angle);
    HRESULT (STDMETHODCALLTYPE *get_Text)(IOcrResult *This, HSTRING *text);
} IOcrResultVtbl;

struct IOcrResult { IOcrResultVtbl *lpVtbl; };

/* IAsyncOperation<OcrResult> vtable */
typedef struct IAsyncOperation_OcrResultVtbl {
    /* IUnknown (0-2) */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAsyncOperation_OcrResult *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IAsyncOperation_OcrResult *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IAsyncOperation_OcrResult *This);
    /* IInspectable (3-5) */
    HRESULT (STDMETHODCALLTYPE *GetIids)(IAsyncOperation_OcrResult *This, ULONG *cnt, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IAsyncOperation_OcrResult *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IAsyncOperation_OcrResult *This, int *level);
    /* IAsyncOperation<OcrResult> (6-7) */
    HRESULT (STDMETHODCALLTYPE *put_Completed)(IAsyncOperation_OcrResult *This, void *handler);
    HRESULT (STDMETHODCALLTYPE *get_Completed)(IAsyncOperation_OcrResult *This, void **handler);
    HRESULT (STDMETHODCALLTYPE *GetResults)(IAsyncOperation_OcrResult *This, IOcrResult **result);
} IAsyncOperation_OcrResultVtbl;

struct IAsyncOperation_OcrResult { IAsyncOperation_OcrResultVtbl *lpVtbl; };

/* IAsyncInfo vtable */
typedef struct IAsyncInfoVtbl {
    /* IUnknown (0-2) */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAsyncInfo *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IAsyncInfo *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IAsyncInfo *This);
    /* IInspectable (3-5) */
    HRESULT (STDMETHODCALLTYPE *GetIids)(IAsyncInfo *This, ULONG *cnt, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IAsyncInfo *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IAsyncInfo *This, int *level);
    /* IAsyncInfo (6-10) */
    HRESULT (STDMETHODCALLTYPE *get_Id)(IAsyncInfo *This, UINT32 *id);
    HRESULT (STDMETHODCALLTYPE *get_Status)(IAsyncInfo *This, AsyncStatus *status);
    HRESULT (STDMETHODCALLTYPE *get_ErrorCode)(IAsyncInfo *This, HRESULT *error);
    HRESULT (STDMETHODCALLTYPE *Cancel)(IAsyncInfo *This);
    HRESULT (STDMETHODCALLTYPE *Close)(IAsyncInfo *This);
} IAsyncInfoVtbl;

struct IAsyncInfo { IAsyncInfoVtbl *lpVtbl; };

/* ISoftwareBitmapFactory vtable */
typedef struct ISoftwareBitmapFactoryVtbl {
    /* IUnknown (0-2) */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ISoftwareBitmapFactory *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ISoftwareBitmapFactory *This);
    ULONG   (STDMETHODCALLTYPE *Release)(ISoftwareBitmapFactory *This);
    /* IInspectable (3-5) */
    HRESULT (STDMETHODCALLTYPE *GetIids)(ISoftwareBitmapFactory *This, ULONG *cnt, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(ISoftwareBitmapFactory *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(ISoftwareBitmapFactory *This, int *level);
    /* ISoftwareBitmapFactory (6) */
    HRESULT (STDMETHODCALLTYPE *Create)(ISoftwareBitmapFactory *This,
        BitmapPixelFormat format, INT32 width, INT32 height, ISoftwareBitmap **bitmap);
    HRESULT (STDMETHODCALLTYPE *CreateWithAlpha)(ISoftwareBitmapFactory *This,
        BitmapPixelFormat format, INT32 width, INT32 height, BitmapAlphaMode alpha, ISoftwareBitmap **bitmap);
} ISoftwareBitmapFactoryVtbl;

struct ISoftwareBitmapFactory { ISoftwareBitmapFactoryVtbl *lpVtbl; };

/* ISoftwareBitmap vtable (partial — only methods we need) */
typedef struct ISoftwareBitmapVtbl {
    /* IUnknown (0-2) */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ISoftwareBitmap *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ISoftwareBitmap *This);
    ULONG   (STDMETHODCALLTYPE *Release)(ISoftwareBitmap *This);
    /* IInspectable (3-5) */
    HRESULT (STDMETHODCALLTYPE *GetIids)(ISoftwareBitmap *This, ULONG *cnt, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(ISoftwareBitmap *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(ISoftwareBitmap *This, int *level);
    /* ISoftwareBitmap (6+) */
    HRESULT (STDMETHODCALLTYPE *get_BitmapPixelFormat)(ISoftwareBitmap *This, BitmapPixelFormat *fmt);
    HRESULT (STDMETHODCALLTYPE *get_BitmapAlphaMode)(ISoftwareBitmap *This, BitmapAlphaMode *mode);
    HRESULT (STDMETHODCALLTYPE *get_PixelWidth)(ISoftwareBitmap *This, INT32 *width);
    HRESULT (STDMETHODCALLTYPE *get_PixelHeight)(ISoftwareBitmap *This, INT32 *height);
    HRESULT (STDMETHODCALLTYPE *get_IsReadOnly)(ISoftwareBitmap *This, BOOLEAN *value);
    HRESULT (STDMETHODCALLTYPE *put_DpiX)(ISoftwareBitmap *This, double dpi);
    HRESULT (STDMETHODCALLTYPE *get_DpiX)(ISoftwareBitmap *This, double *dpi);
    HRESULT (STDMETHODCALLTYPE *put_DpiY)(ISoftwareBitmap *This, double dpi);
    HRESULT (STDMETHODCALLTYPE *get_DpiY)(ISoftwareBitmap *This, double *dpi);
    HRESULT (STDMETHODCALLTYPE *LockBuffer)(ISoftwareBitmap *This, BitmapBufferAccessMode mode, IBitmapBuffer **buffer);
    HRESULT (STDMETHODCALLTYPE *CopyTo)(ISoftwareBitmap *This, ISoftwareBitmap *dest);
    HRESULT (STDMETHODCALLTYPE *CopyFromBuffer)(ISoftwareBitmap *This, void *buffer);
    HRESULT (STDMETHODCALLTYPE *CopyToBuffer)(ISoftwareBitmap *This, void *buffer);
    HRESULT (STDMETHODCALLTYPE *GetReadOnlyView)(ISoftwareBitmap *This, ISoftwareBitmap **view);
} ISoftwareBitmapVtbl;

struct ISoftwareBitmap { ISoftwareBitmapVtbl *lpVtbl; };

/* IBitmapBuffer vtable */
typedef struct IBitmapBufferVtbl {
    /* IUnknown (0-2) */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IBitmapBuffer *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IBitmapBuffer *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IBitmapBuffer *This);
    /* IInspectable (3-5) */
    HRESULT (STDMETHODCALLTYPE *GetIids)(IBitmapBuffer *This, ULONG *cnt, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(IBitmapBuffer *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(IBitmapBuffer *This, int *level);
    /* IBitmapBuffer (6-7) */
    HRESULT (STDMETHODCALLTYPE *GetPlaneCount)(IBitmapBuffer *This, INT32 *count);
    HRESULT (STDMETHODCALLTYPE *GetPlaneDescription)(IBitmapBuffer *This, INT32 index, void *desc);
} IBitmapBufferVtbl;

struct IBitmapBuffer { IBitmapBufferVtbl *lpVtbl; };

/* ILanguageFactory vtable */
typedef struct ILanguageFactoryVtbl {
    /* IUnknown (0-2) */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ILanguageFactory *This, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ILanguageFactory *This);
    ULONG   (STDMETHODCALLTYPE *Release)(ILanguageFactory *This);
    /* IInspectable (3-5) */
    HRESULT (STDMETHODCALLTYPE *GetIids)(ILanguageFactory *This, ULONG *cnt, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(ILanguageFactory *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(ILanguageFactory *This, int *level);
    /* ILanguageFactory (6) */
    HRESULT (STDMETHODCALLTYPE *CreateLanguage)(ILanguageFactory *This, HSTRING tag, ILanguage **lang);
} ILanguageFactoryVtbl;

struct ILanguageFactory { ILanguageFactoryVtbl *lpVtbl; };

struct ILanguage { IInspectableVtbl *lpVtbl; };  /* opaque, passed through */

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
                       "\"capabilities\":[\"screenshot\",\"ocr\"]}";
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

/* ================================================================
 * DXGI capture to raw BGRA pixels (reuses same device/duplication
 * pattern as capture_desktop but returns raw pixels instead of JPEG)
 * ================================================================ */

static int capture_bgra(int req_x, int req_y, int req_w, int req_h,
                        unsigned char **out_pixels, int *out_stride)
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

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           0, NULL, 0, D3D11_SDK_VERSION,
                           &device, NULL, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
                               0, NULL, 0, D3D11_SDK_VERSION,
                               &device, NULL, &ctx);
        if (FAILED(hr)) goto cleanup;
    }

    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgiDevice);
    if (FAILED(hr)) goto cleanup;
    hr = IDXGIDevice_GetAdapter(dxgiDevice, &adapter);
    if (FAILED(hr)) goto cleanup;
    hr = IDXGIAdapter_EnumOutputs(adapter, 0, &output);
    if (FAILED(hr)) goto cleanup;
    hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1);
    if (FAILED(hr)) goto cleanup;
    hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)device, &dupl);
    if (FAILED(hr)) goto cleanup;

    DXGI_OUTDUPL_DESC duplDesc;
    IDXGIOutputDuplication_GetDesc(dupl, &duplDesc);
    int desk_w = (int)duplDesc.ModeDesc.Width;
    int desk_h = (int)duplDesc.ModeDesc.Height;

    if (req_w <= 0 || req_h <= 0) { req_x = 0; req_y = 0; req_w = desk_w; req_h = desk_h; }
    if (req_x < 0) req_x = 0;
    if (req_y < 0) req_y = 0;
    if (req_x + req_w > desk_w) req_w = desk_w - req_x;
    if (req_y + req_h > desk_h) req_h = desk_h - req_y;
    if (req_w <= 0 || req_h <= 0) goto cleanup;

    hr = E_FAIL;
    for (int attempt = 0; attempt < 4; attempt++) {
        hr = IDXGIOutputDuplication_AcquireNextFrame(dupl, 500, &frameInfo, &frameRes);
        if (SUCCEEDED(hr)) break;
        Sleep(50);
    }
    if (FAILED(hr)) goto cleanup;

    hr = IDXGIResource_QueryInterface(frameRes, &IID_ID3D11Texture2D, (void **)&frameTex);
    if (FAILED(hr)) goto cleanup_frame;

    D3D11_TEXTURE2D_DESC texDesc;
    ID3D11Texture2D_GetDesc(frameTex, &texDesc);
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texDesc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(device, &texDesc, NULL, &staging);
    if (FAILED(hr)) goto cleanup_frame;

    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)frameTex);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) goto cleanup_frame;

    /* Copy the requested region into a contiguous BGRA buffer */
    int stride = req_w * 4;
    unsigned char *pixels = (unsigned char *)malloc(stride * req_h);
    if (!pixels) {
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
        goto cleanup_frame;
    }

    unsigned char *src_base = (unsigned char *)mapped.pData;
    for (int row = 0; row < req_h; row++) {
        unsigned char *src_row = src_base + (req_y + row) * mapped.RowPitch + req_x * 4;
        memcpy(pixels + row * stride, src_row, stride);
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);

    *out_pixels = pixels;
    *out_stride = stride;
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
 * WinRT OCR Implementation
 *
 * Uses Windows.Media.Ocr via raw COM vtable calls.
 * Captures screen region with DXGI, creates SoftwareBitmap,
 * runs OCR, returns recognized text as UTF-8.
 * ================================================================ */

/* Debug log for OCR troubleshooting */
static FILE *ocr_log = NULL;
static void ocr_dbg(const char *fmt, ...)
{
    if (!ocr_log)
        ocr_log = fopen("C:\\ProgramData\\winmcp-ocr-debug.log", "a");
    if (!ocr_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(ocr_log, fmt, ap);
    va_end(ap);
    fprintf(ocr_log, "\n");
    fflush(ocr_log);
}

/* ================================================================
 * Lazy-loaded WinRT functions (avoid compile-time link dependency
 * on api-ms-win-core-winrt-* DLLs which blocks LoadLibrary)
 * ================================================================ */

typedef HRESULT (WINAPI *pfn_RoInitialize)(int);
typedef HRESULT (WINAPI *pfn_RoGetActivationFactory)(HSTRING, REFIID, void **);
typedef HRESULT (WINAPI *pfn_WindowsCreateString)(const wchar_t *, UINT32, HSTRING *);
typedef HRESULT (WINAPI *pfn_WindowsDeleteString)(HSTRING);
typedef const wchar_t * (WINAPI *pfn_WindowsGetStringRawBuffer)(HSTRING, UINT32 *);

static struct {
    int loaded;
    pfn_RoInitialize              RoInit;
    pfn_RoGetActivationFactory    RoGetFactory;
    pfn_WindowsCreateString       WinCreateStr;
    pfn_WindowsDeleteString       WinDeleteStr;
    pfn_WindowsGetStringRawBuffer WinGetStrBuf;
} winrt = {0};

static int load_winrt(void)
{
    if (winrt.loaded) return winrt.RoInit ? 1 : 0;
    winrt.loaded = 1;

    HMODULE hrt = LoadLibraryA("combase.dll");
    if (!hrt) return 0;

    winrt.RoInit      = (pfn_RoInitialize)GetProcAddress(hrt, "RoInitialize");
    winrt.RoGetFactory = (pfn_RoGetActivationFactory)GetProcAddress(hrt, "RoGetActivationFactory");
    winrt.WinCreateStr = (pfn_WindowsCreateString)GetProcAddress(hrt, "WindowsCreateString");
    winrt.WinDeleteStr = (pfn_WindowsDeleteString)GetProcAddress(hrt, "WindowsDeleteString");
    winrt.WinGetStrBuf = (pfn_WindowsGetStringRawBuffer)GetProcAddress(hrt, "WindowsGetStringRawBuffer");

    if (!winrt.RoInit || !winrt.RoGetFactory || !winrt.WinCreateStr ||
        !winrt.WinDeleteStr || !winrt.WinGetStrBuf) {
        winrt.RoInit = NULL;
        return 0;
    }
    return 1;
}

static HSTRING create_hstring(const wchar_t *str)
{
    HSTRING hs = NULL;
    UINT32 len = (UINT32)wcslen(str);
    winrt.WinCreateStr(str, len, &hs);
    return hs;
}

WMCP_API int wmcp_ocr_region(int x, int y, int w, int h,
                             const char *lang,
                             char *text_out, int text_max)
{
    if (!text_out || text_max < 1)
        return WMCP_ERR_GENERIC;
    text_out[0] = '\0';

    ocr_dbg("ocr_region: x=%d y=%d w=%d h=%d lang=%s", x, y, w, h, lang ? lang : "(null)");

    /* Step 0: Load WinRT functions */
    if (!load_winrt()) {
        ocr_dbg("FAILED: WinRT functions not available");
        return WMCP_ERR_INIT;
    }

    /* Step 1: Capture screen region as BGRA pixels */
    unsigned char *pixels = NULL;
    int stride = 0;
    int rc = capture_bgra(x, y, w, h, &pixels, &stride);
    ocr_dbg("step1 capture_bgra: rc=%d pixels=%p stride=%d", rc, (void*)pixels, stride);
    if (rc != WMCP_OK || !pixels)
        return WMCP_ERR_CAPTURE;

    int result = WMCP_ERR_GENERIC;
    HRESULT hr;

    /* Step 2: Initialize WinRT */
    hr = winrt.RoInit(1 /* RO_INIT_MULTITHREADED */);
    ocr_dbg("step2 RoInitialize: hr=0x%08lx", (unsigned long)hr);
    if (FAILED(hr) && hr != S_FALSE && hr != (HRESULT)0x80010106L /* RPC_E_CHANGED_MODE */) {
        free(pixels);
        return WMCP_ERR_INIT;
    }

    ISoftwareBitmapFactory *bmpFactory = NULL;
    ISoftwareBitmap *bitmap = NULL;
    IBitmapBuffer *bmpBuf = NULL;
    IMemoryBufferByteAccess *byteAccess = NULL;
    IMemoryBuffer *memBuf = NULL;
    IInspectable *memRef = NULL;
    IOcrEngineStatics *ocrStatics = NULL;
    IOcrEngine *ocrEngine = NULL;
    IAsyncOperation_OcrResult *asyncOp = NULL;
    IOcrResult *ocrResult = NULL;
    ILanguageFactory *langFactory = NULL;
    ILanguage *language = NULL;

    /* Step 3: Create SoftwareBitmap(Bgra8, w, h, Premultiplied) */
    ocr_dbg("step3 RoGetActivationFactory(SoftwareBitmap)...");
    HSTRING hs_bitmap_class = create_hstring(L"Windows.Graphics.Imaging.SoftwareBitmap");
    hr = winrt.RoGetFactory(hs_bitmap_class, &IID_ISoftwareBitmapFactory, (void **)&bmpFactory);
    winrt.WinDeleteStr(hs_bitmap_class);
    ocr_dbg("step3 result: hr=0x%08lx factory=%p", (unsigned long)hr, (void*)bmpFactory);
    if (FAILED(hr) || !bmpFactory) { result = WMCP_ERR_OCR_FACTORY; goto ocr_cleanup; }

    ocr_dbg("step3b CreateWithAlpha(Bgra8, %d, %d)...", w, h);
    hr = bmpFactory->lpVtbl->CreateWithAlpha(bmpFactory,
        BitmapPixelFormat_Bgra8, w, h, BitmapAlphaMode_Premultiplied, &bitmap);
    ocr_dbg("step3b result: hr=0x%08lx bitmap=%p", (unsigned long)hr, (void*)bitmap);
    if (FAILED(hr) || !bitmap) { result = WMCP_ERR_OCR_BITMAP; goto ocr_cleanup; }

    /* Step 4: Lock bitmap buffer and copy BGRA pixels */
    ocr_dbg("step4 LockBuffer...");
    hr = bitmap->lpVtbl->LockBuffer(bitmap, BitmapBufferAccessMode_ReadWrite, &bmpBuf);
    ocr_dbg("step4 result: hr=0x%08lx buf=%p", (unsigned long)hr, (void*)bmpBuf);
    if (FAILED(hr) || !bmpBuf) { result = WMCP_ERR_OCR_LOCK; goto ocr_cleanup; }

    /* Get raw byte access: IBitmapBuffer → QI IMemoryBuffer → CreateReference → QI IMemoryBufferByteAccess */
    ocr_dbg("step4b QI IMemoryBuffer...");
    hr = bmpBuf->lpVtbl->QueryInterface(bmpBuf, &IID_IMemoryBuffer, (void **)&memBuf);
    ocr_dbg("step4b result: hr=0x%08lx memBuf=%p", (unsigned long)hr, (void*)memBuf);
    if (FAILED(hr) || !memBuf) { result = WMCP_ERR_OCR_LOCK; goto ocr_cleanup; }

    ocr_dbg("step4c CreateReference...");
    hr = memBuf->lpVtbl->CreateReference(memBuf, &memRef);
    ocr_dbg("step4c result: hr=0x%08lx memRef=%p", (unsigned long)hr, (void*)memRef);
    if (FAILED(hr) || !memRef) { result = WMCP_ERR_OCR_LOCK; goto ocr_cleanup; }

    ocr_dbg("step4d QI IMemoryBufferByteAccess...");
    hr = memRef->lpVtbl->QueryInterface(memRef, &IID_IMemoryBufferByteAccess, (void **)&byteAccess);
    ocr_dbg("step4d result: hr=0x%08lx byteAccess=%p", (unsigned long)hr, (void*)byteAccess);
    if (FAILED(hr) || !byteAccess) { result = WMCP_ERR_OCR_LOCK; goto ocr_cleanup; }

    BYTE *bufPtr = NULL;
    UINT32 bufCap = 0;
    hr = byteAccess->lpVtbl->GetBuffer(byteAccess, &bufPtr, &bufCap);
    if (FAILED(hr) || !bufPtr) { result = WMCP_ERR_OCR_LOCK; goto ocr_cleanup; }

    /* Copy BGRA pixels into the SoftwareBitmap's buffer */
    UINT32 expected = (UINT32)(w * h * 4);
    ocr_dbg("step4e buffer: bufCap=%u expected=%u bufPtr=%p", bufCap, expected, (void*)bufPtr);
    if (bufCap >= expected) {
        memcpy(bufPtr, pixels, expected);
        ocr_dbg("step4e memcpy done (%u bytes)", expected);
    } else {
        ocr_dbg("step4e SKIPPED: bufCap(%u) < expected(%u)", bufCap, expected);
    }

    /* Release the buffer lock before OCR */
    byteAccess->lpVtbl->Release(byteAccess); byteAccess = NULL;
    memRef->lpVtbl->Release(memRef); memRef = NULL;
    memBuf->lpVtbl->Release(memBuf); memBuf = NULL;
    bmpBuf->lpVtbl->Release(bmpBuf); bmpBuf = NULL;

    /* Step 5: Create OCR engine */
    ocr_dbg("step5 RoGetActivationFactory(OcrEngine)...");
    HSTRING hs_ocr_class = create_hstring(L"Windows.Media.Ocr.OcrEngine");
    hr = winrt.RoGetFactory(hs_ocr_class, &IID_IOcrEngineStatics, (void **)&ocrStatics);
    winrt.WinDeleteStr(hs_ocr_class);
    ocr_dbg("step5 result: hr=0x%08lx statics=%p", (unsigned long)hr, (void*)ocrStatics);
    if (FAILED(hr) || !ocrStatics) { result = WMCP_ERR_OCR_ENGINE; goto ocr_cleanup; }

    if (lang && lang[0]) {
        /* Create engine for specific language */
        HSTRING hs_lang_class = create_hstring(L"Windows.Globalization.Language");
        hr = winrt.RoGetFactory(hs_lang_class, &IID_ILanguageFactory, (void **)&langFactory);
        winrt.WinDeleteStr(hs_lang_class);
        if (SUCCEEDED(hr) && langFactory) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, lang, -1, NULL, 0);
            wchar_t *wlang = (wchar_t *)malloc(wlen * sizeof(wchar_t));
            if (wlang) {
                MultiByteToWideChar(CP_UTF8, 0, lang, -1, wlang, wlen);
                HSTRING hs_lang = create_hstring(wlang);
                hr = langFactory->lpVtbl->CreateLanguage(langFactory, hs_lang, &language);
                winrt.WinDeleteStr(hs_lang);
                free(wlang);
                if (SUCCEEDED(hr) && language) {
                    hr = ocrStatics->lpVtbl->TryCreateFromLanguage(ocrStatics, language, &ocrEngine);
                }
            }
        }
    }

    /* Fall back to user profile languages if specific language failed */
    if (!ocrEngine) {
        hr = ocrStatics->lpVtbl->TryCreateFromUserProfileLanguages(ocrStatics, &ocrEngine);
    }
    if (FAILED(hr) || !ocrEngine) { result = WMCP_ERR_OCR_ENGINE; goto ocr_cleanup; }

    /* Step 6: RecognizeAsync */
    ocr_dbg("step6 RecognizeAsync...");
    hr = ocrEngine->lpVtbl->RecognizeAsync(ocrEngine, bitmap, &asyncOp);
    ocr_dbg("step6 result: hr=0x%08lx asyncOp=%p", (unsigned long)hr, (void*)asyncOp);
    if (FAILED(hr) || !asyncOp) { result = WMCP_ERR_OCR_ASYNC; goto ocr_cleanup; }

    /* Step 7: Wait for async completion by polling IAsyncInfo::Status */
    IAsyncInfo *asyncInfo = NULL;
    hr = asyncOp->lpVtbl->QueryInterface(asyncOp, &IID_IAsyncInfo, (void **)&asyncInfo);
    if (FAILED(hr) || !asyncInfo) { result = WMCP_ERR_OCR_ASYNC; goto ocr_cleanup; }

    for (int i = 0; i < 200; i++) {  /* 10 seconds max */
        AsyncStatus status = AsyncStatus_Created;
        hr = asyncInfo->lpVtbl->get_Status(asyncInfo, &status);
        if (FAILED(hr)) break;
        if (status == AsyncStatus_Completed) break;
        if (status == AsyncStatus_Error || status == AsyncStatus_Canceled) {
            hr = E_FAIL;
            break;
        }
        Sleep(50);
    }
    asyncInfo->lpVtbl->Release(asyncInfo);
    ocr_dbg("step7 async done: hr=0x%08lx", (unsigned long)hr);
    if (FAILED(hr)) { result = WMCP_ERR_OCR_ASYNC; goto ocr_cleanup; }

    /* Step 8: Get result text */
    ocr_dbg("step8 GetResults...");
    hr = asyncOp->lpVtbl->GetResults(asyncOp, &ocrResult);
    ocr_dbg("step8 GetResults: hr=0x%08lx result=%p", (unsigned long)hr, (void*)ocrResult);
    if (FAILED(hr) || !ocrResult) { result = WMCP_ERR_OCR_RESULT; goto ocr_cleanup; }

    HSTRING htext = NULL;
    ocr_dbg("step8b get_Text...");
    hr = ocrResult->lpVtbl->get_Text(ocrResult, &htext);
    ocr_dbg("step8b get_Text: hr=0x%08lx htext=%p", (unsigned long)hr, (void*)htext);
    if (FAILED(hr)) { result = WMCP_ERR_OCR_RESULT; goto ocr_cleanup; }
    /* NULL htext = empty string (no text recognized) — not an error */

    /* Convert HSTRING (UTF-16) to UTF-8 */
    UINT32 wlen2 = 0;
    const wchar_t *wtext = winrt.WinGetStrBuf(htext, &wlen2);
    ocr_dbg("step8c text: wlen=%u wtext=%p", wlen2, (void*)wtext);
    if (wtext && wlen2 > 0) {
        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wtext, wlen2, NULL, 0, NULL, NULL);
        if (utf8_len > 0 && utf8_len < text_max) {
            WideCharToMultiByte(CP_UTF8, 0, wtext, wlen2, text_out, text_max - 1, NULL, NULL);
            text_out[utf8_len] = '\0';
        } else if (utf8_len >= text_max) {
            WideCharToMultiByte(CP_UTF8, 0, wtext, wlen2, text_out, text_max - 1, NULL, NULL);
            text_out[text_max - 1] = '\0';
            result = WMCP_ERR_BUFFER;
            goto ocr_cleanup_skip;
        }
    }
    if (htext) winrt.WinDeleteStr(htext);

    result = WMCP_OK;

ocr_cleanup:
ocr_cleanup_skip:
    if (ocrResult)  ocrResult->lpVtbl->Release(ocrResult);
    if (asyncOp)    asyncOp->lpVtbl->Release(asyncOp);
    if (ocrEngine)  ocrEngine->lpVtbl->Release(ocrEngine);
    if (ocrStatics) ocrStatics->lpVtbl->Release(ocrStatics);
    if (language)   ((IInspectable *)language)->lpVtbl->Release((IInspectable *)language);
    if (langFactory) langFactory->lpVtbl->Release(langFactory);
    if (byteAccess) byteAccess->lpVtbl->Release(byteAccess);
    if (memRef)      memRef->lpVtbl->Release(memRef);
    if (memBuf)      memBuf->lpVtbl->Release(memBuf);
    if (bmpBuf)     bmpBuf->lpVtbl->Release(bmpBuf);
    if (bitmap)     bitmap->lpVtbl->Release(bitmap);
    if (bmpFactory) bmpFactory->lpVtbl->Release(bmpFactory);
    free(pixels);

    return result;
}
