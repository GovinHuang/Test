/*
 * render.c - G 代码路径 GDI 绘制
 */

#include "render.h"
#include <math.h>

void view_default(ViewParams *v) {
    v->scale = 10.0;
    v->offset_x = 0;
    v->offset_y = 0;
    v->invert_y = 1;
    v->arc_tol = 0.05;
    v->show_rapid = 1;
    v->show_grid = 1;
    v->grid_step = 10.0;
}

void view_fit(ViewParams *v, const GCodeContext *ctx,
              int win_w, int win_h, int margin) {
    if (!ctx || !ctx->has_bounds || win_w <= 0 || win_h <= 0) return;
    double w = ctx->max_x - ctx->min_x;
    double h = ctx->max_y - ctx->min_y;
    if (w < 1e-6) w = 1.0;
    if (h < 1e-6) h = 1.0;
    double sx = (win_w - 2*margin) / w;
    double sy = (win_h - 2*margin) / h;
    v->scale = (sx < sy) ? sx : sy;
    /* 将世界中心映射到屏幕中心 */
    double cx_w = (ctx->min_x + ctx->max_x) / 2.0;
    double cy_w = (ctx->min_y + ctx->max_y) / 2.0;
    double cx_s = win_w / 2.0;
    double cy_s = win_h / 2.0;
    v->offset_x = cx_s - cx_w * v->scale;
    v->offset_y = cy_s + cy_w * v->scale; /* 考虑 invert_y */
}

void world_to_screen(const ViewParams *v, int win_w, int win_h,
                     double wx, double wy,
                     int *sx, int *sy) {
    *sx = (int)(wx * v->scale + v->offset_x);
    /* invert_y: 世界 Y 向上 -> 屏幕 Y 向下 */
    *sy = v->invert_y
        ? (int)(-wy * v->scale + v->offset_y)
        : (int)( wy * v->scale + v->offset_y);
}

static HPEN create_pen(int color, int width, int dash_style) {
    /* width 用 1 即可, GDI 会根据 DPI 缩放 */
    return CreatePen(dash_style, 1, color);
}

void render_grid(HDC hdc, const GCodeContext *ctx,
                 const ViewParams *vp, int win_w, int win_h) {
    if (!vp->show_grid || !ctx->has_bounds) return;

    /* 选择灰色细线 */
    HPEN grid_pen = CreatePen(PS_DOT, 1, RGB(192,192,192));
    HPEN old_pen = (HPEN)SelectObject(hdc, grid_pen);

    double step = vp->grid_step;
    double x0 = floor(ctx->min_x / step) * step;
    double x1 = ceil(ctx->max_x / step) * step;
    double y0 = floor(ctx->min_y / step) * step;
    double y1 = ceil(ctx->max_y / step) * step;

    /* 先画主网格 */
    for (double x = x0; x <= x1 + step/2; x += step) {
        int sx1, sy1, sx2, sy2;
        world_to_screen(vp, win_w, win_h, x, y0, &sx1, &sy1);
        world_to_screen(vp, win_w, win_h, x, y1, &sx2, &sy2);
        MoveToEx(hdc, sx1, sy1, NULL);
        LineTo(hdc, sx2, sy2);
    }
    for (double y = y0; y <= y1 + step/2; y += step) {
        int sx1, sy1, sx2, sy2;
        world_to_screen(vp, win_w, win_h, x0, y, &sx1, &sy1);
        world_to_screen(vp, win_w, win_h, x1, y, &sx2, &sy2);
        MoveToEx(hdc, sx1, sy1, NULL);
        LineTo(hdc, sx2, sy2);
    }

    /* 画粗一点的坐标轴 */
    HPEN axis_pen = CreatePen(PS_SOLID, 1, RGB(96,96,96));
    SelectObject(hdc, axis_pen);
    {
        int sx1, sy1, sx2, sy2;
        world_to_screen(vp, win_w, win_h, x0, 0, &sx1, &sy1);
        world_to_screen(vp, win_w, win_h, x1, 0, &sx2, &sy2);
        MoveToEx(hdc, sx1, sy1, NULL);
        LineTo(hdc, sx2, sy2);
        world_to_screen(vp, win_w, win_h, 0, y0, &sx1, &sy1);
        world_to_screen(vp, win_w, win_h, 0, y1, &sx2, &sy2);
        MoveToEx(hdc, sx1, sy1, NULL);
        LineTo(hdc, sx2, sy2);
    }

    SelectObject(hdc, old_pen);
    DeleteObject(grid_pen);
    DeleteObject(axis_pen);
}

