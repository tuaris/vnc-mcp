/*
 * vnc-helper.c — Windows helper agent for VNC MCP Server
 *
 * Provides local system info (cursor, windows, screen) and command execution
 * over a simple JSON-over-TCP protocol. Runs as a system tray application.
 *
 * Cross-compile from FreeBSD:
 *   zig build helper
 * Or manually:
 *   zig cc -target x86_64-windows-gnu -O2 -mwindows -o vnc-helper.exe \
 *     helper/vnc-helper.c -lws2_32 -lshell32 -luser32 -lgdi32
 *
 * Usage:
 *   vnc-helper.exe              Run as tray app (default)
 *   vnc-helper.exe -console     Run with console output for debugging
 *   vnc-helper.exe -port 9800   Set listen port (default: 9800)
 *   vnc-helper.exe install      Add to Windows startup (HKCU Run key)
 *   vnc-helper.exe uninstall    Remove from Windows startup
 *
 * Protocol: newline-delimited JSON over TCP
 *   Request:  {"command":"cursor_position"}\n
 *   Response: {"status":"ok","data":{...}}\n
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VNC_HELPER_VERSION "0.1.0"
#define DEFAULT_PORT       9800
#define MAX_REQUEST        65536
#define MAX_RESPONSE       (2 * 1024 * 1024)
#define MAX_CMD_OUTPUT     (512 * 1024)

#include "resource.h"

#define WM_TRAYICON  (WM_USER + 1)
#define IDM_EXIT     1001
#define IDM_ABOUT    1002

/* ================================================================
 * Globals
 * ================================================================ */

static int  g_port    = DEFAULT_PORT;
static int  g_console = 0;
static int  g_running = 1;
static HWND g_hwnd    = NULL;
static NOTIFYICONDATAA g_nid;
static HICON g_icon_normal    = NULL;
static HICON g_icon_connected = NULL;
static volatile LONG g_client_count = 0;  /* active client connections */
static char g_client_ip[64]     = {0};  /* last connected client IP */
static DWORD g_connect_time     = 0;    /* GetTickCount at connect */
static CRITICAL_SECTION g_cs;           /* protects g_client_ip/g_connect_time */
static char g_password[9]       = {0};  /* VNC password (max 8 chars) */
static int  g_auth_enabled      = 0;
static const char *g_password_file = NULL;
static volatile DWORD g_overlay_linger_until = 0; /* GetTickCount deadline for auto-hide */

#define OVERLAY_LINGER_MS 30000  /* keep overlay visible 30s after last disconnect */

/* Forward declarations */
static void log_msg(const char *fmt, ...);
static void overlay_show(void);
static void overlay_hide(void);

/* ================================================================
 * VNC DES Authentication (RFC 6143 Section 7.2.2)
 *
 * DES-ECB implementation using pre-computed SP tables that merge
 * S-box substitution + P permutation into single lookups.
 * Based on the D3DES algorithm by Richard Outerbridge (public domain).
 * This is the same approach used by TightVNC, RealVNC, and libvncserver.
 * ================================================================ */

static const unsigned long d3des_SP1[64] = {
    0x01010400UL,0x00000000UL,0x00010000UL,0x01010404UL,0x01010004UL,0x00010404UL,0x00000004UL,0x00010000UL,
    0x00000400UL,0x01010400UL,0x01010404UL,0x00000400UL,0x01000404UL,0x01010004UL,0x01000000UL,0x00000004UL,
    0x00000404UL,0x01000400UL,0x01000400UL,0x00010400UL,0x00010400UL,0x01010000UL,0x01010000UL,0x01000404UL,
    0x00010004UL,0x01000004UL,0x01000004UL,0x00010004UL,0x00000000UL,0x00000404UL,0x00010404UL,0x01000000UL,
    0x00010000UL,0x01010404UL,0x00000004UL,0x01010000UL,0x01010400UL,0x01000000UL,0x01000000UL,0x00000400UL,
    0x01010004UL,0x00010000UL,0x00010400UL,0x01000004UL,0x00000400UL,0x00000004UL,0x01000404UL,0x00010404UL,
    0x01010404UL,0x00010004UL,0x01010000UL,0x01000404UL,0x01000004UL,0x00000404UL,0x00010404UL,0x01010400UL,
    0x00000404UL,0x01000400UL,0x01000400UL,0x00000000UL,0x00010004UL,0x00010400UL,0x00000000UL,0x01010004UL};
static const unsigned long d3des_SP2[64] = {
    0x80108020UL,0x80008000UL,0x00008000UL,0x00108020UL,0x00100000UL,0x00000020UL,0x80100020UL,0x80008020UL,
    0x80000020UL,0x80108020UL,0x80108000UL,0x80000000UL,0x80008000UL,0x00100000UL,0x00000020UL,0x80100020UL,
    0x00108000UL,0x00100020UL,0x80008020UL,0x00000000UL,0x80000000UL,0x00008000UL,0x00108020UL,0x80100000UL,
    0x00100020UL,0x80000020UL,0x00000000UL,0x00108000UL,0x00008020UL,0x80108000UL,0x80100000UL,0x00008020UL,
    0x00000000UL,0x00108020UL,0x80100020UL,0x00100000UL,0x80008020UL,0x80100000UL,0x80108000UL,0x00008000UL,
    0x80100000UL,0x80008000UL,0x00000020UL,0x80108020UL,0x00108020UL,0x00000020UL,0x00008000UL,0x80000000UL,
    0x00008020UL,0x80108000UL,0x00100000UL,0x80000020UL,0x00100020UL,0x80008020UL,0x80000020UL,0x00100020UL,
    0x00108000UL,0x00000000UL,0x80008000UL,0x00008020UL,0x80000000UL,0x80100020UL,0x80108020UL,0x00108000UL};
static const unsigned long d3des_SP3[64] = {
    0x00000208UL,0x08020200UL,0x00000000UL,0x08020008UL,0x08000200UL,0x00000000UL,0x00020208UL,0x08000200UL,
    0x00020008UL,0x08000008UL,0x08000008UL,0x00020000UL,0x08020208UL,0x00020008UL,0x08020000UL,0x00000208UL,
    0x08000000UL,0x00000008UL,0x08020200UL,0x00000200UL,0x00020200UL,0x08020000UL,0x08020008UL,0x00020208UL,
    0x08000208UL,0x00020200UL,0x00020000UL,0x08000208UL,0x00000008UL,0x08020208UL,0x00000200UL,0x08000000UL,
    0x08020200UL,0x08000000UL,0x00020008UL,0x00000208UL,0x00020000UL,0x08020200UL,0x08000200UL,0x00000000UL,
    0x00000200UL,0x00020008UL,0x08020208UL,0x08000200UL,0x08000008UL,0x00000200UL,0x00000000UL,0x08020008UL,
    0x08000208UL,0x00020000UL,0x08000000UL,0x08020208UL,0x00000008UL,0x00020208UL,0x00020200UL,0x08000008UL,
    0x08020000UL,0x08000208UL,0x00000208UL,0x08020000UL,0x00020208UL,0x00000008UL,0x08020008UL,0x00020200UL};
static const unsigned long d3des_SP4[64] = {
    0x00802001UL,0x00002081UL,0x00002081UL,0x00000080UL,0x00802080UL,0x00800081UL,0x00800001UL,0x00002001UL,
    0x00000000UL,0x00802000UL,0x00802000UL,0x00802081UL,0x00000081UL,0x00000000UL,0x00800080UL,0x00800001UL,
    0x00000001UL,0x00002000UL,0x00800000UL,0x00802001UL,0x00000080UL,0x00800000UL,0x00002001UL,0x00002080UL,
    0x00800081UL,0x00000001UL,0x00002080UL,0x00800080UL,0x00002000UL,0x00802080UL,0x00802081UL,0x00000081UL,
    0x00800080UL,0x00800001UL,0x00802000UL,0x00802081UL,0x00000081UL,0x00000000UL,0x00000000UL,0x00802000UL,
    0x00002080UL,0x00800080UL,0x00800081UL,0x00000001UL,0x00802001UL,0x00002081UL,0x00002081UL,0x00000080UL,
    0x00802081UL,0x00000081UL,0x00000001UL,0x00002000UL,0x00800001UL,0x00002001UL,0x00802080UL,0x00800081UL,
    0x00002001UL,0x00002080UL,0x00800000UL,0x00802001UL,0x00000080UL,0x00800000UL,0x00002000UL,0x00802080UL};
static const unsigned long d3des_SP5[64] = {
    0x00000100UL,0x02080100UL,0x02080000UL,0x42000100UL,0x00080000UL,0x00000100UL,0x40000000UL,0x02080000UL,
    0x40080100UL,0x00080000UL,0x02000100UL,0x40080100UL,0x42000100UL,0x42080000UL,0x00080100UL,0x40000000UL,
    0x02000000UL,0x40080000UL,0x40080000UL,0x00000000UL,0x40000100UL,0x42080100UL,0x42080100UL,0x02000100UL,
    0x42080000UL,0x40000100UL,0x00000000UL,0x42000000UL,0x02080100UL,0x02000000UL,0x42000000UL,0x00080100UL,
    0x00080000UL,0x42000100UL,0x00000100UL,0x02000000UL,0x40000000UL,0x02080000UL,0x42000100UL,0x40080100UL,
    0x02000100UL,0x40000000UL,0x42080000UL,0x02080100UL,0x40080100UL,0x00000100UL,0x02000000UL,0x42080000UL,
    0x42080100UL,0x00080100UL,0x42000000UL,0x42080100UL,0x02080000UL,0x00000000UL,0x40080000UL,0x42000000UL,
    0x00080100UL,0x02000100UL,0x40000100UL,0x00080000UL,0x00000000UL,0x40080000UL,0x02080100UL,0x40000100UL};
static const unsigned long d3des_SP6[64] = {
    0x20000010UL,0x20400000UL,0x00004000UL,0x20404010UL,0x20400000UL,0x00000010UL,0x20404010UL,0x00400000UL,
    0x20004000UL,0x00404010UL,0x00400000UL,0x20000010UL,0x00400010UL,0x20004000UL,0x20000000UL,0x00004010UL,
    0x00000000UL,0x00400010UL,0x20004010UL,0x00004000UL,0x00404000UL,0x20004010UL,0x00000010UL,0x20400010UL,
    0x20400010UL,0x00000000UL,0x00404010UL,0x20404000UL,0x00004010UL,0x00404000UL,0x20404000UL,0x20000000UL,
    0x20004000UL,0x00000010UL,0x20400010UL,0x00404000UL,0x20404010UL,0x00400000UL,0x00004010UL,0x20000010UL,
    0x00400000UL,0x20004000UL,0x20000000UL,0x00004010UL,0x20000010UL,0x20404010UL,0x00404000UL,0x20400000UL,
    0x00404010UL,0x20404000UL,0x00000000UL,0x20400010UL,0x00000010UL,0x00004000UL,0x20400000UL,0x00404010UL,
    0x00004000UL,0x00400010UL,0x20004010UL,0x00000000UL,0x20404000UL,0x20000000UL,0x00400010UL,0x20004010UL};
