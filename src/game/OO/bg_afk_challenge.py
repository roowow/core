#!/usr/bin/env python3
"""
BG AFK Challenge Service
Listens on Redis for AFK challenge requests from the game server,
calls Ollama to generate questions and validate player responses.

Channels:
  web_chat:afk_req:<realmId>  - game → this service (challenge_req, player_resp)
  web_chat:afk_out:<realmId>  - this service → game (afk_challenge, afk_verdict)
"""

import json
import threading
import time
import signal
import sys
import argparse
import logging

try:
    import redis
except ImportError:
    sys.exit("Missing dependency: pip install redis")

try:
    import requests
except ImportError:
    sys.exit("Missing dependency: pip install requests")

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger(__name__)


# ── config ────────────────────────────────────────────────────────────────────

REDIS_HOST   = "127.0.0.1"
REDIS_PORT   = 6379
REALM_ID     = 1
OLLAMA_URL   = "http://192.168.1.231:11434/api/chat"
OLLAMA_MODEL = "qwen2.5:7b"   # change to larger model if available

# pending: player_name -> {question, answer, ts}
_pending: dict = {}
_lock = threading.Lock()


# ── Ollama helpers ────────────────────────────────────────────────────────────

def _ollama_chat(messages: list, timeout: int = 30) -> str:
    resp = requests.post(
        OLLAMA_URL,
        json={
            "model": OLLAMA_MODEL,
            "messages": messages,
            "stream": False,
            "options": {"temperature": 0.2, "num_predict": 128},
        },
        timeout=timeout,
    )
    resp.raise_for_status()
    return resp.json()["message"]["content"].strip()


_FALLBACK_QUESTIONS = [
    ("3加5等于几？", "8"),
    ("9减4等于几？", "5"),
    ("6加7等于几？", "13"),
    ("8减3等于几？", "5"),
    ("4加6等于几？", "10"),
    ("7加2等于几？", "9"),
    ("10减6等于几？", "4"),
    ("5加8等于几？", "13"),
    ("9减2等于几？", "7"),
    ("3加9等于几？", "12"),
]


def generate_question() -> tuple[str, str]:
    """Ask Ollama for a simple Chinese question; fall back to hardcoded pool on failure."""
    try:
        raw = _ollama_chat([{
            "role": "user",
            "content": (
                "请生成一道极简单的数学题，用于验证玩家是否是真人。\n"
                "要求：\n"
                "- 只涉及两个个位数的加法或减法，结果为正整数\n"
                "- 以JSON格式回复，格式：{\"question\":\"题目（如：3加5等于几？）\",\"answer\":\"数字\"}\n"
                "- 只输出JSON，不要任何其他内容"
            ),
        }])
        data = json.loads(raw)
        return str(data["question"]), str(data["answer"])
    except Exception as e:
        log.warning("Ollama generate_question failed (%s), using fallback pool", e)
        import random
        return random.choice(_FALLBACK_QUESTIONS)


_CN_DIGITS = {"零":0,"一":1,"二":2,"三":3,"四":4,"五":5,
               "六":6,"七":7,"八":8,"九":9,"十":10,
               "十一":11,"十二":12,"十三":13,"十四":14,"十五":15}

def _simple_match(expected: str, response: str) -> bool:
    """Fast numeric match covering Arabic and common Chinese digit forms."""
    response = response.strip()
    if response == expected:
        return True
    # Try converting Chinese numerals
    if response in _CN_DIGITS:
        return str(_CN_DIGITS[response]) == expected
    return False


def validate_answer(question: str, expected: str, player_response: str) -> bool:
    """Validate player response; use Ollama for natural language, fall back to simple match."""
    if _simple_match(expected, player_response):
        return True
    try:
        raw = _ollama_chat([{
            "role": "user",
            "content": (
                f"题目：{question}\n"
                f"正确答案：{expected}\n"
                f"玩家回答：{player_response}\n\n"
                "玩家的回答是否正确？请考虑中文数字（三、七）和阿拉伯数字（3、7）等不同写法。\n"
                "只回复 true 或 false，不要其他内容。"
            ),
        }])
        return raw.lower().startswith("true")
    except Exception as e:
        log.warning("Ollama validate_answer failed (%s), using simple match only", e)
        return False


