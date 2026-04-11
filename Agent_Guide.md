# Pebble On Wrist AI — Agent Guide

> **目标读者**: AI 编程助手（Claude Code 等）。本文档帮助你在接手项目时快速建立全局理解，避免反复踩坑。

---

## 1. 项目概览

**Pebble On Wrist AI (PWAI)** 是一款运行在 Pebble 智能手表上的 AI 对话应用。用户通过手表语音输入问题，手机端通过蓝牙接收并转发给 OpenRouter API，收到 AI 回复后回传到手表上显示。

### 通讯链路

```
[手表 C] ←—— AppMessage (蓝牙) ——→ [手机 PebbleKit JS] ←—— HTTPS ——→ [OpenRouter API]
```

### 核心功能

- 语音转文字 → AI 问答（Dictation API）
- 多对话管理（创建/切换/删除）
- 多模型切换（最多 5 个模型）
- LLM 自动生成对话标题
- Web 配置页面（API Key、System Prompt、模型列表）
- 对话导出

---

## 2. 项目结构

```
pebble-app/
├── package.json           # Pebble 项目配置（UUID、messageKeys、平台列表）
├── wscript                # Pebble 构建脚本
├── Agent_Guide.md         # ← 你正在看的文件
├── README.md
├── LICENSE
│
├── src/
│   ├── c/
│   │   └── mdbl.c         # 【核心】手表端全部 C 代码（~1330 行）
│   │                       #   状态机、UI 绘制、动画、菜单、AppMessage 处理
│   └── pkjs/
│       └── index.js        # 【核心】手机端 PebbleKit JS 代码（~640 行）
│                           #   多对话存储、OpenRouter API、分块传输、配置处理
│
├── config/
│   └── index.html          # Web 配置页面（在 Pebble App 内打开的 WebView）
│                           #   托管在 GitHub Pages: deusaw.github.io/Pebble-Wrist-AI/config/
│
└── resources/              # 资源文件（目前为空，备用）
```

### ⚠️ 重要：只有两个核心代码文件

| 文件 | 运行环境 | 语言 | 大小 |
|------|---------|------|------|
| `src/c/mdbl.c` | Pebble 手表 | C (Pebble SDK 3) | ~1330 行 |
| `src/pkjs/index.js` | 手机后台 | JavaScript (ES5) | ~640 行 |

**配置页面** `config/index.html` 是独立的静态 HTML，托管在 GitHub Pages 上。修改后需要 push 到 GitHub 才能生效（手机 WebView 实时加载远程 URL）。

---

## 3. 技术约束（非常重要）

### PebbleKit JS 引擎限制

| 约束 | 说明 |
|------|------|
| **ES5 Only** | 禁止 `let`/`const`、箭头函数、模板字符串、`Promise`、`class`、`for...of`、解构赋值 |
| **无 `normalize()`** | `String.prototype.normalize()` 不存在，会直接崩溃 |
| **XHR 流式崩溃** | `XMLHttpRequest` 的 `onprogress` 中读取 `responseText` 导致 JS 引擎崩溃 |
| **XHR 非 ASCII 发送失败** | `xhr.send()` 可能按 Latin-1 编码请求体，中文等非 ASCII 字符导致请求畸形 |
| **单线程** | 没有 Web Worker，所有逻辑串行执行 |

### Pebble SDK (C) 限制

| 约束 | 说明 |
|------|------|
| **RAM 24KB** (Basalt) | 所有变量、UI 元素、缓冲区共享这 24KB |
| **AppMessage 单次 ~256 字节** | 蓝牙传输有效载荷限制，长文本必须分块 |
| **无动态内存析构跟踪** | `malloc` 后忘记 `free` = 永久泄漏直到 App 退出 |
| **无浮点运算** | 只有整数运算，须用整数模拟比例/缓动 |
| **ScrollLayer 阴影** | `scroll_layer_set_shadow_hidden()` 是唯一有效的关闭方式，`content_indicator_configure_direction(NULL)` 无效 |

