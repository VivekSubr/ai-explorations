#!/usr/bin/env bash
set -Eeuo pipefail

PATH="/usr/sbin:/sbin:${PATH}"

SCRIPT_NAME="$(basename "$0")"
EXPORT_DIR="${EXPORT_DIR:-/tmp/wsl-nfs-demo/export}"
MOUNT_DIR="${MOUNT_DIR:-/tmp/wsl-nfs-demo/mount}"
EXPORT_FILE="/etc/exports.d/wsl-nfs-demo.exports"
KEEP=0
SKIP_INSTALL=0
MOUNTED=0

usage() {
  cat <<EOF
Usage: ${SCRIPT_NAME} [options]

Demo an NFS export and mount inside WSL. The script creates a temporary export,
mounts it back from localhost, writes sample object-store-like files through the
NFS mount, verifies the files on the exported directory, then cleans up.

Options:
  --keep              Leave the export file, export directory, and mount in place.
  --skip-install      Do not install missing NFS packages with apt-get.
  --export-dir PATH   Directory to export. Default: ${EXPORT_DIR}
  --mount-dir PATH    Directory to mount. Default: ${MOUNT_DIR}
  -h, --help          Show this help.

Environment overrides:
  EXPORT_DIR          Same as --export-dir.
  MOUNT_DIR           Same as --mount-dir.

Notes:
  - Designed for Ubuntu/Debian WSL2.
  - Requires sudo for package install, /etc/exports.d, NFS service start, and mount.
  - WSL1 and some locked-down WSL kernels cannot run nfsd.
EOF
}

log() {
  printf '\n==> %s\n' "$*"
}

warn() {
  printf 'WARN: %s\n' "$*" >&2
}

die() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

as_root() {
  "${SUDO[@]}" "$@"
}

have() {
  command -v "$1" >/dev/null 2>&1
}

