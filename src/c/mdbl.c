// ═══════════════════════════════════════════════════════════════════════════════
// Pebble On Wrist AI — 手表端 C 代码
//
// 架构概览：
//   单文件实现，包含主界面、对话列表菜单、模型选择菜单、
//   动画系统、AppMessage 通讯、状态机管理。
//
// 状态机流转：
//   IDLE_NO_KEY → (收到 API Key) → IDLE_READY
//   IDLE_READY  → (按 SELECT)    → 语音输入 → SENDING → THINKING → SHRINKING → RESPONSE
//   RESPONSE    → (按 SELECT)    → EXPANDING → IDLE_READY (继续追问)
//   任何状态    → (长按 SELECT) → 新建对话 → IDLE_READY
//
// 内存预算 (Basalt 24KB 可用 RAM)：
//   s_reply_buf:    2048 字节
//   s_chat_entries:  20 x 56 = 1120 字节
//   s_model_names:   5 x 48 = 240 字节
//   UI 元素 + 其他:  ~2KB
//   总计: ~5.5KB，安全。
// ═══════════════════════════════════════════════════════════════════════════════
#include <pebble.h>

// ── 状态机 ───────────────────────────────────────────────────────────────────
typedef enum {
  STATE_IDLE_NO_KEY,   // 未配置 API Key，圆圈灰色，提示用户去设置
  STATE_IDLE_READY,    // 已就绪，显示蓝色大圆 + 当前对话标题
  STATE_RECORDING,     // 语音输入中（3点跳动动画）
  STATE_SENDING,       // 正在发送问题到手机（箭头动画）
  STATE_THINKING,      // 等待 AI 回复（呼吸圆 + 旋转小点）
  STATE_SHRINKING,     // 大圆缩小动画（过渡到回复显示）
  STATE_EXPANDING,     // 小圆放大动画（从回复回到大圆继续追问）
  STATE_RESPONSE       // 显示 AI 回复文本（可滚动）
} AppState;

static AppState s_state = STATE_IDLE_NO_KEY;

// ── 配色方案 ──────────────────────────────────────────────────────────────────
// 多彩滩涂背景 + 白色 logo，放弃蓝色圆设计
#define C_TEXT_DARK   GColorWhite       // logo / 圆上文字
#define C_TEXT_LIGHT  GColorBlack       // 白底上的文字
#define C_SUBTITLE    GColorWhite       // 副标题（白色，在彩色背景上）
#define C_ACCENT      GColorBlue        // 强调色（菜单蓝字）
#define C_SHADOW      GColorDarkGray    // logo 阴影色

// 菜单窗口仍使用白底
#define C_BG          GColorWhite
#define C_DOT_ON      GColorBlue
#define C_DOT_OFF     GColorLightGray

// ── 滩涂海浪背景 ─────────────────────────────────────────────────────────────
// 6 层海浪色带，每次启动随机从调色板中选取
#define WAVE_LAYERS 6
static GColor s_wave_colors[WAVE_LAYERS];
static int s_wave_offsets[WAVE_LAYERS];   // 每层波浪的水平相位偏移（随机）
static int s_wave_amp[WAVE_LAYERS];       // 每层波浪振幅（随机 3~8）

// 彩虹脉冲（thinking 时向上涌动）
static int s_pulse_offset = 0;           // thinking 脉冲偏移量

// 调色板：暖色滩涂系（Pebble 64 色中挑选的柔和色）
static const GColor s_palette[] = {
  { .argb = GColorSunsetOrangeARGB8 },
  { .argb = GColorMelonARGB8 },
  { .argb = GColorRajahARGB8 },
  { .argb = GColorIcterineARGB8 },
  { .argb = GColorMintGreenARGB8 },
  { .argb = GColorCelesteARGB8 },
  { .argb = GColorPictonBlueARGB8 },
  { .argb = GColorVividVioletARGB8 },
  { .argb = GColorLavenderIndigoARGB8 },
  { .argb = GColorBabyBlueEyesARGB8 },
  { .argb = GColorInchwormARGB8 },
  { .argb = GColorSpringBudARGB8 },
};
#define PALETTE_SIZE 12

// ── 布局常量 ─────────────────────────────────────────────────────────────────
#define CIRCLE_R_SMALL  14
#define CIRCLE_Y_SMALL  PBL_IF_ROUND_ELSE(28, 20)
#define SCROLL_OFFSET   40

// 动态布局（从屏幕尺寸计算）
static int s_circle_r_big;
static int s_circle_y_big;
static int s_subtitle_y;

// ── 动画 ─────────────────────────────────────────────────────────────────────
static int s_anim_frame = 0;
static AppTimer *s_anim_timer = NULL;

static int s_circle_x;       // 当前中心X
static int s_circle_r;       // 当前半径（动画中）
static int s_circle_y;       // 当前中心Y
static int s_circle_target_x;// 收缩动画的目标 X
static int s_morph_step;     // 缩放动画步数
#define MORPH_TOTAL 18       // 动画总帧数

static int s_pulse_phase = 0;        // 动画相位（海浪波浪 + 动画共用）
static int s_logo_draw_phase = 0;    // logo 描绘动画相位（进入 idle 时笔画逐段出现）
static int s_arc_angle = 0;          // thinking 旋转弧线角度
static int s_logo_morph = 0;         // Wi→圆 变形进度 (0=Wi logo, 16=完整圆环)
#define LOGO_MORPH_TOTAL 16

static bool s_user_scrolled = false;  // 用户手动滚动过则停止自动滚动到底部

static AppTimer *s_thinking_timeout = NULL;
#define THINKING_TIMEOUT_MS 90000    // THINKING 状态超时 90 秒（从 SENDING 开始算）


// ── UI 元素 ──────────────────────────────────────────────────────────────────
static Window      *s_window;
static Layer       *s_canvas_layer;
static ScrollLayer *s_scroll_layer;
static TextLayer   *s_reply_layer;
static TextLayer   *s_question_display_layer;  // 回复区顶部的问题

// ── Dictation ────────────────────────────────────────────────────────────────
static DictationSession *s_dictation_session;

// ── 对话列表（从手机接收）──────────────────────────────────────────────────────
#define MAX_WATCH_CHATS 20   // RAM: 20 × 48 bytes = 960 bytes（Basalt 24KB 内安全）
#define CHAT_ID_LEN     20   // JS 生成的 ID 长度约 13 字符，留充走余量
#define CHAT_TITLE_LEN  36

typedef struct {
  char id[CHAT_ID_LEN];
  char title[CHAT_TITLE_LEN];
} WatchChatEntry;

static WatchChatEntry s_chat_entries[MAX_WATCH_CHATS];
static int s_chat_count = 0;
static int s_active_chat_index = -1;

// 模型列表（从手机接收）
#define MAX_MODELS 5
#define MODEL_NAME_LEN 48

static char s_model_names[MAX_MODELS][MODEL_NAME_LEN];
static int s_model_count = 0;
static int s_current_model_index = 0;
static char s_current_model_display[MODEL_NAME_LEN];  // 当前模型显示名

// 对话列表窗口
static Window    *s_list_window;
static MenuLayer *s_menu_layer;

// 模型选择窗口
static Window    *s_model_window;
static MenuLayer *s_model_menu_layer;

// ── 持久存储 key ─────────────────────────────────────────────────────────────
#define PERSIST_KEY_READY 1

#define R_BUF_SIZE 2048
static char s_reply_buf[R_BUF_SIZE];

#define Q_DISPLAY_SIZE 128
static char s_question_display[Q_DISPLAY_SIZE];  // 回复区顶部显示的问题

#define DICTATION_BUF_SIZE 1024  // 转录 buffer（~10-15句话）

// ── 屏幕尺寸 ─────────────────────────────────────────────────────────────────
static int s_width, s_height;

// ── 前向声明 ─────────────────────────────────────────────────────────────────
static void set_state(AppState new_state);
static void anim_tick(void *data);
static void dictation_callback(DictationSession *session,
                                DictationSessionStatus status,
                                char *transcription,
                                void *context);
static void parse_chat_list(const char *data);
static void parse_model_list(const char *data);
static void show_chat_list(void);
static void show_model_select(void);

