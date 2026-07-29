#!/usr/bin/env python3
"""蒹葭AI 历史对话日志挖掘工具 —— 自我学习 Tier 1 / Tier 2

这是一个只读工具：只读取 logs/JianJia_*.jsonl，**从不写** jianjia_soul.md /
jianjia_knowledge/*.md / jianjia_prompts/*.md 里的任何文件。输出是一份 Markdown
报告，供人工复核决定要不要真的去改知识库——具体怎么改、改完怎么验证，仍然是
人工的事（参考 jianjia_debug.php，2-3 次重采样确认真的解决了问题再手动落地）。

不自动应用任何修改是有意为之：qwen3:14b 本身不稳定（版本号/日期比较这类任务上
反复验证过不可靠），如果让它自己判断"我错在哪、该怎么改"且没有人把关，错误
结论完全可能被写成"官方知识"固化进知识库，比单次回复答错的杀伤面大得多。
详见 JianJia.md "自我学习机制" 一节的分级方案（Tier 1 挖掘候选 / Tier 2 AI
草拟建议 / 均需人工审核才落地，不做无人值守的全自动闭环）。

Tier 1（默认）：纯规则挖掘，标准库即可运行，不需要网络/Ollama/数据库连接。
    - 从日志里挑出两类信号：
      a) outcome=verify_rejected —— AI生成后被自己的审核环节判定为编造、没
         发出去的候选回复。这批数据信号最强：说明"这类问题AI很想答但答不对"。
      b) outcome=answered，且同一频道里紧接着（默认120秒内）出现了带"纠正"
         意味关键词的后续消息 —— 粗筛为"疑似被纠正"，误报率比①高，仅供参考。
    - 按关键词把这些记录聚类，输出候选清单（按出现频率排序，附原始log片段）。

Tier 2（--draft）：在 Tier 1 基础上，额外调用 Ollama，为每个候选簇草拟一条可能
    的知识库/prompt补充文案，明确标注"AI草拟，未经验证，仅供参考起草"。

用法：
    python3 jianjia_gen_faq.py --log-dir logs --days 7
    python3 jianjia_gen_faq.py --log-dir logs --days 7 --draft --config jianjia.toml
"""

import argparse
import glob
import json
import os
import re
import sys
from collections import Counter, defaultdict
from datetime import datetime, timedelta

# 同一玩家在 AI 回复之后的追问/吐槽里，带"这个回答是错的"意味的关键词——粗筛，
# 宁可漏判也不要把正常闲聊/跟AI无关的对话当成纠正信号。
_CORRECTION_RE = re.compile(
    r'不对|错了|说错|瞎说|乱说|骗人|胡说|你说的不是|不是这样|才不是|扯淡|放屁'
)

# 关键词提取用的停用词表。跟 jianjia_chat.py::_SKIP_WORDS 是同一批常见虚词/
# 高频泛用词，这里独立维护一份拷贝而不是 import jianjia_chat——那个模块顶层
# 会拉起 redis/pymysql/requests 等运行时依赖，这个脚本只做离线日志分析，应该
# 能在没有这些依赖、没有网络连接的机器上单独跑起来。如果 jianjia_chat.py 那边
# 的停用词表更新了，这里可以顺手同步一下，但不是强依赖关系。
_SKIP_WORDS = frozenset({
    "什么", "怎么", "哪里", "哪儿", "知道", "可以", "没有", "玩家", "任务",
    "物品", "装备", "怎样", "如何", "在哪", "为什么", "告诉", "一些", "很多",
    "一个", "这个", "那个", "我的", "你的", "他的", "我们", "你们", "所有",
    "战场", "战斗", "队友", "副本", "地图", "魔兽", "服务器", "角色", "不知",
    "请问", "帮我", "帮忙", "可能", "需要", "现在", "今天", "一起", "然后",
    "是不是", "有没有", "可以吗", "还是", "但是", "所以", "如果", "因为",
    "已经", "还有", "大家", "谢谢", "一下",
    # bot 自己的名字（各 realm 可能配不同名字，这三个是目前实际在用的）——
    # world-summon 事件（@提及召唤）的玩家消息里几乎总会出现bot的名字，不然
    # 根本触发不了召唤；不排除的话，"子衿"/"蒹葭"/"白露" 会在几乎所有召唤类
    # 案例里被提取成关键词，把不相关的召唤消息聚成一类假簇，还会误导 --draft
    # 把bot自己的名字当成一个需要澄清的游戏黑话去分析（实测真的发生过：曾经
    # 把"子衿"当成一个可能跟游戏术语混淆的词去草拟建议，完全文不对题）。
    "蒹葭", "子衿", "白露",
})

