#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${LEVELDB_BUILD_DIR:-./build}"
RESULT_DIR="${LEVELDB_RESULTS_DIR:-${BUILD_DIR}/results}"
OUT="${RESULT_DIR}/task1_db_bench_results.txt"
DB_BENCH="${LEVELDB_DB_BENCH:-${BUILD_DIR}/bin/db_bench}"
DB_ROOT="${LEVELDB_DATA_DIR:-${BUILD_DIR}/data/task1}"

mkdir -p "${RESULT_DIR}" "${DB_ROOT}"

NUM="${LEVELDB_BENCH_NUM:-2097152}"
READS="${LEVELDB_BENCH_READS:-${NUM}}"
VALUE_SIZE="${LEVELDB_BENCH_VALUE_SIZE:-1024}"

DEFAULT_WRITE_BUFFER_SIZE=4194304
DEFAULT_BLOCK_SIZE=4096
DEFAULT_BLOOM_BITS=-1

run_case() {
  local label="$1"
  local case_name="$2"
  local benchmarks="$3"
  local result_filter="$4"
  local write_buffer_size="$5"
  local block_size="$6"
  local bloom_bits="$7"

  {
    echo "### ${label}"
    "${DB_BENCH}" \
      --benchmarks="${benchmarks}" \
      --num="${NUM}" \
      --reads="${READS}" \
      --value_size="${VALUE_SIZE}" \
      --db="${DB_ROOT}/${case_name}" \
      --write_buffer_size="${write_buffer_size}" \
      --block_size="${block_size}" \
      --bloom_bits="${bloom_bits}" \
      2>&1 | tr '\r' '\n' | grep -E "${result_filter}"
    echo
  } >> "${OUT}"
}

: > "${OUT}"

run_case "BASE" \
  "BASE" \
  "fillseq,fillrandom,readseq,readrandom" \
  "^(fillseq|fillrandom|readseq|readrandom)[[:space:]]*:" \
  "${DEFAULT_WRITE_BUFFER_SIZE}" "${DEFAULT_BLOCK_SIZE}" "${DEFAULT_BLOOM_BITS}"

run_case "write_buffer_size=16MB" \
  "write_buffer_size_16MB" \
  "fillseq,fillrandom,readseq,readrandom" \
  "^(fillseq|fillrandom|readseq|readrandom)[[:space:]]*:" \
  16777216 "${DEFAULT_BLOCK_SIZE}" "${DEFAULT_BLOOM_BITS}"

run_case "write_buffer_size=64MB" \
  "write_buffer_size_64MB" \
  "fillseq,fillrandom,readseq,readrandom" \
  "^(fillseq|fillrandom|readseq|readrandom)[[:space:]]*:" \
  67108864 "${DEFAULT_BLOCK_SIZE}" "${DEFAULT_BLOOM_BITS}"

run_case "block_size=16KB" \
  "block_size_16KB" \
  "fillseq,fillrandom,readseq,readrandom" \
  "^(fillseq|fillrandom|readseq|readrandom)[[:space:]]*:" \
  "${DEFAULT_WRITE_BUFFER_SIZE}" 16384 "${DEFAULT_BLOOM_BITS}"

run_case "block_size=64KB" \
  "block_size_64KB" \
  "fillseq,fillrandom,readseq,readrandom" \
  "^(fillseq|fillrandom|readseq|readrandom)[[:space:]]*:" \
  "${DEFAULT_WRITE_BUFFER_SIZE}" 65536 "${DEFAULT_BLOOM_BITS}"

run_case "filter_policy=BloomFilter_10_bits_per_key" \
  "filter_policy_bloom_10_bits_per_key" \
  "fillrandom,readrandom" \
  "^readrandom[[:space:]]*:" \
  "${DEFAULT_WRITE_BUFFER_SIZE}" "${DEFAULT_BLOCK_SIZE}" 10

run_case "filter_policy=off" \
  "filter_policy_off" \
  "fillrandom,readrandom" \
  "^readrandom[[:space:]]*:" \
  "${DEFAULT_WRITE_BUFFER_SIZE}" "${DEFAULT_BLOCK_SIZE}" -1

echo "db_bench 结果已输出到: ${OUT}"