static const unsigned long d3des_SP7[64] = {
    0x00200000UL,0x04200002UL,0x04000802UL,0x00000000UL,0x00000800UL,0x04000802UL,0x00200802UL,0x04200800UL,
    0x04200802UL,0x00200000UL,0x00000000UL,0x04000002UL,0x00000002UL,0x04000000UL,0x04200002UL,0x00000802UL,
    0x04000800UL,0x00200802UL,0x00200002UL,0x04000800UL,0x04000002UL,0x04200000UL,0x04200800UL,0x00200002UL,
    0x04200000UL,0x00000800UL,0x00000802UL,0x04200802UL,0x00200800UL,0x00000002UL,0x04000000UL,0x00200800UL,
    0x04000000UL,0x00200800UL,0x00200000UL,0x04000802UL,0x04000802UL,0x04200002UL,0x04200002UL,0x00000002UL,
    0x00200002UL,0x04000000UL,0x04000800UL,0x00200000UL,0x04200800UL,0x00000802UL,0x00200802UL,0x04200800UL,
    0x00000802UL,0x04000002UL,0x04200802UL,0x04200000UL,0x00200800UL,0x00000000UL,0x00000002UL,0x04200802UL,
    0x00000000UL,0x00200802UL,0x04200000UL,0x00000800UL,0x04000002UL,0x04000800UL,0x00000800UL,0x00200002UL};
static const unsigned long d3des_SP8[64] = {
    0x10001040UL,0x00001000UL,0x00040000UL,0x10041040UL,0x10000000UL,0x10001040UL,0x00000040UL,0x10000000UL,
    0x00040040UL,0x10040000UL,0x10041040UL,0x00041000UL,0x10041000UL,0x00041040UL,0x00001000UL,0x00000040UL,
    0x10040000UL,0x10000040UL,0x10001000UL,0x00001040UL,0x00041000UL,0x00040040UL,0x10040040UL,0x10041000UL,
    0x00001040UL,0x00000000UL,0x00000000UL,0x10040040UL,0x10000040UL,0x10001000UL,0x00041040UL,0x00040000UL,
    0x00041040UL,0x00040000UL,0x10041000UL,0x00001000UL,0x00000040UL,0x10040040UL,0x00001000UL,0x00041040UL,
    0x10001000UL,0x00000040UL,0x10000040UL,0x10040000UL,0x10040040UL,0x10000000UL,0x00040000UL,0x10001040UL,
    0x00000000UL,0x10041040UL,0x00040040UL,0x10000040UL,0x10040000UL,0x10001000UL,0x10001040UL,0x00000000UL,
    0x10041040UL,0x00041000UL,0x00041000UL,0x00001040UL,0x00001040UL,0x00040040UL,0x10000000UL,0x10041000UL};

static const unsigned char d3des_pc1[56] = {
    56,48,40,32,24,16, 8, 0,57,49,41,33,25,17, 9, 1,58,50,42,34,26,18,
    10, 2,59,51,43,35,62,54,46,38,30,22,14, 6,61,53,45,37,29,21,13, 5,
    60,52,44,36,28,20,12, 4,27,19,11, 3 };
static const unsigned char d3des_totrot[16] = {1,2,4,6,8,10,12,14,15,17,19,21,23,25,27,28};
static const unsigned char d3des_pc2[48] = {
    13,16,10,23, 0, 4, 2,27,14, 5,20, 9,22,18,11, 3,25, 7,15, 6,26,19,12, 1,
    40,51,30,36,46,54,29,39,50,44,32,47,43,48,38,55,33,52,45,41,49,35,28,31 };

static unsigned long d3des_KnL[32];

static void d3des_cookey(unsigned long *raw1)
{
    unsigned long *cook, *raw0, dough[32];
    int i;
    cook = dough;
    for (i = 0; i < 16; i++, raw1++) {
        raw0 = raw1++;
        *cook    = (*raw0 & 0x00fc0000UL) << 6;
        *cook   |= (*raw0 & 0x00000fc0UL) << 10;
        *cook   |= (*raw1 & 0x00fc0000UL) >> 10;
        *cook++ |= (*raw1 & 0x00000fc0UL) >> 6;
        *cook    = (*raw0 & 0x0003f000UL) << 12;
        *cook   |= (*raw0 & 0x0000003fUL) << 16;
        *cook   |= (*raw1 & 0x0003f000UL) >> 4;
        *cook++ |= (*raw1 & 0x0000003fUL);
    }
    memcpy(d3des_KnL, dough, sizeof(dough));
}

static void d3des_deskey(const unsigned char *key, int edf)
{
    int i, j, l, m, n;
    unsigned char pc1m[56], pcr[56];
    unsigned long kn[32];

    for (j = 0; j < 56; j++) {
        l = d3des_pc1[j];
        m = l & 07;
        pc1m[j] = (unsigned char)((key[l >> 3] & (1 << m)) ? 1 : 0);
    }
    for (i = 0; i < 16; i++) {
        m = edf ? (15 - i) << 1 : i << 1;
        n = m + 1;
        kn[m] = kn[n] = 0UL;
        for (j = 0; j < 28; j++) {
            l = j + d3des_totrot[i];
            if (l < 28) pcr[j] = pc1m[l]; else pcr[j] = pc1m[l - 28];
        }
        for (j = 28; j < 56; j++) {
            l = j + d3des_totrot[i];
            if (l < 56) pcr[j] = pc1m[l]; else pcr[j] = pc1m[l - 28];
        }
        for (j = 0; j < 24; j++) {
            if (pcr[d3des_pc2[j]])      kn[m] |= 1UL << (23 - j);
            if (pcr[d3des_pc2[j + 24]]) kn[n] |= 1UL << (23 - j);
        }
    }
    d3des_cookey(kn);
}

static void d3des_desfunc(unsigned long *block)
{
    unsigned long fval, work, right, leftt;
    int round;
    unsigned long *keys = d3des_KnL;

    leftt = block[0];  right = block[1];

    work = ((leftt >> 4) ^ right) & 0x0f0f0f0fUL;
    right ^= work;  leftt ^= (work << 4);
    work = ((leftt >> 16) ^ right) & 0x0000ffffUL;
    right ^= work;  leftt ^= (work << 16);
    work = ((right >> 2) ^ leftt) & 0x33333333UL;
    leftt ^= work;  right ^= (work << 2);
    work = ((right >> 8) ^ leftt) & 0x00ff00ffUL;
    leftt ^= work;  right ^= (work << 8);
    right = ((right << 1) | ((right >> 31) & 1UL)) & 0xffffffffUL;
    work = (leftt ^ right) & 0xaaaaaaaaUL;
    leftt ^= work;  right ^= work;
    leftt = ((leftt << 1) | ((leftt >> 31) & 1UL)) & 0xffffffffUL;

    for (round = 0; round < 8; round++) {
        work  = (right << 28) | (right >> 4);
        work ^= *keys++;
        fval  = d3des_SP7[ work        & 0x3fUL];
        fval |= d3des_SP5[(work >>  8) & 0x3fUL];
        fval |= d3des_SP3[(work >> 16) & 0x3fUL];
        fval |= d3des_SP1[(work >> 24) & 0x3fUL];
        work  = right ^ *keys++;
        fval |= d3des_SP8[ work        & 0x3fUL];
        fval |= d3des_SP6[(work >>  8) & 0x3fUL];
        fval |= d3des_SP4[(work >> 16) & 0x3fUL];
        fval |= d3des_SP2[(work >> 24) & 0x3fUL];
        leftt ^= fval;

        work  = (leftt << 28) | (leftt >> 4);
        work ^= *keys++;
        fval  = d3des_SP7[ work        & 0x3fUL];
        fval |= d3des_SP5[(work >>  8) & 0x3fUL];
        fval |= d3des_SP3[(work >> 16) & 0x3fUL];
        fval |= d3des_SP1[(work >> 24) & 0x3fUL];
        work  = leftt ^ *keys++;
        fval |= d3des_SP8[ work        & 0x3fUL];
        fval |= d3des_SP6[(work >>  8) & 0x3fUL];
        fval |= d3des_SP4[(work >> 16) & 0x3fUL];
        fval |= d3des_SP2[(work >> 24) & 0x3fUL];
        right ^= fval;
    }

    right = (right << 31) | (right >> 1);
    work = (leftt ^ right) & 0xaaaaaaaaUL;
    leftt ^= work;  right ^= work;
    leftt = (leftt << 31) | (leftt >> 1);
    work = ((leftt >> 8) ^ right) & 0x00ff00ffUL;
    right ^= work;  leftt ^= (work << 8);
    work = ((leftt >> 2) ^ right) & 0x33333333UL;
    right ^= work;  leftt ^= (work << 2);
    work = ((right >> 16) ^ leftt) & 0x0000ffffUL;
    leftt ^= work;  right ^= (work << 16);
    work = ((right >> 4) ^ leftt) & 0x0f0f0f0fUL;
    leftt ^= work;  right ^= (work << 4);

    block[0] = right;  block[1] = leftt;
}

/* DES ECB encrypt one 8-byte block */
static void des_ecb_encrypt(const unsigned char key[8],
                            const unsigned char in[8],
                            unsigned char out[8])
{
    unsigned long work[2];
    d3des_deskey(key, 0);  /* 0 = encrypt */
    work[0] = ((unsigned long)in[0] << 24) | ((unsigned long)in[1] << 16) |
              ((unsigned long)in[2] << 8)  |  (unsigned long)in[3];
    work[1] = ((unsigned long)in[4] << 24) | ((unsigned long)in[5] << 16) |
              ((unsigned long)in[6] << 8)  |  (unsigned long)in[7];
    d3des_desfunc(work);
    out[0] = (unsigned char)(work[0] >> 24); out[1] = (unsigned char)(work[0] >> 16);
    out[2] = (unsigned char)(work[0] >> 8);  out[3] = (unsigned char)(work[0]);
    out[4] = (unsigned char)(work[1] >> 24); out[5] = (unsigned char)(work[1] >> 16);
    out[6] = (unsigned char)(work[1] >> 8);  out[7] = (unsigned char)(work[1]);
}

/* DES ECB decrypt one 8-byte block */
static void des_ecb_decrypt(const unsigned char key[8],
                            const unsigned char in[8],
                            unsigned char out[8])
{
    unsigned long work[2];
    d3des_deskey(key, 1);  /* 1 = decrypt */
    work[0] = ((unsigned long)in[0] << 24) | ((unsigned long)in[1] << 16) |
              ((unsigned long)in[2] << 8)  |  (unsigned long)in[3];
    work[1] = ((unsigned long)in[4] << 24) | ((unsigned long)in[5] << 16) |
              ((unsigned long)in[6] << 8)  |  (unsigned long)in[7];
    d3des_desfunc(work);
    out[0] = (unsigned char)(work[0] >> 24); out[1] = (unsigned char)(work[0] >> 16);
    out[2] = (unsigned char)(work[0] >> 8);  out[3] = (unsigned char)(work[0]);
    out[4] = (unsigned char)(work[1] >> 24); out[5] = (unsigned char)(work[1] >> 16);
    out[6] = (unsigned char)(work[1] >> 8);  out[7] = (unsigned char)(work[1]);
}

