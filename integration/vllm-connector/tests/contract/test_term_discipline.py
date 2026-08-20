"""术语纪律测试（T-115 验收 6）。

确认本卡零越层：具体介质词汇只许出现在 local_store_backend.py 与
local_store/（fake 与 stub 对接的测试侧）。检查对象是**代码 token**
（tokenize 剥离注释与 docstring——backend.py 的"铁律"声明文字本身
提及禁令词汇，属约束声明而非介质实现，不构成越层）。
"""

import io
import re
import tokenize
from pathlib import Path

_ENGINE_DIR = Path(__file__).resolve().parents[2] / "engine"  # vllm-connector/engine

# 既有 engine 层文件（本卡之前定稿，代码 token 不得出现介质词汇）
_ENGINE_FILES = [
    "core.py",
    "backend.py",
    "memory_backend.py",
    "chunk_index.py",
]

# 具体介质词汇（源自 ARCHITECTURE.md §0 + tutti_runtime 接线词汇）
_MEDIA_TERMS = re.compile(
    r"^(fd|epoll|io_uring|iouring|dma|cufile|gds|nvme|ssd|"
    r"uri|open_batch|register_memory|submit|release_io|"
    r"posix|o_direct|mmap|pread|pwrite|aio)$",
    re.IGNORECASE,
)


def _code_tokens(path):
    """生成代码 token（剥离注释/字符串/docstring）。"""
    with open(path, encoding="utf-8") as f:
        for tok in tokenize.generate_tokens(f.readline):
            if tok.type in (tokenize.COMMENT, tokenize.STRING, tokenize.NL,
                            tokenize.NEWLINE, tokenize.INDENT, tokenize.DEDENT,
                            tokenize.ENDMARKER):
                continue
            yield tok.string


def _media_hits(path):
    return sorted({t.lower() for t in _code_tokens(path)
                   if _MEDIA_TERMS.match(t)})


def test_engine_layer_files_have_no_media_terms():
    violations = {name: _media_hits(_ENGINE_DIR / name) for name in _ENGINE_FILES}
    violations = {k: v for k, v in violations.items() if v}
    assert not violations, f"engine 层代码出现介质词汇：{violations}"


def test_media_terms_only_in_local_store_backend():
    """engine/ 顶层介质词只许出现在 local_store_backend.py。"""
    for path in _ENGINE_DIR.glob("*.py"):
        if path.name == "local_store_backend.py":
            continue
        hits = _media_hits(path)
        assert not hits, f"{path.name} 代码出现介质词汇：{hits}"
