// NekoWaShock — 64-bit VST2 rebuild of the original "YOU WA SHOCK !" Winamp DSP plug-in
// 功能等价于原 32 位 Winamp DSP 插件：
//   Volume / Panning / Stereo Separation / Left & Right Volume / Saturation
// 原插件为 Winamp DSP 2.x（winampDSPGetHeader2），非 VST；
// 本文件为一个原生 64 位 VST2 插件，实现相同的音频功能，并带自绘界面。

#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef _WIN32
  #define VST_EXPORT extern "C" __declspec(dllexport)
#else
  #define VST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

typedef int32_t VstInt32;
typedef intptr_t VstIntPtr;

// ---------- VST2 AEffect（公开接口） ----------
enum {
    kEffectMagic = 0x56737450, // 'VstP'
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3,
    effSetProgramName = 4, effGetProgramName = 5,
    effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12,
    effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effEditIdle = 19,
    effGetChunk = 23, effSetChunk = 24,
    effGetEffectName = 45, effGetVendorString = 47,
    effGetProductString = 48, effGetVendorVersion = 49, effGetVstVersion = 50,
    effCanDo = 51
};

enum {
    effFlagsHasEditor    = 1 << 0,
    effFlagsCanReplacing = 1 << 4
};

struct AEffect {
    VstInt32 magic;
    VstIntPtr (*dispatcher)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
    void (*process)(AEffect*, float**, float**, VstInt32);
    void (*setParameter)(AEffect*, VstInt32, float);
    float (*getParameter)(AEffect*, VstInt32);
    VstInt32 numPrograms, numParams, numInputs, numOutputs, flags;
    VstIntPtr resvd1, resvd2;
    VstInt32 initialDelay;
    VstInt32 realQualities, offQualities;
    float ioRatio;
    void* object;
    void* user;
    VstInt32 uniqueID, version;
    void (*processReplacing)(AEffect*, float**, float**, VstInt32);
    void (*processDoubleReplacing)(AEffect*, double**, double**, VstInt32);
    char future[56];
};

struct ERect { short top, left, bottom, right; };

// ---------- 参数 ----------
enum {
    P_VOLUME = 0,
    P_PAN,
    P_SEPARATION,
    P_RIGHT_VOLUME,
    P_LEFT_VOLUME,
    P_CIRC_PAN,
    P_SATURATION,
    NUM_PARAMS
};

struct SliderDef { const char* name; const wchar_t* cn; float lo, hi; int param; int cx, cy, r; };

// 按原版窗体的布局排列：Sep / Pan / Vol 竖直排列，LVol 与 RVol 在底部并列
static const SliderDef g_sliders[NUM_PARAMS] = {
    { "Volume",  L"音量",     -2.0f, 2.0f, P_VOLUME,        120, 234, 20 },
    { "Panning", L"声像",     -1.0f, 1.0f, P_PAN,           120, 166, 18 },
    { "Sep",     L"立体声",   -1.0f, 1.0f, P_SEPARATION,    120,  98, 18 },
    { "RVolume", L"右声道",   -2.0f, 2.0f, P_RIGHT_VOLUME,  180, 300, 18 },
    { "LVolume", L"左声道",   -2.0f, 2.0f, P_LEFT_VOLUME,    60, 300, 18 },
    { "CircPan", L"圆形声像",  0.0f, 1.0f, P_CIRC_PAN,        0,   0,  0 },
    { "Satur",   L"饱和模式",  0.0f, 3.0f, P_SATURATION,      0,   0,  0 }
};

static int g_dragY = 0;
static float g_dragT = 0.0f;

struct Plugin {
    float sampleRate = 44100.0f;
    VstInt32 blockSize = 512;
    float param[NUM_PARAMS];

    float curGainL = 0.0f;
    float curGainR = 0.0f;

    Plugin() {
        param[P_VOLUME]       = 1.0f;  // 0 dB（原插件默认值）
        param[P_PAN]          = 0.0f;
        param[P_SEPARATION]   = 0.0f;
        param[P_RIGHT_VOLUME] = 1.0f;
        param[P_LEFT_VOLUME]  = 1.0f;
        param[P_CIRC_PAN]     = 0.0f;
        param[P_SATURATION]   = 0.0f;  // A = Default
    }

