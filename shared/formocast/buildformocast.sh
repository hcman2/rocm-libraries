#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage:
  buildformocast.sh [pool <formocast|origami_sk0_ntab0>] [--build_dir <path>] [--rocm <path>] --arch <gfx950>] --prepare] --run_bench] --logs_dir <path>] --device <gpuid>]

Options:
  pool                     Solution pool. Defaults to formocast.
  -a, --arch <arch>        Specify gpu arch. Default to gfx950
  -b, --build_dir <path>   Build directory (optional). Defaults to ./build_\${pool}
  --rocm <path>            Specify rocm-libraries path. Default to ./rocm-libraries
  --prepare                Handling rocm-libraries downloads. Default to false
  --run_bench              Runing bf16 bench. Default to false
  --logs_dir <path>        Directory for bench outputs. Default to ./logs
  --device <gpuid>         GPU id for running bench. Default to 0
  --help                   Print this.

Examples:
  buildformocast.sh                               # build formocast pool at build_formocast
  buildformocast.sh origami_sk0_ntab0             # build origami_sk0_ntab0 pool at build_origami_sk0_ntab0
  buildformocast.sh formocast --out_dir /tmp/out  # build origami_sk0_ntab0 pool at /tmp/out
EOF
    exit 1
}

rocm='./rocm-libraries'
user_dir_set=false
pool='formocast'
arch='gfx950'
prepare=false
logs_dir='./logs'
run_bench=false
device=0

while [[ $# -gt 0 ]]; do
    case "$1" in
	--help)
	    usage
	    exit 0
	    ;;
        --rocm)
            if [[ $# -lt 2 ]]; then
                echo "Error: '$1' requires a value." >&2
                usage
            fi
            rocm="$2"
            shift 2
            ;;
        --device)
            if [[ $# -lt 2 ]]; then
                echo "Error: '$1' requires a value." >&2
                usage
            fi
            device="$2"
            shift 2
            ;;
        -a|--arch)
            if [[ $# -lt 2 ]]; then
                echo "Error: '$1' requires a value." >&2
                usage
            fi
            arch="$2"
            shift 2
            ;;
        -b|--build_dir)
            if [[ $# -lt 2 ]]; then
                echo "Error: '$1' requires a value." >&2
                usage
            fi
            build_dir="$2"
	    user_dir_set=true
            shift 2
            ;;
        --arch=*)
            arch="${1#*=}"
            shift
            ;;
        --build_dir=*)
            build_dir="${1#*=}"
	    user_dir_set=true
            shift
            ;;
	--prepare)
	    prepare=true
	    shift
	    ;;
	--run_bench)
	    run_bench=true
	    shift
	    ;;
        --logs_dir)
            if [[ $# -lt 2 ]]; then
                echo "Error: '$1' requires a value." >&2
                usage
            fi
            logs_dir="$2"
            shift 2
            ;;
	formocast|origami_sk0_ntab0)
	    pool="$1"
	    shift
	    ;;
        -*)
            echo "Error: unknown option '$1'." >&2
            usage
            ;;
        *)
            echo "Error: unexpected positional argument '$1'." >&2
            usage
            ;;
    esac
done
if [[ "$user_dir_set" == false ]]; then
    build_dir="./build_${pool}"
fi
rocm=$(realpath $rocm)
build_dir=$(realpath $build_dir)
logs_dir=$(realpath $logs_dir)

if [[ "$prepare" == true ]]; then
    echo "Downloading Formocast Testing Repo into $rocm..."
    repo_url='https://github.com/hcman2/rocm-libraries.git'
    /usr/bin/git clone "$repo_url" "$rocm"
    pushd $(pwd)
    cd $rocm
    /usr/bin/git sparse-checkout set projects/hipblaslt shared/origami shared/mxdatagenerator shared/rocroller shared/formocast
    /usr/bin/git checkout formocast_pr_test_251211
    popd
    echo "Finished."
fi

echo "ROCM libraries:   $rocm"
echo "Solution pool:    $pool"
echo "Build dir:        $build_dir"

pushd $(pwd)
cd "${rocm}/projects/hipblaslt"
cmake --preset hipblaslt-clients -DBUILD_TESTING=OFF -DGPU_TARGETS="$arch" -DTENSILELITE_LOGIC_FILTER="FormoCast/$pool/**/*"  -S . -B "$build_dir"
cmake --build "$build_dir" --parallel
popd

echo "Build $build_dir done."
if [[ "$run_bench" == true ]]; then
    echo "Logs dir:     $logs_dir"
    mkdir -p "$logs_dir"
fi

echo "formocast bench command: TENSILE_PREDICTION_ALGO=1 TENSILE_PREDICTION_LIB=1 ${build_dir}/clients/hipblaslt-bench --yaml <bench.yaml> --device <gpuid>"
if [[ "$run_bench" == true ]]; then
    echo "Running formocast with $pool pool on BBSTN bench set and output to ${logs_dir}/formocast_pr_test_251211_bbstn_formocastP${pool}.log"
    TENSILE_PREDICTION_ALGO=1 TENSILE_PREDICTION_LIB=1 ${build_dir}/clients/hipblaslt-bench --yaml ${rocm}/shared/formocast/gridtest.yaml --device $device > ${logs_dir}/formocast_pr_test_251211_bbstn_formocastP${pool}.log 2>&1
    echo "Done."
fi

echo "origami bench command: ${build_dir}/clients/hipblaslt-bench --yaml <bench.yaml> --device <gpuid>"
if [[ "$run_bench" == true ]]; then
    echo "Running origami with $pool pool on BBSTN bench set and output to ${logs_dir}/formocast_pr_test_251211_bbstn_origamiP${pool}.log"
    ${build_dir}/clients/hipblaslt-bench --yaml ${rocm}/shared/formocast/gridtest.yaml --device $device > ${logs_dir}/formocast_pr_test_251211_bbstn_origamiP${pool}.log 2>&1
    echo "Done."
fi
