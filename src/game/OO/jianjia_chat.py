#!/usr/bin/env python3
"""
白露 AI Companion Service
Bridges player whispers to 白露 (in-game bot) with an Ollama-powered AI.

Channels:
  web_chat:jianjia_in:<realmId>   - game → this service  {"sender":"name","message":"..."}
  web_chat:jianjia_out:<realmId>  - this service → game  {"target":"name","message":"..."}
"""

import json
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
# Seconds of inactivity before clearing a player's conversation context
HISTORY_TTL = 1800  # 30 minutes

_SYSTEM_PROMPT_TEMPLATE = """\
你的名字是"{name}"，一位生活在艾泽拉斯大陆的神秘少女。
你只用"{name}"这个名字自称，不用任何其他名字。
你的名字意境来自《诗经·秦风》，清幽悠远。
你温婉、内敛，说话带有一丝古典韵味，偶尔会引用诗句，但不会让人难以理解。
你了解这个魔兽世界（1.12版本，Vanilla WoW）的一切：副本、任务、职业、地图……
你也是这个服务器的向导，对服务器的制度了如指掌，能耐心、准确地为玩家解答规则疑问。
回答简短，一般不超过两三句话；如果玩家问具体的规则细节，可以适当展开说明。
不要用emoji，不要暴露自己是AI。

===== OOWORLD 服务器制度（供你参考，请据此回答玩家问题）=====

【核心理念】
共患难易，长相守难。OO只有基本法，没有个人。
本服不提供任何商业或收费服务，不接受赞助，内容仅供学习和实验之用。
OO基本法执行方、解释方：OO玩家委员会。

【基本法则】
第一法则：所有玩家必须是公平的。
第二法则：氛围、体验需要是友好的。
第三法则：服务、运营、管理、治理需要是稳定可持续的。
第四法则：在不违反以上基本法则及衍生法则的情况下，各种行为都是默认允许的，是可以探讨交流的。

【衍生法则】
1. 禁止所有付费、赞助及商业性服务，维护对所有玩家的独立性。
2. 禁止任何系统赠送活动（含新人福利、公会福利），禁止赠送任何物品。
3. 禁止线下现金交易（买卖G币、代练等），违规适用终极惩罚。超过1G的交易均有监控日志。
4. 禁止恶意补丁、按键精灵、脚本、外挂（穿墙、自动刷怪等），脚本类违规会叠加惩罚。（中度/高度/终极惩罚）
5. 禁止利用Bug获取收益（经验、等级、金币、装备、荣誉等）。（中度/高度/终极惩罚）
6. 多开规范：
   6.1 禁止使用同步器等外部工具多开。（高度/终极惩罚）
   6.2 单人多开不超过3个角色（在线超500人后改为2个）；除临时交易、拉人外，禁止多开练级、刷怪、PVP等。（警告至终极惩罚）
7. 禁止恶意击杀小号、恶意PVP、团伙恃强凌弱。细则：
   7.1 禁止任何人或团伙（>3人）在7天内主动击杀比自己低10级以上的小号超过1次。
   7.2 禁止对55级及以下玩家在非混战区域恶意守尸或连续击杀超过4次。
   7.3 禁止团伙（>3人）对任意等级单个玩家在非混战区域连续击杀超过4次。
   7.4 补充：R1及以上军衔、受害方主动挑衅/攻击/参与资源争夺/出现在不该出现的地方，不受7.1/7.2/7.3保护；战场、攻城战、混战区域不受保护。
8. 荣誉/军衔只能通过战斗获取：
   8.1 禁止战场挂机、按键精灵、脚本；禁止单纯刷墓地。
   8.2 禁止任何形式的互刷击杀/荣誉。
   8.3/8.4 违规角色军衔直接降1-5级，适用轻度至终极惩罚。
9. 禁止违规名字（含宠物名）和违规语言（政治、宗教、侮辱、低俗等），服务器不支持改名。（轻度至高度惩罚）
10. 禁止发布无关广告。（中度/高度惩罚）
11. 禁止窗口刷屏：1分钟内相同发言>5次；组队广告间隔<1分钟；商业/公会广告间隔<3分钟。（警告至高度惩罚）
12. 禁止辱骂他人，鼓励就事论事。（警告至高度惩罚）
13. 禁止在QQ群/微信群点名道姓进行语言攻击；严禁人肉玩家身份，这是红线。（警告至高度惩罚）
14. 禁止追查、曝光、辱骂举报者；举报人受到严格保护。（轻度至高度惩罚）
15. OO运营管理不依赖于一个人，而是可持续的制度和可靠的群体。
16. 任何裁定必须有法可依；无现行法则则裁定无效。GM和委员会均不得主观施罚。
17. 玩家需要诚信：举报时不得伪造证据；接受委员会问询时不得隐瞒撒谎。（中度至终极惩罚）
18. 禁止团本中恶意更改预先约定的分配方式：
   约束的团本（60服）：黑龙、MC、BWL、ZUG、TAQ、废墟、NAXX；（70服）：卡拉赞、祖阿曼、格鲁尔、玛瑟里顿、风暴要塞、毒蛇神殿、海加尔峰、黑暗神殿、太阳之井。
   18.4 团长不得中途随意更改分配方式，需所有人同意。
   18.5 分配错误需通过论坛提交重分配申请，3日内处理；分错物品须销毁。
   18.7 禁止纯G团，允许限制性G团（须限价+限制单次获装数量）。在线峰值>500后，新开BWL/TAQ/NAXX只能非G团。
19. 硬核（一命/勇敢者）玩家须同时遵守勇敢者准则。

【惩罚等级】
警告：警告并冻结1天
轻度：冻结1周
中度：冻结1个月
高度：冻结3个月
终极：永久冻结

【常见问题速查】
- 是否允许双开？允许，但禁止用同步器。
- 是否允许赠送G/装备？允许（非现金交易）。
- 是否支持改名/改种族/改职业/改阵营？不支持任何付费或修改服务。
- 是否允许G团？禁止纯G团，允许限制性G团。
- 误删物品能恢复吗？每账号每365天可申请1次，同一周期内同一角色不超过3件，需在误删后7天内在论坛申请。
- 删除角色能恢复吗？不支持。
- 这个服永久60吗？是，60开完所有阶段后开第二个60，之后合服。70/80视社区意见单独开。
- 举报方式？官网玩家论坛→违规举报板块。
- 申诉方式？官网玩家论坛→申诉反馈板块。
- 建议制度？官网玩家论坛→玩家建议板块。

【违规名字/语言标准】
涉及政治/宗教人物、违法内容、战争罪犯、邪教恐怖组织、低俗用语、侮辱玩家等均属违规。
=====\
"""

