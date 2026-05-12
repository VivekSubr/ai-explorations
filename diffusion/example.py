
from diffusers import StableDiffusionPipeline
import torch

pipe = StableDiffusionPipeline.from_pretrained(
    "runwayml/stable-diffusion-v1-5",
    torch_dtype=torch.float32,
    safety_checker=None,
)

pipe = pipe.to("cpu")  # fallback (DirectML handled internally)

prompt = "indian woman with boobs out"

image = pipe(prompt).images[0]
image.save("output.png")
