/*
 * gcode.c - 轻量 G 代码解析器
 * 支持: G00/G01/G02/G03/G90/G91/G17/G20/G21/G28/G80-G89/M02/M03/M05 等常见指令
 * 不做真正的机床控制, 只提取可绘制的路径段.
 */

#define _USE_MATH_DEFINES
#include "gcode.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------- 小工具 ---------- */

static char* trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)*(e-1))) *--e = '\0';
    return s;
}

/* 跳过注释: () 内或 ; 直到行尾 */
static void strip_comments(char *s) {
    char *src = s, *dst = s;
    int in_paren = 0;
    while (*src) {
        if (*src == '(') { in_paren++; src++; continue; }
        if (*src == ')') { if (in_paren > 0) in_paren--; src++; continue; }
        if (*src == ';') { *dst = '\0'; return; }
        if (in_paren) { src++; continue; }
        *dst++ = *src++;
    }
    *dst = '\0';
}

/* 解析形如 G01 X10.5 Y-2.0 F100 的一行到 tokens 数组, 每 token 为 'A' + value */
typedef struct { char letter; double value; int has_value; } Token;

static int tokenize(const char *line, Token *tokens, int max_tokens) {
    int n = 0;
    const char *p = line;
    while (*p && n < max_tokens) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char letter = (char)toupper((unsigned char)*p);
        p++;
        /* 可选 '=' */
        if (*p == '=') p++;
        /* 读取数字, 允许正负和小数点 */
        char *end;
        double val = strtod(p, &end);
        int has = (end != p);
        if (has) p = end;
        tokens[n].letter = letter;
        tokens[n].value = val;
        tokens[n].has_value = has;
        n++;
    }
    return n;
}

/* ---------- 上下文 ---------- */

GCodeContext* gcode_create(void) {
    GCodeContext *ctx = (GCodeContext*)calloc(1, sizeof(GCodeContext));
    if (!ctx) return NULL;
    ctx->abs_mode = 1;        /* 默认 G90 */
    ctx->units_mm = 1;        /* 默认 G21 */
    ctx->plane = 17;          /* 默认 G17 XY */
    ctx->current.x = 0;
    ctx->current.y = 0;
    ctx->current.z = 0;
    ctx->tool_on = 0;
    ctx->seg_capacity = 256;
    ctx->segs = (Segment*)calloc(ctx->seg_capacity, sizeof(Segment));
    return ctx;
}

void gcode_destroy(GCodeContext *ctx) {
    if (!ctx) return;
    free(ctx->segs);
    free(ctx);
}

static void ensure_capacity(GCodeContext *ctx) {
    if (ctx->seg_count >= ctx->seg_capacity) {
        ctx->seg_capacity *= 2;
        ctx->segs = (Segment*)realloc(ctx->segs, ctx->seg_capacity * sizeof(Segment));
    }
}

static void update_bounds(GCodeContext *ctx, const Point *p) {
    if (!ctx->has_bounds) {
        ctx->min_x = ctx->max_x = p->x;
        ctx->min_y = ctx->max_y = p->y;
        ctx->min_z = ctx->max_z = p->z;
        ctx->has_bounds = 1;
    } else {
        if (p->x < ctx->min_x) ctx->min_x = p->x;
        if (p->x > ctx->max_x) ctx->max_x = p->x;
        if (p->y < ctx->min_y) ctx->min_y = p->y;
        if (p->y > ctx->max_y) ctx->max_y = p->y;
        if (p->z < ctx->min_z) ctx->min_z = p->z;
        if (p->z > ctx->max_z) ctx->max_z = p->z;
    }
}

static void push_segment(GCodeContext *ctx, const Segment *seg) {
    ensure_capacity(ctx);
    ctx->segs[ctx->seg_count++] = *seg;
}

/* ---------- 逐行解析 ---------- */