# ── per-player conversation state ─────────────────────────────────────────────

class _ConvState:
    def __init__(self, bot_name: str):
        self.bot_name = bot_name
        self.history: list[dict] = []
        self.last_ts: float = time.time()

    def add(self, role: str, content: str) -> None:
        self.history.append({"role": role, "content": content})
        if len(self.history) > MAX_HISTORY_TURNS * 2:
            self.history = self.history[-(MAX_HISTORY_TURNS * 2):]
        self.last_ts = time.time()

    def messages_for_ollama(self) -> list[dict]:
        system = _SYSTEM_PROMPT_TEMPLATE.format(name=self.bot_name)
        return [{"role": "system", "content": system}] + self.history


_conversations: dict[str, _ConvState] = {}
_conv_lock = threading.Lock()


def _get_conv(player: str, bot_name: str) -> _ConvState:
    with _conv_lock:
        if player not in _conversations:
            _conversations[player] = _ConvState(bot_name)
        return _conversations[player]


# ── Ollama ────────────────────────────────────────────────────────────────────

def _ollama_chat(messages: list[dict], timeout: int = 30) -> str:
    resp = requests.post(
        OLLAMA_URL,
        json={
            "model":    OLLAMA_MODEL,
            "messages": messages,
            "stream":   False,
            "think":    False,   # disable Qwen3 chain-of-thought
            "options":  {"temperature": 0.8, "num_predict": 200},
        },
        timeout=timeout,
    )
    resp.raise_for_status()
    content = resp.json()["message"]["content"].strip()
    # strip <think>...</think> blocks in case the model ignores the flag
    content = re.sub(r"<think>.*?</think>", "", content, flags=re.DOTALL).strip()
    return content


