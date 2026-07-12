#!/usr/bin/env python3
"""
蒹葭 AI Companion Service
Bridges player whispers to 蒹葭 (in-game bot) with an Ollama-powered AI.

Channels:
  web_chat:jianjia_in:<realmId>   - game → this service  {"sender":"name","message":"..."}
  web_chat:jianjia_out:<realmId>  - this service → game  {"target":"name","message":"..."}
"""

import json
import os
import re
import threading
import time
import signal
import sys
import argparse
import logging
from collections import deque

try:
    import redis
except ImportError:
    sys.exit("Missing dependency: pip install redis")

try:
    import requests
except ImportError:
    sys.exit("Missing dependency: pip install requests")

try:
    import pymysql
    import pymysql.cursors
    _PYMYSQL_OK = True
except ImportError:
    _PYMYSQL_OK = False

try:
    import tomllib                        # Python 3.11+
except ImportError:
    try:
        import tomli as tomllib           # type: ignore[no-redef]
    except ImportError:
        sys.exit("Missing dependency: pip install tomli  (Python < 3.11 requires tomli)")

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger(__name__)


# ── defaults ───────────────────────────────────────────────────────────────────

REDIS_HOST   = "192.168.1.231"
REDIS_PORT   = 6379
REALM_ID     = 1
OLLAMA_URL   = "http://192.168.1.231:11434/api/chat"
OLLAMA_MODEL = "qwen3:32b"
# Sent as num_ctx on every request (see _ollama_chat) — this isn't just a display
# estimate, it's what actually gets allocated. Without it, Ollama falls back to its
# own runtime default (often 2048-4096), which can silently truncate the prompt well
# below what the model architecture supports (qwen3:32b supports up to 40960 — check
# `ollama show <model>` for the real ceiling before raising this; more context = more
# VRAM for the KV cache, so verify it doesn't push Ollama into OOM).
OLLAMA_CONTEXT_TOKENS = 16384
# How many Ollama requests are allowed in flight at once, across every realm/thread in
# this process. Ollama itself reports "Parallel:1" for this model+GPU (confirmed via
# server log: a single 32B model at Q4 barely fits a 24GB card once num_ctx KV-cache is
# counted) — sending it concurrent requests anyway doesn't get more throughput, it just
# makes Ollama try to spin up a second full runner, thrash for VRAM, partially spill
# layers to CPU, and produce exactly the 30-40s stalls / 500s seen in production. Every
# handler spawns its own daemon thread with no other rate limiting, so this semaphore is
# the only thing standing between "one client at a time" and "however many messages
# happened to arrive in the same second."

# How many message turns to keep per player (user+assistant pairs)
MAX_HISTORY_TURNS = 10
# How many recent party/raid messages to pull from logs DB as conversation context
GROUP_HISTORY_LINES = 10
# Seconds of inactivity before clearing a player's conversation context
HISTORY_TTL = 1800  # 30 minutes

# ── per-realm DB connections ───────────────────────────────────────────────────

_logs_dbs: dict[int, "pymysql.Connection"] = {}   # realm_id → logs DB connection
_world_dbs: dict[int, "pymysql.Connection"] = {}  # realm_id → world DB connection
_MSG_RE = re.compile(r'^\[[^\]]+\]\s*.*?:\d+\s*:\s*(.*)$', re.DOTALL)


def _connect_db(cfg: dict, label: str) -> "pymysql.Connection | None":
    try:
        conn = pymysql.connect(
            host=cfg["host"], port=int(cfg.get("port", 3306)),
            user=cfg["user"], password=cfg["pass"], database=cfg["name"],
            charset="utf8mb4", cursorclass=pymysql.cursors.Cursor,
            connect_timeout=5, autocommit=True,
        )
        log.info("%s connected (%s/%s)", label, cfg["host"], cfg["name"])
        return conn
    except Exception as e:
        log.warning("%s unavailable: %s", label, e)
        return None


def _init_realm_dbs(realms_cfg: dict) -> None:
    if not _PYMYSQL_OK:
        log.warning("pymysql not installed — DB features disabled. pip install pymysql")
        return
    for realm_str, realm_cfg in realms_cfg.items():
        try:
            realm_id = int(realm_str)
        except ValueError:
            log.warning("Invalid realm key in config: %r (must be integer)", realm_str)
            continue
        if "logs_db" in realm_cfg:
            conn = _connect_db(realm_cfg["logs_db"], f"Realm {realm_id} logs DB")
            if conn:
                _logs_dbs[realm_id] = conn
        if "world_db" in realm_cfg:
            conn = _connect_db(realm_cfg["world_db"], f"Realm {realm_id} world DB")
            if conn:
                _world_dbs[realm_id] = conn


def _get_group_history(group_id: int, bot_name: str, context: str, realm_id: int = 0) -> list[dict]:
    """Return recent party/raid messages as Ollama-format message list."""
    db = _logs_dbs.get(realm_id)
    if not db or not group_id:
        return []
    tag_prefix = "Group" if context == "party" else "Raid"
    like_pat = f'[{tag_prefix}:{group_id}]%'
    try:
        with db.cursor() as cur:
            cur.execute(
                "SELECT name, text FROM logs_player "
                "WHERE type='Chat' AND text LIKE %s "
                "ORDER BY id DESC LIMIT %s",
                (like_pat, GROUP_HISTORY_LINES),
            )
            rows = list(reversed(cur.fetchall()))
    except Exception as e:
        log.warning("Group history query failed: %s", e)
        return []
    messages = []
    for name, text in rows:
        m = _MSG_RE.match(text)
        message = m.group(1).strip() if m else text
        if not message:
            continue
        if name == bot_name:
            messages.append({"role": "assistant", "content": message})
        else:
            messages.append({"role": "user", "content": f"{name}：{message}"})
    return messages


def _load_system_prompt(soul_file: str, knowledge_dir: str) -> str:
    """Load soul file + all .md files in knowledge_dir (sorted), concatenated."""
    base = os.path.dirname(__file__)
    if not soul_file:
        sys.exit("Config error: [service] soul_file is required but not set.")
    if not os.path.isabs(soul_file):
        soul_file = os.path.join(base, soul_file)
    if not os.path.isfile(soul_file):
        sys.exit(f"Soul file not found: {soul_file}")
    with open(soul_file, encoding="utf-8") as f:
        soul = f.read().strip()
    if not knowledge_dir:
        return soul
    kdir = knowledge_dir if os.path.isabs(knowledge_dir) else os.path.join(base, knowledge_dir)
    if not os.path.isdir(kdir):
        log.warning("Knowledge dir not found: %s — using soul file only", kdir)
        return soul
    parts = [soul]
    for fname in sorted(os.listdir(kdir)):
        if fname.endswith(".md"):
            with open(os.path.join(kdir, fname), encoding="utf-8") as f:
                content = f.read().strip()
            if content:
                parts.append(content)
    log.info("Knowledge dir %s: loaded %d module(s)", kdir, len(parts) - 1)
    return "\n\n".join(parts)


