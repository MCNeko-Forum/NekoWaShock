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
    P_DRIVE,
    NUM_PARAMS
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
        param[P_DRIVE]        = 0.0f;  // 0 = 直通（与原版默认一致）
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
static ERect g_rect = { 0, 0, 320, 360 };

static const int kWidth = 360;
static const int kHeight = 320;

// 原版布局：左侧一个大旋钮，右侧 A/B/C/D 四档
static const int kKnobCx = 138;
static const int kKnobCy = 200;
static const int kKnobR  = 80;
static const int kAbcdX  = 226;
static const int kAbcdY  = 180;
static const int kAbcdStep = 28;

// 宿主显示的参数名（VST2 限 8 字符）
static const char* kParamNames[NUM_PARAMS] = {
    "Volume", "Pan", "Separation", "RightVol", "LeftVol", "CircPan", "SatMode", "Drive"
};

static void notifyHost(Plugin* p, int idx) {
    if (g_host && g_effect) {
        // opcode 1 = audioMasterAutomate
        float v = (idx == P_SATURATION) ? p->param[idx] / 3.0f : p->param[idx];
        g_host(g_effect, 1, idx, 0, nullptr, v);
    }
}

static void drawArc(HDC dc, int cx, int cy, int r, float a0, float a1, COLORREF col, int w) {
    const int steps = 96;
    POINT pts[97];
    for (int i = 0; i <= steps; i++) {
        float deg = a0 + (a1 - a0) * (float)i / steps;
        float a = deg * 3.14159265f / 180.0f;
        pts[i].x = cx + (int)(sinf(a) * r);
        pts[i].y = cy - (int)(cosf(a) * r);
    }
    HPEN pen = CreatePen(PS_SOLID, w, col);
    HPEN op = (HPEN)SelectObject(dc, pen);
    Polyline(dc, pts, steps + 1);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static void drawKnob(HDC dc, int cx, int cy, int r, float t, bool hot) {
    // 底座
    HBRUSH outer = CreateSolidBrush(RGB(0x2A, 0x2A, 0x2A));
    HBRUSH ob = (HBRUSH)SelectObject(dc, outer);
    HPEN op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(outer);

    // 红色环形指示（原版的 LED 圈）：全周暗红 + 当前值亮红
    drawArc(dc, cx, cy, r - 4, -135.0f, 135.0f, RGB(0x3C, 0x12, 0x12), 6);
    if (t > 0.005f) {
        drawArc(dc, cx, cy, r - 4, -135.0f, -135.0f + t * 270.0f, RGB(0xD8, 0x24, 0x24), 6);
    }

    // 内盘
    int ir = r - 10;
    HBRUSH inner = CreateSolidBrush(hot ? RGB(0x3A, 0x28, 0x28) : RGB(0x1C, 0x1C, 0x1C));
    ob = (HBRUSH)SelectObject(dc, inner);
    HPEN ip = CreatePen(PS_SOLID, 1, RGB(0x44, 0x44, 0x44));
    op = (HPEN)SelectObject(dc, ip);
    Ellipse(dc, cx - ir, cy - ir, cx + ir, cy + ir);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(ip);
    DeleteObject(inner);

    // 刻度：-135° .. +135°
    for (int i = 0; i <= 10; i++) {
        float a = (-135.0f + i * 27.0f) * 3.14159265f / 180.0f;
        int x0 = cx + (int)(sinf(a) * (ir - 4));
        int y0 = cy - (int)(cosf(a) * (ir - 4));
        int x1 = cx + (int)(sinf(a) * (ir - 12));
        int y1 = cy - (int)(cosf(a) * (ir - 12));
        HPEN tp = CreatePen(PS_SOLID, 1, RGB(0x4E, 0x4E, 0x4E));
        HPEN tpOld = (HPEN)SelectObject(dc, tp);
        MoveToEx(dc, x0, y0, nullptr);
        LineTo(dc, x1, y1);
        SelectObject(dc, tpOld);
        DeleteObject(tp);
    }

    // 指针
    float ang = (-135.0f + t * 270.0f) * 3.14159265f / 180.0f;
    int px = cx + (int)(sinf(ang) * (ir - 14));
    int py = cy - (int)(cosf(ang) * (ir - 14));
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(0xE8, 0x38, 0x38));
    op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, cx, cy, nullptr);
    LineTo(dc, px, py);
    SelectObject(dc, op);
    DeleteObject(pen);

    // 中心轴
    HBRUSH hub = CreateSolidBrush(RGB(0xE8, 0x38, 0x38));
    ob = (HBRUSH)SelectObject(dc, hub);
    HPEN hp = CreatePen(PS_SOLID, 1, RGB(0xE8, 0x38, 0x38));
    op = (HPEN)SelectObject(dc, hp);
    Ellipse(dc, cx - 3, cy - 3, cx + 4, cy + 4);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(hp);
    DeleteObject(hub);
}

