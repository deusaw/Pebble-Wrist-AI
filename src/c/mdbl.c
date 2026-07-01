// ═══════════════════════════════════════════════════════════════════════════════
// Wrist AI — 手表端 C 代码
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
  STATE_IDLE_READY,    // 已就绪，显示 Wi logo + 当前对话标题
  STATE_RECORDING,     // 语音输入中（3点跳动动画）
  STATE_REVIEW,        // 显示识别结果：SELECT 发送，DOWN 重录，BACK 取消
  STATE_SENDING,       // 正在发送问题到手机（箭头动画）
  STATE_THINKING,      // 等待 AI 回复（呼吸圆 + 旋转小点）
  STATE_SHRINKING,     // 大圆缩小动画（过渡到回复显示）
  STATE_EXPANDING,     // 小圆放大动画（从回复回到大圆继续追问）
  STATE_RESPONSE       // 显示 AI 回复文本（可滚动）
} AppState;

static AppState s_state = STATE_IDLE_NO_KEY;

// ── 配色方案 ──────────────────────────────────────────────────────────────────
#define C_TEXT_LIGHT  GColorBlack       // 白底上的文字
#define C_ACCENT      GColorBlue        // 强调色（菜单蓝字）
#define C_SHADOW      GColorDarkGray    // logo 阴影色

// 菜单窗口仍使用白底
#define C_BG          GColorWhite
#define C_DOT_ON      GColorBlue
#define C_DOT_OFF     GColorLightGray
// Color watches reuse the random Home theme. Monochrome watches use black so
// the forced white selection text always keeps maximum contrast.
#define C_MENU_HIGHLIGHT PBL_IF_COLOR_ELSE(s_bg_color, GColorBlack)
#define THEME_HEADER_HEIGHT 26

// ── 背景 ─────────────────────────────────────────────────────────────────────
// 每次启动随机纯色底 + 5 层色彩条纹（回复页/菜单页顶部装饰用）
#define WAVE_LAYERS 5
static GColor s_wave_colors[WAVE_LAYERS];
static GColor s_bg_color;

// 主状态页背景：只使用能与白色 Wi Logo 保持清晰对比的饱和颜色。
// 保留原本最搭配 Logo 的橙色和紫色；蓝色/红色选用更深的色阶。
// 注意：回答页和菜单页不使用这组背景色，保持原有白底设计。
static const GColor s_fill_palette[] = {
  { .argb = GColorSunsetOrangeARGB8 },
  { .argb = GColorVividVioletARGB8 },
  { .argb = GColorLavenderIndigoARGB8 },
  { .argb = GColorCobaltBlueARGB8 },
  { .argb = GColorOxfordBlueARGB8 },
  { .argb = GColorDarkCandyAppleRedARGB8 },
  { .argb = GColorDarkGrayARGB8 },
};
#define FILL_PALETTE_SIZE 7
static int s_theme_color = -1;  // -1=random, otherwise s_fill_palette index

// 回答页顶部彩条继续使用原来的完整柔和彩色调色板。
static const GColor s_stripe_palette[] = {
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
#define STRIPE_PALETTE_SIZE 12

// ── 布局常量 ─────────────────────────────────────────────────────────────────
#define CIRCLE_R_SMALL  14
#define CIRCLE_Y_SMALL  PBL_IF_ROUND_ELSE(28, 20)

// Emery / Flint 扬声器平台在回复标题下显示 TTS 操作提示。
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  #define TTS_HINT_OFFSET   20   // 提示下方额外留 6px，避免与正文粘连
#else
  #define TTS_HINT_OFFSET   0
#endif

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

static int s_arc_angle = 0;          // thinking 旋转弧线角度
static int s_logo_morph = 0;         // Wi→圆 变形进度 (0=Wi logo, 16=完整圆环)
#define LOGO_MORPH_TOTAL 16
static int s_heartbeat = 0;          // 脉冲偏移 (±px)

// 偶发脉冲动效（代替持续心跳）
static AppTimer *s_pulse_timer = NULL;
static int s_pulse_frame = 0;
#define PULSE_INTERVAL_MS 4000       // 静止 4 秒后触发一次脉冲
#define PULSE_FIRST_MS    1500       // 首次脉冲更快（1.5 秒后）

static bool s_user_scrolled = false;  // 用户手动滚动过则停止自动滚动到顶部

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
static AppTimer *s_auto_dictation_timer = NULL;
static bool s_auto_dictation_attempted = false;
static bool s_auto_dictation_synced = false;
static AppTimer *s_dictation_retry_timer = NULL;
static bool s_dictation_interrupt_retry_used = false;
static char *s_pending_question = NULL;
static AppLaunchReason s_launch_reason = APP_LAUNCH_USER;
static uint32_t s_launch_args = 0;
static bool s_force_launch_dictation = false;
// True only when Wrist AI was entered via Quick Launch or a Timeline action —
// i.e. a deliberate one-shot entry, not a plain open from the app menu. The
// Auto Dictation setting is honoured only for such entries, so opening the app
// normally from the launcher does not auto-start recording.
static bool s_quick_launch_entry = false;
static char s_launch_context[48];

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

#define MAX_WATCH_NOTES 20
#define NOTE_ID_LEN 24
#define NOTE_TITLE_LEN 36
#define NOTE_DUE_LEN 17
typedef struct {
  char id[NOTE_ID_LEN];
  char title[NOTE_TITLE_LEN];
  char due[NOTE_DUE_LEN];
  bool done;
  bool todo;
  bool event;
} WatchNoteEntry;
static WatchNoteEntry *s_note_entries;
static int s_note_count;
static int s_selected_note_index = -1;
static bool s_notes_list_dirty;
static bool s_note_actions_visible;
static Window *s_notes_window;
static MenuLayer *s_notes_menu_layer;
static Layer *s_notes_bar_layer;
static Window *s_note_actions_window;
static MenuLayer *s_note_actions_menu_layer;
static Layer *s_note_actions_bar_layer;

#define MAX_NOTE_WAKEUPS 7
#define PERSIST_NOTE_WAKEUP_BASE 100
#define PERSIST_SYNC_WAKEUP_ID 108
#define SYNC_WAKEUP_COOKIE 0x5753594E
typedef struct {
  int32_t code;
  WakeupId wakeup_id;
  char note_id[NOTE_ID_LEN];
  char title[NOTE_TITLE_LEN];
} NoteWakeupSlot;
static NoteWakeupSlot *s_note_wakeups;
static char s_wakeup_note_title[NOTE_TITLE_LEN];
static bool s_wakeup_note_active;
static WakeupId s_sync_wakeup_id = -1;
static bool s_sync_wakeup_launch;
static char s_sync_status[24];
static bool s_startup_message_guard = true;
static AppTimer *s_sync_wakeup_timer;
static long s_pending_sync_wakeup_timestamp;

// TTS 音量选择窗口（Emery / Flint）
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
static Window    *s_volume_window;
static MenuLayer *s_volume_menu_layer;
#endif

// ── 持久存储 key ─────────────────────────────────────────────────────────────
#define PERSIST_KEY_READY            1
#define PERSIST_KEY_FONT_SIZE        2  // 字号: 0=Normal, 1=Large, 2=Extra Large
#define PERSIST_KEY_FONT_BOLD        3  // 加粗: 0=否, 1=是
#define PERSIST_KEY_DISABLE_SURPRISE 4  // 禁用彩蛋: 0=启用, 1=禁用
#define PERSIST_KEY_TTS_VOLUME       5  // TTS 音量: 10-100
#define PERSIST_KEY_HEALTH_ENABLED   6  // 健康数据开关: 0=关闭, 1=开启
#define PERSIST_KEY_WEB_SEARCH       8  // 联网搜索: 0=关闭, 1=开启
#define PERSIST_KEY_AUTO_DICTATION  10  // 启动时自动录音: 0=关闭, 1=开启
#define PERSIST_KEY_HEALTH_DAYS     11  // 健康历史范围: 1/3/7 天
#define PERSIST_KEY_HEALTH_SLEEP    12  // 睡眠数据: 0=关闭, 1=开启
#define PERSIST_KEY_DUO_TTS         13  // Flint TTS 实验开关，默认关闭
#define PERSIST_KEY_THEME_COLOR     14  // -1=random, 0..6=fixed Home/menu theme
#define PERSIST_KEY_NOTE_COMMAND    15  // 待同步的幂等 Note 操作
// 注意：key 7 (曾为 AUTO_TTS) 和 key 9 (曾为 HAS_TTS_KEY) 已废弃，不复用以免旧值干扰。

// ── 字体设置 ──────────────────────────────────────────────────────────────────
static int s_font_size = 0;       // 0=Normal, 1=Large, 2=Extra Large
static int s_font_bold = 0;       // 0=不加粗, 1=加粗
static int s_disable_surprise = 0; // 0=启用彩蛋, 1=禁用彩蛋
static int s_health_enabled = 0;  // 0=不发健康数据, 1=发健康数据
static int s_web_search = 1;      // 0=不联网搜索, 1=联网搜索（默认开，仅 OpenRouter 生效，由 JS 端判断）
static int s_auto_dictation = 0;  // 0=关闭, 1=每次 app launch 自动启动一次录音
static int s_health_days = 7;
static int s_health_sleep = 1;
static int s_duo_tts_enabled = 0;
static AppTimer *s_note_command_timer;
static char s_pending_note_command[NOTE_ID_LEN + 16];

// ── 健康数据（随 QUESTION 一起发送）───────────────────────────────
static int32_t s_step_count = 0;
static int32_t s_active_minutes = 0;
#define HEALTH_DATA_SIZE 512
#define HEALTH_INTERVAL_SIZE 384
static char *s_health_data;

#if defined(PBL_HEALTH)
typedef struct {
  char *buffer;
  size_t buffer_size;
  int count;
} SleepIntervalContext;

static bool sleep_interval_callback(HealthActivity activity, time_t time_start,
                                    time_t time_end, void *context) {
  SleepIntervalContext *ctx = context;
  if (!ctx || ctx->count >= 24 || time_end <= time_start) return false;

  struct tm start_tm = *localtime(&time_start);
  struct tm end_tm = *localtime(&time_end);
  char start_text[17], end_text[17];
  strftime(start_text, sizeof(start_text), "%Y-%m-%dT%H:%M", &start_tm);
  strftime(end_text, sizeof(end_text), "%Y-%m-%dT%H:%M", &end_tm);

  size_t used = strlen(ctx->buffer);
  if (used >= ctx->buffer_size - 1) return false;
  int written = snprintf(ctx->buffer + used, ctx->buffer_size - used,
                         "%c|%s|%s|%ld\n",
                         activity == HealthActivityRestfulSleep ? 'D' : 'S',
                         start_text, end_text,
                         (long)((time_end - time_start + 30) / 60));
  if (written < 0 || (size_t)written >= ctx->buffer_size - used) {
    ctx->buffer[used] = '\0';
    return false;
  }
  ctx->count++;
  return true;
}
#endif

// ── TTS 语音播放（Emery / Flint 扬声器平台）────────────────────────
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
#define TTS_DECODE_BYTES     1600   // 每次最多写入 200ms 音频，让 speaker FIFO 吸收 AppTimer 抖动
#define TTS_MIN_WRITE_BYTES  800    // 非流尾至少写 100ms，避免过碎写入造成断续
#define TTS_PLAYBACK_MS      100    // speaker pump 间隔；JS 发送速率需高于 pump 速率，避免抽干 ring
#if defined(PBL_PLATFORM_FLINT)
  // Pebble 2 Duo / Flint 的 app runtime RAM 上限约 30KB，不能使用 Emery 36KB ring。
  #define TTS_RING_SIZE         12288
  #define TTS_START_THRESHOLD    7500
  #define TTS_RESTART_THRESHOLD  6000
  // 留出至少 3.7KB 给已进入 AppMessage 管道的音频，避免 PAUSE 往返期间溢出。
  #define TTS_PAUSE_HIGH         8500
  #define TTS_RESUME_LOW         6500
#else
  // v1.4.2 transcript review adds code to the same 16-bit virtual-size image.
  // Use a 35KB ring and pause earlier, retaining >5KB for in-flight chunks.
  #define TTS_RING_SIZE         35840
  #define TTS_START_THRESHOLD   24000
  #define TTS_RESTART_THRESHOLD  8000
  #define TTS_PAUSE_HIGH        30500
  #define TTS_RESUME_LOW        24500
#endif
#define TTS_CLOSE_DELAY_MS   1500   // 句间延迟关闭扬声器
#define TTS_WATCHDOG_MS      60000  // TTS 看门狗：全量预编码可能等待较久，60s 无 chunk/done 才清理
#define TTS_FLOW_RETRY_MS    120    // PAUSE/RESUME outbox 忙/失败时快速重试，避免 JS 卡在暂停态
// 流控水位线（watch→JS 暂停/恢复）。
// raw PCM 8000 B/s 消费；JS 正常约 8.14KB/s 恒速投递，PAUSE 后约 7KB/s 轻降速。
// 正常播放不再依赖 PAUSE/RESUME 周期，反馈流控只作为接近满缓冲时的保险。
// LOW=26000（3.25s 跑道），HIGH=32000（距 ring 满约 4.75KB），防蓝牙抖动时见底或溢出。
#define TTS_VOLUME_MIN       10
#define TTS_VOLUME_MAX       100
#define TTS_VOLUME_STEP      10
#define TTS_SPEAKER_VOLUME_MAX 60  // 菜单 100% 映射为硬件 60%，防止高音量炸音

// Keep the large PCM ring on the heap. On Emery a static 35KB ring counts
// against the 16-bit virtual image limit even though the same RAM is available
// at runtime.
static uint8_t *s_tts_ring;
static uint32_t s_tts_head = 0;
static uint32_t s_tts_tail = 0;
static uint32_t s_tts_count = 0;
static int8_t *s_tts_pending_write_buf;
static int8_t *s_tts_out_buf;
static uint16_t s_tts_pending_write_len = 0;
static uint16_t s_tts_pending_write_offset = 0;
static bool s_tts_playing = false;       // 是否正在播放（含缓冲中）
static bool s_tts_loading = false;       // TTS 已请求、等待首个音频块期间
static bool s_tts_all_sent = false;      // JS 已发完所有数据（TTS_DONE 到达），等缓冲排空后置 playing=false
static bool s_tts_started_playback = false; // 本轮 TTS 是否已经首次开播；用于区分首次阈值和中途重启阈值
static bool s_tts_paused = false;        // 已向 JS 发出 TTS_PAUSE、等待缓冲排空到 LOW 再 RESUME
static bool s_tts_overflow_logged = false;
static bool s_speaker_open = false;
static int s_tts_volume = 100;           // 10-100
static AppTimer *s_tts_playback_timer = NULL;
static AppTimer *s_tts_close_timer = NULL;
static AppTimer *s_tts_watchdog_timer = NULL;
static AppTimer *s_tts_flow_retry_timer = NULL;
static bool s_tts_flow_retry_pause = false;
#endif

#define R_BUF_SIZE 1850
static char s_reply_buf[R_BUF_SIZE];

#define Q_DISPLAY_SIZE 128
static char s_question_display[Q_DISPLAY_SIZE];  // 回复区顶部显示的问题

#define DICTATION_BUF_SIZE 1024  // 转录 buffer（~10-15句话）

// ── 屏幕尺寸 ─────────────────────────────────────────────────────────────────
static int s_width, s_height;
static int s_response_content_h = 0;

// ── 前向声明 ─────────────────────────────────────────────────────────────────
static void set_state(AppState new_state);
static void anim_tick(void *data);
static void pulse_tick(void *data);
static void schedule_pulse(void);
static bool start_dictation_if_possible(void);
static void maybe_schedule_auto_dictation(void);
static void send_pending_question(void);
static void dictation_callback(DictationSession *session,
                                DictationSessionStatus status,
                                char *transcription,
                                void *context);
static void parse_chat_list(const char *data);
static void parse_model_list(const char *data);
static void show_chat_list(void);
static void show_model_select(void);
static void show_notes(void);
static void parse_note_list(const char *data);
static void schedule_note_wakeup(const char *data);
static void send_queued_note_command(void *data);
static void schedule_sync_wakeup(const char *data);
static void apply_sync_wakeup(void *data);
static void clear_startup_message_guard(void *data);
static void delayed_pop_timer(void *data);
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
static void show_volume_select(void);
#endif

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
    int scroll_top = THEME_HEADER_HEIGHT + 4 + TTS_HINT_OFFSET;
    int scroll_h = s_height - scroll_top;
    if (content_h < scroll_h) content_h = scroll_h;
    s_response_content_h = content_h;
    scroll_layer_set_content_size(s_scroll_layer, GSize(s_width, content_h));

    // 自动滚动到顶部，从头开始阅读（除非用户手动滚过）
    if (!s_user_scrolled) {
      scroll_layer_set_content_offset(s_scroll_layer, GPointZero, false);
    }
  }
}