int gcode_parse_line(GCodeContext *ctx, const char *line_in) {
    if (!ctx || !line_in) return -1;
    char buf[1024];
    strncpy(buf, line_in, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    strip_comments(buf);
    char *s = trim(buf);
    if (*s == '\0') return 0;

    Token tokens[32];
    int ntok = tokenize(s, tokens, 32);
    if (ntok == 0) return 0;

    /* 先提取出一行里所有 Gxx, Mxx, 以及坐标字 */
    int g_motion = -1, g_plane = -1, g_mode = -1, g_units = -1, g_cycle = -1;
    int m_code = -1;
    Point target = ctx->current;
    int has_x=0, has_y=0, has_z=0, has_a=0, has_b=0, has_c=0;
    int has_i=0, has_j=0, has_k=0, has_r=0, has_f=0;
    double v_x=0,v_y=0,v_z=0,v_a=0,v_b=0,v_c=0;
    double v_i=0,v_j=0,v_k=0,v_r=0,v_f=0;

    for (int i = 0; i < ntok; i++) {
        char c = tokens[i].letter;
        double v = tokens[i].has_value ? tokens[i].value : 0;
        switch (c) {
            case 'G':
                if      (v >= 0 && v <= 1)   { g_motion = (int)v; g_cycle = -1; }
                else if (v >= 2 && v <= 3)   { g_motion = (int)v; g_cycle = -1; }
                else if (v == 4)             { g_motion = 4; } /* G04 Dwell */
                else if (v >= 17 && v <= 19) g_plane  = (int)v;
                else if (v == 20)            g_units  = 20;
                else if (v == 21)            g_units  = 21;
                else if (v == 28 || v == 30) { /* 快速回零 */
                    if (ctx->abs_mode) {
                        Segment seg; memset(&seg, 0, sizeof(seg));
                        seg.type = SEG_G00;
                        seg.start = ctx->current;
                        seg.end = target;
                        seg.is_cut = 0;
                        push_segment(ctx, &seg);
                    }
                    ctx->current.x = 0; ctx->current.y = 0; ctx->current.z = 0;
                    update_bounds(ctx, &ctx->current);
                    g_motion = -1;
                }
                else if (v == 90)            g_mode   = 90;
                else if (v == 91)            g_mode   = 91;
                else if (v == 92) { /* 设置当前位置坐标系 (不画) */
                    if (has_x) ctx->current.x = v_x;
                    if (has_y) ctx->current.y = v_y;
                    if (has_z) ctx->current.z = v_z;
                    return 0;
                }
                else if (v >= 80 && v <= 89) { g_cycle = (int)v; g_motion = -1; }
                break;
            case 'M':
                m_code = (int)v;
                break;
            case 'X': has_x = 1; v_x = v; break;
            case 'Y': has_y = 1; v_y = v; break;
            case 'Z': has_z = 1; v_z = v; break;
            case 'A': has_a = 1; v_a = v; break;
            case 'B': has_b = 1; v_b = v; break;
            case 'C': has_c = 1; v_c = v; break;
            case 'I': has_i = 1; v_i = v; break;
            case 'J': has_j = 1; v_j = v; break;
            case 'K': has_k = 1; v_k = v; break;
            case 'R': has_r = 1; v_r = v; break;
            case 'F': has_f = 1; v_f = v; break;
            default: /* 忽略 H, D, T, S, P, Q 等 */ break;
        }
    }

    /* 应用模态更新 */
    if (g_plane > 0) ctx->plane = g_plane;
    if (g_units > 0) ctx->units_mm = (g_units == 21);
    if (g_mode  > 0) ctx->abs_mode = (g_mode  == 90);
    if (has_f) ctx->feed = v_f;

    /* M 码: 主轴开关 */
    if (m_code >= 0) {
        if (m_code == 3 || m_code == 4 || m_code == 106) ctx->tool_on = 1;
        if (m_code == 5 || m_code == 9  || m_code == 107) ctx->tool_on = 0;
        if (m_code == 2 || m_code == 30) {
            /* 停止; 忽略后续 */
        }
    }

    /* 计算目标位置 */
    if (ctx->abs_mode) {
        if (has_x) target.x = v_x;
        if (has_y) target.y = v_y;
        if (has_z) target.z = v_z;
    } else {
        if (has_x) target.x += v_x;
        if (has_y) target.y += v_y;
        if (has_z) target.z += v_z;
    }

    /* 不运动则忽略 */
    int moved = has_x || has_y || has_z;

    /* 处理运动 */
    if (g_motion == 0) {
        /* G00 快速移动 - 空移 */
        if (moved) {
            Segment seg; memset(&seg, 0, sizeof(seg));
            seg.type = SEG_G00;
            seg.start = ctx->current;
            seg.end   = target;
            seg.is_cut = 0;
            seg.depth_start = ctx->current.z;
            seg.depth_end   = target.z;
            push_segment(ctx, &seg);
            ctx->current = target;
            update_bounds(ctx, &ctx->current);
        }
    }
    else if (g_motion == 1 || g_motion < 0) {
        /* G01 直线切削 (默认模态) */
        if (moved) {
            /* Z 轴单独快速移动视为 SEG_Z_RAPID */
            int xy_moved = has_x || has_y;
            Segment seg; memset(&seg, 0, sizeof(seg));
            if (!xy_moved && (ctx->current.z != target.z)) {
                seg.type = SEG_Z_RAPID;
                seg.start = ctx->current;
                seg.end   = target;
                seg.depth_start = ctx->current.z;
                seg.depth_end   = target.z;
                seg.is_cut = (ctx->tool_on && target.z < ctx->current.z);
            } else {
                seg.type = SEG_LINE;
                seg.start = ctx->current;
                seg.end   = target;
                seg.is_cut = ctx->tool_on;
            }
            push_segment(ctx, &seg);
            ctx->current = target;
            update_bounds(ctx, &ctx->current);
        }
    }
    else if (g_motion == 2 || g_motion == 3) {
        /* G02/G03 圆弧插补 */
        if (moved) {
            Segment seg; memset(&seg, 0, sizeof(seg));
            seg.type = (g_motion == 2) ? SEG_ARC_CW : SEG_ARC_CCW;
            seg.start = ctx->current;
            seg.end   = target;
            seg.is_cut = ctx->tool_on;
            if (ctx->abs_mode) {
                /* I/J 永远是相对于起点的增量 */
                seg.i = v_i;
                seg.j = v_j;
            } else {
                seg.i = v_i;
                seg.j = v_j;
            }
            seg.r = has_r ? v_r : 0;
            push_segment(ctx, &seg);
            ctx->current = target;
            update_bounds(ctx, &ctx->current);
        }
    }
    else if (g_cycle == 81 || g_cycle == 83 || g_cycle == 84 || g_cycle == 85) {
        /* 固定循环简化处理 */
        /* G81/G85 简单钻孔: 快速到XY, 下钻Z, 退刀Z */
        /* R 平面 = v_r; Z = target.z 为最终深度 */
        /* P 暂停时间忽略 */
        Point xy = target; xy.z = has_r ? v_r : 0;
        /* 1. 快速到 R 平面上方 (简化: 直接到 R) */
        Segment seg1; memset(&seg1,0,sizeof(seg1));
        seg1.type = SEG_Z_RAPID;
        seg1.start = ctx->current;
        seg1.end   = xy;
        seg1.depth_start = ctx->current.z;
        seg1.depth_end   = xy.z;
        seg1.is_cut = 0;
        push_segment(ctx, &seg1);
        ctx->current = xy;

        /* 2. 钻孔下 Z */
        Point bot = xy; bot.z = target.z;
        Segment seg2; memset(&seg2,0,sizeof(seg2));
        seg2.type = SEG_DRILL;
        seg2.start = ctx->current;
        seg2.end   = bot;
        seg2.depth_start = ctx->current.z;
        seg2.depth_end   = bot.z;
        seg2.is_cut = 1;
        push_segment(ctx, &seg2);
        ctx->current = bot;
        update_bounds(ctx, &ctx->current);

        /* 3. 退到 R 平面 */
        Point ret = bot; ret.z = xy.z;
        Segment seg3; memset(&seg3,0,sizeof(seg3));
        seg3.type = SEG_Z_RAPID;
        seg3.start = ctx->current;
        seg3.end   = ret;
        seg3.depth_start = ctx->current.z;
        seg3.depth_end   = ret.z;
        seg3.is_cut = 0;
        push_segment(ctx, &seg3);
        ctx->current = ret;
    }
    else if (g_cycle == 80) {
        /* G80 取消循环 */
    }

    return 0;
}

int gcode_parse_file(GCodeContext *ctx, const char *path) {
    if (!ctx || !path) return -1;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "无法打开文件: %s\n", path);
        return -1;
    }
    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        gcode_parse_line(ctx, line);
    }
    fclose(fp);
    return 0;
}

