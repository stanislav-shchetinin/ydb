#!/usr/bin/env python3
"""
Проверка чексумм экспортированных файлов data_xx.csv.
Сравнивает SHA256 от содержимого data_xx.csv с записанной в data_xx.csv.sha256.

Использование:
    python3 check_export_checksums.py <директория>
    python3 check_export_checksums.py <хост>:<директория>

Примеры:
    python3 check_export_checksums.py /home/user/exports_fs/1/Table
    python3 check_export_checksums.py sas9-1583.host.testing.ydb.yandex.net:/home/user/exports_fs/1/Table
"""

import hashlib
import os
import sys
import glob
import subprocess
import tempfile
import shutil


def sha256_file(filepath: str) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def read_expected_checksum(sha_filepath: str) -> str:
    with open(sha_filepath, "r") as f:
        content = f.read().strip()
    # формат: "<hex_hash> <filename>" или просто "<hex_hash>"
    return content.split()[0]


def check_directory(directory: str) -> bool:
    data_files = sorted(glob.glob(os.path.join(directory, "data_*.csv")))
    if not data_files:
        # может быть со сжатием
        data_files = sorted(glob.glob(os.path.join(directory, "data_*.csv.zst")))
    if not data_files:
        print(f"  Нет файлов data_*.csv в {directory}")
        return True

    all_ok = True
    for data_file in data_files:
        basename = os.path.basename(data_file)
        # для data_XX.csv.zst чексумма считается по data_XX.csv (без .zst)
        checksum_name = basename
        if checksum_name.endswith(".zst"):
            checksum_name = checksum_name[:-4]
        sha_file = os.path.join(directory, checksum_name + ".sha256")

        if not os.path.exists(sha_file):
            print(f"  {basename}: ⚠ нет файла чексуммы ({checksum_name}.sha256)")
            continue

        actual = sha256_file(data_file)
        expected = read_expected_checksum(sha_file)

        if actual == expected:
            size = os.path.getsize(data_file)
            print(f"  {basename}: ✓ OK (sha256={actual[:16]}..., size={size})")
        else:
            size = os.path.getsize(data_file)
            print(f"  {basename}: ✗ MISMATCH (size={size})")
            print(f"    actual:   {actual}")
            print(f"    expected: {expected}")
            all_ok = False

    return all_ok


def main():
    if len(sys.argv) < 2:
        print(f"Использование: {sys.argv[0]} [хост:]<директория>")
        sys.exit(1)

    target = sys.argv[1]

    if ":" in target and not target.startswith("/"):
        host, remote_dir = target.split(":", 1)
        print(f"Копирование с {host}:{remote_dir} ...")
        local_tmp = tempfile.mkdtemp(prefix="checksum_verify_")
        try:
            subprocess.run(
                ["rsync", "-a", "--include=data_*", "--include=*/", "--exclude=*",
                 f"{host}:{remote_dir}/", f"{local_tmp}/"],
                check=True,
            )
            directory = local_tmp
            run_checks(directory)
        finally:
            shutil.rmtree(local_tmp, ignore_errors=True)
    else:
        directory = target
        if not os.path.isdir(directory):
            print(f"Директория не найдена: {directory}")
            sys.exit(1)
        run_checks(directory)


def run_checks(directory: str):
    # Ищем поддиректории с data-файлами (структура экспорта: dir/N/TableName/)
    subdirs = []
    for root, dirs, files in os.walk(directory):
        has_data = any(f.startswith("data_") and (f.endswith(".csv") or f.endswith(".csv.zst")) for f in files)
        if has_data:
            subdirs.append(root)

    if not subdirs:
        print(f"Не найдено файлов data_*.csv в {directory} и поддиректориях")
        sys.exit(1)

    total_ok = 0
    total_mismatch = 0

    for subdir in sorted(subdirs):
        rel = os.path.relpath(subdir, directory)
        print(f"\n[{rel}]")
        if check_directory(subdir):
            total_ok += 1
        else:
            total_mismatch += 1

    print(f"\n{'='*60}")
    print(f"Итого: {total_ok + total_mismatch} директорий, {total_ok} OK, {total_mismatch} с ошибками")

    if total_mismatch > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