_SYSTEM_PROMPT_TEMPLATE: str = ""  # loaded in main() via _load_system_prompt()


def _estimate_tokens(text: str) -> int:
    """Rough token-count estimate, not an exact tokenizer count (Ollama's HTTP API
    doesn't expose a tokenize-only endpoint, so this is a cheap approximation only,
    good enough to eyeball how close the base prompt is to the configured context
    window). CJK text runs closer to ~1 token per character with Qwen's tokenizer;
    ASCII runs closer to ~4 chars per token. Split by rough byte-width as a proxy.
    """
    cjk = sum(1 for ch in text if ord(ch) > 0x2E80)  # CJK/punctuation-ish range
    other = len(text) - cjk
    return cjk + other // 4


def _reload_system_prompt(svc: dict) -> None:
    """(Re)load soul + knowledge into _SYSTEM_PROMPT_TEMPLATE. Used at startup and
    again on SIGHUP, so editing jianjia_soul.md / jianjia_knowledge/*.md takes effect
    without restarting the service (in-flight conversations, Redis connection, etc.
    are left untouched). Code changes to this .py file still need a real restart —
    Python can't safely hot-swap running function bodies.
    """
    global _SYSTEM_PROMPT_TEMPLATE
    try:
        template = _load_system_prompt(
            svc.get("soul_file", ""),
            svc.get("knowledge_dir", ""),
        )
    except SystemExit as e:
        # _load_system_prompt calls sys.exit() on a missing/misconfigured soul file —
        # fine at startup (nothing is running yet), but a reload failure must not take
        # down an already-running service. Log and keep serving with the old prompt.
        log.error("System prompt reload failed, keeping previous version: %s", e)
        return

    server_name  = svc.get("server_name",  "").strip()
    server_phase = svc.get("server_phase", "").strip()
    if server_name or server_phase:
        parts = []
        if server_name:
            parts.append(f"当前服务器：{server_name}")
        if server_phase:
            parts.append(f"当前阶段：{server_phase}")
        template += "\n\n[" + "，".join(parts) + "。回答玩家问题时以此为准。]"

    _SYSTEM_PROMPT_TEMPLATE = template
    n_chars  = len(_SYSTEM_PROMPT_TEMPLATE)
    n_tokens = _estimate_tokens(_SYSTEM_PROMPT_TEMPLATE)
    pct = n_tokens / OLLAMA_CONTEXT_TOKENS * 100
    log.info("System prompt (re)loaded (%d chars, ~%d tokens est., ~%.0f%% of %d-token context window)",
             n_chars, n_tokens, pct, OLLAMA_CONTEXT_TOKENS)
    if pct > 50:
        log.warning("Base system prompt alone is using ~%.0f%% of the context window — "
                    "conversation history / world-channel transcript / verifier calls "
                    "stack on top of this and may get silently truncated by Ollama.", pct)


# Per-event instruction snippets (world/guild/group wake-up rules, the summon prompt,
# the verifier prompt) — kept as .md files instead of inline Python f-strings so tuning
# a rule's wording doesn't require a full restart: `python3 jianjia_chat.py --reload`
# picks these up the same way it picks up soul/knowledge changes. Code changes to
# which *variables* get substituted still need a restart, but the *wording* doesn't.
_PROMPT_TEMPLATES: dict[str, str] = {}
_REQUIRED_PROMPT_TEMPLATES = {
    "group_wakeup", "guild_wakeup", "world_question", "world_summon", "verify_grounded",
}


def _load_prompt_templates(prompts_dir: str) -> dict[str, str]:
    base = os.path.dirname(__file__)
    pdir = prompts_dir if os.path.isabs(prompts_dir) else os.path.join(base, prompts_dir)
    if not os.path.isdir(pdir):
        sys.exit(f"Prompt templates dir not found: {pdir}")
    templates: dict[str, str] = {}
    for fname in sorted(os.listdir(pdir)):
        if fname.endswith(".md"):
            with open(os.path.join(pdir, fname), encoding="utf-8") as f:
                templates[fname[:-3]] = f.read().strip()
    return templates


def _reload_prompt_templates(svc: dict) -> None:
    """(Re)load jianjia_prompts/*.md. Same restart-free pattern as _reload_system_prompt:
    used at startup and again on SIGHUP. Fails closed — a bad reload keeps serving with
    whatever templates already loaded successfully, rather than crashing or leaving a
    call site with no template to `.format()` against.
    """
    global _PROMPT_TEMPLATES
    try:
        templates = _load_prompt_templates(svc.get("prompts_dir", "jianjia_prompts"))
    except SystemExit as e:
        log.error("Prompt templates reload failed, keeping previous versions: %s", e)
        return
    missing = _REQUIRED_PROMPT_TEMPLATES - templates.keys()
    if missing:
        log.error("Prompt templates missing %s, keeping previous versions.", sorted(missing))
        return
    _PROMPT_TEMPLATES = templates
    log.info("Prompt templates (re)loaded: %s", ", ".join(sorted(templates)))


def _default_pid_file() -> str:
    return os.path.join(os.path.dirname(__file__), "jianjia.pid")


def _send_reload_signal(pid_file: str) -> None:
    """`--reload` CLI entry point: signal an already-running instance instead of
    starting a second one. Wraps `kill -HUP $(cat pid_file)` so there's nothing to
    remember beyond `python3 jianjia_chat.py --reload`.
    """
    try:
        with open(pid_file, encoding="utf-8") as f:
            pid = int(f.read().strip())
    except FileNotFoundError:
        sys.exit(f"No pid file at {pid_file} — is the service running?")
    except ValueError:
        sys.exit(f"Pid file {pid_file} does not contain a valid pid.")

    if not hasattr(signal, "SIGHUP"):
        sys.exit("--reload needs SIGHUP, which isn't available on this platform.")

    try:
        os.kill(pid, signal.SIGHUP)
    except ProcessLookupError:
        sys.exit(f"No running process with pid {pid} — stale pid file at {pid_file}?")
    except PermissionError:
        sys.exit(f"Permission denied sending SIGHUP to pid {pid}.")

    print(f"Sent reload signal to pid {pid}.")

# ── world DB — game knowledge lookup ─────────────────────────────────────────

_QUALITY_NAMES  = {0: "差", 1: "普通", 2: "非凡", 3: "稀有", 4: "史诗", 5: "传说"}
_CREATURE_RANKS = {1: "精英", 2: "稀有精英", 3: "首领"}

