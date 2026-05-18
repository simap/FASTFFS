#!/usr/bin/env bash
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/results"

DEFAULT_IDF_ENV="/Users/benh/.espressif/tools/activate_idf_v6.0-beta2.sh"
IDF_ENV="${IDF_ENV:-$DEFAULT_IDF_ENV}"
PORT="${ESPPORT:-/dev/cu.usbserial-10}"

VARIANT=""
PROJECT=""
BUILD_SUFFIX=""
LOG_NAME=""
APP_NAME=""
DONE_MATCH=""
DRY_RUN=0
NO_LOG=0

declare -a CMAKE_ARGS=()
declare -a RAW_ARGS=()
declare -a IDF_ACTIONS=()
declare -a MONITOR_ARGS=()

die() {
    echo "benchidf: $*" >&2
    exit 2
}

usage() {
    cat <<'EOF'
Usage:
  benchmarks/benchidf.sh --list
  benchmarks/benchidf.sh <variant> [options] [idf actions...] [monitor monitor-options...]

Examples:
  benchmarks/benchidf.sh fastffs-default build
  benchmarks/benchidf.sh fastffs-debt flash monitor --exit-on-crash
  benchmarks/benchidf.sh littlefs build flash monitor --exit-on-crash
  benchmarks/benchidf.sh spiffs --port /dev/cu.usbserial-10 flash monitor

Options:
  -p, --port <port>       Serial port. Defaults to ESPPORT or /dev/cu.usbserial-10.
  --build-name <name>     Override the build directory suffix under the project dir.
  --log-name <name>       Override the timestamped log file label.
  --no-log                Run monitor without creating a timestamped log.
  --dry-run               Print the commands that would run.
  -h, --help              Show this help.

Notes:
  The wrapper owns idf.py -C and -B. Passing -C, -B, or --build-dir is rejected.
  Build directories are always created under benchmarks/<project>/build-<name>.
  Monitor logs are written to benchmarks/results/<timestamp>_<name>.log.
EOF
}

list_variants() {
    cat <<'EOF'
Variants:
  fastffs, fastffs-default
  fastffs-debt
  fastffs-inline
  fastffs-noalloc-inline
  fastffs-noalloc-debt
  fastffs-crippled-inline
  fastffs-crippled-debt
  jesfs
  littlefs
  spiffs
EOF
}

safe_name() {
    printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9._-' '-'
}

contains_arg() {
    local needle="$1"
    shift
    local arg
    for arg in "$@"; do
        [[ "$arg" == "$needle" ]] && return 0
    done
    return 1
}

has_exit_on_match() {
    local arg
    for arg in "$@"; do
        [[ "$arg" == "--exit-on-match" ]] && return 0
        [[ "$arg" == --exit-on-match=* ]] && return 0
    done
    return 1
}

activate_idf() {
    if [[ -z "${IDF_PATH:-}" || -z "${IDF_PYTHON_ENV_PATH:-}" ]]; then
        [[ -f "$IDF_ENV" ]] || die "ESP-IDF environment script not found: $IDF_ENV"
        local line key value
        while IFS= read -r line; do
            [[ "$line" == *=* ]] || continue
            key="${line%%=*}"
            value="${line#*=}"
            export "$key=$value"
        done < <("$IDF_ENV" -e)
    fi

    [[ -n "${IDF_PATH:-}" ]] || die "IDF_PATH is not set after activating ESP-IDF"
    [[ -n "${IDF_PYTHON_ENV_PATH:-}" ]] || die "IDF_PYTHON_ENV_PATH is not set after activating ESP-IDF"
    if [[ -n "${SYSTEM_PATH:-}" ]]; then
        export PATH="$PATH:$SYSTEM_PATH"
    fi
    [[ -x "$IDF_PYTHON_ENV_PATH/bin/python3" ]] || die "IDF Python not executable: $IDF_PYTHON_ENV_PATH/bin/python3"
    [[ -f "$IDF_PATH/tools/idf.py" ]] || die "idf.py not found: $IDF_PATH/tools/idf.py"
}