_CJK_RUN_RE = re.compile(r'[一-鿿]+')
# 缩写黑话（NY/MC/TAQ/FX/AQ/ZG...）本身就是这个服务器术语混淆最集中的重灾区
# （这次会话修的好几个真实bug都是这类：NY当成NAXX、神庙/废墟指代搞反），必须
# 单独抓，CJK正则完全捕获不到纯英文缩写。
_LATIN_RE = re.compile(r'[A-Za-z]{2,8}')

CORRECTION_WINDOW_SEC = 120  # "疑似纠正"信号的时间窗口


def extract_keywords(text: str, limit: int = 8) -> list[str]:
    """粗糙的中文关键词提取：没有引入 jieba 之类的分词依赖（这个脚本刻意保持
    轻量、可以在没有额外依赖的机器上离线跑），退而求其次对每一段连续中文取
    2/3 字滑动窗口子串——只要两条消息提到同一个词，子串里就必然会有重叠，
    聚类靠的是"有没有共同关键词"，不需要子串本身是一个语言学意义上的完整词。
    英文缩写按大写归一化，单独识别（不跟中文用同一套逻辑，因为英文没有"连续
    字符"这个概念，正则直接按单词边界切就够）。
    """
    seen: set[str] = set()
    result: list[str] = []

    def add(word: str) -> None:
        if word not in seen and word not in _SKIP_WORDS:
            seen.add(word)
            result.append(word)

    for tok in _LATIN_RE.findall(text):
        add(tok.upper())

    for run in _CJK_RUN_RE.findall(text):
        for n in (3, 2):
            for i in range(len(run) - n + 1):
                add(run[i:i + n])

    return result[:limit] if limit else result


# ── 日志读取 ──────────────────────────────────────────────────────────────────