/* Reverse bits in a byte (VNC DES key transformation) */
static unsigned char vnc_reverse_bits(unsigned char b)
{
    unsigned char x = b;
    x = ((x & 0x55) << 1) | ((x & 0xAA) >> 1);
    x = ((x & 0x33) << 2) | ((x & 0xCC) >> 2);
    x = ((x & 0x0F) << 4) | ((x & 0xF0) >> 4);
    return x;
}

/* VNC DES encrypt: d3des uses LSB-first convention internally,
 * which matches VNC's bit-reversed DES. Pass raw password bytes. */
static void vnc_auth_encrypt(const unsigned char challenge[16],
                             const char *password,
                             unsigned char response[16])
{
    unsigned char key[8] = {0};
    int len = (int)strlen(password);
    if (len > 8) len = 8;
    for (int i = 0; i < len; i++)
        key[i] = (unsigned char)password[i];

    des_ecb_encrypt(key, challenge,     response);
    des_ecb_encrypt(key, challenge + 8, response + 8);
}

/* Well-known VNC password obfuscation key (before bit-reversal) */
static const unsigned char VNC_OBFUSCATION_KEY[8] = {
    23, 82, 107, 6, 35, 78, 88, 7
};

/* De-obfuscate a VNC password stored in Windows registry.
 * stored_pw is 8 bytes DES-encrypted with the well-known key.
 * Returns the plaintext password (up to 8 chars, null-terminated). */
static void vnc_deobfuscate_password(const unsigned char stored_pw[8],
                                     char *plaintext, int max_len)
{
    /* d3des uses LSB-first convention — pass raw key, no bit reversal */
    unsigned char decrypted[8];
    des_ecb_decrypt(VNC_OBFUSCATION_KEY, stored_pw, decrypted);

    int len = 0;
    for (int i = 0; i < 8 && i < max_len - 1; i++) {
        if (decrypted[i] == 0) break;
        plaintext[len++] = (char)decrypted[i];
    }
    plaintext[len] = '\0';

    SecureZeroMemory(decrypted, sizeof(decrypted));
}

/* Read VNC password from Windows registry (TightVNC, RealVNC, etc.)
 * Returns 1 if password was found, 0 otherwise. */
static int read_vnc_password_registry(char *password, int max_len)
{
    /* Registry locations to try, in order of preference */
    static const struct {
        HKEY    root;
        const char *subkey;
        const char *value;
    } locations[] = {
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\TightVNC\\Server",         "Password"    },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\TightVNC\\Server",         "PasswordViewOnly" },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\RealVNC\\WinVNC4",         "Password"    },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\RealVNC\\vncserver",       "Password"    },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\TigerVNC\\WinVNC4",        "Password"    },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\ORL\\WinVNC3\\Default",    "Password"    },
        { HKEY_LOCAL_MACHINE, "SOFTWARE\\ORL\\WinVNC3",             "Password"    },
        { HKEY_CURRENT_USER,  "SOFTWARE\\TightVNC\\Server",         "Password"    },
        { HKEY_CURRENT_USER,  "SOFTWARE\\ORL\\WinVNC3",             "Password"    },
    };

    for (int i = 0; i < (int)(sizeof(locations) / sizeof(locations[0])); i++) {
        HKEY hkey;
        if (RegOpenKeyExA(locations[i].root, locations[i].subkey,
                          0, KEY_READ, &hkey) != ERROR_SUCCESS)
            continue;

        unsigned char data[8] = {0};
        DWORD data_len = sizeof(data);
        DWORD type = 0;
        LONG result = RegQueryValueExA(hkey, locations[i].value, NULL,
                                        &type, data, &data_len);
        RegCloseKey(hkey);

        if (result == ERROR_SUCCESS && type == REG_BINARY && data_len >= 8) {
            vnc_deobfuscate_password(data, password, max_len);
            SecureZeroMemory(data, sizeof(data));
            if (password[0] != '\0') {
                log_msg("VNC password found in registry: %s\\%s",
                        locations[i].subkey, locations[i].value);
                return 1;
            }
        }
        SecureZeroMemory(data, sizeof(data));
    }

    return 0;
}

/* Read VNC password from a plaintext file (first line, trimmed) */
static int read_vnc_password_file(const char *path, char *password, int max_len)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[256] = {0};
    if (fgets(buf, sizeof(buf), f) == NULL) {
        fclose(f);
        return 0;
    }
    fclose(f);

    /* Trim trailing whitespace */
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' ||
                       buf[len-1] == ' '  || buf[len-1] == '\t'))
        buf[--len] = '\0';

    if (len == 0) return 0;
    if (len > max_len - 1) len = max_len - 1;
    memcpy(password, buf, len);
    password[len] = '\0';

    SecureZeroMemory(buf, sizeof(buf));
    return 1;
}

/* Initialize authentication: try password file first, then registry */
static void init_auth(void)
{
    if (g_password_file) {
        if (read_vnc_password_file(g_password_file, g_password, sizeof(g_password))) {
            g_auth_enabled = 1;
            log_msg("auth enabled (password file: %s)", g_password_file);
            return;
        }
        log_msg("WARNING: could not read password from %s", g_password_file);
    }

    if (read_vnc_password_registry(g_password, sizeof(g_password))) {
        g_auth_enabled = 1;
        log_msg("auth enabled (registry)");
        return;
    }

    log_msg("WARNING: no VNC password found — running WITHOUT authentication");
    log_msg("  Use -password-file <path> or ensure TightVNC/RealVNC is installed");
}

/* Perform VNC DES challenge-response authentication with a client.
 * Returns 1 on success, 0 on failure. */
static int auth_client(SOCKET sock)
{
    if (!g_auth_enabled) return 1;  /* no auth configured */

    /* Generate 16-byte random challenge */
    unsigned char challenge[16];
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT)) {
        log_msg("CryptAcquireContext failed — rejecting client");
        return 0;
    }
    CryptGenRandom(hProv, 16, challenge);
    CryptReleaseContext(hProv, 0);

    /* Send challenge */
    if (send(sock, (const char *)challenge, 16, 0) != 16) {
        log_msg("failed to send auth challenge");
        return 0;
    }

    /* Receive 16-byte response */
    unsigned char client_response[16];
    int total = 0;
    while (total < 16) {
        int n = recv(sock, (char *)client_response + total, 16 - total, 0);
        if (n <= 0) {
            log_msg("failed to receive auth response");
            return 0;
        }
        total += n;
    }

    /* Compute expected response */
    unsigned char expected[16];
    vnc_auth_encrypt(challenge, g_password, expected);

    /* Constant-time comparison */
    int diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= client_response[i] ^ expected[i];

    SecureZeroMemory(expected, sizeof(expected));
    SecureZeroMemory(challenge, sizeof(challenge));

    if (diff != 0) {
        /* Send 4-byte failure result (like RFB SecurityResult) */
        unsigned char fail[4] = {0, 0, 0, 1};  /* big-endian 1 = failed */
        send(sock, (const char *)fail, 4, 0);
        log_msg("auth FAILED — wrong password");
        return 0;
    }

    /* Send 4-byte success result */
    unsigned char ok[4] = {0, 0, 0, 0};  /* big-endian 0 = OK */
    send(sock, (const char *)ok, 4, 0);
    log_msg("auth OK");
    return 1;
}

/* ================================================================
 * JSON Helpers
 * ================================================================ */

/* Escape a C string for safe embedding inside a JSON string value. */
static int json_escape(const char *in, char *out, int out_max)
{
    int j = 0;
    for (int i = 0; in[i] && j < out_max - 7; i++) {
        unsigned char c = (unsigned char)in[i];
        if      (c == '"')  { out[j++] = '\\'; out[j++] = '"';  }
        else if (c == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n';  }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r';  }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't';  }
        else if (c < 0x20)  { j += snprintf(out + j, out_max - j, "\\u%04x", c); }
        else                { out[j++] = (char)c; }
    }
    out[j] = '\0';
    return j;
}

/* Extract a string value for a given key from flat JSON.
 * Handles basic escape sequences in the value. */
static int json_get_string(const char *json, const char *key,
                           char *out, int out_max)
{
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);

    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;

    int i = 0;
    while (*p && i < out_max - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
            case '"':  out[i++] = '"';  break;
            case '\\': out[i++] = '\\'; break;
            case 'n':  out[i++] = '\n'; break;
            case 'r':  out[i++] = '\r'; break;
            case 't':  out[i++] = '\t'; break;
            case '/':  out[i++] = '/';  break;
            default:   out[i++] = *p;   break;
            }
        } else if (*p == '"') {
            break;
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 1;
}

/* Extract an integer value for a given key from flat JSON. */
static int json_get_int(const char *json, const char *key, int *out)
{
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '"') return 0;   /* it's a string, not a number */
    *out = atoi(p);
    return 1;
}

/* ================================================================
 * Logging
 * ================================================================ */

static void log_msg(const char *fmt, ...)
{
    if (!g_console) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[vnc-helper] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

/* ================================================================
 * Network Helpers
 * ================================================================ */

static void send_line(SOCKET sock, const char *json)
{
    send(sock, json, (int)strlen(json), 0);
    send(sock, "\n", 1, 0);
}

static void send_error(SOCKET sock, const char *msg)
{
    char buf[1024];
    char escaped[512];
    json_escape(msg, escaped, sizeof(escaped));
    snprintf(buf, sizeof(buf),
             "{\"status\":\"error\",\"message\":\"%s\"}", escaped);
    send_line(sock, buf);
}

/* ================================================================
 * Command: cursor_position
 * ================================================================ */

static void cmd_cursor_position(SOCKET sock)
{
    POINT pt;
    GetCursorPos(&pt);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"x\":%d,\"y\":%d}}",
             (int)pt.x, (int)pt.y);
    send_line(sock, buf);
}

/* ================================================================
 * Command: window_list
 * ================================================================ */

typedef struct {
    char *buf;
    int   capacity;
    int   offset;
    int   count;
    HWND  fg_hwnd;
} WinEnumCtx;

static BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lparam)
{
    WinEnumCtx *ctx = (WinEnumCtx *)lparam;

    if (!IsWindowVisible(hwnd)) return TRUE;

    char title[512] = {0};
    GetWindowTextA(hwnd, title, sizeof(title));
    if (title[0] == '\0') return TRUE;

    char classname[256] = {0};
    GetClassNameA(hwnd, classname, sizeof(classname));

    RECT rect;
    GetWindowRect(hwnd, &rect);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    char title_esc[1024], class_esc[512];
    json_escape(title, title_esc, sizeof(title_esc));
    json_escape(classname, class_esc, sizeof(class_esc));

    if (ctx->count > 0 && ctx->offset < ctx->capacity - 1)
        ctx->buf[ctx->offset++] = ',';

    ctx->offset += snprintf(
        ctx->buf + ctx->offset, ctx->capacity - ctx->offset,
        "{\"title\":\"%s\",\"class\":\"%s\","
        "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
        "\"pid\":%lu,\"foreground\":%s}",
        title_esc, class_esc,
        (int)rect.left, (int)rect.top,
        (int)(rect.right - rect.left), (int)(rect.bottom - rect.top),
        (unsigned long)pid,
        (hwnd == ctx->fg_hwnd) ? "true" : "false");

    ctx->count++;
    return TRUE;
}

