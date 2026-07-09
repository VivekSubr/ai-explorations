# demo-nfs-wsl.sh

`demo-nfs-wsl.sh` demonstrates how the project can use NFS as the backing storage layer for object bytes. It runs a local NFS export inside WSL, mounts that export back from `127.0.0.1`, writes a sample object-store-like file through the mount, verifies the exported directory sees the same bytes, and then cleans up.

## What It Demonstrates

1. An exported NFS directory can act as the shared object-store path.
2. A mounted NFS client path sees the same file tree as the export.
3. Application code can write normal files and folders while the backing store is networked.
4. The demo layout mirrors the project idea of storing user file bytes under paths like `users/alice/photos/...`.

## Requirements

The script is designed for Ubuntu or Debian running in WSL2.

Required capabilities:

1. `sudo` access.
2. `apt-get` if NFS packages are not already installed.
3. Kernel NFS server support through `nfs-kernel-server`.
4. Ability to write `/etc/exports.d/wsl-nfs-demo.exports`.
5. Ability to mount NFS filesystems.

The script installs these packages if they are missing and `--skip-install` is not used:

```bash
nfs-kernel-server nfs-common
```

## Quick Start

From the repo root:

```bash
./scripts/demo-nfs-wsl.sh
```

The script will:

1. Request sudo.
2. Install missing NFS packages if needed.
3. Create `/tmp/wsl-nfs-demo/export`.
4. Create `/tmp/wsl-nfs-demo/mount`.
5. Write `/etc/exports.d/wsl-nfs-demo.exports`.
6. Start or restart local NFS services.
7. Mount `127.0.0.1:/tmp/wsl-nfs-demo/export` at `/tmp/wsl-nfs-demo/mount`.
8. Write `users/alice/photos/hello-from-nfs.txt` through the mounted path.
9. Verify the same file exists under the exported path.
10. Unmount and remove the temporary demo export by default.

## Keep Resources For Inspection

Use `--keep` to leave the export and mount in place after the demo:

```bash
./scripts/demo-nfs-wsl.sh --keep
```

After inspecting the files, clean up manually:

```bash
sudo umount /tmp/wsl-nfs-demo/mount
sudo rm -f /etc/exports.d/wsl-nfs-demo.exports
sudo exportfs -ra
rm -rf /tmp/wsl-nfs-demo
```

## Options

| Option | Description |
| --- | --- |
| `--keep` | Leave the export file, export directory, and mount in place. |
| `--skip-install` | Do not install missing NFS packages. Useful when package installation is managed separately. |
| `--export-dir PATH` | Use a custom directory as the NFS export. |
| `--mount-dir PATH` | Use a custom directory as the NFS mount point. |
| `-h`, `--help` | Print script help. |

## Environment Overrides

These environment variables match the path options:

```bash
EXPORT_DIR=/tmp/my-export MOUNT_DIR=/tmp/my-mount ./scripts/demo-nfs-wsl.sh
```

| Variable | Description |
| --- | --- |
| `EXPORT_DIR` | Directory exported by the NFS server. |
| `MOUNT_DIR` | Directory used as the local NFS mount point. |

Do not use paths with spaces. The script rejects export paths containing whitespace because `/etc/exports` parsing is easy to get wrong with those paths.

## Expected Output

A successful run prints the major steps and then shows both views of the same file:

```text
==> Writing a file through the NFS mount

==> Verifying the exported directory sees the same file

Mounted view:
/tmp/wsl-nfs-demo/mount/users/alice/photos/hello-from-nfs.txt

Exported view:
/tmp/wsl-nfs-demo/export/users/alice/photos/hello-from-nfs.txt

Demo file contents:
  hello from WSL NFS
  written_at=...
  mount_dir=/tmp/wsl-nfs-demo/mount
  export_dir=/tmp/wsl-nfs-demo/export

==> NFS demo completed successfully
```

## Files And System State

By default the script removes its temporary state before exit.

Temporary paths:

1. Export directory: `/tmp/wsl-nfs-demo/export`
2. Mount directory: `/tmp/wsl-nfs-demo/mount`
3. Export config: `/etc/exports.d/wsl-nfs-demo.exports`

System actions:

1. May run `apt-get update`.
2. May install `nfs-kernel-server` and `nfs-common`.
3. Starts `rpcbind` when available.
4. Starts or restarts `nfs-kernel-server` or `nfs-server` when available.
5. Runs `exportfs -ra` to refresh exports.
6. Mounts an NFS filesystem from `127.0.0.1`.

## Troubleshooting

### WSL Cannot Run NFS Server

If mounting fails, confirm the distro is WSL2:

```powershell
wsl.exe --list --verbose
```

WSL1 cannot run this demo because it does not provide the needed Linux kernel features.

### systemd Is Disabled

Some WSL distros need systemd enabled before NFS services start reliably. Add this to `/etc/wsl.conf`:

```ini
[boot]
systemd=true
```

Then restart WSL from Windows:

```powershell
wsl.exe --shutdown
```

Open the distro again and rerun the script.

### Packages Are Managed Separately

If you do not want the script to install packages, install them first:

```bash
sudo apt-get update
sudo apt-get install -y nfs-kernel-server nfs-common
./scripts/demo-nfs-wsl.sh --skip-install
```

### Mount Is Left Behind

If the script exits unexpectedly or was run with `--keep`, unmount manually:

```bash
sudo umount /tmp/wsl-nfs-demo/mount
```

If unmount says the target is busy, close terminals or processes using that directory and retry.

## Safety Notes

This is a local development demo, not a production NFS export policy. It exports only to `127.0.0.1` and `localhost`, uses a temporary path by default, and writes a dedicated file under `/etc/exports.d` so cleanup is straightforward.

For production storage design, tighten export permissions, avoid `no_root_squash` unless there is a specific operational reason, isolate tenants above the filesystem layer, and pair file bytes with database metadata as described in the broader project docs.