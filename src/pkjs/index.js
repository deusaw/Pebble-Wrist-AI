// ═══════════════════════════════════════════════════════════════════════════════
// PEBBLEKIT JS — 运行在手机端 (PebbleKit JS 是 ES5 引擎，禁止使用 ES6+)
//
// 架构概览:
//   手表 (C) ←→ AppMessage (蓝牙) ←→ 手机 (PebbleKit JS) ←→ OpenRouter API
//
// 本文件职责：
//   1. 管理多对话存储 (localStorage)
//   2. 向 OpenRouter API 发送非流式请求并解析回复
//   3. 自动调用 LLM 为新对话生成短标题
//   4. 通过 AppMessage 与手表端通讯（分块传输、队列管理）
//   5. 处理配置页面的设置读写
// ═══════════════════════════════════════════════════════════════════════════════

var MAX_MESSAGES_PER_CHAT = 20; // 每个对话最多保留的消息条数（防 localStorage 过大）
var MAX_CHATS = 20;             // 对话槽位上限（超出时淘汰最旧的）
var MAX_WATCH_CHATS = 20;       // 手表端菜单最多显示的历史对话数
var MAX_CONFIG_CHATS = 20;      // 配置页面传送的对话数（与手表菜单一致，元数据精简可承受 URL 长度）

// 默认 System Prompt：强制简洁回复、禁止 Markdown，适应手表小屏幕
var DEFAULT_PROMPT = "You are Pebble Wrist AI, a concise assistant on a Pebble smartwatch. Rules: 1) Reply ONLY with the final answer, no reasoning or thinking process. 2) Keep responses under 400 words in English, or under 400 Chinese characters. 3) No markdown, no bullet lists. 4) Use plain sentences only.";