// ═══════════════════════════════════════════════════════════════════════════════
// 回复文本刷新
//
// 由于采用非流式 API，文本到达后直接设置到 TextLayer，
// 同时动态调整 ScrollLayer 可滚动区域大小以适应文本长度。
// ═══════════════════════════════════════════════════════════════════════════════
static void update_response_text(void) {
  text_layer_set_text(s_reply_layer, s_reply_buf);

  // 动态更新滚动区域跟随文字增长
  if (s_state == STATE_RESPONSE) {
    GSize text_size = text_layer_get_content_size(s_reply_layer);
    GSize q_size = text_layer_get_content_size(s_question_display_layer);
    int content_h = q_size.h + 12 + text_size.h + 20;
    int scroll_top = CIRCLE_Y_SMALL + CIRCLE_R_SMALL + 10;
    int scroll_h = s_height - scroll_top;
    if (content_h < scroll_h) content_h = scroll_h;
    scroll_layer_set_content_size(s_scroll_layer, GSize(s_width, content_h));

    // 自动滚动到底部（除非用户手动滚过）
    if (!s_user_scrolled) {
      int max_offset = content_h - scroll_h;
      if (max_offset > 0) {
        scroll_layer_set_content_offset(s_scroll_layer,
                                        GPoint(0, -max_offset), true);
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// 缓动函数（ease-out quad: 减速）
// ═══════════════════════════════════════════════════════════════════════════════
static int ease_out(int from, int to, int step, int total) {
  // t = step/total (0..1), ease = 1-(1-t)^2
  int d = to - from;
  int t1000 = (step * 1000) / total;  // 0..1000
  int inv = 1000 - t1000;
  int ease1000 = 1000 - (inv * inv / 1000);
  return from + (d * ease1000 / 1000);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 绘制辅助
// ═══════════════════════════════════════════════════════════════════════════════

// 滩涂海浪背景：多层彩色波浪由上至下铺满屏幕
// pulse_up: >0 时波浪整体向上偏移（thinking 脉冲效果）
static void draw_wave_bg(GContext *ctx, int pulse_up) {
  int layer_h = s_height / WAVE_LAYERS;
  for (int i = 0; i < WAVE_LAYERS; i++) {
    int base_y = i * layer_h - pulse_up;
    GColor c = s_wave_colors[i];
    graphics_context_set_fill_color(ctx, c);

    // 先填充该层的矩形主体
    int fill_y = (base_y < 0) ? 0 : base_y;
    int fill_h = layer_h + s_wave_amp[i] + 2;
    if (fill_y + fill_h > s_height) fill_h = s_height - fill_y;
    if (fill_h > 0) {
      graphics_fill_rect(ctx, GRect(0, fill_y, s_width, fill_h), 0, GCornerNone);
    }

    // 波浪曲线：在层顶部用 sin 波画锯齿边缘
    int wave_y_base = base_y;
    int amp = s_wave_amp[i];
    int phase = s_wave_offsets[i] + s_pulse_phase * (i + 1);
    for (int x = 0; x < s_width; x++) {
      int32_t angle = ((x * 3 + phase) % 360) * TRIG_MAX_ANGLE / 360;
      int wave_dy = (sin_lookup(angle) * amp) / TRIG_MAX_RATIO;
      int wy = wave_y_base + wave_dy;
      if (wy >= 0 && wy < s_height) {
        // 画从波浪线到层底部的竖线来填充波浪形状
        int bottom = base_y + layer_h + amp;
        if (bottom > s_height) bottom = s_height;
        if (wy < bottom) {
          graphics_draw_line(ctx, GPoint(x, wy), GPoint(x, bottom));
        }
      }
    }
  }
}

// Wi logo 路径绘制（放大版，白色手写风格 + 阴影）
// scale: 缩放百分比 (100=全尺寸, 用于缩小版)
// morph: 0=纯 Wi logo, LOGO_MORPH_TOTAL=完全变成圆环
// shadow: 是否画阴影
static void draw_wi_logo(GContext *ctx, int cx, int cy, int scale, int draw_phase,
                          int morph, GColor color, bool shadow) {
  int keep = scale;
  int m = morph;  // 0..LOGO_MORPH_TOTAL
  int mt = LOGO_MORPH_TOTAL;
  // 圆环半径（morph 目标）
  int ring_r = 28 * keep / 100;

  // Wi logo 的 9 个控制点（相对坐标）
  int logo_x[] = { -28, -22, -14, -6, 0, 6, 14, 20, 26 };
  int logo_y[] = { -20, -6, 14, -3, -12, -3, 14, -6, -20 };
  // 圆环上 9 个等距点（角度 0~320°，间隔 40°）
  // 加上 i 的竖线端点(2个) + 圆点(1个) = 共 12 个点
  // 但为了简洁，W 的 9 个点 morph 到圆环上 9 个等距位置

  // 计算 morph 后的实际坐标
  GPoint pts[9];
  for (int i = 0; i < 9; i++) {
    int lx = logo_x[i] * keep / 100;
    int ly = logo_y[i] * keep / 100;
    // 圆环目标位置：9 个等距点，从顶部偏左开始
    int deg = 200 + i * 160 / 8;  // 200°~360°（底部弧线）
    int32_t angle = (deg * TRIG_MAX_ANGLE) / 360;
    int rx = (sin_lookup(angle) * ring_r) / TRIG_MAX_RATIO;
    int ry = -(cos_lookup(angle) * ring_r) / TRIG_MAX_RATIO;
    // 线性插值
    int fx = cx + lx + (rx - lx) * m / mt;
    int fy = cy + ly + (ry - ly) * m / mt;
    pts[i] = GPoint(fx, fy);
  }

  // i 的竖线端点
  int i_top_lx = 32 * keep / 100, i_top_ly = -6 * keep / 100;
  int i_bot_lx = 32 * keep / 100, i_bot_ly = 14 * keep / 100;
  int i_dot_lx = 32 * keep / 100, i_dot_ly = -14 * keep / 100;
  // 圆环目标：i 的三个点 morph 到圆环顶部弧段
  int i_top_deg = 120, i_bot_deg = 160, i_dot_deg = 80;
  GPoint i_top, i_bot, i_dot;
  {
    int32_t a;
    int rx, ry;
    a = (i_top_deg * TRIG_MAX_ANGLE) / 360;
    rx = (sin_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    ry = -(cos_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    i_top = GPoint(cx + i_top_lx + (rx - i_top_lx) * m / mt,
                   cy + i_top_ly + (ry - i_top_ly) * m / mt);

    a = (i_bot_deg * TRIG_MAX_ANGLE) / 360;
    rx = (sin_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    ry = -(cos_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    i_bot = GPoint(cx + i_bot_lx + (rx - i_bot_lx) * m / mt,
                   cy + i_bot_ly + (ry - i_bot_ly) * m / mt);

    a = (i_dot_deg * TRIG_MAX_ANGLE) / 360;
    rx = (sin_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    ry = -(cos_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    i_dot = GPoint(cx + i_dot_lx + (rx - i_dot_lx) * m / mt,
                   cy + i_dot_ly + (ry - i_dot_ly) * m / mt);
  }

  // 阴影层
  if (shadow) {
    int sx = 2, sy = 2;
    graphics_context_set_stroke_color(ctx, C_SHADOW);
    graphics_context_set_stroke_width(ctx, 5);
    int sw_segs = (draw_phase >= 16) ? 8 : (draw_phase * 8 / 16);
    for (int i = 0; i < sw_segs; i++) {
      graphics_draw_line(ctx, GPoint(pts[i].x+sx, pts[i].y+sy),
                              GPoint(pts[i+1].x+sx, pts[i+1].y+sy));
    }
    if (draw_phase >= 14) {
      graphics_draw_line(ctx, GPoint(i_top.x+sx, i_top.y+sy),
                              GPoint(i_bot.x+sx, i_bot.y+sy));
    }
    if (draw_phase >= 18) {
      graphics_context_set_fill_color(ctx, C_SHADOW);
      int dr = (3 * keep / 100);
      // morph 时圆点变小
      dr = dr * (mt - m) / mt + 1;
      graphics_fill_circle(ctx, GPoint(i_dot.x+sx, i_dot.y+sy), dr > 0 ? dr : 1);
    }
    // morph 时补画圆环的上半弧阴影（W 的 9 个点只覆盖下半弧）
    if (m > mt / 3) {
      int arc_opacity = (m - mt / 3) * 100 / (mt - mt / 3);  // 0~100 渐入
      if (arc_opacity > 0) {
        graphics_context_set_stroke_width(ctx, 3 + arc_opacity * 2 / 100);
        for (int seg = 0; seg < 8; seg++) {
          int d1 = 10 + seg * 190 / 8;
          int d2 = 10 + (seg + 1) * 190 / 8;
          int32_t a1 = (d1 * TRIG_MAX_ANGLE) / 360;
          int32_t a2 = (d2 * TRIG_MAX_ANGLE) / 360;
          int x1 = cx + sx + (sin_lookup(a1) * ring_r) / TRIG_MAX_RATIO;
          int y1 = cy + sy - (cos_lookup(a1) * ring_r) / TRIG_MAX_RATIO;
          int x2 = cx + sx + (sin_lookup(a2) * ring_r) / TRIG_MAX_RATIO;
          int y2 = cy + sy - (cos_lookup(a2) * ring_r) / TRIG_MAX_RATIO;
          graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
        }
      }
    }
  }

  // 主 logo / 变形中的线条
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 5);

  int w_segs = (draw_phase >= 16) ? 8 : (draw_phase * 8 / 16);
  for (int i = 0; i < w_segs; i++) {
    graphics_draw_line(ctx, pts[i], pts[i + 1]);
  }

  // 蓝色小三角装饰（morph 时渐隐）
  if (draw_phase >= 12 && m < mt * 2 / 3) {
    graphics_context_set_stroke_color(ctx, GColorPictonBlue);
    graphics_context_set_stroke_width(ctx, 2);
    int tri_keep = keep * (mt - m) / mt;  // morph 时缩小
    int tlx = cx + (-9) * tri_keep / 100;
    int trx = cx + (-3) * tri_keep / 100;
    int tty = cy + 11 * tri_keep / 100;
    int tbx = cx + (-6) * tri_keep / 100;
    int tby = cy + 16 * tri_keep / 100;
    graphics_draw_line(ctx, GPoint(tlx, tty), GPoint(tbx, tby));
    graphics_draw_line(ctx, GPoint(trx, tty), GPoint(tbx, tby));
    graphics_context_set_stroke_color(ctx, color);
    graphics_context_set_stroke_width(ctx, 5);
  }

  // i 竖线
  if (draw_phase >= 14) {
    graphics_draw_line(ctx, i_top, i_bot);
  }

  // i 圆点（morph 时缩小消失）
  if (draw_phase >= 18) {
    int dot_r = 3 * keep / 100;
    dot_r = dot_r * (mt - m) / mt;
    if (dot_r > 0) {
      graphics_context_set_fill_color(ctx, color);
      graphics_fill_circle(ctx, i_dot, dot_r);
    }
  }

  // morph 时补画圆环的上半弧（W 的 9 个点只覆盖下半弧 200°~360°）
  if (m > mt / 3) {
    int arc_opacity = (m - mt / 3) * 100 / (mt - mt / 3);
    if (arc_opacity > 0) {
      graphics_context_set_stroke_width(ctx, 3 + arc_opacity * 2 / 100);
      for (int seg = 0; seg < 8; seg++) {
        int d1 = 10 + seg * 190 / 8;
        int d2 = 10 + (seg + 1) * 190 / 8;
        int32_t a1 = (d1 * TRIG_MAX_ANGLE) / 360;
        int32_t a2 = (d2 * TRIG_MAX_ANGLE) / 360;
        int x1 = cx + (sin_lookup(a1) * ring_r) / TRIG_MAX_RATIO;
        int y1 = cy - (cos_lookup(a1) * ring_r) / TRIG_MAX_RATIO;
        int x2 = cx + (sin_lookup(a2) * ring_r) / TRIG_MAX_RATIO;
        int y2 = cy - (cos_lookup(a2) * ring_r) / TRIG_MAX_RATIO;
        graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
      }
    }
  }
}

// 大旋转弧线圆（thinking 状态，带阴影）
static void draw_spin_circle(GContext *ctx, int cx, int cy, int r, GColor color, bool shadow) {
  // 阴影
  if (shadow) {
    graphics_context_set_stroke_color(ctx, C_SHADOW);
    graphics_context_set_stroke_width(ctx, 4);
    graphics_draw_circle(ctx, GPoint(cx + 2, cy + 2), r);
  }

  // 主圆环
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 3);
  graphics_draw_circle(ctx, GPoint(cx, cy), r);

  // 旋转弧线：两段 90° 弧，对称分布
  graphics_context_set_stroke_width(ctx, 5);
  int arc_segments = 10;
  for (int a = 0; a < 2; a++) {
    int base = s_arc_angle + a * 180;
    for (int i = 0; i < arc_segments; i++) {
      int deg1 = base + (i * 90 / arc_segments);
      int deg2 = base + ((i + 1) * 90 / arc_segments);
      int32_t a1 = (deg1 * TRIG_MAX_ANGLE) / 360;
      int32_t a2 = (deg2 * TRIG_MAX_ANGLE) / 360;
      int x1 = cx + (sin_lookup(a1) * r) / TRIG_MAX_RATIO;
      int y1 = cy - (cos_lookup(a1) * r) / TRIG_MAX_RATIO;
      int x2 = cx + (sin_lookup(a2) * r) / TRIG_MAX_RATIO;
      int y2 = cy - (cos_lookup(a2) * r) / TRIG_MAX_RATIO;
      graphics_draw_line(ctx, GPoint(x1, y1), GPoint(x2, y2));
    }
  }
}

// RECORDING 状态的 3 个跳动圆点
static void draw_dots(GContext *ctx, int cx, int cy) {
  int spacing = 20;
  for (int i = 0; i < 3; i++) {
    int phase = (s_anim_frame * 2 + i * 4) % 16;
    int dy = 0;
    if (phase < 4) dy = -phase * 3;
    else if (phase < 8) dy = -(8 - phase) * 3;
    int x = cx + (i - 1) * spacing;
    int y = cy + dy;
    bool active = (phase < 8);
    // 阴影
    graphics_context_set_fill_color(ctx, C_SHADOW);
    graphics_fill_circle(ctx, GPoint(x + 1, y + 1), active ? 6 : 4);
    // 白色点
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, GPoint(x, y), active ? 6 : 4);
  }
}

// 上箭头（发送动画，白色带阴影）
static void draw_arrow(GContext *ctx, int cx, int cy) {
  int lift = (s_anim_frame % 6) * 2;
  int ay = cy - lift;
  // 阴影
  graphics_context_set_stroke_color(ctx, C_SHADOW);
  graphics_context_set_stroke_width(ctx, 4);
  graphics_draw_line(ctx, GPoint(cx+1, ay+13), GPoint(cx+1, ay-7));
  graphics_draw_line(ctx, GPoint(cx+1, ay-7), GPoint(cx-6, ay+1));
  graphics_draw_line(ctx, GPoint(cx+1, ay-7), GPoint(cx+8, ay+1));
  // 白色
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 3);
  graphics_draw_line(ctx, GPoint(cx, ay + 12), GPoint(cx, ay - 8));
  graphics_draw_line(ctx, GPoint(cx, ay - 8), GPoint(cx - 7, ay));
  graphics_draw_line(ctx, GPoint(cx, ay - 8), GPoint(cx + 7, ay));
}

// 居中文字（白色带阴影）
static void draw_text(GContext *ctx, const char *text,
                       GFont font, int cy, GColor color) {
  int x_pad = PBL_IF_ROUND_ELSE(20, 0);
  GRect box = GRect(x_pad, cy - 14, s_width - x_pad * 2, 30);
  // 阴影
  GRect sbox = GRect(box.origin.x + 1, box.origin.y + 1, box.size.w, box.size.h);
  graphics_context_set_text_color(ctx, C_SHADOW);
  graphics_draw_text(ctx, text, font, sbox,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  // 主文字
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, box,
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 主绘制
// ═══════════════════════════════════════════════════════════════════════════════
static void canvas_draw(Layer *layer, GContext *ctx) {
  int cx = s_width / 2;
  int cy = s_circle_y_big;
  GFont font_sub   = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont font_small = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  switch (s_state) {
    case STATE_IDLE_NO_KEY:
      // 滩涂海浪背景（静态）
      draw_wave_bg(ctx, 0);
      // Wi logo（灰暗色调表示未就绪）
      draw_wi_logo(ctx, cx, cy, 100, s_logo_draw_phase, 0, GColorLightGray, true);
      draw_text(ctx, "Set API in Pebble app", font_small, s_subtitle_y, GColorWhite);
      break;

    case STATE_IDLE_READY:
      // 滩涂海浪背景
      draw_wave_bg(ctx, 0);
      // 白色 Wi logo 带阴影
      draw_wi_logo(ctx, cx, cy, 100, s_logo_draw_phase, 0, GColorWhite, true);
      {
        const char *title = (s_active_chat_index >= 0 && s_active_chat_index < s_chat_count) ? s_chat_entries[s_active_chat_index].title : "New chat";
        draw_text(ctx, title, font_sub, s_subtitle_y, GColorWhite);
      }
      break;

    case STATE_RECORDING:
      // Wi 正在 morph 成圆 + 跳动圆点叠加
      draw_wave_bg(ctx, 0);
      draw_wi_logo(ctx, cx, cy, 100, 20, s_logo_morph, GColorWhite, true);
      if (s_logo_morph >= LOGO_MORPH_TOTAL) {
        draw_dots(ctx, cx, cy);
      }
      draw_text(ctx, "Listening...", font_small, s_subtitle_y, GColorWhite);
      break;

    case STATE_SENDING:
      // Wi morph 成圆 + 箭头叠加
      draw_wave_bg(ctx, 0);
      draw_wi_logo(ctx, cx, cy, 100, 20, s_logo_morph, GColorWhite, true);
      if (s_logo_morph >= LOGO_MORPH_TOTAL) {
        draw_arrow(ctx, cx, cy);
      }
      draw_text(ctx, "Sending...", font_small, s_subtitle_y, GColorWhite);
      break;

    case STATE_THINKING: {
      // 彩虹向上脉冲背景
      draw_wave_bg(ctx, s_pulse_offset);
      // 大旋转弧线圆（带阴影）
      int spin_r = s_circle_r_big - 4;
      draw_spin_circle(ctx, cx, cy, spin_r, GColorWhite, true);
      draw_text(ctx, "Thinking...", font_small, s_subtitle_y, GColorWhite);
      break;
    }

    case STATE_SHRINKING:
    case STATE_EXPANDING: {
      draw_wave_bg(ctx, 0);
      int logo_scale = s_circle_r * 100 / s_circle_r_big;
      if (logo_scale < 25) logo_scale = 25;
      draw_wi_logo(ctx, s_circle_x, s_circle_y, logo_scale, 20,
                   s_logo_morph, GColorWhite, logo_scale > 40);
      break;
    }

    case STATE_RESPONSE: {
      // 回复界面：白色背景 + 顶部小 Wi logo + 标题
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

      // 顶部彩色条纹装饰（3px 高）
      for (int i = 0; i < WAVE_LAYERS && i < 6; i++) {
        int bar_w = s_width / WAVE_LAYERS;
        graphics_context_set_fill_color(ctx, s_wave_colors[i]);
        graphics_fill_rect(ctx, GRect(i * bar_w, 0, bar_w + 1, 3), 0, GCornerNone);
      }

      const char *title = (s_active_chat_index >= 0 && s_active_chat_index < s_chat_count) ? s_chat_entries[s_active_chat_index].title : "New chat";
      GSize title_size = graphics_text_layout_get_content_size(title, fonts_get_system_font(FONT_KEY_GOTHIC_14), GRect(0, 0, s_width, 20), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
      // 小 Wi logo + 标题居中排列
      int wi_w = 20;  // 缩小版 Wi 宽度
      int total_w = wi_w + 5 + title_size.w;
      int start_x = (s_width - total_w) / 2;
      int wi_cx = start_x + wi_w / 2;
      // 画缩小版 Wi logo
      draw_wi_logo(ctx, wi_cx, CIRCLE_Y_SMALL, 30, 20, 0, GColorDarkGray, false);
      // 标题
      graphics_context_set_text_color(ctx, GColorDarkGray);
      GRect title_box = GRect(start_x + wi_w + 5, CIRCLE_Y_SMALL - 7, title_size.w + 10, 20);
      graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_14), title_box, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
      // 分隔线
      graphics_context_set_fill_color(ctx, GColorLightGray);
      graphics_fill_rect(ctx, GRect(s_width / 4, CIRCLE_Y_SMALL + CIRCLE_R_SMALL + 6, s_width / 2, 1), 0, GCornerNone);
      break;
    }
    default:
      break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// 动画引擎
//
// 统一的帧驱动机制：所有动画共用 anim_tick 回调，根据当前状态执行不同逻辑
// 帧率：IDLE 10fps（省电）→ THINKING/RECORDING 20fps → SHRINKING/EXPANDING 40fps（流畅）
// ═══════════════════════════════════════════════════════════════════════════════
static void start_anim(void) {
  s_anim_frame = 0;
  if (s_anim_timer) app_timer_cancel(s_anim_timer);
  s_anim_timer = app_timer_register(50, anim_tick, NULL);
}

static void stop_anim(void) {
  if (s_anim_timer) {
    app_timer_cancel(s_anim_timer);
    s_anim_timer = NULL;
  }
}

static void anim_tick(void *data) {
  s_anim_frame++;
  s_pulse_phase++;

  // logo 描绘动画（idle 状态前 20 帧逐段出现）
  if ((s_state == STATE_IDLE_NO_KEY || s_state == STATE_IDLE_READY) && s_logo_draw_phase < 20) {
    s_logo_draw_phase++;
  }

  // Wi→圆 morph 驱动
  // RECORDING/SENDING/THINKING/SHRINKING: morph 递增（Wi→圆）
  if (s_state == STATE_RECORDING || s_state == STATE_SENDING ||
      s_state == STATE_THINKING || s_state == STATE_SHRINKING) {
    if (s_logo_morph < LOGO_MORPH_TOTAL) s_logo_morph++;
  }
  // EXPANDING: morph 递减（圆→Wi）
  if (s_state == STATE_EXPANDING) {
    if (s_logo_morph > 0) s_logo_morph--;
  }

  // thinking 旋转弧线 + 彩虹脉冲
  if (s_state == STATE_THINKING) {
    s_arc_angle = (s_arc_angle + 8) % 360;
    s_pulse_offset += 2;  // 背景向上涌动
    if (s_pulse_offset > s_height) s_pulse_offset = 0;
  } else {
    s_pulse_offset = 0;
  }

  if (s_state == STATE_SHRINKING) {
    s_morph_step++;
    if (s_morph_step >= MORPH_TOTAL) {
      s_circle_r = CIRCLE_R_SMALL;
      s_circle_y = CIRCLE_Y_SMALL;
      s_circle_x = s_circle_target_x;
      set_state(STATE_RESPONSE);
      return;
    }
    s_circle_r = ease_out(s_circle_r_big, CIRCLE_R_SMALL, s_morph_step, MORPH_TOTAL);
    s_circle_y = ease_out(s_circle_y_big, CIRCLE_Y_SMALL, s_morph_step, MORPH_TOTAL);
    s_circle_x = ease_out(s_width / 2, s_circle_target_x, s_morph_step, MORPH_TOTAL);
  }

  if (s_state == STATE_EXPANDING) {
    s_morph_step++;
    if (s_morph_step >= MORPH_TOTAL) {
      s_circle_r = s_circle_r_big;
      s_circle_y = s_circle_y_big;
      s_circle_x = s_width / 2;
      s_dictation_session = dictation_session_create(DICTATION_BUF_SIZE, dictation_callback, NULL);
      if (s_dictation_session) {
        dictation_session_start(s_dictation_session);
      } else {
        set_state(STATE_IDLE_READY);
      }
      return;
    }
    s_circle_r = ease_out(CIRCLE_R_SMALL, s_circle_r_big, s_morph_step, MORPH_TOTAL);
    s_circle_y = ease_out(CIRCLE_Y_SMALL, s_circle_y_big, s_morph_step, MORPH_TOTAL);
    s_circle_x = ease_out(s_circle_target_x, s_width / 2, s_morph_step, MORPH_TOTAL);
  }

  layer_mark_dirty(s_canvas_layer);

  int interval = 50;  // 默认 20fps
  if (s_state == STATE_SHRINKING || s_state == STATE_EXPANDING) interval = 25;  // 40fps
  if (s_state == STATE_IDLE_NO_KEY || s_state == STATE_IDLE_READY) {
    interval = 100;  // 10fps
    // logo 描绘完成后停止动画省电（25帧 × 100ms = 2.5s）
    if (s_anim_frame > 25 && s_logo_draw_phase >= 20) {
      s_anim_timer = NULL;
      return;
    }
  }
  s_anim_timer = app_timer_register(interval, anim_tick, NULL);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SENDING → THINKING 延迟
// ═══════════════════════════════════════════════════════════════════════════════
static void sending_done_timer(void *data) {
  if (s_state == STATE_SENDING) {
    set_state(STATE_THINKING);
  }
}

// THINKING 超时恢复
static void thinking_timeout_handler(void *data) {
  s_thinking_timeout = NULL;
  if (s_state == STATE_THINKING) {
    snprintf(s_reply_buf, sizeof(s_reply_buf), "Timed out. Try again.");
    text_layer_set_text(s_reply_layer, s_reply_buf);
    set_state(STATE_SHRINKING);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Dictation 回调（前向声明需要，放在 anim_tick 之后）
// ═══════════════════════════════════════════════════════════════════════════════
static void dictation_callback(DictationSession *session,
                                DictationSessionStatus status,
                                char *transcription,
                                void *context) {
  if (status == DictationSessionStatusSuccess && transcription && transcription[0] != '\0') {
    // 保存问题用于显示
    snprintf(s_question_display, sizeof(s_question_display), "You: %s", transcription);
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
      dict_write_cstring(iter, MESSAGE_KEY_QUESTION, transcription);
      app_message_outbox_send();
      set_state(STATE_SENDING);
    } else {
      // 发送失败，回到就绪
      set_state(STATE_IDLE_READY);
    }
  } else {
    vibes_short_pulse();  // 提示用户 Dictation 被取消
    set_state(STATE_IDLE_READY);
  }
  dictation_session_destroy(s_dictation_session);
  s_dictation_session = NULL;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 状态切换
// ═══════════════════════════════════════════════════════════════════════════════
static void set_state(AppState new_state) {
  stop_anim();
  s_state = new_state;

  bool show_scroll = (new_state == STATE_RESPONSE);
  layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), !show_scroll);

  switch (new_state) {
    case STATE_IDLE_NO_KEY:
    case STATE_IDLE_READY:
      s_logo_draw_phase = 0;  // 重新播放描绘动画
      s_logo_morph = 0;       // 重置为完整 Wi logo
      s_pulse_offset = 0;
      start_anim();
      break;
    case STATE_RECORDING:
    case STATE_SENDING:
    case STATE_THINKING:
      start_anim();
      break;

    case STATE_SHRINKING:
      s_morph_step = 0;
      s_circle_r = s_circle_r_big;
      s_circle_y = s_circle_y_big;
      {
        const char *title = (s_active_chat_index >= 0 && s_active_chat_index < s_chat_count) ? s_chat_entries[s_active_chat_index].title : "New chat";
        GSize ts = graphics_text_layout_get_content_size(title, fonts_get_system_font(FONT_KEY_GOTHIC_14), GRect(0, 0, s_width, 20), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
        s_circle_target_x = (s_width - (28 + 5 + ts.w)) / 2 + 14;
      }
      start_anim();
      break;

    case STATE_EXPANDING:
      s_morph_step = 0;
      s_circle_r = CIRCLE_R_SMALL;
      s_circle_y = CIRCLE_Y_SMALL;
      start_anim();
      break;

    case STATE_RESPONSE:
      // 回复界面用深色文字（白底）
      text_layer_set_text_color(s_question_display_layer, GColorDarkGray);
      text_layer_set_text(s_question_display_layer, s_question_display);
      {
        int scroll_top = CIRCLE_Y_SMALL + CIRCLE_R_SMALL + 10;
        GSize q_size = text_layer_get_content_size(s_question_display_layer);
        int reply_y = q_size.h + 12;
        layer_set_frame(text_layer_get_layer(s_reply_layer), GRect(8, reply_y, s_width - 16, 2000));
        scroll_layer_set_content_size(s_scroll_layer, GSize(s_width, s_height - scroll_top));
        scroll_layer_set_content_offset(s_scroll_layer, GPointZero, false);
      }
      s_user_scrolled = false;
      update_response_text();
      break;

    default:
      break;
  }

  // SENDING 状态注册延迟
  if (new_state == STATE_SENDING) {
    app_timer_register(800, sending_done_timer, NULL);
    // 启动 THINKING 超时（从 SENDING 开始算）
    if (s_thinking_timeout) app_timer_cancel(s_thinking_timeout);
    s_thinking_timeout = app_timer_register(THINKING_TIMEOUT_MS, thinking_timeout_handler, NULL);
  }

  // 收到回复后取消超时
  if (new_state == STATE_SHRINKING || new_state == STATE_RESPONSE ||
      new_state == STATE_IDLE_READY || new_state == STATE_IDLE_NO_KEY) {
    if (s_thinking_timeout) {
      app_timer_cancel(s_thinking_timeout);
      s_thinking_timeout = NULL;
    }
  }

  layer_mark_dirty(s_canvas_layer);
}

// ═══════════════════════════════════════════════════════════════════════════════
// AppMessage 接收处理
//
// 处理来自手机端的所有消息：
//   READY_STATUS  — API Key 状态（决定 IDLE_NO_KEY / IDLE_READY）
//   CHAT_LIST     — 对话列表数据
//   SWITCH_CHAT   — 当前活跃对话 ID
//   MODEL_NAME    — 模型列表数据
//   REPLY_CHUNK   — AI 回复分块（蓝牙 256 字节限制）
//   REPLY_END     — 回复结束标记
//   REPLY         — 完整回复（兜底）
//   STATUS        — 错误消息
// ═══════════════════════════════════════════════════════════════════════════════
static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *ready_t = dict_find(iter, MESSAGE_KEY_READY_STATUS);
  if (ready_t) {
    bool ready = (ready_t->value->int32 == 1);
    persist_write_bool(PERSIST_KEY_READY, ready);
    if (s_state == STATE_IDLE_NO_KEY || s_state == STATE_IDLE_READY) {
      set_state(ready ? STATE_IDLE_READY : STATE_IDLE_NO_KEY);
    }
  }

  bool list_updated = false;

  // 对话列表 "id1|title1\nid2|title2\n..."
  Tuple *list_t = dict_find(iter, MESSAGE_KEY_CHAT_LIST);
  if (list_t) {
    parse_chat_list(list_t->value->cstring);
    list_updated = true;
  }

  // 活跃对话 ID（标记列表中哪个是当前）
  Tuple *active_t = dict_find(iter, MESSAGE_KEY_SWITCH_CHAT);
  if (active_t) {
    const char *aid = active_t->value->cstring;
    s_active_chat_index = -1;
    for (int i = 0; i < s_chat_count; i++) {
      if (strcmp(s_chat_entries[i].id, aid) == 0) {
        s_active_chat_index = i;
        break;
      }
    }
    list_updated = true;
  }

  if (list_updated) {
    layer_mark_dirty(s_canvas_layer);
    if (s_menu_layer) {
      menu_layer_reload_data(s_menu_layer);
    }
  }

  // 模型列表 "idx|model1\nmodel2\nmodel3"
  Tuple *model_t = dict_find(iter, MESSAGE_KEY_MODEL_NAME);
  if (model_t) {
    parse_model_list(model_t->value->cstring);
    layer_mark_dirty(s_canvas_layer);
    if (s_menu_layer) {
      menu_layer_reload_data(s_menu_layer);
    }
    if (s_model_menu_layer) {
      menu_layer_reload_data(s_model_menu_layer);
    }
  }

  Tuple *reply_t = dict_find(iter, MESSAGE_KEY_REPLY);
  Tuple *reply_chunk_t = dict_find(iter, MESSAGE_KEY_REPLY_CHUNK);
  Tuple *reply_end_t = dict_find(iter, MESSAGE_KEY_REPLY_END);

  if (reply_chunk_t) {
    if (s_state == STATE_SENDING || s_state == STATE_THINKING) {
      s_reply_buf[0] = '\0';
      set_state(STATE_SHRINKING);
    }
    strncat(s_reply_buf, reply_chunk_t->value->cstring, sizeof(s_reply_buf) - strlen(s_reply_buf) - 1);
    if (s_state == STATE_RESPONSE) {
      update_response_text();
    }
  } else if (reply_end_t) {
    if (s_state == STATE_THINKING || s_state == STATE_SENDING) {
      set_state(STATE_SHRINKING);
    }
  } else if (reply_t) {
    snprintf(s_reply_buf, sizeof(s_reply_buf), "%s", reply_t->value->cstring);
    if (s_state == STATE_THINKING || s_state == STATE_SENDING) {
      // 缩小动画 → RESPONSE
      set_state(STATE_SHRINKING);
    } else if (s_state == STATE_RESPONSE) {
      update_response_text();
    }
  }

  Tuple *status_t = dict_find(iter, MESSAGE_KEY_STATUS);
  if (status_t && !reply_t && !reply_chunk_t) {
    if (strlen(s_reply_buf) > 0) {
      strncat(s_reply_buf, "\n\n[Err]: ", sizeof(s_reply_buf) - strlen(s_reply_buf) - 1);
      strncat(s_reply_buf, status_t->value->cstring, sizeof(s_reply_buf) - strlen(s_reply_buf) - 1);
    } else {
      snprintf(s_reply_buf, sizeof(s_reply_buf), "%s", status_t->value->cstring);
    }
    
    // 如果还没进入文字界面，提前进入
    if (s_state == STATE_THINKING || s_state == STATE_SENDING) {
      set_state(STATE_SHRINKING);
    } else if (s_state == STATE_RESPONSE) {
      update_response_text();
    }
  }
}

static void inbox_dropped(AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Dropped: %d", (int)reason);
}

static void outbox_sent(DictionaryIterator *iter, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Sent OK");
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Send fail: %d", (int)reason);
  // 发送失败：如果还在等待中，回退到就绪
  if (s_state == STATE_SENDING || s_state == STATE_THINKING) {
    snprintf(s_reply_buf, sizeof(s_reply_buf), "Send failed. Try again.");
    text_layer_set_text(s_reply_layer, s_reply_buf);
    set_state(STATE_SHRINKING);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// 按钮交互
//
// SELECT 短按：IDLE_READY → 语音输入 / RESPONSE → 回到大圆
// SELECT 长按：新建对话
// UP 短按：RESPONSE → 向上滚动 / 其他 → 打开对话列表菜单
// UP 长按：始终打开对话列表菜单
// DOWN 短按：RESPONSE → 向下滚动 / 其他 → 打开对话列表菜单
// DOWN 长按：彩蛋（随机发送预设问题）
// ═══════════════════════════════════════════════════════════════════════════════
static void select_handler(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_IDLE_READY) {
    // 短按：开始语音输入
    s_dictation_session = dictation_session_create(DICTATION_BUF_SIZE, dictation_callback, NULL);
    if (s_dictation_session) {
      dictation_session_start(s_dictation_session);
    }
  } else if (s_state == STATE_RESPONSE) {
    // 短按回复界面：回到大圆（可继续追问）
    set_state(STATE_EXPANDING);
  }
}

// 长按 SELECT：开新对话
// 保护：已经是空白新对话状态时不重复触发，防止手欠乱点
static void select_long_handler(ClickRecognizerRef r, void *ctx) {
  // 处理中不允许操作
  if (s_state == STATE_SENDING || s_state == STATE_THINKING ||
      s_state == STATE_SHRINKING || s_state == STATE_EXPANDING) {
    vibes_double_pulse();
    return;
  }

  if (s_state == STATE_IDLE_READY || s_state == STATE_RESPONSE) {
    // 保护：仅在当前已经是空白新对话时才阻止重复创建
    bool already_new = (s_state == STATE_IDLE_READY
                        && s_reply_buf[0] == '\0'
                        && s_question_display[0] == '\0'
                        && s_active_chat_index >= 0
                        && s_active_chat_index < s_chat_count
                        && strncmp(s_chat_entries[s_active_chat_index].title, "New chat", 8) == 0);
    if (already_new) {
      vibes_double_pulse();  // 已经是空白新对话，提示用户无需操作
      return;
    }

    vibes_short_pulse();

    // 通知手机清空历史
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
      dict_write_uint8(iter, MESSAGE_KEY_NEW_CHAT, 1);
      app_message_outbox_send();
    }

    // 清空手表端显示
    s_reply_buf[0] = '\0';
    s_question_display[0] = '\0';
    text_layer_set_text(s_reply_layer, "");
    text_layer_set_text(s_question_display_layer, "");
    set_state(STATE_IDLE_READY);
  }
}


// ── 导航与配置 ─────────────────────────────────────────────────────────────────
// UP 短按：在回复状态向上滚动；其他状态打开菜单
static void up_handler(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_RESPONSE) {
    GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
    offset.y += 60;
    if (offset.y > 0) offset.y = 0;
    scroll_layer_set_content_offset(s_scroll_layer, offset, true);
  } else {
    show_chat_list();
  }
}

// UP 长按：始终打开菜单
static void up_long_handler(ClickRecognizerRef r, void *ctx) {
  show_chat_list();
}

// DOWN 短按：回复页面向下滚动 60px，其他状态打开菜单
static void down_handler(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_RESPONSE) {
    GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
    offset.y -= 60;
    scroll_layer_set_content_offset(s_scroll_layer, offset, true);
  } else {
    show_chat_list();
  }
}

// DOWN 长按：彩蛋功能 —— 随机发送一个预设趣味问题给 AI
static void down_long_handler(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_SENDING || s_state == STATE_THINKING ||
      s_state == STATE_SHRINKING || s_state == STATE_EXPANDING) {
    vibes_double_pulse();
    return;
  }

  const char *backdoor_questions[] = {
    "Tell me a random joke. Make it clever or silly, between 80 and 200 characters in the reply.",
    "Give me a quick recipe for a random dish. Keep it under 200 characters, just the key steps.",
    "Share one surprising or little-known fact about anything. Keep it under 180 characters.",
    "Ask me a thought-provoking question about life, humanity, or the future. Then give your own short take on it.",
    "Start a random interesting topic — could be history, science, culture, or anything. Keep it to 100-200 characters."
  };

  int idx = rand() % 5;
  const char *q = backdoor_questions[idx];

  snprintf(s_question_display, sizeof(s_question_display), "Surprise: %s", q);
  
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_QUESTION, q);
    app_message_outbox_send();
    vibes_short_pulse();
    set_state(STATE_SENDING);
  }
}

// 注册所有按钮事件（长按判定阈值 700ms）
static void click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, select_long_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, up_handler);
  window_long_click_subscribe(BUTTON_ID_UP, 700, up_long_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_handler);
  window_long_click_subscribe(BUTTON_ID_DOWN, 700, down_long_handler, NULL);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_width = bounds.size.w;
  s_height = bounds.size.h;

  // 动态布局：大圆位置与半径
  s_circle_r_big = s_width / 4;
  s_circle_y_big = s_height / 2 - 10;
  // 副标题位置：需要空出 spinner 轨道(r+12) + 点半径(4) + 间距
  //   矩形屏：圆底往下 29px（spinner 最远16px + 13px间距）
  //   圆形屏：圆底往下 22px，圆弧边缘留出左右内边距
  s_subtitle_y = s_circle_y_big + s_circle_r_big + PBL_IF_ROUND_ELSE(22, 31);

  // 动画状态初始化
  s_circle_x = s_width / 2;
  s_circle_r = s_circle_r_big;
  s_circle_y = s_circle_y_big;
  // 收缩动画时的圆心 X：圆形屏居中更协调，方形屏居左
  s_circle_target_x = PBL_IF_ROUND_ELSE(s_width / 2, 24);

  // 随机初始化滩涂海浪背景
  srand(time(NULL));
  for (int i = 0; i < WAVE_LAYERS; i++) {
    s_wave_colors[i] = s_palette[rand() % PALETTE_SIZE];
    s_wave_offsets[i] = rand() % s_width;
    s_wave_amp[i] = 3 + (rand() % 6);  // 振幅 3~8 像素
  }

  // Canvas 层
  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, canvas_draw);
  layer_add_child(root, s_canvas_layer);

  // Scroll 层 (回复显示区)
  int scroll_top = CIRCLE_Y_SMALL + CIRCLE_R_SMALL + (PBL_IF_ROUND_ELSE(12, 8));
  s_scroll_layer = scroll_layer_create(GRect(0, scroll_top, s_width, s_height - scroll_top));
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window);
  // 恢复窗口基础点击（覆盖 ScrollLayer 的默认行为以便响应 UP/DOWN）
  window_set_click_config_provider(window, click_config);

  // 禁用 ScrollLayer 上下阴影（dithered shadow）
  scroll_layer_set_shadow_hidden(s_scroll_layer, true);

  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));

  // 问题显示层 (滚动区顶部)
  s_question_display_layer = text_layer_create(GRect(10, 0, s_width - 20, 44));
  text_layer_set_text_color(s_question_display_layer, C_SUBTITLE);
  text_layer_set_background_color(s_question_display_layer, GColorWhite);
  text_layer_set_font(s_question_display_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_question_display_layer, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_question_display_layer));

  // 回复内容层
  s_reply_layer = text_layer_create(GRect(10, 44, s_width - 20, 2000));
  text_layer_set_text_color(s_reply_layer, C_TEXT_LIGHT);
  text_layer_set_background_color(s_reply_layer, GColorWhite);
  text_layer_set_font(s_reply_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_reply_layer, GTextOverflowModeWordWrap);
  text_layer_set_text_alignment(s_reply_layer, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft));

  // 圆形屏适配：开启屏幕流式包装以获得更好的显示效果
  #if defined(PBL_ROUND)
  text_layer_enable_screen_text_flow_and_paging(s_reply_layer, 2);
  #endif

  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_reply_layer));

  // 默认隐藏滚动层
  layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), true);

  // 初始化状态位
  bool ready = persist_exists(PERSIST_KEY_READY) ? persist_read_bool(PERSIST_KEY_READY) : false;
  set_state(ready ? STATE_IDLE_READY : STATE_IDLE_NO_KEY);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
  scroll_layer_destroy(s_scroll_layer);
  text_layer_destroy(s_reply_layer);
  text_layer_destroy(s_question_display_layer);
}

