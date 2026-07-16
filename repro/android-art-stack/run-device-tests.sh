#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPRO_JAR=${SCRIPT_DIR}/out/repro.jar
REPRO_LIBRARY=${SCRIPT_DIR}/out/lib/libqbdirepro.so
DEVICE_DIR=/data/local/tmp/qbdi-art-stack
APP_PROCESS="CLASSPATH=${DEVICE_DIR}/repro.jar app_process64 ${DEVICE_DIR} Repro"

[ -f "${REPRO_JAR}" ]
[ -f "${REPRO_LIBRARY}" ]

adb shell "rm -rf ${DEVICE_DIR} && mkdir -p ${DEVICE_DIR}"
adb push "${REPRO_JAR}" "${DEVICE_DIR}/repro.jar" >/dev/null
adb push "${REPRO_LIBRARY}" "${DEVICE_DIR}/libqbdirepro.so" >/dev/null
adb shell "chmod 644 ${DEVICE_DIR}/repro.jar ${DEVICE_DIR}/libqbdirepro.so"

run_success() {
  mode=$1
  expected_result=$2
  output=$(adb shell "${APP_PROCESS} ${mode}" 2>&1)
  printf '%s\n' "${output}"
  printf '%s\n' "${output}" |
    grep -Fq "[java] native result=${expected_result}"

  if [ "$#" -ge 3 ]; then
    printf '%s\n' "${output}" | grep -Fq "$3"
  fi
}

run_success allocator-stress 0
run_success allocator-threaded 0
run_success switch-stress 0
run_success call-findclass 1
run_success call-getmethodid 1
run_success call-pending 1 'pending after CallStaticIntMethod=0'
run_success switch-callstatic 42

adb logcat -c
set +e
low_output=$(adb shell "${APP_PROCESS} lowcall-pending" 2>&1)
low_status=$?
set -e
printf '%s\n' "${low_output}"

if [ "${low_status}" -eq 0 ]; then
  printf '%s\n' 'lowcall-pending unexpectedly succeeded' >&2
  exit 1
fi

printf '%s\n' "${low_output}" | grep -Fq 'pending after CallStaticIntMethod=1'
adb logcat -d -t 500 | grep -Fq 'StackOverflowError: stack size'

printf '%s\n' "expected low-stack failure status=${low_status}"
