<?php
/**
 * 蒹葭 AI 调试工具 —— 独立于游戏服务端，直接调用 Ollama，用于验证人设/知识库效果。
 *
 * 部署：把本文件 + jianjia_soul.md + jianjia_knowledge/ 一起放到能访问 Ollama 的 web server 上，
 * 复制 jianjia_debug_config.example.php 为 jianjia_debug_config.php 并填入真实 token 和 ollama_url。
 *
 * 用法：
 *   浏览器直接打开 jianjia_debug.php?token=xxx           —— 网页聊天测试界面
 *   POST /jianjia_debug.php?token=xxx  {"message": "...", "history": [...]} —— JSON API，供脚本/工具调用
 */

declare(strict_types=1);

// 兜底：任何没被 try/catch 到的真·致命错误（比如内存耗尽），也要以 JSON 形式暴露出来，
// 而不是让 Apache 吐一个没有 body 的空白 500，那样根本没法远程排查。
register_shutdown_function(function (): void {
    $err = error_get_last();
    if ($err && in_array($err['type'], [E_ERROR, E_PARSE, E_CORE_ERROR, E_COMPILE_ERROR], true)) {
        if (!headers_sent()) {
            http_response_code(500);
            header('Content-Type: application/json; charset=utf-8');
        }
        echo json_encode([
            'error' => 'fatal: ' . $err['message'] . ' in ' . $err['file'] . ':' . $err['line'],
        ], JSON_UNESCAPED_UNICODE | JSON_PARTIAL_OUTPUT_ON_ERROR);
    }
});

function jj_json_out(int $status, array $data): void
{
    http_response_code($status);
    header('Content-Type: application/json; charset=utf-8');
    $json = json_encode($data, JSON_UNESCAPED_UNICODE | JSON_PARTIAL_OUTPUT_ON_ERROR);
    if ($json === false) {
        // 极端情况：连 PARTIAL_OUTPUT_ON_ERROR 都编不出来，至少告诉调用方哪里错了
        $json = json_encode(['error' => 'json_encode failed: ' . json_last_error_msg()]);
    }
    echo $json;
    exit;
}

$SOUL_FILE     = __DIR__ . '/jianjia_soul.md';
$KNOWLEDGE_DIR = __DIR__ . '/jianjia_knowledge';

$config = [
    'token'      => 'change-me',
    'ollama_url' => 'http://127.0.0.1:11434/api/chat',
    'model'      => 'qwen3:32b',
    'bot_name'   => '蒹葭',
];
$localConfigFile = __DIR__ . '/jianjia_debug_config.php';
if (is_file($localConfigFile)) {
    $override = require $localConfigFile;
    if (is_array($override)) {
        $config = array_merge($config, $override);
    }
}

function jj_load_system_prompt(string $soulFile, string $knowledgeDir, string $botName): string
{
    if (!is_file($soulFile)) {
        throw new RuntimeException("soul file not found: $soulFile");
    }
    $soul = trim((string)file_get_contents($soulFile));
    $parts = [$soul];
    if (is_dir($knowledgeDir)) {
        $files = glob($knowledgeDir . '/*.md') ?: [];
        sort($files, SORT_STRING);
        foreach ($files as $f) {
            $content = trim((string)file_get_contents($f));
            if ($content !== '') {
                $parts[] = $content;
            }
        }
    }
    $prompt = implode("\n\n", $parts);
    return str_replace('{name}', $botName, $prompt);
}

/** @return array{0: string, 1: int} [响应body, HTTP状态码] */
function jj_http_post_curl(string $url, string $payload): array
{
    $ch = curl_init($url);
    if ($ch === false) {
        throw new RuntimeException("curl_init failed for url: $url");
    }
    curl_setopt_array($ch, [
        CURLOPT_POST           => true,
        CURLOPT_POSTFIELDS     => $payload,
        CURLOPT_HTTPHEADER     => ['Content-Type: application/json'],
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_TIMEOUT        => 120,
    ]);
    $raw = curl_exec($ch);
    if ($raw === false) {
        $err = curl_error($ch);
        curl_close($ch);
        throw new RuntimeException("Ollama request failed (curl): $err");
    }
    $httpCode = (int)curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    return [(string)$raw, $httpCode];
}

/** @return array{0: string, 1: int} [响应body, HTTP状态码] */
function jj_http_post_stream(string $url, string $payload): array
{
    $context = stream_context_create([
        'http' => [
            'method'        => 'POST',
            'header'        => "Content-Type: application/json\r\n",
            'content'       => $payload,
            'timeout'       => 120,
            'ignore_errors' => true, // 非 2xx 也要拿到 body，方便定位问题
        ],
    ]);
    $raw = @file_get_contents($url, false, $context);
    if ($raw === false) {
        $err = error_get_last();
        throw new RuntimeException("Ollama request failed (stream): " . ($err['message'] ?? 'unknown error'));
    }
    $httpCode = 0;
    foreach ($http_response_header ?? [] as $header) {
        if (preg_match('#^HTTP/\S+\s+(\d+)#', $header, $m)) {
            $httpCode = (int)$m[1];
        }
    }
    return [(string)$raw, $httpCode];
}