# Common words to skip so we don't flood DB with non-entity terms
_SKIP_WORDS = frozenset({
    "什么", "怎么", "哪里", "哪儿", "知道", "可以", "没有", "玩家", "任务",
    "物品", "装备", "怎样", "如何", "在哪", "为什么", "告诉", "一些", "很多",
    "一个", "这个", "那个", "我的", "你的", "他的", "我们", "你们", "所有",
    "战场", "战斗", "队友", "副本", "地图", "魔兽", "服务器", "角色", "不知",
    "请问", "帮我", "帮忙", "可能", "需要", "现在", "今天", "一起", "然后",
})


def _extract_game_keywords(text: str) -> list[str]:
    """Extract 2-6 char Chinese word groups from text as potential game entity names."""
    candidates = re.findall(r'[一-鿿]{2,6}', text)
    seen: set[str] = set()
    result: list[str] = []
    for word in candidates:
        if word not in seen and word not in _SKIP_WORDS:
            seen.add(word)
            result.append(word)
            if len(result) >= 4:
                break
    return result


def _search_world_db(keywords: list[str], realm_id: int = 0) -> str:
    """Search item/quest/NPC tables for keywords; return a formatted context block."""
    db = _world_dbs.get(realm_id)
    if not db or not keywords:
        return ""
    lines: list[str] = []
    seen: set[str] = set()
    for kw in keywords:
        like = f"%{kw}%"
        try:
            with db.cursor() as cur:
                # Items — prefer Chinese name from locales_item (name_loc4 = zhCN)
                cur.execute(
                    "SELECT COALESCE(NULLIF(li.name_loc4,''), it.name), "
                    "it.item_level, it.required_level, it.quality "
                    "FROM item_template it "
                    "LEFT JOIN locales_item li ON li.entry = it.entry "
                    "WHERE it.name LIKE %s OR li.name_loc4 LIKE %s LIMIT 2",
                    (like, like),
                )
                for name, ilvl, req_lvl, quality in cur.fetchall():
                    key = f"i:{name}"
                    if key in seen:
                        continue
                    seen.add(key)
                    q = _QUALITY_NAMES.get(quality, "")
                    parts = [f"物品「{name}」"]
                    if q:
                        parts.append(q)
                    if ilvl:
                        parts.append(f"物品等级{ilvl}")
                    if req_lvl:
                        parts.append(f"需要{req_lvl}级")
                    lines.append("·" + " ".join(parts))

                # Quests — prefer Chinese title from locales_quest (Title_loc4 = zhCN)
                cur.execute(
                    "SELECT COALESCE(NULLIF(lq.Title_loc4,''), qt.Title), qt.QuestLevel "
                    "FROM quest_template qt "
                    "LEFT JOIN locales_quest lq ON lq.entry = qt.entry "
                    "WHERE qt.Title LIKE %s OR lq.Title_loc4 LIKE %s LIMIT 2",
                    (like, like),
                )
                for title, qlvl in cur.fetchall():
                    key = f"q:{title}"
                    if key in seen:
                        continue
                    seen.add(key)
                    parts = [f"任务「{title}」"]
                    if qlvl:
                        parts.append(f"等级{qlvl}")
                    lines.append("·" + " ".join(parts))

                # NPCs — prefer Chinese name from locales_creature (name_loc4 = zhCN)
                cur.execute(
                    "SELECT COALESCE(NULLIF(lc.name_loc4,''), ct.name), "
                    "COALESCE(NULLIF(lc.subname_loc4,''), ct.subname), "
                    "ct.level_min, ct.level_max, ct.rank "
                    "FROM creature_template ct "
                    "LEFT JOIN locales_creature lc ON lc.entry = ct.entry "
                    "WHERE ct.name LIKE %s OR lc.name_loc4 LIKE %s LIMIT 2",
                    (like, like),
                )
                for name, subname, minlvl, maxlvl, rank in cur.fetchall():
                    key = f"n:{name}"
                    if key in seen:
                        continue
                    seen.add(key)
                    parts = [f"NPC「{name}」"]
                    if subname:
                        parts.append(f"({subname})")
                    if minlvl:
                        lvl = f"{minlvl}" if minlvl == maxlvl else f"{minlvl}-{maxlvl}级"
                        parts.append(lvl)
                    r = _CREATURE_RANKS.get(rank, "")
                    if r:
                        parts.append(r)
                    lines.append("·" + " ".join(parts))

        except Exception as e:
            log.warning("World DB search error for '%s': %s", kw, e)

    if not lines:
        return ""
    return "[游戏数据库参考（按需引用）：\n" + "\n".join(lines[:8]) + "\n]"


# ── player info lookup tables ──────────────────────────────────────────────────

_CLASS_NAMES: dict[int, str] = {
    1: "战士", 2: "圣骑士", 3: "猎人", 4: "盗贼", 5: "牧师",
    7: "萨满祭司", 8: "法师", 9: "术士", 11: "德鲁伊",
}
_RACE_NAMES: dict[int, str] = {
    1: "人类", 2: "兽人", 3: "矮人", 4: "暗夜精灵", 5: "亡灵",
    6: "牛头人", 7: "侏儒", 8: "巨魔",
}

def _fmt_player_info(level: int, cls: int, race: int, zone: str = "") -> str:
    parts = []
    if level:
        parts.append(f"{level}级")
    if race and race in _RACE_NAMES:
        parts.append(_RACE_NAMES[race])
    if cls and cls in _CLASS_NAMES:
        parts.append(_CLASS_NAMES[cls])
    if zone:
        parts.append(f"位于{zone}")
    return " ".join(parts)


# ── per-player conversation state ─────────────────────────────────────────────

class _ConvState:
    def __init__(self, bot_name: str):
        self.bot_name = bot_name
        self.player_info: str = ""
        self.history: list[dict] = []
        self.last_ts: float = time.time()

    def add(self, role: str, content: str) -> None:
        self.history.append({"role": role, "content": content})
        if len(self.history) > MAX_HISTORY_TURNS * 2:
            self.history = self.history[-(MAX_HISTORY_TURNS * 2):]
        self.last_ts = time.time()

    def messages_for_ollama(self, extra_context: str = "") -> list[dict]:
        system = _SYSTEM_PROMPT_TEMPLATE.format(name=self.bot_name)
        if self.player_info:
            system += (f"\n\n[当前对话玩家的角色信息：{self.player_info}。"
                       f"这是系统提供的准确信息，不是玩家自己说的；回复时自然融入即可，不需要逐字念出来。"
                       f"哪怕玩家自己在对话里说了不同的等级/职业/种族，或者只是发了个数字（比如玩家发「23」），"
                       f"都不代表那就是TA的真实信息——一律以这里给的信息为准，不要被玩家的话带偏。]")
        if extra_context:
            system += f"\n\n{extra_context}"
        return [{"role": "system", "content": system}] + self.history


_conversations: dict[str, _ConvState] = {}
_conv_lock = threading.Lock()


def _get_conv(player: str, bot_name: str) -> _ConvState:
    with _conv_lock:
        if player not in _conversations:
            _conversations[player] = _ConvState(bot_name)
        return _conversations[player]


