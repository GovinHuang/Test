/*
 * winmain.c - G 代码路径查看器 Win32 GUI 入口
 * 功能:
 *   - File/Open 载入 .nc / .gcode / .tap 文件
 *   - 鼠标滚轮缩放, 中键拖拽平移, F 自动 Fit
 *   - 窗口缩放重绘
 *   - 状态栏显示鼠标世界坐标与边界
 */

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcode.h"
#include "render.h"

/* ---------- 资源 ID ---------- */
#define IDM_FILE_OPEN      1001
#define IDM_FILE_EXIT      1002
#define IDM_VIEW_FIT       2001
#define IDM_VIEW_RESET     2002
#define IDM_VIEW_GRID      2003
#define IDM_VIEW_RAPID     2004
#define IDM_HELP_ABOUT     3001
#define IDC_STATUS         4001

/* ---------- 全局窗口状态 ---------- */
static GCodeContext *g_ctx = NULL;
static ViewParams    g_view;
static HWND          g_hwndMain = NULL;
static HWND          g_hwndStatus = NULL;
static char          g_title[256] = "GCode Viewer";
static int           g_dragging = 0;
static POINT         g_lastMouse;
static int           g_mouseX = 0, g_mouseY = 0;

/* 屏幕坐标 -> 世界坐标 (反变换) */
static void screen_to_world(const ViewParams *v, int win_w, int win_h,
                            int sx, int sy,
                            double *wx, double *wy) {
    *wx = (sx - v->offset_x) / v->scale;
    if (v->invert_y)
        *wy = -(sy - v->offset_y) / v->scale;
    else
        *wy =  (sy - v->offset_y) / v->scale;
}

/* ---------- 更新状态栏 ---------- */
static void update_status_bar(void) {
    if (!g_hwndStatus) return;
    double wx, wy;
    RECT rc;
    GetClientRect(g_hwndMain, &rc);
    screen_to_world(&g_view, rc.right, rc.bottom, g_mouseX, g_mouseY, &wx, &wy);

    char text[512];
    if (g_ctx && g_ctx->has_bounds) {
        sprintf(text,
            "X=%.3f  Y=%.3f  |  Bounds X:[%.3f, %.3f] Y:[%.3f, %.3f]  "
            "| Segments: %d",
            wx, wy,
            g_ctx->min_x, g_ctx->max_x, g_ctx->min_y, g_ctx->max_y,
            g_ctx->seg_count);
    } else {
        sprintf(text, "X=%.3f  Y=%.3f  |  未加载 G 代码文件", wx, wy);
    }
    SetWindowTextA(g_hwndStatus, text);
}

/* ---------- 创建状态栏 ---------- */
static HWND create_statusbar(HWND hwnd) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance   = GetModuleHandleA(NULL);
    wc.hCursor     = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "GCodeStatusBar";
    RegisterClassA(&wc);

    RECT rc; GetClientRect(hwnd, &rc);
    int h = 20;
    return CreateWindowA("GCodeStatusBar", "",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, rc.bottom - h, rc.right, h,
        hwnd, (HMENU)(LONG_PTR)IDC_STATUS,
        GetModuleHandleA(NULL), NULL);
}

/* ---------- 调整布局 ---------- */
static void relayout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    if (g_hwndStatus) {
        int h = 20;
        MoveWindow(g_hwndStatus, 0, rc.bottom - h, rc.right, h, TRUE);
    }
}

/* ---------- 重绘 ---------- */
static void invalidate(void) {
    if (g_hwndMain) InvalidateRect(g_hwndMain, NULL, TRUE);
}