### 构建环境

- **CloudPebble**: 在 `cloudpebble.repebble.com` 在线编译
- **SDK 版本**: Pebble SDK 3
- **目标平台**: basalt（主要）, chalk, diorite, emery
- **不能本地构建**: 依赖 Pebble 交叉编译工具链，现已不再官方维护

---

## 4. 状态机（mdbl.c 的灵魂）

```
                         ┌──────────────── 长按 SELECT ────────────────┐
                         │                                            │
                         ▼                                            │
┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ IDLE_NO_KEY  │───▶│ IDLE_READY   │───▶│  RECORDING   │───▶│   SENDING    │
│  (灰圆)      │    │  (蓝圆)      │    │  (跳动点)     │    │  (箭头)      │
└──────────────┘    └──────────────┘    └──────────────┘    └──────────────┘
                         ▲                                        │
                         │                                   800ms 延迟
                    按 SELECT                                     │
                         │                                        ▼
                    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
                    │  EXPANDING   │◀──│  RESPONSE    │◀──│  SHRINKING   │
                    │  (放大动画)   │    │  (文字滚动)   │    │  (缩小动画)   │
                    └──────────────┘    └──────────────┘    └──────────────┘
                                                                  ▲
                                                                  │
                                                           ┌──────────────┐
                                                           │   THINKING   │
                                                           │  (呼吸圆)     │
                                                           └──────────────┘
```

### 状态转换规则

| 当前状态 | 触发条件 | 目标状态 |
|---------|---------|---------|
| IDLE_NO_KEY | 收到 READY_STATUS=1 | IDLE_READY |
| IDLE_READY | 短按 SELECT | 启动 Dictation → RECORDING（由系统回调） |
| RECORDING | Dictation 成功 | SENDING |
| SENDING | 800ms 定时器 | THINKING |
| THINKING | 收到 REPLY_CHUNK / REPLY_END | SHRINKING |
| THINKING | 90s 超时 | SHRINKING（显示超时信息） |
| SHRINKING | 动画完成（18帧） | RESPONSE |
| RESPONSE | 短按 SELECT | EXPANDING |
| EXPANDING | 动画完成（18帧） | 立即启动 Dictation |
| 任何状态 | 长按 SELECT | IDLE_READY（新建对话） |

---

## 5. AppMessage 协议

### 手表 → 手机

| Message Key | 类型 | 含义 |
|------------|------|------|
| `QUESTION` | string | 用户语音转文字的问题 |
| `NEW_CHAT` | uint8 | 新建对话请求 |
| `SWITCH_CHAT` | string | 切换到指定对话 ID（空字符串 = 新建） |
| `SWITCH_MODEL` | string | 切换到指定模型全名 |

### 手机 → 手表

| Message Key | 类型 | 含义 |
|------------|------|------|
| `READY_STATUS` | int32 | API Key 状态（1=有 / 0=无） |
| `REPLY` | string | 完整 AI 回复（兜底） |
| `REPLY_CHUNK` | string | AI 回复分块（≤256字节） |
| `REPLY_END` | int | 回复结束标记 |
| `STATUS` | string | 错误信息 |
| `CHAT_LIST` | string | 对话列表 `"id1\|title1\nid2\|title2\n..."` |
| `SWITCH_CHAT` | string | 当前活跃对话 ID |
| `MODEL_NAME` | string | 模型列表 `"idx\|model1\nmodel2\nmodel3"` |

### 分块传输流程

```
手机: REPLY_CHUNK("这是第一段文字这是第一段文字这...") → 256 bytes
手机: REPLY_CHUNK("这是第二段文字...")                 → 剩余内容
手机: REPLY_END(1)                                    → 结束标记
```

手表端在 `inbox_received` 中用 `strncat` 追加到 `s_reply_buf`（2048字节），收到 `REPLY_END` 后切换状态。

---

## 6. 内存布局