static void window_appear(Window *window) {
  // 刷新以确保显示最新状态
  layer_mark_dirty(s_canvas_layer);
}
// ═══════════════════════════════════════════════════════════════════════════════
// 对话列表窗口 (MenuLayer)
//
// 菜单结构：
//   row 0     — 模型行（显示当前模型名，点击打开模型选择子菜单）
//   row 1     — 「+ START NEW CHAT」按钮
//   row 2+    — 历史对话列表（活跃对话有 Celeste 底色 + 蓝点标记）
//
// 配色规则：
//   模型行 / 新建对话：无底色，蓝字 (C_ACCENT)，高亮时 CobaltBlue 底 + 白字
//   对话行：白底（活跃为 Celeste），高亮时 CobaltBlue 底 + 白字
// ═══════════════════════════════════════════════════════════════════════════════

// 提取模型短名：取最后一个 '/' 后的部分
static const char *model_short_name(const char *full) {
  const char *slash = full;
  const char *last = full;
  while (*slash) {
    if (*slash == '/') last = slash + 1;
    slash++;
  }
  return last;
}

static uint16_t menu_get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  // row 0 = 模型行, row 1 = 新对话, row 2+ = 对话列表
  return 2 + s_chat_count;
}

static int16_t menu_get_cell_height(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (idx->row == 0) return 36;  // 模型行紧凑一些
  return 44;  // 默认高度
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *idx, void *data) {
  GRect bounds = layer_get_bounds(cell_layer);
  MenuLayer *menu = (MenuLayer *)data;
  MenuIndex sel = menu_layer_get_selected_index(menu);
  bool highlighted = (sel.row == idx->row && sel.section == idx->section);

  // 高亮背景（所有行通用）
  if (highlighted) {
    graphics_context_set_fill_color(ctx, GColorCobaltBlue);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  }

  if (idx->row == 0) {
    // ── 模型行：无底色，模型名 + 右侧三个点 ──
    const char *display = s_current_model_display[0] ? model_short_name(s_current_model_display) : "No model";
    int x_pad = PBL_IF_ROUND_ELSE(24, 10);

    graphics_context_set_text_color(ctx, highlighted ? GColorWhite : C_ACCENT);
    GRect name_rect = GRect(x_pad, bounds.size.h / 2 - 11, bounds.size.w - x_pad - 30, 22);
    graphics_draw_text(ctx, display, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       name_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    int dot_y = bounds.size.h / 2;
    int dot_x_start = bounds.size.w - x_pad - 14;
    graphics_context_set_fill_color(ctx, highlighted ? GColorWhite : C_DOT_OFF);
    for (int d = 0; d < 3; d++) {
      graphics_fill_circle(ctx, GPoint(dot_x_start + d * 6, dot_y), 2);
    }
    return;
  }

  if (idx->row == 1) {
    // ── 新建对话按钮：无底色 ──
    graphics_context_set_text_color(ctx, highlighted ? GColorWhite : C_ACCENT);
    GRect add_rect = GRect(0, bounds.size.h / 2 - 12, bounds.size.w, 24);
    graphics_draw_text(ctx, "+ START NEW CHAT", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       add_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }

  // ── 对话列表 ──
  int i = idx->row - 2;
  bool active = (i == s_active_chat_index);

  if (!highlighted && active) {
    graphics_context_set_fill_color(ctx, GColorCeleste);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  }

  int x_offset = PBL_IF_ROUND_ELSE(24, 8);
  if (active) {
    graphics_context_set_fill_color(ctx, highlighted ? GColorWhite : C_DOT_ON);
    graphics_fill_circle(ctx, GPoint(x_offset + 2, bounds.size.h / 2), 4);
  }

  const char *title = s_chat_entries[i].title;
  const char *sub = active ? "Active" : "Tap to switch";

  int text_x = active ? x_offset + 12 : x_offset;
  GRect title_rect = GRect(text_x, 2, bounds.size.w - text_x - 6, 20);
  GRect sub_rect = GRect(text_x, 20, bounds.size.w - text_x - 6, 20);

  graphics_context_set_text_color(ctx, highlighted ? GColorWhite : GColorBlack);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), title_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, highlighted ? GColorCeleste : (active ? C_ACCENT : C_SUBTITLE));
  graphics_draw_text(ctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_14), sub_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void delayed_pop_timer(void *data) {
  window_stack_pop(true);
}

static void menu_select_click(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (idx->row == 0) {
    // 点击模型行 → 打开模型选择子窗口
    if (s_model_count > 0) {
      show_model_select();
    } else {
      vibes_double_pulse();  // 没有可选模型
    }
    return;
  }

  if (idx->row == 1) {
    // 点击新建对话
    s_active_chat_index = -1;
    s_circle_target_x = s_width / 2; // 无标题时居中
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
      dict_write_cstring(iter, MESSAGE_KEY_SWITCH_CHAT, "");
      app_message_outbox_send();
    }

    if (s_state == STATE_RESPONSE) {
      s_reply_buf[0] = '\0';
      set_state(STATE_IDLE_READY);
    }
    app_timer_register(100, delayed_pop_timer, NULL);
    return;
  }

  // 点击历史对话
  int chat_idx = idx->row - 2;
  if (chat_idx >= s_chat_count) {
    app_timer_register(100, delayed_pop_timer, NULL);
    return;
  }

  if (s_state == STATE_SENDING || s_state == STATE_THINKING ||
      s_state == STATE_SHRINKING || s_state == STATE_EXPANDING) {
    vibes_double_pulse();
    return;
  }

  if (chat_idx == s_active_chat_index) {
    app_timer_register(100, delayed_pop_timer, NULL);
    return;
  }

  // 预先计算目标位置以消除动画抖动
  const char *target_title = s_chat_entries[chat_idx].title;
  GSize ts = graphics_text_layout_get_content_size(target_title, fonts_get_system_font(FONT_KEY_GOTHIC_14), GRect(0, 0, s_width, 20), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  s_circle_target_x = (s_width - (28 + 5 + ts.w)) / 2 + 14;

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_SWITCH_CHAT, s_chat_entries[chat_idx].id);
    app_message_outbox_send();
  }

  // 动态更新收缩动画的目标 X：使其与 canvas_draw 中动态计算的水平居中位置一致
  // Header total width = circle(28) + padding(5) + text_width
  const char *title = (s_active_chat_index >= 0 && s_active_chat_index < s_chat_count) ? s_chat_entries[s_active_chat_index].title : "New chat";
  GSize title_size = graphics_text_layout_get_content_size(title, fonts_get_system_font(FONT_KEY_GOTHIC_14), GRect(0, 0, s_width, 20), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  int header_w = 28 + 5 + title_size.w;
  s_circle_target_x = (s_width - header_w) / 2 + 14;

  s_reply_buf[0] = '\0';
  s_question_display[0] = '\0';
  text_layer_set_text(s_reply_layer, "");
  text_layer_set_text(s_question_display_layer, "");

  set_state(STATE_IDLE_READY);

  app_timer_register(100, delayed_pop_timer, NULL);
}

