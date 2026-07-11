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
                # Items
                cur.execute(
                    "SELECT name, ItemLevel, RequiredLevel, Quality "
                    "FROM item_template WHERE name LIKE %s LIMIT 2",
                    (like,),
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

                # Quests
                cur.execute(
                    "SELECT Title, QuestLevel FROM quest_template WHERE Title LIKE %s LIMIT 2",
                    (like,),
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

                # NPCs / creatures
                cur.execute(
                    "SELECT name, subname, minlevel, maxlevel, rank "
                    "FROM creature_template WHERE name LIKE %s LIMIT 2",
                    (like,),
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
            system += f"\n\n[当前对话玩家的角色信息：{self.player_info}。了解即可，回复时自然融入，无需直接提及。]"
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

def _ollama_chat(messages: list[dict], timeout: int = 30, temperature: float = 0.8) -> str:
    resp = requests.post(
        OLLAMA_URL,
        json={
            "model":    OLLAMA_MODEL,
            "messages": messages,
            "stream":   False,
            "think":    False,   # disable Qwen3 chain-of-thought
            "options":  {"temperature": temperature, "num_predict": 200},
        },
        timeout=timeout,
    )
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

def _write_conv_log(realm: int, bot: str, player: str, player_info: str, user_msg: str, ai_reply: str) -> None:
    if not _conv_log_file:
        return
    record = json.dumps({
        "ts":     time.strftime("%Y-%m-%d %H:%M:%S"),
        "realm":  realm,
        "bot":    bot,
        "player": player,
        "info":   player_info,
        "user":   user_msg,
        "reply":  ai_reply,
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
        return
    if bot_mentioned:
        _wake_up(chat_context, group_id)

    channel_name = _CHANNEL_NAMES.get(chat_context, "频道")
    system = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
    system += (
        f"\n\n你正在监听{channel_name}频道。只在以下情况才开口："
        f"① 有人直接叫你名字；② 有人直接向你提问；"
        f"③ 有人完成了任务或成就、分享了好消息，给予真诚鼓励。"
        f"其他闲聊、日常对话一律保持安静，回复 [PASS]。"
    )
    conv = _get_conv(sender, bot_name)
    if conv.player_info:
        system += f"\n[{sender}的角色信息：{conv.player_info}]"

    history = _get_group_history(group_id, bot_name, chat_context, realm)
    messages = [{"role": "system", "content": system}] + history + [
        {"role": "user", "content": f"{sender}：{message}"}
    ]
    try:
        reply = _ollama_chat(messages)
    except Exception as e:
        log.warning("Ollama error for group chat: %s", e)
        return
    if not reply or "[PASS]" in reply:
        return

    payload = json.dumps({"target": sender, "message": reply, "channel": chat_context},
                         ensure_ascii=False, separators=(",", ":"))
    r_pub.publish(out_key, payload)
    log.info("[%s] %s chat reply to %s: %s", bot_name, chat_context, sender, reply)
    _write_conv_log(realm, bot_name, sender, conv.player_info, f"[{chat_context}]{message}", reply)


def _verify_grounded(bot_name: str, question: str, reply: str) -> bool:
    """Second-pass fact check for world-channel replies: does every factual claim in
    `reply` actually come from the knowledge base? A separate judge call reviewing an
    already-written answer against the reference text is a much narrower, more reliable
    task for the model than asking it to predict its own knowledge boundaries up front
    (which is what the main answer-or-PASS instruction already tries and still misses
    sometimes). Fails closed on any error or unclear verdict: staying silent is always
    safer than broadcasting an unverified answer to the whole world channel.
    """
    kb = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
    system = (
        f"你是内容审核员，不用扮演任何角色。下面是{bot_name}的参考资料，以及她对一个玩家问题给出的候选回复。"
        f"请检查候选回复里的每一句事实性内容，是否都能在参考资料里找到依据（原文或合理改写均可）。"
        f"如果回复里包含任何参考资料没有提到的具体细节（编造的机制、编造的物品/技能名、编造的原因等），判定为不合格。"
        f"语气词、称呼、寒暄等非事实性内容不计入判断。"
        f"只输出「合格」或「不合格」这两个词之一，不要输出任何其他内容。\n\n"
        f"【参考资料】\n{kb}\n\n"
        f"【玩家问题】\n{question}\n\n"
        f"【候选回复】\n{reply}"
    )
    try:
        verdict = _ollama_chat(
            [{"role": "system", "content": system},
             {"role": "user", "content": "请给出判定。"}],
            temperature=0.1,
        )
    except Exception as e:
        log.warning("Ollama error verifying channel reply for %s: %s", question, e)
        return False
    return "不合格" not in verdict and "合格" in verdict


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
            return
        if bot_mentioned:
            _wake_up("guild", context_id)

        system = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
        system += (
            f"\n\n你正在监听{channel_name}。只在以下情况才开口："
            f"① 有人直接叫你名字；② 有人直接向你提问；"
            f"③ 有人完成了任务或成就、分享了好消息，给予真诚鼓励。"
            f"其他闲聊、日常对话一律保持安静，回复 [PASS]。"
        )
        if player_info:
            system += f"\n[{sender}的角色信息：{player_info}]"
        messages = [
            {"role": "system", "content": system},
            {"role": "user", "content": f"{sender}（{channel_name}）：{message}"},
        ]
        try:
            reply = _ollama_chat(messages)
        except Exception as e:
            log.warning("Ollama error for guild chat (%s): %s", sender, e)
            return
        if not reply or "[PASS]" in reply:
            return

    else:  # world channel: question-based filter
        if not _QUESTION_RE.search(message):
            return
        with _channel_reply_lock:
            last = _channel_reply_ts.get(sender, 0.0)
            if time.time() - last < _CHANNEL_REPLY_CD:
                return
            _channel_reply_ts[sender] = time.time()

        system = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
        system += (
            f"\n\n你正在监听{channel_name}。"
            f"只回答服务器规则/制度/版本进度这类「服务器知识」问题，且答案必须能在你掌握的FAQ知识库中原文或近似原文找到。"
            f"具体的游戏内容/玩法问题一律不参与，就算你知道答案也不要说，直接回复 [PASS]——"
            f"比如某个怪物为什么打不了、声望怎么刷、任务/副本怎么打、门怎么开、天赋加点这类通用魔兽世界玩法问题，"
            f"这些应该让玩家自己去问其他玩家，不是你的职责范围。"
            f"如果问题含糊、不在FAQ范围内、已被其他玩家回答、或只是普通闲聊，"
            f"同样直接回复 [PASS]（不要附加任何文字）。"
            f"回答时，先@{sender} 称呼对方，然后用一到两句话给出简洁准确的答案。"
            f"不要展开说太多，不要卖弄学识，保持蒹葭的性格。"
        )
        if player_info:
            system += f"\n[{sender}的角色信息：{player_info}]"
        messages = [
            {"role": "system", "content": system},
            {"role": "user", "content": f"{sender}（{channel_name}）：{message}"},
        ]
        try:
            # Low temperature: this is a strict "answer only from the knowledge base,
            # otherwise PASS" judgment call, not free-form chat — high temperature made
            # it fabricate answers for out-of-scope questions instead of staying silent.
            reply = _ollama_chat(messages, temperature=_WORLD_CHANNEL_TEMPERATURE)
        except Exception as e:
            log.warning("Ollama error for world chat (%s): %s", sender, e)
            with _channel_reply_lock:
                _channel_reply_ts.pop(sender, None)
            return
        if not reply or "[PASS]" in reply:
            with _channel_reply_lock:
                _channel_reply_ts.pop(sender, None)
            return

        if not _verify_grounded(bot_name, message, reply):
            log.info("[%s] world reply to %s failed grounding check, discarding: %s",
                     bot_name, sender, reply)
            with _channel_reply_lock:
                _channel_reply_ts.pop(sender, None)
            return

    payload = json.dumps({"target": sender, "message": reply, "channel": chat_context},
                         ensure_ascii=False, separators=(",", ":"))
    r_pub.publish(out_key, payload)
    log.info("[%s] %s reply to %s: %s", bot_name, chat_context, sender, reply)
    _write_conv_log(realm, bot_name, sender, player_info, f"[{chat_context}]{message}", reply)


def handle_bg_afk(r_pub: "redis.Redis", sender: str, bot_name: str,
                  stage: int, afk_level: int, notice_type: str, player_info: str,
                  out_key: str, realm: int = 0) -> None:
    system = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
    system += "\n\n你现在在战场频道发言，所有队友都能看到。"
    if player_info:
        system += f"\n[{sender}的角色信息：{player_info}]"

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
    try:
        reply = _ollama_chat(messages)
    except Exception as e:
        log.warning("Ollama error for bg_afk (%s): %s — signalling C++ fallback", sender, e)
        payload = json.dumps({"target": sender, "channel": "fallback",
                              "stage": stage, "afk_level": afk_level, "notice": notice_type},
                             ensure_ascii=False, separators=(",", ":"))
        r_pub.publish(out_key, payload)
        return

    payload = json.dumps({"target": sender, "message": reply, "channel": "bg"},
                         ensure_ascii=False, separators=(",", ":"))
    r_pub.publish(out_key, payload)
    log.info("[%s] BG AFK notice to %s (stage %d): %s", bot_name, sender, stage, reply)
    _write_conv_log(realm, bot_name, sender, player_info, f"[bg_afk stage={stage}]", reply)


def handle_quest_complete(r_pub: "redis.Redis", sender: str, bot_name: str,
                          quest_title: str, player_info: str, out_key: str, realm: int = 0) -> None:
    system = _SYSTEM_PROMPT_TEMPLATE.format(name=bot_name)
    if player_info:
        system += f"\n\n[当前对话玩家的角色信息：{player_info}。了解即可，回复时自然融入，无需直接提及。]"
    user_content = f"{sender}完成了「{quest_title}」！" if quest_title else f"{sender}完成了一个任务！"
    messages = [
        {"role": "system", "content": system},
        {"role": "user", "content": user_content},
    ]
    try:
        reply = _ollama_chat(messages)
    except Exception as e:
        log.warning("Ollama error for quest complete (%s): %s", sender, e)
        return
    payload = json.dumps({"target": sender, "message": reply, "channel": "group"},
                         ensure_ascii=False, separators=(",", ":"))
    r_pub.publish(out_key, payload)
    log.info("[%s] Quest cheer to %s (%s): %s", bot_name, sender, quest_title, reply)
    _write_conv_log(realm, bot_name, sender, player_info, f"[quest:{quest_title}]", reply)


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

    try:
        reply = _ollama_chat(messages)
    except Exception as e:
        log.warning("Ollama error for %s: %s — using fallback", sender, e)
        reply = _fallback()
        with _conv_lock:
            conv.history.pop()
    else:
        with _conv_lock:
            conv.add("assistant", reply)

    payload = json.dumps({"target": sender, "message": reply}, ensure_ascii=False, separators=(",", ":"))
    r_pub.publish(out_key, payload)
    log.info("[%s] Reply to %s: %s", bot_name, sender, reply)
    _write_conv_log(realm, bot_name, sender, conv.player_info, message, reply)


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
    global OLLAMA_MODEL, OLLAMA_URL, MAX_HISTORY_TURNS, HISTORY_TTL

    parser = argparse.ArgumentParser(description="蒹葭 AI Companion Service")
    parser.add_argument(
        "--config", default="",
        help="Path to jianjia.toml (default: jianjia.toml next to this script)",
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

    OLLAMA_MODEL      = ollama_cfg.get("model",       OLLAMA_MODEL)
    OLLAMA_URL        = ollama_cfg.get("url",         OLLAMA_URL)
    MAX_HISTORY_TURNS = svc.get("max_turns",          MAX_HISTORY_TURNS)
    HISTORY_TTL       = svc.get("history_ttl",        HISTORY_TTL)
    redis_host        = redis_cfg.get("host",         REDIS_HOST)
    redis_port        = int(redis_cfg.get("port",     REDIS_PORT))

    # ── Load system prompt (soul + knowledge modules) ─────────────────────────
    global _SYSTEM_PROMPT_TEMPLATE
    _SYSTEM_PROMPT_TEMPLATE = _load_system_prompt(
        svc.get("soul_file", ""),
        svc.get("knowledge_dir", ""),
    )
    server_name  = svc.get("server_name",  "").strip()
    server_phase = svc.get("server_phase", "").strip()
    if server_name or server_phase:
        parts = []
        if server_name:
            parts.append(f"当前服务器：{server_name}")
        if server_phase:
            parts.append(f"当前阶段：{server_phase}")
        _SYSTEM_PROMPT_TEMPLATE += "\n\n[" + "，".join(parts) + "。回答玩家问题时以此为准。]"
    log.info("System prompt loaded (%d chars)", len(_SYSTEM_PROMPT_TEMPLATE))

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
        sys.exit(0)

    signal.signal(signal.SIGINT,  _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    realms_str = ", ".join(sorted(realms_cfg)) if realms_cfg else "(no realms configured)"
    log.info("AI companion started (model=%s realms=%s)", OLLAMA_MODEL, realms_str)
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