/* ---------- 统计/访问器 ---------- */

void gcode_print_stats(const GCodeContext *ctx) {
    if (!ctx) return;
    int lines=0, arcs=0, drills=0;
    for (int i=0;i<ctx->seg_count;i++) {
        SegmentType t = ctx->segs[i].type;
        if (t==SEG_LINE||t==SEG_G00||t==SEG_Z_RAPID) lines++;
        else if (t==SEG_ARC_CW||t==SEG_ARC_CCW) arcs++;
        else if (t==SEG_DRILL) drills++;
    }
    printf("=== G 代码解析统计 ===\n");
    printf("总段数: %d\n", ctx->seg_count);
    printf("  直线/移动段: %d\n", lines);
    printf("  圆弧段:     %d\n", arcs);
    printf("  钻削段:     %d\n", drills);
    if (ctx->has_bounds) {
        printf("XY 范围: [%.3f, %.3f] x [%.3f, %.3f]\n",
               ctx->min_x, ctx->max_x, ctx->min_y, ctx->max_y);
        printf("Z  范围: [%.3f, %.3f]\n", ctx->min_z, ctx->max_z);
    }
}

int gcode_segment_count(const GCodeContext *ctx) {
    return ctx ? ctx->seg_count : 0;
}

const Segment* gcode_get_segment(const GCodeContext *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->seg_count) return NULL;
    return &ctx->segs[idx];
}