void render_segment(HDC hdc, const Segment *seg,
                    const ViewParams *vp, int win_w, int win_h,
                    double tol) {
    if (!seg) return;

    int color;
    int pen_style = PS_SOLID;
    int width = seg->is_cut ? 2 : 1;

    switch (seg->type) {
        case SEG_G00:
            if (!vp->show_rapid) return;
            color = RGB(192, 192, 192);
            pen_style = PS_DASH;
            break;
        case SEG_Z_RAPID:
            color = RGB(255, 165, 0);  /* 橙色 Z 空移 */
            pen_style = PS_DASH;
            break;
        case SEG_LINE:
            color = seg->is_cut ? RGB(255, 0, 0) : RGB(0, 128, 0);
            break;
        case SEG_ARC_CW:
        case SEG_ARC_CCW:
            color = RGB(0, 0, 255);
            break;
        case SEG_DRILL:
            color = RGB(128, 0, 128);
            break;
        default:
            color = RGB(100, 100, 100);
            break;
    }

    /* 对于圆弧/钻孔折线: 采样后画 */
    if (seg->type == SEG_ARC_CW || seg->type == SEG_ARC_CCW) {
        Point pts[4096];
        int n = gcode_arc_sample(seg, tol, pts, 4096);
        if (n < 2) return;
        HPEN pen = CreatePen(pen_style, width, color);
        HPEN old = (HPEN)SelectObject(hdc, pen);
        for (int i = 0; i < n; i++) {
            int sx, sy;
            world_to_screen(vp, win_w, win_h, pts[i].x, pts[i].y, &sx, &sy);
            if (i == 0) MoveToEx(hdc, sx, sy, NULL);
            else LineTo(hdc, sx, sy);
        }
        SelectObject(hdc, old);
        DeleteObject(pen);
        return;
    }

    /* 直线 */
    HPEN pen = CreatePen(pen_style, width, color);
    HPEN old = (HPEN)SelectObject(hdc, pen);
    {
        int sx1, sy1, sx2, sy2;
        world_to_screen(vp, win_w, win_h, seg->start.x, seg->start.y, &sx1, &sy1);
        world_to_screen(vp, win_w, win_h, seg->end.x,   seg->end.y,   &sx2, &sy2);
        MoveToEx(hdc, sx1, sy1, NULL);
        LineTo(hdc, sx2, sy2);
    }
    SelectObject(hdc, old);
    DeleteObject(pen);
}

void render_all(HDC hdc, const GCodeContext *ctx,
                const ViewParams *vp, int win_w, int win_h) {
    /* 背景白色 */
    RECT r; r.left=0; r.top=0; r.right=win_w; r.bottom=win_h;
    FillRect(hdc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));

    render_grid(hdc, ctx, vp, win_w, win_h);

    for (int i = 0; i < ctx->seg_count; i++) {
        render_segment(hdc, &ctx->segs[i], vp, win_w, win_h, vp->arc_tol);
    }

    /* 在结束点画一个红圈标识 */
    if (ctx->seg_count > 0) {
        int sx, sy;
        const Segment *last = &ctx->segs[ctx->seg_count-1];
        world_to_screen(vp, win_w, win_h, last->end.x, last->end.y, &sx, &sy);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255,0,0));
        HPEN old = (HPEN)SelectObject(hdc, pen);
        SelectObject(hdc, (HBRUSH)GetStockObject(HOLLOW_BRUSH));
        Ellipse(hdc, sx-6, sy-6, sx+6, sy+6);
        SelectObject(hdc, old);
        DeleteObject(pen);
    }

    /* 起点 */
    if (ctx->seg_count > 0) {
        int sx, sy;
        const Segment *first = &ctx->segs[0];
        world_to_screen(vp, win_w, win_h, first->start.x, first->start.y, &sx, &sy);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0,160,0));
        HPEN old = (HPEN)SelectObject(hdc, pen);
        SelectObject(hdc, (HBRUSH)GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, sx-5, sy-5, sx+5, sy+5);
        SelectObject(hdc, old);
        DeleteObject(pen);
    }
}
