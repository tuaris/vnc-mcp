/*
 * vnc-helper-uia.c — Native UI Automation commands via COM
 *
 * Direct IUIAutomation COM calls — no PowerShell, no .ps1 dependency.
 * Replaces the previous PowerShell wrapper (vnc-uia.ps1).
 *
 * Copyright (c) 2026, The Daniel Morante Company, Inc.
 * BSD 2-Clause License
 */

#define COBJMACROS
#include <initguid.h>

#include "vnc-helper.h"

#include <ole2.h>
#include <oleauto.h>
#include <uiautomationclient.h>

/* ================================================================
 * BSTR / WCHAR helpers
 * ================================================================ */

/* Convert a BSTR (or any WCHAR*) to UTF-8 into a caller-owned buffer.
 * Returns the number of bytes written (excluding NUL), or 0 on failure. */
static int bstr_to_utf8(const BSTR bstr, char *out, int out_max)
{
    if (!bstr || !bstr[0]) { out[0] = '\0'; return 0; }
    int len = WideCharToMultiByte(CP_UTF8, 0, bstr, -1, out, out_max, NULL, NULL);
    if (len <= 0) { out[0] = '\0'; return 0; }
    return len - 1; /* exclude NUL */
}

/* Convert a UTF-8 string to a newly allocated BSTR. Caller must SysFreeString(). */
static BSTR utf8_to_bstr(const char *utf8)
{
    if (!utf8 || !utf8[0]) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    BSTR bstr = SysAllocStringLen(NULL, wlen - 1);
    if (!bstr) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, bstr, wlen);
    return bstr;
}

/* ================================================================
 * Control type ID → human-readable name
 * ================================================================ */

static const char *control_type_name(CONTROLTYPEID ct)
{
    switch (ct) {
    case UIA_ButtonControlTypeId:      return "Button";
    case UIA_CheckBoxControlTypeId:    return "CheckBox";
    case UIA_ComboBoxControlTypeId:    return "ComboBox";
    case UIA_EditControlTypeId:        return "Edit";
    case UIA_ListItemControlTypeId:    return "ListItem";
    case UIA_MenuItemControlTypeId:    return "MenuItem";
    case UIA_RadioButtonControlTypeId: return "RadioButton";
    case UIA_TextControlTypeId:        return "Text";
    case UIA_TreeItemControlTypeId:    return "TreeItem";
    case UIA_WindowControlTypeId:      return "Window";
    case 50001:                        return "Calendar";
    case 50005:                        return "Hyperlink";
    case 50006:                        return "Image";
    case 50008:                        return "List";
    case 50009:                        return "Menu";
    case 50010:                        return "MenuBar";
    case 50012:                        return "ProgressBar";
    case 50014:                        return "ScrollBar";
    case 50015:                        return "Slider";
    case 50016:                        return "Spinner";
    case 50017:                        return "StatusBar";
    case 50018:                        return "Tab";
    case 50019:                        return "TabItem";
    case 50021:                        return "ToolBar";
    case 50022:                        return "ToolTip";
    case 50023:                        return "Tree";
    case 50025:                        return "Custom";
    case 50026:                        return "Group";
    case 50027:                        return "Thumb";
    case 50028:                        return "DataGrid";
    case 50029:                        return "DataItem";
    case 50030:                        return "Document";
    case 50031:                        return "SplitButton";
    case 50033:                        return "Pane";
    case 50034:                        return "Header";
    case 50035:                        return "HeaderItem";
    case 50036:                        return "Table";
    case 50037:                        return "TitleBar";
    case 50038:                        return "Separator";
    case 50039:                        return "SemanticZoom";
    case 50040:                        return "AppBar";
    default:                           return "Unknown";
    }
}

