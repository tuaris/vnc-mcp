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

#define _WIN32_WINNT 0x0600

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
static char g_password[9]       = {0};  /* VNC password (max 8 chars) */
static int  g_auth_enabled      = 0;
static const char *g_password_file = NULL;

/* Forward declarations */
static void log_msg(const char *fmt, ...);

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
 * Client Handler
 * ================================================================ */

static void handle_client(SOCKET sock)
{
    char request[MAX_REQUEST];
    int  total = 0;

    memset(request, 0, sizeof(request));

    while (total < MAX_REQUEST - 1) {
        int n = recv(sock, request + total, MAX_REQUEST - total - 1, 0);
        if (n <= 0) return;
        total += n;
        request[total] = '\0';
        if (strchr(request, '\n')) break;
    }

    /* Strip trailing whitespace / newline */
    while (total > 0 && (request[total - 1] == '\n' ||
                         request[total - 1] == '\r' ||
                         request[total - 1] == ' ')) {
        request[--total] = '\0';
    }

    if (total == 0) return;
    log_msg("request: %s", request);

    char command[64] = {0};
    if (!json_get_string(request, "command", command, sizeof(command))) {
        send_error(sock, "Missing 'command' field");
        return;
    }

    if      (strcmp(command, "cursor_position") == 0) cmd_cursor_position(sock);
    else if (strcmp(command, "window_list")     == 0) cmd_window_list(sock);
    else if (strcmp(command, "active_window")   == 0) cmd_active_window(sock);
    else if (strcmp(command, "run_command")     == 0) cmd_run_command(sock, request);
    else if (strcmp(command, "screen_info")     == 0) cmd_screen_info(sock);
    else if (strcmp(command, "file_upload")     == 0) cmd_file_upload(sock, request);
    else if (strcmp(command, "file_download")   == 0) cmd_file_download(sock, request);
    else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Unknown command: %s", command);
        send_error(sock, msg);
    }
}

/* ================================================================
 * TCP Server Thread
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

        /* Set receive timeout so stale connections don't block forever */
        DWORD recv_timeout = 10000; /* 10 seconds */
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&recv_timeout, sizeof(recv_timeout));

        log_msg("client connected from %s:%d",
                inet_ntoa(client_addr.sin_addr),
                ntohs(client_addr.sin_port));
        if (auth_client(client_sock))
            handle_client(client_sock);
        closesocket(client_sock);
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
    g_nid.hIcon            = LoadIconA(NULL, IDI_APPLICATION);
    snprintf(g_nid.szTip, sizeof(g_nid.szTip),
             "VNC Helper (port %d)", g_port);

    Shell_NotifyIconA(NIM_ADD, &g_nid);
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
 * Entry Point
 * ================================================================ */

int main(int argc, char *argv[])
{
    HINSTANCE hInstance = GetModuleHandle(NULL);

    /* Parse command line */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-console") == 0) {
            g_console = 1;
        } else if (strcmp(argv[i], "-port") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-password-file") == 0 && i + 1 < argc) {
            g_password_file = argv[++i];
        } else if (strcmp(argv[i], "install") == 0) {
            do_install();
            return 0;
        } else if (strcmp(argv[i], "uninstall") == 0) {
            do_uninstall();
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
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
            return 0;
        }
    }

    /* Console / tray mode setup */
    if (g_console) {
        /* Ensure we have a visible console (needed if built with -mwindows) */
        if (!GetConsoleWindow()) {
            AllocConsole();
        }
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    } else {
        /* Hide console window in tray mode */
        HWND con = GetConsoleWindow();
        if (con) ShowWindow(con, SW_HIDE);
    }

    /* Initialize Winsock */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    log_msg("VNC Helper v" VNC_HELPER_VERSION " starting on port %d", g_port);

    /* Initialize authentication (password file or registry) */
    init_auth();

    /* Start TCP server in background thread */
    CreateThread(NULL, 0, server_thread, NULL, 0, NULL);

    /* Setup system tray icon and run message loop */
    setup_tray(hInstance);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    WSACleanup();
    return 0;
}
