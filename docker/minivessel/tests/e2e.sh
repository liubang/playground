#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
COMPOSE=(docker compose -f "$ROOT/docker/minivessel/docker-compose.yml")
ctl() { local r=$1 c=$2; shift 2; "${COMPOSE[@]}" exec -T "$r" minivessel-control --address=127.0.0.1:9300 --command="$c" "$@"; }
status() { ctl "$1" status; }
log_status() { "${COMPOSE[@]}" exec -T replica-a minivessel-control --address=sharedlog:9200 --command=sharedlog-status; }
field() { local text=$1 key=$2; sed -n "s/.*${key}=\([^ ]*\).*/\1/p" <<<"$text"; }
wait_value() { local r=$1 value=$2 lrsn=$3; for _ in {1..80}; do local s; s=$(status "$r" 2>/dev/null || true); [[ $s == *"value=$value "* && $s == *"applied_lrsn=$lrsn "* ]] && return 0; sleep .25; done; status "$r"; return 1; }
promote_until() { local r=$1 deadline=$((SECONDS + 12)); while ((SECONDS < deadline)); do if ctl "$r" promote; then return 0; fi; sleep .25; done; echo "timed out promoting $r" >&2; return 1; }
assert_clean_cluster() { local log replica; log=$(log_status); [[ $(field "$log" durable_lrsn) == 0 && $(field "$log" object_count) == 0 && $(field "$log" writer_epoch) == 0 ]]; for replica in replica-a replica-b replica-c; do local s; s=$(status "$replica"); [[ $s == *'role=standby '* && $s == *'value=0 '* && $s == *'applied_lrsn=0 '* && $s == *'transitions=0'* ]]; done; }
assert_services_healthy() { local service state health; for service in mysql namenode datanode1 datanode2 datanode3 sharedlog replica-a replica-b replica-c; do state=$(${COMPOSE[@]} ps -q "$service" | xargs docker inspect -f '{{.State.Status}}'); health=$(${COMPOSE[@]} ps -q "$service" | xargs docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}'); [[ $state == running && $health == healthy ]] || { echo "$service is state=$state health=$health" >&2; return 1; }; done; }
logs_on_error() { local rc=$?; [[ -n ${stale_error:-} ]] && rm -f "$stale_error"; if ((rc)); then echo "FAILED: preserving containers and printing logs"; "${COMPOSE[@]}" ps; "${COMPOSE[@]}" logs --no-color --tail=300; fi; exit "$rc"; }
trap logs_on_error EXIT
case ${1:-all} in
  build) "${COMPOSE[@]}" build ;;
  start) "${COMPOSE[@]}" up -d --wait ;;
  down) trap - EXIT; "${COMPOSE[@]}" down ;;
  reset) trap - EXIT; "${COMPOSE[@]}" down -v --remove-orphans ;;
  test)
    echo '[0/8] require a clean, healthy nine-process cluster'
    assert_services_healthy
    if ! assert_clean_cluster; then
      echo 'test requires a clean cluster; run the all or reset command first' >&2
      exit 1
    fi

    echo '[1/8] promote A, append 10/-3, checkpoint, and converge by record replay'
    ctl replica-a promote
    l1=$(ctl replica-a add --delta=10 --request_id=a-10 | cut -d= -f2)
    l2=$(ctl replica-a add --delta=-3 --request_id=a-minus-3 | cut -d= -f2)
    cp_lrsn=$(ctl replica-a checkpoint | cut -d= -f2)
    wait_value replica-b 7 "$cp_lrsn"; wait_value replica-c 7 "$cp_lrsn"

    echo '[2/8] verify immutable object paths and static NameNode 3-replica reports'
    records=$(${COMPOSE[@]} exec -T namenode minidfs --namenode=namenode:9000 ls /minivessel/groups/counter/1/wal/records)
    checkpoints=$(${COMPOSE[@]} exec -T namenode minidfs --namenode=namenode:9000 ls /minivessel/groups/counter/1/wal/checkpoints)
    grep -q "$(printf '%020d' "$l1")" <<<"$records"
    grep -q "$(printf '%020d' "$cp_lrsn")" <<<"$checkpoints"
    paths=()
    while IFS= read -r path; do [[ -n $path ]] && paths+=("$path"); done < <(printf '%s\n%s\n' "$records" "$checkpoints" | grep -o '/minivessel/[^ ]*')
    [[ ${#paths[@]} -eq 4 ]] || { echo "expected 4 immutable objects, found ${#paths[@]}" >&2; exit 1; }
    for path in "${paths[@]}"; do
      blocks=$(${COMPOSE[@]} exec -T namenode minidfs --namenode=namenode:9000 blocks "$path")
      block_rows=$(grep -Ec 'committed[[:space:]]+3[[:space:]]+3([[:space:]]|$)' <<<"$blocks")
      [[ $block_rows -gt 0 ]] || { echo "no committed 3/3 block row for $path" >&2; exit 1; }
      data_rows=$(grep -Ec '^[[:space:]]*[0-9]+' <<<"$blocks")
      [[ $block_rows -eq $data_rows ]] || { echo "found a non-committed or non-3/3 block row for $path" >&2; exit 1; }
    done

    echo '[3/8] pause A polling without demotion, let lease expire, and promote B with a deadline'
    old_epoch=$(field "$(status replica-a)" writer_epoch)
    ctl replica-a pause-polling
    sleep 5
    promote_until replica-b
    new_epoch=$(field "$(status replica-b)" writer_epoch); ((new_epoch > old_epoch))

    echo '[4/8] prove stale A append is authority-fenced as ABORTED without catalog publication'
    before=$(log_status); before_lrsn=$(field "$before" durable_lrsn); before_count=$(field "$before" object_count)
    stale_error=$(mktemp)
    if ctl replica-a add --delta=100 --request_id=stale-a 2>"$stale_error"; then echo 'stale A unexpectedly accepted write'; exit 1; fi
    grep -q '^ABORTED:' "$stale_error"; rm -f "$stale_error"; stale_error=
    after=$(log_status)
    [[ $(field "$after" durable_lrsn) == "$before_lrsn" && $(field "$after" object_count) == "$before_count" ]]
    a_status=$(status replica-a); [[ $a_status == *'role=standby '* && $a_status == *'transitions=3'* ]]
    ctl replica-a resume-polling

    echo '[5/8] B appends 5, then C is stopped while B appends 8'
    l3=$(ctl replica-b add --delta=5 --request_id=b-5 | cut -d= -f2)
    wait_value replica-a 12 "$l3"
    c_pid_before=$(${COMPOSE[@]} ps -q replica-c | xargs docker inspect -f '{{.State.Pid}}')
    "${COMPOSE[@]}" stop replica-c
    c_state=$(${COMPOSE[@]} ps -a -q replica-c | xargs docker inspect -f '{{.State.Status}}'); [[ $c_state == exited ]]
    l4=$(ctl replica-b add --delta=8 --request_id=b-8 | cut -d= -f2)

    echo '[6/8] restart C as a new process and replay records through the authority'
    "${COMPOSE[@]}" start --wait replica-c
    c_pid_after=$(${COMPOSE[@]} ps -q replica-c | xargs docker inspect -f '{{.State.Pid}}')
    [[ $c_pid_after != "$c_pid_before" ]]
    wait_value replica-c 20 "$l4"

    echo '[7/8] demote B, promote recovered C, append 2, and converge to 22'
    ctl replica-b demote
    promote_until replica-c
    l5=$(ctl replica-c add --delta=2 --request_id=c-2 | cut -d= -f2)
    wait_value replica-a 22 "$l5"; wait_value replica-b 22 "$l5"; wait_value replica-c 22 "$l5"

    echo '[8/8] assert exact lifecycle transitions and final resource ownership'
    a=$(status replica-a); b=$(status replica-b); c=$(status replica-c)
    echo "$a"; echo "$b"; echo "$c"
    [[ $a == *'role=standby '* && $a == *'value=22 '* && $a == *'resources=inactive '* && $a == *'transitions=3'* ]]
    [[ $b == *'role=standby '* && $b == *'value=22 '* && $b == *'resources=inactive '* && $b == *'transitions=3'* ]]
    [[ $c == *'role=primary '* && $c == *'value=22 '* && $c == *'resources=active '* && $c == *'transitions=2'* ]]
    final_log=$(log_status)
    [[ $(field "$final_log" durable_lrsn) == "$l5" && $(field "$final_log" object_count) == "$l5" ]]
    assert_services_healthy
    echo 'PASS: in-process authority fencing, MiniDFS immutable-object replay, replica process restart, and convergence verified'
    ;;
  all) "${COMPOSE[@]}" down -v --remove-orphans || true; "${COMPOSE[@]}" build; "${COMPOSE[@]}" up -d --wait; "$0" test; trap - EXIT; "${COMPOSE[@]}" down ;;
  *) echo "usage: $0 {all|build|start|test|down|reset}" >&2; exit 2 ;;
esac