// 与 jianjia_chat.py::_ollama_chat() 保持一致（think 关闭、temperature/num_predict 默认值、
// <think> 块清理、角色名前缀清理），否则调试台看到的回复和游戏里实际发出的不是一回事。
function jj_call_ollama(string $url, string $model, array $messages, float $temperature = 0.8, int $numPredict = 200): array
{
    $payload = json_encode([
        'model'    => $model,
        'stream'   => false,
        'think'    => false,
        'options'  => ['temperature' => $temperature, 'num_predict' => $numPredict],
        'messages' => $messages,
    ], JSON_UNESCAPED_UNICODE);

    if (function_exists('curl_init')) {
        [$raw, $httpCode] = jj_http_post_curl($url, $payload);
    } else {
        // 部分精简版 PHP（php-cli/低配 LNMP 环境）没装 ext-curl，退化到 stream context，
        // 这样调试台不依赖服务器是否装了 curl 扩展也能用。
        [$raw, $httpCode] = jj_http_post_stream($url, $payload);
    }
    $data = json_decode((string)$raw, true);
    if ($httpCode !== 200 || !isset($data['message']['content'])) {
        throw new RuntimeException("Ollama bad response (HTTP $httpCode): " . substr((string)$raw, 0, 500));
    }
    return $data;
}

// mbstring 不一定装了（和 ext-curl 一样，这台服务器上没有），退化到字节长度，
// 只是给调试台展示用，不需要精确的多字节字符数。
function jj_strlen(string $s): int
{
    return function_exists('mb_strlen') ? mb_strlen($s) : strlen($s);
}

function jj_clean_reply(string $content): string
{
    // 保险：即使 think=false，某些模型仍可能把思维链塞进正文
    $content = preg_replace('/<think>.*?<\/think>/su', '', $content) ?? $content;
    // 去掉模型偶尔加的 "角色名：" 前缀
    $content = preg_replace('/^\S{1,8}[：:]\s*/u', '', trim($content)) ?? $content;
    return trim($content);
}

// ── 鉴权：必须先把 jianjia_debug_config.php 里的 token 改成真实值，否则一律拒绝 ──
if (!isset($config['token']) || $config['token'] === 'change-me') {
    jj_json_out(500, ['error' => '请先复制 jianjia_debug_config.example.php 为 jianjia_debug_config.php 并设置真实 token']);
}

$token = $_GET['token'] ?? $_POST['token'] ?? ($_SERVER['HTTP_X_AUTH_TOKEN'] ?? '');
if (!hash_equals((string)$config['token'], (string)$token)) {
    jj_json_out(403, ['error' => 'invalid token']);
}

// ── JSON API：POST 请求，或 GET 带 ?api=1 ──────────────────────────────────────
$isApi = ($_SERVER['REQUEST_METHOD'] === 'POST') || isset($_GET['api']);
if ($isApi) {
    $body = json_decode((string)(file_get_contents('php://input') ?: '{}'), true);
    if (!is_array($body)) {
        $body = [];
    }
    $message = trim((string)($body['message'] ?? $_POST['message'] ?? $_GET['message'] ?? ''));
    if ($message === '') {
        jj_json_out(400, ['error' => 'message is required']);
    }

    $history = $body['history'] ?? [];
    if (!is_array($history)) {
        $history = [];
    }

    $model       = (string)($body['model'] ?? $config['model']);
    $ollamaUrl   = (string)($body['ollama_url'] ?? $config['ollama_url']);
    $extraSystem = trim((string)($body['extra_system'] ?? ''));
    $temperature = isset($body['temperature']) ? (float)$body['temperature'] : 0.8;
    $numPredict  = isset($body['num_predict']) ? (int)$body['num_predict'] : 200;

    try {
        $system = jj_load_system_prompt($SOUL_FILE, $KNOWLEDGE_DIR, (string)$config['bot_name']);
    } catch (Throwable $e) {
        jj_json_out(500, ['error' => $e->getMessage(), 'stage' => 'load_system_prompt']);
    }
    if ($extraSystem !== '') {
        $system .= "\n\n" . $extraSystem;
    }

    $messages = [['role' => 'system', 'content' => $system]];
    foreach ($history as $turn) {
        if (is_array($turn) && isset($turn['role'], $turn['content'])) {
            $messages[] = ['role' => (string)$turn['role'], 'content' => (string)$turn['content']];
        }
    }
    $messages[] = ['role' => 'user', 'content' => $message];

    $t0 = microtime(true);
    try {
        $result = jj_call_ollama($ollamaUrl, $model, $messages, $temperature, $numPredict);
    } catch (Throwable $e) {
        jj_json_out(502, ['error' => $e->getMessage(), 'stage' => 'call_ollama']);
    }
    $elapsedMs = (int)round((microtime(true) - $t0) * 1000);

    jj_json_out(200, [
        'reply'               => jj_clean_reply((string)$result['message']['content']),
        'model'                => $model,
        'ollama_url'           => $ollamaUrl,
        'system_prompt_chars'  => jj_strlen($system),
        'elapsed_ms'           => $elapsedMs,
    ]);
}