/* Parse a control type name string to its ID. Returns 0 if unknown. */
static CONTROLTYPEID control_type_from_name(const char *name)
{
    if (!name || !name[0]) return 0;
    struct { const char *n; CONTROLTYPEID id; } map[] = {
        {"Button",      UIA_ButtonControlTypeId},
        {"CheckBox",    UIA_CheckBoxControlTypeId},
        {"ComboBox",    UIA_ComboBoxControlTypeId},
        {"Edit",        UIA_EditControlTypeId},
        {"ListItem",    UIA_ListItemControlTypeId},
        {"MenuItem",    UIA_MenuItemControlTypeId},
        {"RadioButton", UIA_RadioButtonControlTypeId},
        {"Text",        UIA_TextControlTypeId},
        {"TreeItem",    UIA_TreeItemControlTypeId},
        {"Window",      UIA_WindowControlTypeId},
        {"Menu",        50009}, {"MenuBar", 50010},
        {"ToolBar",     50021}, {"ToolTip", 50022},
        {"Tree",        50023}, {"Pane",    50033},
        {"TitleBar",    50037}, {"Tab",     50018},
        {"TabItem",     50019}, {"Group",   50026},
        {"List",        50008}, {"DataGrid",50028},
        {"Document",    50030}, {"Hyperlink",50005},
        {"Image",       50006}, {"Header",  50034},
        {"HeaderItem",  50035}, {"Table",   50036},
        {"ScrollBar",   50014}, {"Slider",  50015},
        {"StatusBar",   50017}, {"ProgressBar", 50012},
        {"SplitButton", 50031}, {"Separator",   50038},
        {"Custom",      50025}, {"Spinner", 50016},
    };
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
        if (_stricmp(name, map[i].n) == 0) return map[i].id;
    }
    return 0;
}

/* ================================================================
 * COM session — create/release IUIAutomation per call
 * ================================================================ */

typedef struct {
    IUIAutomation *uia;
    int com_inited;
} UiaSession;

static int uia_begin(UiaSession *s)
{
    memset(s, 0, sizeof(*s));
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE)
        s->com_inited = 1;
    else {
        /* Try STA as fallback */
        hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr) || hr == S_FALSE)
            s->com_inited = 1;
        else
            return 0;
    }
    hr = CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUIAutomation, (void **)&s->uia);
    if (FAILED(hr) || !s->uia) return 0;
    return 1;
}

static void uia_end(UiaSession *s)
{
    if (s->uia) { IUIAutomation_Release(s->uia); s->uia = NULL; }
    if (s->com_inited) { CoUninitialize(); s->com_inited = 0; }
}

/* ================================================================
 * Element info → JSON fragment (appended to a buffer)
 * ================================================================ */

static int append_element_info(IUIAutomationElement *el, char *buf, int pos, int max)
{
    BSTR bname = NULL, baid = NULL, bcls = NULL;
    CONTROLTYPEID ct = 0;
    WINBOOL enabled = FALSE;
    RECT rect = {0};

    IUIAutomationElement_get_CurrentName(el, &bname);
    IUIAutomationElement_get_CurrentAutomationId(el, &baid);
    IUIAutomationElement_get_CurrentClassName(el, &bcls);
    IUIAutomationElement_get_CurrentControlType(el, &ct);
    IUIAutomationElement_get_CurrentIsEnabled(el, &enabled);
    IUIAutomationElement_get_CurrentBoundingRectangle(el, &rect);

    char name_utf8[1024] = {0}, aid_utf8[512] = {0}, cls_utf8[512] = {0};
    bstr_to_utf8(bname, name_utf8, sizeof(name_utf8));
    bstr_to_utf8(baid, aid_utf8, sizeof(aid_utf8));
    bstr_to_utf8(bcls, cls_utf8, sizeof(cls_utf8));

    char name_esc[2048], aid_esc[1024], cls_esc[1024];
    json_escape(name_utf8, name_esc, sizeof(name_esc));
    json_escape(aid_utf8, aid_esc, sizeof(aid_esc));
    json_escape(cls_utf8, cls_esc, sizeof(cls_esc));

    pos += snprintf(buf + pos, max - pos,
        "{\"name\":\"%s\",\"controlType\":\"%s\",\"automationId\":\"%s\","
        "\"className\":\"%s\",\"isEnabled\":%s,"
        "\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld",
        name_esc, control_type_name(ct), aid_esc, cls_esc,
        enabled ? "true" : "false",
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);

    if (bname) SysFreeString(bname);
    if (baid)  SysFreeString(baid);
    if (bcls)  SysFreeString(bcls);

    return pos;
}

/* ================================================================
 * Recursive tree walk
 * ================================================================ */