static void cmd_window_list(SOCKET sock)
{
    char *buf = (char *)malloc(MAX_RESPONSE);
    if (!buf) { send_error(sock, "Out of memory"); return; }

    int offset = snprintf(buf, MAX_RESPONSE,
                          "{\"status\":\"ok\",\"data\":{\"windows\":[");

    WinEnumCtx ctx;
    ctx.buf      = buf;
    ctx.capacity = MAX_RESPONSE - 16;
    ctx.offset   = offset;
    ctx.count    = 0;
    ctx.fg_hwnd  = GetForegroundWindow();

    EnumWindows(enum_windows_cb, (LPARAM)&ctx);
    snprintf(buf + ctx.offset, MAX_RESPONSE - ctx.offset, "]}}");

    send_line(sock, buf);
    free(buf);
}

/* ================================================================
 * Command: active_window
 * ================================================================ */

static void cmd_active_window(SOCKET sock)
{
    HWND  hwnd = GetForegroundWindow();
    char  title[512] = {0};
    char  classname[256] = {0};
    RECT  rect = {0, 0, 0, 0};
    DWORD pid = 0;

    if (hwnd) {
        GetWindowTextA(hwnd, title, sizeof(title));
        GetClassNameA(hwnd, classname, sizeof(classname));
        GetWindowRect(hwnd, &rect);
        GetWindowThreadProcessId(hwnd, &pid);
    }

    char title_esc[1024], class_esc[512];
    json_escape(title, title_esc, sizeof(title_esc));
    json_escape(classname, class_esc, sizeof(class_esc));

    char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"title\":\"%s\",\"class\":\"%s\","
             "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"pid\":%lu}}",
             title_esc, class_esc,
             (int)rect.left, (int)rect.top,
             (int)(rect.right - rect.left), (int)(rect.bottom - rect.top),
             (unsigned long)pid);
    send_line(sock, buf);
}

/* ================================================================
 * Command: run_command
 * ================================================================ */

static void cmd_run_command(SOCKET sock, const char *json)
{
    char command[4096] = {0};
    int  timeout_ms = 30000;

    if (!json_get_string(json, "cmd", command, sizeof(command))) {
        send_error(sock, "Missing 'cmd' parameter");
        return;
    }
    json_get_int(json, "timeout", &timeout_ms);
    if (timeout_ms < 1000)   timeout_ms = 1000;
    if (timeout_ms > 300000) timeout_ms = 300000;

    log_msg("run_command: %s (timeout=%d)", command, timeout_ms);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE stdout_rd, stdout_wr, stderr_rd, stderr_wr;
    if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0) ||
        !CreatePipe(&stderr_rd, &stderr_wr, &sa, 0)) {
        send_error(sock, "Failed to create pipes");
        return;
    }

    SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_wr;
    si.hStdError  = stderr_wr;
    si.hStdInput  = NULL;

    char cmd_line[8192];
    snprintf(cmd_line, sizeof(cmd_line), "cmd.exe /c %s", command);

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        send_error(sock, "Failed to create process");
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stderr_rd); CloseHandle(stderr_wr);
        return;
    }

    /* Close write ends — child has its own handles */
    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    DWORD wait_result = WaitForSingleObject(pi.hProcess, (DWORD)timeout_ms);
    int   timed_out   = (wait_result == WAIT_TIMEOUT);

    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    /* Read captured output.
     * Use PeekNamedPipe to avoid blocking — child processes spawned by
     * cmd.exe (e.g. "start notepad.exe") may inherit pipe handles,
     * keeping the pipe open after cmd.exe itself exits. */
    Sleep(100);  /* let output buffer before peeking */

    char *stdout_buf = (char *)malloc(MAX_CMD_OUTPUT);
    char *stderr_buf = (char *)malloc(MAX_CMD_OUTPUT);
    DWORD bytes_read, bytes_avail;
    int stdout_len = 0, stderr_len = 0;

    while (stdout_len < MAX_CMD_OUTPUT - 1) {
        if (!PeekNamedPipe(stdout_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_CMD_OUTPUT - stdout_len - 1))
            to_read = (DWORD)(MAX_CMD_OUTPUT - stdout_len - 1);
        if (!ReadFile(stdout_rd, stdout_buf + stdout_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stdout_len += (int)bytes_read;
    }
    stdout_buf[stdout_len] = '\0';

    while (stderr_len < MAX_CMD_OUTPUT - 1) {
        if (!PeekNamedPipe(stderr_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_CMD_OUTPUT - stderr_len - 1))
            to_read = (DWORD)(MAX_CMD_OUTPUT - stderr_len - 1);
        if (!ReadFile(stderr_rd, stderr_buf + stderr_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stderr_len += (int)bytes_read;
    }
    stderr_buf[stderr_len] = '\0';

    CloseHandle(stdout_rd);
    CloseHandle(stderr_rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /* Build response */
    char *out_esc = (char *)malloc(stdout_len * 6 + 1);
    char *err_esc = (char *)malloc(stderr_len * 6 + 1);
    json_escape(stdout_buf, out_esc, stdout_len * 6 + 1);
    json_escape(stderr_buf, err_esc, stderr_len * 6 + 1);

    char *response = (char *)malloc(MAX_RESPONSE);
    if (timed_out) {
        snprintf(response, MAX_RESPONSE,
                 "{\"status\":\"ok\",\"data\":{"
                 "\"stdout\":\"%s\",\"stderr\":\"%s\","
                 "\"exit_code\":%lu,\"timed_out\":true}}",
                 out_esc, err_esc, (unsigned long)exit_code);
    } else {
        snprintf(response, MAX_RESPONSE,
                 "{\"status\":\"ok\",\"data\":{"
                 "\"stdout\":\"%s\",\"stderr\":\"%s\","
                 "\"exit_code\":%lu}}",
                 out_esc, err_esc, (unsigned long)exit_code);
    }

    send_line(sock, response);

    free(stdout_buf); free(stderr_buf);
    free(out_esc);    free(err_esc);
    free(response);
}

/* ================================================================
 * Command: screen_info
 * ================================================================ */

typedef struct {
    char *buf;
    int   capacity;
    int   offset;
    int   count;
} MonitorCtx;

static BOOL CALLBACK enum_monitors_cb(HMONITOR hmon, HDC hdc,
                                      LPRECT lprc, LPARAM lparam)
{
    (void)hdc; (void)lprc;
    MonitorCtx *ctx = (MonitorCtx *)lparam;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoA(hmon, &mi);

    int primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;

    if (ctx->count > 0 && ctx->offset < ctx->capacity - 1)
        ctx->buf[ctx->offset++] = ',';

    ctx->offset += snprintf(
        ctx->buf + ctx->offset, ctx->capacity - ctx->offset,
        "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"primary\":%s}",
        (int)mi.rcMonitor.left, (int)mi.rcMonitor.top,
        (int)(mi.rcMonitor.right - mi.rcMonitor.left),
        (int)(mi.rcMonitor.bottom - mi.rcMonitor.top),
        primary ? "true" : "false");

    ctx->count++;
    return TRUE;
}

static void cmd_screen_info(SOCKET sock)
{
    HDC hdc = GetDC(NULL);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(NULL, hdc);

    char monitors_buf[4096];
    MonitorCtx ctx;
    ctx.buf      = monitors_buf;
    ctx.capacity = (int)sizeof(monitors_buf) - 1;
    ctx.offset   = 0;
    ctx.count    = 0;

    EnumDisplayMonitors(NULL, NULL, enum_monitors_cb, (LPARAM)&ctx);
    monitors_buf[ctx.offset] = '\0';

    char response[8192];
    snprintf(response, sizeof(response),
             "{\"status\":\"ok\",\"data\":{\"monitors\":[%s],\"dpi\":%d}}",
             monitors_buf, dpi);
    send_line(sock, response);
}

/* ================================================================
 * Command: file_upload
 * ================================================================ */

/* Minimal base64 decode — returns malloc'd buffer, sets *out_len */
static unsigned char *b64_decode(const char *in, int in_len, int *out_len)
{
    static const unsigned char T[256] = {
        ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
        ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
        ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
        ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
        ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63
    };

    int alloc_len = (in_len / 4) * 3 + 4;
    unsigned char *out = (unsigned char *)malloc(alloc_len);
    if (!out) return NULL;

    int j = 0;
    for (int i = 0; i < in_len; ) {
        unsigned int a = (i < in_len && in[i] != '=') ? T[(unsigned char)in[i]] : 0; i++;
        unsigned int b = (i < in_len && in[i] != '=') ? T[(unsigned char)in[i]] : 0; i++;
        unsigned int c = (i < in_len && in[i] != '=') ? T[(unsigned char)in[i]] : 0; i++;
        unsigned int d = (i < in_len && in[i] != '=') ? T[(unsigned char)in[i]] : 0; i++;
        unsigned int triple = (a << 18) | (b << 12) | (c << 6) | d;
        out[j++] = (unsigned char)((triple >> 16) & 0xFF);
        out[j++] = (unsigned char)((triple >> 8) & 0xFF);
        out[j++] = (unsigned char)(triple & 0xFF);
    }
    /* Adjust for padding */
    if (in_len > 0 && in[in_len - 1] == '=') j--;
    if (in_len > 1 && in[in_len - 2] == '=') j--;
    *out_len = j;
    return out;
}

static void cmd_file_upload(SOCKET sock, const char *json)
{
    char path[MAX_PATH] = {0};
    char content[MAX_REQUEST] = {0};

    if (!json_get_string(json, "path", path, sizeof(path))) {
        send_error(sock, "Missing 'path' parameter");
        return;
    }
    if (!json_get_string(json, "content", content, sizeof(content))) {
        send_error(sock, "Missing 'content' parameter (base64)");
        return;
    }

    log_msg("file_upload: %s (%d bytes b64)", path, (int)strlen(content));

    int decoded_len = 0;
    unsigned char *decoded = b64_decode(content, (int)strlen(content), &decoded_len);
    if (!decoded) {
        send_error(sock, "Base64 decode failed");
        return;
    }

    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to create file: %s (error %lu)",
                 path, (unsigned long)GetLastError());
        send_error(sock, msg);
        free(decoded);
        return;
    }

    DWORD written;
    WriteFile(hFile, decoded, (DWORD)decoded_len, &written, NULL);
    CloseHandle(hFile);
    free(decoded);

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"path\":\"%s\",\"bytes\":%d}}",
             path, decoded_len);
    send_line(sock, buf);
}

/* ================================================================
 * Command: file_download
 * ================================================================ */

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const unsigned char *in, int in_len, int *out_len)
{
    int alloc = ((in_len + 2) / 3) * 4 + 1;
    char *out = (char *)malloc(alloc);
    if (!out) return NULL;

    int j = 0;
    for (int i = 0; i < in_len; i += 3) {
        unsigned int a = in[i];
        unsigned int b = (i + 1 < in_len) ? in[i + 1] : 0;
        unsigned int c = (i + 2 < in_len) ? in[i + 2] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;

        out[j++] = B64_CHARS[(triple >> 18) & 0x3F];
        out[j++] = B64_CHARS[(triple >> 12) & 0x3F];
        out[j++] = (i + 1 < in_len) ? B64_CHARS[(triple >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < in_len) ? B64_CHARS[triple & 0x3F] : '=';
    }
    out[j] = '\0';
    *out_len = j;
    return out;
}

static void cmd_file_download(SOCKET sock, const char *json)
{
    char path[MAX_PATH] = {0};

    if (!json_get_string(json, "path", path, sizeof(path))) {
        send_error(sock, "Missing 'path' parameter");
        return;
    }

    log_msg("file_download: %s", path);

    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Failed to open file: %s (error %lu)",
                 path, (unsigned long)GetLastError());
        send_error(sock, msg);
        return;
    }

    DWORD file_size = GetFileSize(hFile, NULL);
    if (file_size > 10 * 1024 * 1024) { /* 10MB limit */
        send_error(sock, "File too large (max 10MB)");
        CloseHandle(hFile);
        return;
    }

    unsigned char *file_buf = (unsigned char *)malloc(file_size);
    DWORD bytes_read;
    ReadFile(hFile, file_buf, file_size, &bytes_read, NULL);
    CloseHandle(hFile);

    int b64_len = 0;
    char *b64 = b64_encode(file_buf, (int)bytes_read, &b64_len);
    free(file_buf);

    if (!b64) {
        send_error(sock, "Base64 encode failed");
        return;
    }

    /* Build response — path needs escaping for JSON */
    char path_esc[MAX_PATH * 2];
    json_escape(path, path_esc, sizeof(path_esc));

    int resp_len = b64_len + 256;
    char *response = (char *)malloc(resp_len);
    snprintf(response, resp_len,
             "{\"status\":\"ok\",\"data\":{\"path\":\"%s\",\"bytes\":%lu,\"content\":\"%s\"}}",
             path_esc, (unsigned long)bytes_read, b64);
    send_line(sock, response);

    free(b64);
    free(response);
}

