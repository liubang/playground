#!/usr/bin/env bash
# Shared helpers for module bootstrap scripts. Sourcing this file has no side effects.

log_ok()   { printf '\033[32m[OK]\033[0m    %s\n' "$1"; }
log_skip() { printf '\033[33m[SKIP]\033[0m  %s\n' "$1"; }
log_run()  { printf '\033[36m[...]\033[0m  %s\n' "$1"; }
log_warn() { printf '\033[33m[WARN]\033[0m  %s\n' "$1"; }
log_fail() { printf '\033[31m[FAIL]\033[0m  %s\n' "$1" >&2; }
die()      { log_fail "$1"; exit 1; }

bootstrap_init() {
    local module_dir="$1"
    shift
    START_SERVICES=true
    RESET=false
    while (($#)); do
        case "$1" in
            --no-start) START_SERVICES=false ;;
            --reset) RESET=true ;;
            *) die "未知参数: $1" ;;
        esac
        shift
    done
    cd "$module_dir"
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "未找到命令: $1"
}

bootstrap_check_dependencies() {
    echo "==> 检查依赖..."
    require_cmd docker
    local cmd
    for cmd in "$@"; do
        require_cmd "$cmd"
    done
    docker compose version >/dev/null 2>&1 || die "docker compose 不可用"
    docker info >/dev/null 2>&1 || die "Docker daemon 不可用"
    log_ok "依赖检查通过"
}

bootstrap_handle_reset() {
    [ "$RESET" = true ] || return 0
    echo "==> --reset: 清除所有 volume 和配置..."
    docker compose down -v 2>/dev/null || true
    rm -f .env
    log_ok "已清除所有 volume 和配置"
}

gen_secret() {
    local length="${1:-20}" secret
    [[ "$length" =~ ^[1-9][0-9]*$ ]] || die "无效的密钥长度: $length"
    secret="$(docker run --rm alpine:3 sh -c "tr -dc A-Za-z0-9 </dev/urandom | head -c '$length'")"
    [ "${#secret}" -eq "$length" ] || die "随机密钥生成失败"
    printf '%s' "$secret"
}

prompt_value() {
    local var_name="$1" prompt_text="$2" default_val="$3" input
    if [ -t 0 ]; then
        read -r -p "${prompt_text}（回车使用默认值 [${default_val}]）: " input
        printf -v "$var_name" '%s' "${input:-$default_val}"
    else
        printf -v "$var_name" '%s' "$default_val"
    fi
}

prompt_secret() {
    local var_name="$1" prompt_text="$2" default_len="${3:-20}" default_val input value
    default_val="$(gen_secret "$default_len")"
    if [ -t 0 ]; then
        read -r -s -p "${prompt_text}（回车使用随机生成值）: " input
        printf '\n'
        value="${input:-$default_val}"
    else
        value="$default_val"
    fi
    [[ "$value" =~ ^[A-Za-z0-9._-]+$ ]] || die "${prompt_text} 仅允许字母、数字、点、下划线和连字符"
    printf -v "$var_name" '%s' "$value"
}

validate_env_value() {
    local key="$1" value="$2"
    [[ "$key" =~ ^[A-Z_][A-Z0-9_]*$ ]] || die "无效的环境变量名: $key"
    [[ "$value" != *$'\n'* && "$value" != *$'\r'* ]] || die "$key 不能包含换行符"
    [[ "$value" != *'$'* ]] || die "$key 不能包含美元符号"
}

write_env_file() {
    local output="$1" key value
    shift
    [ $(( $# % 2 )) -eq 0 ] || die "write_env_file 参数必须为 key/value 对"
    : >"$output"
    while (($#)); do
        key="$1"
        value="$2"
        validate_env_value "$key" "$value"
        printf '%s=%s\n' "$key" "$value" >>"$output"
        shift 2
    done
    chmod 600 "$output"
}

select_bind_mode() {
    local safe_description="$1" all_description="$2" confirm_message="$3"
    local input confirm
    PUBLIC_BIND_ADDR="127.0.0.1"
    INTERNAL_BIND_ADDR="127.0.0.1"
    BIND_MODE="local"
    [ -t 0 ] || return 0
    printf '\n网络暴露模式：\n'
    printf '  1) 仅本机（默认，推荐）\n'
    printf '  2) 安全公开：%s\n' "$safe_description"
    printf '  3) 全部公开：%s\n' "$all_description"
    read -r -p "请选择 [1]: " input
    case "$input" in
        2)
            PUBLIC_BIND_ADDR="0.0.0.0"
            BIND_MODE="safe-public"
            ;;
        3)
            read -r -p "${confirm_message}，输入 YES 继续: " confirm
            if [ "$confirm" = "YES" ]; then
                PUBLIC_BIND_ADDR="0.0.0.0"
                INTERNAL_BIND_ADDR="0.0.0.0"
                BIND_MODE="all-public"
            else
                log_warn "未确认，使用仅本机模式"
            fi
            ;;
    esac
}

select_public_bind() {
    local description="$1" input
    PUBLIC_BIND_ADDR="127.0.0.1"
    BIND_MODE="local"
    [ -t 0 ] || return 0
    printf '\n网络暴露模式：\n'
    printf '  1) 仅本机（默认，推荐）\n'
    printf '  2) 安全公开：%s\n' "$description"
    read -r -p "请选择 [1]: " input
    if [ "$input" = "2" ]; then
        PUBLIC_BIND_ADDR="0.0.0.0"
        BIND_MODE="safe-public"
    fi
}

wait_until() {
    local description="$1" attempts="$2" interval="$3"
    shift 3
    local i
    echo "==> 等待 ${description}..."
    for ((i = 1; i <= attempts; i++)); do
        if "$@"; then
            log_ok "${description}"
            return 0
        fi
        sleep "$interval"
    done
    log_fail "${description} 在预期时间内未就绪"
    return 1
}

compose_start() {
    echo "==> 启动服务..."
    docker compose up -d
}

show_compose_status() {
    printf '\n==> 服务状态:\n'
    docker compose ps
}

print_no_start() {
    printf '\n==> 环境准备完成（--no-start 模式，未启动服务）\n'
    echo "手动启动: docker compose up -d"
}