static int walk_tree(IUIAutomationTreeWalker *walker, IUIAutomationElement *el,
                     int cur_depth, int max_depth, char *buf, int pos, int max)
{
    if (pos >= max - 128) return pos;

    pos = append_element_info(el, buf, pos, max);

    if (cur_depth < max_depth) {
        IUIAutomationElement *child = NULL;
        IUIAutomationTreeWalker_GetFirstChildElement(walker, el, &child);

        if (child) {
            pos += snprintf(buf + pos, max - pos, ",\"children\":[");
            int first = 1, count = 0;

            while (child && count < 100 && pos < max - 128) {
                if (!first) pos += snprintf(buf + pos, max - pos, ",");
                first = 0;
                pos = walk_tree(walker, child, cur_depth + 1, max_depth, buf, pos, max);
                IUIAutomationElement *next = NULL;
                IUIAutomationTreeWalker_GetNextSiblingElement(walker, child, &next);
                IUIAutomationElement_Release(child);
                child = next;
                count++;
            }
            if (child) IUIAutomationElement_Release(child);
            pos += snprintf(buf + pos, max - pos, "]");
        }
    }

    pos += snprintf(buf + pos, max - pos, "}");
    return pos;
}

/* ================================================================
 * Build condition from name/automationId/controlType
 * ================================================================ */

static IUIAutomationCondition *build_condition(IUIAutomation *uia,
    const char *name, const char *automation_id, const char *control_type_str)
{
    IUIAutomationCondition *conds[3] = {NULL};
    int n = 0;

    if (name[0]) {
        BSTR bname = utf8_to_bstr(name);
        if (bname) {
            VARIANT v; VariantInit(&v);
            v.vt = VT_BSTR; v.bstrVal = bname;
            IUIAutomation_CreatePropertyCondition(uia, UIA_NamePropertyId, v, &conds[n]);
            SysFreeString(bname);
            if (conds[n]) n++;
        }
    }

    if (automation_id[0]) {
        BSTR baid = utf8_to_bstr(automation_id);
        if (baid) {
            VARIANT v; VariantInit(&v);
            v.vt = VT_BSTR; v.bstrVal = baid;
            IUIAutomation_CreatePropertyCondition(uia, UIA_AutomationIdPropertyId, v, &conds[n]);
            SysFreeString(baid);
            if (conds[n]) n++;
        }
    }

    if (control_type_str[0]) {
        CONTROLTYPEID ctid = control_type_from_name(control_type_str);
        if (ctid) {
            VARIANT v; VariantInit(&v);
            v.vt = VT_I4; v.lVal = (LONG)ctid;
            IUIAutomation_CreatePropertyCondition(uia, UIA_ControlTypePropertyId, v, &conds[n]);
            if (conds[n]) n++;
        }
    }

    if (n == 0) return NULL;
    if (n == 1) return conds[0];

    /* Combine with AND */
    IUIAutomationCondition *combined = NULL;
    if (n == 2) {
        IUIAutomation_CreateAndCondition(uia, conds[0], conds[1], &combined);
        IUIAutomationCondition_Release(conds[0]);
        IUIAutomationCondition_Release(conds[1]);
    } else {
        IUIAutomationCondition *tmp = NULL;
        IUIAutomation_CreateAndCondition(uia, conds[0], conds[1], &tmp);
        IUIAutomationCondition_Release(conds[0]);
        IUIAutomationCondition_Release(conds[1]);
        if (tmp) {
            IUIAutomation_CreateAndCondition(uia, tmp, conds[2], &combined);
            IUIAutomationCondition_Release(tmp);
        }
        IUIAutomationCondition_Release(conds[2]);
    }

    return combined;
}

/* ================================================================
 * Command: ui_tree
 *
 * Returns the accessibility tree of the foreground window (or a
 * specific PID). Native COM — no PowerShell.
 * ================================================================ */