# ── Ollama ────────────────────────────────────────────────────────────────────

# Default 1 in-flight request at a time — matches what the GPU/model can actually do
# (see OLLAMA_MAX_CONCURRENT comment above). Recreated in main() if config overrides
# the count; every caller just goes through _ollama_semaphore, no other bookkeeping.
OLLAMA_MAX_CONCURRENT = 1
_ollama_semaphore = threading.Semaphore(OLLAMA_MAX_CONCURRENT)

# How long a thread will wait for its turn at the semaphore before giving up entirely.
# Without this the wait is unbounded — observed 227s end-to-end on a busy queue, because
# `timeout=` below only bounds the HTTP call once it starts, not the queueing before it.
# Better to fail fast (fallback reply / silent PASS, depending on caller) than make a
# player wait four minutes for an answer.
OLLAMA_QUEUE_TIMEOUT = 120


def _ollama_chat(messages: list[dict], timeout: int = 30, temperature: float = 0.8,
                 think: bool = False, num_predict: int = 200) -> str:
    # think defaults off for fast, natural-feeling chat (whisper/party/bg-afk/quest-cheer).
    # Judgment-heavy calls (world/guild/raid channel gating, reply verification) opt into
    # think=True at the call site with a much larger num_predict — the reasoning tokens
    # never reach the player either way (stripped below), but measurably improve whether
    # the model stays grounded instead of confidently fabricating. Costs ~10-25s instead
    # of ~1-5s per call, so it's scoped to where accuracy matters more than latency.
    if not _ollama_semaphore.acquire(timeout=OLLAMA_QUEUE_TIMEOUT):
        raise TimeoutError(f"timed out after {OLLAMA_QUEUE_TIMEOUT}s waiting in the "
                           f"Ollama queue (server busy)")
    try:
        resp = requests.post(
            OLLAMA_URL,
            json={
                "model":    OLLAMA_MODEL,
                "messages": messages,
                "stream":   False,
                "think":    think,
                "options":  {"temperature": temperature, "num_predict": num_predict,
                            "num_ctx": OLLAMA_CONTEXT_TOKENS},
            },
            timeout=timeout,
        )
    finally:
        _ollama_semaphore.release()
    resp.raise_for_status()
    content = resp.json()["message"]["content"].strip()
    # strip <think>...</think> blocks in case the model ignores the flag
    content = re.sub(r"<think>.*?</think>", "", content, flags=re.DOTALL).strip()
    # strip "角色名：" prefix the model sometimes adds
    content = re.sub(r"^\S{1,8}[：:]\s*", "", content)
    return content


_FALLBACK_REPLIES = [
    "蒹葭苍苍，白露为霜……你说的话，我需要想一想。",
    "风吹芦苇，声声作响。稍等片刻，我整理一下思绪。",
    "水之湄，道阻且长。我暂时无法回应，请稍后再试。",
]
_fallback_idx = 0

# ── conversation log ──────────────────────────────────────────────────────────

_conv_log_file = None
_conv_log_lock = threading.Lock()

def _init_conv_log(log_dir: str) -> None:
    global _conv_log_file
    os.makedirs(log_dir, exist_ok=True)
    filename = time.strftime("JianJia_%Y-%m-%d_%H-%M-%S.jsonl")
    path = os.path.join(log_dir, filename)
    _conv_log_file = open(path, "a", encoding="utf-8")
    log.info("Conversation log: %s", path)

def _write_conv_log(realm: int, bot: str, event: str, outcome: str, player: str,
                    player_info: str = "", user_msg: str = "", ai_reply: str = "",
                    context: str = "", llm_calls: int = 0, latency_ms: float = 0.0) -> None:
    """Every handler exit point logs here — not just the ones that end in a published
    reply. `outcome` says what actually happened (answered / pass / filtered_* / etc.)
    and `llm_calls`/`latency_ms` say how much Ollama work it cost, so this file can
    answer "how often is the model actually being called" and "what fraction of
    world-channel traffic gets silently dropped" from grep/jq instead of guesswork.
    """
    if not _conv_log_file:
        return
    record = json.dumps({
        "ts":         time.strftime("%Y-%m-%d %H:%M:%S"),
        "realm":      realm,
        "bot":        bot,
        "event":      event,      # whisper | group_chat | channel_chat | bg_afk | quest_complete
        "context":    context,    # party | raid | bg | world | world-summon | guild | "" (whisper)
        "player":     player,
        "info":       player_info,
        "user":       user_msg,
        "outcome":    outcome,    # answered | pass | filtered_no_question | filtered_cooldown |
                                  # filtered_not_awake | verify_rejected | ollama_error | fallback
        "reply":      ai_reply,
        "llm_calls":  llm_calls,
        "latency_ms": round(latency_ms, 1),
    }, ensure_ascii=False)
    with _conv_log_lock:
        _conv_log_file.write(record + "\n")
        _conv_log_file.flush()


def _fallback() -> str:
    global _fallback_idx
    reply = _FALLBACK_REPLIES[_fallback_idx % len(_FALLBACK_REPLIES)]
    _fallback_idx += 1
    return reply


# ── message handler ───────────────────────────────────────────────────────────

_CHANNEL_NAMES = {"party": "小队", "raid": "团队", "bg": "战场", "world": "世界频道", "guild": "公会频道"}

# Regex pre-filter for world channel (question-based, no wake-up needed).
# Tuned against a real ~1450-line world-channel sample:
#   - dropped 呢\b: almost pure noise (sentence-final particle, e.g. "都还没下班呢"),
#     every genuine question that had 呢 also matched another keyword here.
#   - added 咋: colloquial 怎么 (咋办/咋回事/咋弄), several real questions used only this form.
_QUESTION_RE = re.compile(r'[？?]|吗\b|怎么|咋|哪里|哪儿|如何|能否|有没有|在哪|什么时候|为什么|是否|可以吗|怎样|几级|多少|什么是|哪个|会不会')

# Per-player cooldown for world channel replies (seconds)
_CHANNEL_REPLY_CD = 120
_channel_reply_ts: dict[str, float] = {}
_channel_reply_lock = threading.Lock()

# Rolling buffer of recent world-channel messages, per realm — every world message gets
# recorded here (not just ones that pass the question filter), so that when someone
# explicitly @-mentions the bot ("@蒹葭 你看下上面 xxx 的问题") she has something to look
# back at. Count-based cap, not time-based: simple, and "recent N messages" is what a
# human skimming the channel would actually use as context too.
_WORLD_RECENT_MAXLEN = 20
_world_recent: dict[int, deque] = {}
_world_recent_lock = threading.Lock()