    // 原插件旋钮范围 -2..+2，默认 1 = unity
    static float gainOf(float v) {
        if (v <= -2.0f) v = -2.0f;
        if (v >= 2.0f) v = 2.0f;
        return v;
    }

    void updateGains(float& gl, float& gr) const {
        float vol  = gainOf(param[P_VOLUME]);
        float lvol = gainOf(param[P_LEFT_VOLUME]);
        float rvol = gainOf(param[P_RIGHT_VOLUME]);
        float pan  = param[P_PAN];
        if (pan < -1.0f) pan = -1.0f;
        if (pan >  1.0f) pan =  1.0f;

        float pl, pr;
        if (param[P_CIRC_PAN] >= 0.5f) {
            const float kPi4 = 0.78539816339f;
            pl = std::cos((pan + 1.0f) * kPi4);
            pr = std::sin((pan + 1.0f) * kPi4);
        } else {
            // 平衡式：居中不衰减
            pl = 1.0f - (pan > 0.0f ? pan : 0.0f);
            pr = 1.0f + (pan < 0.0f ? pan : 0.0f);
        }

        gl = vol * lvol * pl;
        gr = vol * rvol * pr;
    }
};

// ---------- GUI ----------
typedef VstIntPtr (*HostCallbackFn)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);

static HostCallbackFn g_host = nullptr;
static AEffect* g_effect = nullptr;
static HWND g_hwnd = nullptr;
static int g_drag = -1;
static bool g_classReady = false;
static ERect g_rect = { 0, 0, 420, 240 };

static const int kWidth = 240;
static const int kHeight = 420;

static void notifyHost(Plugin* p, int idx) {
    if (g_host && g_effect) {
        // opcode 1 = audioMasterAutomate
        g_host(g_effect, 1, idx, 0, nullptr, p->param[idx]);
    }
}

static void drawKnob(HDC dc, int cx, int cy, int r, float t, bool hot) {
    // 底座
    HBRUSH outer = CreateSolidBrush(RGB(0x2C, 0x2C, 0x2C));
    HBRUSH ob = (HBRUSH)SelectObject(dc, outer);
    HPEN op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(outer);

    // 内盘
    int ir = r - 6;
    HBRUSH inner = CreateSolidBrush(hot ? RGB(0x3E, 0x2A, 0x2A) : RGB(0x1F, 0x1F, 0x1F));
    ob = (HBRUSH)SelectObject(dc, inner);
    HPEN ip = CreatePen(PS_SOLID, 1, RGB(0x4A, 0x4A, 0x4A));
    op = (HPEN)SelectObject(dc, ip);
    Ellipse(dc, cx - ir, cy - ir, cx + ir, cy + ir);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(ip);
    DeleteObject(inner);

    // 指针：-135° .. +135°
    float ang = (-135.0f + t * 270.0f) * 3.14159265f / 180.0f;
    int px = cx + (int)(sinf(ang) * (ir - 2));
    int py = cy - (int)(cosf(ang) * (ir - 2));
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(0xE0, 0x40, 0x40));
    op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, cx, cy, nullptr);
    LineTo(dc, px, py);
    SelectObject(dc, op);
    DeleteObject(pen);

    // 中心轴
    HBRUSH hub = CreateSolidBrush(RGB(0xE0, 0x40, 0x40));
    ob = (HBRUSH)SelectObject(dc, hub);
    HPEN hp = CreatePen(PS_SOLID, 1, RGB(0xE0, 0x40, 0x40));
    op = (HPEN)SelectObject(dc, hp);
    Ellipse(dc, cx - 2, cy - 2, cx + 3, cy + 3);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(hp);
    DeleteObject(hub);
}

