// PEBBLEKIT JS — runs on iPhone
// 多对话管理 + OpenRouter API + LLM 自动生成对话标题

var MAX_MESSAGES_PER_CHAT = 20;
var MAX_CHATS = 20;             // 限制 20 个对话槽位
var MAX_WATCH_CHATS = 20;       // 手表端菜单最多显示 20 个历史对话
var MAX_CONFIG_CHATS = 8;       // 配置页面按倒序传递 8 个，保障蓝牙与 URL 组装性能（全量细节存在于 Hash 传输中）

var DEFAULT_PROMPT = "You are Pebble Wrist AI, a concise assistant on a Pebble smartwatch. Rules: 1) Reply ONLY with the final answer, no reasoning or thinking process. 2) Keep responses under 400 words in English, or under 400 Chinese characters. 3) No markdown, no bullet lists. 4) Use plain sentences only.";

// ═══════════════════════════════════════════════════════════════════════════════
// 文本清洗：完美解决 Pebble 缺失某些 Unicode 标点引发乱码（â☐☐）的问题
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

// ═══════════════════════════════════════════════════════════════════════════════
// 数据层：多对话存储
// ═══════════════════════════════════════════════════════════════════════════════
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

var currentAskSessionId = 0; // 全局自增，用于阻截孤魂回调

// ── AppMessage 发送信使（严格排队机制）────────────────────────────────────────
// 解决 Pebble 原生单次通讯不能超过 256 字节的物理缺陷。此处设立指令队列，确保分块传输时不会丢包或乱序
var amQueue = [];
var amSending = false;

function processAmQueue() {
  if (amSending || amQueue.length === 0) return;
  amSending = true;
  
  var item = amQueue[0];
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
      if (item.retries < 3) {
        setTimeout(function() { amSending = false; processAmQueue(); }, 500);
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

  var xhr = new XMLHttpRequest();
  xhr.open('POST', 'https://openrouter.ai/api/v1/chat/completions', true);
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.setRequestHeader('Authorization', 'Bearer ' + apiKey);
  xhr.setRequestHeader('HTTP-Referer', 'https://github.com/deusaw/Pebble-Wrist-AI');
  xhr.setRequestHeader('X-Title', 'Pebble Wrist AI');
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

  xhr.send(JSON.stringify({
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
// OpenRouter API
// ═══════════════════════════════════════════════════════════════════════════════
function askAI(question, onChunk, onFinish) {
  currentAskSessionId++;
  var thisSessionId = currentAskSessionId;

  var apiKey = getSetting('api_key', '');
  var model = getSetting('model', 'google/gemma-4-31b-it');
  var systemMessage = getSetting('system_message', DEFAULT_PROMPT);

  if (!apiKey) {
    onFinish('No API key. Open settings.', '');
    return;
  }

  var store = loadStore();
  var chat = getActiveChat(store);
  if (!chat) {
    chat = createNewChat(store);
  }

  var sendMessages = chat.messages.concat([{ role: 'user', content: question }]);

  var xhr = new XMLHttpRequest();
  xhr.open('POST', 'https://openrouter.ai/api/v1/chat/completions', true);
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.setRequestHeader('Authorization', 'Bearer ' + apiKey);
  xhr.setRequestHeader('HTTP-Referer', 'https://github.com/deusaw/Pebble-Wrist-AI');
  xhr.setRequestHeader('X-Title', 'Pebble Wrist AI');

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
        accumulatedReply = data.choices[0].message.content || data.choices[0].message.reasoning || '';
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
    var rs = replyToSave.length > 800 ? replyToSave.substring(0, 800) + '...' : replyToSave;

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

  var messages = [{ role: 'system', content: systemMessage }].concat(sendMessages);
  var body = { model: model, stream: false, messages: messages };
  
  if (model.indexOf('gemma') !== -1) {
    body.provider = { allow_fallbacks: true };
    body.extra_body = { reasoning: { enabled: true } };
  }

  xhr.send(JSON.stringify(body));
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
    return;
  }

  var question = e.payload['QUESTION'];
  if (!question || question.trim().length === 0) return;  // 空问题直接忽略

  console.log('Q: ' + question.substring(0, 60));

  var messageBuffer = '';
  var sendTimeout = null;
  var totalChunksSentLength = 0;

  function flushChunk() {
    if (messageBuffer.length > 0) {
      // Pebble AppMessage maximum payload payload size could be limited.
      // Split into 256 char chunks to ensure reliable bluetooth transmission.
      var chunkSize = 256;
      for (var i = 0; i < messageBuffer.length; i += chunkSize) {
        var chunk = messageBuffer.substring(i, i + chunkSize);
        sendToWatch({ 'REPLY_CHUNK': chunk });
        totalChunksSentLength += chunk.length;
      }
      messageBuffer = '';
    }
    sendTimeout = null;
  }

  var MAX_WATCH_CHARS = 1800;  // 手表端单条回复的展示上限（由于 C 端内存只有 2048 字节缓存空间，留足余量防溢出崩溃）
  var truncated = false;

  askAI(question, function onChunk(text) {
    if (truncated) return;  // 已触达上限，忽略后续 chunk

    // 检查是否超出上限
    if (totalChunksSentLength + messageBuffer.length + text.length > MAX_WATCH_CHARS) {
      // 把当前 buffer + 截断提示一并 flush
      var remaining = MAX_WATCH_CHARS - totalChunksSentLength - messageBuffer.length;
      if (remaining > 0) {
        messageBuffer += text.substring(0, remaining);
      }
      messageBuffer += '\n[...truncated]';
      truncated = true;
      if (sendTimeout) { clearTimeout(sendTimeout); sendTimeout = null; }
      flushChunk();
      return;
    }

    messageBuffer += text;
    if (!sendTimeout) {
      sendTimeout = setTimeout(flushChunk, 250); // debounce AppMessage interval 250ms
    }
  }, function onFinish(err, reply) {
    if (truncated) {
      sendToWatch({ 'REPLY_END': 1 });
      return;
    }
    if (sendTimeout) {
      clearTimeout(sendTimeout);
      sendTimeout = null;
      flushChunk();
    }
    if (err) {
      console.log('Error: ' + err);
      sendToWatch({ 'STATUS': 'Error: ' + err.substring(0, 40) });
      sendToWatch({ 'REPLY_END': 1 }); // 确保手表退出 THINKING 状态，不会永久卡死
      return;
    }
    // 如果流式没有输出任何成功内容（被非流式返回），则兜底重新发送整个回复。
    if (reply && reply.length > 0 && totalChunksSentLength === 0) {
       messageBuffer += reply.substring(0, MAX_WATCH_CHARS);
       flushChunk();
    }
    sendToWatch({ 'REPLY_END': 1 });
  });
});

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

  var url = 'https://deusaw.github.io/Pebble-Wrist-AI/config/'
    + '?has_key=' + hasKey
    + '&model=' + encodeURIComponent(model)
    + '&model_list=' + encodeURIComponent(JSON.stringify(modelList))
    + '&system_message=' + encodeURIComponent(systemMessage.substring(0, 300))
    + '&active_id=' + encodeURIComponent(store.active_id || '')
    + '&chats=' + encodeURIComponent(JSON.stringify(chatMeta))
    + '#export_data=' + encodeURIComponent(JSON.stringify(safeDocs));

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
      deleteChat(store2, settings.delete_chat);
    }

    if (settings.clear_all) {
      currentAskSessionId++;
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

  sendReadyStatus();
});