// ── 浏览器测试页面：简单的单页聊天界面，对话历史保存在前端 JS 里 ──────────────
header('Content-Type: text/html; charset=utf-8');
$tokenAttr = htmlspecialchars((string)$token, ENT_QUOTES);
$modelAttr = htmlspecialchars((string)$config['model'], ENT_QUOTES);
$urlAttr   = htmlspecialchars((string)$config['ollama_url'], ENT_QUOTES);
?><!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>蒹葭 AI 调试台</title>
<style>
  body { font-family: system-ui, "Microsoft YaHei", sans-serif; max-width: 720px; margin: 24px auto; padding: 0 16px; }
  h1 { font-size: 18px; }
  #log { border: 1px solid #ccc; border-radius: 8px; padding: 12px; height: 420px; overflow-y: auto; margin-bottom: 12px; background: #fafafa; }
  .msg { margin: 8px 0; white-space: pre-wrap; }
  .msg .role { font-weight: bold; margin-right: 6px; }
  .msg.user .role { color: #2563eb; }
  .msg.assistant .role { color: #db2777; }
  .msg.error .role { color: #dc2626; }
  .meta { color: #888; font-size: 12px; margin-left: 6px; }
  form { display: flex; gap: 8px; margin-bottom: 12px; }
  input[type=text] { flex: 1; padding: 8px; }
  button { padding: 8px 16px; }
  details { margin-bottom: 12px; font-size: 13px; color: #555; }
  details input { width: 100%; margin: 4px 0; padding: 4px; box-sizing: border-box; }
</style>
</head>
<body>
<h1>蒹葭 AI 调试台</h1>
<details>
  <summary>连接设置（可覆盖默认值，仅本次会话生效）</summary>
  <label>model <input type="text" id="model" value="<?= $modelAttr ?>"></label>
  <label>ollama_url <input type="text" id="ollamaUrl" value="<?= $urlAttr ?>"></label>
  <label>额外系统提示（临时追加，用于测试特定场景） <input type="text" id="extraSystem" value=""></label>
</details>
<div id="log"></div>
<form id="chatForm">
  <input type="text" id="message" placeholder="输入测试问题…" autocomplete="off" required>
  <button type="submit">发送</button>
  <button type="button" id="clearBtn">清空对话</button>
</form>

<script>
const TOKEN = <?= json_encode($tokenAttr) ?>;
const log = document.getElementById('log');
let history = [];

function appendMsg(role, text, meta) {
  const div = document.createElement('div');
  div.className = 'msg ' + role;
  const roleLabel = { user: '你', assistant: '蒹葭', error: '错误' }[role] || role;

  const roleSpan = document.createElement('span');
  roleSpan.className = 'role';
  roleSpan.textContent = roleLabel + '：';
  div.appendChild(roleSpan);
  div.appendChild(document.createTextNode(text));

  if (meta) {
    const metaSpan = document.createElement('span');
    metaSpan.className = 'meta';
    metaSpan.textContent = meta;
    div.appendChild(metaSpan);
  }

  log.appendChild(div);
  log.scrollTop = log.scrollHeight;
}

document.getElementById('chatForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const input = document.getElementById('message');
  const message = input.value.trim();
  if (!message) return;
  input.value = '';
  appendMsg('user', message);

  const body = {
    message,
    history,
    model: document.getElementById('model').value.trim(),
    ollama_url: document.getElementById('ollamaUrl').value.trim(),
    extra_system: document.getElementById('extraSystem').value.trim(),
  };

  try {
    const resp = await fetch('?token=' + encodeURIComponent(TOKEN), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    const data = await resp.json();
    if (!resp.ok) {
      appendMsg('error', data.error || ('HTTP ' + resp.status));
      return;
    }
    appendMsg('assistant', data.reply, ' (' + data.elapsed_ms + 'ms, system=' + data.system_prompt_chars + ' chars)');
    history.push({ role: 'user', content: message });
    history.push({ role: 'assistant', content: data.reply });
  } catch (err) {
    appendMsg('error', String(err));
  }
});

document.getElementById('clearBtn').addEventListener('click', () => {
  history = [];
  log.innerHTML = '';
});
</script>
</body>
</html>