// ═══════════════════════════════════════════════════════════════════════════════
// 文本清洗
//
// Pebble 显示不了大部分 Unicode 特殊标点（智能引号、破折号、省略号等），
// LLM 返回的文本里经常包含这些字符，在手表上会渲染为乱码。
//
// 注意：PebbleKit JS 的 XHR 引擎可能不按 UTF-8 解码响应，
// 导致 3 字节 UTF-8 序列（如 E2 80 99 = U+2019 '）拆成 3 个 Latin-1 字符，
// 因此除了标准 Unicode 替换，还需要匹配 raw UTF-8 字节序列作为兜底。
// ═══════════════════════════════════════════════════════════════════════════════
function sanitizePebbleText(text) {
  if (!text) return '';
  return text
    .replace(/\*\*/g, '')
    .replace(/###+/g, '')
    .replace(/[\u2018\u2019\u0060]/g, "'")
    .replace(/[\u201C\u201D]/g, '"')
    .replace(/[\u2013\u2014\u2015]/g, '-')
    .replace(/\u2026/g, '...')
    .replace(/[\u2022\u2023\u25E6]/g, '-')
    .replace(/[\u00A0]/g, ' ')
    .replace(/[\u200B\u200C\u200D\uFEFF]/g, '')
    .replace(/\u00E2\u0080\u0098/g, "'")
    .replace(/\u00E2\u0080\u0099/g, "'")
    .replace(/\u00E2\u0080\u009C/g, '"')
    .replace(/\u00E2\u0080\u009D/g, '"')
    .replace(/\u00E2\u0080\u0093/g, '-')
    .replace(/\u00E2\u0080\u0094/g, '-')
    .replace(/\u00E2\u0080\u00A6/g, '...')
    .replace(/\u00E2\u0080[\u0090-\u00BF]/g, '');
}

// PebbleKit JS 的 XHR 可能无法正确发送非 ASCII 字符（如中文 system prompt），
// 将所有非 ASCII 转义为 JSON \uXXXX 格式，确保请求体纯 ASCII。
function safeJsonStringify(obj) {
  var str = JSON.stringify(obj);
  return str.replace(/[\u0080-\uFFFF]/g, function(c) {
    return '\\u' + ('0000' + c.charCodeAt(0).toString(16)).slice(-4);
  });
}

// ═══════════════════════════════════════════════════════════════════════════════
// 数据层：多对话存储
//
// 数据结构 (localStorage key: 'chat_store'):
//   { active_id: string|null, chats: [{id, title, created, messages: [{role, content}]}] }
// ═══════════════════════════════════════════════════════════════════════════════

// 生成唯一 ID：时间戳(base36) + 随机数(base36)，总长约 13 字符
function generateId() {
  return Date.now().toString(36) + Math.random().toString(36).substring(2, 7);
}

function loadStore() {
  try {
    var raw = localStorage.getItem('chat_store');
    if (raw) return JSON.parse(raw);
  } catch (e) {}
  return { active_id: null, chats: [] };
}

function saveStore(store) {
  if (store.chats.length > MAX_CHATS) {
    store.chats = store.chats.slice(store.chats.length - MAX_CHATS);
    // 如果活跃对话被截掉，改为最新的
    var found = false;
    for (var i = 0; i < store.chats.length; i++) {
      if (store.chats[i].id === store.active_id) { found = true; break; }
    }
    if (!found && store.chats.length > 0) {
      store.active_id = store.chats[store.chats.length - 1].id;
    }
  }
  localStorage.setItem('chat_store', JSON.stringify(store));
}

function getActiveChat(store) {
  if (!store.active_id) return null;
  for (var i = 0; i < store.chats.length; i++) {
    if (store.chats[i].id === store.active_id) return store.chats[i];
  }
  return null;
}

function createNewChat(store) {
  var chat = {
    id: generateId(),
    title: 'New chat',
    created: Date.now(),
    messages: []
  };
  store.chats.push(chat);
  store.active_id = chat.id;
  saveStore(store);
  return chat;
}

function deleteChat(store, chatId) {
  store.chats = store.chats.filter(function(c) { return c.id !== chatId; });
  if (store.active_id === chatId) {
    store.active_id = store.chats.length > 0
      ? store.chats[store.chats.length - 1].id
      : null;
  }
  saveStore(store);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 设置
// ═══════════════════════════════════════════════════════════════════════════════
function getSetting(key, defaultVal) {
  return localStorage.getItem(key) || defaultVal;
}

// API 模式：'openrouter'（默认）或 'custom'
function getApiEndpoint() {
  var mode = getSetting('api_mode', 'openrouter');
  if (mode === 'custom') {
    return getSetting('custom_api_url', '');
  }
  return 'https://openrouter.ai/api/v1/chat/completions';
}

function isOpenRouter() {
  return getSetting('api_mode', 'openrouter') !== 'custom';
}

// 全局自增会话ID：每次新请求 +1，旧请求的回调通过比对此值判断是否已过期
var currentAskSessionId = 0;

// ── AppMessage 发送信使（严格排队机制）────────────────────────────────────────
// 解决 Pebble 原生单次通讯不能超过 256 字节的物理缺陷。此处设立指令队列，确保分块传输时不会丢包或乱序
var amQueue = [];
var amSending = false;
var AM_RETRY_DELAY_MS = 500;
var TTS_STREAM_RETRY_DELAY_MS = 120; // TTS 流消息失败多为蓝牙/收件箱瞬时忙，500ms 会直接变成可闻大空洞
var TTS_STREAM_MAX_RETRIES = 80;     // 约 9.6s 内不丢音频块；宁可等待也不要挖掉 PCM

function isTTSStreamPayload(payload) {
  return typeof payload['TTS_CHUNK'] !== 'undefined' ||
         typeof payload['TTS_END'] !== 'undefined' ||
         typeof payload['TTS_DONE'] !== 'undefined';
}

function processAmQueue() {
  if (amSending || amQueue.length === 0) return;
  amSending = true;

  var item = amQueue[0];
  // TTS 流消息丢一个会产生音频缺口；给更多重试机会，但重试间隔必须短。
  // 旧的 500ms backoff 连续几次失败会直接变成 2s+ 大空洞。
  var isTTSStream = isTTSStreamPayload(item.payload);
  var maxRetries = isTTSStream ? TTS_STREAM_MAX_RETRIES : 3;
  var retryDelay = isTTSStream ? TTS_STREAM_RETRY_DELAY_MS : AM_RETRY_DELAY_MS;
  Pebble.sendAppMessage(
    item.payload,
    function() {
      amQueue.shift();
      amSending = false;
      setTimeout(processAmQueue, 15);
    },
    function(e) {
      console.log('sendToWatch fail: ' + JSON.stringify(e));
      item.retries++;
      if (item.retries < maxRetries) {
        setTimeout(function() { amSending = false; processAmQueue(); }, retryDelay);
      } else {
        amQueue.shift();
        amSending = false;
        setTimeout(processAmQueue, 15);
      }
    }
  );
}

function sendToWatch(payload) {
  amQueue.push({ payload: payload, retries: 0 });
  processAmQueue();
}

// ── 模型列表管理 ────────────────────────────────────────────────────────────────
function getModelList() {
  try {
    var raw = localStorage.getItem('model_list');
    if (raw) {
      var list = JSON.parse(raw);
      if (Array.isArray(list) && list.length > 0) return list;
    }
  } catch(e) {}
  // 默认：把当前 model 作为唯一项
  var m = getSetting('model', 'google/gemma-4-31b-it');
  return [m];
}

function saveModelList(list) {
  localStorage.setItem('model_list', JSON.stringify(list.slice(0, 5)));
}

function sendModelList() {
  var models = getModelList();
  var current = getSetting('model', 'google/gemma-4-31b-it');
  var idx = 0;
  for (var i = 0; i < models.length; i++) {
    if (models[i] === current) { idx = i; break; }
  }
  // 格式: "idx|model1\nmodel2\nmodel3"
  var payload = idx + '|' + models.join('\n');
  sendToWatch({ 'MODEL_NAME': payload });
}

function sendReadyStatus() {
  var apiKey = getSetting('api_key', '');
  var isReady = (apiKey && apiKey.trim().length > 0) ? 1 : 0;
  sendToWatch({ 'READY_STATUS': isReady });
  // 同步显示设置给手表（合并在后续消息中避免额外蓝牙通讯）
  setTimeout(function() {
    var fontSize = parseInt(getSetting('font_size', '0'), 10);
    var fontBold = parseInt(getSetting('font_bold', '0'), 10);
    var disableSurprise = parseInt(getSetting('disable_surprise', '0'), 10);
    var healthEnabled = parseInt(getSetting('health_enabled', '0'), 10);
    var webSearch = parseInt(getSetting('web_search_enabled', '1'), 10);
    console.log('[Settings] healthEnabled=' + healthEnabled + ' webSearch=' + webSearch);
    sendToWatch({ 'FONT_SIZE': fontSize, 'FONT_BOLD': fontBold, 'DISABLE_SURPRISE': disableSurprise, 'HEALTH_ENABLED': healthEnabled, 'WEB_SEARCH_ENABLED': webSearch });
  }, 200);
  setTimeout(sendChatList, 400);  // 稍长间隔确保 READY_STATUS 先到
  setTimeout(sendModelList, 800); // 再发模型列表
}

function sendChatList() {
  var store = loadStore();
  var chats = store.chats.slice().reverse().slice(0, MAX_WATCH_CHATS);
  var lines = chats.map(function(c) {
    return c.id + '|' + (c.title || 'New chat').substring(0, 35);
  });
  var listStr = lines.join('\n');
  sendToWatch({ 'CHAT_LIST': listStr, 'SWITCH_CHAT': store.active_id || '' });
}

// ═══════════════════════════════════════════════════════════════════════════════
// LLM 自动生成对话标题
// ═══════════════════════════════════════════════════════════════════════════════
function generateTitle(chatId, userMsg, aiReply) {
  var apiKey = getSetting('api_key', '');
  var model = getSetting('model', 'google/gemma-4-31b-it');
  if (!apiKey) return;

  var endpoint = getApiEndpoint();
  if (!endpoint) return;

  var xhr = new XMLHttpRequest();
  xhr.open('POST', endpoint, true);
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.setRequestHeader('Authorization', 'Bearer ' + apiKey);
  if (isOpenRouter()) {
    xhr.setRequestHeader('HTTP-Referer', 'https://github.com/deusaw/Pebble-Wrist-AI');
    xhr.setRequestHeader('X-Title', 'Pebble Wrist AI');
  }
  xhr.timeout = 30000;

  xhr.onload = function() {
    if (xhr.status >= 400) {
      console.log('[Title] HTTP error: ' + xhr.status);
      return;
    }
    try {
      var data = JSON.parse(xhr.responseText);
      var msg = data.choices[0].message;
      var title = '';

      // Handle string content (normal models) vs array content (thinking models like Claude)
      if (typeof msg.content === 'string') {
        title = msg.content.trim();
      } else if (Array.isArray(msg.content)) {
        // Thinking models return an array: [{type:"thinking",...}, {type:"text", text:"..."}]
        for (var i = 0; i < msg.content.length; i++) {
          if (msg.content[i].type === 'text' && msg.content[i].text) {
            title = msg.content[i].text.trim();
            break;
          }
        }
      }

      // Fallback: try reasoning field
      if (!title && msg.reasoning) {
        title = msg.reasoning.split(/[.\n]/)[0].trim();
      }

      // Clean up quotes and markdown
      title = sanitizePebbleText(title);
      title = title.replace(/^[\"'\*]+|[\"'\*]+$/g, '').trim();
      title = title.replace(/^[#\s\-:]+/g, '').trim();
      title = title.replace(/\*+/g, '');
      if (title.toLowerCase().indexOf('title:') === 0) {
        title = title.substring(6).trim();
      }
      if (title.length > 35) title = title.substring(0, 35);

      console.log('[Title] Generated: "' + title + '"');

      if (title.length > 0) {
        var store = loadStore();
        for (var j = 0; j < store.chats.length; j++) {
          if (store.chats[j].id === chatId) {
            store.chats[j].title = title;
            saveStore(store);
            sendChatList();
            break;
          }
        }
      }
    } catch (e) { console.log('[Title] Parse error: ' + e); }
  };
  xhr.onerror = function() { console.log('[Title] Network error'); };
  xhr.ontimeout = function() { console.log('[Title] Timed out'); };

  xhr.send(safeJsonStringify({
    model: model,
    stream: false,
    max_tokens: 300,
    messages: [
      { role: 'system', content: 'Generate a very short title (2-5 words, no quotes, no punctuation) for this conversation.' },
      { role: 'user', content: userMsg.substring(0, 200) },
      { role: 'assistant', content: (typeof aiReply === 'string' ? aiReply : '').substring(0, 200) }
    ]
  }));
}

// ═══════════════════════════════════════════════════════════════════════════════
// OpenRouter API 调用
//
// 采用非流式 (stream: false) 模式：
//   - PebbleKit JS 的 XHR 引擎不支持流式 responseText（onprogress 崩溃）
//   - 非流式一次性返回完整 JSON，稳定可靠
//   - 超时 80秒（大模型思考可能较慢）
// ═══════════════════════════════════════════════════════════════════════════════
// JS3 fix: 移除未使用的 onChunk 参数，非流式模式不需要分块回调
function askAI(question, contextText, onFinish) {
  currentAskSessionId++;
  var thisSessionId = currentAskSessionId;

  var apiKey = getSetting('api_key', '');
  var model = getSetting('model', 'google/gemma-4-31b-it');
  var systemMessage = getSetting('system_message', DEFAULT_PROMPT);

  if (!apiKey) {
    onFinish('No API key. Open settings.', '');
    return;
  }

  var endpoint = getApiEndpoint();
  if (!endpoint) {
    onFinish('No API URL. Open settings.', '');
    return;
  }

  var store = loadStore();
  var chat = getActiveChat(store);
  if (!chat) {
    chat = createNewChat(store);
  }

  var sendMessages = chat.messages.concat([{ role: 'user', content: question }]);

  var xhr = new XMLHttpRequest();
  xhr.open('POST', endpoint, true);
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.setRequestHeader('Authorization', 'Bearer ' + apiKey);
  if (isOpenRouter()) {
    xhr.setRequestHeader('HTTP-Referer', 'https://github.com/deusaw/Pebble-Wrist-AI');
    xhr.setRequestHeader('X-Title', 'Pebble Wrist AI');
  }

  xhr.onload = function() {
    if (thisSessionId !== currentAskSessionId) return;

    if (xhr.status >= 400) {
      var errMsg = 'HTTP ' + xhr.status;
      try {
        var errData = JSON.parse(xhr.responseText);
        if (errData.error && errData.error.message) {
          errMsg = errData.error.message.substring(0, 50);
        }
      } catch (e) {}
      onFinish(errMsg, '');
      return;
    }

    var accumulatedReply = '';
    try {
      var data = JSON.parse(xhr.responseText);
      if (data.choices && data.choices.length > 0 && data.choices[0].message) {
        var msg = data.choices[0].message;
        // JS1 fix: 正确处理 thinking model 返回的 content 数组
        // 如 Claude: [{type:"thinking",...}, {type:"text", text:"..."}]
        if (typeof msg.content === 'string') {
          accumulatedReply = msg.content;
        } else if (Array.isArray(msg.content)) {
          for (var ci = 0; ci < msg.content.length; ci++) {
            if (msg.content[ci].type === 'text' && msg.content[ci].text) {
              accumulatedReply = msg.content[ci].text;
              break;
            }
          }
        }
        // Fallback: reasoning 字段（某些 thinking model）
        if (!accumulatedReply && msg.reasoning) {
          accumulatedReply = msg.reasoning;
        }
      }
    } catch (e) {
      console.log('[Parse] JSON error: ' + e);
    }

    // Filter <think>...</think> blocks and sanitize
    if (accumulatedReply) {
      accumulatedReply = accumulatedReply.replace(/<think>[\s\S]*?<\/think>/g, '').trim();
      accumulatedReply = sanitizePebbleText(accumulatedReply);
    }

    console.log('[Done] reply len=' + (accumulatedReply || '').length);

    var replyToSave = accumulatedReply || '(Failed to parse model response)';
    
    var qs = question.length > 800 ? question.substring(0, 800) + '...' : question;
    var rs = replyToSave.length > 1800 ? replyToSave.substring(0, 1800) + '...' : replyToSave;

    chat.messages.push({ role: 'user', content: qs });
    chat.messages.push({ role: 'assistant', content: rs });
    if (chat.messages.length > MAX_MESSAGES_PER_CHAT) {
      chat.messages = chat.messages.slice(chat.messages.length - MAX_MESSAGES_PER_CHAT);
    }
    saveStore(store);

    if (chat.messages.length <= 2) {
      sendChatList();
    }

    if (chat.messages.length <= 4 && chat.title === 'New chat') {
      generateTitle(chat.id, question, replyToSave);
    }

    onFinish(null, replyToSave);
  };

  xhr.onerror = function() { onFinish('Network error', ''); };
  xhr.timeout = 80000;
  xhr.ontimeout = function() { onFinish('Request timed out', ''); };

  // 拼接 System Prompt + contextText（健康/定位数据）
  var fullSystemMessage = systemMessage;
  if (contextText && contextText.length > 0) {
    fullSystemMessage += '\n\n' + contextText;
  }

  var messages = [{ role: 'system', content: fullSystemMessage }].concat(sendMessages);
  var body = { model: model, stream: false, messages: messages };
  
  if (model.indexOf('gemma') !== -1) {
    body.provider = { allow_fallbacks: true };
    body.extra_body = { reasoning: { enabled: true } };
  }

  // Web Search（仅 OpenRouter）：启用 OpenRouter 的 web 插件让模型联网搜索
  if (isOpenRouter() && getSetting('web_search_enabled', '1') === '1') {
    body.plugins = [{ id: 'web' }];
  }

  xhr.send(safeJsonStringify(body));
}

// ═══════════════════════════════════════════════════════════════════════════════
// TTS 语音朗读（Google Cloud TTS → raw 8bit PCM → 分块蓝牙传输）
//
// 流程：收到 TTS_REQUEST → 取当前回复文本 → 按句切分 → 逐句调 TTS → 取高 8 位转 raw PCM
// → 分块发往手表 → 手表端环形缓冲 + 音量缩放播放
//
// 语言自动检测：回复含中文字符即用 cmn-CN 语音，否则用 en-US。
// ═══════════════════════════════════════════════════════════════════════════════

// Base64 解码：PebbleKit JS 无 atob，自行实现。
// 返回二进制字符串（每个字符 0-255），与 atob 行为一致，供 charCodeAt 逐字节读取。
var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function atobLocal(b64) {
  // 去除 padding 和空白
  b64 = b64.replace(/[^A-Za-z0-9+/]/g, '');
  var bytes = [];
  for (var i = 0; i < b64.length; i += 4) {
    var c0 = B64.indexOf(b64.charAt(i));
    var c1 = B64.indexOf(b64.charAt(i + 1));
    var c2 = B64.indexOf(b64.charAt(i + 2));
    var c3 = B64.indexOf(b64.charAt(i + 3));
    var n = (c0 << 18) | (c1 << 12) | (c2 << 6) | c3;
    bytes.push((n >> 16) & 0xFF);
    if (c2 >= 0) bytes.push((n >> 8) & 0xFF);
    if (c3 >= 0) bytes.push(n & 0xFF);
  }
  // 转成二进制字符串
  var str = '';
  for (var j = 0; j < bytes.length; j++) {
    str += String.fromCharCode(bytes[j]);
  }
  return str;
}

// 16bit PCM → 8bit PCM（取高 8 位）。
// Google TTS 返回 LINEAR16（16-bit 小端），扬声器要 8-bit。
// 纯 >>8 截断：8bit 量化底噪是宽频白噪声（~50dB SNR），不刺耳。
// 注：曾尝试加 ±1 LSB 交替抖动"白化"量化噪声，但对静音段会输出 +1,-1,+1,-1...
// 即 4000Hz 近满幅方波（刺耳蜂鸣），确定性抖动在 Fs/2 必然产生音调，已移除。
// 若后续需要抖动，须用 Math.random() 生成真随机 TPDF 噪声。
// 字节格式：输出 unsigned 0..255，手表端 (int8_t) 强转回有符号（标准二进制补码往返）。
function pcm16ToPcm8(pcm16) {
  var pcm8 = [];
  for (var i = 0; i < pcm16.length; i++) {
    pcm8.push((pcm16[i] >> 8) & 0xFF);  // 取高 8 位为 unsigned 0..255
  }
  return pcm8;
}

// 检测文本语言：含中文字符返回中文语音，否则返回英文语音
function detectTTSVoice(text) {
  if (/[\u4e00-\u9fff]/.test(text)) {
    return { lang: 'cmn-CN', name: 'cmn-CN-Wavenet-A' };
  }
  return { lang: 'en-US', name: 'en-US-Journey-F' };
}

// TTS 朗读主流程
var ttsSessionId = 0;         // 递增防止旧进程干扰新请求
var ttsSentenceQueue = [];
var ttsAudioQueue = [];
var ttsPaused = false;        // 手表环形缓冲水位过高时发 TTS_PAUSE，置 true 后软降速；TTS_RESUME 恢复
var ttsPauseSince = 0;        // PAUSE 到达时间；RESUME 丢失时用于自动退出软降速
var ttsBoostUntil = 0;        // 低水位/underrun 后临时加速补货的截止时间
// 顺序保证：并发预取时 TTS 响应可能乱序到达。用序号 + 待发 map 保证按原始句序入队。
var ttsNextSeq = 0;           // 下一个待编码的句序号（ttsFetchNext 取走时分配）
var ttsNextDeliver = 0;       // 下一个待投递的句序号（按序检查 pendingDeliveries）
var ttsPendingDeliveries = {}; // seq → 编码好的 PCM 字节数组，等前序句子到齐后按序入 ttsAudioQueue
var ttsTotalSentences = 0;    // 当前 session 总句数；全量预编码完成后才开始发送
var ttsSendStarted = false;

// 取消 TTS：手表停止朗读时调用。递增 sessionId 断绝旧进程，
// 清空音频/句子队列，并从 amQueue 移除所有待发的 TTS_CHUNK/TTS_END/TTS_DONE。
function cancelTTS() {
  ttsSessionId++;  // 旧 session 的 fetch/send 回调比对失败即停止
  ttsFetchingCount = 0;       // 重置并发计数：在途的 fetch 回调会因 sessionId 不匹配而早退
  ttsAudioQueue = [];
  ttsSentenceQueue = [];
  ttsCurrentSentence = null;
  ttsCurrentOffset = 0;
  ttsPaused = false;          // 取消时清除流控暂停态，下次 startTTS 干净开始
  ttsPauseSince = 0;
  ttsBoostUntil = 0;
  ttsNextSeq = 0;
  ttsNextDeliver = 0;
  ttsPendingDeliveries = {};
  ttsTotalSentences = 0;
  ttsSendStarted = false;
  // 从 amQueue 移除待发的 TTS 消息。
  // 注意：若 amQueue[0] 正在发送（amSending=true），必须保留它，
  // 否则发送成功回调的 amQueue.shift() 会 shift 掉错误元素。
  var startIndex = amSending ? 1 : 0;
  var kept = amQueue.slice(0, startIndex);
  var rest = amQueue.slice(startIndex).filter(function(item) {
    return typeof item.payload['TTS_CHUNK'] === 'undefined' &&
           typeof item.payload['TTS_END'] === 'undefined' &&
           typeof item.payload['TTS_DONE'] === 'undefined';
  });
  amQueue = kept.concat(rest);
}

function startTTS() {
  ttsSessionId++;  // 断绝旧进程
  var thisSession = ttsSessionId;

  var ttsApiKey = getSetting('tts_api_key', '');
  if (!ttsApiKey || ttsApiKey.trim().length === 0) {
    sendToWatch({ 'STATUS': 'No TTS API key' });
    return;
  }

  var store = loadStore();
  var chat = getActiveChat(store);
  if (!chat || !chat.messages || chat.messages.length === 0) {
    sendToWatch({ 'STATUS': 'No text to read' });
    return;
  }

  // 取最后一条 assistant 消息
  var replyText = '';
  for (var i = chat.messages.length - 1; i >= 0; i--) {
    if (chat.messages[i].role === 'assistant') {
      replyText = chat.messages[i].content;
      break;
    }
  }
  if (!replyText || replyText.length === 0) {
    sendToWatch({ 'STATUS': 'No text to read' });
    return;
  }

  // 按句切分（兼容中英文标点）
  var sentences = replyText.match(/[^.!?。！？]+[.!?。！？]*/g) || [replyText];
  ttsSentenceQueue = [];
  for (var s = 0; s < sentences.length; s++) {
    var trimmed = sentences[s].trim();
    if (trimmed.length > 0) ttsSentenceQueue.push(trimmed);
  }
  if (ttsSentenceQueue.length === 0) {
    sendToWatch({ 'STATUS': 'No TTS text' });
    return;
  }

  // 清空残留队列，防止旧 session 残留数据混入
  ttsAudioQueue = [];
  ttsFetchingCount = 0;       // 重置并发计数
  ttsPaused = false;          // 新请求：清除上一次可能残留的流控暂停态
  ttsPauseSince = 0;
  ttsBoostUntil = 0;
  ttsNextSeq = 0;
  ttsNextDeliver = 0;
  ttsPendingDeliveries = {};
  ttsTotalSentences = ttsSentenceQueue.length;
  ttsSendStarted = false;
  // 从 amQueue 移除上一次 session 残留的 TTS chunk。
  // watch 的 tts_request() 不发 TTS_CANCEL（避免 outbox 占用），所以旧 chunk 会继续
  // 灌入新 session 的缓冲 → 音频污染。这里显式过滤。
  // 注意：若 amQueue[0] 正在发送（amSending）则保留它（与 cancelTTS 同理）。
  var startIndex = amSending ? 1 : 0;
  var kept = amQueue.slice(0, startIndex);
  var rest = amQueue.slice(startIndex).filter(function(item) {
    return typeof item.payload['TTS_CHUNK'] === 'undefined' &&
           typeof item.payload['TTS_END'] === 'undefined' &&
           typeof item.payload['TTS_DONE'] === 'undefined';
  });
  amQueue = kept.concat(rest);

  // 启动全量预编码：先把整段回复所有句子都合成为 PCM，再开始发送给手表。
  // 这样播放期间不再等待 Google TTS RTT，避免长文本后半段追赶失败造成大空窗。
  ttsFetchNext(ttsApiKey, thisSession);
  ttsFetchNext(ttsApiKey, thisSession);
  ttsFetchNext(ttsApiKey, thisSession);
  ttsMaybeStartSending(thisSession);
}

// LOOP 1：逐句调 Google TTS → 转 raw 8bit PCM → 推入音频队列
// 预取流水线：允许最多 3 个并发 TTS 请求，保证播放当前句时后面 2 句已在编码/到达中。
// 2 并发在长文本（~10 秒后）领先优势会被消费光——第 5-6 句的 TTS API 响应稍慢
// 就赶不上 8000 B/s 消费，缓冲见底 → 卡顿。3 并发多一层领先缓冲。
var TTS_PREFETCH_CONCURRENCY = 3;
var ttsFetchingCount = 0;

function ttsQueueDelivery(seq, pcm8Bytes) {
  ttsPendingDeliveries[seq] = pcm8Bytes || [];
  while (ttsPendingDeliveries.hasOwnProperty(ttsNextDeliver)) {
    ttsAudioQueue.push(ttsPendingDeliveries[ttsNextDeliver]);
    delete ttsPendingDeliveries[ttsNextDeliver];
    ttsNextDeliver++;
  }
}

function ttsMaybeStartSending(sessionId) {
  if (sessionId !== ttsSessionId) return;
  if (ttsSendStarted) return;
  if (ttsFetchingCount > 0 || ttsSentenceQueue.length > 0) return;
  if (ttsNextDeliver < ttsTotalSentences) return;

  ttsSendStarted = true;
  ttsSendNext(sessionId);
}

function ttsFetchNext(ttsApiKey, sessionId) {
  if (ttsFetchingCount >= TTS_PREFETCH_CONCURRENCY) return;
  if (ttsSentenceQueue.length === 0) return;
  if (sessionId !== ttsSessionId) return;  // 旧 session，停止
  ttsFetchingCount++;

  var sentence = ttsSentenceQueue.shift();
  var seq = ttsNextSeq++;       // 分配句序号（并发响应可能乱序到达，靠序号保证投递顺序）
  var voice = detectTTSVoice(sentence);

  var url = 'https://texttospeech.googleapis.com/v1/text:synthesize?key=' + ttsApiKey;
  var xhr = new XMLHttpRequest();
  xhr.open('POST', url, true);
  xhr.setRequestHeader('Content-Type', 'application/json');

  // 语速：config 可配（默认 1.0 正常语速），范围 0.25-4.0
  var speakingRate = parseFloat(getSetting('tts_rate', '1.0'));
  if (isNaN(speakingRate) || speakingRate < 0.25) speakingRate = 0.25;
  if (speakingRate > 4.0) speakingRate = 4.0;

  var body = {
    input: { text: sentence },
    voice: { languageCode: voice.lang, name: voice.name },
    audioConfig: { audioEncoding: 'LINEAR16', sampleRateHertz: 8000, speakingRate: speakingRate }
  };

  xhr.onload = function() {
    if (sessionId !== ttsSessionId) return;  // 旧 session，丢弃
    if (xhr.status >= 200 && xhr.status < 300) {
      try {
        var data = JSON.parse(xhr.responseText);
        if (data.audioContent) {
          var decoded = atobLocal(data.audioContent);
          var pcm16 = [];
          // 跳过 WAV 头（44 字节），LINEAR16 为小端 16-bit 采样
          for (var i = 44; i < decoded.length; i += 2) {
            var lo = decoded.charCodeAt(i);
            var hi = decoded.charCodeAt(i + 1);
            var sample = (hi << 8) | lo;
            if (sample > 32767) sample -= 65536;
            pcm16.push(sample);
          }
          var pcm8Bytes = pcm16ToPcm8(pcm16);
          // 顺序投递：并发响应可能乱序，用 seq 保证 ttsAudioQueue 仍按原文顺序。
          ttsQueueDelivery(seq, pcm8Bytes);
        } else {
          ttsQueueDelivery(seq, []);
        }
      } catch (e) {
        console.log('[TTS] Parse error: ' + e);
        ttsQueueDelivery(seq, []);
      }
    } else {
      // HTTP 错误（403 鉴权失败/配额超限/无效 key 等）：先本地停止发送，再通知手表
      console.log('[TTS] HTTP error: ' + xhr.status);
      cancelTTS();  // 立即清队列，防止 ttsSendNext 继续发残余 chunk 导致手表重新播放
      var errMsg = 'TTS error: HTTP ' + xhr.status;
      try {
        var errData = JSON.parse(xhr.responseText);
        if (errData && errData.error && errData.error.message) {
          errMsg = 'TTS: ' + errData.error.message.substring(0, 40);
        }
      } catch (e2) {}
      sendToWatch({ 'STATUS': errMsg });  // 手表收到后 tts_stop（也会回 TTS_CANCEL，但 cancelTTS 已执行过，幂等）
      // cancelTTS 已把 ttsFetchingCount 置 0，这里不再 --（否则变 -1）
      return;
    }
    ttsFetchingCount--;
    // 尝试再启动一个并发 fetch（流水线补位）。若无更多句子或并发已满，ttsFetchNext 内部会早退。
    ttsFetchNext(ttsApiKey, sessionId);
    ttsMaybeStartSending(sessionId);
  };

  xhr.onerror = function() {
    if (sessionId !== ttsSessionId) return;
    console.log('[TTS] Network error');
    cancelTTS();  // 已把 ttsFetchingCount 置 0，无需再 --
    sendToWatch({ 'STATUS': 'TTS: network error' });
    return;
  };

  xhr.send(safeJsonStringify(body));
}

// LOOP 2：从音频队列取 raw PCM 数据 → 分块入 amQueue 串行发送
// 终局策略：恒速前馈，而不是靠 PAUSE/RESUME 闭环调速。
// 正常按 350B/43ms≈8.14KB/s 投递，略高于 8KB/s 播放消费；手表先攒 3s 再开播。
// PAUSE/RESUME 只作为接近满缓冲的保险，正常朗读不应频繁触发。
var TTS_AMQUEUE_SAFETY_LIMIT = 8; // amQueue 待发 TTS chunk 安全上限（防 PAUSE 往返期间无限堆积）
var TTS_CHUNK_SIZE = 350;        // 单包适中，减少蓝牙消息数量，同时保持接近播放时钟的恒速投递
var TTS_SEND_DELAY_MS = 43;      // 350B/43ms≈8.14KB/s，接近播放速率，避免水位大幅震荡
var TTS_BOOST_SEND_DELAY_MS = 30; // 低水位/underrun 后≈11.7KB/s 短暂补货，缩短重启空洞
var TTS_BOOST_MS = 5000;         // boost 只持续 5s，避免长期高吞吐重新触发 PAUSE 循环
var TTS_SOFT_PAUSE_SEND_DELAY_MS = 50; // PAUSE 后轻降到≈7KB/s，不断流，只让 ring 缓慢回落
var TTS_PAUSE_STALE_MS = 7000;   // RESUME 丢失兜底；轻降速足够安全，给 C 端时间降到 LOW
var ttsCurrentSentence = null;   // 正在分块发送的句子（字节组）
var ttsCurrentOffset = 0;        // 当前句子已发送到的字节偏移

function ttsPendingChunkCount() {
  var n = 0;
  for (var q = 0; q < amQueue.length; q++) {
    if (typeof amQueue[q].payload['TTS_CHUNK'] !== 'undefined') n++;
  }
  return n;
}

function ttsSendNext(sessionId) {
  if (sessionId !== ttsSessionId) return;  // 旧 session，停止

  // PAUSE 是接近满缓冲时的保险，不是日常节拍器。轻降速持续足够久，
  // 让 ring 回到 LOW；若 RESUME 丢失，7s 后恢复正常恒速，避免永久慢喂导致见底。
  var now = Date.now();
  if (ttsPaused && now - ttsPauseSince >= TTS_PAUSE_STALE_MS) {
    ttsPaused = false;
    ttsPauseSince = 0;
  }

  // 安全阀：amQueue 堆积过多（PAUSE 往返延迟期间可能堆积）时短暂等待，
  // 让 processAmQueue 消化。正常流程下 PAUSE 会在缓冲到 HIGH 时及时触发，
  // 不会堆积到此上限。等待时间短（30ms）避免成为限速瓶颈。
  if (ttsPendingChunkCount() >= TTS_AMQUEUE_SAFETY_LIMIT) {
    setTimeout(function() { ttsSendNext(sessionId); }, 30);
    return;
  }

  // 当前句子还没发完：发下一个 chunk
  if (ttsCurrentSentence && ttsCurrentOffset < ttsCurrentSentence.length) {
    var chunk = [];
    var end = ttsCurrentOffset + TTS_CHUNK_SIZE;
    if (end > ttsCurrentSentence.length) end = ttsCurrentSentence.length;
    for (var i = ttsCurrentOffset; i < end; i++) {
      chunk.push(ttsCurrentSentence[i]);
    }
    sendToWatch({ 'TTS_CHUNK': chunk });
    ttsCurrentOffset = end;
    // 正常略高于消费；PAUSE 时软降速但不断流，避免大空洞。
    var delay = TTS_SEND_DELAY_MS;
    if (ttsPaused) {
      delay = TTS_SOFT_PAUSE_SEND_DELAY_MS;
    } else if (now < ttsBoostUntil) {
      delay = TTS_BOOST_SEND_DELAY_MS;
    }
    setTimeout(function() { ttsSendNext(sessionId); }, delay);
    return;
  }

  // 当前句子发完：发结束标记，准备取下一句
  if (ttsCurrentSentence) {
    sendToWatch({ 'TTS_END': 1 });
    ttsCurrentSentence = null;
    ttsCurrentOffset = 0;
  }

  // 取下一句
  if (ttsAudioQueue.length === 0) {
    // 队列空了但句子可能还在编码/获取中，等一会再查
    if (ttsFetchingCount > 0 || ttsSentenceQueue.length > 0) {
      setTimeout(function() { ttsSendNext(sessionId); }, 300);
      return;
    }
    // 全部句子发完且无在途获取：发"朗读全部结束"标记
    sendToWatch({ 'TTS_DONE': 1 });
    return;
  }

  ttsCurrentSentence = ttsAudioQueue.shift();
  ttsCurrentOffset = 0;
  setTimeout(function() { ttsSendNext(sessionId); }, 4);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 手表消息
// ═══════════════════════════════════════════════════════════════════════════════
Pebble.addEventListener('appmessage', function(e) {
  if (typeof e.payload['NEW_CHAT'] !== 'undefined') {
    currentAskSessionId++; // 断绝旧进程亡魂
    var store = loadStore();
    createNewChat(store);
    setTimeout(sendChatList, 200);
    return;
  }

  if (typeof e.payload['SWITCH_MODEL'] === 'string' && e.payload['SWITCH_MODEL'].length > 0) {
    var modelName = e.payload['SWITCH_MODEL'];
    localStorage.setItem('model', modelName);
    console.log('[Model] Switched to: ' + modelName);
    // 不清空对话历史，只切换模型 → 下一次 askAI 自动用新模型
    setTimeout(sendModelList, 200);  // 确认回传
    return;
  }

  // 手表菜单切换联网搜索 → 更新 localStorage 真值（双路同步：手表改 config 真值）
  if (typeof e.payload['SWITCH_WEB_SEARCH'] !== 'undefined') {
    var wsVal = e.payload['SWITCH_WEB_SEARCH'] ? '1' : '0';
    localStorage.setItem('web_search_enabled', wsVal);
    console.log('[WebSearch] Switched to: ' + wsVal);
    return;
  }

  // 手表取消 TTS（用户按 SELECT 停止 / 切对话 / 新建对话）：停止 JS 端发送并清队列
  if (typeof e.payload['TTS_CANCEL'] !== 'undefined') {
    console.log('[TTS] Cancel received from watch');
    cancelTTS();
    return;
  }

  if (typeof e.payload['SWITCH_CHAT'] !== 'undefined') {
    currentAskSessionId++; // 截断进程
    var switchId = e.payload['SWITCH_CHAT'];
    var store2 = loadStore();
    if (switchId === "") {
      createNewChat(store2);
    } else {
      for (var j = 0; j < store2.chats.length; j++) {
        if (store2.chats[j].id === switchId) {
          store2.active_id = switchId;
          saveStore(store2);
          break;
        }
      }
    }
    setTimeout(sendChatList, 100);

    // 加载切换到的对话的最后一条 Q&A，让手表直接显示历史回复
    var switchedChat = getActiveChat(store2);
    if (switchedChat && switchedChat.messages && switchedChat.messages.length >= 2) {
      var lastQ = '', lastA = '';
      for (var k = switchedChat.messages.length - 1; k >= 0; k--) {
        if (!lastA && switchedChat.messages[k].role === 'assistant') {
          lastA = switchedChat.messages[k].content;
        }
        if (!lastQ && switchedChat.messages[k].role === 'user') {
          lastQ = switchedChat.messages[k].content;
        }
        if (lastQ && lastA) break;
      }
      if (lastA) {
        setTimeout(function() {
          var MAX_WATCH_CHARS = 1800;
          if (lastQ) {
            sendToWatch({ 'USER_QUESTION': lastQ.substring(0, 120) });
          }
          var text = lastA.substring(0, MAX_WATCH_CHARS);
          var chunkSize = 256;
          for (var ci = 0; ci < text.length; ci += chunkSize) {
            sendToWatch({ 'REPLY_CHUNK': text.substring(ci, ci + chunkSize) });
          }
          sendToWatch({ 'REPLY_END': 1 });
        }, 300);
      }
    }

    return;
  }

  // ── TTS 朗读请求 ──
  if (typeof e.payload['TTS_REQUEST'] !== 'undefined') {
    startTTS();
    return;
  }

  // ── TTS 流控（手表环形缓冲水位信号）──
  // PAUSE：手表缓冲接近高水位，JS 进入软降速；RESUME：缓冲回到安全线，恢复正常速度。
  // 不再硬停投递，否则 RESUME 抖动会把几百毫秒蓝牙延迟放大成可闻大空洞。
  if (typeof e.payload['TTS_PAUSE'] !== 'undefined') {
    ttsPaused = true;
    ttsPauseSince = Date.now();
    ttsBoostUntil = 0;
    return;
  }
  if (typeof e.payload['TTS_RESUME'] !== 'undefined') {
    ttsPaused = false;
    ttsPauseSince = 0;
    ttsBoostUntil = Date.now() + TTS_BOOST_MS;
    // RESUME 既表示从 PAUSE 恢复，也被 C 端用作 underrun/低水位补货信号。
    ttsSendNext(ttsSessionId);
    return;
  }

  var question = e.payload['QUESTION'];
  if (!question || question.trim().length === 0) return;  // 空问题直接忽略

  // 提取健康数据（手表端随 QUESTION 一起发来）。
  // -1 哨兵 = 数据不可用（HEALTH_VALUE_UNAVAILABLE 或无健康 API 平台），不注入；
  // 0 = 合法零值（清晨/久坐），正常注入。
  var stepCount = (typeof e.payload['STEP_COUNT'] !== 'undefined') ? e.payload['STEP_COUNT'] : -1;
  var activeMinutes = (typeof e.payload['ACTIVE_MINUTES'] !== 'undefined') ? e.payload['ACTIVE_MINUTES'] : -1;

  console.log('Q: ' + question.substring(0, 60));

  var MAX_WATCH_CHARS = 1800;  // 手表端单条回复的展示上限（C 端 R_BUF_SIZE=2048，留余量防溢出）

  // 构造健康数据上下文（只要有一个指标可用即注入，合法 0 也算）
  var contextText = '';
  if (stepCount >= 0 || activeMinutes >= 0) {
    contextText = 'Current health data:';
    if (stepCount >= 0) contextText += '\n- Steps today: ' + stepCount;
    if (activeMinutes >= 0) contextText += '\n- Active minutes: ' + activeMinutes;
  }

  // 检查定位开关
  var locationEnabled = (getSetting('location_enabled', '0') === '1');

  // 捕获当前问答 session ID：GPS/地理编码异步回调最多 5s，期间用户可能已问新问题。
  // 回调中比对 session，旧问题的迟到结果不覆盖新问题的回复。
  var locSessionId = currentAskSessionId;

  // 如果定位开关打开，先获取 GPS，再调 askAI；否则直接调
  if (locationEnabled) {
    if (navigator.geolocation) {
      navigator.geolocation.getCurrentPosition(
        function success(pos) {
          if (locSessionId !== currentAskSessionId) return;  // 旧问题，丢弃
          var lat = pos.coords.latitude;
          var lng = pos.coords.longitude;
          // 反向地理编码：坐标 → 城市名（免费 API，无需 key，CORS 友好）
          var geoXhr = new XMLHttpRequest();
          var geoUrl = 'https://api.bigdatacloud.net/data/reverse-geocode-client?latitude=' +
            lat + '&longitude=' + lng + '&localityLanguage=zh';
          geoXhr.open('GET', geoUrl, true);
          geoXhr.timeout = 3000;
          geoXhr.onload = function() {
            if (locSessionId !== currentAskSessionId) return;  // 旧问题，丢弃
            var locStr = lat.toFixed(2) + ', ' + lng.toFixed(2);  // 回退：裸坐标
            try {
              var geo = JSON.parse(geoXhr.responseText);
              var parts = [];
              if (geo.city) parts.push(geo.city);
              else if (geo.locality) parts.push(geo.locality);
              if (geo.principalSubdivision) parts.push(geo.principalSubdivision);
              if (geo.countryName) parts.push(geo.countryName);
              if (parts.length > 0) locStr = parts.join(', ');
            } catch (e) {}
            if (contextText) contextText += '\n';
            contextText += '- Approx location: ' + locStr;
            callAskAI(question, contextText, MAX_WATCH_CHARS);
          };
          geoXhr.onerror = function() {
            if (locSessionId !== currentAskSessionId) return;  // 旧问题，丢弃
            // 反查失败，回退到裸坐标
            if (contextText) contextText += '\n';
            contextText += '- Approx location: ' + lat.toFixed(2) + ', ' + lng.toFixed(2);
            callAskAI(question, contextText, MAX_WATCH_CHARS);
          };
          geoXhr.ontimeout = function() {
            if (locSessionId !== currentAskSessionId) return;  // 旧问题，丢弃
            // 超时，回退到裸坐标
            if (contextText) contextText += '\n';
            contextText += '- Approx location: ' + lat.toFixed(2) + ', ' + lng.toFixed(2);
            callAskAI(question, contextText, MAX_WATCH_CHARS);
          };
          geoXhr.send();
        },
        function error() {
          if (locSessionId !== currentAskSessionId) return;  // 旧问题，丢弃
        // 定位失败（用户拒绝/超时），不带定位继续
          callAskAI(question, contextText, MAX_WATCH_CHARS);
        },
        { enableHighAccuracy: false, timeout: 2000, maximumAge: 300000 }
      );
    } else {
      // 浏览器不支持定位，不带定位继续
    callAskAI(question, contextText, MAX_WATCH_CHARS);
    }
  } else {
    // 定位开关关闭，直接调
    callAskAI(question, contextText, MAX_WATCH_CHARS);
  }
});

// 提取出来的 askAI 调用逻辑，支持 contextText 注入
function callAskAI(question, contextText, MAX_WATCH_CHARS) {
  // JS3 fix: 移除 onChunk 死代码，非流式模式下直接在 onFinish 中处理完整回复
  askAI(question, contextText, function onFinish(err, reply) {
    if (err) {
      console.log('Error: ' + err);
      sendToWatch({ 'STATUS': 'Error: ' + err.substring(0, 40) });
      sendToWatch({ 'REPLY_END': 1 });
      return;
    }
    if (reply && reply.length > 0) {
      // 截断过长回复 + 分块发送（蓝牙 256 字节限制）
      var text = reply.substring(0, MAX_WATCH_CHARS);
      if (reply.length > MAX_WATCH_CHARS) {
        text += '\n[...truncated]';
      }
      var chunkSize = 256;
    for (var i = 0; i < text.length; i += chunkSize) {
        sendToWatch({ 'REPLY_CHUNK': text.substring(i, i + chunkSize) });
      }
    }
    sendToWatch({ 'REPLY_END': 1 });
  });
}

// ═══════════════════════════════════════════════════════════════════════════════
// 配置页面
// ═══════════════════════════════════════════════════════════════════════════════
Pebble.addEventListener('showConfiguration', function() {
  var hasKey = getSetting('api_key', '') ? '1' : '0';
  var model = getSetting('model', 'google/gemma-4-31b-it');
  var systemMessage = getSetting('system_message', DEFAULT_PROMPT);
  var store = loadStore();

  // 恢复精简 metadata（展示列表用）, 加强防御性判断防止异常数据拉取崩溃
  var chatMeta = store.chats.slice(-MAX_CONFIG_CHATS).map(function(c) {
    return { id: c.id, title: (c.title || 'New chat').substring(0, 30), created: c.created, count: (c.messages ? c.messages.length : 0) };
  });

  // 由于我们已经改用 Hash (#) 传递数据，避开了服务器 8KB 拦截死线，纯粹依靠手机本地 WebView 消化。
  // 现代手机起步承受力在 2MB 以上。这里将安全线放宽到 500,000 字符（约 500KB），
  // 确保即使是一整段“全满极限中文对话（约 144KB）”也能被 100% 完整传输出去并导出。
  var safeDocs = [];
  var chatsCopy = store.chats.slice(-MAX_CONFIG_CHATS).reverse(); 
  for (var i = 0; i < chatsCopy.length; i++) {
    safeDocs.push({ id: chatsCopy[i].id, messages: (chatsCopy[i].messages || []) });
    if (encodeURIComponent(JSON.stringify(safeDocs)).length > 500000) {
      safeDocs.pop(); // 极度超限（可能积累了多条巨长巨长的历史），抛弃保护
      break;
    }
  }

  var modelList = getModelList();
  var apiMode = getSetting('api_mode', 'openrouter');
  var customApiUrl = getSetting('custom_api_url', '');

  // 检测手表平台：TTS 仅 Emery 可用，非 Emery 时隐藏 config 页 TTS 区域
  var isEmery = '0';
  try {
    var watchInfo = Pebble.getActiveWatchInfo();
    if (watchInfo && watchInfo.platform === 'emery') isEmery = '1';
  } catch (e) {}

  var url = 'https://deusaw.github.io/Pebble-Wrist-AI/config/'
    + '?has_key=' + hasKey
    + '&is_emery=' + isEmery
    + '&model=' + encodeURIComponent(model)
    + '&model_list=' + encodeURIComponent(JSON.stringify(modelList))
    + '&system_message=' + encodeURIComponent(systemMessage)
    + '&active_id=' + encodeURIComponent(store.active_id || '')
    + '&chats=' + encodeURIComponent(JSON.stringify(chatMeta))
    + '&api_mode=' + encodeURIComponent(apiMode)
    + '&custom_api_url=' + encodeURIComponent(customApiUrl)
    + '&font_size=' + encodeURIComponent(getSetting('font_size', '0'))
    + '&font_bold=' + encodeURIComponent(getSetting('font_bold', '0'))
    + '&disable_surprise=' + encodeURIComponent(getSetting('disable_surprise', '0'))
    + '&location_enabled=' + encodeURIComponent(getSetting('location_enabled', '0'))
    + '&web_search_enabled=' + encodeURIComponent(getSetting('web_search_enabled', '1'))
    + '&health_enabled=' + encodeURIComponent(getSetting('health_enabled', '0'))
    + '&has_tts_key=' + (getSetting('tts_api_key', '') ? '1' : '0')
    + '&tts_rate=' + encodeURIComponent(getSetting('tts_rate', '1.0'))
    // 用 escape() 而非 encodeURIComponent()：escape() 对中文直接编码为 %uXXXX（Unicode 码点），
    // 不依赖 UTF-8 byte 序列，PebbleKit WebView 用任何字符集都能正确处理，不会出乱码。
    + '#export_data=' + escape(JSON.stringify(safeDocs));

  Pebble.openURL(url);
});

Pebble.addEventListener('webviewclosed', function(e) {
  // e.response 可能是 undefined（用户直接关闭 webview）
  if (!e || !e.response || e.response === 'CANCELLED') {
    return;
  }
  try {
    var settings = JSON.parse(decodeURIComponent(e.response));
    if (typeof settings !== 'object' || settings === null) return;

    if (settings.delete_key) {
      localStorage.removeItem('api_key');
    } else if (settings.api_key && settings.api_key.trim().length > 0) {
      localStorage.setItem('api_key', settings.api_key.trim());
    }

    if (settings.delete_tts_key) {
      localStorage.removeItem('tts_api_key');
    }

    if (settings.api_mode) {
      localStorage.setItem('api_mode', settings.api_mode);
    }
    if (settings.api_mode === 'custom' && typeof settings.custom_api_url === 'string') {
      localStorage.setItem('custom_api_url', settings.custom_api_url.trim());
    }

    if (settings.model && settings.model.trim().length > 0) {
      localStorage.setItem('model', settings.model.trim());
    }
    if (settings.model_list && Array.isArray(settings.model_list)) {
      saveModelList(settings.model_list);
      // 确保当前 model 在列表中
      var curModel = getSetting('model', 'google/gemma-4-31b-it');
      if (settings.model_list.indexOf(curModel) === -1 && settings.model_list.length > 0) {
        localStorage.setItem('model', settings.model_list[0]);
      }
    }
    if (settings.system_message && settings.system_message.trim().length > 0) {
      localStorage.setItem('system_message', settings.system_message.trim());
    }

    // 显示设置：字号、加粗、彩蛋开关
    if (typeof settings.font_size !== 'undefined') {
      localStorage.setItem('font_size', String(settings.font_size));
    }
    if (typeof settings.font_bold !== 'undefined') {
      localStorage.setItem('font_bold', String(settings.font_bold));
    }
    if (typeof settings.disable_surprise !== 'undefined') {
      localStorage.setItem('disable_surprise', String(settings.disable_surprise));
    }
    if (typeof settings.location_enabled !== 'undefined') {
      localStorage.setItem('location_enabled', String(settings.location_enabled));
    }
    if (typeof settings.web_search_enabled !== 'undefined') {
      localStorage.setItem('web_search_enabled', String(settings.web_search_enabled));
    }
    if (typeof settings.health_enabled !== 'undefined') {
      localStorage.setItem('health_enabled', String(settings.health_enabled));
    }
    if (settings.tts_api_key && settings.tts_api_key.trim().length > 0) {
      localStorage.setItem('tts_api_key', settings.tts_api_key.trim());
    }
    if (settings.tts_rate) {
      localStorage.setItem('tts_rate', settings.tts_rate);
    }

    if (settings.switch_to) {
      currentAskSessionId++;
      var store = loadStore();
      for (var i = 0; i < store.chats.length; i++) {
        if (store.chats[i].id === settings.switch_to) {
          store.active_id = settings.switch_to;
          saveStore(store);
          break;
        }
      }
    }

    if (settings.delete_chat) {
      var store2 = loadStore();
      // 若删除的是当前活跃对话，停止其 TTS 朗读
      if (store2.active_id === settings.delete_chat) cancelTTS();
      deleteChat(store2, settings.delete_chat);
    }

    // 批量删除（config 页面积累的多条删除操作）
    if (settings.deleted_chats && Array.isArray(settings.deleted_chats)) {
      var storeD = loadStore();
      var activeDeleted = false;
      for (var di = 0; di < settings.deleted_chats.length; di++) {
        if (storeD.active_id === settings.deleted_chats[di]) activeDeleted = true;
        storeD.chats = storeD.chats.filter(function(c) { return c.id !== settings.deleted_chats[di]; });
      }
      if (activeDeleted) cancelTTS();  // 活跃对话被删，停止其 TTS
      if (storeD.active_id) {
        var stillExists = false;
        for (var si = 0; si < storeD.chats.length; si++) {
          if (storeD.chats[si].id === storeD.active_id) { stillExists = true; break; }
        }
        if (!stillExists) {
          storeD.active_id = storeD.chats.length > 0 ? storeD.chats[storeD.chats.length - 1].id : null;
        }
      }
      saveStore(storeD);
    }

    if (settings.clear_all) {
      currentAskSessionId++;
      cancelTTS();  // 全部清除：停止任何正在进行的 TTS
      var store3 = loadStore();
      store3.chats = [];
      store3.active_id = null;
      saveStore(store3);
    }

    sendReadyStatus();
  } catch (ex) {
    console.log('webviewclosed parse error: ' + ex);
  }
});

// ═══════════════════════════════════════════════════════════════════════════════
// 就绪
// ═══════════════════════════════════════════════════════════════════════════════
Pebble.addEventListener('ready', function() {
  console.log('PebbleKit JS ready');

  // 迁移旧数据
  var oldHistory = localStorage.getItem('conversation_history');
  if (oldHistory) {
    try {
      var msgs = JSON.parse(oldHistory);
      if (msgs.length > 0) {
        var store = loadStore();
        store.chats.push({ id: generateId(), title: 'Migrated chat', created: Date.now(), messages: msgs });
        store.active_id = store.chats[store.chats.length - 1].id;
        saveStore(store);
      }
    } catch (e) {}
    localStorage.removeItem('conversation_history');
  }

  // 每次启动默认进入新对话（但不重复创建空白对话）
  var store = loadStore();
  var activeChat = getActiveChat(store);
  if (!activeChat || activeChat.messages.length > 0) {
    // 当前没有活跃对话或活跃对话已有消息 → 找现有空白对话复用
    var emptyChat = null;
    for (var i = store.chats.length - 1; i >= 0; i--) {
      if (store.chats[i].messages.length === 0) {
        emptyChat = store.chats[i];
        break;
      }
    }
    if (emptyChat) {
      store.active_id = emptyChat.id;
      saveStore(store);
    } else {
      createNewChat(store);
    }
  }
  // else: 活跃对话已是空白的 → 不重复创建

  sendReadyStatus();
});
