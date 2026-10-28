#ifndef GCODE_H
#define GCODE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 数据类型 ---------- */

/* 坐标点 */
typedef struct {
    double x;
    double y;
    double z;
} Point;

/* 线段类型 */
typedef enum {
    SEG_LINE,        /* 直线 */
    SEG_ARC_CW,      /* 顺时针圆弧 */
    SEG_ARC_CCW,     /* 逆时针圆弧 */
    SEG_G00,         /* 快速移动 (空移) */
    SEG_Z_RAPID,     /* Z轴快速移动 */
    SEG_DRILL        /* 钻孔循环 (简化) */
} SegmentType;

/* 路径段 */
typedef struct {
    SegmentType type;
    Point start;      /* 起点 */
    Point end;        /* 终点 */
    /* 圆弧参数 */
    double i, j;      /* 圆心相对于起点的偏移 (G02/G03) */
    double r;         /* 半径 (可选) */
    /* 绘图辅助 */
    double depth_start; /* 起始Z (Z_RAPID/DRILL) */
    double depth_end;   /* 结束Z */
    int is_cut;       /* 是否切削段 */
} Segment;

/* 解析上下文 */
typedef struct {
    Point current;          /* 当前位置 */
    int abs_mode;           /* G90=绝对, G91=增量 */
    int units_mm;           /* G21=mm, G20=inch */
    int plane;              /* G17=XY, G18=XZ, G19=YZ */
    int tool_on;            /* 切削开启 (主轴/激光) */
    double feed;
    /* 输出 */
    Segment *segs;
    int seg_count;
    int seg_capacity;
    /* 边界 */
    double min_x, max_x;
    double min_y, max_y;
    double min_z, max_z;
    int has_bounds;
} GCodeContext;

/* ---------- API ---------- */

GCodeContext* gcode_create(void);
void          gcode_destroy(GCodeContext *ctx);

/* 从文件解析 G 代码, 成功返回 0 */
int           gcode_parse_file(GCodeContext *ctx, const char *path);

/* 从字符串解析一行 */
int           gcode_parse_line(GCodeContext *ctx, const char *line);

/* 打印解析统计 */
void          gcode_print_stats(const GCodeContext *ctx);

/* 访问器 */
int           gcode_segment_count(const GCodeContext *ctx);
const Segment* gcode_get_segment(const GCodeContext *ctx, int idx);

/* 计算圆弧真实圆心 (返回0失败) */
int           gcode_arc_center(const Segment *seg, double *cx, double *cy);

/* 将圆弧离散化为折线 (返回折线点数) */
int           gcode_arc_sample(const Segment *seg, double tol,
                               Point *pts, int max_pts);

#ifdef __cplusplus
}
#endif

#endif /* GCODE_H */