static void list_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  window_set_background_color(window, C_BG);

  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, s_menu_layer, (MenuLayerCallbacks){
    .get_num_rows = menu_get_num_rows,
    .get_cell_height = menu_get_cell_height,
    .draw_row = menu_draw_row,
    .select_click = menu_select_click,
  });
  menu_layer_set_normal_colors(s_menu_layer, C_BG, C_TEXT_LIGHT);
  menu_layer_set_highlight_colors(s_menu_layer, GColorCobaltBlue, GColorWhite);
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void list_window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  s_menu_layer = NULL;
}

static void show_chat_list(void) {
  if (!s_list_window) {
    s_list_window = window_create();
    window_set_window_handlers(s_list_window, (WindowHandlers){
      .load = list_window_load,
      .unload = list_window_unload,
    });
  }
  window_stack_push(s_list_window, true);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 模型选择子窗口 (MenuLayer)
// ═══════════════════════════════════════════════════════════════════════════════
static uint16_t model_menu_get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return s_model_count;
}

static void model_menu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *idx, void *data) {
  GRect bounds = layer_get_bounds(cell_layer);
  int i = idx->row;
  bool active = (i == s_current_model_index);

  int x_offset = PBL_IF_ROUND_ELSE(24, 8);

  // 活跃模型标记
  if (active) {
    graphics_context_set_fill_color(ctx, C_ACCENT);
    graphics_fill_circle(ctx, GPoint(x_offset, bounds.size.h / 2), 4);
  }

  const char *display = model_short_name(s_model_names[i]);
  const char *sub = active ? "Active" : "Tap to switch";

  int text_x = active ? x_offset + 10 : x_offset;
  GRect name_rect = GRect(text_x, 2, bounds.size.w - text_x - 4, 22);
  GRect sub_rect = GRect(text_x, 22, bounds.size.w - text_x - 4, 18);

  graphics_draw_text(ctx, display, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     name_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     sub_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void model_menu_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  int i = idx->row;
  if (i >= s_model_count) return;

  // 已经是当前模型，直接返回
  if (i == s_current_model_index) {
    window_stack_pop(true);
    return;
  }

  // 发送 SWITCH_MODEL 给手机
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_SWITCH_MODEL, s_model_names[i]);
    app_message_outbox_send();
  }

  s_current_model_index = i;
  strncpy(s_current_model_display, s_model_names[i], MODEL_NAME_LEN - 1);
  s_current_model_display[MODEL_NAME_LEN - 1] = '\0';

  vibes_short_pulse();

  // 刷新主菜单的模型行
  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
  }

  window_stack_pop(true);
}

