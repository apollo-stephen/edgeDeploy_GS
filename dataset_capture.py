"""Local dataset storage and capture client for the ESP32 camera."""

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
import csv
import hashlib
import io
import json
import os
import re
import stat
import threading
import time
from urllib import request


DEFAULT_DEVICE_URL = "http://192.168.4.1"
DEFAULT_INTERVAL_MS = 500
MIN_INTERVAL_MS = 200
MAX_INTERVAL_MS = 60_000
FRAME_RETRIES = 2
RETRY_DELAY_SECONDS = 0.5
MAX_CONSECUTIVE_FAILURES = 5
DATASET_NAME_MAX_LENGTH = 64


@dataclass(frozen=True)
class Frame:
    payload: bytes
    source_url: str
    captured_at: datetime


def normalize_dataset_name(value: str) -> str:
    """Return a displayable single-directory dataset name."""
    if not isinstance(value, str):
        raise ValueError("Dataset name must be text")

    normalized = value.strip()
    if not 1 <= len(normalized) <= DATASET_NAME_MAX_LENGTH:
        raise ValueError("Dataset name must contain 1 to 64 characters")
    if normalized in {".", ".."}:
        raise ValueError("Dataset name cannot be a path component")
    if "/" in normalized or "\\" in normalized:
        raise ValueError("Dataset name cannot contain path separators")
    if not all(
        character in " _-"
        or "A" <= character <= "Z"
        or "a" <= character <= "z"
        or "0" <= character <= "9"
        or "\u3400" <= character <= "\u4dbf"
        or "\u4e00" <= character <= "\u9fff"
        for character in normalized
    ):
        raise ValueError("Dataset name contains unsupported characters")
    return normalized


def resolve_dataset_directory(data_root: Path, dataset_name: str) -> Path:
    """Resolve a validated dataset directory without allowing root escape."""
    resolved_root = data_root.resolve()
    candidate = (resolved_root / normalize_dataset_name(dataset_name)).resolve()
    if not candidate.is_relative_to(resolved_root):
        raise ValueError("Dataset directory must stay under the data root")
    return candidate


def validate_frame(
    payload: bytes,
    content_type: str | None,
    width: str | None,
    height: str | None,
) -> None:
    """Reject anything other than a complete 128 by 128 JPEG frame."""
    media_type = content_type.split(";", 1)[0].strip().lower() if content_type else ""
    if media_type != "image/jpeg":
        raise ValueError("Capture response is not a JPEG")
    if not payload.startswith(b"\xff\xd8") or not payload.endswith(b"\xff\xd9"):
        raise ValueError("Capture response is not a complete JPEG")
    if width != "128" or height != "128":
        raise ValueError("Capture response is not 128 by 128")