void cmd_ui_tree(SOCKET sock, const char *json)
{
    int depth = 3, pid = 0;
    json_get_int(json, "depth", &depth);
    json_get_int(json, "pid", &pid);
    if (depth < 1) depth = 1;
    if (depth > 10) depth = 10;

    log_msg("ui_tree: depth=%d pid=%d", depth, pid);

    UiaSession s;
    if (!uia_begin(&s)) {
        send_error(sock, "Failed to initialize UI Automation COM");
        return;
    }

    IUIAutomationElement *root = NULL;

    if (pid > 0) {
        /* Find top-level window by PID */
        VARIANT v; VariantInit(&v);
        v.vt = VT_I4; v.lVal = pid;
        IUIAutomationCondition *cond = NULL;
        IUIAutomation_CreatePropertyCondition(s.uia, UIA_ProcessIdPropertyId, v, &cond);
        if (cond) {
            IUIAutomationElement *desktop = NULL;
            IUIAutomation_GetRootElement(s.uia, &desktop);
            if (desktop) {
                IUIAutomationElement_FindFirst(desktop, TreeScope_Children, cond, &root);
                IUIAutomationElement_Release(desktop);
            }
            IUIAutomationCondition_Release(cond);
        }
        if (!root) {
            char msg[128];
            snprintf(msg, sizeof(msg), "No window found for PID %d", pid);
            send_error(sock, msg);
            uia_end(&s);
            return;
        }
    } else {
        /* Use the foreground window */
        HWND fg = GetForegroundWindow();
        if (fg) {
            HRESULT hr = IUIAutomation_ElementFromHandle(s.uia, (UIA_HWND)fg, &root);
            if (FAILED(hr) || !root)
                IUIAutomation_GetRootElement(s.uia, &root);
        } else {
            IUIAutomation_GetRootElement(s.uia, &root);
        }
    }

    if (!root) {
        send_error(sock, "Could not obtain root element");
        uia_end(&s);
        return;
    }

    IUIAutomationTreeWalker *walker = NULL;
    IUIAutomation_get_ControlViewWalker(s.uia, &walker);
    if (!walker) {
        send_error(sock, "Could not obtain ControlViewWalker");
        IUIAutomationElement_Release(root);
        uia_end(&s);
        return;
    }

    char *buf = (char *)malloc(MAX_RESPONSE);
    if (!buf) {
        send_error(sock, "Out of memory");
        IUIAutomationTreeWalker_Release(walker);
        IUIAutomationElement_Release(root);
        uia_end(&s);
        return;
    }

    int pos = walk_tree(walker, root, 0, depth, buf, 0, MAX_RESPONSE - 256);

    IUIAutomationTreeWalker_Release(walker);
    IUIAutomationElement_Release(root);

    char *response = (char *)malloc(MAX_RESPONSE);
    snprintf(response, MAX_RESPONSE, "{\"status\":\"ok\",\"data\":%s}", buf);
    send_line(sock, response);

    free(buf);
    free(response);
    uia_end(&s);
}

/* ================================================================
 * Command: ui_element_text
 *
 * Find an element by name/automationId and return its text/value.
 * Tries ValuePattern, then TextPattern, then falls back to Name.
 * ================================================================ */

