/*
 * winmcp-auth.c — VNC DES Authentication (RFC 6143 Section 7.2.2)
 *
 * DES-ECB implementation using pre-computed SP tables that merge
 * S-box substitution + P permutation into single lookups.
 * Based on the D3DES algorithm by Richard Outerbridge (public domain).
 * This is the same approach used by TightVNC, RealVNC, and libvncserver.
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#include "winmcp.h"

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
    /* d3des uses LSB-first convention -- pass raw key, no bit reversal */
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
void init_auth(void)
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

    log_msg("WARNING: no VNC password found -- running WITHOUT authentication");
    log_msg("  Use -password-file <path> or ensure TightVNC/RealVNC is installed");
}

/* Perform VNC DES challenge-response authentication with a client.
 * Returns 1 on success, 0 on failure. */
int auth_client(SOCKET sock)
{
    if (!g_auth_enabled) return 1;  /* no auth configured */

    /* Generate 16-byte random challenge */
    unsigned char challenge[16];
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT)) {
        log_msg("CryptAcquireContext failed -- rejecting client");
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
        log_msg("auth FAILED -- wrong password");
        return 0;
    }

    /* Send 4-byte success result */
    unsigned char ok[4] = {0, 0, 0, 0};  /* big-endian 0 = OK */
    send(sock, (const char *)ok, 4, 0);
    log_msg("auth OK");
    return 1;
}
