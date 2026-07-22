#!/usr/bin/env python3

"""Fetch checked native binary fixtures for real-world smoke tests."""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path


SKIP_CODE = 77


@dataclass(frozen=True)
class DebFixture:
    """A native fixture extracted from a pinned Debian package."""

    urls: tuple[str, ...]
    deb_name: str
    deb_sha256: str
    member_path: str
    binary_sha256: str


FIXTURES: dict[str, DebFixture] = {
    "fortune-x86_64": DebFixture(
        urls=(
            "https://archive.ubuntu.com/ubuntu/pool/universe/f/fortune-mod/"
            "fortune-mod_1.99.1-7.3build1_amd64.deb",
            "https://snapshot.ubuntu.com/ubuntu/pool/universe/f/fortune-mod/"
            "fortune-mod_1.99.1-7.3build1_amd64.deb",
        ),
        deb_name="fortune-mod_1.99.1-7.3build1_amd64.deb",
        deb_sha256="244da2ac7eeb1d8f99fce1ae8bbe534344f9db9d47fd2cdd0fc2348511f28bda",
        member_path="usr/games/fortune",
        binary_sha256="5e67d5be923d7e4ea3aa5ffbd1ea18057c042a953cc7137cb0293914453f75ee",
    ),
}


def require_fixtures() -> bool:
    value = os.environ.get("NOTDEC_REQUIRE_NATIVE_FIXTURES", "")
    return value.lower() in {"1", "true", "yes", "on"}


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1 if require_fixtures() else SKIP_CODE


def sha256(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def has_hash(path: Path, expected: str) -> bool:
    return path.is_file() and sha256(path) == expected


def download(urls: tuple[str, ...], dest: Path) -> str | None:
    last_error = ""
    for url in urls:
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                with dest.open("wb") as output:
                    shutil.copyfileobj(response, output)
            return None
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            last_error = f"{url}: {error}"
    return last_error or "no fixture URL configured"


def fetch_deb(fixture: DebFixture, cache_dir: Path) -> tuple[int, Path | None]:
    deb_path = cache_dir / fixture.deb_name
    if has_hash(deb_path, fixture.deb_sha256):
        return 0, deb_path

    cache_dir.mkdir(parents=True, exist_ok=True)
    tmp_path = cache_dir / f"{fixture.deb_name}.download"
    if tmp_path.exists():
        tmp_path.unlink()

    error = download(fixture.urls, tmp_path)
    if error is not None:
        return fail(f"SKIP: could not download native fixture: {error}"), None
    if sha256(tmp_path) != fixture.deb_sha256:
        tmp_path.unlink(missing_ok=True)
        return fail("SKIP: downloaded native fixture has unexpected sha256"), None

    tmp_path.replace(deb_path)
    return 0, deb_path


def extract_binary(fixture: DebFixture, deb_path: Path, binary_path: Path) -> int:
    dpkg_deb = shutil.which("dpkg-deb")
    if dpkg_deb is None:
        return fail("SKIP: dpkg-deb is required to extract native fixture")

    binary_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="extract-", dir=str(binary_path.parent)) as tmp:
        extract_root = Path(tmp) / "root"
        subprocess.run(
            [dpkg_deb, "-x", str(deb_path), str(extract_root)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        extracted = extract_root / fixture.member_path
        if not extracted.is_file():
            return fail(f"SKIP: fixture package does not contain {fixture.member_path}")
        if sha256(extracted) != fixture.binary_sha256:
            return fail("SKIP: extracted native fixture has unexpected sha256")
        shutil.copy2(extracted, binary_path)
    return 0


def fetch_fixture(name: str, source_dir: Path) -> tuple[int, Path | None]:
    fixture = FIXTURES[name]
    cache_root = Path(
        os.environ.get(
            "NOTDEC_NATIVE_FIXTURE_CACHE",
            source_dir / "tests" / "fixtures" / "native" / "downloads",
        )
    )
    cache_dir = cache_root / name
    binary_path = cache_dir / fixture.member_path

    if has_hash(binary_path, fixture.binary_sha256):
        return 0, binary_path

    rc, deb_path = fetch_deb(fixture, cache_dir)
    if rc != 0 or deb_path is None:
        return rc, None

    rc = extract_binary(fixture, deb_path, binary_path)
    if rc != 0:
        return rc, None
    return 0, binary_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("fixture", choices=sorted(FIXTURES))
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="NotDec-bin2llvm source directory",
    )
    args = parser.parse_args(argv)

    rc, binary = fetch_fixture(args.fixture, args.source_dir.resolve())
    if rc != 0:
        return rc
    assert binary is not None
    print(binary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
