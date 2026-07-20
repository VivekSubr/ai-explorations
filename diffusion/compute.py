from __future__ import annotations

import os


LOCAL_ONNX_GPU_PROVIDERS = (
    "DmlExecutionProvider",
    "ROCMExecutionProvider",
    "MIGraphXExecutionProvider",
    "CUDAExecutionProvider",
)

CPU_ONNX_PROVIDER = "CPUExecutionProvider"
NON_LOCAL_ONNX_PROVIDERS = (CPU_ONNX_PROVIDER, "AzureExecutionProvider")

DEFAULT_IMAGE_MODEL = "imagepipeline/EpiCRealism-Natural-Sin"
DEFAULT_IMAGE_VARIANT = "fp16"
DEFAULT_TORCH_DTYPE = "auto"
DEFAULT_VIDEO_MODEL = "damo-vilab/text-to-video-ms-1.7b"
DEFAULT_IMAGE_SYSTEM_PROMPT = (
    "photorealistic RAW photo, natural skin texture, realistic eyes, realistic human anatomy, "
    "35mm DSLR photography, natural lighting, sharp focus, high detail, lifelike colors"
)
DEFAULT_IMAGE_NEGATIVE_PROMPT = (
    "cartoon, anime, illustration, painting, drawing, cgi, 3d render, doll, plastic skin, "
    "waxy skin, airbrushed, overprocessed, deformed, distorted, bad anatomy, extra fingers, "
    "missing fingers, blurry, low resolution, low quality"
)


def get_onnx_providers():
    import onnxruntime as ort

    return ort.get_available_providers()


def is_local_onnx_gpu_provider(provider):
    return provider in LOCAL_ONNX_GPU_PROVIDERS


def has_local_onnx_gpu_provider(providers):
    return any(is_local_onnx_gpu_provider(provider) for provider in providers)


def select_onnx_provider(requested_provider=None, allow_cpu=False, providers=None):
    providers = list(get_onnx_providers() if providers is None else providers)
    requested_provider = requested_provider.strip() if requested_provider else None

    if requested_provider:
        if requested_provider not in providers:
            raise RuntimeError(f"Requested provider {requested_provider!r} is not available. Available providers: {providers}")
        if requested_provider in NON_LOCAL_ONNX_PROVIDERS and not allow_cpu:
            raise RuntimeError(f"Requested provider {requested_provider!r} is not a local GPU provider.")
        return requested_provider

    for provider in LOCAL_ONNX_GPU_PROVIDERS:
        if provider in providers:
            return provider

    if allow_cpu and CPU_ONNX_PROVIDER in providers:
        return CPU_ONNX_PROVIDER

    raise RuntimeError(
        "No local GPU ONNX Runtime provider is available from this Python. "
        f"Available providers: {providers}. "
        "For this AMD laptop, use native Windows Python with onnxruntime-directml so DmlExecutionProvider is listed, "
        "or install a supported Linux ROCm ONNX Runtime stack so ROCMExecutionProvider is listed. "
        "AzureExecutionProvider is not the laptop GPU. Refusing CPU fallback; pass --allow_cpu only if you intentionally want CPU."
    )


def get_torch_directml_module():
    try:
        import torch_directml
    except ModuleNotFoundError as exc:
        if exc.name != "torch_directml":
            raise
        return None

    return torch_directml


def get_directml_device_name(torch_directml=None, device_id=0):
    torch_directml = get_torch_directml_module() if torch_directml is None else torch_directml
    if torch_directml is None:
        return ""

    name_fn = getattr(torch_directml, "device_name", None)
    return str(name_fn(device_id)).rstrip("\x00") if callable(name_fn) else ""


def configure_directml(torch_directml):
    disable_value = os.environ.get("TORCH_DIRECTML_DISABLE_TILED_RESOURCES", "1").strip().lower()
    should_disable = disable_value not in {"0", "false", "no", "off"}
    disable_fn = getattr(torch_directml, "disable_tiled_resources", None)
    if callable(disable_fn):
        disable_fn(should_disable)
    return should_disable