static size_t utf8_safe_prefix_bytes(const char *text, size_t max_bytes) {
  size_t i = 0;
  while (text[i] != '\0' && i < max_bytes) {
    const uint8_t lead = (uint8_t)text[i];
    size_t char_bytes = 1;
    if ((lead & 0x80) == 0) char_bytes = 1;
    else if ((lead & 0xE0) == 0xC0) char_bytes = 2;
    else if ((lead & 0xF0) == 0xE0) char_bytes = 3;
    else if ((lead & 0xF8) == 0xF0) char_bytes = 4;
    if (i + char_bytes > max_bytes) break;
    i += char_bytes;
  }
  return i;
}

static void utf8_safe_append(char *dest, size_t dest_size, const char *source) {
  size_t used = strlen(dest);
  if (used >= dest_size - 1) return;
  size_t copy_bytes = utf8_safe_prefix_bytes(source, dest_size - used - 1);
  memcpy(dest + used, source, copy_bytes);
  dest[used + copy_bytes] = '\0';
}

static void utf8_safe_copy(char *dest, size_t dest_size, const char *source) {
  if (dest_size == 0) return;
  dest[0] = '\0';
  utf8_safe_append(dest, dest_size, source);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 健康数据读取
//
// 读取今日步数和活跃分钟，随 QUESTION 一起发给手机端注入 System Prompt。
// PBL_HEALTH 保护：无健康 API 的平台（如 Aplite）跳过，发 -1 哨兵（JS 端不注入）。
// 有健康 API 但数据不可用（HEALTH_VALUE_UNAVAILABLE）也发 -1；合法 0 正常发。
// ═══════════════════════════════════════════════════════════════════════════════
static void read_health_data(void) {
#if defined(PBL_HEALTH)
  if (s_health_data) free(s_health_data);
  s_health_data = malloc(HEALTH_DATA_SIZE);
  if (!s_health_data) {
    s_step_count = -1;
    s_active_minutes = -1;
    return;
  }
  s_health_data[0] = '\0';
  time_t now = time(NULL);
  struct tm today_tm = *localtime(&now);
  today_tm.tm_hour = 0; today_tm.tm_min = 0; today_tm.tm_sec = 0;

  for (int offset = s_health_days - 1; offset >= 0; offset--) {
    struct tm day_tm = today_tm;
    day_tm.tm_mday -= offset;
    time_t day_start = mktime(&day_tm);
    struct tm next_tm = day_tm;
    next_tm.tm_mday += 1;
    time_t day_end = mktime(&next_tm);
    if (day_end > now) day_end = now;

#define HEALTH_SUM(metric) \
    ((health_service_metric_accessible((metric), day_start, day_end) & \
      HealthServiceAccessibilityMaskAvailable) ? \
      health_service_sum((metric), day_start, day_end) : -1)
    HealthValue steps = HEALTH_SUM(HealthMetricStepCount);
    HealthValue active_sec = HEALTH_SUM(HealthMetricActiveSeconds);
    HealthValue distance = HEALTH_SUM(HealthMetricWalkedDistanceMeters);
    HealthValue active_kcal = HEALTH_SUM(HealthMetricActiveKCalories);
    HealthValue resting_kcal = HEALTH_SUM(HealthMetricRestingKCalories);
#undef HEALTH_SUM
    HealthValue heart_avg = -1;
    if (health_service_metric_accessible(
          HealthMetricHeartRateBPM, day_start, day_end) &
        HealthServiceAccessibilityMaskAvailable) {
      heart_avg = health_service_aggregate_averaged(
          HealthMetricHeartRateBPM, day_start, day_end,
          HealthAggregationAvg, HealthServiceTimeScopeOnce);
    }

    HealthValue sleep_sec = -1, restful_sec = -1;
    if (s_health_sleep) {
      struct tm sleep_start_tm = day_tm;
      sleep_start_tm.tm_mday -= 1;
      sleep_start_tm.tm_hour = 12;
      time_t sleep_start = mktime(&sleep_start_tm);
      struct tm sleep_end_tm = day_tm;
      sleep_end_tm.tm_hour = 12;
      time_t sleep_end = mktime(&sleep_end_tm);
      if (sleep_end > now) sleep_end = now;
      if (health_service_metric_accessible(
            HealthMetricSleepSeconds, sleep_start, sleep_end) &
          HealthServiceAccessibilityMaskAvailable)
        sleep_sec = health_service_sum(HealthMetricSleepSeconds, sleep_start, sleep_end);
      if (health_service_metric_accessible(
            HealthMetricSleepRestfulSeconds, sleep_start, sleep_end) &
          HealthServiceAccessibilityMaskAvailable)
        restful_sec = health_service_sum(HealthMetricSleepRestfulSeconds, sleep_start, sleep_end);
    }

    struct tm label_tm = *localtime(&day_start);
    size_t used = strlen(s_health_data);
    if (used < HEALTH_DATA_SIZE - 1) {
      snprintf(s_health_data + used, HEALTH_DATA_SIZE - used,
               "%04d-%02d-%02d|%ld|%ld|%ld|%ld|%ld|%ld|%ld|%ld\n",
               label_tm.tm_year + 1900, label_tm.tm_mon + 1, label_tm.tm_mday,
               (long)steps, active_sec < 0 ? -1L : (long)((active_sec + 30) / 60),
               (long)distance, sleep_sec < 0 ? -1L : (long)((sleep_sec + 30) / 60),
               restful_sec < 0 ? -1L : (long)((restful_sec + 30) / 60),
               (long)active_kcal, (long)resting_kcal, (long)heart_avg);
    }
    if (offset == 0) {
      s_step_count = steps < 0 ? -1 : (int32_t)steps;
      s_active_minutes = active_sec < 0 ? -1 : (int32_t)((active_sec + 30) / 60);
    }
  }

#else
  s_step_count = -1;   // 无健康 API 的平台：哨兵值，JS 端不注入
  s_active_minutes = -1;
  if (s_health_data) s_health_data[0] = '\0';
#endif
}

static void write_sleep_intervals(DictionaryIterator *iter) {
#if defined(PBL_HEALTH)
  if (!s_health_data) return;
  if (!s_health_sleep || !iter) {
    free(s_health_data);
    s_health_data = NULL;
    return;
  }
  // Reuse the aggregate-health buffer after it has already been copied into
  // the AppMessage dictionary. This avoids another large static allocation on Emery.
  s_health_data[0] = '\0';
  time_t now = time(NULL);
  SleepIntervalContext interval_ctx = {
    .buffer = s_health_data,
    .buffer_size = HEALTH_INTERVAL_SIZE,
    .count = 0
  };
  health_service_activities_iterate(
      HealthActivitySleep | HealthActivityRestfulSleep,
      now - (time_t)s_health_days * 24 * 60 * 60, now,
      // Latest-first guarantees the most recent complete sleep survives if
      // many old deep-sleep segments fill the compact AppMessage payload.
      HealthIterationDirectionPast, sleep_interval_callback, &interval_ctx);
  if (s_health_data[0] != '\0')
    dict_write_cstring(iter, MESSAGE_KEY_HEALTH_SLEEP_INTERVALS, s_health_data);
  free(s_health_data);
  s_health_data = NULL;
#else
  (void)iter;
#endif
}

static void note_wakeup_alert(int32_t code) {
  static const uint32_t segments[] = {
    500, 180, 500, 180, 500, 180, 500, 180, 500
  };
  vibes_enqueue_custom_pattern((VibePattern){
    .durations = segments,
    .num_segments = ARRAY_LENGTH(segments),
  });
  if (s_note_wakeups) {
    for (int i = 0; i < MAX_NOTE_WAKEUPS; i++) {
      if (s_note_wakeups[i].code == code) {
        strncpy(s_wakeup_note_title, s_note_wakeups[i].title,
                sizeof(s_wakeup_note_title) - 1);
        s_wakeup_note_title[sizeof(s_wakeup_note_title) - 1] = '\0';
        s_wakeup_note_active = true;
        s_note_wakeups[i].wakeup_id = -1;
        persist_delete(PERSIST_NOTE_WAKEUP_BASE + i);
        break;
      }
    }
  }
  if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
}

static void note_wakeup_handler(WakeupId wakeup_id, int32_t cookie) {
  if (cookie == SYNC_WAKEUP_COOKIE) {
    s_sync_wakeup_id = -1;
    persist_delete(PERSIST_SYNC_WAKEUP_ID);
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
      dict_write_uint8(iter, MESSAGE_KEY_SYNC_WAKEUP, 1);
      app_message_outbox_send();
    }
    return;
  }
  note_wakeup_alert(cookie);
}

static void apply_sync_wakeup(void *data) {
  (void)data;
  s_sync_wakeup_timer = NULL;
  long timestamp = s_pending_sync_wakeup_timestamp;
  s_pending_sync_wakeup_timestamp = 0;

  if (s_sync_wakeup_id >= 0) {
    time_t old_time;
    if (wakeup_query(s_sync_wakeup_id, &old_time)) {
      // A repeated sync commonly asks for effectively the same wakeup. Keep
      // the existing OS object instead of cancelling and recreating it.
      long delta = (long)old_time - timestamp;
      if (timestamp > (long)time(NULL) + 30 &&
          delta > -90 && delta < 90) return;
      wakeup_cancel(s_sync_wakeup_id);
    }
  }
  s_sync_wakeup_id = -1;
  persist_delete(PERSIST_SYNC_WAKEUP_ID);
  if (timestamp <= (long)time(NULL) + 30) return;

  WakeupId id = -1;
  // A strong Note reminder may occupy the same one-minute Wakeup window.
  // Move background sync by two minutes rather than silently losing it.
  for (int attempt = 0; attempt < 5; attempt++) {
    id = wakeup_schedule((time_t)(timestamp + attempt * 120),
                         SYNC_WAKEUP_COOKIE, false);
    if (id >= 0 || id != E_RANGE) break;
  }
  if (id >= 0) {
    s_sync_wakeup_id = id;
    persist_write_int(PERSIST_SYNC_WAKEUP_ID, (int)id);
  }
}

static void schedule_sync_wakeup(const char *data) {
  if (!data) return;
  s_pending_sync_wakeup_timestamp = strtol(data, NULL, 10);
  if (s_sync_wakeup_timer) app_timer_cancel(s_sync_wakeup_timer);
  // Do not call Wakeup scheduling APIs from inside AppMessage's inbox
  // callback. On real watches this path could terminate the app immediately
  // after Todoist reported "Synced", even with an empty item list.
  s_sync_wakeup_timer = app_timer_register(750, apply_sync_wakeup, NULL);
}

static void clear_startup_message_guard(void *data) {
  (void)data;
  s_startup_message_guard = false;
}

static void schedule_note_wakeup(const char *data) {
  if (!data || !s_note_wakeups) return;
  if (strncmp(data, "cancel|", 7) == 0) {
    int32_t cancel_code = (int32_t)strtol(data + 7, NULL, 10);
    for (int i = 0; i < MAX_NOTE_WAKEUPS; i++) {
      if (s_note_wakeups[i].code == cancel_code) {
        if (s_note_wakeups[i].wakeup_id >= 0)
          wakeup_cancel(s_note_wakeups[i].wakeup_id);
        memset(&s_note_wakeups[i], 0, sizeof(NoteWakeupSlot));
        persist_delete(PERSIST_NOTE_WAKEUP_BASE + i);
        break;
      }
    }
    return;
  }
  char copy[NOTE_ID_LEN + NOTE_TITLE_LEN + 40];
  strncpy(copy, data, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = '\0';
  char *p1 = strchr(copy, '|');
  if (!p1) return;
  *p1++ = '\0';
  char *p2 = strchr(p1, '|');
  if (!p2) return;
  *p2++ = '\0';
  char *p3 = strchr(p2, '|');
  if (!p3) return;
  *p3++ = '\0';
  int32_t code = (int32_t)strtol(copy, NULL, 10);
  time_t timestamp = (time_t)strtol(p1, NULL, 10);
  if (timestamp <= time(NULL) + 30) return;

  int slot = -1;
  for (int i = 0; i < MAX_NOTE_WAKEUPS; i++) {
    if (s_note_wakeups[i].code == code) {
      slot = i;
      if (s_note_wakeups[i].wakeup_id >= 0)
        wakeup_cancel(s_note_wakeups[i].wakeup_id);
      break;
    }
    time_t old_time;
    if (slot < 0 && (s_note_wakeups[i].code == 0 ||
        s_note_wakeups[i].wakeup_id < 0 ||
        !wakeup_query(s_note_wakeups[i].wakeup_id, &old_time))) {
      slot = i;
    }
  }
  if (slot < 0) return;
  WakeupId wakeup_id = wakeup_schedule(timestamp, code, true);
  if (wakeup_id < 0) return;
  NoteWakeupSlot *entry = &s_note_wakeups[slot];
  memset(entry, 0, sizeof(*entry));
  entry->code = code;
  entry->wakeup_id = wakeup_id;
  strncpy(entry->note_id, p2, sizeof(entry->note_id) - 1);
  strncpy(entry->title, p3, sizeof(entry->title) - 1);
  persist_write_data(PERSIST_NOTE_WAKEUP_BASE + slot, entry, sizeof(*entry));
}

// ═══════════════════════════════════════════════════════════════════════════════
// TTS 语音播放（Emery / Flint 扬声器平台）
//
// 手机端把 AI 回复经 Google TTS (8kHz 16-bit) → 取高 8 位转 raw 8bit PCM → 分块发来。
// 手表端：收 raw PCM → 环形缓冲 → 保持原始动态 → speaker_stream_write 播放。
// 不再使用 ADPCM 压缩：4bit ADPCM 量化噪声大（底噪）且瞬态跟随差（沙哑），
// raw 8bit PCM 8000B/s 在蓝牙带宽（23k+ B/s）和流控范围内，音质显著提升。
// ═══════════════════════════════════════════════════════════════════════════════
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
static void tts_cancel_close_timer(void) {
  if (s_tts_close_timer) {
    app_timer_cancel(s_tts_close_timer);
    s_tts_close_timer = NULL;
  }
}

static void tts_clear_pending_write(void) {
  s_tts_pending_write_len = 0;
  s_tts_pending_write_offset = 0;
}

static void tts_cancel_flow_retry(void) {
  if (s_tts_flow_retry_timer) {
    app_timer_cancel(s_tts_flow_retry_timer);
    s_tts_flow_retry_timer = NULL;
  }
}

static int tts_clamp_volume(int volume) {
  if (volume < TTS_VOLUME_MIN) return TTS_VOLUME_MIN;
  if (volume > TTS_VOLUME_MAX) return TTS_VOLUME_MAX;
  return volume;
}

static uint8_t tts_effective_speaker_volume(void) {
  int logical = tts_clamp_volume(s_tts_volume);
  // 保留菜单 10%-100% 的完整范围，底层按比例映射到硬件 6%-60%。
  return (uint8_t)((logical * TTS_SPEAKER_VOLUME_MAX + 50) / 100);
}

static void tts_apply_speaker_volume(void) {
  s_tts_volume = tts_clamp_volume(s_tts_volume);
  speaker_set_volume(tts_effective_speaker_volume());
}

static bool tts_write_or_buffer(const int8_t *samples, uint16_t len) {
  if (len == 0) return true;
  if (!s_tts_pending_write_buf) return false;

  tts_apply_speaker_volume();
  uint32_t written = speaker_stream_write((uint8_t *)samples, len);
  uint16_t accepted = (written > (uint32_t)len) ? len : (uint16_t)written;
  if (accepted < len) {
    uint16_t remaining = len - accepted;
    memcpy(s_tts_pending_write_buf, samples + accepted, remaining);
    s_tts_pending_write_len = remaining;
    s_tts_pending_write_offset = 0;
    return false;
  }
  return true;
}

static bool tts_flush_pending_write(void) {
  if (s_tts_pending_write_offset >= s_tts_pending_write_len) {
    tts_clear_pending_write();
    return true;
  }

  uint16_t remaining = s_tts_pending_write_len - s_tts_pending_write_offset;
  tts_apply_speaker_volume();
  uint32_t written = speaker_stream_write(
    (uint8_t *)(s_tts_pending_write_buf + s_tts_pending_write_offset),
    remaining
  );
  uint16_t accepted = (written > (uint32_t)remaining) ? remaining : (uint16_t)written;
  s_tts_pending_write_offset += accepted;

  if (s_tts_pending_write_offset < s_tts_pending_write_len) {
    return false;
  }

  tts_clear_pending_write();
  return true;
}

static void tts_delayed_close_callback(void *data) {
  s_tts_close_timer = NULL;
  if (s_tts_count == 0 && s_speaker_open) {
    speaker_stream_close();
    s_speaker_open = false;
    tts_clear_pending_write();
    // 全部数据已发完且缓冲排空：朗读会话真正结束，置 playing=false 恢复按钮语义。
    // 句间停顿（all_sent 未置）时只关扬声器，保留 playing=true 让 SELECT 停止手势有效。
    if (s_tts_all_sent) {
      s_tts_playing = false;
      s_tts_loading = false;
      s_tts_all_sent = false;
      s_tts_started_playback = false;
      tts_cancel_flow_retry();
      layer_mark_dirty(s_canvas_layer);
      if (s_tts_watchdog_timer) {
        app_timer_cancel(s_tts_watchdog_timer);
        s_tts_watchdog_timer = NULL;
      }
    }
  } else if (s_tts_count > 0 && !s_tts_all_sent && s_speaker_open) {
    // 缓冲有残余但不是流尾（句间/异常停顿 1500ms 无新数据到达）。
    // 不再空转等数据：把残余冲掉并关闭扬声器，避免 20ms 重试空转到 30s watchdog。
    // 这是数据流中断的兜底，正常情况下句间 TTS_END/TTS_DONE 会正常推进。
    APP_LOG(APP_LOG_LEVEL_WARNING, "TTS close timeout with %d bytes remaining", (int)s_tts_count);
    s_tts_count = 0;
    s_tts_head = 0;
    s_tts_tail = 0;
    tts_clear_pending_write();
    speaker_stream_close();
    s_speaker_open = false;
    s_tts_playing = false;
    s_tts_loading = false;
    s_tts_started_playback = false;
    s_tts_paused = false;
    tts_cancel_flow_retry();
    layer_mark_dirty(s_canvas_layer);
    if (s_tts_watchdog_timer) {
      app_timer_cancel(s_tts_watchdog_timer);
      s_tts_watchdog_timer = NULL;
    }
  }
}

static void tts_schedule_close_timer(void) {
  tts_cancel_close_timer();
  s_tts_close_timer = app_timer_register(TTS_CLOSE_DELAY_MS, tts_delayed_close_callback, NULL);
}

static void tts_stop(void);  // 前向声明：看门狗回调在 tts_stop 定义之前调用它
static void tts_cancel_remote(void);  // 前向声明：看门狗在定义之前调用

// TTS 看门狗：防止 TTS_DONE/STATUS 被蓝牙丢弃导致 playing/loading 永久卡死
static void tts_watchdog_callback(void *data) {
  s_tts_watchdog_timer = NULL;
  if (s_tts_playing || s_tts_loading) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "TTS watchdog timeout");
    tts_stop();
    tts_cancel_remote();  // 通知 JS 停止发送
    vibes_double_pulse();
  }
}

