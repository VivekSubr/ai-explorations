
import argparse
import os
from PIL import Image

from compute import (
    DEFAULT_IMAGE_MODEL,
    DEFAULT_IMAGE_SYSTEM_PROMPT,
    DEFAULT_IMAGE_VARIANT,
    DEFAULT_TORCH_DTYPE,
    apply_image_prompt_defaults,
    get_onnx_providers,
    select_onnx_provider,
    select_torch_device,
    select_torch_dtype,
    validate_torch_diffusion_profile,
)


def load_image(path, size=512):
    img = Image.open(path).convert("RGB")
    img = img.resize((size, size))
    return img

def validate_ort_dir(path):
    if not os.path.isdir(path):
        raise RuntimeError(f"ORT model directory does not exist: {path}")
    model_index = os.path.join(path, "model_index.json")
    if not os.path.isfile(model_index):
        raise RuntimeError(f"ORT model directory is missing model_index.json: {path}")

def output_path_or_error(parser, output):
    output_path = output.strip() or "output.png"
    if not os.path.splitext(output_path)[1]:
        parser.error("--output must include a file extension, for example output.png")
    return output_path

def run_torch_img2img(parser, args, output_path):
    import torch
    from diffusers import DPMSolverMultistepScheduler, StableDiffusionImg2ImgPipeline

    try:
        device, backend = select_torch_device(allow_cpu=args.allow_cpu, device=args.device)
        torch_dtype, dtype_name = select_torch_dtype(args.dtype, backend=backend, variant=args.variant)
        prompt, negative_prompt = apply_image_prompt_defaults(
            args.prompt,
            negative_prompt=args.negative,
            system_prompt=args.system_prompt,
            disable_system_prompt=args.no_system_prompt,
        )
        validate_torch_diffusion_profile(
            model=args.model,
            backend=backend,
            attention_slicing=args.attention_slicing,
            allow_slow_directml=args.allow_slow_directml,
        )
    except RuntimeError as exc:
        parser.error(str(exc))

    print("Using PyTorch backend:", backend)
    print("Using PyTorch device:", device)
    print("Using PyTorch dtype:", dtype_name)
    print("Using system prompt:", "disabled" if args.no_system_prompt else args.system_prompt)
    print("Using negative prompt:", negative_prompt)
    if args.seed:
        torch.manual_seed(args.seed)

    pipe = StableDiffusionImg2ImgPipeline.from_pretrained(
        args.model,
        variant=args.variant.strip() or None,
        torch_dtype=torch_dtype,
        safety_checker=None,
        feature_extractor=None,
        requires_safety_checker=False,
    )
    pipe.scheduler = DPMSolverMultistepScheduler.from_config(
        pipe.scheduler.config,
        algorithm_type="dpmsolver++",
        use_karras_sigmas=True,
    )
    if args.attention_slicing:
        pipe.enable_attention_slicing()
    pipe = pipe.to(device)

    init_image = load_image(args.input, size=args.size)
    with torch.no_grad():
        result = pipe(
            prompt=prompt,
            negative_prompt=negative_prompt,
            image=init_image,
            strength=args.strength,
            guidance_scale=args.cfg,
            num_inference_steps=args.steps,
        )

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    result.images[0].save(output_path)
    print(f"Saved: {output_path}")

def run_ort_img2img(parser, args, output_path):
    from optimum.onnxruntime import ORTStableDiffusionImg2ImgPipeline

    providers = get_onnx_providers()
    try:
        provider = select_onnx_provider(requested_provider=args.provider, allow_cpu=args.allow_cpu, providers=providers)
        validate_ort_dir(args.ort_dir)
        prompt, negative_prompt = apply_image_prompt_defaults(
            args.prompt,
            negative_prompt=args.negative,
            system_prompt=args.system_prompt,
            disable_system_prompt=args.no_system_prompt,
        )
    except RuntimeError as exc:
        parser.error(str(exc))

    print("Available providers:", providers)
    print("Using provider:", provider)
    print("Using system prompt:", "disabled" if args.no_system_prompt else args.system_prompt)
    print("Using negative prompt:", negative_prompt)

    pipe = ORTStableDiffusionImg2ImgPipeline.from_pretrained(
        args.ort_dir,
        provider=provider
    )
    pipe.safety_checker = None
    pipe.feature_extractor = None

    init_image = load_image(args.input, size=args.size)

    result = pipe(
        prompt=prompt,
        negative_prompt=negative_prompt,
        image=init_image,
        strength=args.strength,
        guidance_scale=args.cfg,
        num_inference_steps=args.steps,
        seed=args.seed if args.seed != 0 else None
    )

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    result.images[0].save(output_path)
    print(f"Saved: {output_path}")

def main():
    ap = argparse.ArgumentParser(description="Stable Diffusion img2img using PyTorch DirectML by default, or ONNX Runtime with --ort_dir")
    ap.add_argument("--input", required=True)
    ap.add_argument("--output", default="out.png")
    ap.add_argument("--prompt", required=True)
    ap.add_argument("--negative", default="", help="Additional negative prompt; appended to the shared photorealistic negative prompt")
    ap.add_argument("--system-prompt", default=DEFAULT_IMAGE_SYSTEM_PROMPT, help="Prompt prefix used to bias image generation")
    ap.add_argument("--no-system-prompt", action="store_true", help="Do not prepend the shared photorealistic prompt prefix")
    ap.add_argument("--model", default=DEFAULT_IMAGE_MODEL)
    ap.add_argument("--variant", default=DEFAULT_IMAGE_VARIANT, help="Optional diffusers weight variant, for example fp16")
    ap.add_argument("--dtype", default=DEFAULT_TORCH_DTYPE, help="Torch dtype for PyTorch mode: auto, float32, or float16")
    ap.add_argument("--ort_dir", help="Path to exported ORT pipeline directory; enables ONNX Runtime mode")
    ap.add_argument("--strength", type=float, default=0.6)
    ap.add_argument("--steps", type=int, default=25)
    ap.add_argument("--cfg", type=float, default=7.5)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--size", type=int, default=512)
    ap.add_argument("--provider", help="Optional ONNX Runtime provider override, for example DmlExecutionProvider")
    ap.add_argument("--attention-slicing", action="store_true", help="Enable diffusers attention slicing in PyTorch mode")
    ap.add_argument("--device", choices=("auto", "gpu", "cpu"), default="auto", help="Torch device selection")
    ap.add_argument("--allow_cpu", "--allow-cpu", dest="allow_cpu", action="store_true", help="Allow CPU fallback; generation requires a GPU by default")
    ap.add_argument("--allow-slow-directml", action="store_true", help="Allow DirectML profiles known to hang on this laptop")
    args = ap.parse_args()

    output_path = output_path_or_error(ap, args.output)
    if args.ort_dir:
        run_ort_img2img(ap, args, output_path)
    else:
        run_torch_img2img(ap, args, output_path)

if __name__ == "__main__":
    main()