def select_torch_device(allow_cpu=False, device="auto"):
    import torch

    device = (device or "auto").strip().lower()
    if device not in {"auto", "gpu", "cpu"}:
        raise RuntimeError("--device must be one of: auto, gpu, cpu")
    if device == "cpu":
        if allow_cpu:
            return torch.device("cpu"), "CPU"
        raise RuntimeError("CPU device was requested; pass --allow-cpu if you intentionally want CPU.")

    torch_directml = get_torch_directml_module()
    if torch_directml is not None and torch_directml.is_available():
        tiled_resources_disabled = configure_directml(torch_directml)
        device_name = get_directml_device_name(torch_directml)
        backend = f"DirectML ({device_name})" if device_name else "DirectML"
        if tiled_resources_disabled:
            backend = f"{backend} [tiled resources disabled]"
        return torch_directml.device(), backend

    if torch.cuda.is_available():
        backend = "ROCm" if torch.version.hip else "CUDA"
        device_name = torch.cuda.get_device_name(0)
        return torch.device("cuda"), f"{backend} ({device_name})"

    if allow_cpu and device == "auto":
        return torch.device("cpu"), "CPU"

    raise RuntimeError(
        "No PyTorch GPU backend is available from this Python. "
        "For this AMD laptop, use native Windows Python with torch-directml, "
        "or install a supported Linux ROCm PyTorch build. Refusing CPU fallback; "
        "pass --allow-cpu only if you intentionally want CPU."
    )


def select_torch_dtype(dtype="auto", backend="", variant=""):
    import torch

    dtype = (dtype or "auto").strip().lower()
    if dtype in {"float32", "fp32"}:
        return torch.float32, "float32"
    if dtype in {"float16", "fp16"}:
        return torch.float16, "float16"
    if dtype != "auto":
        raise RuntimeError("--dtype must be one of: auto, float32, float16")

    backend_name = (backend or "").lower()
    variant_name = (variant or "").strip().lower()
    if variant_name == "fp16" and "cpu" not in backend_name:
        return torch.float16, "float16"
    return torch.float32, "float32"


def apply_image_prompt_defaults(prompt, negative_prompt="", system_prompt=None, disable_system_prompt=False):
    prompt = (prompt or "").strip()
    negative_prompt = (negative_prompt or "").strip()
    system_prompt = DEFAULT_IMAGE_SYSTEM_PROMPT if system_prompt is None else system_prompt.strip()

    if not prompt:
        raise RuntimeError("Prompt cannot be empty.")

    if not disable_system_prompt and system_prompt:
        prompt = f"{system_prompt}, {prompt}"

    if negative_prompt:
        negative_prompt = f"{DEFAULT_IMAGE_NEGATIVE_PROMPT}, {negative_prompt}"
    else:
        negative_prompt = DEFAULT_IMAGE_NEGATIVE_PROMPT

    return prompt, negative_prompt


def validate_torch_diffusion_profile(model, backend, attention_slicing=False, allow_slow_directml=False):
    if allow_slow_directml:
        return

    model_name = (model or "").strip().lower()
    backend_name = (backend or "").strip().lower()
    if not backend_name.startswith("directml"):
        return

    if model_name == DEFAULT_IMAGE_MODEL.lower() and attention_slicing:
        raise RuntimeError(
            "imagepipeline/EpiCRealism-Natural-Sin hangs on this DirectML laptop when attention slicing is enabled. "
            "Run without --attention-slicing, or pass --allow-slow-directml if you intentionally want to try it."
        )


def describe_onnx_environment(providers, is_wsl_environment=False):
    if "DmlExecutionProvider" in providers:
        yield "ONNX Runtime can use DirectML from this Python. That is the normal AMD GPU path on Windows."
    elif "ROCMExecutionProvider" in providers:
        yield "ONNX Runtime can use ROCm from this Python. That is the normal AMD GPU path on Linux."
    elif "CUDAExecutionProvider" in providers:
        yield "ONNX Runtime can use CUDA from this Python. That is for NVIDIA GPUs, not AMD GPUs."
    else:
        yield "ONNX Runtime does not expose a local GPU provider from this Python."

    if "AzureExecutionProvider" in providers:
        yield "AzureExecutionProvider is not your laptop GPU; it does not mean AMD acceleration is active."

    if is_wsl_environment and not has_local_onnx_gpu_provider(providers):
        yield (
            "This is WSL without a Linux AMD GPU provider. For AMD acceleration, use native Windows Python "
            "with onnxruntime-directml, or set up ROCm in WSL if your hardware and drivers support it."
        )