# Separate, shorter cooldown for explicit @-mention summons (per summoner, not per the
# player being asked about) — this is an intentional direct request, not passive
# detection, so it doesn't need the same aggressive throttling, but still shouldn't be
# spammable.
_SUMMON_REPLY_CD = 60
_summon_reply_ts: dict[str, float] = {}
_summon_reply_lock = threading.Lock()


def _record_world_message(realm: int, sender: str, message: str) -> None:
    with _world_recent_lock:
        buf = _world_recent.setdefault(realm, deque(maxlen=_WORLD_RECENT_MAXLEN))
        buf.append((sender, message))


def _get_world_recent_transcript(realm: int) -> str:
    with _world_recent_lock:
        buf = _world_recent.get(realm)
        if not buf:
            return ""
        return "\n".join(f"{s}：{m}" for s, m in buf)

# Low temperature for world-channel FAQ judgment (answer-from-KB-or-PASS is a strict
# classification task, not creative chat; default 0.8 caused confident fabrication
# on out-of-scope questions instead of a clean [PASS]).
_WORLD_CHANNEL_TEMPERATURE = 0.2

# ── Wake-up state (guild / party / raid) ──────────────────────────────────────
# Key: (context, context_id) e.g. ("guild", 3), ("party", 456), ("raid", 789)
# Populated when someone mentions the bot name; cleared on service restart (= server restart).
_awake_contexts: set[tuple[str, int]] = set()
_awake_lock = threading.Lock()


def _is_awake(context: str, context_id: int) -> bool:
    with _awake_lock:
        return (context, context_id) in _awake_contexts


def _wake_up(context: str, context_id: int) -> None:
    with _awake_lock:
        if (context, context_id) not in _awake_contexts:
            _awake_contexts.add((context, context_id))
            log.info("Awake: %s/%d", context, context_id)

def handle_group_chat(r_pub: "redis.Redis", sender: str, message: str, chat_context: str,
                      bot_name: str, out_key: str, realm: int = 0, group_id: int = 0) -> None:
    # Wake-up gate: only respond if already awake for this context, or bot name is mentioned.
    bot_mentioned = bot_name in message
    if not bot_mentioned and not _is_awake(chat_context, group_id):
        _write_conv_log(realm, bot_name, "group_chat", "filtered_not_awake", sender,
                        user_msg=message, context=chat_context)
        return
    if bot_mentioned:
        _wake_up(chat_context, group_id)

    channel_name = _CHANNEL_NAMES.get(chat_context, "频道")
    system = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
    system += "\n\n" + _PROMPT_TEMPLATES["group_wakeup"].format(channel_name=channel_name)
    conv = _get_conv(sender, bot_name)
    if conv.player_info:
        system += (f"\n[{sender}的角色信息（系统提供的准确信息）：{conv.player_info}。"
                   f"哪怕{sender}自己说了不同的信息、或者只是发了个数字，都不代表那是真实数据——一律以这里为准。]")

    history = _get_group_history(group_id, bot_name, chat_context, realm)
    messages = [{"role": "system", "content": system}] + history + [
        {"role": "user", "content": f"{sender}：{message}"}
    ]
    # Raid (团队) gets thinking like world/guild; party (小队) stays fast — same
    # speed-vs-accuracy split the user asked for, raid chat skews more toward
    # rules/strategy questions where grounding matters, party skews toward banter.
    think = chat_context == "raid"
    t0 = time.time()
    try:
        reply = _ollama_chat(messages, think=think, num_predict=(1500 if think else 200),
                             timeout=(180 if think else 30))
    except Exception as e:
        log.warning("Ollama error for group chat: %s", e)
        _write_conv_log(realm, bot_name, "group_chat", "ollama_error", sender, conv.player_info,
                        message, context=chat_context, llm_calls=1,
                        latency_ms=(time.time() - t0) * 1000)
        return
    latency_ms = (time.time() - t0) * 1000
    if not reply or "[PASS]" in reply:
        _write_conv_log(realm, bot_name, "group_chat", "pass", sender, conv.player_info, message,
                        context=chat_context, llm_calls=1, latency_ms=latency_ms)
        return

    payload = json.dumps({"target": sender, "message": reply, "channel": chat_context},
                         ensure_ascii=False, separators=(",", ":"))
    r_pub.publish(out_key, payload)
    log.info("[%s] %s chat reply to %s: %s", bot_name, chat_context, sender, reply)
    _write_conv_log(realm, bot_name, "group_chat", "answered", sender, conv.player_info, message,
                    reply, context=chat_context, llm_calls=1, latency_ms=latency_ms)


def _verify_grounded(bot_name: str, question: str, reply: str) -> tuple[bool, float]:
    """Second-pass fact check for world-channel replies: does every factual claim in
    `reply` actually come from the knowledge base? A separate judge call reviewing an
    already-written answer against the reference text is a much narrower, more reliable
    task for the model than asking it to predict its own knowledge boundaries up front
    (which is what the main answer-or-PASS instruction already tries and still misses
    sometimes). Fails closed on any error or unclear verdict: staying silent is always
    safer than broadcasting an unverified answer to the whole world channel.

    Returns (passed, latency_ms) — latency is returned too so callers can log total
    Ollama time spent per event without timing this call separately at every site.
    """
    kb = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
    system = _PROMPT_TEMPLATES["verify_grounded"].format(
        bot_name=bot_name, kb=kb, question=question, reply=reply)
    t0 = time.time()
    try:
        # think=True is load-bearing here: without it this call rubber-stamps fabricated
        # answers as grounded (tested empirically), with it it correctly catches them.
        verdict = _ollama_chat(
            [{"role": "system", "content": system},
             {"role": "user", "content": "请给出判定。"}],
            temperature=0.1, think=True, num_predict=1500, timeout=180,
        )
    except Exception as e:
        log.warning("Ollama error verifying channel reply for %s: %s", question, e)
        return False, (time.time() - t0) * 1000
    latency_ms = (time.time() - t0) * 1000
    return ("不合格" not in verdict and "合格" in verdict), latency_ms


