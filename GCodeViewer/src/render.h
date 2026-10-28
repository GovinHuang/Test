#ifndef RENDER_H
#define RENDER_H

#include <windows.h>
#include "gcode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 视图参数 */
typedef struct {
    double scale;       /* 世界到屏幕的比例 (像素/单位) */
    double offset_x;    /* 屏幕上的中心偏移 (像素) */
    double offset_y;
    int    invert_y;    /* CNC 坐标 Y 向上, 屏幕 Y 向下; 默认倒置 */
    double arc_tol;     /* 圆弧离散的弦高误差 (世界单位), 默认 0.05 */
    int    show_rapid;  /* 是否显示 G00 空移 */
    int    show_grid;   /* 是否显示网格 */
    double grid_step;   /* 网格步长 (世界单位) */
} ViewParams;

/* 初始化默认视图 */
void view_default(ViewParams *v);

/* 根据 bounds 自动 fit 到窗口 */
void view_fit(ViewParams *v, const GCodeContext *ctx,
              int win_w, int win_h, int margin);

/* 世界坐标 -> 屏幕坐标 */
void world_to_screen(const ViewParams *v, int win_w, int win_h,
                     double wx, double wy,
                     int *sx, int *sy);

/* 在给定 HDC 上绘制全部路径 */
void render_all(HDC hdc, const GCodeContext *ctx,
                const ViewParams *vp, int win_w, int win_h);

/* 绘制网格 */
void render_grid(HDC hdc, const GCodeContext *ctx,
                 const ViewParams *vp, int win_w, int win_h);

/* 绘制单段 */
void render_segment(HDC hdc, const Segment *seg,
                    const ViewParams *vp, int win_w, int win_h,
                    double tol);

#ifdef __cplusplus
}
#endif

#endif /* RENDER_H */