static void tts_reset_watchdog(void) {
  if (s_tts_watchdog_timer) app_timer_cancel(s_tts_watchdog_timer);
  s_tts_watchdog_timer = app_timer_register(TTS_WATCHDOG_MS, tts_watchdog_callback, NULL);
}

// 向 JS 发送流控信号（PAUSE/RESUME）。outbox 单槽，但这是 watch→JS 方向，
// 与 JS→watch 的 TTS chunk（走 inbox）不竞争同一槽，不会卡住数据流。
static void tts_flow_retry_callback(void *data);

static void tts_schedule_flow_retry(bool pause) {
  s_tts_flow_retry_pause = pause;
  tts_cancel_flow_retry();
  s_tts_flow_retry_timer = app_timer_register(TTS_FLOW_RETRY_MS, tts_flow_retry_callback, NULL);
}

static void tts_send_flow_control(bool pause) {
  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  if (result == APP_MSG_OK) {
    dict_write_uint8(iter, pause ? MESSAGE_KEY_TTS_PAUSE : MESSAGE_KEY_TTS_RESUME, 1);
    result = app_message_outbox_send();
    if (result == APP_MSG_OK) {
      tts_cancel_flow_retry();
      return;
    }
  }
  APP_LOG(APP_LOG_LEVEL_WARNING, "TTS flow %s send deferred: %d", pause ? "PAUSE" : "RESUME", (int)result);
  tts_schedule_flow_retry(pause);
}

static void tts_flow_retry_callback(void *data) {
  s_tts_flow_retry_timer = NULL;
  if (!s_tts_playing && !s_tts_loading) return;
  tts_send_flow_control(s_tts_flow_retry_pause);
}

// 把收到的 raw PCM 字节推入环形缓冲
static void tts_push(const uint8_t *data, uint16_t len) {
  if (!s_tts_ring) return;
  for (uint16_t i = 0; i < len; i++) {
    if (s_tts_count < TTS_RING_SIZE) {
      s_tts_ring[s_tts_tail] = data[i];
      s_tts_tail = (s_tts_tail + 1) % TTS_RING_SIZE;
      s_tts_count++;
    } else {
      // 缓冲溢出：流控 PAUSE 往返期间在途 chunk 超出预期余量。
      // 记日志便于诊断（正常不应触发；触发则需调大 RING/HIGH 或减小 watermark）。
      if (!s_tts_overflow_logged) {
        APP_LOG(APP_LOG_LEVEL_WARNING, "TTS ring overflow, dropping %d bytes", (int)(len - i));
        s_tts_overflow_logged = true;
      }
      break;
    }
  }
  tts_cancel_close_timer();

  // 流控：缓冲水位检查。放在数据进入时（而非播放消费时）反应更快，
  // 在溢出发生前就通知 JS 停止投递。
  if (s_tts_count >= TTS_PAUSE_HIGH && !s_tts_paused) {
    s_tts_paused = true;
    tts_send_flow_control(true);
  }
}

static void tts_playback_timer_callback(void *data);

static void tts_start_playback(void) {
  tts_cancel_close_timer();
  if (!s_speaker_open) {
    s_tts_volume = tts_clamp_volume(s_tts_volume);
    uint16_t open_volume = (uint16_t)tts_effective_speaker_volume();
    bool ok = speaker_stream_open(SpeakerPcmFormat_8kHz_8bit, open_volume);
    if (!ok) {
      // 扬声器打开失败：快速失败而非等 30s watchdog。
      // 清理 TTS 状态恢复按钮语义，通知 JS 停止发送。
      APP_LOG(APP_LOG_LEVEL_ERROR, "Speaker open failed");
      tts_stop();
      tts_cancel_remote();
      vibes_double_pulse();
      return;
    }
    s_speaker_open = true;
    tts_apply_speaker_volume();
  }
  s_tts_started_playback = true;
  if (!s_tts_playback_timer) {
    s_tts_playback_timer = app_timer_register(TTS_PLAYBACK_MS, tts_playback_timer_callback, NULL);
  }
}

// 关键：speaker_stream_write 本身会按 PCM 采样率播放；这里的 timer 是 pump。
// 每次多写一点，让 speaker 内部 FIFO 覆盖 AppTimer 抖动；短写则保留未写入部分，10ms 后继续补。
static void tts_playback_timer_callback(void *data) {
  s_tts_playback_timer = NULL;
  if (!s_speaker_open) return;

  if (!s_tts_out_buf || !s_tts_pending_write_buf) {
    tts_stop();
    return;
  }

  if (!tts_flush_pending_write()) {
    s_tts_playback_timer = app_timer_register(10, tts_playback_timer_callback, NULL);
    return;
  }

  uint16_t decode_count = 0;
  if (s_tts_count >= TTS_DECODE_BYTES) {
    decode_count = TTS_DECODE_BYTES;
  } else if (s_tts_count >= TTS_MIN_WRITE_BYTES) {
    decode_count = (uint16_t)s_tts_count;
  }

  if (decode_count > 0) {
    for (int i = 0; i < decode_count; i++) {
      int8_t sample = (int8_t)s_tts_ring[s_tts_head];
      s_tts_head = (s_tts_head + 1) % TTS_RING_SIZE;
      s_tts_count--;
      // 不在 8-bit PCM 上做数字音量衰减；低音量会损失有效位深，明显放大量化底噪。
      // 音量交给 speaker_stream_open(..., volume)，保持样本动态范围。
      s_tts_out_buf[i] = sample;
    }
    if (tts_write_or_buffer(s_tts_out_buf, decode_count)) {
      s_tts_playback_timer = app_timer_register(TTS_PLAYBACK_MS, tts_playback_timer_callback, NULL);
    } else {
      s_tts_playback_timer = app_timer_register(10, tts_playback_timer_callback, NULL);
    }

    // 流控恢复：消费一帧后缓冲已降到 LOW 以下 → 通知 JS 继续投递。
    // 放这里（而非 tts_push）是因为 RESUME 取决于消费进度，只有播放定时器知道。
    if (s_tts_paused && s_tts_count <= TTS_RESUME_LOW) {
      s_tts_paused = false;
      tts_send_flow_control(false);
    }

  } else if (s_tts_count > 0) {
    // 缓冲不足一帧（蓝牙流控导致的瞬时回落）。
    if (s_tts_all_sent) {
      // 整句已全部到达：这是真正的流尾，把剩余 < 800B 一次性冲掉（末尾断续可接受）
      int decode_count = s_tts_count;
      for (int i = 0; i < decode_count; i++) {
        int8_t sample = (int8_t)s_tts_ring[s_tts_head];
        s_tts_head = (s_tts_head + 1) % TTS_RING_SIZE;
        s_tts_count--;
        s_tts_out_buf[i] = sample;
      }
      if (tts_write_or_buffer(s_tts_out_buf, decode_count)) {
        // 缓冲已空，由 close 回调收尾
        tts_schedule_close_timer();
      } else {
        s_tts_playback_timer = app_timer_register(10, tts_playback_timer_callback, NULL);
      }
    } else {
      // 还有数据在路上：10ms 后重试，不写半帧（避免静音间隙）。
      // 100ms 帧长下 10ms 重试 = 帧长的 10%，Emery CPU 足够支撑。
      // 蓝牙继续补包时，一两次重试通常即可凑满一帧。
      s_tts_playback_timer = app_timer_register(10, tts_playback_timer_callback, NULL);
      tts_schedule_close_timer();  // 兜底：若后续数据不再来，超时关闭
    }
  } else {
    // 缓冲空，安排延迟关闭
    if (!s_tts_all_sent) {
      // 中途 underrun：通知 JS 临时加速补货。复用 RESUME，避免新增 message key。
      tts_send_flow_control(false);
    }
    tts_schedule_close_timer();
  }
}

static void tts_stop(void) {
  if (s_speaker_open) {
    speaker_stream_close();
    s_speaker_open = false;
  }
  if (s_tts_playback_timer) {
    app_timer_cancel(s_tts_playback_timer);
    s_tts_playback_timer = NULL;
  }
  tts_cancel_close_timer();
  s_tts_head = 0;
  s_tts_tail = 0;
  s_tts_count = 0;
  tts_clear_pending_write();
  s_tts_playing = false;
  s_tts_loading = false;
  s_tts_all_sent = false;
  s_tts_started_playback = false;
  s_tts_paused = false;       // 重置流控状态：停止时不再处于暂停态
  s_tts_overflow_logged = false;
  tts_cancel_flow_retry();
  if (s_tts_watchdog_timer) {
    app_timer_cancel(s_tts_watchdog_timer);
    s_tts_watchdog_timer = NULL;
  }
  // 注意：不发 TTS_CANCEL。tts_stop 只清本地状态。
  // 需要通知 JS 停止发送时由调用方调 tts_cancel_remote()，
  // 避免 tts_request 里 tts_stop 占用 outbox 导致 TTS_REQUEST 发送失败。
  // 重绘 canvas：标题区可能从 "Reading..." 恢复，TTS 提示需重新显示。
  layer_mark_dirty(s_canvas_layer);
}

// 通知 JS 取消 TTS：停止 fetch/send 并清空 amQueue 里的 TTS chunk。
// 与 tts_stop 分离，避免 tts_request→tts_stop→outbox 占用导致 TTS_REQUEST 失败。
static void tts_cancel_remote(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter, MESSAGE_KEY_TTS_CANCEL, 1);
    app_message_outbox_send();
  }
}

// 向手机请求朗读当前回复
static void tts_request(void) {
  if (!s_tts_ring || !s_tts_pending_write_buf || !s_tts_out_buf) {
    vibes_double_pulse();
    return;
  }
  tts_stop();          // 清掉旧状态（不发 TTS_CANCEL，避免占用 outbox）
  s_tts_playing = true;
  s_tts_loading = true;
  layer_mark_dirty(s_canvas_layer);  // 立即显示 "Reading..." 提示
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter, MESSAGE_KEY_TTS_REQUEST, 1);
    app_message_outbox_send();
    vibes_short_pulse();
    tts_reset_watchdog();  // 启动看门狗，防 TTS_DONE/STATUS 丢失导致卡死
  } else {
    s_tts_playing = false;
    s_tts_loading = false;
    vibes_double_pulse();
  }
}

#endif  // PBL_PLATFORM_EMERY || PBL_PLATFORM_FLINT
// 根据 s_font_size 和 s_font_bold 选择回复区域和问题显示的字体，
// 并动态调整问题显示层高度。标题行（canvas_draw 中）在调用时也需联动。
// ═══════════════════════════════════════════════════════════════════════════════
static GFont get_reply_font(void) {
  // Use the same 14/18/24 ladder as the question area. Previously replies
  // started at 18 and used 18/24/28, so Chinese answers stayed visibly larger
  // than the user's Chinese question even when Small was selected.
  if (s_font_size == 2) return s_font_bold ? fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD) : fonts_get_system_font(FONT_KEY_GOTHIC_24);
  if (s_font_size == 1) return s_font_bold ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD) : fonts_get_system_font(FONT_KEY_GOTHIC_18);
  return s_font_bold ? fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD) : fonts_get_system_font(FONT_KEY_GOTHIC_14);
}

static GFont get_question_font(void) {
  if (s_font_size == 2) return s_font_bold ? fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD) : fonts_get_system_font(FONT_KEY_GOTHIC_24);
  if (s_font_size == 1) return s_font_bold ? fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD) : fonts_get_system_font(FONT_KEY_GOTHIC_18);
  return s_font_bold ? fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD) : fonts_get_system_font(FONT_KEY_GOTHIC_14);
}

static int get_question_layer_height(void) {
  if (s_font_size == 2) return 60;
  if (s_font_size == 1) return 52;
  return 44;
}

