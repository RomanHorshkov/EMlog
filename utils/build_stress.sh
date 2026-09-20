#!/usr/bin/env bash
set -euo pipefail

# Build stress benchmark executables against every requested emlog library
# artifact. This script only builds. It does not run benchmarks.
#
# Outputs:
#   build/stress/<profile>/<linkage>/stress
#   build/stress/<profile>/<linkage>/stress_mt
#   build/stress/manifest.tsv

CC="${CC:-gcc}"

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}" >/dev/null 2>&1 || true; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROFILE_FILE="${SCRIPT_DIR}/gcc_build_profiles.sh"
BUILD_DIR="${ROOT_DIR}/build/stress"
MANIFEST_FILE="${BUILD_DIR}/manifest.tsv"

cd -- "${ROOT_DIR}"

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_tool() {
    local tool="$1"

    if ! command -v "${tool}" >/dev/null 2>&1; then
        die "required tool not found: ${tool}"
    fi
}

require_file() {
    local file_path="$1"

    [[ -f "${file_path}" ]] || die "required file not found: ${file_path}"
}

require_array() {
    local array_name="$1"

    if ! declare -p "${array_name}" >/dev/null 2>&1; then
        die "required flag array is not defined: ${array_name}"
    fi
}

profile_is_known() {
    local requested_profile="$1"
    local known_profile

    for known_profile in "${GCC_BUILD_PROFILES[@]}"; do
        if [[ "${requested_profile}" == "${known_profile}" ]]; then
            return 0
        fi
    done

    return 1
}

add_profile_once() {
    local profile="$1"

    if [[ -z "${REQUESTED_PROFILE_SET[${profile}]:-}" ]]; then
        REQUESTED_PROFILES+=("${profile}")
        REQUESTED_PROFILE_SET["${profile}"]=1
    fi
}

write_manifest_header() {
    mkdir -p "${BUILD_DIR}"
    printf '# profile\tlinkage\tbenchmark\texecutable\tlibrary\n' > "${MANIFEST_FILE}"
}

append_manifest_row() {
    local profile="$1"
    local linkage="$2"
    local benchmark="$3"
    local executable="$4"
    local library="$5"

    printf '%s\t%s\t%s\t%s\t%s\n' \
        "${profile}" \
        "${linkage}" \
        "${benchmark}" \
        "${executable}" \
        "${library}" >> "${MANIFEST_FILE}"
}

profile_arrays() {
    local profile="$1"
    local upper_profile="${profile^^}"

    CPPFLAGS_ARRAY="CPPFLAGS_${upper_profile}"
    CFLAGS_ARRAY="CFLAGS_${upper_profile}"
    LDFLAGS_ARRAY="LDFLAGS_${upper_profile}"

    if [[ "${profile}" == "release_cov" ]]; then
        CPPFLAGS_RELEASE_COV_STRESS=("${CPPFLAGS_RELEASE[@]}")
        CFLAGS_RELEASE_COV_STRESS=("${CFLAGS_RELEASE[@]}")
        LDFLAGS_RELEASE_COV_STRESS=(
          "${LDFLAGS_RELEASE[@]}"
          "${LDFLAGS_INSTRUMENT_COVERAGE[@]}"
        )

        CPPFLAGS_ARRAY="CPPFLAGS_RELEASE_COV_STRESS"
        CFLAGS_ARRAY="CFLAGS_RELEASE_COV_STRESS"
        LDFLAGS_ARRAY="LDFLAGS_RELEASE_COV_STRESS"
    fi

    require_array "${CPPFLAGS_ARRAY}"
    require_array "${CFLAGS_ARRAY}"
    require_array "${LDFLAGS_ARRAY}"
}

