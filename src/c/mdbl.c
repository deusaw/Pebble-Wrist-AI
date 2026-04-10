#include <pebble.h>

// ── 状态机 ───────────────────────────────────────────────────────────────────
typedef enum {
  STATE_IDLE_NO_KEY,
  STATE_IDLE_READY,
  STATE_RECORDING,
  STATE_SENDING,
  STATE_THINKING,
  STATE_SHRINKING,
  STATE_EXPANDING,    // 从回复状态回到大圆（新提问）
  STATE_RESPONSE
} AppState;

static AppState s_state = STATE_IDLE_NO_KEY;

// ── 配色（深蓝底 + 青蓝圆 + 亮青强调）────────────────────────────────────────
#define C_BG          GColorWhite
#define C_CIRCLE      GColorPictonBlue
#define C_CIRCLE_DIM  GColorLightGray
#define C_ACCENT      GColorBlue
#define C_ACCENT_DIM  GColorPictonBlue
#define C_TEXT_DARK   GColorWhite        // 圆上的文字
#define C_TEXT_LIGHT  GColorBlack        // 白底上的文字
#define C_SUBTITLE    GColorDarkGray
#define C_DOT_ON      GColorBlue
#define C_DOT_OFF     GColorLightGray

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

static int s_pulse_phase = 0;
static int s_spinner_angle = 0;  // THINKING 旋转角度

// 真正的网络流，不需要打字机本地延迟
static bool s_user_scrolled = false;  // 用户手动滚动过则停止自动滚动

static AppTimer *s_thinking_timeout = NULL;
#define THINKING_TIMEOUT_MS 90000  // 90秒超时


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
// 无需本地假流式效果，直接刷新文本框与滚动区域
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

// 呼吸光环
static void draw_breathe_ring(GContext *ctx, int cx, int cy, int r) {
  int wave = s_pulse_phase % 20;
  int offset = (wave <= 10) ? wave : 20 - wave;  // 0..10..0

  // 外环（淡青）
  graphics_context_set_stroke_color(ctx, C_ACCENT_DIM);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_circle(ctx, GPoint(cx, cy), r + 6 + offset / 2);

  // 内环（亮青）
  graphics_context_set_stroke_color(ctx, C_ACCENT);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_circle(ctx, GPoint(cx, cy), r + 3 + offset / 3);
}

static void draw_spinner(GContext *ctx, int cx, int cy, int r) {
  int dot_count = 12;
  int dot_r_big = 4;
  int dot_r_small = 2;
  int orbit = r + 12;

  // 根据当前旋转角度计算那两颗被“覆盖”放大的点
  int active_idx = (s_spinner_angle * dot_count / 360) % dot_count;

  for (int i = 0; i < dot_count; i++) {
    // 固定的 12 个等距位置
    int angle_deg = (i * 360 / dot_count);
    int32_t angle = (angle_deg * TRIG_MAX_ANGLE) / 360;
    int dx = (sin_lookup(angle) * orbit) / TRIG_MAX_RATIO;
    int dy = (-cos_lookup(angle) * orbit) / TRIG_MAX_RATIO;

    // 当大点转到当前小点位置时，该点变大并改变颜色
    bool head = (i == active_idx || i == (active_idx + dot_count - 1) % dot_count);
    
    graphics_context_set_fill_color(ctx, head ? C_ACCENT : C_DOT_OFF);
    graphics_fill_circle(ctx, GPoint(cx + dx, cy + dy),
                         head ? dot_r_big : dot_r_small);
  }
}

// 3 个跳动点
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
    graphics_context_set_fill_color(ctx, active ? C_DOT_ON : C_DOT_OFF);
    graphics_fill_circle(ctx, GPoint(x, y), active ? 6 : 4);
  }
}