/* ================================================================
 * Command: ocr_region
 *
 * Thin wrapper around vnc-ocr.ps1 (deployed alongside this exe).
 * Captures a screen region and returns OCR-recognized text.
 * ================================================================ */

static void cmd_ocr_region(SOCKET sock, const char *json)
{
    int x = 0, y = 0, w = 0, h = 0;
    char lang[64] = {0};

    if (!json_get_int(json, "x", &x) || !json_get_int(json, "y", &y) ||
        !json_get_int(json, "w", &w) || !json_get_int(json, "h", &h)) {
        send_error(sock, "Missing x, y, w, h parameters");
        return;
    }

    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        send_error(sock, "Invalid region dimensions (1-4096)");
        return;
    }

    json_get_string(json, "lang", lang, sizeof(lang));

    log_msg("ocr_region: x=%d y=%d w=%d h=%d lang=%s", x, y, w, h,
            lang[0] ? lang : "(default)");

    /* Locate vnc-ocr.ps1 next to our exe */
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);

    /* Strip filename to get directory */
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    char ps1_path[MAX_PATH];
    snprintf(ps1_path, sizeof(ps1_path), "%svnc-ocr.ps1", exe_path);

    /* Check script exists */
    DWORD attrs = GetFileAttributesA(ps1_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        send_error(sock, "vnc-ocr.ps1 not found next to vnc-helper.exe");
        return;
    }

    /* Build PowerShell command line */
    char cmd_line[4096];
    if (lang[0]) {
        snprintf(cmd_line, sizeof(cmd_line),
                 "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                 "-File \"%s\" -X %d -Y %d -W %d -H %d -Lang \"%s\"",
                 ps1_path, x, y, w, h, lang);
    } else {
        snprintf(cmd_line, sizeof(cmd_line),
                 "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                 "-File \"%s\" -X %d -Y %d -W %d -H %d",
                 ps1_path, x, y, w, h);
    }

    /* Execute with captured output */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE stdout_rd, stdout_wr, stderr_rd, stderr_wr;
    if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0) ||
        !CreatePipe(&stderr_rd, &stderr_wr, &sa, 0)) {
        send_error(sock, "Failed to create pipes");
        return;
    }

    SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_wr;
    si.hStdError  = stderr_wr;
    si.hStdInput  = NULL;

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to start PowerShell (error %lu)",
                 (unsigned long)GetLastError());
        send_error(sock, msg);
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stderr_rd); CloseHandle(stderr_wr);
        return;
    }

    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    /* 30 second timeout for OCR (PS startup + WinRT init can be slow) */
    DWORD wait_result = WaitForSingleObject(pi.hProcess, 30000);
    int timed_out = (wait_result == WAIT_TIMEOUT);

    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    Sleep(100);

    /* Read stdout (OCR text) */
    char *stdout_buf = (char *)malloc(MAX_CMD_OUTPUT);
    char *stderr_buf = (char *)malloc(MAX_CMD_OUTPUT);
    DWORD bytes_read, bytes_avail;
    int stdout_len = 0, stderr_len = 0;

    while (stdout_len < MAX_CMD_OUTPUT - 1) {
        if (!PeekNamedPipe(stdout_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_CMD_OUTPUT - stdout_len - 1))
            to_read = (DWORD)(MAX_CMD_OUTPUT - stdout_len - 1);
        if (!ReadFile(stdout_rd, stdout_buf + stdout_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stdout_len += (int)bytes_read;
    }
    stdout_buf[stdout_len] = '\0';

    while (stderr_len < MAX_CMD_OUTPUT - 1) {
        if (!PeekNamedPipe(stderr_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_CMD_OUTPUT - stderr_len - 1))
            to_read = (DWORD)(MAX_CMD_OUTPUT - stderr_len - 1);
        if (!ReadFile(stderr_rd, stderr_buf + stderr_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stderr_len += (int)bytes_read;
    }
    stderr_buf[stderr_len] = '\0';

    CloseHandle(stdout_rd);
    CloseHandle(stderr_rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (timed_out) {
        send_error(sock, "OCR timed out (30s)");
        free(stdout_buf); free(stderr_buf);
        return;
    }

    if (exit_code != 0) {
        char *err_esc = (char *)malloc(stderr_len * 6 + 128);
        json_escape(stderr_buf, err_esc, stderr_len * 6 + 128);
        char *response = (char *)malloc(MAX_RESPONSE);
        snprintf(response, MAX_RESPONSE,
                 "{\"status\":\"error\",\"message\":\"OCR failed (exit %lu): %s\"}",
                 (unsigned long)exit_code, err_esc);
        send_line(sock, response);
        free(err_esc); free(response);
        free(stdout_buf); free(stderr_buf);
        return;
    }

    /* Build success response with recognized text */
    char *text_esc = (char *)malloc(stdout_len * 6 + 1);
    json_escape(stdout_buf, text_esc, stdout_len * 6 + 1);

    char *response = (char *)malloc(MAX_RESPONSE);
    snprintf(response, MAX_RESPONSE,
             "{\"status\":\"ok\",\"data\":{\"text\":\"%s\"}}",
             text_esc);
    send_line(sock, response);

    free(text_esc); free(response);
    free(stdout_buf); free(stderr_buf);
}

/* ================================================================
 * Command: ui_tree
 *
 * Wrapper around vnc-uia.ps1 -Mode tree. Returns the accessibility
 * tree of the foreground window (or a specific PID).
 * ================================================================ */

static void cmd_ui_tree(SOCKET sock, const char *json)
{
    int depth = 3;
    int pid = 0;

    json_get_int(json, "depth", &depth);
    json_get_int(json, "pid", &pid);

    if (depth < 1)  depth = 1;
    if (depth > 10) depth = 10;

    log_msg("ui_tree: depth=%d pid=%d", depth, pid);

    /* Locate vnc-uia.ps1 next to our exe */
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    char ps1_path[MAX_PATH];
    snprintf(ps1_path, sizeof(ps1_path), "%svnc-uia.ps1", exe_path);

    DWORD attrs = GetFileAttributesA(ps1_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        send_error(sock, "vnc-uia.ps1 not found next to vnc-helper.exe");
        return;
    }

    /* Build PowerShell command line */
    char cmd_line[4096];
    if (pid > 0) {
        snprintf(cmd_line, sizeof(cmd_line),
                 "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                 "-File \"%s\" -Mode tree -Depth %d -Pid %d",
                 ps1_path, depth, pid);
    } else {
        snprintf(cmd_line, sizeof(cmd_line),
                 "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                 "-File \"%s\" -Mode tree -Depth %d",
                 ps1_path, depth);
    }

    /* Execute with captured output */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE stdout_rd, stdout_wr, stderr_rd, stderr_wr;
    if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0) ||
        !CreatePipe(&stderr_rd, &stderr_wr, &sa, 0)) {
        send_error(sock, "Failed to create pipes");
        return;
    }

    SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_wr;
    si.hStdError  = stderr_wr;
    si.hStdInput  = NULL;

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to start PowerShell (error %lu)",
                 (unsigned long)GetLastError());
        send_error(sock, msg);
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stderr_rd); CloseHandle(stderr_wr);
        return;
    }

    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    DWORD wait_result = WaitForSingleObject(pi.hProcess, 30000);
    int timed_out = (wait_result == WAIT_TIMEOUT);

    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    Sleep(100);

    char *stdout_buf = (char *)malloc(MAX_RESPONSE);
    char *stderr_buf = (char *)malloc(MAX_CMD_OUTPUT);
    DWORD bytes_read, bytes_avail;
    int stdout_len = 0, stderr_len = 0;

    while (stdout_len < MAX_RESPONSE - 1) {
        if (!PeekNamedPipe(stdout_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_RESPONSE - stdout_len - 1))
            to_read = (DWORD)(MAX_RESPONSE - stdout_len - 1);
        if (!ReadFile(stdout_rd, stdout_buf + stdout_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stdout_len += (int)bytes_read;
    }
    stdout_buf[stdout_len] = '\0';

    while (stderr_len < MAX_CMD_OUTPUT - 1) {
        if (!PeekNamedPipe(stderr_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_CMD_OUTPUT - stderr_len - 1))
            to_read = (DWORD)(MAX_CMD_OUTPUT - stderr_len - 1);
        if (!ReadFile(stderr_rd, stderr_buf + stderr_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stderr_len += (int)bytes_read;
    }
    stderr_buf[stderr_len] = '\0';

    CloseHandle(stdout_rd);
    CloseHandle(stderr_rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (timed_out) {
        send_error(sock, "UI tree timed out (30s)");
        free(stdout_buf); free(stderr_buf);
        return;
    }

    if (exit_code != 0 && stdout_len == 0) {
        char *err_esc = (char *)malloc(stderr_len * 6 + 128);
        json_escape(stderr_buf, err_esc, stderr_len * 6 + 128);
        char *response = (char *)malloc(MAX_RESPONSE);
        snprintf(response, MAX_RESPONSE,
                 "{\"status\":\"error\",\"message\":\"UI automation failed (exit %lu): %s\"}",
                 (unsigned long)exit_code, err_esc);
        send_line(sock, response);
        free(err_esc); free(response);
        free(stdout_buf); free(stderr_buf);
        return;
    }

    /* The PS1 script outputs JSON directly — wrap it in our response format */
    char *response = (char *)malloc(MAX_RESPONSE);
    snprintf(response, MAX_RESPONSE,
             "{\"status\":\"ok\",\"data\":%s}",
             stdout_buf);
    send_line(sock, response);

    free(response);
    free(stdout_buf); free(stderr_buf);
}

/* ================================================================
 * Command: ui_element_text
 *
 * Wrapper around vnc-uia.ps1 -Mode text. Returns text/value of
 * a UI element found by name or automationId.
 * ================================================================ */

static void cmd_ui_element_text(SOCKET sock, const char *json)
{
    char name[512] = {0};
    char automation_id[512] = {0};
    char control_type[128] = {0};

    json_get_string(json, "name", name, sizeof(name));
    json_get_string(json, "automation_id", automation_id, sizeof(automation_id));
    json_get_string(json, "control_type", control_type, sizeof(control_type));

    if (name[0] == '\0' && automation_id[0] == '\0') {
        send_error(sock, "Must specify 'name' or 'automation_id'");
        return;
    }

    log_msg("ui_element_text: name=%s aid=%s ct=%s", name, automation_id, control_type);

    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    char ps1_path[MAX_PATH];
    snprintf(ps1_path, sizeof(ps1_path), "%svnc-uia.ps1", exe_path);

    DWORD attrs = GetFileAttributesA(ps1_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        send_error(sock, "vnc-uia.ps1 not found next to vnc-helper.exe");
        return;
    }

    char cmd_line[4096];
    int offset = snprintf(cmd_line, sizeof(cmd_line),
                          "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                          "-File \"%s\" -Mode text", ps1_path);

    if (name[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -Name \"%s\"", name);
    if (automation_id[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -AutomationId \"%s\"", automation_id);
    if (control_type[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -ControlType \"%s\"", control_type);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE stdout_rd, stdout_wr, stderr_rd, stderr_wr;
    if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0) ||
        !CreatePipe(&stderr_rd, &stderr_wr, &sa, 0)) {
        send_error(sock, "Failed to create pipes");
        return;
    }

    SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_wr;
    si.hStdError  = stderr_wr;
    si.hStdInput  = NULL;

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        send_error(sock, "Failed to start PowerShell");
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stderr_rd); CloseHandle(stderr_wr);
        return;
    }

    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    WaitForSingleObject(pi.hProcess, 30000);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    Sleep(100);

    char *stdout_buf = (char *)malloc(MAX_RESPONSE);
    DWORD bytes_read, bytes_avail;
    int stdout_len = 0;

    while (stdout_len < MAX_RESPONSE - 1) {
        if (!PeekNamedPipe(stdout_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_RESPONSE - stdout_len - 1))
            to_read = (DWORD)(MAX_RESPONSE - stdout_len - 1);
        if (!ReadFile(stdout_rd, stdout_buf + stdout_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stdout_len += (int)bytes_read;
    }
    stdout_buf[stdout_len] = '\0';

    CloseHandle(stdout_rd);
    CloseHandle(stderr_rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    char *response = (char *)malloc(MAX_RESPONSE);
    snprintf(response, MAX_RESPONSE, "{\"status\":\"ok\",\"data\":%s}", stdout_buf);
    send_line(sock, response);

    free(response);
    free(stdout_buf);
}

/* ================================================================
 * Command: ui_click_element
 *
 * Wrapper around vnc-uia.ps1 -Mode click. Finds a UI element by
 * name/automationId and invokes its default action.
 * ================================================================ */

static void cmd_ui_click_element(SOCKET sock, const char *json)
{
    char name[512] = {0};
    char automation_id[512] = {0};
    char control_type[128] = {0};

    json_get_string(json, "name", name, sizeof(name));
    json_get_string(json, "automation_id", automation_id, sizeof(automation_id));
    json_get_string(json, "control_type", control_type, sizeof(control_type));

    if (name[0] == '\0' && automation_id[0] == '\0') {
        send_error(sock, "Must specify 'name' or 'automation_id'");
        return;
    }

    log_msg("ui_click_element: name=%s aid=%s ct=%s", name, automation_id, control_type);

    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    char ps1_path[MAX_PATH];
    snprintf(ps1_path, sizeof(ps1_path), "%svnc-uia.ps1", exe_path);

    DWORD attrs = GetFileAttributesA(ps1_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        send_error(sock, "vnc-uia.ps1 not found next to vnc-helper.exe");
        return;
    }

    char cmd_line[4096];
    int offset = snprintf(cmd_line, sizeof(cmd_line),
                          "powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive "
                          "-File \"%s\" -Mode click", ps1_path);

    if (name[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -Name \"%s\"", name);
    if (automation_id[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -AutomationId \"%s\"", automation_id);
    if (control_type[0])
        offset += snprintf(cmd_line + offset, sizeof(cmd_line) - offset,
                           " -ControlType \"%s\"", control_type);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE stdout_rd, stdout_wr, stderr_rd, stderr_wr;
    if (!CreatePipe(&stdout_rd, &stdout_wr, &sa, 0) ||
        !CreatePipe(&stderr_rd, &stderr_wr, &sa, 0)) {
        send_error(sock, "Failed to create pipes");
        return;
    }

    SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_wr;
    si.hStdError  = stderr_wr;
    si.hStdInput  = NULL;

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        send_error(sock, "Failed to start PowerShell");
        CloseHandle(stdout_rd); CloseHandle(stdout_wr);
        CloseHandle(stderr_rd); CloseHandle(stderr_wr);
        return;
    }

    CloseHandle(stdout_wr);
    CloseHandle(stderr_wr);

    WaitForSingleObject(pi.hProcess, 30000);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    Sleep(100);

    char *stdout_buf = (char *)malloc(MAX_RESPONSE);
    DWORD bytes_read, bytes_avail;
    int stdout_len = 0;

    while (stdout_len < MAX_RESPONSE - 1) {
        if (!PeekNamedPipe(stdout_rd, NULL, 0, NULL, &bytes_avail, NULL)
            || bytes_avail == 0)
            break;
        DWORD to_read = bytes_avail;
        if (to_read > (DWORD)(MAX_RESPONSE - stdout_len - 1))
            to_read = (DWORD)(MAX_RESPONSE - stdout_len - 1);
        if (!ReadFile(stdout_rd, stdout_buf + stdout_len, to_read,
                      &bytes_read, NULL) || bytes_read == 0)
            break;
        stdout_len += (int)bytes_read;
    }
    stdout_buf[stdout_len] = '\0';

    CloseHandle(stdout_rd);
    CloseHandle(stderr_rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    char *response = (char *)malloc(MAX_RESPONSE);
    snprintf(response, MAX_RESPONSE, "{\"status\":\"ok\",\"data\":%s}", stdout_buf);
    send_line(sock, response);

    free(response);
    free(stdout_buf);
}

/* ================================================================
 * Command: click_marker
 *
 * Draw a bright yellow ring on screen at (x,y) that auto-destroys
 * after a short duration. Visible in VNC screenshots (no capture
 * exclusion). Used by vnc_click for visual confirmation.
 * ================================================================ */

#define MARKER_WND_CLASS  "VncHelperMarker"
#define MARKER_TIMER_ID   99
#define MARKER_RADIUS     20
#define MARKER_STROKE     3
#define MARKER_SIZE       ((MARKER_RADIUS + MARKER_STROKE) * 2 + 2)
#define MARKER_DURATION   2000  /* ms before auto-destroy */

static volatile HWND g_marker_hwnd = NULL;

static LRESULT CALLBACK marker_wnd_proc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == MARKER_TIMER_ID) {
            KillTimer(hwnd, MARKER_TIMER_ID);
            DestroyWindow(hwnd);
            g_marker_hwnd = NULL;
        }
        return 0;
    case WM_DESTROY:
        g_marker_hwnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Create and show a click marker at the given screen coordinates.
 * Must be called from the GUI thread (posts a message to trigger creation). */
static void create_click_marker(int cx, int cy, int duration_ms)
{
    /* Destroy previous marker if still visible */
    if (g_marker_hwnd) {
        DestroyWindow(g_marker_hwnd);
        g_marker_hwnd = NULL;
    }

    int sz = MARKER_SIZE;
    int x = cx - sz / 2;
    int y = cy - sz / 2;

    HINSTANCE hInst = GetModuleHandleA(NULL);

    /* Register class once */
    static int registered = 0;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = marker_wnd_proc;
        wc.hInstance      = hInst;
        wc.lpszClassName  = MARKER_WND_CLASS;
        wc.hbrBackground  = (HBRUSH)GetStockObject(NULL_BRUSH);
        RegisterClassA(&wc);
        registered = 1;
    }

    HWND hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        MARKER_WND_CLASS, NULL,
        WS_POPUP,
        x, y, sz, sz,
        NULL, NULL, hInst, NULL);

    if (!hwnd) return;

    /* Paint yellow ring into a 32-bit ARGB DIB */
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = sz;
    bmi.bmiHeader.biHeight      = -sz;  /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    BYTE *bits = NULL;
    HDC screen_dc = GetDC(NULL);
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    HBITMAP dib = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS,
                                   (void **)&bits, NULL, 0);
    HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, dib);

    /* Clear to fully transparent */
    memset(bits, 0, (size_t)(sz * sz * 4));

    /* Draw yellow ring with per-pixel alpha */
    int center = sz / 2;
    int r_outer = MARKER_RADIUS + MARKER_STROKE;
    int r_inner = MARKER_RADIUS - MARKER_STROKE;
    for (int py = 0; py < sz; py++) {
        for (int px = 0; px < sz; px++) {
            int dx = px - center;
            int dy = py - center;
            int dist_sq = dx * dx + dy * dy;
            if (dist_sq <= r_outer * r_outer && dist_sq >= r_inner * r_inner) {
                BYTE *p = bits + (py * sz + px) * 4;
                /* Pre-multiplied alpha BGRA: bright yellow, fully opaque */
                p[0] = 0;     /* B */
                p[1] = 255;   /* G */
                p[2] = 255;   /* R */
                p[3] = 255;   /* A */
            }
        }
    }

    /* Also draw a small crosshair at center (4px) */
    for (int i = -4; i <= 4; i++) {
        /* Horizontal */
        if (center + i >= 0 && center + i < sz) {
            BYTE *p = bits + (center * sz + center + i) * 4;
            p[0] = 0; p[1] = 255; p[2] = 255; p[3] = 255;
        }
        /* Vertical */
        if (center + i >= 0 && center + i < sz) {
            BYTE *p = bits + ((center + i) * sz + center) * 4;
            p[0] = 0; p[1] = 255; p[2] = 255; p[3] = 255;
        }
    }

    /* UpdateLayeredWindow */
    POINT pt_src = {0, 0};
    POINT pt_pos = {x, y};
    SIZE wnd_sz = {sz, sz};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    UpdateLayeredWindow(hwnd, screen_dc, &pt_pos, &wnd_sz,
                        mem_dc, &pt_src, 0, &blend, ULW_ALPHA);

    SelectObject(mem_dc, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);

    /* Show and start auto-destroy timer */
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    if (duration_ms < 500)  duration_ms = 500;
    if (duration_ms > 10000) duration_ms = 10000;
    SetTimer(hwnd, MARKER_TIMER_ID, (UINT)duration_ms, NULL);

    g_marker_hwnd = hwnd;
}

static void cmd_click_marker(SOCKET sock, const char *json)
{
    int x = 0, y = 0, duration = MARKER_DURATION;

    if (!json_get_int(json, "x", &x) || !json_get_int(json, "y", &y)) {
        send_error(sock, "Missing 'x' and 'y' parameters");
        return;
    }
    json_get_int(json, "duration", &duration);

    log_msg("click_marker: x=%d y=%d duration=%d", x, y, duration);

    /* Must create the overlay window on the GUI thread.
     * Since we're on a worker thread, post a message to the tray window
     * and have it call create_click_marker. For simplicity, just call
     * it directly — CreateWindowEx works from any thread as long as the
     * message loop processes messages (which it does via GetMessage). */
    create_click_marker(x, y, duration);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"data\":{\"x\":%d,\"y\":%d,\"duration\":%d}}",
             x, y, duration);
    send_line(sock, buf);
}

/* ================================================================
 * Client Handler
 * ================================================================ */

/* Read one newline-delimited request from the socket.
 * Returns bytes read (>0) on success, 0 on clean disconnect, -1 on error/timeout. */
static int read_request(SOCKET sock, char *buf, int buf_size)
{
    int total = 0;
    memset(buf, 0, buf_size);

    while (total < buf_size - 1) {
        int n = recv(sock, buf + total, buf_size - total - 1, 0);
        if (n <= 0) return (total > 0 && strchr(buf, '\n')) ? total : (n == 0 ? 0 : -1);
        total += n;
        buf[total] = '\0';
        if (strchr(buf, '\n')) break;
    }

    /* Strip trailing whitespace / newline */
    while (total > 0 && (buf[total - 1] == '\n' ||
                         buf[total - 1] == '\r' ||
                         buf[total - 1] == ' ')) {
        buf[--total] = '\0';
    }

    return total;
}

/* Persistent client handler: reads multiple requests per connection.
 * Each request/response is newline-delimited JSON.
 * An optional "id" field in the request is echoed in the response. */
static void handle_client(SOCKET sock)
{
    char request[MAX_REQUEST];

    while (g_running) {
        int n = read_request(sock, request, sizeof(request));
        if (n <= 0) break;  /* disconnect or error */

        log_msg("request: %s", request);

        char command[64] = {0};
        if (!json_get_string(request, "command", command, sizeof(command))) {
            send_error(sock, "Missing 'command' field");
            continue;
        }

        if      (strcmp(command, "cursor_position") == 0) cmd_cursor_position(sock);
        else if (strcmp(command, "window_list")     == 0) cmd_window_list(sock);
        else if (strcmp(command, "active_window")   == 0) cmd_active_window(sock);
        else if (strcmp(command, "run_command")     == 0) cmd_run_command(sock, request);
        else if (strcmp(command, "screen_info")     == 0) cmd_screen_info(sock);
        else if (strcmp(command, "file_upload")     == 0) cmd_file_upload(sock, request);
        else if (strcmp(command, "file_download")   == 0) cmd_file_download(sock, request);
        else if (strcmp(command, "ocr_region")      == 0) cmd_ocr_region(sock, request);
        else if (strcmp(command, "ui_tree")         == 0) cmd_ui_tree(sock, request);
        else if (strcmp(command, "ui_element_text") == 0) cmd_ui_element_text(sock, request);
        else if (strcmp(command, "ui_click_element")== 0) cmd_ui_click_element(sock, request);
        else if (strcmp(command, "click_marker")   == 0) cmd_click_marker(sock, request);
        else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Unknown command: %s", command);
            send_error(sock, msg);
        }
    }
}

/* ================================================================
 * Client Worker Thread (one per connection)
 * ================================================================ */

typedef struct {
    SOCKET sock;
    char   ip[64];
    int    port;
} ClientCtx;

static DWORD WINAPI client_thread(LPVOID param)
{
    ClientCtx *ctx = (ClientCtx *)param;
    LONG prev;

    /* Track connection — cancel any pending linger hide */
    prev = InterlockedIncrement(&g_client_count);
    g_overlay_linger_until = 0;
    EnterCriticalSection(&g_cs);
    strncpy(g_client_ip, ctx->ip, sizeof(g_client_ip) - 1);
    g_connect_time = GetTickCount();
    LeaveCriticalSection(&g_cs);

    /* Switch tray icon to connected state + show overlay */
    if (g_icon_connected && g_hwnd) {
        g_nid.hIcon = g_icon_connected;
        snprintf(g_nid.szTip, sizeof(g_nid.szTip),
                 "VNC Helper (CONNECTED: %s)", ctx->ip);
        Shell_NotifyIconA(NIM_MODIFY, &g_nid);
    }
    overlay_show();

    log_msg("client connected from %s:%d", ctx->ip, ctx->port);

    if (auth_client(ctx->sock))
        handle_client(ctx->sock);
    closesocket(ctx->sock);

    log_msg("client disconnected: %s:%d", ctx->ip, ctx->port);

    /* Decrement — start linger countdown if no more clients */
    prev = InterlockedDecrement(&g_client_count);
    if (prev == 0) {
        /* Don't hide immediately — set linger deadline.
         * The overlay timer (1s) will auto-hide after OVERLAY_LINGER_MS. */
        g_overlay_linger_until = GetTickCount() + OVERLAY_LINGER_MS;
    }

    free(ctx);
    return 0;
}

/* ================================================================
 * TCP Server Thread (accept loop)
 * ================================================================ */

static DWORD WINAPI server_thread(LPVOID param)
{
    (void)param;

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        log_msg("socket() failed: %d", WSAGetLastError());
        return 1;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((u_short)g_port);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr))
            == SOCKET_ERROR) {
        log_msg("bind() failed on port %d: %d", g_port, WSAGetLastError());
        closesocket(listen_sock);
        return 1;
    }

    if (listen(listen_sock, 5) == SOCKET_ERROR) {
        log_msg("listen() failed: %d", WSAGetLastError());
        closesocket(listen_sock);
        return 1;
    }

    log_msg("listening on port %d", g_port);

    while (g_running) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client_sock = accept(listen_sock,
                                    (struct sockaddr *)&client_addr,
                                    &client_len);

        if (client_sock == INVALID_SOCKET) {
            if (g_running)
                log_msg("accept() failed: %d", WSAGetLastError());
            continue;
        }

        /* Enable TCP keepalive to detect dead persistent connections */
        int keepalive = 1;
        setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE,
                   (const char *)&keepalive, sizeof(keepalive));

        /* Spawn worker thread for this client */
        ClientCtx *ctx = (ClientCtx *)malloc(sizeof(ClientCtx));
        if (!ctx) {
            log_msg("malloc failed for client context");
            closesocket(client_sock);
            continue;
        }
        ctx->sock = client_sock;
        strncpy(ctx->ip, inet_ntoa(client_addr.sin_addr), sizeof(ctx->ip) - 1);
        ctx->ip[sizeof(ctx->ip) - 1] = '\0';
        ctx->port = ntohs(client_addr.sin_port);

        HANDLE ht = CreateThread(NULL, 0, client_thread, ctx, 0, NULL);
        if (ht) {
            CloseHandle(ht);  /* detach — thread runs independently */
        } else {
            log_msg("CreateThread failed: %lu", (unsigned long)GetLastError());
            closesocket(client_sock);
            free(ctx);
        }
    }

    closesocket(listen_sock);
    return 0;
}

