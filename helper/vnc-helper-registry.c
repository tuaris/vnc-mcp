/*
 * vnc-helper-registry.c — Windows registry commands
 *
 * registry_read, registry_write, registry_list
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#include "vnc-helper.h"

/* ================================================================
 * Command: registry_read
 *
 * Read a registry value. Supports REG_SZ, REG_DWORD, REG_EXPAND_SZ,
 * REG_MULTI_SZ, REG_BINARY, REG_QWORD.
 * ================================================================ */

static HKEY parse_root_key(const char *name)
{
    if (_stricmp(name, "HKLM") == 0 || _stricmp(name, "HKEY_LOCAL_MACHINE") == 0)
        return HKEY_LOCAL_MACHINE;
    if (_stricmp(name, "HKCU") == 0 || _stricmp(name, "HKEY_CURRENT_USER") == 0)
        return HKEY_CURRENT_USER;
    if (_stricmp(name, "HKCR") == 0 || _stricmp(name, "HKEY_CLASSES_ROOT") == 0)
        return HKEY_CLASSES_ROOT;
    if (_stricmp(name, "HKU") == 0 || _stricmp(name, "HKEY_USERS") == 0)
        return HKEY_USERS;
    if (_stricmp(name, "HKCC") == 0 || _stricmp(name, "HKEY_CURRENT_CONFIG") == 0)
        return HKEY_CURRENT_CONFIG;
    return NULL;
}

static const char *reg_type_name(DWORD type)
{
    switch (type) {
    case REG_SZ:           return "REG_SZ";
    case REG_EXPAND_SZ:    return "REG_EXPAND_SZ";
    case REG_DWORD:        return "REG_DWORD";
    case REG_QWORD:        return "REG_QWORD";
    case REG_BINARY:       return "REG_BINARY";
    case REG_MULTI_SZ:     return "REG_MULTI_SZ";
    case REG_NONE:         return "REG_NONE";
    default:               return "UNKNOWN";
    }
}

