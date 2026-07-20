import argparse
import os
import sys
from pathlib import Path

import imageio.v3 as imageio
import torch
from diffusers import TextToVideoSDPipeline

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compute import DEFAULT_TORCH_DTYPE, DEFAULT_VIDEO_MODEL, select_torch_device, select_torch_dtype


def main():
    parser = argparse.ArgumentParser(description="Text-to-video using a required PyTorch GPU backend")
    parser.add_argument("--prompt", help="Text description of the video to generate")
    parser.add_argument("--output", default="video/output.mp4")
    parser.add_argument("--model", default=DEFAULT_VIDEO_MODEL)
    parser.add_argument("--frames", type=int, default=16)
    parser.add_argument("--steps", type=int, default=20)
    parser.add_argument("--fps", type=int, default=8)
    parser.add_argument("--cfg", type=float, default=7.5)
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=256)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--dtype", default=DEFAULT_TORCH_DTYPE, help="Torch dtype: auto, float32, or float16")
    parser.add_argument("--allow-cpu", action="store_true", help="Allow CPU fallback; generation requires a GPU by default")
    parser.add_argument("--check", action="store_true", help="Only verify imports and GPU device selection; do not load the model")
    args = parser.parse_args()

    if not args.prompt and not args.check:
        parser.error("--prompt is required unless --check is used")

    try:
        device, backend = select_torch_device(allow_cpu=args.allow_cpu)
        torch_dtype, dtype_name = select_torch_dtype(args.dtype, backend=backend)
    except RuntimeError as exc:
        parser.error(str(exc))

    print("Using PyTorch backend:", backend)
    print("Using PyTorch device:", device)
    print("Using PyTorch dtype:", dtype_name)
    if args.check:
        print("Video DirectML smoke check passed.")
        return

    if args.seed:
        torch.manual_seed(args.seed)

    pipe = TextToVideoSDPipeline.from_pretrained(
        args.model,
        torch_dtype=torch_dtype,
    )
    pipe.enable_attention_slicing()
    pipe = pipe.to(device)

    with torch.no_grad():
        frames = pipe(
            args.prompt,
            num_frames=args.frames,
            num_inference_steps=args.steps,
            guidance_scale=args.cfg,
            width=args.width,
            height=args.height,
        ).frames[0]

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    imageio.imwrite(args.output, frames, fps=args.fps)
    print(f"Saved: {args.output}")


if __name__ == "__main__":
    main()