# ── message handlers ──────────────────────────────────────────────────────────

def handle_challenge_req(r: "redis.Redis", player: str, out_key: str) -> None:
    log.info("Generating challenge for player: %s", player)
    try:
        question, answer = generate_question()
    except Exception as e:
        log.error("Failed to generate question for %s: %s", player, e)
        return

    with _lock:
        _pending[player] = {"question": question, "answer": answer, "ts": time.time()}

    payload = json.dumps({"type": "afk_challenge", "player_name": player, "question": question})
    r.publish(out_key, payload)
    log.info("Challenge sent to %s: %s (answer: %s)", player, question, answer)


def handle_player_response(r: "redis.Redis", player: str, response: str, out_key: str) -> None:
    with _lock:
        entry = _pending.pop(player, None)

    if entry is None:
        log.warning("No pending challenge for player: %s", player)
        return

    log.info("Validating response from %s: '%s' (expected: %s)", player, response, entry["answer"])
    try:
        passed = validate_answer(entry["question"], entry["answer"], response)
    except Exception as e:
        log.error("Ollama validation error for %s: %s", player, e)
        passed = False

    payload = json.dumps({"type": "afk_verdict", "player_name": player, "pass": passed})
    r.publish(out_key, payload)
    log.info("Verdict for %s: %s", player, "PASS" if passed else "FAIL")


def process_message(r: "redis.Redis", data: bytes, out_key: str) -> None:
    try:
        msg = json.loads(data)
    except json.JSONDecodeError as e:
        log.error("Invalid JSON: %s", e)
        return

    msg_type = msg.get("type")
    player   = msg.get("player_name", "")

    if not player:
        return

    if msg_type == "afk_challenge_req":
        handle_challenge_req(r, player, out_key)
    elif msg_type == "afk_response":
        handle_player_response(r, player, msg.get("response", ""), out_key)
    else:
        log.debug("Ignored message type: %s", msg_type)


# ── cleanup expired pending ───────────────────────────────────────────────────

def cleanup_loop(ttl: float = 120.0) -> None:
    while True:
        time.sleep(30)
        cutoff = time.time() - ttl
        with _lock:
            expired = [p for p, e in _pending.items() if e["ts"] < cutoff]
            for p in expired:
                del _pending[p]
                log.info("Expired pending challenge for %s", p)


# ── main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="BG AFK Challenge Service")
    parser.add_argument("--redis-host", default=REDIS_HOST)
    parser.add_argument("--redis-port", type=int, default=REDIS_PORT)
    parser.add_argument("--realm-id",   type=int, default=REALM_ID)
    parser.add_argument("--model",      default=OLLAMA_MODEL)
    parser.add_argument("--ollama-url", default=OLLAMA_URL)
    args = parser.parse_args()

    global OLLAMA_MODEL, OLLAMA_URL
    OLLAMA_MODEL = args.model
    OLLAMA_URL   = args.ollama_url

    in_key  = f"web_chat:afk_req:{args.realm_id}"
    out_key = f"web_chat:afk_out:{args.realm_id}"

    r_pub = redis.Redis(host=args.redis_host, port=args.redis_port, decode_responses=True)
    r_sub = redis.Redis(host=args.redis_host, port=args.redis_port, decode_responses=True)
    pubsub = r_sub.pubsub()
    pubsub.subscribe(in_key)

    threading.Thread(target=cleanup_loop, daemon=True).start()

    def _shutdown(sig, frame):
        log.info("Shutting down.")
        pubsub.unsubscribe()
        sys.exit(0)

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    log.info("BG AFK Challenge service started (model=%s realm=%d)", OLLAMA_MODEL, args.realm_id)
    log.info("Listening on %s, publishing to %s", in_key, out_key)

    for msg in pubsub.listen():
        if msg["type"] != "message":
            continue
        threading.Thread(
            target=process_message,
            args=(r_pub, msg["data"], out_key),
            daemon=True,
        ).start()


if __name__ == "__main__":
    main()
