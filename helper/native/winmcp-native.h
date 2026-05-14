/*
 * winmcp-native.h — Public C ABI for WinMCP native DLL
 *
 * This header defines the interface between winmcp.exe and
 * winmcp-native.dll. The DLL provides WinRT/DXGI capabilities
 * that require the Windows SDK and cannot be cross-compiled.
 *
 * The agent loads the DLL at startup via LoadLibrary and resolves
 * function pointers via GetProcAddress. If the DLL is absent,
 * the agent continues without native capabilities.
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#ifndef WINMCP_NATIVE_H
#define WINMCP_NATIVE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WINMCP_NATIVE_EXPORTS
  #define WMCP_API __declspec(dllexport)
#else
  #define WMCP_API __declspec(dllimport)
#endif

/* Return codes */
#define WMCP_OK             0
#define WMCP_ERR_GENERIC   -1
#define WMCP_ERR_NOT_IMPL  -2
#define WMCP_ERR_BUFFER    -3  /* output buffer too small */
#define WMCP_ERR_INIT      -4  /* initialization failed */
#define WMCP_ERR_CAPTURE   -5  /* screen capture failed */

/*
 * wmcp_version — Get DLL version and capability info.
 *
 * Writes a JSON string to `out` with version and supported features:
 *   {"version":"0.2.0","capabilities":["screenshot","ocr"]}
 *
 * Returns: WMCP_OK on success, WMCP_ERR_BUFFER if out_max too small.
 */
WMCP_API int wmcp_version(char *out, int out_max);

/*
 * wmcp_screenshot — Capture a screen region as JPEG.
 *
 * Uses DXGI Desktop Duplication (IDXGIOutputDuplication) for capture.
 * Encodes the specified rectangle to JPEG and writes to jpeg_out.
 *
 * Parameters:
 *   x, y, w, h   — capture rectangle (0,0 = full screen if w=0, h=0)
 *   jpeg_out      — output buffer for JPEG data
 *   jpeg_max      — size of output buffer in bytes
 *   jpeg_len      — [out] actual JPEG size written
 *   quality       — JPEG quality 1-100
 *
 * Returns: WMCP_OK on success, WMCP_ERR_* on failure.
 */
WMCP_API int wmcp_screenshot(int x, int y, int w, int h,
                             unsigned char *jpeg_out, int jpeg_max,
                             int *jpeg_len, int quality);

/*
 * wmcp_ocr_region — Recognize text in a screen region.
 *
 * Captures the specified rectangle and runs Windows.Media.Ocr
 * (C++/WinRT) on the bitmap.
 *
 * Parameters:
 *   x, y, w, h  — screen region to capture and OCR
 *   lang         — BCP 47 language tag (e.g., "en-US"), NULL for system default
 *   text_out     — output buffer for recognized text (UTF-8)
 *   text_max     — size of output buffer in bytes
 *
 * Returns: WMCP_OK on success, WMCP_ERR_* on failure.
 */
WMCP_API int wmcp_ocr_region(int x, int y, int w, int h,
                             const char *lang,
                             char *text_out, int text_max);

#ifdef __cplusplus
}
#endif

#endif /* WINMCP_NATIVE_H */