/* ================================================================
 * System Tray
 * ================================================================ */

#define TRAY_WND_CLASS "VncHelperTray"

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg,
                                 WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuA(menu, MF_STRING | MF_GRAYED, IDM_ABOUT,
                        "VNC Helper v" VNC_HELPER_VERSION);
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(menu, MF_STRING, IDM_EXIT, "Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                           pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(menu);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_EXIT:
            g_running = 0;
            Shell_NotifyIconA(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            break;
        case IDM_ABOUT: {
            char about[512];
            snprintf(about, sizeof(about),
                     "VNC Helper Agent v" VNC_HELPER_VERSION "\n"
                     "Part of vnc-mcp-server\n\n"
                     "Listening on port %d\n\n"
                     "Copyright (c) 2026,\n"
                     "The Daniel Morante Company, Inc.",
                     g_port);
            MessageBoxA(NULL, about, "VNC Helper",
                        MB_OK | MB_ICONINFORMATION);
            break;
        }
        }
        return 0;

    case WM_DESTROY:
        g_running = 0;
        Shell_NotifyIconA(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void setup_tray(HINSTANCE hInstance)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = TRAY_WND_CLASS;
    RegisterClassA(&wc);

    g_hwnd = CreateWindowA(TRAY_WND_CLASS, "VNC Helper", 0,
                           0, 0, 0, 0,
                           HWND_MESSAGE, NULL, hInstance, NULL);

    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    /* Load custom icons from embedded resources */
    g_icon_normal = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_TRAY_NORMAL));
    g_icon_connected = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_TRAY_CONNECTED));
    if (!g_icon_normal)
        g_icon_normal = LoadIconA(NULL, IDI_APPLICATION);
    if (!g_icon_connected)
        g_icon_connected = g_icon_normal;

    g_nid.hIcon            = g_icon_normal;
    snprintf(g_nid.szTip, sizeof(g_nid.szTip),
             "VNC Helper (port %d)", g_port);

    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

