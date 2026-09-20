#!/usr/bin/env bash
set -euo pipefail

# Run stress benchmark executables produced by utils/build_stress.sh.
# This script does not build. It consumes build/stress/manifest.tsv and writes
# timing logs for every profile/linkage/benchmark row.

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}" >/dev/null 2>&1 || true; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/stress"
MANIFEST_FILE="${BUILD_DIR}/manifest.tsv"
DEFAULT_RESULT_DIR="${ROOT_DIR}/tests/results/stress"
RUN_RESULT_DIR="${EMLOG_RESULTS_RUN_DIR:-${DEFAULT_RESULT_DIR}}"

if [[ -n "${EMLOG_RESULTS_RUN_DIR:-}" ]]; then
    RESULT_DIR="${RUN_RESULT_DIR}/stress"
    SUMMARY_FILE="${RUN_RESULT_DIR}/stress_summary.tsv"
else
    RESULT_DIR="${DEFAULT_RESULT_DIR}"
    SUMMARY_FILE="${DEFAULT_RESULT_DIR}/stress_summary.tsv"
fi

cd -- "${ROOT_DIR}"

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_file() {
    local file_path="$1"

    [[ -f "${file_path}" ]] || die "required file not found: ${file_path}"
}

write_summary_header() {
    mkdir -p "${RUN_RESULT_DIR}"
    printf '# status\tprofile\tlinkage\tbenchmark\tmean_ns_per_line\tmean_lines_per_s\tresult\texecutable\tlibrary\n' > "${SUMMARY_FILE}"
}

append_summary_row() {
    local status="$1"
    local profile="$2"
    local linkage="$3"
    local benchmark="$4"
    local mean_ns_per_line="$5"
    local mean_lines_per_s="$6"
    local result_file="$7"
    local executable="$8"
    local library="$9"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${status}" \
        "${profile}" \
        "${linkage}" \
        "${benchmark}" \
        "${mean_ns_per_line}" \
        "${mean_lines_per_s}" \
        "${result_file}" \
        "${executable}" \
        "${library}" >> "${SUMMARY_FILE}"
}

extract_mean_for_label() {
    local label="$1"
    local result_file="$2"

    awk -v wanted="${label}" '
        $0 == wanted { in_section = 1; next }
        in_section && /^  mean:/ { print $2; exit }
        in_section && /^[^ ]/ { in_section = 0 }
    ' "${result_file}"
}

extract_mean_ns_per_line() {
    local benchmark="$1"
    local result_file="$2"

    case "${benchmark}" in
        stress) extract_mean_for_label 'cost per line' "${result_file}" ;;
        stress_mt) extract_mean_for_label 'all per-thread cost samples' "${result_file}" ;;
        *) printf 'NA\n' ;;
    esac
}

extract_mean_lines_per_s() {
    local benchmark="$1"
    local result_file="$2"

    case "${benchmark}" in
        stress) extract_mean_for_label 'throughput' "${result_file}" ;;
        stress_mt) extract_mean_for_label 'aggregate throughput per run' "${result_file}" ;;
        *) printf 'NA\n' ;;
    esac
}

run_one_benchmark() {
    local profile="$1"
    local linkage="$2"
    local benchmark="$3"
    local executable="$4"
    local library_path="$5"
    local result_file="$6"
    local rc=0
    local library_dir
    local previous_ld_path

    mkdir -p "${result_file%/*}"

    printf '\n[%s/%s/%s]\n' "${profile}" "${linkage}" "${benchmark}"
    printf '  executable: %s\n' "${executable}"
    printf '  library:    %s\n' "${library_path}"
    printf '  result:     %s\n' "${result_file}"

    if [[ "${linkage}" == "shared" ]]; then
        library_dir="$(cd "$(dirname "${library_path}")" && pwd)"
        previous_ld_path="${LD_LIBRARY_PATH:-}"

        if [[ -n "${previous_ld_path}" ]]; then
            LD_LIBRARY_PATH="${library_dir}:${previous_ld_path}" \
                "${executable}" > "${result_file}" 2>&1 || rc=$?
        else
            LD_LIBRARY_PATH="${library_dir}" \
                "${executable}" > "${result_file}" 2>&1 || rc=$?
        fi
    else
        "${executable}" > "${result_file}" 2>&1 || rc=$?
    fi

    cat "${result_file}"

    if ((rc != 0)); then
        printf '  status: FAIL exit=%d\n' "${rc}"
    else
        printf '  status: PASS\n'
    fi

    return "${rc}"
}

print_summary() {
    local status profile linkage benchmark mean_ns_per_line mean_lines_per_s result_file executable library

    printf '\n[stress summary]\n'
    printf '  summary: %s\n' "${SUMMARY_FILE}"
    printf '  columns: status profile linkage benchmark mean_ns_per_line mean_lines_per_s result\n'

    while IFS=$'\t' read -r status profile linkage benchmark mean_ns_per_line mean_lines_per_s result_file executable library; do
        [[ -z "${status}" ]] && continue
        [[ "${status}" == \#* ]] && continue
        printf '  %-4s  %-11s %-7s %-9s  %12s ns/line  %14s lines/s  %s\n' \
            "${status}" \
            "${profile}" \
            "${linkage}" \
            "${benchmark}" \
            "${mean_ns_per_line}" \
            "${mean_lines_per_s}" \
            "${result_file}"
    done < "${SUMMARY_FILE}"
}

require_file "${MANIFEST_FILE}"
mkdir -p "${RESULT_DIR}"
write_summary_header

printf '[run_stress]\n'
printf '  manifest:    %s\n' "${MANIFEST_FILE}"
printf '  result dir:  %s\n' "${RESULT_DIR}"
printf '  summary:     %s\n' "${SUMMARY_FILE}"

STRESS_RC=0

while IFS=$'\t' read -r profile linkage benchmark executable library_path; do
    [[ -z "${profile}" ]] && continue
    [[ "${profile}" == \#* ]] && continue

    require_file "${executable}"
    [[ -x "${executable}" ]] || die "stress executable is not executable: ${executable}"
    require_file "${library_path}"

    result_file="${RESULT_DIR}/${profile}/${linkage}/${benchmark}_result.txt"

    if run_one_benchmark \
        "${profile}" \
        "${linkage}" \
        "${benchmark}" \
        "${executable}" \
        "${library_path}" \
        "${result_file}"; then
        mean_ns_per_line="$(extract_mean_ns_per_line "${benchmark}" "${result_file}")"
        mean_lines_per_s="$(extract_mean_lines_per_s "${benchmark}" "${result_file}")"
        append_summary_row PASS \
            "${profile}" \
            "${linkage}" \
            "${benchmark}" \
            "${mean_ns_per_line:-NA}" \
            "${mean_lines_per_s:-NA}" \
            "${result_file}" \
            "${executable}" \
            "${library_path}"
    else
        STRESS_RC=1
        append_summary_row FAIL \
            "${profile}" \
            "${linkage}" \
            "${benchmark}" \
            NA \
            NA \
            "${result_file}" \
            "${executable}" \
            "${library_path}"
    fi
done < "${MANIFEST_FILE}"

print_summary
exit "${STRESS_RC}"