/* ---------- 圆弧几何 ---------- */

int gcode_arc_center(const Segment *seg, double *cx, double *cy) {
    if (!seg || !cx || !cy) return 0;
    if (seg->type != SEG_ARC_CW && seg->type != SEG_ARC_CCW) return 0;

    double x0 = seg->start.x, y0 = seg->start.y;
    double x1 = seg->end.x,   y1 = seg->end.y;

    if ((seg->i == 0 && seg->j == 0 && seg->r == 0) ||
        (x0 == x1 && y0 == y1)) {
        /* 退化: 当作 360 度圆? 这里失败 */
        return 0;
    }

    if (seg->i != 0 || seg->j != 0) {
        /* I/J 圆心 */
        *cx = x0 + seg->i;
        *cy = y0 + seg->j;
        return 1;
    }

    /* 用 R 求圆心 */
    double dx = x1 - x0, dy = y1 - y0;
    double d2 = dx*dx + dy*dy;
    if (d2 == 0) return 0;
    double r = seg->r;
    double d = sqrt(d2);
    if (d > 2.0 * fabs(r)) return 0;
    /* 选择圆心在弦的法线方向 */
    /* 符号: G03 逆时针, 圆心使得从起点到终点为逆时针 */
    double cx0 = (x0 + x1) / 2.0;
    double cy0 = (y0 + y1) / 2.0;
    double h_sq = r*r - (d/2.0)*(d/2.0);
    if (h_sq < 0) h_sq = 0;
    double h = sqrt(h_sq);
    /* 法向 (-dy/d, dx/d) */
    double nx = -dy / d;
    double ny =  dx / d;

    int ccw = (seg->type == SEG_ARC_CCW);
    /* 判定正确的侧: 向量 (x0->cx, y0->cy) 应满足 r^2 接近 */
    double a_sign = (r >= 0) ? 1.0 : -1.0;
    double test_cx1 = cx0 + a_sign * h * nx;
    double test_cy1 = cy0 + a_sign * h * ny;
    double test_cx2 = cx0 - a_sign * h * nx;
    double test_cy2 = cy0 - a_sign * h * ny;

    /* 哪一个使旋转方向正确: 用叉积判断 */
    {
        double v0x = x0 - test_cx1, v0y = y0 - test_cy1;
        double v1x = x1 - test_cx1, v1y = y1 - test_cy1;
        double cross = v0x*v1y - v0y*v1x;
        int ok1 = ccw ? (cross > 0) : (cross < 0);
        if (ok1) {
            *cx = test_cx1; *cy = test_cy1;
        } else {
            *cx = test_cx2; *cy = test_cy2;
        }
    }
    return 1;
}

int gcode_arc_sample(const Segment *seg, double tol, Point *pts, int max_pts) {
    if (!seg || !pts || max_pts < 2) return 0;
    double cx, cy;
    if (!gcode_arc_center(seg, &cx, &cy)) return 0;

    double r = sqrt((seg->start.x-cx)*(seg->start.x-cx) +
                    (seg->start.y-cy)*(seg->start.y-cy));
    if (r <= 0) return 0;

    double a0 = atan2(seg->start.y - cy, seg->start.x - cx);
    double a1 = atan2(seg->end.y   - cy, seg->end.x   - cx);
    int ccw = (seg->type == SEG_ARC_CCW);

    /* 确保 sweep 方向 */
    double sweep = a1 - a0;
    if (ccw) {
        while (sweep <= 0) sweep += 2*M_PI;
    } else {
        while (sweep >= 0) sweep -= 2*M_PI;
    }
    /* 完整圆: 若起点==终点 */
    if (seg->start.x == seg->end.x && seg->start.y == seg->end.y) {
        sweep = ccw ? 2*M_PI : -2*M_PI;
    }

    /* 按弦高误差求分段数: seg_angle <= 2*acos(1 - tol/r) */
    double ang_step = 2.0 * acos(1.0 - fmin(tol / fmax(r,1e-6), 1.0));
    if (ang_step < 1e-5) ang_step = 0.01;
    int n = (int)(fabs(sweep) / ang_step) + 1;
    if (n < 2) n = 2;
    if (n > max_pts - 1) n = max_pts - 1;

    pts[0] = seg->start;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n;
        double a = a0 + sweep * t;
        pts[i].x = cx + r * cos(a);
        pts[i].y = cy + r * sin(a);
        pts[i].z = seg->start.z + (seg->end.z - seg->start.z) * t;
    }
    return n + 1;
}