/* ---------- 打开文件对话框 ---------- */
static void open_file_dialog(void) {
    OPENFILENAMEA ofn;
    char filename[MAX_PATH] = "";
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwndMain;
    ofn.lpstrFile = filename;
    ofn.nMaxFile  = MAX_PATH;
    ofn.lpstrFilter =
        "G-Code Files (*.nc;*.tap;*.gcode;*.cnc)\0*.nc;*.tap;*.gcode;*.cnc\0"
        "All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameA(&ofn)) return;

    /* 释放旧上下文 */
    if (g_ctx) { gcode_destroy(g_ctx); g_ctx = NULL; }
    g_ctx = gcode_create();
    if (!g_ctx) { MessageBoxA(g_hwndMain, "内存不足", "错误", MB_OK|MB_ICONERROR); return; }

    if (gcode_parse_file(g_ctx, filename) != 0) {
        MessageBoxA(g_hwndMain, "文件解析失败", "错误", MB_OK|MB_ICONERROR);
        gcode_destroy(g_ctx); g_ctx = NULL;
        return;
    }

    /* 更新标题 */
    const char *base = strrchr(filename, '\\');
    if (base) base++; else base = filename;
    sprintf(g_title, "GCode Viewer - %s", base);
    SetWindowTextA(g_hwndMain, g_title);

    /* Fit */
    RECT rc; GetClientRect(g_hwndMain, &rc);
    view_fit(&g_view, g_ctx, rc.right, rc.bottom - 20, 24);

    gcode_print_stats(g_ctx);
    invalidate();
    update_status_bar();
}

/* ---------- 菜单 ---------- */
static HMENU build_menu(void) {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuA(file, MF_STRING, IDM_FILE_OPEN, "打开 G 代码文件...\tCtrl+O");
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    AppendMenuA(file, MF_STRING, IDM_FILE_EXIT, "退出");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)file, "文件(&F)");

    HMENU view = CreatePopupMenu();
    AppendMenuA(view, MF_STRING, IDM_VIEW_FIT,    "自适应 Fit\tF");
    AppendMenuA(view, MF_STRING, IDM_VIEW_RESET,  "重置视图");
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);
    AppendMenuA(view, MF_CHECKED | MF_STRING, IDM_VIEW_GRID,  "显示网格");
    AppendMenuA(view, MF_CHECKED | MF_STRING, IDM_VIEW_RAPID, "显示快速移动");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)view, "视图(&V)");

    HMENU help = CreatePopupMenu();
    AppendMenuA(help, MF_STRING, IDM_HELP_ABOUT, "关于");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)help, "帮助(&H)");
    return menu;
}

