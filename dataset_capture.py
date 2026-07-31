"""Local dataset storage and capture client for the ESP32 camera."""

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
import csv
import hashlib
import json
import os
import re
import threading
import time
import unicodedata
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
    if any(unicodedata.category(character).startswith("C") for character in normalized):
        raise ValueError("Dataset name cannot contain control characters")
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
            metadata_existed = metadata_path.exists()
            metadata_size = metadata_path.stat().st_size if metadata_existed else 0
            if destination.exists():
                raise FileExistsError(destination)

            try:
                with temporary.open("xb") as frame_file:
                    frame_file.write(frame.payload)
                    frame_file.flush()
                    os.fsync(frame_file.fileno())
                temporary.replace(destination)
                try:
                    self._append_metadata(destination, frame, index)
                except Exception:
                    destination.unlink(missing_ok=True)
                    self._restore_metadata(metadata_path, metadata_existed, metadata_size)
                    raise
            finally:
                temporary.unlink(missing_ok=True)

            self._next_index += 1
            return destination

    @staticmethod
    def _restore_metadata(metadata_path: Path, existed: bool, size: int) -> None:
        if existed:
            with metadata_path.open("r+b") as metadata_file:
                metadata_file.truncate(size)
        else:
            metadata_path.unlink(missing_ok=True)

    def _append_metadata(self, destination: Path, frame: Frame, index: int) -> None:
        metadata_path = self.directory / "metadata.csv"
        write_header = not metadata_path.exists() or metadata_path.stat().st_size == 0
        row = {
            "relative_path": destination.name,
            "dataset_name": self.dataset_name,
            "index": index,
            "captured_at": frame.captured_at.isoformat(),
            "bytes": len(frame.payload),
            "sha256": hashlib.sha256(frame.payload).hexdigest(),
            "source_url": frame.source_url,
        }
        with metadata_path.open("a", encoding="utf-8", newline="") as metadata_file:
            writer = csv.DictWriter(metadata_file, fieldnames=self.METADATA_FIELDS)
            if write_header:
                writer.writeheader()
            writer.writerow(row)


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