parse_args() {
  while (($#)); do
    case "$1" in
      --keep)
        KEEP=1
        ;;
      --skip-install)
        SKIP_INSTALL=1
        ;;
      --export-dir)
        [[ $# -ge 2 ]] || die "--export-dir requires a path"
        EXPORT_DIR="$2"
        shift
        ;;
      --mount-dir)
        [[ $# -ge 2 ]] || die "--mount-dir requires a path"
        MOUNT_DIR="$2"
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        die "unknown option: $1"
        ;;
    esac
    shift
  done
}

cleanup() {
  local status=$?

  if ((KEEP)); then
    log "Keeping demo resources"
    printf 'Export file: %s\n' "$EXPORT_FILE"
    printf 'Export dir:  %s\n' "$EXPORT_DIR"
    printf 'Mount dir:   %s\n' "$MOUNT_DIR"
    return "$status"
  fi

  if ((MOUNTED)) || mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
    log "Unmounting demo mount"
    as_root umount "$MOUNT_DIR" >/dev/null 2>&1 || warn "could not unmount ${MOUNT_DIR}"
  fi

  if [[ -f "$EXPORT_FILE" ]]; then
    log "Removing demo export"
    as_root rm -f "$EXPORT_FILE"
    if have exportfs; then
      as_root exportfs -ra >/dev/null 2>&1 || warn "could not refresh exports after cleanup"
    fi
  fi

  if [[ "$EXPORT_DIR" == /tmp/wsl-nfs-demo/* && "$MOUNT_DIR" == /tmp/wsl-nfs-demo/* ]]; then
    rm -rf /tmp/wsl-nfs-demo
  fi

  return "$status"
}

check_environment() {
  if ! grep -qi microsoft /proc/version 2>/dev/null && [[ -z "${WSL_DISTRO_NAME:-}" ]]; then
    warn "this does not look like WSL; continuing because standard Linux can also run the demo"
  fi

  case "$EXPORT_DIR" in
    *[[:space:]]*) die "export path cannot contain whitespace: ${EXPORT_DIR}" ;;
  esac

  if [[ "$EXPORT_DIR" == "$MOUNT_DIR" ]]; then
    die "export and mount directories must be different"
  fi

  if ! have sudo && ((EUID != 0)); then
    die "sudo is required when not running as root"
  fi
}

setup_sudo() {
  SUDO=()
  if ((EUID != 0)); then
    SUDO=(sudo)
    log "Requesting sudo access"
    sudo -v
  fi
}

install_packages() {
  local missing=()

  for command_name in exportfs mount.nfs rpc.nfsd; do
    if ! have "$command_name"; then
      missing+=("$command_name")
    fi
  done

  if ((${#missing[@]} == 0)); then
    return
  fi

  if ((SKIP_INSTALL)); then
    die "missing NFS commands: ${missing[*]}. Re-run without --skip-install or install nfs-kernel-server nfs-common."
  fi

  have apt-get || die "missing NFS commands (${missing[*]}) and apt-get is not available"

  log "Installing NFS packages"
  as_root apt-get update
  as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y nfs-kernel-server nfs-common
}

prepare_directories() {
  local owner
  owner="${SUDO_USER:-${USER:-root}}"

  log "Preparing demo directories"
  mkdir -p "$EXPORT_DIR" "$MOUNT_DIR"

  if id "$owner" >/dev/null 2>&1; then
    as_root chown "$(id -u "$owner"):$(id -g "$owner")" "$EXPORT_DIR" "$MOUNT_DIR"
  fi

  if mountpoint -q "$MOUNT_DIR"; then
    die "mount directory is already mounted: ${MOUNT_DIR}"
  fi
}

write_export() {
  local export_tmp
  export_tmp="$(mktemp)"

  log "Writing NFS export"
  cat >"$export_tmp" <<EOF
# Created by ${SCRIPT_NAME}; safe to remove after the demo.
${EXPORT_DIR} 127.0.0.1(rw,sync,no_subtree_check,no_root_squash,insecure) localhost(rw,sync,no_subtree_check,no_root_squash,insecure)
EOF

  as_root mkdir -p /etc/exports.d
  as_root cp "$export_tmp" "$EXPORT_FILE"
  rm -f "$export_tmp"
  as_root exportfs -ra
}

start_nfs_services() {
  log "Starting NFS services"

  if [[ -d /proc/fs/nfsd ]] && ! mountpoint -q /proc/fs/nfsd; then
    as_root mount -t nfsd nfsd /proc/fs/nfsd >/dev/null 2>&1 || true
  fi

  if have systemctl && systemctl list-unit-files >/dev/null 2>&1; then
    as_root systemctl start rpcbind >/dev/null 2>&1 || true
    as_root systemctl restart nfs-kernel-server >/dev/null 2>&1 || \
      as_root systemctl restart nfs-server >/dev/null 2>&1 || true
  fi

  if have service; then
    as_root service rpcbind start >/dev/null 2>&1 || true
    as_root service nfs-kernel-server restart >/dev/null 2>&1 || true
  fi

  as_root exportfs -ra
}

mount_export() {
  local source
  source="127.0.0.1:${EXPORT_DIR}"

  log "Mounting ${source} at ${MOUNT_DIR}"
  if as_root mount -t nfs -o vers=4,proto=tcp "$source" "$MOUNT_DIR"; then
    MOUNTED=1
    printf 'Mounted with NFSv4.\n'
    return
  fi

  warn "NFSv4 mount failed; trying NFSv3 with nolock"
  if as_root mount -t nfs -o vers=3,proto=tcp,nolock "$source" "$MOUNT_DIR"; then
    MOUNTED=1
    printf 'Mounted with NFSv3.\n'
    return
  fi

  cat >&2 <<EOF
ERROR: Could not mount the local NFS export.

Common WSL causes:
  - The distro is WSL1 instead of WSL2.
  - The WSL kernel does not expose /proc/fs/nfsd.
  - systemd/services are disabled and nfs-kernel-server did not start.

Try enabling systemd in /etc/wsl.conf, restarting WSL, then running this script again:

  [boot]
  systemd=true

EOF
  exit 1
}

run_demo() {
  local mounted_file
  local source_file
  mounted_file="${MOUNT_DIR}/users/alice/photos/hello-from-nfs.txt"
  source_file="${EXPORT_DIR}/users/alice/photos/hello-from-nfs.txt"

  log "Writing a file through the NFS mount"
  mkdir -p "$(dirname "$mounted_file")"
  cat >"$mounted_file" <<EOF
hello from WSL NFS
written_at=$(date -Iseconds)
mount_dir=${MOUNT_DIR}
export_dir=${EXPORT_DIR}
EOF

  log "Verifying the exported directory sees the same file"
  [[ -f "$source_file" ]] || die "expected file not found in export: ${source_file}"
  cmp "$mounted_file" "$source_file"

  printf '\nMounted view:\n'
  find "$MOUNT_DIR" -maxdepth 5 -type f -print

  printf '\nExported view:\n'
  find "$EXPORT_DIR" -maxdepth 5 -type f -print

  printf '\nDemo file contents:\n'
  sed 's/^/  /' "$source_file"
}

main() {
  parse_args "$@"
  check_environment
  setup_sudo
  trap cleanup EXIT
  install_packages
  prepare_directories
  write_export
  start_nfs_services
  mount_export
  run_demo
  log "NFS demo completed successfully"
}

main "$@"