static void paintEditor(HWND h, HDC dc, Plugin* p) {
    RECT full = { 0, 0, kWidth, kHeight };
    HBRUSH bg = CreateSolidBrush(RGB(0x18, 0x18, 0x18));
    FillRect(dc, &full, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT old = (HFONT)SelectObject(dc, font);

    // 标题
    SetTextColor(dc, RGB(0xE8, 0x44, 0x44));
    RECT tr = { 12, 10, kWidth - 12, 32 };
    DrawTextW(dc, L"NekoWaShock", -1, &tr, DT_LEFT | DT_SINGLELINE);

    HPEN sepLine = CreatePen(PS_SOLID, 1, RGB(0x38, 0x38, 0x38));
    HPEN oldPen = (HPEN)SelectObject(dc, sepLine);
    MoveToEx(dc, 12, 40, nullptr);
    LineTo(dc, kWidth - 12, 40);
    SelectObject(dc, oldPen);
    DeleteObject(sepLine);

    for (int i = 0; i < NUM_PARAMS; i++) {
        const SliderDef& s = g_sliders[i];
        if (s.param == P_CIRC_PAN || s.param == P_SATURATION) continue;

        float v = p->param[s.param];
        float t = (v - s.lo) / (s.hi - s.lo);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        drawKnob(dc, s.cx, s.cy, s.r, t, g_drag == i);

        // 标签行：中文名 + 当前值
        wchar_t line[64];
        swprintf(line, 64, L"%s  %.2f", s.cn, v);
        SetTextColor(dc, RGB(0xD0, 0xD0, 0xD0));
        RECT lr = { s.cx - 46, s.cy - s.r - 21, s.cx + 46, s.cy - s.r - 3 };
        DrawTextW(dc, line, -1, &lr, DT_CENTER | DT_SINGLELINE);
    }

    // 底部 L / R 声道标记
    SetTextColor(dc, RGB(0x88, 0x88, 0x88));
    RECT lL = { 14, 293, 38, 313 };
    DrawTextW(dc, L"L", -1, &lL, DT_CENTER | DT_SINGLELINE);
    RECT lR = { 202, 293, 226, 313 };
    DrawTextW(dc, L"R", -1, &lR, DT_CENTER | DT_SINGLELINE);

    HPEN sepLine2 = CreatePen(PS_SOLID, 1, RGB(0x38, 0x38, 0x38));
    oldPen = (HPEN)SelectObject(dc, sepLine2);
    MoveToEx(dc, 12, 334, nullptr);
    LineTo(dc, kWidth - 12, 334);
    SelectObject(dc, oldPen);
    DeleteObject(sepLine2);

    // Circular panning law 复选框
    int cy = 348;
    bool on = p->param[P_CIRC_PAN] >= 0.5f;
    RECT box = { 30, cy, 46, cy + 16 };
    HBRUSH fb = CreateSolidBrush(RGB(0x2E, 0x2E, 0x2E));
    FillRect(dc, &box, fb);
    DeleteObject(fb);
    HBRUSH edge = CreateSolidBrush(on ? RGB(0xE0, 0x40, 0x40) : RGB(0x66, 0x66, 0x66));
    FrameRect(dc, &box, edge);
    DeleteObject(edge);
    if (on) {
        RECT in = { 34, cy + 4, 42, cy + 12 };
        HBRUSH kb = CreateSolidBrush(RGB(0xE0, 0x40, 0x40));
        FillRect(dc, &in, kb);
        DeleteObject(kb);
    }
    SetTextColor(dc, on ? RGB(0xE8, 0x44, 0x44) : RGB(0xA0, 0xA0, 0xA0));
    RECT cr = { 54, cy - 1, kWidth - 10, cy + 18 };
    DrawTextW(dc, L"Circular panning law", -1, &cr, DT_LEFT | DT_SINGLELINE);

    // 饱和模式 A B C D（原版 saturation mode 选择）
    SetTextColor(dc, RGB(0x90, 0x90, 0x90));
    RECT sl = { 14, 372, 62, 392 };
    DrawTextW(dc, L"饱和", -1, &sl, DT_LEFT | DT_SINGLELINE);

    int satMode = (int)(p->param[P_SATURATION] + 0.5f);
    if (satMode < 0) satMode = 0;
    if (satMode > 3) satMode = 3;

    for (int i = 0; i < 4; i++) {
        int bx = 70 + i * 40;
        RECT b = { bx, 370, bx + 34, 394 };
        bool sel = (satMode == i);
        HBRUSH bb = CreateSolidBrush(sel ? RGB(0xE0, 0x40, 0x40) : RGB(0x2A, 0x2A, 0x2A));
        FillRect(dc, &b, bb);
        DeleteObject(bb);
        HBRUSH be = CreateSolidBrush(sel ? RGB(0xE0, 0x40, 0x40) : RGB(0x5A, 0x5A, 0x5A));
        FrameRect(dc, &b, be);
        DeleteObject(be);
        SetTextColor(dc, sel ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xB8, 0xB8, 0xB8));
        wchar_t ch[2] = { (wchar_t)(L'A' + i), 0 };
        DrawTextW(dc, ch, -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(dc, old);
}

static LRESULT CALLBACK EditorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }

    Plugin* p = (Plugin*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (p) paintEditor(h, dc, p);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!p) return 0;
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);

        // 复选框区域
        if (x >= 24 && x <= kWidth - 8 && y >= 342 && y <= 374) {
            p->param[P_CIRC_PAN] = (p->param[P_CIRC_PAN] >= 0.5f) ? 0.0f : 1.0f;
            notifyHost(p, P_CIRC_PAN);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }

        // 饱和模式 A/B/C/D 按钮
        if (y >= 366 && y <= 398) {
            for (int i = 0; i < 4; i++) {
                int bx = 70 + i * 40;
                if (x >= bx && x <= bx + 34) {
                    p->param[P_SATURATION] = (float)i;
                    notifyHost(p, P_SATURATION);
                    InvalidateRect(h, nullptr, FALSE);
                    return 0;
                }
            }
        }

        // 圆形命中测试
        for (int i = 0; i < NUM_PARAMS; i++) {
            const SliderDef& s = g_sliders[i];
            if (s.param == P_CIRC_PAN || s.param == P_SATURATION) continue;
            int dx = x - s.cx;
            int dy = y - s.cy;
            if (dx * dx + dy * dy <= s.r * s.r) {
                g_drag = i;
                g_dragY = y;
                g_dragT = (p->param[s.param] - s.lo) / (s.hi - s.lo);
                SetCapture(h);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (p && g_drag >= 0) {
            const SliderDef& s = g_sliders[g_drag];
            int y = (int)(short)HIWORD(lp);
            float t = g_dragT + (float)(g_dragY - y) / 160.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            p->param[s.param] = s.lo + t * (s.hi - s.lo);
            notifyHost(p, s.param);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag >= 0) { g_drag = -1; ReleaseCapture(); }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static bool ensureClass() {
    if (g_classReady) return true;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = EditorProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"NEKOWASHOCK_EDITOR";
    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    g_classReady = true;
    return true;
}

// 软削波（tanh 的 Padé 近似），用于饱和模式
static inline float softClip(float x) {
    if (x <= -3.0f) return -1.0f;
    if (x >=  3.0f) return  1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// ---------- 音频处理 ----------
static void doProcess(Plugin* p, float** in, float** out, VstInt32 n) {
    float targetL, targetR;
    p->updateGains(targetL, targetR);

    const float* iL = in[0];
    const float* iR = in[1];
    float* oL = out[0];
    float* oR = out[1];

    float sep = p->param[P_SEPARATION];
    if (sep < -1.0f) sep = -1.0f;
    if (sep >  1.0f) sep =  1.0f;

    float gl = p->curGainL;
    float gr = p->curGainR;
    const float k = 1.0f / 64.0f;

    int satMode = (int)(p->param[P_SATURATION] + 0.5f);
    if (satMode < 0) satMode = 0;
    if (satMode > 3) satMode = 3;
    // A=Default 直通，B=No saturation 直通，C/D 为两级饱和
    float drive = (satMode == 2) ? 1.5f : (satMode == 3 ? 3.0f : 1.0f);
    float norm  = 1.0f / softClip(drive);

    for (VstInt32 i = 0; i < n; i++) {
        gl += (targetL - gl) * k;
        gr += (targetR - gr) * k;

        float l = iL[i] * gl;
        float r = iR[i] * gr;

        if (sep != 0.0f) {
            float m = (l + r) * 0.5f;
            float s = (l - r) * 0.5f;
            // +1 = 加宽（Side 增益 2x），0 = 原样，-1 = 收成单声道
            s *= (1.0f + sep);
            l = m + s;
            r = m - s;
        }

        if (satMode >= 2) {
            l = softClip(l * drive) * norm;
            r = softClip(r * drive) * norm;
        }

        oL[i] = l;
        oR[i] = r;
    }

    p->curGainL = gl;
    p->curGainR = gr;
}

// ---------- 回调 ----------
static VstIntPtr vstDispatcher(AEffect* e, VstInt32 op, VstInt32 index,
                               VstIntPtr value, void* ptr, float opt) {
    Plugin* p = (Plugin*)e->object;
    switch (op) {
    case effOpen:  return 0;
    case effClose:
        if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
        delete p; e->object = nullptr; return 0;

    case effSetSampleRate:
        if (p) p->sampleRate = opt > 0 ? opt : 44100.0f;
        return 0;
    case effSetBlockSize:
        if (p) p->blockSize = (VstInt32)value;
        return 0;
    case effMainsChanged:
        if (p) {
            float l, r;
            p->updateGains(l, r);
            if (value) { p->curGainL = l; p->curGainR = r; }
        }
        return 0;

    case effEditGetRect:
        if (ptr) *(ERect**)ptr = &g_rect;
        return 1;

    case effEditOpen: {
        if (!ensureClass()) return 0;
        HWND parent = (HWND)ptr;
        g_effect = e;
        g_hwnd = CreateWindowExW(
            0, L"NEKOWASHOCK_EDITOR", L"NekoWaShock",
            WS_CHILD | WS_VISIBLE,
            0, 0, kWidth, kHeight,
            parent, nullptr, GetModuleHandleW(nullptr), p);
        return g_hwnd ? 1 : 0;
    }
    case effEditClose:
        if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
        return 0;
    case effEditIdle:
        return 0;

    case effGetParamName:
        if (index >= 0 && index < NUM_PARAMS) {
            strncpy((char*)ptr, g_sliders[index].name, 8);
            ((char*)ptr)[7] = 0;
        }
        return 0;
    case effGetParamLabel:
        strcpy((char*)ptr, "");
        return 0;
    case effGetParamDisplay:
        if (p && index >= 0 && index < NUM_PARAMS) {
            if (index == P_CIRC_PAN) {
                strcpy((char*)ptr, p->param[index] >= 0.5f ? "On" : "Off");
            } else if (index == P_SATURATION) {
                static const char* names[4] = { "Default", "No Sat", "Sat A", "Sat B" };
                int m = (int)(p->param[index] + 0.5f);
                if (m < 0) m = 0;
                if (m > 3) m = 3;
                strcpy((char*)ptr, names[m]);
            } else {
                snprintf((char*)ptr, 32, "%.2f", p->param[index]);
            }
        }
        return 0;

    case effGetEffectName:
        strncpy((char*)ptr, "NekoWaShock", 32);
        return 0;
    case effGetVendorString:
        strncpy((char*)ptr, "MCNeko-Forum", 32);
        return 0;
    case effGetProductString:
        strncpy((char*)ptr, "NekoWaShock", 32);
        return 0;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion:    return 2400;
    case effCanDo:
        if (ptr && strcmp((const char*)ptr, "receiveVstEvents") == 0) return 1;
        return 0;
    default:
        return 0;
    }
}

static void vstProcessReplacing(AEffect* e, float** in, float** out, VstInt32 n) {
    Plugin* p = (Plugin*)e->object;
    if (p) doProcess(p, in, out, n);
}

static void vstSetParameter(AEffect* e, VstInt32 index, float value) {
    Plugin* p = (Plugin*)e->object;
    if (p && index >= 0 && index < NUM_PARAMS) {
        p->param[index] = value;
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

static float vstGetParameter(AEffect* e, VstInt32 index) {
    Plugin* p = (Plugin*)e->object;
    if (p && index >= 0 && index < NUM_PARAMS) return p->param[index];
    return 0.0f;
}

static AEffect* createEffect() {
    AEffect* e = new AEffect();
    memset(e, 0, sizeof(AEffect));
    e->magic = kEffectMagic;
    e->dispatcher = vstDispatcher;
    e->process = nullptr;
    e->setParameter = vstSetParameter;
    e->getParameter = vstGetParameter;
    e->numPrograms = 1;
    e->numParams = NUM_PARAMS;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsHasEditor;
    e->uniqueID = 'Neko';
    e->version = 1000;
    e->processReplacing = vstProcessReplacing;
    e->object = new Plugin();
    g_effect = e;
    return e;
}

VST_EXPORT AEffect* VSTPluginMain(void* hostCallback) {
    g_host = (HostCallbackFn)hostCallback;
    return createEffect();
}