void cmd_registry_read(SOCKET sock, const char *json)
{
    char key[1024] = {0};
    char value_name[512] = {0};

    if (!json_get_string(json, "key", key, sizeof(key))) {
        send_error(sock, "Missing 'key' parameter (e.g. HKLM\\SOFTWARE\\Microsoft)");
        return;
    }
    json_get_string(json, "value", value_name, sizeof(value_name));

    log_msg("registry_read: key=%s value=%s", key, value_name[0] ? value_name : "(Default)");

    /* Split root from subkey */
    char *sep = strchr(key, '\\');
    if (!sep) {
        send_error(sock, "Invalid key format (expected ROOT\\SubKey, e.g. HKLM\\SOFTWARE)");
        return;
    }
    *sep = '\0';
    const char *subkey = sep + 1;
    HKEY root = parse_root_key(key);
    *sep = '\\';  /* restore */
    if (!root) {
        send_error(sock, "Unknown root key (use HKLM, HKCU, HKCR, HKU, or HKCC)");
        return;
    }

    HKEY hkey;
    LONG rc = RegOpenKeyExA(root, subkey, 0, KEY_READ, &hkey);
    if (rc != ERROR_SUCCESS) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to open key (error %ld)", rc);
        send_error(sock, msg);
        return;
    }

    DWORD type = 0;
    DWORD data_len = 0;
    rc = RegQueryValueExA(hkey, value_name[0] ? value_name : NULL, NULL, &type, NULL, &data_len);
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(hkey);
        char msg[256];
        snprintf(msg, sizeof(msg), "Value not found (error %ld)", rc);
        send_error(sock, msg);
        return;
    }

    unsigned char *data = (unsigned char *)malloc(data_len + 2);
    rc = RegQueryValueExA(hkey, value_name[0] ? value_name : NULL, NULL, &type, data, &data_len);
    RegCloseKey(hkey);

    if (rc != ERROR_SUCCESS) {
        free(data);
        send_error(sock, "Failed to read value data");
        return;
    }

    char *response = (char *)malloc(MAX_RESPONSE);
    char key_esc[2048], val_esc[1024];
    json_escape(key, key_esc, sizeof(key_esc));
    json_escape(value_name, val_esc, sizeof(val_esc));

    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        data[data_len] = '\0';
        char *data_esc = (char *)malloc(data_len * 6 + 1);
        json_escape((char *)data, data_esc, (int)(data_len * 6 + 1));
        snprintf(response, MAX_RESPONSE,
                 "{\"status\":\"ok\",\"data\":{\"key\":\"%s\",\"value\":\"%s\","
                 "\"type\":\"%s\",\"data\":\"%s\"}}",
                 key_esc, val_esc, reg_type_name(type), data_esc);
        free(data_esc);
    } else if (type == REG_DWORD && data_len >= 4) {
        DWORD dval = *(DWORD *)data;
        snprintf(response, MAX_RESPONSE,
                 "{\"status\":\"ok\",\"data\":{\"key\":\"%s\",\"value\":\"%s\","
                 "\"type\":\"REG_DWORD\",\"data\":%lu}}",
                 key_esc, val_esc, (unsigned long)dval);
    } else if (type == REG_QWORD && data_len >= 8) {
        unsigned long long qval = *(unsigned long long *)data;
        snprintf(response, MAX_RESPONSE,
                 "{\"status\":\"ok\",\"data\":{\"key\":\"%s\",\"value\":\"%s\","
                 "\"type\":\"REG_QWORD\",\"data\":%llu}}",
                 key_esc, val_esc, qval);
    } else if (type == REG_MULTI_SZ) {
        /* Multi-string: double-null terminated, show as JSON array */
        int off = snprintf(response, MAX_RESPONSE,
                           "{\"status\":\"ok\",\"data\":{\"key\":\"%s\",\"value\":\"%s\","
                           "\"type\":\"REG_MULTI_SZ\",\"data\":[",
                           key_esc, val_esc);
        char *p = (char *)data;
        int first = 1;
        while (*p) {
            if (!first && off < MAX_RESPONSE - 2) response[off++] = ',';
            first = 0;
            char *str_esc = (char *)malloc(strlen(p) * 6 + 1);
            json_escape(p, str_esc, (int)(strlen(p) * 6 + 1));
            off += snprintf(response + off, MAX_RESPONSE - off, "\"%s\"", str_esc);
            free(str_esc);
            p += strlen(p) + 1;
        }
        snprintf(response + off, MAX_RESPONSE - off, "]}}");
    } else {
        /* REG_BINARY or unknown — hex encode */
        int off = snprintf(response, MAX_RESPONSE,
                           "{\"status\":\"ok\",\"data\":{\"key\":\"%s\",\"value\":\"%s\","
                           "\"type\":\"%s\",\"data\":\"",
                           key_esc, val_esc, reg_type_name(type));
        for (DWORD i = 0; i < data_len && off < MAX_RESPONSE - 4; i++)
            off += snprintf(response + off, MAX_RESPONSE - off, "%02x", data[i]);
        snprintf(response + off, MAX_RESPONSE - off, "\"}}");
    }

    send_line(sock, response);
    free(data);
    free(response);
}

/* ================================================================
 * Command: registry_write
 *
 * Write a registry value. Types: REG_SZ, REG_DWORD, REG_EXPAND_SZ.
 * ================================================================ */

void cmd_registry_write(SOCKET sock, const char *json)
{
    char key[1024] = {0};
    char value_name[512] = {0};
    char reg_type[32] = {0};
    char str_data[4096] = {0};
    int  dword_data = 0;

    if (!json_get_string(json, "key", key, sizeof(key))) {
        send_error(sock, "Missing 'key' parameter");
        return;
    }
    json_get_string(json, "value", value_name, sizeof(value_name));
    json_get_string(json, "type", reg_type, sizeof(reg_type));

    if (!reg_type[0]) strcpy(reg_type, "REG_SZ");

    log_msg("registry_write: key=%s value=%s type=%s", key, value_name[0] ? value_name : "(Default)", reg_type);

    char *sep = strchr(key, '\\');
    if (!sep) {
        send_error(sock, "Invalid key format (expected ROOT\\SubKey)");
        return;
    }
    *sep = '\0';
    const char *subkey = sep + 1;
    HKEY root = parse_root_key(key);
    *sep = '\\';
    if (!root) {
        send_error(sock, "Unknown root key");
        return;
    }

    HKEY hkey;
    LONG rc = RegCreateKeyExA(root, subkey, 0, NULL, 0, KEY_WRITE, NULL, &hkey, NULL);
    if (rc != ERROR_SUCCESS) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to open/create key (error %ld)", rc);
        send_error(sock, msg);
        return;
    }

    if (_stricmp(reg_type, "REG_DWORD") == 0) {
        json_get_int(json, "data", &dword_data);
        DWORD dval = (DWORD)dword_data;
        rc = RegSetValueExA(hkey, value_name[0] ? value_name : NULL, 0, REG_DWORD,
                            (const BYTE *)&dval, sizeof(dval));
    } else if (_stricmp(reg_type, "REG_EXPAND_SZ") == 0) {
        json_get_string(json, "data", str_data, sizeof(str_data));
        rc = RegSetValueExA(hkey, value_name[0] ? value_name : NULL, 0, REG_EXPAND_SZ,
                            (const BYTE *)str_data, (DWORD)(strlen(str_data) + 1));
    } else {
        /* Default: REG_SZ */
        json_get_string(json, "data", str_data, sizeof(str_data));
        rc = RegSetValueExA(hkey, value_name[0] ? value_name : NULL, 0, REG_SZ,
                            (const BYTE *)str_data, (DWORD)(strlen(str_data) + 1));
    }
    RegCloseKey(hkey);

    if (rc != ERROR_SUCCESS) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to write value (error %ld)", rc);
        send_error(sock, msg);
        return;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"data\":{\"written\":true}}");
    send_line(sock, buf);
}

