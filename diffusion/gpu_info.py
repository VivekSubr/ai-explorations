import argparse
import importlib.metadata as importlib_metadata
import importlib.util
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

from compute import (
    describe_onnx_environment,
    get_directml_device_name,
    get_onnx_providers,
    has_local_onnx_gpu_provider,
)


PACKAGE_CHECKS = [
    ("numpy", "numpy", "numpy"),
    ("onnxruntime", "onnxruntime", "onnxruntime"),
    ("onnxruntime-directml", "onnxruntime-directml", None),
    ("onnxruntime-gpu", "onnxruntime-gpu", None),
    ("optimum", "optimum", "optimum"),
    ("diffusers", "diffusers", "diffusers"),
    ("transformers", "transformers", "transformers"),
    ("pillow", "Pillow", "PIL"),
    ("torch", "torch", "torch"),
    ("torch-directml", "torch-directml", "torch_directml"),
]


def section(title):
    print()
    print(title)
    print("-" * len(title))


def print_kv(key, value):
    print(f"{key:28} {value}")


def dist_version(distribution):
    try:
        return importlib_metadata.version(distribution)
    except importlib_metadata.PackageNotFoundError:
        return "not installed"


def import_available(import_name):
    try:
        return importlib.util.find_spec(import_name) is not None
    except (ImportError, ValueError):
        return False


def is_wsl():
    if os.environ.get("WSL_DISTRO_NAME"):
        return True
    try:
        release = Path("/proc/sys/kernel/osrelease").read_text(encoding="utf-8").lower()
    except OSError:
        release = platform.release().lower()
    return "microsoft" in release or "wsl" in release


def trim_output(output, limit=4000):
    output = output.strip()
    if len(output) <= limit:
        return output
    return output[:limit] + "\n... output trimmed ..."


