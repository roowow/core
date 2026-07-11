<?php
/**
 * jianjia_debug.php 的本地配置。
 * 使用方法：复制为 jianjia_debug_config.php（同目录），填入真实值，不要提交到仓库。
 */

return [
    // 随便生成一个长随机字符串，访问 jianjia_debug.php 时通过 ?token=xxx 传入
    'token'      => 'CHANGE_ME_TO_A_RANDOM_SECRET',

    // Ollama 的 HTTP API 地址；如果本 PHP 文件和 Ollama 不在同一台机器上，
    // 填内网可达的地址，例如 http://192.168.1.231:11434/api/chat
    'ollama_url' => 'http://127.0.0.1:11434/api/chat',

    'model'      => 'qwen3:32b',
    'bot_name'   => '蒹葭',
];
