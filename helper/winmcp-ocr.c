/*
 * winmcp-ocr.c — OCR command via native DLL
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#include "winmcp.h"

/* ================================================================
 * Command: ocr_region
 *
 * Captures a screen region and runs Windows.Media.Ocr via the
 * native DLL (wmcp_ocr_region). Falls back to error if DLL
 * is not loaded or OCR is not available.
 * ================================================================ */

void cmd_ocr_region(SOCKET sock, const char *json)
{
    int x = 0, y = 0, w = 0, h = 0;
    char lang[64] = {0};

    if (!json_get_int(json, "x", &x) || !json_get_int(json, "y", &y) ||
        !json_get_int(json, "w", &w) || !json_get_int(json, "h", &h)) {
        send_error(sock, "Missing x/y/w/h parameters");
        return;
    }
    json_get_string(json, "lang", lang, sizeof(lang));

    log_msg("ocr_region: x=%d y=%d w=%d h=%d lang=%s", x, y, w, h, lang[0] ? lang : "(auto)");

    if (!g_native.ocr_region) {
        send_error(sock, "OCR not available - native DLL not loaded or missing ocr capability");
        return;
    }

    /* 256KB text buffer — should be more than enough for any screen region */
    char *text = (char *)malloc(256 * 1024);
    if (!text) {
        send_error(sock, "Memory allocation failed");
        return;
    }

    int rc = g_native.ocr_region(x, y, w, h,
                                  lang[0] ? lang : NULL,
                                  text, 256 * 1024);

    if (rc != 0) {  /* WMCP_OK = 0 */
        free(text);
        char msg[128];
        snprintf(msg, sizeof(msg), "OCR failed (error %d)", rc);
        send_error(sock, msg);
        return;
    }

    /* JSON-escape the recognized text and send response */
    int text_len = (int)strlen(text);
    /* Worst case: every char becomes \uXXXX (6x expansion) + JSON wrapper */
    int buf_size = text_len * 6 + 256;
    char *buf = (char *)malloc(buf_size);
    if (!buf) {
        free(text);
        send_error(sock, "Memory allocation failed");
        return;
    }

    /* Manual JSON escape for the text value */
    char *escaped = (char *)malloc(text_len * 6 + 1);
    if (!escaped) {
        free(text);
        free(buf);
        send_error(sock, "Memory allocation failed");
        return;
    }
    json_escape(text, escaped, text_len * 6 + 1);
    free(text);

    snprintf(buf, buf_size,
             "{\"status\":\"ok\",\"data\":{\"text\":\"%s\"}}", escaped);
    free(escaped);

    send_line(sock, buf);
    free(buf);
}