```
总可用 RAM: ~24KB (Basalt)

┌──────────────────────────────────┐
│ s_reply_buf         2048 字节    │  AI 回复缓冲区
│ s_question_display   128 字节    │  问题显示缓冲区
│ s_chat_entries      1120 字节    │  20 × 56B 对话列表
│ s_model_names        240 字节    │  5 × 48B 模型名
│ 各种 int/指针/状态    ~200 字节    │
│ UI 元素 (Window, Layer, etc)     │  ~2KB
│ SDK 内部使用                      │  ~4KB
├──────────────────────────────────┤
│ 可用余量              ~14KB       │
└──────────────────────────────────┘
```

> ⚠️ `s_reply_buf` 定义为 2048 字节，但 JS 端的 `MAX_WATCH_CHARS` 限制为 1800 字符，留出安全余量。

---

## 7. UI 系统

### 配色方案

所有颜色在 `mdbl.c` 顶部统一定义为宏，**全局共用**：

| 宏名 | Pebble 颜色 | 用途 |
|------|------------|------|
| `C_BG` | White | 全局背景 |
| `C_CIRCLE` | PictonBlue | 主圆圈 |
| `C_CIRCLE_DIM` | LightGray | 未就绪时的圆圈 |
| `C_ACCENT` | Blue | 强调色（标题、活跃标记） |
| `C_ACCENT_DIM` | PictonBlue | 呼吸光环外环 |
| `C_TEXT_DARK` | White | 圆圈上的文字 |
| `C_TEXT_LIGHT` | Black | 白底上的文字 |
| `C_SUBTITLE` | DarkGray | 副标题 |
| `C_DOT_ON` | Blue | 活跃圆点 |
| `C_DOT_OFF` | LightGray | 非活跃圆点 |

### 字体规则

| 上下文 | 字体 | 粗体? |
|-------|------|-------|
| 主界面标题/按钮 | GOTHIC_24_BOLD / 18_BOLD / 14_BOLD | ✅ 粗体 |
| 菜单对话标题 | GOTHIC_18_BOLD | ✅ 粗体 |
| 菜单副标题 (Active / Tap to switch) | GOTHIC_14 | ❌ 非粗体 |
| 用户问题显示 | GOTHIC_14 | ❌ 非粗体 |
| AI 回复文本 | GOTHIC_18 | ❌ 非粗体 |

### 动画帧率

| 状态 | 帧率 | 原因 |
|------|------|------|
| IDLE_NO_KEY / IDLE_READY | 10fps (100ms) | 省电，5 秒后自动停止 |
| RECORDING / SENDING / THINKING | 20fps (50ms) | 流畅但不耗电 |
| SHRINKING / EXPANDING | 40fps (25ms) | 过渡动画需要流畅 |

### Thinking 动画参数

- **圆半径**: `s_circle_r_big - 4`（比正常小 4px）
- **呼吸幅度**: ±4px，24 帧一个周期
- **旋转小点**: 12 个固定位置，2 个"头部"点变大变蓝，每帧旋转 15°
- **小点不跟随呼吸**: spinner 用固定的 `s_circle_r_big - 4` 作为轨道半径

---

## 8. 常见踩坑点 🚨

### 8.1 PebbleKit JS 崩溃

**症状**: 手表显示 "Thinking..." 后永久卡死，日志无任何回复。

**原因**: PebbleKit JS 引擎在以下情况会静默崩溃：
- 使用 `xhr.onprogress` 读取 `responseText`（流式解析）
- 调用 `String.prototype.normalize()`
- 使用 ES6 语法

**解决方案**: 
- ✅ 使用 `stream: false` 的非流式请求
- ✅ 用 `sanitizePebbleText()` 正则替换代替 `normalize()`
- ✅ 全程使用 ES5 语法

### 8.2 中文 System Prompt 导致请求失败

**症状**: 设置中文 System Prompt 后，所有请求超时，OpenRouter 日志无记录。