static void apply_font_settings(void) {
  text_layer_set_font(s_reply_layer, get_reply_font());
  text_layer_set_font(s_question_display_layer, get_question_font());
  int q_h = get_question_layer_height();
  layer_set_frame(text_layer_get_layer(s_question_display_layer), GRect(10, 0, s_width - 20, q_h));
  if (s_state == STATE_RESPONSE) {
    // 重算回复层 y 偏移，适配新问题层高度
    GSize q_size = text_layer_get_content_size(s_question_display_layer);
    int reply_y = q_size.h + 12;
    layer_set_frame(text_layer_get_layer(s_reply_layer), GRect(8, reply_y, s_width - 16, 2000));
    update_response_text();
  }
  layer_mark_dirty(s_canvas_layer);
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

// 纯色背景
static void draw_bg(GContext *ctx) {
  graphics_context_set_fill_color(ctx, s_bg_color);
  graphics_fill_rect(ctx, GRect(0, 0, s_width, s_height), 0, GCornerNone);
}

// Pebble-style compact screen header. It follows the Home theme selected for
// this launch and always uses white bold text for reliable contrast.
static void draw_theme_header(GContext *ctx, const char *title, int width) {
  graphics_context_set_fill_color(ctx, C_MENU_HIGHLIGHT);
  graphics_fill_rect(
    ctx, GRect(0, 0, width, THEME_HEADER_HEIGHT), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(
    ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(6, 1, width - 12, THEME_HEADER_HEIGHT - 1),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Wi logo 路径绘制
// scale: 缩放百分比 (100=全尺寸)
// morph: 0=Wi logo, LOGO_MORPH_TOTAL=完整圆环
// heartbeat: 心跳偏移像素 (±2)
static void draw_wi_logo(GContext *ctx, int cx, int cy, int scale,
                          int morph, GColor color, bool shadow, int heartbeat) {
  int keep = scale + heartbeat;  // 心跳影响整体缩放
  if (keep < 10) keep = 10;
  int m = morph;
  int mt = LOGO_MORPH_TOTAL;
  int ring_r = 28 * keep / 100;
  int sw = (keep < 50) ? 2 : 5;  // 小尺寸用细线条

  // W 的 9 个控制点（static const 避免栈分配）
  static const int logo_x[] = { -28, -22, -14, -6, 0, 6, 14, 20, 26 };
  static const int logo_y[] = { -20, -6, 14, -3, -12, -3, 14, -6, -20 };

  GPoint pts[9];
  if (m == 0) {
    // 无 morph，直接用 logo 坐标，跳过三角函数
    for (int i = 0; i < 9; i++) {
      pts[i] = GPoint(cx + logo_x[i] * keep / 100,
                      cy + logo_y[i] * keep / 100);
    }
  } else {
    for (int i = 0; i < 9; i++) {
      int lx = logo_x[i] * keep / 100;
      int ly = logo_y[i] * keep / 100;
      int deg = 200 + i * 160 / 8;
      int32_t angle = (deg * TRIG_MAX_ANGLE) / 360;
      int rx = (sin_lookup(angle) * ring_r) / TRIG_MAX_RATIO;
      int ry = -(cos_lookup(angle) * ring_r) / TRIG_MAX_RATIO;
      pts[i] = GPoint(cx + lx + (rx - lx) * m / mt,
                      cy + ly + (ry - ly) * m / mt);
    }
  }

  // i 的竖线和圆点 — 与 W 右斜线平行（dx=6,dy=-14 方向）
  // i 底部起点在 W 右端稍右，沿斜线方向向上延伸
  int i_base_x = 30, i_base_y = 10;   // i 底部
  int i_dx = 5, i_dy = -18;           // i 方向（与 W 右斜线平行）
  int i_bot_lx = i_base_x * keep / 100;
  int i_bot_ly = i_base_y * keep / 100;
  int i_top_lx = (i_base_x + i_dx) * keep / 100;
  int i_top_ly = (i_base_y + i_dy) * keep / 100;
  int i_dot_lx = (i_base_x + i_dx + 2) * keep / 100;  // 圆点在顶部再上方
  int i_dot_ly = (i_base_y + i_dy - 6) * keep / 100;
  GPoint i_top, i_bot, i_dot;
  if (m == 0) {
    i_top = GPoint(cx + i_top_lx, cy + i_top_ly);
    i_bot = GPoint(cx + i_bot_lx, cy + i_bot_ly);
    i_dot = GPoint(cx + i_dot_lx, cy + i_dot_ly);
  } else {
    int32_t a; int rx, ry;
    a = (120 * TRIG_MAX_ANGLE) / 360;
    rx = (sin_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    ry = -(cos_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    i_top = GPoint(cx + i_top_lx + (rx - i_top_lx) * m / mt,
                   cy + i_top_ly + (ry - i_top_ly) * m / mt);
    a = (160 * TRIG_MAX_ANGLE) / 360;
    rx = (sin_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    ry = -(cos_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    i_bot = GPoint(cx + i_bot_lx + (rx - i_bot_lx) * m / mt,
                   cy + i_bot_ly + (ry - i_bot_ly) * m / mt);
    a = (80 * TRIG_MAX_ANGLE) / 360;
    rx = (sin_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    ry = -(cos_lookup(a) * ring_r) / TRIG_MAX_RATIO;
    i_dot = GPoint(cx + i_dot_lx + (rx - i_dot_lx) * m / mt,
                   cy + i_dot_ly + (ry - i_dot_ly) * m / mt);
  }

  // 阴影
  if (shadow) {
    int sx = 2, sy = 2;
    graphics_context_set_stroke_color(ctx, C_SHADOW);
    graphics_context_set_stroke_width(ctx, sw);
    for (int i = 0; i < 8; i++)
      graphics_draw_line(ctx, GPoint(pts[i].x+sx, pts[i].y+sy),
                              GPoint(pts[i+1].x+sx, pts[i+1].y+sy));
    graphics_draw_line(ctx, GPoint(i_top.x+sx, i_top.y+sy),
                            GPoint(i_bot.x+sx, i_bot.y+sy));
    int dr = 3 * keep / 100;
    dr = dr * (mt - m) / mt;
    if (dr > 0) {
      graphics_context_set_fill_color(ctx, C_SHADOW);
      graphics_fill_circle(ctx, GPoint(i_dot.x+sx, i_dot.y+sy), dr);
    }
    // morph 时补画上半弧阴影
    if (m > mt / 3) {
      for (int seg = 0; seg < 6; seg++) {
        int d1 = 10 + seg * 190 / 6;
        int d2 = 10 + (seg + 1) * 190 / 6;
        int32_t a1 = (d1 * TRIG_MAX_ANGLE) / 360;
        int32_t a2 = (d2 * TRIG_MAX_ANGLE) / 360;
        graphics_draw_line(ctx,
          GPoint(cx+sx + (sin_lookup(a1)*ring_r)/TRIG_MAX_RATIO,
                 cy+sy - (cos_lookup(a1)*ring_r)/TRIG_MAX_RATIO),
          GPoint(cx+sx + (sin_lookup(a2)*ring_r)/TRIG_MAX_RATIO,
                 cy+sy - (cos_lookup(a2)*ring_r)/TRIG_MAX_RATIO));
      }
    }
  }

  // 主线条
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, sw);
  for (int i = 0; i < 8; i++)
    graphics_draw_line(ctx, pts[i], pts[i + 1]);

  // i 竖线
  graphics_draw_line(ctx, i_top, i_bot);

  // i 圆点（morph 时缩小）
  {
    int dot_r = 3 * keep / 100;
    dot_r = dot_r * (mt - m) / mt;
    if (dot_r > 0) {
      graphics_context_set_fill_color(ctx, color);
      graphics_fill_circle(ctx, i_dot, dot_r);
    }
  }

  // morph 时补画上半弧
  if (m > mt / 3) {
    graphics_context_set_stroke_width(ctx, 4);
    for (int seg = 0; seg < 6; seg++) {
      int d1 = 10 + seg * 190 / 6;
      int d2 = 10 + (seg + 1) * 190 / 6;
      int32_t a1 = (d1 * TRIG_MAX_ANGLE) / 360;
      int32_t a2 = (d2 * TRIG_MAX_ANGLE) / 360;
      graphics_draw_line(ctx,
        GPoint(cx + (sin_lookup(a1)*ring_r)/TRIG_MAX_RATIO,
               cy - (cos_lookup(a1)*ring_r)/TRIG_MAX_RATIO),
        GPoint(cx + (sin_lookup(a2)*ring_r)/TRIG_MAX_RATIO,
               cy - (cos_lookup(a2)*ring_r)/TRIG_MAX_RATIO));
    }
  }
}

// thinking 大圆 + 旋转小圆点（规整画法）
// opacity: 0=不可见, 16=完全可见（用于浮现效果）
static void draw_spin_circle(GContext *ctx, int cx, int cy, int r,
                              GColor color, bool shadow, int opacity) {
  if (opacity <= 0) return;
  // opacity < 16 时用灰色模拟半透明
  GColor draw_c = (opacity >= 12) ? color : C_SHADOW;

  if (shadow) {
    graphics_context_set_stroke_color(ctx, C_SHADOW);
    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_circle(ctx, GPoint(cx + 2, cy + 2), r);
  }
  // 主圆环
  graphics_context_set_stroke_color(ctx, draw_c);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_circle(ctx, GPoint(cx, cy), r);

  // 旋转小圆点（在圆环上滑动）
  if (opacity >= 8) {
    int32_t a = (s_arc_angle * TRIG_MAX_ANGLE) / 360;
    int px = cx + (sin_lookup(a) * r) / TRIG_MAX_RATIO;
    int py = cy - (cos_lookup(a) * r) / TRIG_MAX_RATIO;
    if (shadow) {
      graphics_context_set_fill_color(ctx, C_SHADOW);
      graphics_fill_circle(ctx, GPoint(px + 1, py + 1), 4);
    }
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_circle(ctx, GPoint(px, py), 4);
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
      draw_bg(ctx);
      // Wi logo（灰暗色调表示未就绪）
      draw_wi_logo(ctx, cx, cy, 100, 0, GColorLightGray, true, s_heartbeat);
      draw_text(ctx, "Set API in Pebble app", font_small, s_subtitle_y, GColorWhite);
      break;

    case STATE_IDLE_READY:
      // 滩涂海浪背景
      draw_bg(ctx);
      // 白色 Wi logo 带阴影
      draw_wi_logo(ctx, cx, cy, 100, 0, GColorWhite, true, s_heartbeat);
      {
        const char *title = s_sync_status[0] ? s_sync_status :
          (s_wakeup_note_active ? s_wakeup_note_title :
          ((s_active_chat_index >= 0 && s_active_chat_index < s_chat_count) ?
            s_chat_entries[s_active_chat_index].title : "New chat"));
        draw_text(ctx, title, font_sub, s_subtitle_y, GColorWhite);
      }
      break;

    case STATE_RECORDING:
      draw_bg(ctx);
      // Wi 渐隐（morph 前半段画灰色，后半段不画）
      if (s_logo_morph < LOGO_MORPH_TOTAL / 2)
        draw_wi_logo(ctx, cx, cy, 100, 0, C_SHADOW, false, 0);
      if (s_logo_morph >= LOGO_MORPH_TOTAL / 2)
        draw_dots(ctx, cx, cy);
      draw_text(ctx, "Listening...", font_small, s_subtitle_y, GColorWhite);
      break;

    case STATE_REVIEW: {
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
      draw_theme_header(ctx, "Review", s_width);
      graphics_context_set_text_color(ctx, GColorDarkGray);
      graphics_draw_text(
          ctx, "SELECT Send  DOWN Re-record",
          fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
          GRect(4, THEME_HEADER_HEIGHT + 3, s_width - 8, 30),
          GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
      graphics_context_set_fill_color(ctx, GColorLightGray);
      graphics_fill_rect(ctx,
                         GRect(s_width / 4,
                               THEME_HEADER_HEIGHT + 31,
                               s_width / 2, 1),
                         0, GCornerNone);
      break;
    }

    case STATE_SENDING:
      draw_bg(ctx);
      if (s_logo_morph < LOGO_MORPH_TOTAL / 2)
        draw_wi_logo(ctx, cx, cy, 100, 0, C_SHADOW, false, 0);
      if (s_logo_morph >= LOGO_MORPH_TOTAL / 2)
        draw_arrow(ctx, cx, cy);
      draw_text(ctx, "Sending...", font_small, s_subtitle_y, GColorWhite);
      break;

    case STATE_THINKING: {
      draw_bg(ctx);
      int spin_r = s_circle_r_big - 4;
      draw_spin_circle(ctx, cx, cy, spin_r, GColorWhite, true, 16);
      draw_text(ctx, "Thinking...", font_small, s_subtitle_y, GColorWhite);
      break;
    }

    case STATE_SHRINKING:
    case STATE_EXPANDING: {
      draw_bg(ctx);
      int logo_scale = s_circle_r * 100 / s_circle_r_big;
      if (logo_scale < 25) logo_scale = 25;
      if (s_state == STATE_EXPANDING && s_logo_morph < LOGO_MORPH_TOTAL / 2)
        draw_wi_logo(ctx, s_circle_x, s_circle_y, logo_scale, 0, GColorWhite, logo_scale > 40, 0);
      else if (s_state == STATE_SHRINKING)
        draw_wi_logo(ctx, s_circle_x, s_circle_y, logo_scale, 0, C_SHADOW, false, 0);
      break;
    }

    case STATE_RESPONSE: {
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

      const char *title = (s_active_chat_index >= 0 && s_active_chat_index < s_chat_count) ? s_chat_entries[s_active_chat_index].title : "New chat";
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
      // TTS 等待首个音频块期间，标题替换为 "Reading..." 作为加载反馈
      if (s_tts_loading) title = "Reading...";
#endif
      draw_theme_header(ctx, title, s_width);
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
      // TTS 操作提示：主题标题栏下方，最小字号显示。
      // 朗读中（s_tts_playing 或 s_tts_loading）时隐藏提示（标题已变 "Reading..."，且此时提示无意义）。
      if (!s_tts_playing && !s_tts_loading) {
        GFont hint_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
#if defined(PBL_PLATFORM_FLINT)
        const char *hint = s_duo_tts_enabled ?
          "Long Press Down to TTS" : "Enable Duo TTS in Config";
#else
        const char *hint = "Long Press Down to TTS";
#endif
        GSize hint_size = graphics_text_layout_get_content_size(hint, hint_font, GRect(0, 0, s_width, 14), GTextOverflowModeWordWrap, GTextAlignmentCenter);
        graphics_context_set_text_color(ctx, GColorLightGray);
        GRect hint_box = GRect((s_width - hint_size.w) / 2,
                               THEME_HEADER_HEIGHT + 3,
                               hint_size.w, 14);
        graphics_draw_text(ctx, hint, hint_font, hint_box, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
      }
#endif
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
  if (s_pulse_timer) {
    app_timer_cancel(s_pulse_timer);
    s_pulse_timer = NULL;
  }
}

// ── 偶发脉冲动效 ─────────────────────────────────────────────────────────────
// 静止 4 秒 → 一次快速 pulse → 再静止 4 秒
// 90% 时间零 CPU 开销

static void schedule_pulse(void) {
  if (s_pulse_timer) {
    app_timer_cancel(s_pulse_timer);
    s_pulse_timer = NULL;
  }
  // 首次脉冲更快出现（1.5秒），后续间隔 4 秒
  int delay = (s_pulse_frame == 0 && s_heartbeat == 0) ? PULSE_FIRST_MS : PULSE_INTERVAL_MS;
  s_pulse_timer = app_timer_register(delay, pulse_tick, NULL);
}

static void pulse_tick(void *data) {
  s_pulse_timer = NULL;
  if (s_state != STATE_IDLE_NO_KEY && s_state != STATE_IDLE_READY) return;

  s_pulse_frame++;

  // 脉冲曲线：10帧膨胀 + 6帧回弹，每帧最大跳 2-3px，视觉平滑
  if (s_pulse_frame <= 10) {
    // 帧 1-10: 正弦半周 0 → +8 → 0
    int deg = s_pulse_frame * 180 / 10;
    int32_t angle = (deg * TRIG_MAX_ANGLE) / 360;
    s_heartbeat = (sin_lookup(angle) * 8) / TRIG_MAX_RATIO;
  } else if (s_pulse_frame <= 16) {
    // 帧 11-16: 回弹 0 → -2 → 0
    int deg = (s_pulse_frame - 10) * 180 / 6;
    int32_t angle = (deg * TRIG_MAX_ANGLE) / 360;
    s_heartbeat = -(sin_lookup(angle) * 2) / TRIG_MAX_RATIO;
  } else {
    // 脉冲结束，恢复静态
    s_heartbeat = 0;
    s_pulse_frame = 0;
    layer_mark_dirty(s_canvas_layer);
    schedule_pulse();
    return;
  }

  layer_mark_dirty(s_canvas_layer);
  s_pulse_timer = app_timer_register(33, pulse_tick, NULL);  // ~30fps
}

static void anim_tick(void *data) {
  s_anim_frame++;

  // IDLE 状态不再走 anim_tick，由 pulse_tick 独立驱动
  s_heartbeat = 0;

  // Wi→圆 morph 驱动
  if (s_state == STATE_RECORDING || s_state == STATE_SENDING ||
      s_state == STATE_THINKING || s_state == STATE_SHRINKING) {
    if (s_logo_morph < LOGO_MORPH_TOTAL) s_logo_morph++;
  }
  if (s_state == STATE_EXPANDING) {
    if (s_logo_morph > 0) s_logo_morph--;
  }

  // thinking: 旋转弧线
  if (s_state == STATE_THINKING) {
    s_arc_angle = (s_arc_angle + 3) % 360;
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
      set_state(STATE_IDLE_READY);
      start_dictation_if_possible();
      return;
    }
    s_circle_r = ease_out(CIRCLE_R_SMALL, s_circle_r_big, s_morph_step, MORPH_TOTAL);
    s_circle_y = ease_out(CIRCLE_Y_SMALL, s_circle_y_big, s_morph_step, MORPH_TOTAL);
    s_circle_x = ease_out(s_circle_target_x, s_width / 2, s_morph_step, MORPH_TOTAL);
  }

  layer_mark_dirty(s_canvas_layer);

  int interval = 50;  // 默认 20fps
  if (s_state == STATE_SHRINKING || s_state == STATE_EXPANDING) interval = 25;  // 40fps
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

// 所有手动/自动录音共用同一入口，保证同一时刻只有一个 DictationSession。
static bool start_dictation_if_possible(void) {
  if (s_state != STATE_IDLE_READY || s_dictation_session) return false;
  s_dictation_session = dictation_session_create(
      DICTATION_BUF_SIZE, dictation_callback, NULL);
  if (!s_dictation_session) return false;
  // Wrist AI supplies its own transcript review screen with an explicit
  // re-record action, so skip Pebble's fixed confirmation dialog.
  dictation_session_enable_confirmation(s_dictation_session, false);
  set_state(STATE_RECORDING);
  dictation_session_start(s_dictation_session);
  return true;
}

static void auto_dictation_timer_callback(void *data) {
  s_auto_dictation_timer = NULL;
  // Auto Dictation 路径仅在 Quick Launch/Timeline action 入口生效；
  // s_force_launch_dictation（Timeline action）仍无条件强制录音。
  bool auto_ok = (s_auto_dictation && s_quick_launch_entry) ||
                 s_force_launch_dictation;
  if (auto_ok) {
    s_dictation_interrupt_retry_used = false;
    start_dictation_if_possible();
  }
}

static void maybe_schedule_auto_dictation(void) {
  bool auto_ok = (s_auto_dictation && s_quick_launch_entry) ||
                 s_force_launch_dictation;
  APP_LOG(APP_LOG_LEVEL_INFO,
          "AutoDict decision: auto_dict=%d quick_entry=%d force=%d -> %s",
          s_auto_dictation, (int)s_quick_launch_entry,
          (int)s_force_launch_dictation, auto_ok ? "SCHEDULE" : "skip");
  if (!auto_ok ||
      s_auto_dictation_attempted ||
      s_state != STATE_IDLE_READY || s_dictation_session) return;
  // 注册时即标记，确保取消/失败后本次 launch 不会自动循环重试。
  s_auto_dictation_attempted = true;
  s_auto_dictation_timer = app_timer_register(
      400, auto_dictation_timer_callback, NULL);
}

static void dictation_interrupt_retry_callback(void *data) {
  s_dictation_retry_timer = NULL;
  if (s_state == STATE_IDLE_READY && !s_dictation_session)
    start_dictation_if_possible();
}

static void send_pending_question(void) {
  if (!s_pending_question || !s_pending_question[0]) {
    set_state(STATE_IDLE_READY);
    return;
  }

  utf8_safe_copy(s_question_display, sizeof(s_question_display), "You: ");
  utf8_safe_append(s_question_display, sizeof(s_question_display),
                   s_pending_question);

  if (s_health_enabled) {
    read_health_data();
  } else {
    s_step_count = -1;
    s_active_minutes = -1;
  }

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_QUESTION, s_pending_question);
    if (s_health_enabled) {
      dict_write_int32(iter, MESSAGE_KEY_STEP_COUNT, s_step_count);
      dict_write_int32(iter, MESSAGE_KEY_ACTIVE_MINUTES, s_active_minutes);
      if (s_health_data && s_health_data[0] != '\0')
        dict_write_cstring(iter, MESSAGE_KEY_HEALTH_DATA, s_health_data);
      write_sleep_intervals(iter);
    }
    if (s_launch_context[0] != '\0')
      dict_write_cstring(iter, MESSAGE_KEY_LAUNCH_CONTEXT, s_launch_context);
    app_message_outbox_send();
    free(s_pending_question);
    s_pending_question = NULL;
    set_state(STATE_SENDING);
  } else {
    if (s_health_data) {
      free(s_health_data);
      s_health_data = NULL;
    }
    vibes_double_pulse();
    set_state(STATE_REVIEW);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Dictation 回调（前向声明需要，放在 anim_tick 之后）
// ═══════════════════════════════════════════════════════════════════════════════
static void dictation_callback(DictationSession *session,
                                DictationSessionStatus status,
                                char *transcription,
                                void *context) {
  if (s_dictation_session) {
    dictation_session_destroy(s_dictation_session);
    s_dictation_session = NULL;
  }

  if (status == DictationSessionStatusSuccess && transcription && transcription[0] != '\0') {
    if (s_pending_question) free(s_pending_question);
    size_t transcription_len = strlen(transcription);
    s_pending_question = malloc(transcription_len + 1);
    if (!s_pending_question) {
      vibes_double_pulse();
      set_state(STATE_IDLE_READY);
      return;
    }
    memcpy(s_pending_question, transcription, transcription_len + 1);
    text_layer_set_text(s_question_display_layer, "");
    text_layer_set_text(s_reply_layer, s_pending_question);
    set_state(STATE_REVIEW);
    vibes_short_pulse();
    s_dictation_interrupt_retry_used = false;
  } else if (status == DictationSessionStatusFailureSystemAborted &&
             !s_dictation_interrupt_retry_used) {
    // Notifications and other system overlays can abort Dictation. Retry once
    // after the overlay goes away; never loop indefinitely.
    s_dictation_interrupt_retry_used = true;
    set_state(STATE_IDLE_READY);
    if (s_dictation_retry_timer)
      app_timer_cancel(s_dictation_retry_timer);
    s_dictation_retry_timer = app_timer_register(
        700, dictation_interrupt_retry_callback, NULL);
  } else {
    vibes_short_pulse();  // 提示用户 Dictation 被取消
    s_dictation_interrupt_retry_used = false;
    set_state(STATE_IDLE_READY);
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// 状态切换
// ═══════════════════════════════════════════════════════════════════════════════
static void set_state(AppState new_state) {
  stop_anim();
  s_state = new_state;

  bool show_scroll = (new_state == STATE_RESPONSE || new_state == STATE_REVIEW);
  layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), !show_scroll);

  switch (new_state) {
    case STATE_IDLE_NO_KEY:
    case STATE_IDLE_READY:
      s_logo_morph = 0;       // 重置为完整 Wi logo
      s_heartbeat = 0;        // 确保静态起步
      s_pulse_frame = 0;      // 重置脉冲帧计数（防中途打断后残留）
      schedule_pulse();
      break;
    case STATE_RECORDING:
    case STATE_SENDING:
    case STATE_THINKING:
      start_anim();
      break;

    case STATE_REVIEW: {
      text_layer_set_text_color(s_question_display_layer, GColorDarkGray);
      text_layer_set_text(s_question_display_layer, "");
      text_layer_set_text(s_reply_layer,
                          s_pending_question ? s_pending_question : "");
      layer_set_frame(text_layer_get_layer(s_reply_layer),
                      GRect(8, 4, s_width - 16, 2000));
      GSize review_size = text_layer_get_content_size(s_reply_layer);
      int visible_h = layer_get_bounds(
          scroll_layer_get_layer(s_scroll_layer)).size.h;
      s_response_content_h = review_size.h + 12;
      if (s_response_content_h < visible_h)
        s_response_content_h = visible_h;
      scroll_layer_set_content_size(
          s_scroll_layer, GSize(s_width, s_response_content_h));
      scroll_layer_set_content_offset(s_scroll_layer, GPointZero, false);
      s_user_scrolled = false;
      break;
    }

    case STATE_SHRINKING:
      s_morph_step = 0;
      s_circle_r = s_circle_r_big;
      s_circle_y = s_circle_y_big;
      {
        const char *title = (s_active_chat_index >= 0 && s_active_chat_index < s_chat_count) ? s_chat_entries[s_active_chat_index].title : "New chat";
        GFont shrink_title_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
        int shrink_title_h = 20;
        if (s_font_size == 2) { shrink_title_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD); shrink_title_h = 28; }
        else if (s_font_size == 1) { shrink_title_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD); shrink_title_h = 24; }
        GSize ts = graphics_text_layout_get_content_size(title, shrink_title_font, GRect(0, 0, s_width, shrink_title_h), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
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
        int scroll_top = THEME_HEADER_HEIGHT + 4 + TTS_HINT_OFFSET;
        GSize q_size = text_layer_get_content_size(s_question_display_layer);
        int reply_y = q_size.h + 12;
        layer_set_frame(text_layer_get_layer(s_reply_layer), GRect(8, reply_y, s_width - 16, 2000));
        s_response_content_h = s_height - scroll_top;
        scroll_layer_set_content_size(s_scroll_layer, GSize(s_width, s_response_content_h));
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
    // JS 端 sendToWatch({'READY_STATUS': isReady}) 发送小整数，PebbleKit JS 自动编码为 uint8。
    // 用 uint8 读取（与 JS 编码一致），避免 int32 读 4 字节时高 3 字节残留导致误判。
    bool ready = (ready_t->value->uint8 == 1);
    persist_write_bool(PERSIST_KEY_READY, ready);
    if (s_state == STATE_IDLE_NO_KEY || s_state == STATE_IDLE_READY) {
      set_state(ready ? STATE_IDLE_READY : STATE_IDLE_NO_KEY);
      // 必须等本次启动的 AUTO_DICTATION 设置同步完成，不能使用上次持久化值抢跑。
      if (ready && s_auto_dictation_synced) maybe_schedule_auto_dictation();
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

  Tuple *note_list_t = dict_find(iter, MESSAGE_KEY_NOTE_LIST);
  if (note_list_t) {
    MenuIndex previous_note_selection = { .section = 0, .row = 0 };
    bool can_reload_notes =
      s_notes_menu_layer && !s_note_actions_visible;
    if (can_reload_notes) {
      previous_note_selection =
        menu_layer_get_selected_index(s_notes_menu_layer);
      // 解析新列表前先把 SDK 内部索引移到旧列表一定存在的安全行。
      // 删除最后一项时，新行数会减少；如果带着旧索引 reload，部分 Pebble
      // 固件会在 draw_row 前直接越界。
      MenuIndex safe_selection = { .section = 0, .row = 0 };
      menu_layer_set_selected_index(s_notes_menu_layer, safe_selection,
                                    MenuRowAlignNone, false);
    }
    parse_note_list(note_list_t->value->cstring);
    if (can_reload_notes) {
      uint16_t rows = s_note_count > 0 ? s_note_count : 1;
      menu_layer_reload_data(s_notes_menu_layer);
      previous_note_selection.section = 0;
      if (previous_note_selection.row >= rows)
        previous_note_selection.row = rows - 1;
      menu_layer_set_selected_index(s_notes_menu_layer,
                                    previous_note_selection,
                                    MenuRowAlignNone, false);
      s_notes_list_dirty = false;
    } else if (s_notes_menu_layer) {
      s_notes_list_dirty = true;
    }
  }
  Tuple *note_wakeup_t = dict_find(iter, MESSAGE_KEY_NOTE_WAKEUP);
  if (!s_startup_message_guard &&
      note_wakeup_t && note_wakeup_t->type == TUPLE_CSTRING)
    schedule_note_wakeup(note_wakeup_t->value->cstring);
  Tuple *sync_wakeup_t = dict_find(iter, MESSAGE_KEY_SYNC_WAKEUP);
  if (!s_startup_message_guard &&
      sync_wakeup_t && sync_wakeup_t->type == TUPLE_CSTRING)
    schedule_sync_wakeup(sync_wakeup_t->value->cstring);
  Tuple *sync_status_t = dict_find(iter, MESSAGE_KEY_SYNC_STATUS);
  if (sync_status_t && sync_status_t->type == TUPLE_CSTRING) {
    utf8_safe_copy(s_sync_status, sizeof(s_sync_status),
                   sync_status_t->value->cstring);
    if (s_canvas_layer) layer_mark_dirty(s_canvas_layer);
    if (s_sync_wakeup_launch &&
        (strncmp(s_sync_status, "Synced", 6) == 0 ||
         strncmp(s_sync_status, "Offline", 7) == 0 ||
         strncmp(s_sync_status, "Sync failed", 11) == 0)) {
      s_sync_wakeup_launch = false;
      app_timer_register(1800, delayed_pop_timer, NULL);
    }
  }
  Tuple *theme_t = dict_find(iter, MESSAGE_KEY_THEME_COLOR);
  if (theme_t) {
    int value = (int)theme_t->value->int32;
    if (value < -1 || value >= FILL_PALETTE_SIZE) value = -1;
    s_theme_color = value;
    persist_write_int(PERSIST_KEY_THEME_COLOR, s_theme_color);
    if (s_theme_color >= 0)
      s_bg_color = s_fill_palette[s_theme_color];
    layer_mark_dirty(s_canvas_layer);
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

  // 字体大小设置 (0=Normal, 1=Large, 2=Extra Large)
  Tuple *font_size_t = dict_find(iter, MESSAGE_KEY_FONT_SIZE);
  if (font_size_t) {
    s_font_size = font_size_t->value->uint8;
    if (s_font_size > 2) s_font_size = 0;
    persist_write_int(PERSIST_KEY_FONT_SIZE, s_font_size);
    apply_font_settings();
  }

  // 加粗设置 (0=不加粗, 1=加粗)
  Tuple *font_bold_t = dict_find(iter, MESSAGE_KEY_FONT_BOLD);
  if (font_bold_t) {
    s_font_bold = font_bold_t->value->uint8 ? 1 : 0;
    persist_write_int(PERSIST_KEY_FONT_BOLD, s_font_bold);
    apply_font_settings();
  }

  // 禁用彩蛋设置 (0=启用, 1=禁用)
  Tuple *disable_surprise_t = dict_find(iter, MESSAGE_KEY_DISABLE_SURPRISE);
  if (disable_surprise_t) {
    s_disable_surprise = disable_surprise_t->value->uint8 ? 1 : 0;
    persist_write_int(PERSIST_KEY_DISABLE_SURPRISE, s_disable_surprise);
  }

  // 健康数据开关 (0=关闭, 1=开启)
  Tuple *health_enabled_t = dict_find(iter, MESSAGE_KEY_HEALTH_ENABLED);
  if (health_enabled_t) {
    s_health_enabled = health_enabled_t->value->uint8 ? 1 : 0;
    persist_write_int(PERSIST_KEY_HEALTH_ENABLED, s_health_enabled);
  }

  Tuple *auto_dictation_t = dict_find(iter, MESSAGE_KEY_AUTO_DICTATION);
  if (auto_dictation_t) {
    s_auto_dictation_synced = true;
    s_auto_dictation = auto_dictation_t->value->uint8 ? 1 : 0;
    persist_write_int(PERSIST_KEY_AUTO_DICTATION, s_auto_dictation);
    if (!s_auto_dictation && !s_force_launch_dictation &&
        s_auto_dictation_timer) {
      app_timer_cancel(s_auto_dictation_timer);
      s_auto_dictation_timer = NULL;
    } else {
      maybe_schedule_auto_dictation();
    }
  }

  Tuple *health_days_t = dict_find(iter, MESSAGE_KEY_HEALTH_DAYS);
  if (health_days_t) {
    int days = (int)health_days_t->value->uint8;
    s_health_days = (days == 1 || days == 3 || days == 7) ? days : 7;
    persist_write_int(PERSIST_KEY_HEALTH_DAYS, s_health_days);
  }
  Tuple *health_sleep_t = dict_find(iter, MESSAGE_KEY_HEALTH_SLEEP);
  if (health_sleep_t) {
    s_health_sleep = health_sleep_t->value->uint8 ? 1 : 0;
    persist_write_int(PERSIST_KEY_HEALTH_SLEEP, s_health_sleep);
  }
  Tuple *duo_tts_t = dict_find(iter, MESSAGE_KEY_DUO_TTS_ENABLED);
  if (duo_tts_t) {
    s_duo_tts_enabled = duo_tts_t->value->uint8 ? 1 : 0;
    persist_write_int(PERSIST_KEY_DUO_TTS, s_duo_tts_enabled);
  }

  // 联网搜索开关 (0=关闭, 1=开启，默认开，仅 OpenRouter 生效由 JS 端判断)
  Tuple *web_search_t = dict_find(iter, MESSAGE_KEY_WEB_SEARCH_ENABLED);
  if (web_search_t) {
    s_web_search = web_search_t->value->uint8 ? 1 : 0;
    persist_write_int(PERSIST_KEY_WEB_SEARCH, s_web_search);
    if (s_menu_layer) menu_layer_reload_data(s_menu_layer);
  }

  // 用户问题（切换对话时由 JS 发来的历史问题）
  Tuple *user_q_t = dict_find(iter, MESSAGE_KEY_USER_QUESTION);
  if (user_q_t) {
    utf8_safe_copy(s_question_display, sizeof(s_question_display), "You: ");
    utf8_safe_append(s_question_display, sizeof(s_question_display), user_q_t->value->cstring);
    // 为新内容做准备：清空回复缓冲区，如果已在 RESPONSE 则重置布局
    s_reply_buf[0] = '\0';
    if (s_state == STATE_RESPONSE) {
      set_state(STATE_RESPONSE);
    }
  }

  Tuple *reply_t = dict_find(iter, MESSAGE_KEY_REPLY);
  Tuple *reply_chunk_t = dict_find(iter, MESSAGE_KEY_REPLY_CHUNK);
  Tuple *reply_end_t = dict_find(iter, MESSAGE_KEY_REPLY_END);

  if (reply_chunk_t) {
    if (s_state == STATE_SENDING || s_state == STATE_THINKING) {
      s_reply_buf[0] = '\0';
      set_state(STATE_SHRINKING);
    } else if (s_state == STATE_IDLE_READY) {
      // 历史加载：跳过动画直接进入回复页面
      s_reply_buf[0] = '\0';
      set_state(STATE_RESPONSE);
    }
    utf8_safe_append(s_reply_buf, sizeof(s_reply_buf), reply_chunk_t->value->cstring);
    if (s_state == STATE_RESPONSE) {
      update_response_text();
    }
  } else if (reply_end_t) {
    if (s_state == STATE_THINKING || s_state == STATE_SENDING) {
      set_state(STATE_SHRINKING);
    }
  } else if (reply_t) {
    utf8_safe_copy(s_reply_buf, sizeof(s_reply_buf), reply_t->value->cstring);
    if (s_state == STATE_THINKING || s_state == STATE_SENDING) {
      // 缩小动画 → RESPONSE
      set_state(STATE_SHRINKING);
    } else if (s_state == STATE_RESPONSE) {
      update_response_text();
    }
  }

  Tuple *status_t = dict_find(iter, MESSAGE_KEY_STATUS);
  if (status_t && !reply_t && !reply_chunk_t) {
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
    // TTS 失败回传的 STATUS：清理 TTS 状态，不污染回复内容。
    // 区分 TTS 错误与普通错误：JS 端 TTS 错误的 STATUS 文本以 "TTS" 开头
    // （如 "TTS error: HTTP 403"、"TTS: API key invalid"、"No TTS API key" 等）。
    // 非前缀的 STATUS（如后台同步错误）即使在 TTS 播放中到达也不应误停 TTS。
    const char *status_msg = status_t->value->cstring;
    bool is_tts_error = (strncmp(status_msg, "TTS", 3) == 0 ||
                         strncmp(status_msg, "No TTS", 6) == 0);
    if (is_tts_error && (s_tts_playing || s_tts_loading)) {
      tts_stop();
      return;
    }
#endif
    // C2 fix: 追加错误信息前检查剩余空间，防止逼近缓冲区上限
    size_t remaining = sizeof(s_reply_buf) - strlen(s_reply_buf) - 1;
    if (strlen(s_reply_buf) > 0 && remaining > 12) {
      strncat(s_reply_buf, "\n[Err]: ", remaining);
      remaining = sizeof(s_reply_buf) - strlen(s_reply_buf) - 1;
      if (remaining > 0) {
        strncat(s_reply_buf, status_t->value->cstring, remaining);
      }
    } else if (strlen(s_reply_buf) == 0) {
      utf8_safe_copy(s_reply_buf, sizeof(s_reply_buf), status_t->value->cstring);
    }
    
    // 如果还没进入文字界面，提前进入
    if (s_state == STATE_THINKING || s_state == STATE_SENDING) {
      set_state(STATE_SHRINKING);
    } else if (s_state == STATE_RESPONSE) {
      update_response_text();
    }
  }

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  // TTS 音频分块（raw 8bit PCM 字节）
  Tuple *tts_chunk_t = dict_find(iter, MESSAGE_KEY_TTS_CHUNK);
  if (tts_chunk_t) {
    // Session-active 守卫：只有正在播放或加载中才接受音频 chunk。
    // tts_stop/tts_cancel_remote 后蓝牙管道里仍可能有在途 chunk 到达，
    // 不加守卫会无条件复活 s_tts_playing=true 并把旧音频推入已清空的缓冲，
    // 导致按钮瘫痪（30s watchdog 才恢复）或新 session 音频污染。
    if (!s_tts_playing && !s_tts_loading) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "TTS chunk dropped: no active session");
    } else {
      // 首个音频块到达：清除加载提示，恢复原标题
      if (s_tts_loading) {
        s_tts_loading = false;
        layer_mark_dirty(s_canvas_layer);
      }
      tts_push(tts_chunk_t->value->data, tts_chunk_t->length);
      // 首次开播攒足 3s；中途 underrun 后只需 1s 即可恢复，避免重启空洞过长。
      uint32_t start_threshold = s_tts_started_playback ? TTS_RESTART_THRESHOLD : TTS_START_THRESHOLD;
      if (s_tts_count >= start_threshold) {
        tts_start_playback();
      }
      tts_reset_watchdog();  // 收到数据，重置看门狗
    }
  }

  // 单句结束标记：只重置看门狗，不绕过开播阈值。
  // TTS_END 是每一句的边界，不代表整段 TTS 已全部到达；如果这里立即开播，
  // 第一句较短时会用很小的缓冲启动，后续蓝牙抖动就会表现成“几个字一个大空洞”。
  // 真正的流尾由 TTS_DONE 处理，届时即使不足 START_THRESHOLD 也会开播冲尾料。
  Tuple *tts_end_t = dict_find(iter, MESSAGE_KEY_TTS_END);
  if (tts_end_t) {
    // 句间会等下一句 TTS API 往返，期间无 chunk 到达。重置看门狗避免误超时。
    if (s_tts_playing || s_tts_loading) {
      tts_reset_watchdog();
    }
    if (s_tts_count >= TTS_START_THRESHOLD) {
      tts_start_playback();
    }
  }

  // TTS 全部朗读结束（所有句子发完）。
  // L1 修复：若缓冲还有未播数据，只标记 all_sent，不立即置 playing=false，
  // 让播放定时器排空后由 close callback 置 false——避免末段 SELECT 停止失效、音频渗入下一状态。
  // 若缓冲已空（短回复或播放赶上了），直接置 false。
  Tuple *tts_done_t = dict_find(iter, MESSAGE_KEY_TTS_DONE);
  if (tts_done_t) {
    if (s_tts_watchdog_timer) {
      app_timer_cancel(s_tts_watchdog_timer);
      s_tts_watchdog_timer = NULL;
    }
    if (s_tts_count > 0) {
      // 还有未播数据：标记等排空，保持 playing=true 让停止手势有效
      s_tts_all_sent = true;
      // 关键：唤醒播放定时器。新逻辑下缓冲不足一帧时不写半帧、靠 20ms 重试等数据；
      // 若此刻定时器未在跑（例如末句短、从未达到 6000B 开播阈值，或上一个 TTS_END 被丢），
      // 仅置 all_sent 不会触发播放，尾料会卡在缓冲里。这里显式开播兜底。
      tts_start_playback();
    } else {
      // 已播完：直接结束
      s_tts_playing = false;
      s_tts_loading = false;
      s_tts_all_sent = false;
      s_tts_started_playback = false;
      layer_mark_dirty(s_canvas_layer);
    }
  }
#endif
}

static void inbox_dropped(AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Dropped: %d", (int)reason);
}

static void outbox_sent(DictionaryIterator *iter, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Sent OK");
  Tuple *note_command = dict_find(iter, MESSAGE_KEY_NOTE_COMMAND);
  if (note_command && strcmp(note_command->value->cstring,
                             s_pending_note_command) == 0) {
    persist_delete(PERSIST_KEY_NOTE_COMMAND);
    s_pending_note_command[0] = '\0';
  }
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Send fail: %d", (int)reason);
  if (dict_find(iter, MESSAGE_KEY_NOTE_COMMAND)) {
    if (!s_note_command_timer)
      s_note_command_timer =
        app_timer_register(1500, send_queued_note_command, NULL);
    return;
  }
  // 问答路径：发送失败回退到就绪
  if (s_state == STATE_SENDING || s_state == STATE_THINKING) {
    snprintf(s_reply_buf, sizeof(s_reply_buf), "Send failed. Try again.");
    text_layer_set_text(s_reply_layer, s_reply_buf);
    set_state(STATE_SHRINKING);
    return;
  }
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  Tuple *tts_pause_t = dict_find(iter, MESSAGE_KEY_TTS_PAUSE);
  Tuple *tts_resume_t = dict_find(iter, MESSAGE_KEY_TTS_RESUME);
  if (tts_pause_t || tts_resume_t) {
    bool pause = (tts_pause_t != NULL);
    APP_LOG(APP_LOG_LEVEL_WARNING, "TTS flow %s failed, retrying", pause ? "PAUSE" : "RESUME");
    tts_schedule_flow_retry(pause);
    return;
  }

  // TTS_REQUEST 异步发送失败：清理 TTS 状态，防止 s_tts_playing 卡死导致按钮瘫痪。
  // 利用 s_tts_playing 与问答 SENDING 状态互斥区分消息类型（TTS_REQUEST 发出时
  // s_tts_playing=true，问答 SENDING 状态下 s_tts_playing=false），无需读 iter。
  if (s_tts_playing || s_tts_loading) {
    tts_stop();
    tts_cancel_remote();  // 通知 JS 停止发送残余 chunk
    vibes_double_pulse();
  }
#endif
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
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  // TTS 播放中 → 短按 SELECT 停止朗读
  if (s_tts_playing) {
    tts_stop();
    tts_cancel_remote();  // 通知 JS 停止发送
    return;
  }
#endif
  if (s_state == STATE_IDLE_READY) {
    s_dictation_interrupt_retry_used = false;
    start_dictation_if_possible();
  } else if (s_state == STATE_REVIEW) {
    send_pending_question();
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
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  if (s_tts_playing) { tts_stop(); tts_cancel_remote(); vibes_double_pulse(); return; }
#endif

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
static void scroll_response_by(int dy) {
  if ((s_state != STATE_RESPONSE && s_state != STATE_REVIEW) ||
      !s_scroll_layer) return;

  s_user_scrolled = true;  // 用户手动滚动后停止自动滚到顶部

  GRect viewport = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
  int visible_h = viewport.size.h;
  if (s_response_content_h < visible_h) {
    s_response_content_h = visible_h;
  }

  int min_y = visible_h - s_response_content_h;
  if (min_y > 0) min_y = 0;

  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);
  offset.y += dy;
  if (offset.y > 0) offset.y = 0;
  if (offset.y < min_y) offset.y = min_y;

  bool animated = true;
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  // 播放 TTS 时 speaker pump/AppMessage 已经在高频跑；ScrollLayer 动画会再叠一组内部定时器。
  // 这里改成立即跳转，保留翻页能力，同时避开播放中动画滚动导致的偶发崩溃。
  if (s_tts_playing || s_tts_loading) {
    animated = false;
  }
#endif
  scroll_layer_set_content_offset(s_scroll_layer, offset, animated);
}

// UP 短按：在回复状态向上滚动；其他状态打开菜单
static void up_handler(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_RESPONSE || s_state == STATE_REVIEW) {
    scroll_response_by(60);
  } else {
    show_chat_list();
  }
}

// UP 长按：始终打开菜单
static void up_long_handler(ClickRecognizerRef r, void *ctx) {
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  if (s_tts_playing || s_tts_loading) {
    vibes_double_pulse();
    return;
  }
#endif
  show_chat_list();
}

// DOWN 短按：回复页面向下滚动 60px，其他状态打开菜单
static void down_handler(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_REVIEW) {
    text_layer_set_text(s_reply_layer, "");
    if (s_pending_question) {
      free(s_pending_question);
      s_pending_question = NULL;
    }
    s_dictation_interrupt_retry_used = false;
    set_state(STATE_IDLE_READY);
    start_dictation_if_possible();
  } else if (s_state == STATE_RESPONSE) {
    scroll_response_by(-60);
  } else {
    show_notes();
  }
}

static void back_handler(ClickRecognizerRef r, void *ctx) {
  if (s_state != STATE_REVIEW) {
    window_stack_pop(true);
    return;
  }
  text_layer_set_text(s_reply_layer, "");
  if (s_pending_question) {
    free(s_pending_question);
    s_pending_question = NULL;
  }
  s_dictation_interrupt_retry_used = false;
  set_state(STATE_IDLE_READY);
  vibes_short_pulse();
}

// DOWN 长按：Emery 上朗读/调节音量 / 其他平台彩蛋功能
static void down_long_handler(ClickRecognizerRef r, void *ctx) {
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  // 音量现在在 UP 菜单里预先选择，speaker 打开时生效；播放中不做实时重开。
  if (s_tts_playing || s_tts_loading) {
    vibes_double_pulse();
    return;
  }
  // 在 RESPONSE 状态 → 请求朗读当前回复
  if (s_state == STATE_RESPONSE) {
#if defined(PBL_PLATFORM_FLINT)
    if (!s_duo_tts_enabled) {
      vibes_double_pulse();
      return;
    }
#endif
    tts_request();
    return;
  }
#endif

  if (s_disable_surprise) {
    vibes_double_pulse();
    return;
  }
  // 处理中不允许触发彩蛋（含 RECORDING：录音中长按 DOWN 会与 dictation 冲突）
  if (s_state == STATE_RECORDING || s_state == STATE_REVIEW ||
      s_state == STATE_SENDING ||
      s_state == STATE_THINKING ||
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
   
  // 读取最新健康数据（仅在用户开启时）
  if (s_health_enabled) {
    read_health_data();
  } else {
    s_step_count = -1;   // 开关关闭：哨兵值
    s_active_minutes = -1;
  }

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_QUESTION, q);
    if (s_health_enabled) {
      dict_write_int32(iter, MESSAGE_KEY_STEP_COUNT, s_step_count);
      dict_write_int32(iter, MESSAGE_KEY_ACTIVE_MINUTES, s_active_minutes);
      if (s_health_data && s_health_data[0] != '\0')
        dict_write_cstring(iter, MESSAGE_KEY_HEALTH_DATA, s_health_data);
      write_sleep_intervals(iter);
    }
    if (s_launch_context[0] != '\0')
      dict_write_cstring(iter, MESSAGE_KEY_LAUNCH_CONTEXT, s_launch_context);
    app_message_outbox_send();
    vibes_short_pulse();
    set_state(STATE_SENDING);
  } else {
    // 发送失败，清空残留显示
    s_question_display[0] = '\0';
    set_state(STATE_IDLE_READY);
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
  window_single_click_subscribe(BUTTON_ID_BACK, back_handler);
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

  // 初始化随机或 Config 固定主题色。
  srand(time(NULL));
  s_theme_color = persist_exists(PERSIST_KEY_THEME_COLOR) ?
    (int)persist_read_int(PERSIST_KEY_THEME_COLOR) : -1;
  if (s_theme_color < -1 || s_theme_color >= FILL_PALETTE_SIZE)
    s_theme_color = -1;
  s_bg_color = s_theme_color >= 0 ?
    s_fill_palette[s_theme_color] :
    s_fill_palette[rand() % FILL_PALETTE_SIZE];
  // 色彩条纹：从随机起点取相邻色，确保配色和谐（步长 2 增加色域跨度）
  int stripe_base = rand() % STRIPE_PALETTE_SIZE;
  for (int i = 0; i < WAVE_LAYERS; i++) {
    s_wave_colors[i] = s_stripe_palette[(stripe_base + i * 2) % STRIPE_PALETTE_SIZE];
  }

  // Canvas 层
  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, canvas_draw);
  layer_add_child(root, s_canvas_layer);

  // Scroll 层位于主题标题栏下；扬声器平台再为 TTS 提示保留空间。
  int scroll_top = THEME_HEADER_HEIGHT + 4 + TTS_HINT_OFFSET;
  s_scroll_layer = scroll_layer_create(GRect(0, scroll_top, s_width, s_height - scroll_top));
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window);
  // 恢复窗口基础点击（覆盖 ScrollLayer 的默认行为以便响应 UP/DOWN）
  window_set_click_config_provider(window, click_config);

  // 禁用 ScrollLayer 上下阴影（dithered shadow）
  scroll_layer_set_shadow_hidden(s_scroll_layer, true);

  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));

  // 问题显示层 (滚动区顶部)
  s_question_display_layer = text_layer_create(GRect(10, 0, s_width - 20, 44));
  // C13 fix: 初始颜色用 GColorDarkGray 而非白色，避免白字白底不可见
  text_layer_set_text_color(s_question_display_layer, GColorDarkGray);
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

  // 初始化状态位和字体设置
  bool ready = persist_exists(PERSIST_KEY_READY) ? persist_read_bool(PERSIST_KEY_READY) : false;
  s_font_size = persist_exists(PERSIST_KEY_FONT_SIZE) ? (int)persist_read_int(PERSIST_KEY_FONT_SIZE) : 0;
  if (s_font_size > 2) s_font_size = 0;
  s_font_bold = persist_exists(PERSIST_KEY_FONT_BOLD) ? (int)persist_read_int(PERSIST_KEY_FONT_BOLD) : 0;
  s_disable_surprise = persist_exists(PERSIST_KEY_DISABLE_SURPRISE) ? (int)persist_read_int(PERSIST_KEY_DISABLE_SURPRISE) : 0;
  s_health_enabled = persist_exists(PERSIST_KEY_HEALTH_ENABLED) ? (int)persist_read_int(PERSIST_KEY_HEALTH_ENABLED) : 0;
  s_web_search = persist_exists(PERSIST_KEY_WEB_SEARCH) ? (int)persist_read_int(PERSIST_KEY_WEB_SEARCH) : 1;
  s_auto_dictation = persist_exists(PERSIST_KEY_AUTO_DICTATION) ? (int)persist_read_int(PERSIST_KEY_AUTO_DICTATION) : 0;
  s_health_days = persist_exists(PERSIST_KEY_HEALTH_DAYS) ? (int)persist_read_int(PERSIST_KEY_HEALTH_DAYS) : 7;
  if (s_health_days != 1 && s_health_days != 3 && s_health_days != 7) s_health_days = 7;
  s_health_sleep = persist_exists(PERSIST_KEY_HEALTH_SLEEP) ? (int)persist_read_int(PERSIST_KEY_HEALTH_SLEEP) : 1;
  s_duo_tts_enabled = persist_exists(PERSIST_KEY_DUO_TTS) ? (int)persist_read_int(PERSIST_KEY_DUO_TTS) : 0;
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  if (!s_tts_ring)
    s_tts_ring = malloc(TTS_RING_SIZE);
  if (!s_tts_pending_write_buf)
    s_tts_pending_write_buf = malloc(TTS_DECODE_BYTES);
  if (!s_tts_out_buf)
    s_tts_out_buf = malloc(TTS_DECODE_BYTES);
  s_tts_volume = persist_exists(PERSIST_KEY_TTS_VOLUME) ? (int)persist_read_int(PERSIST_KEY_TTS_VOLUME) : 100;
  s_tts_volume = tts_clamp_volume(s_tts_volume);
#endif
  apply_font_settings();
  set_state(ready ? STATE_IDLE_READY : STATE_IDLE_NO_KEY);
}

static void window_unload(Window *window) {
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  tts_stop();
  if (s_tts_pending_write_buf) {
    free(s_tts_pending_write_buf);
    s_tts_pending_write_buf = NULL;
  }
  if (s_tts_out_buf) {
    free(s_tts_out_buf);
    s_tts_out_buf = NULL;
  }
  if (s_tts_ring) {
    free(s_tts_ring);
    s_tts_ring = NULL;
  }
#endif
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
//   row 2     — 联网搜索开关（显示 ON/OFF，点击切换并同步给手机）
//   row 3     — TTS 音量（Emery / Flint，点击打开本地音量选择）
//   row 3/4+  — 历史对话列表（活跃对话有 Celeste 底色 + 蓝点标记）
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

static uint16_t menu_fixed_rows(void) {
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  return 4;
#else
  return 3;
#endif
}

static uint16_t menu_get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return menu_fixed_rows() + s_chat_count;
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

  // 高亮背景跟随本次进入主页时抽到的主题色；文字始终用白色保证对比。
  if (highlighted) {
    graphics_context_set_fill_color(ctx, C_MENU_HIGHLIGHT);
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
    // ── ToDo & Notes 入口 ──
    graphics_context_set_text_color(ctx, highlighted ? GColorWhite : C_ACCENT);
    GRect add_rect = GRect(0, bounds.size.h / 2 - 12, bounds.size.w, 24);
    graphics_draw_text(ctx, "ToDo & Notes", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       add_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }

  if (idx->row == 2) {
    // ── 联网搜索开关行：标签 + ON/OFF 状态 ──
    graphics_context_set_text_color(ctx, highlighted ? GColorWhite : C_ACCENT);
    int x_pad = PBL_IF_ROUND_ELSE(24, 10);
    GRect label_rect = GRect(x_pad, bounds.size.h / 2 - 11, bounds.size.w - x_pad * 2 - 50, 22);
    graphics_draw_text(ctx, "Web Search", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       label_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    const char *state_str = s_web_search ? "ON" : "OFF";
    graphics_context_set_text_color(ctx, highlighted ? GColorWhite : (s_web_search ? C_DOT_ON : C_DOT_OFF));
    GRect state_rect = GRect(bounds.size.w - x_pad - 40, bounds.size.h / 2 - 11, 40, 22);
    graphics_draw_text(ctx, state_str, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       state_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    return;
  }

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  if (idx->row == 3) {
    // ── TTS 音量行：标签 + 当前百分比 ──
    graphics_context_set_text_color(ctx, highlighted ? GColorWhite : C_ACCENT);
    int x_pad = PBL_IF_ROUND_ELSE(24, 10);
    GRect label_rect = GRect(x_pad, bounds.size.h / 2 - 11, bounds.size.w - x_pad * 2 - 58, 22);
    graphics_draw_text(ctx, "Volume", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       label_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    char vol_str[8];
    snprintf(vol_str, sizeof(vol_str), "%d%%", tts_clamp_volume(s_tts_volume));
    graphics_context_set_text_color(ctx, highlighted ? GColorWhite : C_DOT_ON);
    GRect state_rect = GRect(bounds.size.w - x_pad - 48, bounds.size.h / 2 - 11, 48, 22);
    graphics_draw_text(ctx, vol_str, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       state_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    return;
  }
#endif

  // ── 对话列表 ──
  int i = idx->row - menu_fixed_rows();
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
  graphics_context_set_text_color(ctx, highlighted ? GColorCeleste : (active ? C_ACCENT : GColorDarkGray));
  graphics_draw_text(ctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_14), sub_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void delayed_pop_timer(void *data) {
  window_stack_pop(true);
}

// Note open 需要连弹两层(operations → notes → 主界面),用第二次定时器串行,
// 避免在 MenuLayer 的 select 回调里同步 pop 自毁回调所属对象导致 UAF。
static void delayed_pop_twice_second(void *data) {
  window_stack_pop(true);
}
static void delayed_pop_twice_first(void *data) {
  window_stack_pop(false);  // 先弹 note_actions(无动画,立即 unload)
  app_timer_register(10, delayed_pop_twice_second, NULL);  // 再弹 notes 列表
}

static void menu_select_click(MenuLayer *menu, MenuIndex *idx, void *ctx) {
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  // 切换对话前停止 TTS（防止旧回复音频继续播放，含加载阶段）
  if (s_tts_playing || s_tts_loading) {
    tts_stop();
    tts_cancel_remote();  // 通知 JS 停止发送旧回复的 chunk
  }
#endif
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
    // 点击 Notes / TODOs 入口
    show_notes();
    return;
  }

  if (idx->row == 2) {
    // 点击联网搜索开关 → 切换并同步给手机（双路：手表改 localStorage 真值）
    s_web_search = s_web_search ? 0 : 1;
    persist_write_int(PERSIST_KEY_WEB_SEARCH, s_web_search);
    vibes_short_pulse();
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
      dict_write_uint8(iter, MESSAGE_KEY_SWITCH_WEB_SEARCH, (uint8_t)s_web_search);
      app_message_outbox_send();
    }
    menu_layer_reload_data(s_menu_layer);
    return;
  }

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  if (idx->row == 3) {
    show_volume_select();
    return;
  }
#endif

  // 点击历史对话
  int chat_idx = idx->row - menu_fixed_rows();
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

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_SWITCH_CHAT, s_chat_entries[chat_idx].id);
    app_message_outbox_send();
  }

  s_reply_buf[0] = '\0';
  s_question_display[0] = '\0';
  text_layer_set_text(s_reply_layer, "");
  text_layer_set_text(s_question_display_layer, "");

  set_state(STATE_IDLE_READY);

  app_timer_register(100, delayed_pop_timer, NULL);
}

// 菜单顶部主题标题栏
static Layer *s_menu_bar_layer = NULL;
static void menu_bar_draw(Layer *layer, GContext *ctx) {
  draw_theme_header(ctx, "Setting", layer_get_bounds(layer).size.w);
}

static void notes_bar_draw(Layer *layer, GContext *ctx) {
  draw_theme_header(ctx, "ToDo & Notes", layer_get_bounds(layer).size.w);
}

static void note_actions_bar_draw(Layer *layer, GContext *ctx) {
  draw_theme_header(ctx, "Note", layer_get_bounds(layer).size.w);
}

static void list_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  window_set_background_color(window, C_BG);

  s_menu_layer = menu_layer_create(
    GRect(0, THEME_HEADER_HEIGHT, bounds.size.w,
          bounds.size.h - THEME_HEADER_HEIGHT));
  menu_layer_set_callbacks(s_menu_layer, s_menu_layer, (MenuLayerCallbacks){
    .get_num_rows = menu_get_num_rows,
    .get_cell_height = menu_get_cell_height,
    .draw_row = menu_draw_row,
    .select_click = menu_select_click,
  });
  menu_layer_set_normal_colors(s_menu_layer, C_BG, C_TEXT_LIGHT);
  menu_layer_set_highlight_colors(
    s_menu_layer, C_MENU_HIGHLIGHT, GColorWhite);
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));

  s_menu_bar_layer =
    layer_create(GRect(0, 0, bounds.size.w, THEME_HEADER_HEIGHT));
  layer_set_update_proc(s_menu_bar_layer, menu_bar_draw);
  layer_add_child(root, s_menu_bar_layer);
}

static void list_window_unload(Window *window) {
  if (s_menu_bar_layer) {
    layer_destroy(s_menu_bar_layer);
    s_menu_bar_layer = NULL;
  }
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

static uint16_t notes_menu_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return s_note_count > 0 ? s_note_count : 1;
}

static void notes_menu_draw(GContext *ctx, const Layer *cell_layer,
                            MenuIndex *idx, void *data) {
  GRect b = layer_get_bounds(cell_layer);
  MenuLayer *menu = (MenuLayer *)data;
  MenuIndex selected = menu_layer_get_selected_index(menu);
  bool highlighted =
    selected.row == idx->row && selected.section == idx->section;
  if (highlighted) {
    graphics_context_set_fill_color(ctx, C_MENU_HIGHLIGHT);
    graphics_fill_rect(ctx, b, 0, GCornerNone);
  }
  if (s_note_count == 0) {
    graphics_context_set_text_color(ctx,
      highlighted ? GColorWhite : GColorDarkGray);
    graphics_draw_text(ctx, "No Items", fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       b, GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
    return;
  }
  WatchNoteEntry *note = &s_note_entries[idx->row];
  int x_pad = PBL_IF_ROUND_ELSE(24, 8);
  int dot_x = x_pad + 4;
  graphics_context_set_stroke_color(ctx,
    highlighted ? GColorWhite : (note->done ? GColorDarkGray : C_DOT_ON));
  graphics_context_set_fill_color(ctx,
    highlighted ? GColorWhite : (note->done ? GColorLightGray : C_DOT_ON));
  if (note->done) {
    graphics_draw_circle(ctx, GPoint(dot_x, b.size.h / 2), 5);
  } else {
    graphics_fill_circle(ctx, GPoint(dot_x, b.size.h / 2), 5);
  }
  int text_x = x_pad + 15;
  graphics_context_set_text_color(ctx,
    highlighted ? GColorWhite : (note->done ? GColorDarkGray : GColorBlack));
  graphics_draw_text(ctx, note->title,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(text_x, 2, b.size.w - text_x - 6, 22),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx,
    highlighted ? GColorCeleste : (note->done ? GColorDarkGray : C_ACCENT));
  graphics_draw_text(ctx, note->due[0] ? note->due :
                     (note->event ? "Event" :
                      (note->todo ? "TODO" : "Note")),
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(text_x, 21, b.size.w - text_x - 6, 20),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
}

static uint16_t note_action_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return 3;
}

static void note_action_draw(GContext *ctx, const Layer *cell_layer,
                             MenuIndex *idx, void *data) {
  MenuLayer *menu = (MenuLayer *)data;
  MenuIndex selected = menu_layer_get_selected_index(menu);
  bool highlighted =
    selected.row == idx->row && selected.section == idx->section;
  GRect bounds = layer_get_bounds(cell_layer);
  if (highlighted) {
    graphics_context_set_fill_color(ctx, C_MENU_HIGHLIGHT);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  }
  bool done = s_selected_note_index >= 0 &&
    s_selected_note_index < s_note_count &&
    s_note_entries[s_selected_note_index].done;
  const char *label = idx->row == 0 ? "Open Conversation" :
    (idx->row == 1 ? (done ? "Reopen" : "Complete") : "Delete");
  graphics_context_set_text_color(ctx,
    highlighted ? GColorWhite : (idx->row == 2 ? GColorRed : C_ACCENT));
  graphics_draw_text(ctx, label,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(8, bounds.size.h / 2 - 12,
                           bounds.size.w - 16, 24),
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

static void send_queued_note_command(void *data) {
  s_note_command_timer = NULL;
  if (!s_pending_note_command[0] &&
      persist_exists(PERSIST_KEY_NOTE_COMMAND)) {
    persist_read_string(PERSIST_KEY_NOTE_COMMAND, s_pending_note_command,
                        sizeof(s_pending_note_command));
  }
  if (!s_pending_note_command[0]) return;
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_cstring(iter, MESSAGE_KEY_NOTE_COMMAND,
                       s_pending_note_command);
    if (app_message_outbox_send() == APP_MSG_OK) return;
  }
  s_note_command_timer =
    app_timer_register(1500, send_queued_note_command, NULL);
}

static void queue_note_command(const char *command) {
  if (s_selected_note_index < 0 ||
      s_selected_note_index >= s_note_count) return;
  snprintf(s_pending_note_command, sizeof(s_pending_note_command), "%s|%s",
           command,
           s_note_entries[s_selected_note_index].id);
  persist_write_string(PERSIST_KEY_NOTE_COMMAND, s_pending_note_command);
  // Menu 回调只写本机持久存储。等窗口动画、按键派发和绘制全部空闲后再通信。
  if (s_note_command_timer) app_timer_cancel(s_note_command_timer);
  s_note_command_timer =
    app_timer_register(900, send_queued_note_command, NULL);
}

static void note_action_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (idx->row == 0) {
    queue_note_command("open");
    // 连弹两层回主界面。
    app_timer_register(10, delayed_pop_twice_first, NULL);
  } else if (idx->row == 1) {
    bool done = s_selected_note_index >= 0 &&
      s_selected_note_index < s_note_count &&
      s_note_entries[s_selected_note_index].done;
    queue_note_command(done ? "reopen" : "complete");
    // Notes 列表只接受手机端回传的完整快照，不在此回调中修改底层 MenuLayer。
    app_timer_register(10, delayed_pop_timer, NULL);
  } else {
    queue_note_command("delete");
    app_timer_register(10, delayed_pop_timer, NULL);
  }
}

static void note_actions_load(Window *window) {
  s_note_actions_visible = true;
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  window_set_background_color(window, C_BG);
  s_note_actions_menu_layer =
    menu_layer_create(GRect(0, THEME_HEADER_HEIGHT, b.size.w,
                            b.size.h - THEME_HEADER_HEIGHT));
  menu_layer_set_callbacks(s_note_actions_menu_layer,
      s_note_actions_menu_layer,
      (MenuLayerCallbacks){
        .get_num_rows = note_action_rows,
        .draw_row = note_action_draw,
        .select_click = note_action_select,
      });
  menu_layer_set_normal_colors(
    s_note_actions_menu_layer, C_BG, C_TEXT_LIGHT);
  menu_layer_set_highlight_colors(
    s_note_actions_menu_layer, C_MENU_HIGHLIGHT, GColorWhite);
  menu_layer_set_click_config_onto_window(s_note_actions_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_note_actions_menu_layer));
  s_note_actions_bar_layer =
    layer_create(GRect(0, 0, b.size.w, THEME_HEADER_HEIGHT));
  layer_set_update_proc(s_note_actions_bar_layer, note_actions_bar_draw);
  layer_add_child(root, s_note_actions_bar_layer);
}

static void note_actions_unload(Window *window) {
  s_note_actions_visible = false;
  if (s_note_actions_bar_layer) {
    layer_destroy(s_note_actions_bar_layer);
    s_note_actions_bar_layer = NULL;
  }
  menu_layer_destroy(s_note_actions_menu_layer);
  s_note_actions_menu_layer = NULL;
}

static void notes_window_appear(Window *window) {
  if (s_notes_menu_layer && s_notes_list_dirty) {
    MenuIndex selection = menu_layer_get_selected_index(s_notes_menu_layer);
    uint16_t rows = s_note_count > 0 ? s_note_count : 1;
    menu_layer_reload_data(s_notes_menu_layer);
    selection.section = 0;
    if (selection.row >= rows) selection.row = rows - 1;
    menu_layer_set_selected_index(s_notes_menu_layer, selection,
                                  MenuRowAlignNone, false);
    s_notes_list_dirty = false;
  }
}

static void notes_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (s_note_count == 0) return;
  s_selected_note_index = idx->row;
  if (!s_note_actions_window) {
    s_note_actions_window = window_create();
    window_set_window_handlers(s_note_actions_window, (WindowHandlers){
      .load = note_actions_load,
      .unload = note_actions_unload,
    });
  }
  window_stack_push(s_note_actions_window, true);
}

static void notes_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  window_set_background_color(window, C_BG);
  s_notes_menu_layer =
    menu_layer_create(GRect(0, THEME_HEADER_HEIGHT, b.size.w,
                            b.size.h - THEME_HEADER_HEIGHT));
  menu_layer_set_callbacks(s_notes_menu_layer, s_notes_menu_layer,
      (MenuLayerCallbacks){
    .get_num_rows = notes_menu_rows,
    .draw_row = notes_menu_draw,
    .select_click = notes_select,
  });
  menu_layer_set_normal_colors(s_notes_menu_layer, C_BG, C_TEXT_LIGHT);
  menu_layer_set_highlight_colors(
    s_notes_menu_layer, C_MENU_HIGHLIGHT, GColorWhite);
  menu_layer_set_click_config_onto_window(s_notes_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_notes_menu_layer));
  s_notes_bar_layer =
    layer_create(GRect(0, 0, b.size.w, THEME_HEADER_HEIGHT));
  layer_set_update_proc(s_notes_bar_layer, notes_bar_draw);
  layer_add_child(root, s_notes_bar_layer);
}

static void notes_window_unload(Window *window) {
  if (s_notes_bar_layer) {
    layer_destroy(s_notes_bar_layer);
    s_notes_bar_layer = NULL;
  }
  menu_layer_destroy(s_notes_menu_layer);
  s_notes_menu_layer = NULL;
}

static void show_notes(void) {
  if (!s_notes_window) {
    s_notes_window = window_create();
    window_set_window_handlers(s_notes_window, (WindowHandlers){
      .load = notes_window_load,
      .unload = notes_window_unload,
      .appear = notes_window_appear,
    });
  }
  window_stack_push(s_notes_window, true);
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
  menu_layer_set_highlight_colors(
    s_model_menu_layer, C_MENU_HIGHLIGHT, GColorWhite);
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

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
// ═══════════════════════════════════════════════════════════════════════════════
// TTS 音量选择子窗口 (MenuLayer)
// ═══════════════════════════════════════════════════════════════════════════════
static uint16_t volume_menu_get_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return (TTS_VOLUME_MAX - TTS_VOLUME_MIN) / TTS_VOLUME_STEP + 1;
}

static int volume_value_for_row(uint16_t row) {
  int value = TTS_VOLUME_MIN + (int)row * TTS_VOLUME_STEP;
  return tts_clamp_volume(value);
}

static uint16_t volume_row_for_value(int volume) {
  int clamped = tts_clamp_volume(volume);
  return (uint16_t)((clamped - TTS_VOLUME_MIN) / TTS_VOLUME_STEP);
}

static void volume_menu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *idx, void *data) {
  GRect bounds = layer_get_bounds(cell_layer);
  int value = volume_value_for_row(idx->row);
  bool active = (value == tts_clamp_volume(s_tts_volume));

  int x_offset = PBL_IF_ROUND_ELSE(24, 8);
  if (active) {
    graphics_context_set_fill_color(ctx, C_ACCENT);
    graphics_fill_circle(ctx, GPoint(x_offset, bounds.size.h / 2), 4);
  }

  char display[8];
  snprintf(display, sizeof(display), "%d%%", value);
  const char *sub = active ? "Current" : "Tap to set";

  int text_x = active ? x_offset + 10 : x_offset;
  GRect name_rect = GRect(text_x, 2, bounds.size.w - text_x - 4, 22);
  GRect sub_rect = GRect(text_x, 22, bounds.size.w - text_x - 4, 18);

  graphics_draw_text(ctx, display, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     name_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     sub_rect, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void volume_menu_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  s_tts_volume = volume_value_for_row(idx->row);
  persist_write_int(PERSIST_KEY_TTS_VOLUME, s_tts_volume);
  vibes_short_pulse();

  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
  }

  window_stack_pop(true);
}

static void volume_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  window_set_background_color(window, C_BG);

  s_volume_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_volume_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = volume_menu_get_num_rows,
    .draw_row = volume_menu_draw_row,
    .select_click = volume_menu_select,
  });
  menu_layer_set_normal_colors(s_volume_menu_layer, C_BG, C_TEXT_LIGHT);
  menu_layer_set_highlight_colors(
    s_volume_menu_layer, C_MENU_HIGHLIGHT, GColorWhite);
  menu_layer_set_click_config_onto_window(s_volume_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_volume_menu_layer));

  MenuIndex selected = { .section = 0, .row = volume_row_for_value(s_tts_volume) };
  menu_layer_set_selected_index(s_volume_menu_layer, selected, MenuRowAlignCenter, false);
}