void cmd_ui_element_text(SOCKET sock, const char *json)
{
    char name[512] = {0}, automation_id[512] = {0}, control_type[128] = {0};
    json_get_string(json, "name", name, sizeof(name));
    json_get_string(json, "automation_id", automation_id, sizeof(automation_id));
    json_get_string(json, "control_type", control_type, sizeof(control_type));

    if (!name[0] && !automation_id[0]) {
        send_error(sock, "Must specify 'name' or 'automation_id'");
        return;
    }

    log_msg("ui_element_text: name=%s aid=%s ct=%s", name, automation_id, control_type);

    UiaSession s;
    if (!uia_begin(&s)) {
        send_error(sock, "Failed to initialize UI Automation COM");
        return;
    }

    IUIAutomationCondition *cond = build_condition(s.uia, name, automation_id, control_type);
    if (!cond) {
        send_error(sock, "Failed to build search condition");
        uia_end(&s);
        return;
    }

    IUIAutomationElement *desktop = NULL;
    IUIAutomation_GetRootElement(s.uia, &desktop);
    if (!desktop) {
        send_error(sock, "Failed to get desktop element");
        IUIAutomationCondition_Release(cond);
        uia_end(&s);
        return;
    }

    IUIAutomationElement *found = NULL;
    IUIAutomationElement_FindFirst(desktop, TreeScope_Descendants, cond, &found);
    IUIAutomationElement_Release(desktop);
    IUIAutomationCondition_Release(cond);

    if (!found) {
        send_error(sock, "Element not found");
        uia_end(&s);
        return;
    }

    /* Try ValuePattern first */
    char text_utf8[8192] = {0};
    int got_text = 0;

    IUnknown *pat_unk = NULL;
    HRESULT hr = IUIAutomationElement_GetCurrentPattern(found, UIA_ValuePatternId, &pat_unk);
    if (SUCCEEDED(hr) && pat_unk) {
        IUIAutomationValuePattern *vp = NULL;
        IUnknown_QueryInterface(pat_unk, &IID_IUIAutomationValuePattern, (void **)&vp);
        IUnknown_Release(pat_unk);
        if (vp) {
            BSTR val = NULL;
            IUIAutomationValuePattern_get_CurrentValue(vp, &val);
            if (val) {
                bstr_to_utf8(val, text_utf8, sizeof(text_utf8));
                SysFreeString(val);
                got_text = 1;
            }
            IUIAutomationValuePattern_Release(vp);
        }
    }

    /* Try TextPattern if ValuePattern didn't produce text */
    if (!got_text) {
        pat_unk = NULL;
        hr = IUIAutomationElement_GetCurrentPattern(found, UIA_TextPatternId, &pat_unk);
        if (SUCCEEDED(hr) && pat_unk) {
            IUIAutomationTextPattern *tp = NULL;
            IUnknown_QueryInterface(pat_unk, &IID_IUIAutomationTextPattern, (void **)&tp);
            IUnknown_Release(pat_unk);
            if (tp) {
                IUIAutomationTextRange *range = NULL;
                IUIAutomationTextPattern_get_DocumentRange(tp, &range);
                if (range) {
                    BSTR val = NULL;
                    IUIAutomationTextRange_GetText(range, -1, &val);
                    if (val) {
                        bstr_to_utf8(val, text_utf8, sizeof(text_utf8));
                        SysFreeString(val);
                        got_text = 1;
                    }
                    IUIAutomationTextRange_Release(range);
                }
                IUIAutomationTextPattern_Release(tp);
            }
        }
    }

    /* Fall back to Name property */
    if (!got_text) {
        BSTR bname = NULL;
        IUIAutomationElement_get_CurrentName(found, &bname);
        if (bname) {
            bstr_to_utf8(bname, text_utf8, sizeof(text_utf8));
            SysFreeString(bname);
            got_text = 1;
        }
    }

    /* Build response with element info + text */
    char *buf = (char *)malloc(MAX_RESPONSE);
    int pos = 0;
    char text_esc[16384];
    json_escape(text_utf8, text_esc, sizeof(text_esc));

    pos += snprintf(buf + pos, MAX_RESPONSE - pos,
                    "{\"status\":\"ok\",\"data\":{\"text\":\"%s\",\"element\":", text_esc);
    pos = append_element_info(found, buf, pos, MAX_RESPONSE - 64);
    pos += snprintf(buf + pos, MAX_RESPONSE - pos, "}}}");

    send_line(sock, buf);
    free(buf);

    IUIAutomationElement_Release(found);
    uia_end(&s);
}

/* ================================================================
 * Command: ui_click_element
 *
 * Find an element and invoke its default action. Tries patterns in
 * order: Invoke, Toggle, SelectionItem, ExpandCollapse, SetFocus.
 * ================================================================ */