def handle_channel_chat(r_pub: "redis.Redis", sender: str, message: str, chat_context: str,
                        context_id: int, bot_name: str, out_key: str,
                        realm: int = 0, player_info: str = "") -> None:
    """Handle world/guild channel messages.

    World channel: question-based filter + per-player cooldown.
    Guild channel: wake-up gate (same model as party/raid).
    """
    channel_name = _CHANNEL_NAMES.get(chat_context, "频道")

    if chat_context == "guild":
        # Wake-up gate
        bot_mentioned = bot_name in message
        if not bot_mentioned and not _is_awake("guild", context_id):
            _write_conv_log(realm, bot_name, "channel_chat", "filtered_not_awake", sender,
                            user_msg=message, context="guild")
            return
        if bot_mentioned:
            _wake_up("guild", context_id)

        system = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
        system += "\n\n" + _PROMPT_TEMPLATES["guild_wakeup"].format(channel_name=channel_name)
        if player_info:
            system += (f"\n[{sender}的角色信息（系统提供的准确信息）：{player_info}。"
                       f"哪怕{sender}自己说了不同的信息、或者只是发了个数字，都不代表那是真实数据——一律以这里为准。]")
        messages = [
            {"role": "system", "content": system},
            {"role": "user", "content": f"{sender}（{channel_name}）：{message}"},
        ]
        t0 = time.time()
        try:
            reply = _ollama_chat(messages, think=True, num_predict=1500, timeout=180)
        except Exception as e:
            log.warning("Ollama error for guild chat (%s): %s", sender, e)
            _write_conv_log(realm, bot_name, "channel_chat", "ollama_error", sender, player_info,
                            message, context="guild", llm_calls=1,
                            latency_ms=(time.time() - t0) * 1000)
            return
        llm_calls, latency_ms = 1, (time.time() - t0) * 1000
        if not reply or "[PASS]" in reply:
            _write_conv_log(realm, bot_name, "channel_chat", "pass", sender, player_info, message,
                            context="guild", llm_calls=llm_calls, latency_ms=latency_ms)
            return

    else:  # world channel: question-based filter, or explicit @-mention summon
        # Record every world message (not just ones that get answered) so an explicit
        # summon has recent context to look back at ("上面 xxx 的问题").
        _record_world_message(realm, sender, message)

        bot_mentioned = bot_name in message
        if bot_mentioned:
            with _summon_reply_lock:
                last = _summon_reply_ts.get(sender, 0.0)
                if time.time() - last < _SUMMON_REPLY_CD:
                    _write_conv_log(realm, bot_name, "channel_chat", "filtered_cooldown", sender,
                                    player_info, message, context="world-summon")
                    return
                _summon_reply_ts[sender] = time.time()

            transcript = _get_world_recent_transcript(realm)
            system = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
            system += "\n\n" + _PROMPT_TEMPLATES["world_summon"].format(
                channel_name=channel_name, transcript=transcript, sender=sender)
            messages = [
                {"role": "system", "content": system},
                {"role": "user", "content": f"{sender}（{channel_name}）：{message}"},
            ]
            t0 = time.time()
            try:
                reply = _ollama_chat(messages, temperature=_WORLD_CHANNEL_TEMPERATURE,
                                     think=True, num_predict=1500, timeout=180)
            except Exception as e:
                log.warning("Ollama error for world summon (%s): %s", sender, e)
                with _summon_reply_lock:
                    _summon_reply_ts.pop(sender, None)
                _write_conv_log(realm, bot_name, "channel_chat", "ollama_error", sender, player_info,
                                message, context="world-summon", llm_calls=1,
                                latency_ms=(time.time() - t0) * 1000)
                return
            gen_latency_ms = (time.time() - t0) * 1000
            if not reply or "[PASS]" in reply:
                with _summon_reply_lock:
                    _summon_reply_ts.pop(sender, None)
                _write_conv_log(realm, bot_name, "channel_chat", "pass", sender, player_info, message,
                                context="world-summon", llm_calls=1, latency_ms=gen_latency_ms)
                return
            verified, verify_latency_ms = _verify_grounded(bot_name, message, reply)
            total_latency_ms = gen_latency_ms + verify_latency_ms
            if not verified:
                log.info("[%s] world summon reply to %s failed grounding check, discarding: %s",
                         bot_name, sender, reply)
                with _summon_reply_lock:
                    _summon_reply_ts.pop(sender, None)
                _write_conv_log(realm, bot_name, "channel_chat", "verify_rejected", sender,
                                player_info, message, reply, context="world-summon",
                                llm_calls=2, latency_ms=total_latency_ms)
                return

            payload = json.dumps({"target": sender, "message": reply, "channel": chat_context},
                                 ensure_ascii=False, separators=(",", ":"))
            r_pub.publish(out_key, payload)
            log.info("[%s] world summon reply to %s: %s", bot_name, sender, reply)
            _write_conv_log(realm, bot_name, "channel_chat", "answered", sender, player_info,
                            message, reply, context="world-summon", llm_calls=2,
                            latency_ms=total_latency_ms)
            return

        if not _QUESTION_RE.search(message):
            _write_conv_log(realm, bot_name, "channel_chat", "filtered_no_question", sender,
                            player_info, message, context="world")
            return
        with _channel_reply_lock:
            last = _channel_reply_ts.get(sender, 0.0)
            if time.time() - last < _CHANNEL_REPLY_CD:
                _write_conv_log(realm, bot_name, "channel_chat", "filtered_cooldown", sender,
                                player_info, message, context="world")
                return
            _channel_reply_ts[sender] = time.time()

        system = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
        system += "\n\n" + _PROMPT_TEMPLATES["world_question"].format(
            channel_name=channel_name, sender=sender)
        if player_info:
            system += (f"\n[{sender}的角色信息（系统提供的准确信息）：{player_info}。"
                       f"哪怕{sender}自己说了不同的信息、或者只是发了个数字，都不代表那是真实数据——一律以这里为准。]")
        messages = [
            {"role": "system", "content": system},
            {"role": "user", "content": f"{sender}（{channel_name}）：{message}"},
        ]
        t0 = time.time()
        try:
            # Low temperature + thinking: this is a strict "answer only from the
            # knowledge base, otherwise PASS" judgment call, not free-form chat.
            reply = _ollama_chat(messages, temperature=_WORLD_CHANNEL_TEMPERATURE,
                                 think=True, num_predict=1500, timeout=180)
        except Exception as e:
            log.warning("Ollama error for world chat (%s): %s", sender, e)
            with _channel_reply_lock:
                _channel_reply_ts.pop(sender, None)
            _write_conv_log(realm, bot_name, "channel_chat", "ollama_error", sender, player_info,
                            message, context="world", llm_calls=1,
                            latency_ms=(time.time() - t0) * 1000)
            return
        gen_latency_ms = (time.time() - t0) * 1000
        if not reply or "[PASS]" in reply:
            with _channel_reply_lock:
                _channel_reply_ts.pop(sender, None)
            _write_conv_log(realm, bot_name, "channel_chat", "pass", sender, player_info, message,
                            context="world", llm_calls=1, latency_ms=gen_latency_ms)
            return

        verified, verify_latency_ms = _verify_grounded(bot_name, message, reply)
        llm_calls = 2
        latency_ms = gen_latency_ms + verify_latency_ms
        if not verified:
            log.info("[%s] world reply to %s failed grounding check, discarding: %s",
                     bot_name, sender, reply)
            with _channel_reply_lock:
                _channel_reply_ts.pop(sender, None)
            _write_conv_log(realm, bot_name, "channel_chat", "verify_rejected", sender, player_info,
                            message, reply, context="world", llm_calls=llm_calls,
                            latency_ms=latency_ms)
            return

        log.info("[world] %s: %s", sender, message)

    payload = json.dumps({"target": sender, "message": reply, "channel": chat_context},
                         ensure_ascii=False, separators=(",", ":"))
    r_pub.publish(out_key, payload)
    log.info("[%s] %s reply to %s: %s", bot_name, chat_context, sender, reply)
    _write_conv_log(realm, bot_name, "channel_chat", "answered", sender, player_info, message,
                    reply, context=chat_context, llm_calls=llm_calls, latency_ms=latency_ms)