resolve_variant() {
    VARIANT="$1"
    CMAKE_ARGS=()

    case "$VARIANT" in
        fastffs|fastffs-default)
            PROJECT="esp32s3_fastffs"
            BUILD_SUFFIX="fastffs-default"
            LOG_NAME="fastffs_default"
            APP_NAME="esp32s3_fastffs_bench"
            DONE_MATCH="FASTFFS ESP32-S3 benchmark done"
            ;;
        fastffs-debt)
            PROJECT="esp32s3_fastffs"
            BUILD_SUFFIX="fastffs-debt"
            LOG_NAME="fastffs_debt"
            APP_NAME="esp32s3_fastffs_bench"
            DONE_MATCH="FASTFFS ESP32-S3 benchmark done"
            CMAKE_ARGS+=("-DFASTFFS_BENCH_CHURN_GC_POLICY=FASTFFS_CHURN_GC_POLICY_DEBT")
            ;;
        fastffs-inline)
            PROJECT="esp32s3_fastffs"
            BUILD_SUFFIX="fastffs-inline"
            LOG_NAME="fastffs_inline"
            APP_NAME="esp32s3_fastffs_bench"
            DONE_MATCH="FASTFFS ESP32-S3 benchmark done"
            CMAKE_ARGS+=("-DFASTFFS_BENCH_CHURN_GC_POLICY=FASTFFS_CHURN_GC_POLICY_NONE")
            ;;
        fastffs-noalloc-inline)
            PROJECT="esp32s3_fastffs"
            BUILD_SUFFIX="fastffs-noalloc-inline"
            LOG_NAME="fastffs_noalloc_inline"
            APP_NAME="esp32s3_fastffs_bench"
            DONE_MATCH="FASTFFS ESP32-S3 benchmark done"
            CMAKE_ARGS+=("-DFASTFFS_BENCH_ALLOC_MAP_MODE=FFFS_ALLOC_MAP_NONE")
            CMAKE_ARGS+=("-DFASTFFS_BENCH_CHURN_GC_POLICY=FASTFFS_CHURN_GC_POLICY_NONE")
            ;;
        fastffs-noalloc-debt)
            PROJECT="esp32s3_fastffs"
            BUILD_SUFFIX="fastffs-noalloc-debt"
            LOG_NAME="fastffs_noalloc_debt"
            APP_NAME="esp32s3_fastffs_bench"
            DONE_MATCH="FASTFFS ESP32-S3 benchmark done"
            CMAKE_ARGS+=("-DFASTFFS_BENCH_ALLOC_MAP_MODE=FFFS_ALLOC_MAP_NONE")
            CMAKE_ARGS+=("-DFASTFFS_BENCH_CHURN_GC_POLICY=FASTFFS_CHURN_GC_POLICY_DEBT")
            ;;
        fastffs-crippled-inline)
            PROJECT="esp32s3_fastffs"
            BUILD_SUFFIX="fastffs-crippled-inline"
            LOG_NAME="fastffs_crippled_inline"
            APP_NAME="esp32s3_fastffs_bench"
            DONE_MATCH="FASTFFS ESP32-S3 benchmark done"
            CMAKE_ARGS+=("-DFASTFFS_BENCH_INDEX_CACHE_MODE=FFFS_INDEX_CACHE_NONE")
            CMAKE_ARGS+=("-DFASTFFS_BENCH_ALLOC_MAP_MODE=FFFS_ALLOC_MAP_NONE")
            CMAKE_ARGS+=("-DFASTFFS_BENCH_CHURN_GC_POLICY=FASTFFS_CHURN_GC_POLICY_NONE")
            CMAKE_ARGS+=("-DFASTFFS_BENCH_SCRATCH_SIZE=128")
            CMAKE_ARGS+=("-DFASTFFS_BENCH_FILE_WRITE_BUFFER=64")
            ;;
        fastffs-crippled-debt)
            PROJECT="esp32s3_fastffs"
            BUILD_SUFFIX="fastffs-crippled-debt"
            LOG_NAME="fastffs_crippled_debt"
            APP_NAME="esp32s3_fastffs_bench"
            DONE_MATCH="FASTFFS ESP32-S3 benchmark done"
            CMAKE_ARGS+=("-DFASTFFS_BENCH_INDEX_CACHE_MODE=FFFS_INDEX_CACHE_NONE")
            CMAKE_ARGS+=("-DFASTFFS_BENCH_ALLOC_MAP_MODE=FFFS_ALLOC_MAP_NONE")
            CMAKE_ARGS+=("-DFASTFFS_BENCH_CHURN_GC_POLICY=FASTFFS_CHURN_GC_POLICY_DEBT")
            CMAKE_ARGS+=("-DFASTFFS_BENCH_SCRATCH_SIZE=128")
            CMAKE_ARGS+=("-DFASTFFS_BENCH_FILE_WRITE_BUFFER=64")
            ;;
        jesfs)
            PROJECT="esp32s3_jesfs"
            BUILD_SUFFIX="jesfs"
            LOG_NAME="jesfs"
            APP_NAME="esp32s3_jesfs_bench"
            DONE_MATCH="JesFS ESP32-S3 benchmark done"
            ;;
        littlefs)
            PROJECT="esp32s3_littlefs"
            BUILD_SUFFIX="littlefs"
            LOG_NAME="littlefs"
            APP_NAME="esp32s3_littlefs_bench"
            DONE_MATCH="LittleFS ESP32-S3 benchmark done"
            ;;
        spiffs)
            PROJECT="esp32s3_spiffs"
            BUILD_SUFFIX="spiffs"
            LOG_NAME="spiffs"
            APP_NAME="esp32s3_spiffs_bench"
            DONE_MATCH="SPIFFS ESP32-S3 benchmark done"
            ;;
        *)
            die "unknown variant '$VARIANT'. Run benchmarks/benchidf.sh --list"
            ;;
    esac
}