static void paintEditor(HWND h, HDC dc, Plugin* p) {
    RECT full = { 0, 0, kWidth, kHeight };
    HBRUSH bg = CreateSolidBrush(RGB(0x12, 0x12, 0x12));
    FillRect(dc, &full, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT old = (HFONT)SelectObject(dc, font);

    // 标题
    SetTextColor(dc, RGB(0xE8, 0x44, 0x44));
    RECT tr = { 16, 10, kWidth - 16, 34 };
    DrawTextW(dc, L"NekoWaShock", -1, &tr, DT_LEFT | DT_SINGLELINE);

    SetTextColor(dc, RGB(0x6E, 0x6E, 0x6E));
    RECT sr = { 16, 33, kWidth - 16, 51 };
    DrawTextW(dc, L"YouWaShock 64-bit", -1, &sr, DT_LEFT | DT_SINGLELINE);

    HPEN sepLine = CreatePen(PS_SOLID, 1, RGB(0x30, 0x30, 0x30));
    HPEN oldPen = (HPEN)SelectObject(dc, sepLine);
    MoveToEx(dc, 16, 56, nullptr);
    LineTo(dc, kWidth - 16, 56);
    SelectObject(dc, oldPen);
    DeleteObject(sepLine);

    // 中央大旋钮：驱动量 0..1
    float drive = p->param[P_DRIVE];
    if (drive < 0.0f) drive = 0.0f;
    if (drive > 1.0f) drive = 1.0f;
    drawKnob(dc, kKnobCx, kKnobCy, kKnobR, drive, g_drag >= 0);

    wchar_t val[64];
    swprintf(val, 64, L"DRIVE  %d%%", (int)(drive * 100.0f + 0.5f));
    SetTextColor(dc, RGB(0xD8, 0xD8, 0xD8));
    RECT vr = { kKnobCx - 100, kKnobCy + kKnobR + 6, kKnobCx + 100, kKnobCy + kKnobR + 28 };
    DrawTextW(dc, val, -1, &vr, DT_CENTER | DT_SINGLELINE);

    // 右侧 A B C D 四档
    int satMode = (int)(p->param[P_SATURATION] + 0.5f);
    if (satMode < 0) satMode = 0;
    if (satMode > 3) satMode = 3;

    HFONT abcdFont = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT prevFont = (HFONT)SelectObject(dc, abcdFont);

    for (int i = 0; i < 4; i++) {
        int bx = kAbcdX + i * kAbcdStep;
        RECT b = { bx, kAbcdY, bx + 24, kAbcdY + 28 };
        bool sel = (satMode == i);
        if (sel) {
            HBRUSH bb = CreateSolidBrush(RGB(0x38, 0x14, 0x14));
            FillRect(dc, &b, bb);
            DeleteObject(bb);
        }
        SetTextColor(dc, sel ? RGB(0xFF, 0x7A, 0x2A) : RGB(0x8A, 0x8A, 0x8A));
        wchar_t ch[2] = { (wchar_t)(L'A' + i), 0 };
        DrawTextW(dc, ch, -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(dc, prevFont);
    DeleteObject(abcdFont);

    // 左下帮助标记
    SetTextColor(dc, RGB(0x5A, 0x5A, 0x5A));
    RECT hb = { 16, kHeight - 30, 40, kHeight - 10 };
    DrawTextW(dc, L"[?]", -1, &hb, DT_LEFT | DT_SINGLELINE);

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
    case WM_PRINTCLIENT: {
        // 供截图/打印：把界面画到外部传入的 DC
        if (p) paintEditor(h, (HDC)wp, p);
        return 0;
    }
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

        // 右侧 A/B/C/D 四档（原版：点击字母切换饱和档位）
        for (int i = 0; i < 4; i++) {
            int bx = kAbcdX + i * kAbcdStep;
            if (x >= bx - 4 && x <= bx + 26 && y >= kAbcdY - 4 && y <= kAbcdY + 28) {
                p->param[P_SATURATION] = (float)i;
                notifyHost(p, P_SATURATION);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
        }

        // 中央大旋钮：驱动量
        int dx = x - kKnobCx;
        int dy = y - kKnobCy;
        if (dx * dx + dy * dy <= kKnobR * kKnobR) {
            g_drag = 0;
            g_dragY = y;
            g_dragT = p->param[P_DRIVE];
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (p && g_drag == 0) {
            int y = (int)(short)HIWORD(lp);
            float t = g_dragT + (float)(g_dragY - y) / 200.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            p->param[P_DRIVE] = t;
            notifyHost(p, P_DRIVE);
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

    // 驱动量来自界面上的大旋钮（0..1）。
    // 各档位决定最大过载倍数 kmax；trim = 1/k 保证小信号增益恒为 1，
    // 只有大信号才被软削波压缩并产生谐波，避免「只是变响」。
    float drive = p->param[P_DRIVE];
    if (drive < 0.0f) drive = 0.0f;
    if (drive > 1.0f) drive = 1.0f;

    float kmax = 1.0f;
    switch (satMode) {
        case 0:  kmax = 3.0f;  break;  // A 默认
        case 1:  kmax = 1.0f;  break;  // B 不饱和
        case 2:  kmax = 6.0f;  break;  // C 饱和 A
        default: kmax = 12.0f; break;  // D 饱和 B
    }

    float satK = 1.0f + drive * (kmax - 1.0f);
    float satTrim = 1.0f / satK;
    bool doSat = (satMode != 1) && (drive > 0.0001f);

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

        if (doSat) {
            l = softClip(l * satK) * satTrim;
            r = softClip(r * satK) * satTrim;
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
            strncpy((char*)ptr, kParamNames[index], 8);
            ((char*)ptr)[8] = 0;
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
            } else if (index == P_DRIVE) {
                snprintf((char*)ptr, 32, "%d%%", (int)(p->param[index] * 100.0f + 0.5f));
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
        // 饱和模式内部为 0..3，宿主侧为 0..1
        p->param[index] = (index == P_SATURATION) ? value * 3.0f : value;
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

static float vstGetParameter(AEffect* e, VstInt32 index) {
    Plugin* p = (Plugin*)e->object;
    if (p && index >= 0 && index < NUM_PARAMS) {
        return (index == P_SATURATION) ? p->param[index] / 3.0f : p->param[index];
    }
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