def run_command(label, command, timeout=8):
    executable = command[0]
    if shutil.which(executable) is None:
        print_kv(label, f"{executable} not found")
        return

    try:
        result = subprocess.run(
            command,
            capture_output=True,
            check=False,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        print_kv(label, f"timed out after {timeout}s")
        return
    except OSError as exc:
        print_kv(label, exc)
        return

    output = trim_output(result.stdout or result.stderr)
    if output:
        print(f"{label}:")
        for line in output.splitlines():
            print(f"  {line}")
    else:
        print_kv(label, f"no output, exit code {result.returncode}")


def print_python_info():
    section("Python")
    print_kv("executable", sys.executable)
    print_kv("version", sys.version.replace("\n", " "))
    print_kv("platform", platform.platform())
    print_kv("machine", platform.machine())
    print_kv("prefix", sys.prefix)
    print_kv("base_prefix", sys.base_prefix)
    print_kv("virtualenv", sys.prefix != sys.base_prefix)
    print_kv("wsl", is_wsl())
    print_kv("wsl_distro", os.environ.get("WSL_DISTRO_NAME", ""))


def print_environment_info():
    section("Relevant Environment")
    for name in [
        "CUDA_VISIBLE_DEVICES",
        "HIP_VISIBLE_DEVICES",
        "ROCR_VISIBLE_DEVICES",
        "HSA_OVERRIDE_GFX_VERSION",
        "ROCM_PATH",
        "HIP_PATH",
        "LD_LIBRARY_PATH",
        "PATH",
    ]:
        value = os.environ.get(name, "")
        if name == "PATH" and value:
            value = value[:300] + ("..." if len(value) > 300 else "")
        print_kv(name, value)


def print_package_info():
    section("Python Packages")
    print(f"{'package':24} {'version':18} import")
    for label, distribution, import_name in PACKAGE_CHECKS:
        version = dist_version(distribution)
        if import_name:
            import_status = f"{import_name}={import_available(import_name)}"
        else:
            import_status = "shares onnxruntime import"
        print(f"{label:24} {version:18} {import_status}")


def print_onnxruntime_info():
    section("ONNX Runtime")
    try:
        import onnxruntime as ort
    except Exception as exc:
        print_kv("status", f"not importable: {type(exc).__name__}: {exc}")
        return []

    providers = get_onnx_providers()
    print_kv("version", getattr(ort, "__version__", "unknown"))
    print_kv("device", getattr(ort, "get_device", lambda: "unknown")())
    print_kv("providers", providers)

    if "DmlExecutionProvider" in providers:
        print_kv("directml", "available; this is the usual AMD GPU path on Windows")
    else:
        print_kv("directml", "not available")

    if "ROCMExecutionProvider" in providers:
        print_kv("rocm", "available; this is the usual AMD GPU path on Linux")
    else:
        print_kv("rocm", "not available")

    if "AzureExecutionProvider" in providers:
        print_kv("azure_provider", "available, but this is not the laptop GPU")

    return providers


def print_torch_info():
    section("PyTorch")
    try:
        import torch
    except Exception as exc:
        print_kv("torch", f"not importable: {type(exc).__name__}: {exc}")
        return

    print_kv("version", getattr(torch, "__version__", "unknown"))
    print_kv("cuda_version", getattr(torch.version, "cuda", None))
    print_kv("hip_version", getattr(torch.version, "hip", None))
    print_kv("cuda_available", torch.cuda.is_available())
    print_kv("cuda_device_count", torch.cuda.device_count())
    for index in range(torch.cuda.device_count()):
        print_kv(f"cuda_device_{index}", torch.cuda.get_device_name(index))

    try:
        import torch_directml
    except Exception as exc:
        print_kv("torch_directml", f"not importable: {type(exc).__name__}: {exc}")
        return

    is_available = getattr(torch_directml, "is_available", lambda: "unknown")()
    print_kv("torch_directml", f"importable, available={is_available}")
    device_name = get_directml_device_name(torch_directml)
    if device_name:
        print_kv("torch_directml_device_name", device_name)
    try:
        print_kv("torch_directml_device", torch_directml.device())
    except Exception as exc:
        print_kv("torch_directml_device", f"error: {type(exc).__name__}: {exc}")


def print_hardware_info():
    section("Hardware Visibility")
    if sys.platform.startswith("linux") and shutil.which("bash"):
        run_command(
            "linux_display_adapters",
            [
                "bash",
                "-lc",
                "if command -v lspci >/dev/null; then lspci | grep -Ei 'vga|3d|display|amd|ati|radeon' || echo 'no display adapters reported by lspci'; else echo 'lspci not found'; fi",
            ],
        )
        run_command(
            "rocminfo_summary",
            [
                "bash",
                "-lc",
                "command -v rocminfo >/dev/null && rocminfo 2>/dev/null | grep -Ei 'Name:|Marketing Name|gfx|Agent' | head -n 60 || echo 'rocminfo not found'",
            ],
        )
        run_command(
            "rocm_smi",
            ["bash", "-lc", "command -v rocm-smi >/dev/null && rocm-smi || echo 'rocm-smi not found'"],
        )
        run_command(
            "opencl_summary",
            [
                "bash",
                "-lc",
                "command -v clinfo >/dev/null && clinfo 2>/dev/null | grep -Ei 'Platform Name|Device Name|Device Type' | head -n 60 || echo 'clinfo not found'",
            ],
        )

    powershell = shutil.which("powershell.exe") or shutil.which("powershell")
    if powershell:
        run_command(
            "windows_video_controllers",
            [
                powershell,
                "-NoProfile",
                "-Command",
                "Get-CimInstance Win32_VideoController | Select-Object Name,AdapterCompatibility,DriverVersion | Format-List",
            ],
        )


def print_model_info(ort_dir):
    section("Model Directory")
    path = Path(ort_dir).expanduser()
    print_kv("ort_dir", str(path))
    print_kv("exists", path.exists())
    print_kv("is_dir", path.is_dir())
    print_kv("model_index.json", (path / "model_index.json").exists())
    if path.exists() and path.is_dir() and not (path / "model_index.json").exists():
        matches = list(path.rglob("model_index.json"))[:5]
        print_kv("nested_model_indexes", [str(match) for match in matches])


def print_interpretation(providers):
    section("Interpretation")
    for message in describe_onnx_environment(providers, is_wsl_environment=is_wsl()):
        print(message)


def main():
    parser = argparse.ArgumentParser(description="Report Python GPU backend information for this repo")
    parser.add_argument(
        "--ort-dir",
        default=os.environ.get("ORT_DIR", "/mnt/c/models/sd15-ort-text2img"),
        help="ORT diffusion model directory to check",
    )
    parser.add_argument(
        "--require-gpu",
        action="store_true",
        help="Exit with an error if ONNX Runtime does not expose a local GPU provider",
    )
    args = parser.parse_args()

    print_python_info()
    print_environment_info()
    print_package_info()
    providers = print_onnxruntime_info()
    print_torch_info()
    print_hardware_info()
    print_model_info(args.ort_dir)
    print_interpretation(providers)

    if args.require_gpu:
        section("GPU Test")
        if has_local_onnx_gpu_provider(providers):
            print("PASS: ONNX Runtime exposes a local GPU provider from this Python.")
            return 0
        print("FAIL: ONNX Runtime does not expose a local GPU provider from this Python.")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())