/* ================================================================
 * Command: registry_list
 *
 * Enumerate subkeys and values under a registry key.
 * ================================================================ */

void cmd_registry_list(SOCKET sock, const char *json)
{
    char key[1024] = {0};

    if (!json_get_string(json, "key", key, sizeof(key))) {
        send_error(sock, "Missing 'key' parameter");
        return;
    }

    log_msg("registry_list: key=%s", key);

    char *sep = strchr(key, '\\');
    if (!sep) {
        send_error(sock, "Invalid key format (expected ROOT\\SubKey)");
        return;
    }
    *sep = '\0';
    const char *subkey = sep + 1;
    HKEY root = parse_root_key(key);
    *sep = '\\';
    if (!root) {
        send_error(sock, "Unknown root key");
        return;
    }

    HKEY hkey;
    LONG rc = RegOpenKeyExA(root, subkey, 0, KEY_READ, &hkey);
    if (rc != ERROR_SUCCESS) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to open key (error %ld)", rc);
        send_error(sock, msg);
        return;
    }

    char *response = (char *)malloc(MAX_RESPONSE);
    char key_esc[2048];
    json_escape(key, key_esc, sizeof(key_esc));
    int off = snprintf(response, MAX_RESPONSE,
                       "{\"status\":\"ok\",\"data\":{\"key\":\"%s\",\"subkeys\":[",
                       key_esc);

    /* Enumerate subkeys */
    char name_buf[256];
    DWORD name_len;
    for (DWORD i = 0; ; i++) {
        name_len = sizeof(name_buf);
        rc = RegEnumKeyExA(hkey, i, name_buf, &name_len, NULL, NULL, NULL, NULL);
        if (rc != ERROR_SUCCESS) break;
        char name_esc[512];
        json_escape(name_buf, name_esc, sizeof(name_esc));
        if (i > 0 && off < MAX_RESPONSE - 2) response[off++] = ',';
        off += snprintf(response + off, MAX_RESPONSE - off, "\"%s\"", name_esc);
    }

    off += snprintf(response + off, MAX_RESPONSE - off, "],\"values\":[");

    /* Enumerate values */
    for (DWORD i = 0; ; i++) {
        name_len = sizeof(name_buf);
        DWORD type = 0;
        DWORD data_size = 0;
        rc = RegEnumValueA(hkey, i, name_buf, &name_len, NULL, &type, NULL, &data_size);
        if (rc != ERROR_SUCCESS) break;
        char name_esc[512];
        json_escape(name_buf, name_esc, sizeof(name_esc));
        if (i > 0 && off < MAX_RESPONSE - 2) response[off++] = ',';
        off += snprintf(response + off, MAX_RESPONSE - off,
                        "{\"name\":\"%s\",\"type\":\"%s\",\"size\":%lu}",
                        name_esc, reg_type_name(type), (unsigned long)data_size);
    }

    snprintf(response + off, MAX_RESPONSE - off, "]}}");
    RegCloseKey(hkey);

    send_line(sock, response);
    free(response);
}