static void model_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  window_set_background_color(window, C_BG);

  s_model_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_model_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = model_menu_get_num_rows,
    .draw_row = model_menu_draw_row,
    .select_click = model_menu_select,
  });
  menu_layer_set_normal_colors(s_model_menu_layer, C_BG, C_TEXT_LIGHT);
  menu_layer_set_highlight_colors(s_model_menu_layer, GColorCobaltBlue, GColorWhite);
  menu_layer_set_click_config_onto_window(s_model_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_model_menu_layer));
}

static void model_window_unload(Window *window) {
  menu_layer_destroy(s_model_menu_layer);
  s_model_menu_layer = NULL;
}

static void show_model_select(void) {
  if (!s_model_window) {
    s_model_window = window_create();
    window_set_window_handlers(s_model_window, (WindowHandlers){
      .load = model_window_load,
      .unload = model_window_unload,
    });
  }
  window_stack_push(s_model_window, true);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 解析函数
// ═══════════════════════════════════════════════════════════════════════════════

// 解析手机发来的对话列表 "id1|title1\nid2|title2\n..."
static void parse_chat_list(const char *data) {
  s_chat_count = 0;
  s_active_chat_index = -1;

  const char *p = data;
  while (*p && s_chat_count < MAX_WATCH_CHATS) {
    // 找 '|' 分隔 id 和 title
    const char *bar = p;
    while (*bar && *bar != '|') bar++;
    if (!*bar) break;

    // 找换行分隔条目
    const char *nl = bar + 1;
    while (*nl && *nl != '\n') nl++;

    int id_len = bar - p;
    int title_len = nl - bar - 1;
    if (id_len >= CHAT_ID_LEN) id_len = CHAT_ID_LEN - 1;
    if (title_len >= CHAT_TITLE_LEN) title_len = CHAT_TITLE_LEN - 1;

    memcpy(s_chat_entries[s_chat_count].id, p, id_len);
    s_chat_entries[s_chat_count].id[id_len] = '\0';

    memcpy(s_chat_entries[s_chat_count].title, bar + 1, title_len);
    s_chat_entries[s_chat_count].title[title_len] = '\0';

    s_chat_count++;
    p = *nl ? nl + 1 : nl;
  }
}

// 解析手机发来的模型列表 "idx|model1\nmodel2\nmodel3"
static void parse_model_list(const char *data) {
  s_model_count = 0;
  s_current_model_index = 0;

  // 先解析 idx
  const char *bar = data;
  while (*bar && *bar != '|') bar++;
  if (!*bar) return;

  // 解析当前索引
  char idx_buf[4];
  int idx_len = bar - data;
  if (idx_len >= (int)sizeof(idx_buf)) idx_len = sizeof(idx_buf) - 1;
  memcpy(idx_buf, data, idx_len);
  idx_buf[idx_len] = '\0';
  s_current_model_index = atoi(idx_buf);

  // 解析模型名列表
  const char *p = bar + 1;
  while (*p && s_model_count < MAX_MODELS) {
    const char *nl = p;
    while (*nl && *nl != '\n') nl++;

    int name_len = nl - p;
    if (name_len >= MODEL_NAME_LEN) name_len = MODEL_NAME_LEN - 1;
    if (name_len > 0) {
      memcpy(s_model_names[s_model_count], p, name_len);
      s_model_names[s_model_count][name_len] = '\0';
      s_model_count++;
    }

    p = *nl ? nl + 1 : nl;
  }

  // 更新当前模型显示名
  if (s_current_model_index >= 0 && s_current_model_index < s_model_count) {
    strncpy(s_current_model_display, s_model_names[s_current_model_index], MODEL_NAME_LEN - 1);
    s_current_model_display[MODEL_NAME_LEN - 1] = '\0';
  } else if (s_model_count > 0) {
    s_current_model_index = 0;
    strncpy(s_current_model_display, s_model_names[0], MODEL_NAME_LEN - 1);
    s_current_model_display[MODEL_NAME_LEN - 1] = '\0';
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// App 生命周期
// ═══════════════════════════════════════════════════════════════════════════════
static void init(void) {
  srand(time(NULL));  // 初始化随机数种子（用于彩蛋问题随机选择）

  // 注册 AppMessage 回调
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  // 使用最大缓冲区（Basalt: inbox 8200 / outbox 2048）
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());

  // 初始化模型显示名
  s_current_model_display[0] = '\0';

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
    .appear = window_appear,
  });
  window_stack_push(s_window, true);
}

// 清理所有资源（定时器、语音会话、子窗口、主窗口）
static void deinit(void) {
  stop_anim();
  if (s_thinking_timeout) app_timer_cancel(s_thinking_timeout);
  if (s_dictation_session) dictation_session_destroy(s_dictation_session);

  if (s_model_window) window_destroy(s_model_window);
  if (s_list_window) window_destroy(s_list_window);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