/* ================================================================
 * On-Screen Connection Indicator Overlay
 *
 * A small translucent pill at top-right of screen showing:
 *   "● 192.168.1.233 (0:05)"
 * Appears when a client connects, hides when all disconnect.
 * WS_EX_TOPMOST + WS_EX_TOOLWINDOW + WS_EX_LAYERED (click-through)
 * ================================================================ */

#define OVERLAY_WND_CLASS "VncHelperOverlay"
#define OVERLAY_TIMER_ID  42
#define OVERLAY_WIDTH     260
#define OVERLAY_HEIGHT    30
#define OVERLAY_MARGIN    8
#define OVERLAY_BG_ALPHA  210  /* 0-255 background translucency */

/* WDA_EXCLUDEFROMCAPTURE: hide overlay from screen capture (Win10 2004+) */
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static HWND  g_overlay_hwnd = NULL;

/* Paint the overlay into a 32-bit ARGB DIB and call UpdateLayeredWindow.
 * This gives per-pixel alpha: the background is semi-transparent while
 * the text is rendered at full opacity — no washed-out appearance. */
static void overlay_repaint(HWND hwnd)
{
    if (!hwnd) return;

    int w = OVERLAY_WIDTH, h = OVERLAY_HEIGHT;

    /* Create 32-bit ARGB DIB section */
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;  /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    BYTE *bits = NULL;
    HDC screen_dc = GetDC(NULL);
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    HBITMAP dib = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS,
                                   (void **)&bits, NULL, 0);
    HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, dib);

    /* Clear to transparent */
    memset(bits, 0, (size_t)(w * h * 4));

    /* Fill rounded rect background with per-pixel alpha */
    {
        int radius = 10;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                /* Check if pixel is inside the rounded rect */
                int inside = 1;
                /* Top-left corner */
                if (x < radius && y < radius) {
                    int dx = radius - x - 1, dy = radius - y - 1;
                    if (dx * dx + dy * dy > radius * radius) inside = 0;
                }
                /* Top-right corner */
                else if (x >= w - radius && y < radius) {
                    int dx = x - (w - radius), dy = radius - y - 1;
                    if (dx * dx + dy * dy > radius * radius) inside = 0;
                }
                /* Bottom-left corner */
                else if (x < radius && y >= h - radius) {
                    int dx = radius - x - 1, dy = y - (h - radius);
                    if (dx * dx + dy * dy > radius * radius) inside = 0;
                }
                /* Bottom-right corner */
                else if (x >= w - radius && y >= h - radius) {
                    int dx = x - (w - radius), dy = y - (h - radius);
                    if (dx * dx + dy * dy > radius * radius) inside = 0;
                }

                if (inside) {
                    BYTE *p = bits + (y * w + x) * 4;
                    /* Pre-multiplied alpha BGRA */
                    BYTE a = OVERLAY_BG_ALPHA;
                    p[0] = (BYTE)(30 * a / 255);   /* B */
                    p[1] = (BYTE)(30 * a / 255);   /* G */
                    p[2] = (BYTE)(30 * a / 255);   /* R */
                    p[3] = a;                       /* A */
                }
            }
        }
    }

    /* Build text: show IP and duration while connected or lingering */
    char text[128] = {0};
    EnterCriticalSection(&g_cs);
    if (g_client_ip[0] && g_connect_time) {
        DWORD elapsed = (GetTickCount() - g_connect_time) / 1000;
        int mins = (int)(elapsed / 60);
        int secs = (int)(elapsed % 60);
        if (g_client_count > 0) {
            snprintf(text, sizeof(text), "Connected: %s (%d:%02d)",
                     g_client_ip, mins, secs);
        } else {
            snprintf(text, sizeof(text), "Last: %s (%d:%02d)",
                     g_client_ip, mins, secs);
        }
    }
    LeaveCriticalSection(&g_cs);

    if (text[0]) {
        /* Two-pass text rendering: GDI DrawText doesn't write alpha,
         * so draw text on a separate all-zero surface to isolate text
         * pixels, then composite onto the main DIB at full opacity. */
        BYTE *text_bits = NULL;
        HDC text_dc = CreateCompatibleDC(screen_dc);
        HBITMAP text_dib = CreateDIBSection(text_dc, &bmi, DIB_RGB_COLORS,
                                            (void **)&text_bits, NULL, 0);
        HBITMAP text_old_bmp = (HBITMAP)SelectObject(text_dc, text_dib);
        memset(text_bits, 0, (size_t)(w * h * 4));

        SetBkMode(text_dc, TRANSPARENT);
        SetTextColor(text_dc, RGB(180, 255, 180));

        HFONT font = CreateFontA(16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        HFONT old_font = (HFONT)SelectObject(text_dc, font);

        RECT text_rc = {12, 0, w - 8, h};
        DrawTextA(text_dc, text, -1, &text_rc,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT);

        SelectObject(text_dc, old_font);
        DeleteObject(font);

        /* Composite: any text_bits pixel with non-zero RGB is text.
         * Write it into the main DIB as fully opaque. */
        for (int i = 0; i < w * h; i++) {
            BYTE *tp = text_bits + i * 4;
            if (tp[0] | tp[1] | tp[2]) {
                BYTE *dp = bits + i * 4;
                dp[0] = tp[0];  /* B */
                dp[1] = tp[1];  /* G */
                dp[2] = tp[2];  /* R */
                dp[3] = 255;    /* fully opaque */
            }
        }

        SelectObject(text_dc, text_old_bmp);
        DeleteObject(text_dib);
        DeleteDC(text_dc);
    }

    /* UpdateLayeredWindow with per-pixel alpha */
    POINT pt_src = {0, 0};
    SIZE sz = {w, h};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    POINT pt_pos;
    RECT win_rc;
    GetWindowRect(hwnd, &win_rc);
    pt_pos.x = win_rc.left;
    pt_pos.y = win_rc.top;

    UpdateLayeredWindow(hwnd, screen_dc, &pt_pos, &sz,
                        mem_dc, &pt_src, 0, &blend, ULW_ALPHA);

    SelectObject(mem_dc, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);
}