void cmd_ui_click_element(SOCKET sock, const char *json)
{
    char name[512] = {0}, automation_id[512] = {0}, control_type[128] = {0};
    json_get_string(json, "name", name, sizeof(name));
    json_get_string(json, "automation_id", automation_id, sizeof(automation_id));
    json_get_string(json, "control_type", control_type, sizeof(control_type));

    if (!name[0] && !automation_id[0]) {
        send_error(sock, "Must specify 'name' or 'automation_id'");
        return;
    }

    log_msg("ui_click_element: name=%s aid=%s ct=%s", name, automation_id, control_type);

    UiaSession s;
    if (!uia_begin(&s)) {
        send_error(sock, "Failed to initialize UI Automation COM");
        return;
    }

    IUIAutomationCondition *cond = build_condition(s.uia, name, automation_id, control_type);
    if (!cond) {
        send_error(sock, "Failed to build search condition");
        uia_end(&s);
        return;
    }

    IUIAutomationElement *desktop = NULL;
    IUIAutomation_GetRootElement(s.uia, &desktop);
    if (!desktop) {
        send_error(sock, "Failed to get desktop element");
        IUIAutomationCondition_Release(cond);
        uia_end(&s);
        return;
    }

    IUIAutomationElement *found = NULL;
    IUIAutomationElement_FindFirst(desktop, TreeScope_Descendants, cond, &found);
    IUIAutomationElement_Release(desktop);
    IUIAutomationCondition_Release(cond);

    if (!found) {
        send_error(sock, "Element not found");
        uia_end(&s);
        return;
    }

    const char *action = NULL;
    IUnknown *pat_unk = NULL;
    HRESULT hr;

    /* 1. InvokePattern (buttons, links, menu items) */
    hr = IUIAutomationElement_GetCurrentPattern(found, UIA_InvokePatternId, &pat_unk);
    if (SUCCEEDED(hr) && pat_unk) {
        IUIAutomationInvokePattern *ip = NULL;
        IUnknown_QueryInterface(pat_unk, &IID_IUIAutomationInvokePattern, (void **)&ip);
        IUnknown_Release(pat_unk);
        if (ip) {
            hr = IUIAutomationInvokePattern_Invoke(ip);
            IUIAutomationInvokePattern_Release(ip);
            if (SUCCEEDED(hr)) action = "invoked";
        }
    }

    /* 2. TogglePattern (checkboxes) */
    if (!action) {
        pat_unk = NULL;
        hr = IUIAutomationElement_GetCurrentPattern(found, UIA_TogglePatternId, &pat_unk);
        if (SUCCEEDED(hr) && pat_unk) {
            IUIAutomationTogglePattern *tp = NULL;
            IUnknown_QueryInterface(pat_unk, &IID_IUIAutomationTogglePattern, (void **)&tp);
            IUnknown_Release(pat_unk);
            if (tp) {
                hr = IUIAutomationTogglePattern_Toggle(tp);
                IUIAutomationTogglePattern_Release(tp);
                if (SUCCEEDED(hr)) action = "toggled";
            }
        }
    }

    /* 3. SelectionItemPattern (radio buttons, list items) */
    if (!action) {
        pat_unk = NULL;
        hr = IUIAutomationElement_GetCurrentPattern(found, UIA_SelectionItemPatternId, &pat_unk);
        if (SUCCEEDED(hr) && pat_unk) {
            IUIAutomationSelectionItemPattern *sp = NULL;
            IUnknown_QueryInterface(pat_unk, &IID_IUIAutomationSelectionItemPattern, (void **)&sp);
            IUnknown_Release(pat_unk);
            if (sp) {
                hr = IUIAutomationSelectionItemPattern_Select(sp);
                IUIAutomationSelectionItemPattern_Release(sp);
                if (SUCCEEDED(hr)) action = "selected";
            }
        }
    }

    /* 4. ExpandCollapsePattern (dropdowns, tree items) */
    if (!action) {
        pat_unk = NULL;
        hr = IUIAutomationElement_GetCurrentPattern(found, UIA_ExpandCollapsePatternId, &pat_unk);
        if (SUCCEEDED(hr) && pat_unk) {
            IUIAutomationExpandCollapsePattern *ep = NULL;
            IUnknown_QueryInterface(pat_unk, &IID_IUIAutomationExpandCollapsePattern, (void **)&ep);
            IUnknown_Release(pat_unk);
            if (ep) {
                enum ExpandCollapseState state;
                IUIAutomationExpandCollapsePattern_get_CurrentExpandCollapseState(ep, &state);
                if (state == ExpandCollapseState_Collapsed) {
                    hr = IUIAutomationExpandCollapsePattern_Expand(ep);
                    if (SUCCEEDED(hr)) action = "expanded";
                } else {
                    hr = IUIAutomationExpandCollapsePattern_Collapse(ep);
                    if (SUCCEEDED(hr)) action = "collapsed";
                }
                IUIAutomationExpandCollapsePattern_Release(ep);
            }
        }
    }

    /* 5. Fallback: SetFocus */
    if (!action) {
        hr = IUIAutomationElement_SetFocus(found);
        if (SUCCEEDED(hr))
            action = "focused";
        else
            action = "none";
    }

    /* Build response */
    char *buf = (char *)malloc(MAX_RESPONSE);
    int pos = 0;
    pos += snprintf(buf + pos, MAX_RESPONSE - pos,
                    "{\"status\":\"ok\",\"data\":{\"action\":\"%s\",\"element\":", action);
    pos = append_element_info(found, buf, pos, MAX_RESPONSE - 64);
    pos += snprintf(buf + pos, MAX_RESPONSE - pos, "}}}");

    send_line(sock, buf);
    free(buf);

    IUIAutomationElement_Release(found);
    uia_end(&s);
}
