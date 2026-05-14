/*
 * winmcp-ocr.c — OCR command (stub — native DLL not yet available)
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#include "winmcp.h"

/* ================================================================
 * Command: ocr_region
 *
 * Stub implementation. Native OCR requires winmcp-native.dll
 * (MSVC-compiled WinRT wrapper). See issue #14.
 * ================================================================ */

void cmd_ocr_region(SOCKET sock, const char *json)
{
    (void)json;
    send_error(sock, "OCR not available - native DLL not installed (see issue #14)");
}