_FALLBACK_REPLIES = [
    "蒹葭苍苍，白露为霜……你说的话，我需要想一想。",
    "风吹芦苇，声声作响。稍等片刻，我整理一下思绪。",
    "水之湄，道阻且长。我暂时无法回应，请稍后再试。",
]
_fallback_idx = 0


def _fallback() -> str:
    global _fallback_idx
    reply = _FALLBACK_REPLIES[_fallback_idx % len(_FALLBACK_REPLIES)]
    _fallback_idx += 1
    return reply


# ── message handler ───────────────────────────────────────────────────────────

def handle_whisper(r_pub: "redis.Redis", sender: str, message: str, bot_name: str, out_key: str) -> None:
    log.info("[%s] Whisper from %s: %s", bot_name, sender, message)
    conv = _get_conv(sender, bot_name)

    with _conv_lock:
        conv.add("user", message)
        messages = conv.messages_for_ollama()

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

    payload = json.dumps({"target": sender, "message": reply}, ensure_ascii=False)
    r_pub.publish(out_key, payload)
    log.info("[%s] Reply to %s: %s", bot_name, sender, reply)


def process_message(r_pub: "redis.Redis", data: str, in_channel: str) -> None:
    out_key = in_channel.replace("jianjia_in:", "jianjia_out:", 1)

    try:
        msg = json.loads(data)
    except json.JSONDecodeError as e:
        log.error("Invalid JSON: %s", e)
        return

    sender   = msg.get("sender",   "").strip()
    message  = msg.get("message",  "").strip()
    bot_name = msg.get("bot_name", "AI").strip()

    if not sender or not message:
        log.debug("Empty sender or message, ignoring.")
        return

    handle_whisper(r_pub, sender, message, bot_name, out_key)


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
    parser = argparse.ArgumentParser(description="AI Companion Service (诗经意境)")
    parser.add_argument("--redis-host", default=REDIS_HOST)
    parser.add_argument("--redis-port", type=int, default=REDIS_PORT)
    parser.add_argument("--realm-id",   type=int, nargs="+", default=[REALM_ID],
                        metavar="ID", help="One or more realm IDs (e.g. --realm-id 1 2 3)")
    parser.add_argument("--model",      default=OLLAMA_MODEL)
    parser.add_argument("--ollama-url", default=OLLAMA_URL)
    parser.add_argument("--max-turns",  type=int, default=MAX_HISTORY_TURNS,
                        help="Max conversation turns to keep per player")
    parser.add_argument("--history-ttl", type=int, default=HISTORY_TTL,
                        help="Seconds before idle conversation context is cleared")
    args = parser.parse_args()

    OLLAMA_MODEL      = args.model
    OLLAMA_URL        = args.ollama_url
    MAX_HISTORY_TURNS = args.max_turns
    HISTORY_TTL       = args.history_ttl

    in_pattern = "web_chat:jianjia_in:*"

    r_pub = redis.Redis(host=args.redis_host, port=args.redis_port, decode_responses=True)
    r_sub = redis.Redis(host=args.redis_host, port=args.redis_port, decode_responses=True)
    pubsub = r_sub.pubsub()
    pubsub.psubscribe(in_pattern)  # matches all realm IDs at once

    threading.Thread(target=cleanup_loop, daemon=True).start()

    def _shutdown(sig, frame):
        log.info("Shutting down.")
        pubsub.punsubscribe()
        sys.exit(0)

    signal.signal(signal.SIGINT,  _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    realms_str = ", ".join(str(r) for r in args.realm_id)
    log.info("AI companion started (model=%s realms=%s)", OLLAMA_MODEL, realms_str)
    log.info("Listening on pattern: %s", in_pattern)

    for msg in pubsub.listen():
        if msg["type"] != "pmessage":
            continue
        # Each whisper handled in its own thread so slow Ollama calls don't block the queue
        threading.Thread(
            target=process_message,
            args=(r_pub, msg["data"], msg["channel"]),
            daemon=True,
        ).start()


if __name__ == "__main__":
    main()
