#!/usr/bin/env bash

set -euo pipefail

benchmark_binary=${1:-build/xournalpp}
document=${2:-}
iterations=${3:-5}

if [[ ! -x ${benchmark_binary} ]]; then
    echo "Xournal++ binary is not executable: ${benchmark_binary}" >&2
    exit 1
fi
if [[ -z ${document} || ! -f ${document} ]]; then
    echo "Usage: $0 [xournalpp-binary] document [iterations]" >&2
    exit 1
fi
if [[ ! ${iterations} =~ ^[1-9][0-9]*$ ]]; then
    echo "Iterations must be a positive integer: ${iterations}" >&2
    exit 1
fi

benchmark_binary=$(realpath "${benchmark_binary}")
document=$(realpath "${document}")
binary_dir=$(dirname "${benchmark_binary}")
benchmark_root=$(mktemp -d /tmp/xopp-gui-load.XXXXXX)
launcher_dir=
cleanup() {
    rm -rf -- "${benchmark_root}"
    if [[ -n ${launcher_dir} ]]; then
        rm -rf -- "${launcher_dir}"
    fi
}
trap cleanup EXIT

launch_binary=${benchmark_binary}
# Build-tree resources are staged beside the binary, while Xournal++ searches
# one directory above it. Put a hard link in a temporary child directory so the
# normal installed-layout lookup also works for an uninstalled build.
if [[ -f ${binary_dir}/ui/about.glade ]]; then
    launcher_dir=$(mktemp -d "${binary_dir}/.xopp-gui-load.XXXXXX")
    launch_binary=${launcher_dir}/xournalpp
    ln "${benchmark_binary}" "${launch_binary}"
fi

mkdir -p "${benchmark_root}/config" "${benchmark_root}/data"
durations=()
peak_rss=0

for ((iteration = 1; iteration <= iterations; ++iteration)); do
    output=$(
        env \
            GSETTINGS_BACKEND=memory \
            NO_AT_BRIDGE=1 \
            XDG_CONFIG_HOME="${benchmark_root}/config" \
            XDG_DATA_HOME="${benchmark_root}/data" \
            XOPP_BENCHMARK_GUI_LOAD=quit \
            "${launch_binary}" --disable-audio "${document}" 2>&1
    )
    result=$(sed -n 's/^.*\(XOPP_GUI_LOAD_RESULT .*$\)/\1/p' <<<"${output}")
    if [[ -z ${result} ]]; then
        echo "GUI load benchmark did not produce a result:" >&2
        echo "${output}" >&2
        exit 1
    fi

    echo "${result}"
    duration=$(sed -n 's/.*"duration_ms":\([0-9.]*\).*/\1/p' <<<"${result}")
    rss=$(sed -n 's/.*"peak_rss_kib":\([0-9]*\).*/\1/p' <<<"${result}")
    durations+=("${duration}")
    if [[ -n ${rss} && ${rss} -gt ${peak_rss} ]]; then
        peak_rss=${rss}
    fi
done

mapfile -t sorted_durations < <(printf '%s\n' "${durations[@]}" | sort -n)
middle=$((iterations / 2))
if ((iterations % 2 == 1)); then
    median=${sorted_durations[middle]}
else
    median=$(awk -v low="${sorted_durations[middle - 1]}" -v high="${sorted_durations[middle]}" \
        'BEGIN { printf "%.3f", (low + high) / 2 }')
fi

printf 'XOPP_GUI_LOAD_SUMMARY {"iterations":%d,"median_ms":%s,"max_peak_rss_kib":%d}\n' \
    "${iterations}" "${median}" "${peak_rss}"
