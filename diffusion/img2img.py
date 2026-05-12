
import argparse
import os
from PIL import Image
import numpy as np

from optimum.onnxruntime import ORTStableDiffusionImg2ImgPipeline
from onnxruntime import get_available_providers

def load_image(path, size=512):
    img = Image.open(path).convert("RGB")
    img = img.resize((size, size))
    return img

def main():
    ap = argparse.ArgumentParser(description="Stable Diffusion img2img using ONNX Runtime + DirectML")
    ap.add_argument("--input", required=True)
    ap.add_argument("--output", default="out.png")
    ap.add_argument("--prompt", required=True)
    ap.add_argument("--negative", default="")
    ap.add_argument("--ort_dir", required=True, help="Path to exported ORT pipeline directory")
    ap.add_argument("--strength", type=float, default=0.6)
    ap.add_argument("--steps", type=int, default=25)
    ap.add_argument("--cfg", type=float, default=7.5)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--size", type=int, default=512)
    args = ap.parse_args()

    providers = get_available_providers()
    # Prefer DirectML if present
    provider = "DmlExecutionProvider" if "DmlExecutionProvider" in providers else providers[0]
    print("Available providers:", providers)
    print("Using provider:", provider)

    pipe = ORTStableDiffusionImg2ImgPipeline.from_pretrained(
        args.ort_dir,
        provider=provider
    )

    init_image = load_image(args.input, size=args.size)

    # Some ORT pipelines accept numpy generator seeds differently; simplest is set seed at call
    result = pipe(
        prompt=args.prompt,
        negative_prompt=args.negative if args.negative else None,
        image=init_image,
        strength=args.strength,
        guidance_scale=args.cfg,
        num_inference_steps=args.steps,
        seed=args.seed if args.seed != 0 else None
    )

    out = result.images[0]
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    out.save(args.output)
    print(f"Saved: {args.output}")

if __name__ == "__main__":
    main()