static LRESULT CALLBACK overlay_wnd_proc(HWND hwnd, UINT msg,
                                          WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == OVERLAY_TIMER_ID) {
            overlay_repaint(hwnd);
            /* Auto-hide after linger period with no active clients */
            if (g_client_count == 0 && g_overlay_linger_until != 0 &&
                GetTickCount() >= g_overlay_linger_until) {
                g_overlay_linger_until = 0;
                ShowWindow(hwnd, SW_HIDE);
                /* Restore tray icon to idle */
                if (g_icon_normal && g_hwnd) {
                    g_nid.hIcon = g_icon_normal;
                    snprintf(g_nid.szTip, sizeof(g_nid.szTip),
                             "VNC Helper (port %d)", g_port);
                    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
                }
            }
        }
        return 0;

    case WM_NCHITTEST:
        /* Allow dragging the overlay by its client area */
        return HTCAPTION;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void overlay_create(HINSTANCE hInstance)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = overlay_wnd_proc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = OVERLAY_WND_CLASS;
    wc.hbrBackground  = (HBRUSH)GetStockObject(NULL_BRUSH);
    RegisterClassA(&wc);

    /* Position at top-right of primary monitor */
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int x = screen_w - OVERLAY_WIDTH - OVERLAY_MARGIN;
    int y = OVERLAY_MARGIN;

    g_overlay_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        OVERLAY_WND_CLASS, "VNC Connection",
        WS_POPUP,
        x, y, OVERLAY_WIDTH, OVERLAY_HEIGHT,
        NULL, NULL, hInstance, NULL);

    /* Hide overlay from screen capture / VNC (Win10 2004+) */
    SetWindowDisplayAffinity(g_overlay_hwnd, WDA_EXCLUDEFROMCAPTURE);

    /* 1-second timer for duration updates + per-pixel alpha repaint */
    SetTimer(g_overlay_hwnd, OVERLAY_TIMER_ID, 1000, NULL);
}

static void overlay_show(void)
{
    if (g_overlay_hwnd) {
        ShowWindow(g_overlay_hwnd, SW_SHOWNOACTIVATE);
        overlay_repaint(g_overlay_hwnd);  /* immediate content */
    }
}

static void overlay_hide(void)
{
    if (g_overlay_hwnd)
        ShowWindow(g_overlay_hwnd, SW_HIDE);
}

/* ================================================================
 * Registry Install / Uninstall
 * ================================================================ */

#define REG_RUN_KEY    "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_VALUE_NAME "VncHelper"

static void do_install(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);

    char value[MAX_PATH + 32];
    snprintf(value, sizeof(value), "\"%s\" -port %d", path, g_port);

    HKEY hkey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_RUN_KEY, 0,
                      KEY_SET_VALUE, &hkey) == ERROR_SUCCESS) {
        RegSetValueExA(hkey, REG_VALUE_NAME, 0, REG_SZ,
                       (const BYTE *)value, (DWORD)strlen(value) + 1);
        RegCloseKey(hkey);
        printf("Installed to startup: %s\n", value);
    } else {
        fprintf(stderr, "Failed to open registry key\n");
    }
}

static void do_uninstall(void)
{
    HKEY hkey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_RUN_KEY, 0,
                      KEY_SET_VALUE, &hkey) == ERROR_SUCCESS) {
        RegDeleteValueA(hkey, REG_VALUE_NAME);
        RegCloseKey(hkey);
        printf("Removed from startup\n");
    } else {
        fprintf(stderr, "Failed to open registry key\n");
    }
}

/* ================================================================
 * Entry Point — WinMain (GUI subsystem, no console by default)
 * ================================================================ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Parse command line using Win32 API */
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);

    for (int i = 1; i < argc; i++) {
        if (wcscmp(wargv[i], L"-console") == 0) {
            g_console = 1;
        } else if (wcscmp(wargv[i], L"-port") == 0 && i + 1 < argc) {
            g_port = _wtoi(wargv[++i]);
        } else if (wcscmp(wargv[i], L"-password-file") == 0 && i + 1 < argc) {
            /* Convert wide string to narrow for password file path */
            static char pw_path[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, wargv[++i], -1,
                                pw_path, sizeof(pw_path), NULL, NULL);
            g_password_file = pw_path;
        } else if (wcscmp(wargv[i], L"install") == 0) {
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            do_install();
            LocalFree(wargv);
            return 0;
        } else if (wcscmp(wargv[i], L"uninstall") == 0) {
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            do_uninstall();
            LocalFree(wargv);
            return 0;
        } else if (wcscmp(wargv[i], L"-h") == 0 ||
                   wcscmp(wargv[i], L"--help") == 0) {
            AllocConsole();
            freopen("CONOUT$", "w", stdout);
            printf("VNC Helper Agent v" VNC_HELPER_VERSION "\n\n"
                   "Usage: vnc-helper.exe [options]\n"
                   "  -console              Show console output\n"
                   "  -port PORT            Listen port (default: %d)\n"
                   "  -password-file PATH   Read VNC password from file\n"
                   "  install               Add to Windows startup\n"
                   "  uninstall             Remove from Windows startup\n\n"
                   "Authentication: reads VNC password from the Windows\n"
                   "registry (TightVNC, RealVNC, TigerVNC, UltraVNC) or\n"
                   "from -password-file. Uses VNC DES challenge-response.\n",
                   DEFAULT_PORT);
            LocalFree(wargv);
            return 0;
        }
    }
    LocalFree(wargv);

    /* Console mode: allocate a console for debugging output */
    if (g_console) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    /* Initialize critical section for client tracking */
    InitializeCriticalSection(&g_cs);

    /* Initialize Winsock */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (g_console) fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    log_msg("VNC Helper v" VNC_HELPER_VERSION " starting on port %d", g_port);

    /* Initialize authentication (password file or registry) */
    init_auth();

    /* Start TCP server in background thread */
    CreateThread(NULL, 0, server_thread, NULL, 0, NULL);

    /* Setup system tray icon and connection overlay */
    setup_tray(hInstance);
    overlay_create(hInstance);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    DeleteCriticalSection(&g_cs);
    WSACleanup();
    return 0;
}