class DatasetWriter:
    METADATA_FIELDS = (
        "relative_path",
        "dataset_name",
        "index",
        "captured_at",
        "bytes",
        "sha256",
        "source_url",
    )
    INDEX_PATTERN = re.compile(r"^(\d+)_.*\.jpe?g$", re.IGNORECASE)
    _directory_locks: dict[Path, threading.Lock] = {}
    _directory_locks_guard = threading.Lock()

    def __init__(self, data_root: Path, dataset_name: str) -> None:
        self.dataset_name = normalize_dataset_name(dataset_name)
        self.directory = resolve_dataset_directory(data_root, self.dataset_name)
        self.directory.mkdir(parents=True, exist_ok=True)
        self._lock = self._lock_for_directory(self.directory)
        self._next_index = self._find_next_index()

    @classmethod
    def _lock_for_directory(cls, directory: Path) -> threading.Lock:
        with cls._directory_locks_guard:
            return cls._directory_locks.setdefault(directory, threading.Lock())

    def _find_next_index(self) -> int:
        indexes = []
        for path in self.directory.iterdir():
            match = self.INDEX_PATTERN.fullmatch(path.name)
            if match:
                indexes.append(int(match.group(1)))
        return max(indexes, default=0) + 1

    def save(self, frame: Frame) -> Path:
        with self._lock:
            self._next_index = max(self._next_index, self._find_next_index())
            index = self._next_index
            timestamp = frame.captured_at.strftime("%Y%m%dT%H%M%S")
            filename = f"{index:05d}_{timestamp}_{frame.captured_at.microsecond // 1000:03d}.jpg"
            destination = self.directory / filename
            temporary = Path(f"{destination}.part")
            metadata_path = self.directory / "metadata.csv"
            if destination.exists():
                raise FileExistsError(destination)

            try:
                with temporary.open("xb") as frame_file:
                    frame_file.write(frame.payload)
                    frame_file.flush()
                    os.fsync(frame_file.fileno())
                temporary.replace(destination)
                metadata_created = False
                metadata_identity = None
                try:
                    (
                        metadata_fd,
                        metadata_created,
                        metadata_size,
                        metadata_identity,
                    ) = self._open_metadata(metadata_path)
                    try:
                        self._append_metadata(
                            metadata_fd,
                            metadata_size == 0,
                            destination,
                            frame,
                            index,
                        )
                        os.fsync(metadata_fd)
                    except Exception:
                        self._restore_metadata(metadata_fd, metadata_size)
                        raise
                    finally:
                        os.close(metadata_fd)
                except Exception:
                    destination.unlink(missing_ok=True)
                    if metadata_created and metadata_identity is not None:
                        self._unlink_metadata_if_same_file(
                            metadata_path,
                            metadata_identity,
                        )
                    raise
            finally:
                temporary.unlink(missing_ok=True)

            self._next_index += 1
            return destination

    @staticmethod
    def _open_metadata(metadata_path: Path) -> tuple[int, bool, int, tuple[int, int]]:
        flags = os.O_WRONLY | os.O_APPEND | os.O_NONBLOCK | os.O_NOFOLLOW
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        created = False
        try:
            metadata_fd = os.open(
                metadata_path,
                flags | os.O_CREAT | os.O_EXCL,
                0o644,
            )
            created = True
        except FileExistsError:
            metadata_fd = os.open(metadata_path, flags)

        try:
            metadata_stat = os.fstat(metadata_fd)
            if not stat.S_ISREG(metadata_stat.st_mode):
                raise OSError("metadata.csv must be a regular file")
            return (
                metadata_fd,
                created,
                metadata_stat.st_size,
                (metadata_stat.st_dev, metadata_stat.st_ino),
            )
        except Exception:
            os.close(metadata_fd)
            raise

    @staticmethod
    def _restore_metadata(metadata_fd: int, size: int) -> None:
        os.ftruncate(metadata_fd, size)
        os.fsync(metadata_fd)

    @staticmethod
    def _unlink_metadata_if_same_file(
        metadata_path: Path,
        identity: tuple[int, int],
    ) -> None:
        try:
            metadata_stat = metadata_path.stat(follow_symlinks=False)
        except FileNotFoundError:
            return
        if (metadata_stat.st_dev, metadata_stat.st_ino) == identity:
            metadata_path.unlink()

    @staticmethod
    def _write_metadata(metadata_fd: int, payload: bytes) -> None:
        remaining = memoryview(payload)
        while remaining:
            written = os.write(metadata_fd, remaining)
            if written == 0:
                raise OSError("metadata.csv write made no progress")
            remaining = remaining[written:]

    def _append_metadata(
        self,
        metadata_fd: int,
        write_header: bool,
        destination: Path,
        frame: Frame,
        index: int,
    ) -> None:
        row = {
            "relative_path": destination.name,
            "dataset_name": self.dataset_name,
            "index": index,
            "captured_at": frame.captured_at.isoformat(),
            "bytes": len(frame.payload),
            "sha256": hashlib.sha256(frame.payload).hexdigest(),
            "source_url": frame.source_url,
        }
        metadata_buffer = io.StringIO(newline="")
        writer = csv.DictWriter(metadata_buffer, fieldnames=self.METADATA_FIELDS)
        if write_header:
            writer.writeheader()
        writer.writerow(row)
        self._write_metadata(metadata_fd, metadata_buffer.getvalue().encode("utf-8"))