// 上箭头（发送动画）
static void draw_arrow(GContext *ctx, int cx, int cy) {
  // 箭头在圆内上下浮动
  int lift = (s_anim_frame % 6) * 2;
  int ay = cy - lift;

  graphics_context_set_stroke_color(ctx, C_ACCENT);
  graphics_context_set_stroke_width(ctx, 3);
  graphics_draw_line(ctx, GPoint(cx, ay + 12), GPoint(cx, ay - 8));
  graphics_draw_line(ctx, GPoint(cx, ay - 8), GPoint(cx - 7, ay));
  graphics_draw_line(ctx, GPoint(cx, ay - 8), GPoint(cx + 7, ay));
}

// 居中文字（自动适配圆形/矩形边距）
static void draw_text(GContext *ctx, const char *text,
                       GFont font, int cy, GColor color) {
  graphics_context_set_text_color(ctx, color);
  // 圆形屏：左右各内缩 20px 以避开圆弧边缘导致的文字被截断
  int x_pad = PBL_IF_ROUND_ELSE(20, 0);
  GRect box = GRect(x_pad, cy - 14, s_width - x_pad * 2, 30);
  graphics_draw_text(ctx, text, font, box,
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 主绘制
// ═══════════════════════════════════════════════════════════════════════════════
static void canvas_draw(Layer *layer, GContext *ctx) {
  // 黑色背景
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

  int cx = s_width / 2;
  GFont font_title = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont font_sub   = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont font_small = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  switch (s_state) {
    case STATE_IDLE_NO_KEY:
      draw_breathe_ring(ctx, cx, s_circle_y_big, s_circle_r_big);
      graphics_context_set_fill_color(ctx, C_CIRCLE_DIM);
      graphics_fill_circle(ctx, GPoint(cx, s_circle_y_big), s_circle_r_big);
      draw_text(ctx, "PWAI", font_title, s_circle_y_big, C_SUBTITLE);
      draw_text(ctx, "Set API in Pebble app", font_small, s_subtitle_y, C_SUBTITLE);
      break;

    case STATE_IDLE_READY:
      draw_breathe_ring(ctx, cx, s_circle_y_big, s_circle_r_big);
      graphics_context_set_fill_color(ctx, C_CIRCLE);
      graphics_fill_circle(ctx, GPoint(cx, s_circle_y_big), s_circle_r_big);
      draw_text(ctx, "PWAI", font_title, s_circle_y_big, C_TEXT_DARK);
      {
        const char *title = (s_active_chat_index >= 0 && s_active_chat_index < s_chat_count) ? s_chat_entries[s_active_chat_index].title : "New chat";
        draw_text(ctx, title, font_sub, s_subtitle_y, C_ACCENT);
      }
      break;

    case STATE_RECORDING:
      graphics_context_set_fill_color(ctx, C_CIRCLE);
      graphics_fill_circle(ctx, GPoint(cx, s_circle_y_big), s_circle_r_big);
      draw_dots(ctx, cx, s_circle_y_big);
      draw_text(ctx, "Listening...", font_small, s_subtitle_y, C_ACCENT);
      break;

    case STATE_SENDING:
      graphics_context_set_fill_color(ctx, C_CIRCLE);
      graphics_fill_circle(ctx, GPoint(cx, s_circle_y_big), s_circle_r_big);
      draw_arrow(ctx, cx, s_circle_y_big);
      draw_text(ctx, "Sending...", font_small, s_subtitle_y, C_SUBTITLE);
      break;

    case STATE_THINKING: {
      int wave = s_pulse_phase % 24;
      int offset = (wave <= 12) ? wave : 24 - wave;  // 0..12..0
      int breathe_r = s_circle_r_big - 4 + (offset * 4 / 12); // ±4px
      graphics_context_set_fill_color(ctx, C_CIRCLE);
      graphics_fill_circle(ctx, GPoint(cx, s_circle_y_big), breathe_r);
      draw_text(ctx, "PWAI", font_title, s_circle_y_big, C_TEXT_DARK);
      draw_spinner(ctx, cx, s_circle_y_big, s_circle_r_big - 4);
      draw_text(ctx, "Thinking...", font_small, s_subtitle_y, C_ACCENT);
      break;
    }

    case STATE_SHRINKING:
    case STATE_EXPANDING:
      graphics_context_set_fill_color(ctx, C_CIRCLE);
      graphics_fill_circle(ctx, GPoint(s_circle_x, s_circle_y), s_circle_r);
      if (s_circle_r > 20) {
        draw_text(ctx, "PWAI", font_title, s_circle_y, C_TEXT_DARK);
      } else {
        GRect p_box = GRect(s_circle_x - 9, s_circle_y - 12 + (CIRCLE_Y_SMALL - s_circle_y), 20, 20);
        graphics_context_set_text_color(ctx, C_TEXT_DARK);
        graphics_draw_text(ctx, "P", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                           p_box, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
      }
      break;

    case STATE_RESPONSE: {
      const char *title = (s_active_chat_index >= 0 && s_active_chat_index < s_chat_count) ? s_chat_entries[s_active_chat_index].title : "New chat";
      GSize title_size = graphics_text_layout_get_content_size(title, fonts_get_system_font(FONT_KEY_GOTHIC_14), GRect(0, 0, s_width, 20), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
      int total_w = 28 + 5 + title_size.w; 
      int start_x = (s_width - total_w) / 2;
      int current_circle_x = start_x + 14;
      graphics_context_set_fill_color(ctx, C_CIRCLE);
      graphics_fill_circle(ctx, GPoint(current_circle_x, CIRCLE_Y_SMALL), CIRCLE_R_SMALL);
      graphics_context_set_text_color(ctx, C_TEXT_DARK);
      GRect p_box = GRect(current_circle_x - 9, CIRCLE_Y_SMALL - 12, 20, 20);
      graphics_draw_text(ctx, "P", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), p_box, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
      graphics_context_set_text_color(ctx, C_SUBTITLE);
      GRect title_box = GRect(current_circle_x + 14 + 5, CIRCLE_Y_SMALL - 7, title_size.w + 10, 20);
      graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_14), title_box, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
      graphics_context_set_fill_color(ctx, C_ACCENT_DIM);
      graphics_fill_rect(ctx, GRect(s_width / 4, CIRCLE_Y_SMALL + CIRCLE_R_SMALL + 6, s_width / 2, 1), 0, GCornerNone);
      break;
    }
    default:
      break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// 动画
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

  if (s_state == STATE_THINKING) {
    s_spinner_angle = (s_spinner_angle + 15) % 360;
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
    // 5秒后停止动画省电（50帧 × 100ms）
    if (s_anim_frame > 50) {
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
// AppMessage
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
// 按钮
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
    // 保护：IDLE_READY 下必须有内容才能"清空"；RESPONSE 状态一定有内容
    bool has_content = (s_reply_buf[0] != '\0' || s_question_display[0] != '\0');
    if (!has_content && s_state == STATE_IDLE_READY) {
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

static void down_handler(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_RESPONSE) {
    GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
    offset.y -= 60;
    scroll_layer_set_content_offset(s_scroll_layer, offset, true);
  } else {
    show_chat_list();
  }
}

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

  // 禁用 ScrollLayer 上下阴影/箭头指示器
  // Pebble SDK 文档：禁用某个方向，直接传 NULL 作为 config 参数（不是在 config 里设 layer=NULL）
  ContentIndicator *indicator = scroll_layer_get_content_indicator(s_scroll_layer);
  content_indicator_configure_direction(indicator, ContentIndicatorDirectionUp, NULL);
  content_indicator_configure_direction(indicator, ContentIndicatorDirectionDown, NULL);

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
// 对话列表窗口 (MenuLayer) — row 0: 模型, row 1: 新对话, row 2+: 对话列表
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
  graphics_draw_text(ctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), sub_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
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
// App
// ═══════════════════════════════════════════════════════════════════════════════
static void init(void) {
  srand(time(NULL));
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
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