def iter_records(log_dir: str, since):
    paths = sorted(glob.glob(os.path.join(log_dir, "JianJia_*.jsonl")))
    if not paths:
        print(f"[warn] {log_dir} 下没有找到 JianJia_*.jsonl 文件", file=sys.stderr)
    for path in paths:
        if since is not None:
            # 文件名/mtime 做一次便宜的粗筛，跳过明显早于窗口的整个文件，避免
            # 逐行解析所有历史日志——生产环境跑久了这批文件可能相当大。
            try:
                if datetime.fromtimestamp(os.path.getmtime(path)) < since:
                    continue
            except OSError:
                pass
        try:
            with open(path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        rec = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if since is not None:
                        try:
                            ts = datetime.strptime(rec.get("ts", ""), "%Y-%m-%d %H:%M:%S")
                        except ValueError:
                            ts = None
                        if ts is not None and ts < since:
                            continue
                    rec["_file"] = os.path.basename(path)
                    yield rec
        except OSError as e:
            print(f"[warn] 跳过无法读取的日志文件 {path}: {e}", file=sys.stderr)


# ── 信号分类 ──────────────────────────────────────────────────────────────────

def find_flagged(records: list[dict]) -> list[dict]:
    """挑出值得复核的记录，见模块 docstring。"""
    flagged: list[dict] = []

    for rec in records:
        outcome = rec.get("outcome")
        # verify_rejected：候选回复被判定编造后直接丢弃（world_question 路径）。
        # deflect：world_summon 路径下同样被判定编造，但没有直接丢弃，而是另外
        # 生成了一句敷衍/岔开话题的回复顶上——对玩家来说观感通常还行（不会
        # 完全没反应），但这依然是"候选回复没通过grounding检查"的同一类信号，
        # 只是已经有了兜底、没有沉默，容易被忽略，同样值得挖出来看。
        if outcome in ("verify_rejected", "deflect"):
            r = dict(rec)
            r["_flag"] = outcome
            flagged.append(r)

    # "疑似被纠正"信号需要按频道把记录排好序，往后看一个时间窗口。
    by_channel: dict[tuple, list[dict]] = defaultdict(list)
    for rec in records:
        key = (rec.get("realm"), rec.get("context") or rec.get("event"))
        by_channel[key].append(rec)

    for chan_records in by_channel.values():
        chan_records.sort(key=lambda r: r.get("ts", ""))
        for i, rec in enumerate(chan_records):
            if rec.get("outcome") != "answered":
                continue
            try:
                t0 = datetime.strptime(rec["ts"], "%Y-%m-%d %H:%M:%S")
            except (KeyError, ValueError):
                continue
            for later in chan_records[i + 1:]:
                try:
                    t1 = datetime.strptime(later["ts"], "%Y-%m-%d %H:%M:%S")
                except (KeyError, ValueError):
                    continue
                if (t1 - t0).total_seconds() > CORRECTION_WINDOW_SEC:
                    break
                if _CORRECTION_RE.search(later.get("user", "")):
                    r = dict(rec)
                    r["_flag"] = "disputed"
                    r["_correction_msg"] = later.get("user", "")
                    r["_correction_by"] = later.get("player", "")
                    flagged.append(r)
                    break
    return flagged


# ── 聚类（关键词共现，粗糙但够用——目的是帮人快速定位，不是精确分类）────────────

def cluster(flagged: list[dict], min_count: int) -> list[tuple[str, list[dict]]]:
    by_keyword: dict[str, list[tuple[tuple, dict]]] = defaultdict(list)
    for rec in flagged:
        # (ts, player, user) 作为这条记录的稳定身份——同一条记录可能命中好几个
        # n-gram，去重用；跨n-gram的相同记录不应该在同一个簇里重复计数。
        ident = (rec.get("ts"), rec.get("player"), rec.get("user"))
        kws = extract_keywords(rec.get("user", ""))
        if not kws:
            by_keyword["（无法提取关键词）"].append((ident, rec))
            continue
        for kw in kws:
            by_keyword[kw].append((ident, rec))

    raw: list[tuple[str, list[dict], frozenset]] = []
    for kw, pairs in by_keyword.items():
        seen_ids: set = set()
        recs: list[dict] = []
        for ident, rec in pairs:
            if ident in seen_ids:
                continue
            seen_ids.add(ident)
            recs.append(rec)
        if len(recs) >= min_count:
            raw.append((kw, recs, frozenset(seen_ids)))

    # 相邻长度的n-gram（"黑上扣"/"上扣眼"/"扣眼睛"/"黑上"……）经常覆盖完全相同
    # 的一批记录——按"成员数更多优先，关键词更短优先"排序后贪心去掉被已保留簇
    # 完全覆盖（子集或相等）的重复簇，避免报告里出现好几份内容一样的分组，
    # 只留下最短、最像一个真正术语/黑话的那个标签。
    raw.sort(key=lambda c: (-len(c[2]), len(c[0])))
    kept: list[tuple[str, list[dict]]] = []
    kept_id_sets: list[frozenset] = []
    for kw, recs, ids in raw:
        if any(ids <= existing for existing in kept_id_sets):
            continue
        kept.append((kw, recs))
        kept_id_sets.append(ids)

    kept.sort(key=lambda kv: len(kv[1]), reverse=True)
    return kept


# ── Tier 2: AI 草拟建议（可选，需要 --draft）───────────────────────────────────

def load_ollama_config(config_path: str) -> dict:
    try:
        import tomllib
    except ModuleNotFoundError:
        import tomli as tomllib  # type: ignore
    with open(config_path, "rb") as f:
        cfg = tomllib.load(f)
    return cfg.get("ollama", {})


def draft_suggestion(ollama_cfg: dict, keyword: str, recs: list[dict]) -> str:
    try:
        import requests
    except ModuleNotFoundError:
        return "（草拟失败：本机未安装 requests，运行 `pip install requests` 后重试）"

    examples = []
    for r in recs[:8]:
        line = f"- 玩家问：{r.get('user', '')}"
        if r.get("reply"):
            line += f"｜AI候选回复（{r.get('_flag')}）：{r.get('reply')}"
        if r.get("_correction_msg"):
            line += f"｜后续被追问纠正：{r.get('_correction_msg')}"
        examples.append(line)

    prompt = (
        f"下面是蒹葭AI（魔兽世界私服的AI向导角色）历史对话日志里，跟关键词"
        f"「{keyword}」相关、疑似答错或被自我审核拒绝的真实案例：\n\n"
        + "\n".join(examples) +
        "\n\n请分析这些案例可能共同反映了什么问题（比如某个术语被误认成别的副本、"
        "某类问题该沉默却被回答、知识库缺少某条事实、还是别的原因），然后草拟一条"
        "可以加进知识库或prompt规则的补充文字建议。如果案例信息不足以判断出具体"
        "该写什么内容（比如需要真实游戏内数据但你并不确定），明确说明需要人工核实"
        "哪方面的信息，不要编造具体数字或事实。控制在200字以内，直接给建议文字，"
        "不要输出思考过程。"
    )
    try:
        resp = requests.post(
            ollama_cfg.get("url", "http://127.0.0.1:11434/api/chat"),
            json={
                "model": ollama_cfg.get("model", "qwen3:14b"),
                "messages": [{"role": "user", "content": prompt}],
                "stream": False,
                "options": {
                    "temperature": 0.3,
                    "num_ctx": ollama_cfg.get("context_tokens", 8192),
                },
            },
            timeout=90,
        )
        resp.raise_for_status()
        content = resp.json().get("message", {}).get("content", "")
        content = re.sub(r'<think>.*?</think>', '', content, flags=re.DOTALL).strip()
        return content or "（模型没有返回内容）"
    except Exception as e:
        return f"（草拟失败，请人工分析：{e}）"


# ── Tier 3: 提交 PR（可选，需要 --pr，隐含 --draft）─────────────────────────────
#
# 关键安全设计：这里绝不直接改 jianjia_soul.md / jianjia_knowledge/*.md /
# jianjia_prompts/*.md ——那些文件会被 jianjia_chat.py 整个目录扫描加载进
# 每一次线上请求的 system prompt，一旦分支被合并，内容立刻对所有玩家生效。
# 这个函数只在一个**新分支**上新增一份独立的建议文件（`jianjia_faq_suggestions/`
# 目录，jianjia_chat.py 的加载逻辑根本不会扫到这个目录），然后开一个PR——
# PR本身就是人工审核关卡：人工看完证据链之后，如果认可，再由人工在这个PR里
# 追加一次commit，把内容真正整理进知识库文件，合并才会生效。AI只负责"起草
# 一份带证据的建议+开个PR方便讨论"，不负责、也没有权限让改动自己生效。

def slugify(text: str) -> str:
    safe = re.sub(r'[^0-9A-Za-z一-鿿]+', '-', text).strip('-')
    return safe[:40] or "suggestion"


def _scrub_secrets(text: str) -> str:
    """git的错误输出里可能原样带出远端URL——如果那个URL是 https://user:token@host/...
    这种嵌了凭据的形式（这个仓库目前配的是SSH remote，用不到，但脚本不该假设
    以后也一直是这样），不能把它原样写进报告文件/PR正文里。"""
    return re.sub(r'https://[^/@\s]+:[^/@\s]+@', 'https://***:***@', text)


def _run_git(repo_root: str, args: list[str]):
    import subprocess
    # 显式指定 utf-8——subprocess 的 text=True 在 Windows 上默认按系统代码页解码
    # （比如中文Windows是GBK），但 git 输出的分支名/提交信息/路径都是UTF-8，一旦
    # 出现中文关键词（分支名里就有，比如 ai-faq/20260729-黑上），GBK解码会直接
    # 抛 UnicodeDecodeError 崩掉整个脚本。生产环境是Linux（系统locale通常本来就是
    # UTF-8）不会踩到，但脚本不应该依赖运行环境的locale猜对编码。
    return subprocess.run(["git"] + args, cwd=repo_root, check=True,
                          capture_output=True, text=True, encoding="utf-8")


def _compare_url(repo_root: str, base_branch: str, branch: str) -> str:
    import subprocess
    try:
        url = subprocess.run(["git", "remote", "get-url", "origin"], cwd=repo_root,
                             check=True, capture_output=True, text=True,
                             encoding="utf-8").stdout.strip()
    except Exception:
        return "(无法读取 origin 远端地址，请手动去 GitHub 上开PR)"
    # 必须先确认 origin 真的是 github.com，不能看到"最后两段路径长得像owner/repo"
    # 就直接拼一个 github.com 链接——origin 完全可能是本地路径/自建git服务器，
    # 那样拼出来的链接纯属捏造，比"不知道"更误导人。
    if "github.com" not in url:
        return f"(origin（{_scrub_secrets(url)}）不是 GitHub 地址，请手动去对应的git托管平台开PR)"
    m = re.search(r'github\.com[:/]([^/:]+/[^/]+?)(?:\.git)?$', url)
    if not m:
        return f"(无法从 {_scrub_secrets(url)} 解析出 owner/repo，请手动去 GitHub 上开PR)"
    return f"https://github.com/{m.group(1)}/compare/{base_branch}...{branch}?expand=1"


def build_suggestion_doc(keyword: str, recs: list[dict], suggestion: str) -> tuple[str, str]:
    """返回 (PR标题, 建议文件正文)。"""
    pr_title = f"[AI草拟待review] 「{keyword}」相关知识库补充建议（{len(recs)}条证据）"
    lines = [
        f"# AI草拟建议：「{keyword}」",
        "",
        f"来源：`jianjia_gen_faq.py` 自动挖掘，共 {len(recs)} 条相关日志记录。",
        "",
        "**⚠️ 未经验证，仅供参考起草，不代表可以直接采用。合并前必须：**",
        "1. 人工判断这批案例反映的问题是否真实、建议是否合理，AI偶尔会分析错方向",
        "2. 决定具体要改哪个文件、放在哪个位置——本文件本身不在 `jianjia_knowledge/`",
        "   目录下，不会被 `jianjia_chat.py` 加载，不影响任何线上行为",
        "3. 用 `jianjia_debug.php` 跑2-3次重采样验证真的解决了问题，没有引入新的回归",
        "4. 确认无误后，在这个PR里追加一次commit，把内容整理进",
        "   `jianjia_soul.md` / `jianjia_knowledge/*.md` / `jianjia_prompts/*.md`",
        "   对应位置，再合并——合并这个动作本身就是让改动生效的唯一途径",
        "",
        "## AI草拟的建议文案",
        "",
        suggestion,
        "",
        "## 原始日志证据",
        "",
    ]
    for r in recs[:10]:
        channel = r.get("context") or r.get("event", "?")
        lines.append(f"- `[{r.get('ts', '?')}] [{channel}] {r.get('player', '?')}`：{r.get('user', '')}")
        if r.get("reply"):
            lines.append(f"  → AI候选回复：{r.get('reply')}")
        if r.get("_correction_msg"):
            lines.append(f"  → 后续追问（疑似纠正，来自 {r.get('_correction_by', '?')}）："
                          f"{r.get('_correction_msg')}")
    return pr_title, "\n".join(lines) + "\n"


def open_suggestion_pr(repo_root: str, base_branch: str, keyword: str, recs: list[dict],
                       suggestion: str, dry_run: bool) -> str:
    date_str = datetime.now().strftime("%Y%m%d")
    slug = slugify(keyword)
    branch = f"ai-faq/{date_str}-{slug}"
    rel_dir = "src/game/OO/jianjia_faq_suggestions"
    rel_path = f"{rel_dir}/{date_str}-{slug}.md"
    pr_title, content = build_suggestion_doc(keyword, recs, suggestion)

    if dry_run:
        return (f"[dry-run] 会创建分支 `{branch}`，写入 `{rel_path}`，"
                f"commit + push + 开PR「{pr_title}」（未实际执行任何git命令）")

    orig_branch = ""
    try:
        orig_branch = _run_git(repo_root, ["rev-parse", "--abbrev-ref", "HEAD"]).stdout.strip()
        _run_git(repo_root, ["checkout", "-b", branch, base_branch])
        full_dir = os.path.join(repo_root, rel_dir)
        os.makedirs(full_dir, exist_ok=True)
        with open(os.path.join(repo_root, rel_path), "w", encoding="utf-8") as f:
            f.write(content)
        _run_git(repo_root, ["add", rel_path])
        _run_git(repo_root, ["commit", "-m",
                             f"{pr_title}\n\nAuto-generated by jianjia_gen_faq.py --pr, needs human review."])
        _run_git(repo_root, ["push", "-u", "origin", branch])

        import shutil
        import subprocess
        if shutil.which("gh"):
            result = subprocess.run(
                ["gh", "pr", "create", "--title", pr_title, "--body", content,
                 "--base", base_branch, "--head", branch],
                cwd=repo_root, capture_output=True, text=True, encoding="utf-8",
            )
            outcome = result.stdout.strip() if result.returncode == 0 else (
                f"分支已推送，但 gh pr create 失败：{_scrub_secrets(result.stderr.strip())}\n"
                f"手动开PR：{_compare_url(repo_root, base_branch, branch)}")
        else:
            outcome = (f"分支已推送（未安装 gh CLI，无法自动开PR）。"
                       f"手动开PR：{_compare_url(repo_root, base_branch, branch)}")
        return outcome
    except Exception as e:
        import subprocess
        detail = e.stderr.strip() if isinstance(e, subprocess.CalledProcessError) and e.stderr else str(e)
        return f"（失败：{_scrub_secrets(detail)}）"
    finally:
        if orig_branch:
            try:
                _run_git(repo_root, ["checkout", orig_branch])
            except Exception:
                pass


# ── 报告输出 ──────────────────────────────────────────────────────────────────

def write_report(clusters, out_path: str, suggestions: dict, pr_results: dict) -> None:
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("# 蒹葭AI 待复核候选清单\n\n")
        f.write(f"生成时间：{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")
        f.write(
            "> 本报告仅供人工复核，**不代表任何一条建议已经验证正确**，不要直接"
            "照抄进知识库文件。每条建议都应该先用 `jianjia_debug.php` 跑2-3次"
            "重采样确认真的解决了问题、没有引入新的回归，再手动改 "
            "`jianjia_soul.md` / `jianjia_knowledge/*.md` / `jianjia_prompts/*.md`"
            " 并 `--reload` 生效。\n\n"
        )
        if not clusters:
            f.write("（本次扫描没有发现达到聚类阈值的候选——数据量不足，或者最近确实没有异常。）\n")
            return
        for kw, recs in clusters:
            flags = Counter(r["_flag"] for r in recs)
            flag_desc = "、".join(f"{k}×{v}" for k, v in flags.items())
            f.write(f"## 「{kw}」（{len(recs)}条，{flag_desc}）\n\n")
            for r in recs[:6]:
                channel = r.get("context") or r.get("event", "?")
                f.write(f"- `[{r.get('ts', '?')}] [{channel}] {r.get('player', '?')}`："
                        f"{r.get('user', '')}\n")
                if r.get("reply"):
                    f.write(f"  → AI候选回复：{r.get('reply')}\n")
                if r.get("_correction_msg"):
                    f.write(f"  → 后续追问（疑似纠正，来自 {r.get('_correction_by', '?')}）："
                            f"{r.get('_correction_msg')}\n")
            if len(recs) > 6:
                f.write(f"  …以及另外 {len(recs) - 6} 条同类记录（本报告每簇只列前6条，"
                        f"完整数据请自行查阅原始日志文件）\n")
            if kw in suggestions:
                f.write(f"\n**AI草拟建议（未经验证，仅供参考起草）：** {suggestions[kw]}\n")
            if kw in pr_results:
                f.write(f"\n**PR：** {pr_results[kw]}\n")
            f.write("\n")


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log-dir", default="logs", help="JianJia_*.jsonl 日志所在目录")
    ap.add_argument("--days", type=int, default=7, help="只看最近N天的日志，0表示不限制")
    ap.add_argument("--min-count", type=int, default=2, help="同一关键词至少出现几次才成簇")
    ap.add_argument("--out", default="jianjia_faq_candidates.md", help="报告输出路径")
    ap.add_argument("--draft", action="store_true",
                     help="额外调用Ollama为每个候选簇草拟建议文案（需要 --config 指向的Ollama可访问）")
    ap.add_argument("--config", default="jianjia.toml", help="--draft/--pr 时读取Ollama连接配置")
    ap.add_argument("--pr", action="store_true",
                     help="为排名最靠前的候选簇创建分支+提交待审建议文件+开PR（隐含--draft；"
                          "绝不直接改knowledge/soul/prompts文件，PR本身就是人工审核关卡）")
    ap.add_argument("--repo-root", default=".", help="--pr 时执行git命令的仓库根目录")
    ap.add_argument("--base-branch", default="development", help="--pr 时PR的目标分支")
    ap.add_argument("--top-k", type=int, default=3,
                     help="--pr 时最多为几个候选簇开PR（避免一次刷一堆PR）")
    ap.add_argument("--dry-run", action="store_true",
                     help="配合 --pr：只打印/记录会执行的操作，不实际跑git命令/不实际开PR")
    args = ap.parse_args()

    if args.pr:
        args.draft = True

    since = None
    if args.days > 0:
        since = datetime.now() - timedelta(days=args.days)

    records = list(iter_records(args.log_dir, since))
    flagged = find_flagged(records)
    clusters = cluster(flagged, args.min_count)

    ollama_cfg = None
    if args.draft:
        try:
            ollama_cfg = load_ollama_config(args.config)
        except Exception as e:
            print(f"[warn] 读取 {args.config} 失败，--draft/--pr 功能禁用: {e}", file=sys.stderr)
            args.draft = False
            args.pr = False

    suggestions: dict = {}
    pr_results: dict = {}

    if args.draft and ollama_cfg is not None:
        pr_targets = set(kw for kw, _ in clusters[:args.top_k]) if args.pr else set()
        for kw, recs in clusters:
            # 非 --pr 场景（纯 --draft）给所有簇都草拟；--pr 场景为控制Ollama调用量
            # 和PR数量，只给排名前 top_k 的簇草拟（其余簇仍然进报告，只是没有草拟文案）。
            if args.pr and kw not in pr_targets:
                continue
            suggestions[kw] = draft_suggestion(ollama_cfg, kw, recs)

        if args.pr:
            for kw, recs in clusters:
                if kw not in pr_targets:
                    continue
                pr_results[kw] = open_suggestion_pr(
                    args.repo_root, args.base_branch, kw, recs,
                    suggestions[kw], args.dry_run)
                print(f"[info] 「{kw}」→ {pr_results[kw]}", file=sys.stderr)

    write_report(clusters, args.out, suggestions, pr_results)
    print(f"[info] 扫描了 {len(records)} 条日志记录，标记 {len(flagged)} 条可疑记录，"
          f"聚类出 {len(clusters)} 个候选簇 → {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