**原因**: PebbleKit JS 的 `xhr.send()` 可能按 Latin-1 编码请求体，中文字节变成乱码。

**解决方案**: 使用 `safeJsonStringify()` 将所有非 ASCII 字符转义为 `\uXXXX`：
```javascript
function safeJsonStringify(obj) {
  var str = JSON.stringify(obj);
  return str.replace(/[\u0080-\uFFFF]/g, function(c) {
    return '\\u' + ('0000' + c.charCodeAt(0).toString(16)).slice(-4);
  });
}
```

### 8.3 ScrollLayer 阴影去不掉

**症状**: 回复页面顶部/底部有丑陋的 dithered shadow（锯齿阴影）。

**错误尝试**: `content_indicator_configure_direction(indicator, ..., NULL)` — **无效**！

**正确方法**:
```c
scroll_layer_set_shadow_hidden(s_scroll_layer, true);
```

### 8.4 对话 ID 截断导致状态不同步

**症状**: 切换对话后手表 Active 标记不正确，对话列表混乱。

**原因**: C 端的 `CHAT_ID_LEN` 太小（曾经只有 12），而 JS 生成的 ID 约 13 字符。`strncpy` 截断后 ID 不匹配。

**当前状态**: 已修复，`CHAT_ID_LEN = 20`，留充足余量。

### 8.5 长按 SELECT 无法新建对话

**症状**: 在 IDLE_READY 状态，有活跃对话时，长按 SELECT 无反应（只有震动）。

**原因**: 旧逻辑检查 `s_reply_buf` 是否为空来判断"已是空白新对话"。但回到 IDLE_READY 时 buffer 总是被清空的，即使当前是有内容的旧对话。

**修复**: 改为同时检查：buffer 为空 + 对话标题为 "New chat" + 对话在列表中存在。

### 8.6 文本乱码（â☐☐）

**症状**: AI 回复中出现 `â€™`、`â€œ` 等乱码字符。

**原因**: LLM 返回的智能引号（U+2019 '）等 3 字节 UTF-8 字符，被 PebbleKit JS 的 XHR 引擎按 Latin-1 解码，拆成 3 个独立字符（â, €, ™）。

**解决方案**: `sanitizePebbleText()` 中同时匹配：
1. 标准 Unicode 码点 (`\u2018`, `\u2019` 等)
2. Raw UTF-8 字节序列 (`\u00E2\u0080\u0099` 等)

### 8.7 AppMessage 发送失败/乱序

**症状**: 连续发送多条 AppMessage 时部分丢失。

**原因**: Pebble 蓝牙不支持并发发送，必须等上一条确认后再发下一条。

**解决方案**: 使用 `amQueue` 队列 + `processAmQueue()` 串行发送，失败自动重试 3 次。

### 8.8 CloudPebble 红色日志

**症状**: 控制台大量红色错误信息。

**实际情况**: 这些是 Pebble SDK 内部的 warning/info，**不是代码 bug**：
- `Timer XXX does not exist` — 动画定时器竞态（SDK 已知问题）
- `Animation XXXXXXX does not exist` — 同上
- `text_layer...must be attached to` — 圆屏适配顺序问题
- `essage_outbox/inbox` — AppMessage 缓冲区大小提示

**不需要修复**，也改不了（SDK 层面的）。

### 8.9 配置页面修改不生效

**症状**: 修改 `config/index.html` 后，手机端打开配置页仍是旧版本。

**原因**: 配置页面 URL 指向 GitHub Pages（`deusaw.github.io/Pebble-Wrist-AI/config/`），不是本地文件。

**解决**: 必须 `git push` 到 GitHub，等 GitHub Pages 部署完成（通常 1-2 分钟）。

### 8.10 回复过长导致崩溃

**症状**: 长回复导致手表端崩溃或显示异常。

**防护机制**:
- JS 端: `MAX_WATCH_CHARS = 1800`，超出自动截断 + `[...truncated]`
- C 端: `s_reply_buf` = 2048 字节
- 存储端: 每条消息限 800 字符，每对话限 20 条