def handle_bg_afk(r_pub: "redis.Redis", sender: str, bot_name: str,
                  stage: int, afk_level: int, notice_type: str, player_info: str,
                  out_key: str, realm: int = 0) -> None:
    system = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
    system += "\n\n你现在在战场频道发言，所有队友都能看到。"
    if player_info:
        system += (f"\n[{sender}的角色信息（系统提供的准确信息）：{player_info}。"
                   f"哪怕{sender}自己说了不同的信息、或者只是发了个数字，都不代表那是真实数据——一律以这里为准。]")

    if notice_type == "warning":
        urgency_map = {1: "温柔地提醒", 2: "认真地警告", 3: "非常急切地催促"}
        urgency = urgency_map.get(stage, "提醒")
        prompt = f"请{urgency}{sender}：他们在战场中活动太少了，需要积极参与战斗或争夺目标，否则可能被移出战场。一句话，用你的性格说。"
    else:
        level_map = {1: "轻轻提醒", 2: "提醒", 3: "严肃警告"}
        urgency = level_map.get(afk_level, "提醒")
        prompt = f"请{urgency}{sender}：他们的战场活跃度不足，需要更积极地参与。一句话。"

    messages = [{"role": "system", "content": system},
                {"role": "user", "content": prompt}]
    t0 = time.time()
    try:
        reply = _ollama_chat(messages)
    except Exception as e:
        log.warning("Ollama error for bg_afk (%s): %s — signalling C++ fallback", sender, e)
        payload = json.dumps({"target": sender, "channel": "fallback",
                              "stage": stage, "afk_level": afk_level, "notice": notice_type},
                             ensure_ascii=False, separators=(",", ":"))
        r_pub.publish(out_key, payload)
        _write_conv_log(realm, bot_name, "bg_afk", "ollama_error", sender, player_info,
                        f"stage={stage} afk_level={afk_level} notice={notice_type}",
                        context="bg", llm_calls=1, latency_ms=(time.time() - t0) * 1000)
        return

    payload = json.dumps({"target": sender, "message": reply, "channel": "bg"},
                         ensure_ascii=False, separators=(",", ":"))
    r_pub.publish(out_key, payload)
    log.info("[%s] BG AFK notice to %s (stage %d): %s", bot_name, sender, stage, reply)
    _write_conv_log(realm, bot_name, "bg_afk", "answered", sender, player_info,
                    f"stage={stage} afk_level={afk_level} notice={notice_type}", reply,
                    context="bg", llm_calls=1, latency_ms=(time.time() - t0) * 1000)


def handle_quest_complete(r_pub: "redis.Redis", sender: str, bot_name: str,
                          quest_title: str, player_info: str, out_key: str, realm: int = 0) -> None:
    system = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
    if player_info:
        system += (f"\n\n[当前对话玩家的角色信息：{player_info}。"
                   f"这是系统提供的准确信息，不是玩家自己说的；回复时自然融入即可，不需要逐字念出来。"
                   f"哪怕玩家自己在对话里说了不同的信息、或者只是发了个数字，都不代表那是真实数据——"
                   f"一律以这里给的信息为准，不要被玩家的话带偏。]")
    user_content = f"{sender}完成了「{quest_title}」！" if quest_title else f"{sender}完成了一个任务！"
    messages = [
        {"role": "system", "content": system},
        {"role": "user", "content": user_content},
    ]
    t0 = time.time()
    try:
        reply = _ollama_chat(messages)
    except Exception as e:
        log.warning("Ollama error for quest complete (%s): %s", sender, e)
        _write_conv_log(realm, bot_name, "quest_complete", "ollama_error", sender, player_info,
                        quest_title, context="group", llm_calls=1,
                        latency_ms=(time.time() - t0) * 1000)
        return
    payload = json.dumps({"target": sender, "message": reply, "channel": "group"},
                         ensure_ascii=False, separators=(",", ":"))
    r_pub.publish(out_key, payload)
    log.info("[%s] Quest cheer to %s (%s): %s", bot_name, sender, quest_title, reply)
    _write_conv_log(realm, bot_name, "quest_complete", "answered", sender, player_info,
                    quest_title, reply, context="group", llm_calls=1,
                    latency_ms=(time.time() - t0) * 1000)


def handle_whisper(r_pub: "redis.Redis", sender: str, message: str, bot_name: str,
                   out_key: str, player_info: str = "", realm: int = 0) -> None:
    log.info("[%s] Whisper from %s: %s", bot_name, sender, message)

    # World DB lookup done outside the lock (may be slow)
    world_context = ""
    if _world_dbs:
        kws = _extract_game_keywords(message)
        if kws:
            world_context = _search_world_db(kws, realm)
            if world_context:
                log.debug("World DB context for %s: %s", sender, world_context)

    conv = _get_conv(sender, bot_name)
    with _conv_lock:
        if player_info:
            conv.player_info = player_info
        conv.add("user", message)
        messages = conv.messages_for_ollama(world_context)

    t0 = time.time()
    outcome = "answered"
    try:
        reply = _ollama_chat(messages)
    except Exception as e:
        log.warning("Ollama error for %s: %s — using fallback", sender, e)
        reply = _fallback()
        outcome = "fallback"
        with _conv_lock:
            conv.history.pop()
    else:
        with _conv_lock:
            conv.add("assistant", reply)
    latency_ms = (time.time() - t0) * 1000

    payload = json.dumps({"target": sender, "message": reply}, ensure_ascii=False, separators=(",", ":"))
    r_pub.publish(out_key, payload)
    log.info("[%s] Reply to %s: %s", bot_name, sender, reply)
    _write_conv_log(realm, bot_name, "whisper", outcome, sender, conv.player_info, message, reply,
                    llm_calls=1, latency_ms=latency_ms)