build_one_benchmark() {
    local profile="$1"
    local linkage="$2"
    local benchmark="$3"
    local source_file="$4"
    local library_path="$5"
    local output_dir="${BUILD_DIR}/${profile}/${linkage}"
    local object_file="${output_dir}/${benchmark}.o"
    local executable="${output_dir}/${benchmark}"
    local library_dir
    local -a link_args=()

    local -n cppflags_ref="${CPPFLAGS_ARRAY}"
    local -n cflags_ref="${CFLAGS_ARRAY}"
    local -n ldflags_ref="${LDFLAGS_ARRAY}"

    mkdir -p "${output_dir}"

    case "${linkage}" in
        static)
            link_args=("${library_path}")
            ;;
        shared)
            library_dir="$(cd "$(dirname "${library_path}")" && pwd)"
            link_args=("${library_path}")
            ;;
        *)
            die "unknown stress linkage: ${linkage}"
            ;;
    esac

    printf '  [%s/%s/%s] compiling %s\n' \
        "${profile}" "${linkage}" "${benchmark}" "${object_file}"
    "${CC}" \
        "${cppflags_ref[@]}" \
        "${cflags_ref[@]}" \
        -pthread \
        -Iapp \
        -Itests/stress \
        -c "${source_file}" \
        -o "${object_file}"

    printf '  [%s/%s/%s] linking %s\n' \
        "${profile}" "${linkage}" "${benchmark}" "${executable}"
    "${CC}" \
        "${ldflags_ref[@]}" \
        "${object_file}" \
        "${link_args[@]}" \
        -o "${executable}" \
        -lm \
        -pthread

    append_manifest_row \
        "${profile}" \
        "${linkage}" \
        "${benchmark}" \
        "${executable}" \
        "${library_path}"
}

build_profile_linkage() {
    local profile="$1"
    local linkage="$2"
    local library_path

    case "${linkage}" in
        static) library_path="${ROOT_DIR}/build/${profile}/libemlog.a" ;;
        shared) library_path="${ROOT_DIR}/build/${profile}/libemlog.so" ;;
        *) die "unknown stress linkage: ${linkage}" ;;
    esac

    require_file "${library_path}"

    build_one_benchmark "${profile}" "${linkage}" stress \
        tests/stress/stress.c "${library_path}"
    build_one_benchmark "${profile}" "${linkage}" stress_mt \
        tests/stress/stress_mt.c "${library_path}"
}

require_tool "${CC}"
require_file "${PROFILE_FILE}"
require_file "${SCRIPT_DIR}/build_libs.sh"
require_file tests/stress/stress.c
require_file tests/stress/stress_mt.c

source "${PROFILE_FILE}"

require_array GCC_BUILD_PROFILES
require_array CPPFLAGS_RELEASE
require_array CFLAGS_RELEASE
require_array LDFLAGS_RELEASE
require_array LDFLAGS_INSTRUMENT_COVERAGE

declare -a REQUESTED_PROFILES=()
declare -A REQUESTED_PROFILE_SET=()

if (($# == 0)); then
    for profile in "${GCC_BUILD_PROFILES[@]}"; do
        add_profile_once "${profile}"
    done
else
    for requested_profile in "$@"; do
        case "${requested_profile}" in
            all)
                for profile in "${GCC_BUILD_PROFILES[@]}"; do
                    add_profile_once "${profile}"
                done
                            ;;
            profiles)
                for profile in "${GCC_BUILD_PROFILES[@]}"; do
                    add_profile_once "${profile}"
                done
                ;;
            coverage|release_cov)
                            ;;
            *)
                if profile_is_known "${requested_profile}"; then
                    add_profile_once "${requested_profile}"
                else
                    die "unknown stress profile: ${requested_profile}"
                fi
                ;;
        esac
    done
fi

if ((${#REQUESTED_PROFILES[@]} == 0)); then
    die "no stress profiles requested"
fi

mkdir -p "${BUILD_DIR}"
write_manifest_header

printf 'building EMlog libraries for stress matrix\n'
printf '  profiles:'
printf ' %s' "${REQUESTED_PROFILES[@]}"
printf '\n'
"${SCRIPT_DIR}/build_libs.sh" "${REQUESTED_PROFILES[@]}"

printf '\nbuilding stress benchmark matrix\n'
printf '  build dir: %s\n' "${BUILD_DIR}"
printf '  manifest:  %s\n' "${MANIFEST_FILE}"

for profile in "${REQUESTED_PROFILES[@]}"; do
    profile_arrays "${profile}"
    build_profile_linkage "${profile}" static
    build_profile_linkage "${profile}" shared
done

printf '\nbuilt stress benchmark executables:\n'
while IFS=$'\t' read -r profile linkage benchmark executable library; do
    [[ -z "${profile}" ]] && continue
    [[ "${profile}" == \#* ]] && continue
    printf '  %-11s %-7s %-9s -> %s\n' \
        "${profile}" \
        "${linkage}" \
        "${benchmark}" \
        "${executable}"
    printf '    library: %s\n' "${library}"
done < "${MANIFEST_FILE}"