reject_unsafe_args() {
    local arg
    for arg in "$@"; do
        case "$arg" in
            -B|--build-dir|-C|--project-dir)
                die "do not pass $arg; benchidf owns project and build directories"
                ;;
            -B*|--build-dir=*|-C*|--project-dir=*)
                die "do not pass $arg; benchidf owns project and build directories"
                ;;
        esac
    done
}

split_actions() {
    local seen_monitor=0
    local arg
    IDF_ACTIONS=()
    MONITOR_ARGS=()

    for arg in "$@"; do
        if (( seen_monitor )); then
            MONITOR_ARGS+=("$arg")
        elif [[ "$arg" == "monitor" ]]; then
            seen_monitor=1
        else
            IDF_ACTIONS+=("$arg")
        fi
    done

    if (( ! seen_monitor )) && [[ ${#IDF_ACTIONS[@]} -eq 0 ]]; then
        IDF_ACTIONS=("build")
    fi
}

needs_port() {
    local action
    for action in "$@"; do
        [[ "$action" == "flash" || "$action" == "encrypted-flash" || "$action" == "erase-flash" ]] && return 0
    done
    return 1
}

print_command() {
    printf '%q ' "$@"
    printf '\n'
}

run_monitor() {
    local build_dir="$1"
    local elf="$build_dir/$APP_NAME.elf"
    local log_path=""
    local timestamp
    local script_bin
    declare -a cmd=()

    if (( ! DRY_RUN )); then
        [[ -f "$elf" ]] || die "ELF not found: $elf; run build first"
    fi

    if [[ ${#MONITOR_ARGS[@]} -eq 0 ]] || ! contains_arg "--exit-on-crash" "${MONITOR_ARGS[@]}"; then
        MONITOR_ARGS+=("--exit-on-crash")
    fi
    if [[ -n "$DONE_MATCH" ]] && { [[ ${#MONITOR_ARGS[@]} -eq 0 ]] || ! has_exit_on_match "${MONITOR_ARGS[@]}"; }; then
        MONITOR_ARGS+=("--exit-on-match" "$DONE_MATCH")
    fi

    cmd=("$IDF_PYTHON_ENV_PATH/bin/idf-monitor" "-p" "$PORT" "$elf" "${MONITOR_ARGS[@]}")

    if (( NO_LOG )); then
        echo "benchidf: monitor without log"
        print_command "${cmd[@]}"
        (( DRY_RUN )) || "${cmd[@]}"
        return
    fi

    mkdir -p "$RESULTS_DIR"
    timestamp="$(date +%Y%m%d_%H%M%S)"
    log_path="$RESULTS_DIR/${timestamp}_${LOG_NAME}.log"
    echo "benchidf: logging monitor to $log_path"

    if (( DRY_RUN )); then
        print_command script -q "$log_path" "${cmd[@]}"
        return
    fi

    script_bin="$(command -v script || true)"
    if [[ -n "$script_bin" ]]; then
        "$script_bin" -q "$log_path" "${cmd[@]}"
    else
        "${cmd[@]}" | tee "$log_path"
    fi
}

if [[ $# -eq 0 ]]; then
    usage
    exit 2
fi

case "$1" in
    -h|--help)
        usage
        exit 0
        ;;
    --list)
        list_variants
        exit 0
        ;;
esac

resolve_variant "$1"
shift

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --list)
            list_variants
            exit 0
            ;;
        -p|--port)
            [[ $# -ge 2 ]] || die "$1 requires a value"
            PORT="$2"
            shift 2
            ;;
        --build-name)
            [[ $# -ge 2 ]] || die "$1 requires a value"
            BUILD_SUFFIX="$(safe_name "$2")"
            shift 2
            ;;
        --log-name)
            [[ $# -ge 2 ]] || die "$1 requires a value"
            LOG_NAME="$(safe_name "$2")"
            shift 2
            ;;
        --no-log)
            NO_LOG=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        *)
            RAW_ARGS+=("$1")
            shift
            ;;
    esac
done

reject_unsafe_args "${RAW_ARGS[@]}"
split_actions "${RAW_ARGS[@]}"

PROJECT_DIR="$SCRIPT_DIR/$PROJECT"
BUILD_DIR="$PROJECT_DIR/build-$BUILD_SUFFIX"

[[ -d "$PROJECT_DIR" ]] || die "project directory not found: $PROJECT_DIR"
case "$BUILD_DIR" in
    "$PROJECT_DIR"/build-*) ;;
    *) die "refusing unsafe build directory: $BUILD_DIR" ;;
esac

activate_idf

declare -a IDF_CMD=("$IDF_PYTHON_ENV_PATH/bin/python3" "$IDF_PATH/tools/idf.py" "-C" "$PROJECT_DIR" "-B" "$BUILD_DIR")
if [[ ${#CMAKE_ARGS[@]} -gt 0 ]]; then
    IDF_CMD+=("${CMAKE_ARGS[@]}")
fi
if needs_port "${IDF_ACTIONS[@]}"; then
    IDF_CMD+=("-p" "$PORT")
fi
IDF_CMD+=("${IDF_ACTIONS[@]}")

if [[ ${#IDF_ACTIONS[@]} -gt 0 ]]; then
    echo "benchidf: project $PROJECT"
    echo "benchidf: build dir $BUILD_DIR"
    print_command "${IDF_CMD[@]}"
    (( DRY_RUN )) || "${IDF_CMD[@]}"
fi

if contains_arg "monitor" "${RAW_ARGS[@]}"; then
    run_monitor "$BUILD_DIR"
fi
