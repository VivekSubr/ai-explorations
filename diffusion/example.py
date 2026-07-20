import argparse
import os

import torch
from diffusers import DPMSolverMultistepScheduler, StableDiffusionPipeline

from compute import (
    DEFAULT_IMAGE_MODEL,
    DEFAULT_IMAGE_NEGATIVE_PROMPT,
    DEFAULT_IMAGE_SYSTEM_PROMPT,
    DEFAULT_IMAGE_VARIANT,
    DEFAULT_TORCH_DTYPE,
    apply_image_prompt_defaults,
    select_torch_device,
    select_torch_dtype,
    validate_torch_diffusion_profile,
)


def main():
    parser = argparse.ArgumentParser(description="Stable Diffusion text2img using a required PyTorch GPU backend")
    parser.add_argument("--prompt", required=True, help="Text description of the image to generate")
    parser.add_argument("--output", default="output.png")
    parser.add_argument("--negative", default="", help="Additional negative prompt; appended to the shared photorealistic negative prompt")
    parser.add_argument("--system-prompt", default=DEFAULT_IMAGE_SYSTEM_PROMPT, help="Prompt prefix used to bias image generation")
    parser.add_argument("--no-system-prompt", action="store_true", help="Do not prepend the shared photorealistic prompt prefix")
    parser.add_argument("--model", default=DEFAULT_IMAGE_MODEL)
    parser.add_argument("--variant", default=DEFAULT_IMAGE_VARIANT, help="Optional diffusers weight variant, for example fp16")
    parser.add_argument("--dtype", default=DEFAULT_TORCH_DTYPE, help="Torch dtype: auto, float32, or float16")
    parser.add_argument("--steps", type=int, default=25)
    parser.add_argument("--cfg", type=float, default=7.5)
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--height", type=int, default=512)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--attention-slicing", action="store_true", help="Enable diffusers attention slicing")
    parser.add_argument("--device", choices=("auto", "gpu", "cpu"), default="auto", help="Torch device selection")
    parser.add_argument("--allow-cpu", action="store_true", help="Allow CPU fallback; generation requires a GPU by default")
    parser.add_argument("--allow-slow-directml", action="store_true", help="Allow DirectML profiles known to hang on this laptop")
    args = parser.parse_args()

    try:
        device, backend = select_torch_device(allow_cpu=args.allow_cpu, device=args.device)
        torch_dtype, dtype_name = select_torch_dtype(args.dtype, backend=backend, variant=args.variant)
        prompt, negative_prompt = apply_image_prompt_defaults(
            args.prompt,
            negative_prompt=args.negative,
            system_prompt=args.system_prompt,
            disable_system_prompt=args.no_system_prompt,
        )
    except RuntimeError as exc:
        parser.error(str(exc))

    output_path = args.output.strip() or "output.png"
    if not os.path.splitext(output_path)[1]:
        parser.error("--output must include a file extension, for example output.png")
    try:
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
    print("Using negative prompt:", negative_prompt or DEFAULT_IMAGE_NEGATIVE_PROMPT)
    if args.seed:
        torch.manual_seed(args.seed)

    pipe = StableDiffusionPipeline.from_pretrained(
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

    with torch.no_grad():
        image = pipe(
            prompt,
            negative_prompt=negative_prompt,
            num_inference_steps=args.steps,
            guidance_scale=args.cfg,
            width=args.width,
            height=args.height,
        ).images[0]

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    image.save(output_path)
    print(f"Saved: {output_path}")


if __name__ == "__main__":
    main()
