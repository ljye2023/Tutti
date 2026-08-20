"""StorageBackend SPI — 存储介质契约（D-010/D-011，ARCHITECTURE.md §3.2）。

engine 的唯一存储依赖：staging 槽 ↔ 持久化 之间的 chunk 段搬运。
零布局感知：不知道 vllm、不知道 gather/scatter、不知道 paged。

换存储后端 = 新增一个本 Protocol 的实现（MemoryBackend /
LocalStoreBackend / 未来的 RemoteBackend…），engine 及以上零改动。

chunk_id := 文件路径 key（D-011，opaque 字符串），由 engine 侧
ChunkIndex 分配，backend 自行解释（如 ``mem://slot/<i>``）。
"""

from typing import Protocol, runtime_checkable

ChunkKey = bytes  # 链式哈希（D-007）
ChunkId = str  # 文件路径 key（D-011，opaque；backend 自行解释）


@runtime_checkable
class StorageBackend(Protocol):
    """staging 槽 ↔ 持久化 之间的 chunk 段搬运契约。"""

    def chunk_paths(self, num_chunks: int) -> "list[str]":
        """生成容量为 num_chunks 的 chunk 路径池（engine 构造时调用一次）。

        路径是 opaque 字符串（D-011）：由本 backend 生成并解释，
        engine/adapter 只透传不解析。
        非法 num_chunks（<1 或超 backend 容量）→ ValueError。
        """
        ...

    def bind_staging(
        self,
        staging_addr: int,
        chunk_bytes: int,
        num_slots: int,
        segment_bytes: int,
        io_stream,
    ) -> None:
        """注册 staging 缓冲（介质所需的 DMA 注册等在此做）。

        staging_addr：staging 缓冲基地址（engine 分配并持有）；
        chunk_bytes：单个 chunk 总字节数（所有层段之和）；
        num_slots：staging 槽数（每槽 segment_bytes 字节）；
        segment_bytes：单层段字节数；
        io_stream：介质所需的 IO 流（CUDA stream 等；内存后端忽略）。
        """
        ...

    def write_chunk_segment(
        self, chunk_id: ChunkId, layer_idx: int, slot: int
    ) -> object:
        """异步：staging[slot] 的 layer 段 → 持久化。返回完成句柄。"""
        ...

    def read_chunk_segment(
        self, chunk_id: ChunkId, layer_idx: int, slot: int
    ) -> object:
        """异步：持久化 → staging[slot] 的 layer 段。返回完成句柄。"""
        ...

    def wait_idle(self) -> None:
        """等待全部在途搬运完成。"""
        ...

    def shutdown(self) -> None:
        """释放介质资源；幂等。"""
        ...