static void volume_window_unload(Window *window) {
  menu_layer_destroy(s_volume_menu_layer);
  s_volume_menu_layer = NULL;
}

static void show_volume_select(void) {
  if (!s_volume_window) {
    s_volume_window = window_create();
    window_set_window_handlers(s_volume_window, (WindowHandlers){
      .load = volume_window_load,
      .unload = volume_window_unload,
    });
  }
  window_stack_push(s_volume_window, true);
}
#endif

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

static void copy_note_field(char *dest, size_t size,
                            const char *start, const char *end) {
  size_t len = end > start ? (size_t)(end - start) : 0;
  if (len >= size) len = size - 1;
  memcpy(dest, start, len);
  dest[len] = '\0';
}

static void parse_note_list(const char *data) {
  s_note_count = 0;
  if (!s_note_entries) {
    s_note_entries = calloc(MAX_WATCH_NOTES, sizeof(WatchNoteEntry));
    if (!s_note_entries) return;
  }
  const char *line = data;
  while (*line && s_note_count < MAX_WATCH_NOTES) {
    const char *end = strchr(line, '\n');
    if (!end) end = line + strlen(line);
    const char *p1 = strchr(line, '|');
    if (!p1 || p1 >= end) break;
    const char *p2 = strchr(p1 + 1, '|');
    if (!p2 || p2 >= end) break;
    const char *p3 = strchr(p2 + 1, '|');
    if (!p3 || p3 >= end) break;
    const char *p4 = strchr(p3 + 1, '|');
    if (!p4 || p4 >= end) break;
    WatchNoteEntry *note = &s_note_entries[s_note_count++];
    copy_note_field(note->id, sizeof(note->id), line, p1);
    note->done = p1[1] == '1';
    note->todo = p2[1] == 'T';
    note->event = p2[1] == 'E';
    copy_note_field(note->due, sizeof(note->due), p3 + 1, p4);
    copy_note_field(note->title, sizeof(note->title), p4 + 1, end);
    line = *end ? end + 1 : end;
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
  // C12 fix: srand 已在 window_load 中调用，此处移除重复调用

  s_note_entries = calloc(MAX_WATCH_NOTES, sizeof(WatchNoteEntry));
  s_note_wakeups = calloc(MAX_NOTE_WAKEUPS, sizeof(NoteWakeupSlot));
  if (s_note_wakeups) {
    for (int i = 0; i < MAX_NOTE_WAKEUPS; i++) {
      int key = PERSIST_NOTE_WAKEUP_BASE + i;
      if (persist_exists(key) &&
          persist_get_size(key) == (int)sizeof(NoteWakeupSlot))
        persist_read_data(key, &s_note_wakeups[i], sizeof(NoteWakeupSlot));
    }
  }
  // v1.4.2 早期版本曾允许第 8 个 Note Wakeup。该槽现预留给同步；
  // 升级时取消遗留闹钟，避免一个不可管理的旧提醒继续占用 OS 资源。
  if (persist_exists(PERSIST_NOTE_WAKEUP_BASE + MAX_NOTE_WAKEUPS) &&
      persist_get_size(PERSIST_NOTE_WAKEUP_BASE + MAX_NOTE_WAKEUPS) ==
        (int)sizeof(NoteWakeupSlot)) {
    NoteWakeupSlot legacy_slot;
    persist_read_data(PERSIST_NOTE_WAKEUP_BASE + MAX_NOTE_WAKEUPS,
                      &legacy_slot, sizeof(legacy_slot));
    if (legacy_slot.wakeup_id >= 0) wakeup_cancel(legacy_slot.wakeup_id);
    persist_delete(PERSIST_NOTE_WAKEUP_BASE + MAX_NOTE_WAKEUPS);
  }
  if (persist_exists(PERSIST_SYNC_WAKEUP_ID))
    s_sync_wakeup_id = (WakeupId)persist_read_int(PERSIST_SYNC_WAKEUP_ID);
  wakeup_service_subscribe(note_wakeup_handler);

  s_launch_reason = launch_reason();
  s_launch_args = launch_get_args();
  const char *reason_name = "system";
  switch (s_launch_reason) {
    case APP_LAUNCH_USER: reason_name = "user"; break;
    case APP_LAUNCH_PHONE: reason_name = "phone"; break;
    case APP_LAUNCH_WAKEUP: reason_name = "wakeup"; break;
    case APP_LAUNCH_WORKER: reason_name = "worker"; break;
    case APP_LAUNCH_QUICK_LAUNCH: reason_name = "quick_launch"; break;
    case APP_LAUNCH_TIMELINE_ACTION:
      reason_name = "timeline_action";
      s_force_launch_dictation = true;
      break;
    case APP_LAUNCH_SMARTSTRAP: reason_name = "smartstrap"; break;
    default: break;
  }
  // Auto Dictation is a one-shot convenience for deliberate fast entries
  // (Quick Launch shortcut or a Timeline action). A normal open from the app
  // menu/list must not grab the microphone.
  s_quick_launch_entry = (s_launch_reason == APP_LAUNCH_QUICK_LAUNCH ||
                          s_launch_reason == APP_LAUNCH_TIMELINE_ACTION);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "Launch: reason=%s quick_entry=%d force_dict=%d auto_dict(persist)=%d",
          reason_name, (int)s_quick_launch_entry,
          (int)s_force_launch_dictation,
          (int)(persist_exists(PERSIST_KEY_AUTO_DICTATION) ?
                persist_read_int(PERSIST_KEY_AUTO_DICTATION) : 0));
  snprintf(s_launch_context, sizeof(s_launch_context), "%s|%lu",
           reason_name, (unsigned long)s_launch_args);
  if (s_launch_reason == APP_LAUNCH_WAKEUP) {
    WakeupId launch_wakeup_id;
    int32_t launch_cookie;
    if (wakeup_get_launch_event(&launch_wakeup_id, &launch_cookie)) {
      s_launch_args = (uint32_t)launch_cookie;
      snprintf(s_launch_context, sizeof(s_launch_context), "wakeup|%lu",
               (unsigned long)s_launch_args);
      if (launch_cookie == SYNC_WAKEUP_COOKIE) {
        s_sync_wakeup_launch = true;
        utf8_safe_copy(s_sync_status, sizeof(s_sync_status), "Syncing...");
        s_sync_wakeup_id = -1;
        persist_delete(PERSIST_SYNC_WAKEUP_ID);
      } else {
        note_wakeup_alert(launch_cookie);
      }
    }
  }

  // 注册 AppMessage 回调
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_sent(outbox_sent);
  app_message_register_outbox_failed(outbox_failed);
  // 使用最大缓冲区（Basalt: inbox 8200 / outbox 2048）
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());
  app_timer_register(2500, clear_startup_message_guard, NULL);
  if (persist_exists(PERSIST_KEY_NOTE_COMMAND)) {
    // Older 1.4.2 test builds could leave a pending Note command in watch
    // persistent storage after a crash/retry. Replaying it during app startup
    // is unsafe on real watches because the phone JS side may not be ready yet,
    // and it can also repeat stale destructive actions. Local note state has
    // already been updated on the watch, so drop the stale command and wait for
    // the phone to resend NOTE_LIST during the normal ready handshake.
    persist_delete(PERSIST_KEY_NOTE_COMMAND);
    s_pending_note_command[0] = '\0';
  }

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
  if (s_auto_dictation_timer) app_timer_cancel(s_auto_dictation_timer);
  if (s_dictation_retry_timer) app_timer_cancel(s_dictation_retry_timer);
  if (s_note_command_timer) app_timer_cancel(s_note_command_timer);
  if (s_sync_wakeup_timer) app_timer_cancel(s_sync_wakeup_timer);
  if (s_dictation_session) dictation_session_destroy(s_dictation_session);
  if (s_pending_question) {
    free(s_pending_question);
    s_pending_question = NULL;
  }
  if (s_note_entries) {
    free(s_note_entries);
    s_note_entries = NULL;
  }
  if (s_note_wakeups) {
    free(s_note_wakeups);
    s_note_wakeups = NULL;
  }
  if (s_health_data) {
    free(s_health_data);
    s_health_data = NULL;
  }
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  tts_stop();
#endif

  // C4 fix: 先从 window stack 移除再销毁，防止 stack 上悬空指针导致崩溃
  if (s_model_window) {
    window_stack_remove(s_model_window, false);
    window_destroy(s_model_window);
  }
  if (s_note_actions_window) {
    window_stack_remove(s_note_actions_window, false);
    window_destroy(s_note_actions_window);
  }
  if (s_notes_window) {
    window_stack_remove(s_notes_window, false);
    window_destroy(s_notes_window);
  }
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_FLINT)
  if (s_volume_window) {
    window_stack_remove(s_volume_window, false);
    window_destroy(s_volume_window);
  }
#endif
  if (s_list_window) {
    window_stack_remove(s_list_window, false);
    window_destroy(s_list_window);
  }
  window_stack_remove(s_window, false);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