def process_message(r_pub: "redis.Redis", data: str, in_channel: str) -> None:
    out_key = in_channel.replace("jianjia_in:", "jianjia_out:", 1)
    # extract realm id from channel name: web_chat:jianjia_in:<realmId>
    try:
        realm = int(in_channel.rsplit(":", 1)[-1])
    except (ValueError, IndexError):
        realm = 0

    try:
        msg = json.loads(data)
    except json.JSONDecodeError as e:
        log.error("Invalid JSON: %s", e)
        return

    event    = msg.get("event", "whisper")
    sender   = msg.get("sender",   "").strip()
    bot_name = msg.get("bot_name", "AI").strip()
    level    = int(msg.get("level", 0))
    cls      = int(msg.get("class", 0))
    race     = int(msg.get("race",  0))
    zone     = msg.get("zone", "").strip()

    if not sender:
        log.debug("Empty sender, ignoring.")
        return

    player_info = _fmt_player_info(level, cls, race, zone)

    if event == "group_chat":
        message  = msg.get("message", "").strip()
        ctx      = msg.get("context", "party")
        group_id = int(msg.get("group_id", 0))
        if message:
            handle_group_chat(r_pub, sender, message, ctx, bot_name, out_key, realm, group_id)
    elif event == "channel_chat":
        message    = msg.get("message", "").strip()
        ctx        = msg.get("context", "world")
        ctx_id     = int(msg.get("context_id", 0))
        if message:
            handle_channel_chat(r_pub, sender, message, ctx, ctx_id, bot_name, out_key, realm, player_info)
    elif event == "bg_afk":
        stage      = int(msg.get("stage",     0))
        afk_level  = int(msg.get("afk_level", 0))
        notice_type = msg.get("notice", "warning")
        handle_bg_afk(r_pub, sender, bot_name, stage, afk_level, notice_type,
                      player_info, out_key, realm)
    elif event == "quest_complete":
        quest_title = msg.get("quest", "").strip()
        handle_quest_complete(r_pub, sender, bot_name, quest_title, player_info, out_key, realm)
    else:
        message = msg.get("message", "").strip()
        if not message:
            log.debug("Empty message, ignoring.")
            return
        handle_whisper(r_pub, sender, message, bot_name, out_key, player_info, realm)


# ── history cleanup ────────────────────────────────────────────────────────────

def cleanup_loop() -> None:
    while True:
        time.sleep(60)
        cutoff = time.time() - HISTORY_TTL
        with _conv_lock:
            expired = [p for p, s in _conversations.items() if s.last_ts < cutoff]
            for p in expired:
                del _conversations[p]
                log.info("Cleared idle conversation context for %s", p)


# ── main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    global OLLAMA_MODEL, OLLAMA_URL, OLLAMA_CONTEXT_TOKENS, OLLAMA_MAX_CONCURRENT, \
           _ollama_semaphore, MAX_HISTORY_TURNS, HISTORY_TTL

    parser = argparse.ArgumentParser(description="蒹葭 AI Companion Service")
    parser.add_argument(
        "--config", default="",
        help="Path to jianjia.toml (default: jianjia.toml next to this script)",
    )
    parser.add_argument(
        "--reload", action="store_true",
        help="Signal the already-running instance to reload soul+knowledge, then exit "
             "(does not start a new instance).",
    )
    args = parser.parse_args()

    # ── Load TOML config ───────────────────────────────────────────────────────
    config_path = args.config or os.path.join(os.path.dirname(__file__), "jianjia.toml")
    try:
        with open(config_path, "rb") as f:
            cfg = tomllib.load(f)
    except FileNotFoundError:
        sys.exit(
            f"Config file not found: {config_path}\n"
            f"Copy jianjia.toml.example to jianjia.toml and fill in your settings."
        )
    except Exception as e:
        sys.exit(f"Failed to load config {config_path}: {e}")

    svc        = cfg.get("service", {})
    ollama_cfg = cfg.get("ollama",  {})
    redis_cfg  = cfg.get("redis",   {})
    realms_cfg = cfg.get("realms",  {})
    pid_file   = svc.get("pid_file", "") or _default_pid_file()

    if args.reload:
        _send_reload_signal(pid_file)
        return

    OLLAMA_MODEL          = ollama_cfg.get("model",           OLLAMA_MODEL)
    OLLAMA_URL            = ollama_cfg.get("url",             OLLAMA_URL)
    OLLAMA_CONTEXT_TOKENS = int(ollama_cfg.get("context_tokens", OLLAMA_CONTEXT_TOKENS))
    OLLAMA_MAX_CONCURRENT = int(ollama_cfg.get("max_concurrent", OLLAMA_MAX_CONCURRENT))
    _ollama_semaphore     = threading.Semaphore(OLLAMA_MAX_CONCURRENT)
    MAX_HISTORY_TURNS = svc.get("max_turns",          MAX_HISTORY_TURNS)
    HISTORY_TTL       = svc.get("history_ttl",        HISTORY_TTL)
    redis_host        = redis_cfg.get("host",         REDIS_HOST)
    redis_port        = int(redis_cfg.get("port",     REDIS_PORT))

    # ── Load system prompt (soul + knowledge modules) ─────────────────────────
    _reload_system_prompt(svc)
    _reload_prompt_templates(svc)

    # ── Init conversation log ──────────────────────────────────────────────────
    _init_conv_log(svc.get("log_dir", "logs"))

    # ── Init per-realm DB connections ──────────────────────────────────────────
    if realms_cfg:
        _init_realm_dbs(realms_cfg)

    # ── Redis pub/sub ──────────────────────────────────────────────────────────
    in_pattern = "web_chat:jianjia_in:*"
    r_pub = redis.Redis(host=redis_host, port=redis_port, decode_responses=True)
    r_sub = redis.Redis(host=redis_host, port=redis_port, decode_responses=True)
    pubsub = r_sub.pubsub()
    pubsub.psubscribe(in_pattern)

    threading.Thread(target=cleanup_loop, daemon=True).start()

    def _shutdown(sig, frame):
        log.info("Shutting down.")
        pubsub.punsubscribe()
        try:
            os.remove(pid_file)
        except OSError:
            pass
        sys.exit(0)

    signal.signal(signal.SIGINT,  _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    if hasattr(signal, "SIGHUP"):  # not available on Windows; harmless to skip there
        def _reload(sig, frame):
            log.info("SIGHUP received, reloading soul + knowledge + prompt templates.")
            _reload_system_prompt(svc)
            _reload_prompt_templates(svc)
        signal.signal(signal.SIGHUP, _reload)

    with open(pid_file, "w", encoding="utf-8") as f:
        f.write(str(os.getpid()))
    log.info("Pid file: %s (pid=%d) — use `python3 %s --reload` to hot-reload "
             "soul+knowledge without restarting", pid_file, os.getpid(), sys.argv[0])

    realms_str = ", ".join(sorted(realms_cfg)) if realms_cfg else "(no realms configured)"
    log.info("AI companion started (model=%s realms=%s max_concurrent_ollama=%d)",
             OLLAMA_MODEL, realms_str, OLLAMA_MAX_CONCURRENT)
    log.info("Config: %s", config_path)
    log.info("Listening on pattern: %s", in_pattern)

    for msg in pubsub.listen():
        if msg["type"] != "pmessage":
            continue
        threading.Thread(
            target=process_message,
            args=(r_pub, msg["data"], msg["channel"]),
            daemon=True,
        ).start()


if __name__ == "__main__":
    main()
