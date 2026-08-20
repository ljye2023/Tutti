"""contract 测试公共配置：vllm-connector/（engine 包父目录）入 sys.path。

`python -m pytest`（cwd=vllm-connector/）时 cwd 已在 sys.path；此处为
直接 pytest 调用提供兜底。
"""

import sys
from pathlib import Path

_CONNECTOR_ROOT = Path(__file__).resolve().parents[2]
if str(_CONNECTOR_ROOT) not in sys.path:
    sys.path.insert(0, str(_CONNECTOR_ROOT))