class Esp32Client:
    def __init__(
        self,
        base_url: str = DEFAULT_DEVICE_URL,
        timeout_seconds: float = 10.0,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout_seconds = timeout_seconds

    def fetch_status(self) -> dict[str, object]:
        return self._fetch_status(self.timeout_seconds)

    def _fetch_status(self, timeout_seconds: float) -> dict[str, object]:
        with request.urlopen(
            f"{self.base_url}/api/status", timeout=timeout_seconds
        ) as response:
            if response.getcode() != 200:
                raise ValueError("ESP32 status request failed")
            try:
                status = json.loads(response.read())
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise ValueError("ESP32 status response is not JSON") from error

        if not isinstance(status, dict):
            raise ValueError("ESP32 status response is not an object")
        if status.get("camera_ready") is not True:
            raise ValueError("ESP32 camera is not ready")
        if status.get("frame_size") != "128x128":
            raise ValueError("ESP32 frame size is not 128x128")
        return status

    def wait_for_stream_release(self, timeout_seconds: float = 3.0) -> None:
        deadline = time.monotonic() + timeout_seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("ESP32 stream client did not disconnect in time")
            status = self._fetch_status(min(self.timeout_seconds, remaining))
            if status.get("stream_client_connected") is False:
                return
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("ESP32 stream client did not disconnect in time")
            time.sleep(min(0.1, remaining))

    def capture(self) -> Frame:
        capture_url = f"{self.base_url}/capture?t={time.time_ns()}"
        with request.urlopen(capture_url, timeout=self.timeout_seconds) as response:
            if response.getcode() != 200:
                raise ValueError("ESP32 capture request failed")
            payload = response.read()
            validate_frame(
                payload,
                response.headers.get("Content-Type"),
                response.headers.get("X-Frame-Width"),
                response.headers.get("X-Frame-Height"),
            )
        return Frame(
            payload=payload,
            source_url=capture_url,
            captured_at=datetime.now().astimezone(),
        )


class CaptureAlreadyRunningError(RuntimeError):
    pass


class CaptureManager:
    def __init__(self, data_root: Path, client: Esp32Client) -> None:
        self._data_root = data_root
        self._client = client
        self._lock = threading.Lock()
        self._stop_event = threading.Event()
        self._worker: threading.Thread | None = None
        self._state = {
            "running": False,
            "dataset_name": None,
            "interval_ms": DEFAULT_INTERVAL_MS,
            "saved_count": 0,
            "failed_count": 0,
            "latest_file": None,
            "last_error": None,
        }

    def start(self, dataset_name: str, interval_ms: int) -> dict[str, object]:
        if not MIN_INTERVAL_MS <= interval_ms <= MAX_INTERVAL_MS:
            raise ValueError(
                f"Capture interval must be between {MIN_INTERVAL_MS} and {MAX_INTERVAL_MS} ms"
            )
        dataset_name = normalize_dataset_name(dataset_name)

        with self._lock:
            if self._state["running"]:
                raise CaptureAlreadyRunningError("Dataset capture is already running")
            writer = DatasetWriter(self._data_root, dataset_name)
            self._stop_event.clear()
            self._state = {
                "running": True,
                "dataset_name": dataset_name,
                "interval_ms": interval_ms,
                "saved_count": 0,
                "failed_count": 0,
                "latest_file": None,
                "last_error": None,
            }
            self._worker = threading.Thread(
                target=self._run,
                args=(writer, interval_ms),
                name="dataset-capture",
                daemon=True,
            )
            self._worker.start()
            return dict(self._state)

    def _run(self, writer: DatasetWriter, interval_ms: int) -> None:
        consecutive_failures = 0
        try:
            self._client.wait_for_stream_release(3.0)
            while not self._stop_event.is_set():
                last_error = None
                for attempt in range(FRAME_RETRIES + 1):
                    if self._stop_event.is_set():
                        return
                    try:
                        destination = writer.save(self._client.capture())
                    except Exception as error:
                        last_error = error
                        if attempt < FRAME_RETRIES and self._stop_event.wait(RETRY_DELAY_SECONDS):
                            return
                    else:
                        consecutive_failures = 0
                        with self._lock:
                            self._state["saved_count"] += 1
                            self._state["latest_file"] = str(destination)
                            self._state["last_error"] = None
                        if self._stop_event.wait(interval_ms / 1000):
                            return
                        break
                else:
                    consecutive_failures += 1
                    with self._lock:
                        self._state["failed_count"] += 1
                        self._state["last_error"] = f"采集失败: {last_error}"
                    if consecutive_failures >= MAX_CONSECUTIVE_FAILURES:
                        with self._lock:
                            self._state["last_error"] = (
                                f"连续 {MAX_CONSECUTIVE_FAILURES} 张采集失败: {last_error}"
                            )
                        return
        except Exception as error:
            with self._lock:
                self._state["last_error"] = f"采集失败: {error}"
        finally:
            with self._lock:
                self._state["running"] = False

    def snapshot(self) -> dict[str, object]:
        with self._lock:
            return dict(self._state)

    def stop(self) -> dict[str, object]:
        with self._lock:
            self._stop_event.set()
            worker = self._worker
        if worker is not None and worker is not threading.current_thread():
            worker.join()
        return self.snapshot()

    def shutdown(self) -> None:
        self.stop()