static void update_menu_checks(HMENU menu) {
    if (!menu) return;
    HMENU view = GetSubMenu(menu, 1);
    if (!view) return;
    CheckMenuItem(view, IDM_VIEW_GRID,
        g_view.show_grid  ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(view, IDM_VIEW_RAPID,
        g_view.show_rapid ? MF_CHECKED : MF_UNCHECKED);
}

/* ---------- 消息处理 ---------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            SetMenu(hwnd, build_menu());
            g_hwndStatus = create_statusbar(hwnd);
            view_default(&g_view);
            return 0;
        }

        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDM_FILE_OPEN:
                    open_file_dialog();
                    break;
                case IDM_FILE_EXIT:
                    DestroyWindow(hwnd);
                    break;
                case IDM_VIEW_FIT: {
                    if (g_ctx && g_ctx->has_bounds) {
                        RECT rc; GetClientRect(hwnd, &rc);
                        view_fit(&g_view, g_ctx, rc.right, rc.bottom - 20, 24);
                        invalidate();
                    }
                    break;
                }
                case IDM_VIEW_RESET: {
                    view_default(&g_view);
                    if (g_ctx && g_ctx->has_bounds) {
                        RECT rc; GetClientRect(hwnd, &rc);
                        view_fit(&g_view, g_ctx, rc.right, rc.bottom - 20, 24);
                    }
                    invalidate();
                    break;
                }
                case IDM_VIEW_GRID:
                    g_view.show_grid = !g_view.show_grid;
                    update_menu_checks(GetMenu(hwnd));
                    invalidate();
                    break;
                case IDM_VIEW_RAPID:
                    g_view.show_rapid = !g_view.show_rapid;
                    update_menu_checks(GetMenu(hwnd));
                    invalidate();
                    break;
                case IDM_HELP_ABOUT:
                    MessageBoxA(hwnd,
                        "GCode Viewer v1.0\n"
                        "--------------------------------\n"
                        "C 语言 + Win32 GDI 实现\n\n"
                        "支持 G00/G01/G02/G03/G20/G21/G90/G91/G17/G28/G80-G89 等\n\n"
                        "操作:\n"
                        "  F   - 自适应 Fit\n"
                        "  滚轮 - 缩放\n"
                        "  右键拖拽 - 平移",
                        "关于", MB_OK | MB_ICONINFORMATION);
                    break;
            }
            return 0;
        }

        case WM_SIZE:
            relayout(hwnd);
            invalidate();
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rc; GetClientRect(hwnd, &rc);
            /* 扣除状态栏高度 */
            int w = rc.right, h = rc.bottom - 20;
            if (h < 20) h = rc.bottom;

            render_all(hdc, g_ctx, &g_view, w, h);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = (short)HIWORD(wp);
            double factor = (delta > 0) ? 1.15 : 1.0/1.15;

            /* 鼠标所在位置缩放 */
            POINT pt; pt.x = LOWORD(lp); pt.y = HIWORD(lp);
            ScreenToClient(hwnd, &pt);
            RECT rc; GetClientRect(hwnd, &rc);
            int w = rc.right, h = rc.bottom - 20;

            /* 求该点世界坐标 */
            double wx, wy;
            screen_to_world(&g_view, w, h, pt.x, pt.y, &wx, &wy);

            g_view.scale *= factor;
            /* 反算 offset, 让该点保持不动 */
            g_view.offset_x = pt.x - wx * g_view.scale;
            if (g_view.invert_y)
                g_view.offset_y = pt.y + wy * g_view.scale;
            else
                g_view.offset_y = pt.y - wy * g_view.scale;

            invalidate();
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            /* 中键/右键拖拽平移 */
            if (msg == WM_MBUTTONDOWN || (msg == WM_LBUTTONDOWN &&
                (GetKeyState(VK_CONTROL) & 0x8000))) {
                g_dragging = 1;
                GetCursorPos(&g_lastMouse);
                SetCapture(hwnd);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            g_mouseX = LOWORD(lp);
            g_mouseY = HIWORD(lp);
            if (g_dragging) {
                POINT cur; GetCursorPos(&cur);
                int dx = cur.x - g_lastMouse.x;
                int dy = cur.y - g_lastMouse.y;
                g_view.offset_x += dx;
                g_view.offset_y += dy;
                g_lastMouse = cur;
                invalidate();
            }
            update_status_bar();
            return 0;
        }

        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
            if (g_dragging) {
                g_dragging = 0;
                ReleaseCapture();
            }
            return 0;

        case WM_KEYDOWN: {
            if (wp == 'F') {
                SendMessageA(hwnd, WM_COMMAND, IDM_VIEW_FIT, 0);
            } else if (wp == '+' || wp == '=') {
                g_view.scale *= 1.2;
                invalidate();
            } else if (wp == '-') {
                g_view.scale /= 1.2;
                invalidate();
            }
            return 0;
        }

        case WM_DESTROY:
            if (g_ctx) { gcode_destroy(g_ctx); g_ctx = NULL; }
            PostQuitMessage(0);
            return 0;

        case WM_ACCELERATORS:
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---------- WinMain ---------- */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nShowCmd) {
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc    = WndProc;
    wc.hInstance      = hInstance;
    wc.hCursor        = LoadCursorA(NULL, IDC_CROSS);
    wc.hbrBackground  = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName  = "GCodeViewerClass";
    wc.hIcon          = LoadIconA(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);

    g_hwndMain = CreateWindowA(
        "GCodeViewerClass",
        g_title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 720,
        NULL, NULL, hInstance, NULL);

    ShowWindow(g_hwndMain, nShowCmd);
    UpdateWindow(g_hwndMain);
    update_menu_checks(GetMenu(g_hwndMain));
    update_status_bar();

    /* 如果命令行带了文件名, 自动打开 */
    if (lpCmdLine && lpCmdLine[0]) {
        g_ctx = gcode_create();
        if (g_ctx && gcode_parse_file(g_ctx, lpCmdLine) == 0) {
            RECT rc; GetClientRect(g_hwndMain, &rc);
            view_fit(&g_view, g_ctx, rc.right, rc.bottom - 20, 24);
            SetWindowTextA(g_hwndMain, lpCmdLine);
            gcode_print_stats(g_ctx);
            invalidate();
            update_status_bar();
        } else {
            if (g_ctx) { gcode_destroy(g_ctx); g_ctx = NULL; }
        }
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