---

## 9. 修改指南

### 修改 C 代码 (mdbl.c)

1. 在 CloudPebble (`cloudpebble.repebble.com`) 上编辑和构建
2. 测试时使用 Emulator 或连接真机
3. `git push` 同步代码到 GitHub

### 修改 JS 代码 (index.js)

1. 同样在 CloudPebble 上编辑
2. JS 代码在手机端运行，需要真机测试（Emulator 有限支持）
3. 调试使用 `console.log()`，输出在 CloudPebble 日志面板

### 修改配置页面 (config/index.html)

1. 本地编辑文件
2. `git push` 到 GitHub
3. 等待 GitHub Pages 部署
4. 在手机 Pebble App 中打开配置页面测试

### 添加新的 Message Key

1. 在 `package.json` 的 `messageKeys` 数组中添加
2. 在 C 代码中使用 `MESSAGE_KEY_YOUR_KEY`
3. 在 JS 代码中使用 `e.payload['YOUR_KEY']`

---

## 10. 代码约定

### C 代码 (mdbl.c)

- 全局变量以 `s_` 前缀命名
- 常量用 `#define`，全大写加下划线
- 颜色宏以 `C_` 前缀
- 函数注释用中文
- 圆形/矩形屏适配用 `PBL_IF_ROUND_ELSE(round_val, rect_val)`

### JS 代码 (index.js)

- 使用 `var`（不是 `let`/`const`）
- 使用 `function` 关键字（不是箭头函数）
- 用 `+` 拼接字符串（不是模板字符串）
- 注释用中文（section 标题用 ═ 分隔线）

### 通用

- 注释语言：中文
- Git 提交信息：中文
- 所有数字常量必须有注释说明含义

---

## 11. 快速参考

### 关键函数索引

#### mdbl.c

| 函数 | 行号范围 | 作用 |
|------|---------|------|
| `canvas_draw` | ~275 | 主绘制函数，根据状态绘制不同 UI |
| `anim_tick` | ~385 | 动画帧回调，处理所有动画 |
| `set_state` | ~490 | 状态机切换，管理 UI 可见性和动画 |
| `inbox_received` | ~575 | AppMessage 接收入口 |
| `select_handler` | ~695 | SELECT 按钮处理 |
| `select_long_handler` | ~710 | 长按 SELECT 新建对话 |
| `down_long_handler` | ~770 | 彩蛋（随机问题） |
| `menu_draw_row` | ~920 | 菜单行绘制（模型/新建/对话） |
| `parse_chat_list` | ~1220 | 解析手机发来的对话列表 |
| `parse_model_list` | ~1250 | 解析手机发来的模型列表 |

#### index.js

| 函数 | 作用 |
|------|------|
| `sanitizePebbleText` | 清洗 LLM 返回文本中的不可显示字符 |
| `safeJsonStringify` | 将非 ASCII 转义为 `\uXXXX`，解决 XHR 编码问题 |
| `loadStore/saveStore` | 对话数据持久化 (localStorage) |
| `sendToWatch` | 排队发送 AppMessage |
| `sendChatList` | 向手表发送对话列表 |
| `sendModelList` | 向手表发送模型列表 |
| `generateTitle` | 调用 LLM 自动生成对话标题 |
| `askAI` | 核心 API 调用（非流式） |
| `sendReadyStatus` | 告知手表 API Key 状态 |

---

## 12. 已知限制（不需要修）

1. **对话历史不持久**: 手表端重启后不保留对话内容（仅保留标题列表），完整历史在手机 `localStorage` 中
2. **单语言 UI**: 界面文字为英文硬编码（"Thinking...", "Listening...", "+ START NEW CHAT" 等）
3. **无图片/表情**: Pebble 屏幕只显示纯文本
4. **Dictation 仅限英文**: Pebble 语音识别仅支持英语
5. **对话不可编辑**: 无法在手表上编辑已发送的消息
