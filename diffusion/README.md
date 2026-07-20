Use img2img as,

```
python img2img.py ^
  --input input.jpg ^
  --output output.png ^
  --prompt "clean studio product photo, softbox lighting, high detail" ^
  --strength 0.5 --steps 25 --cfg 7.5 --size 512
```

Use text2img as,

```
python text2img.py ^
  --output output.png ^
  --prompt "clean studio product photo, softbox lighting, high detail" ^
  --steps 25 --cfg 7.5 --width 512 --height 512
```

Or use the Makefile with only a prompt,

```
make PROMPT="clean studio product photo, softbox lighting, high detail"
```

Check Python GPU setup,

```
make gpu-info
```

Run the PyTorch example,

```
make example
make example PROMPT="clean studio product photo, softbox lighting, high detail"
```

The default `make example` settings use `imagepipeline/EpiCRealism-Natural-Sin` with the `fp16` Diffusers variant and DirectML on the laptop GPU at `512x512` for `20` steps. The shared compute layer disables DirectML tiled resources by default, and attention slicing is opt-in because it makes this model hang on DirectML.

The image scripts (`example.py`, `text2img.py`, and `img2img.py`) default to `imagepipeline/EpiCRealism-Natural-Sin` with the `fp16` Diffusers variant. `text2img.py` and `img2img.py` still support exported ONNX Runtime models when `--ort_dir` is passed.

The image scripts also prepend a shared photorealistic prompt prefix and apply a shared negative prompt by default. Override the prefix with `--system-prompt "..."`, disable it with `--no-system-prompt`, and add more negative terms with `--negative "..."`.

GPU/device selection and image-model defaults live in `compute.py`, shared by the PyTorch, ONNX Runtime, video, and GPU-info scripts.

Run script smoke tests,

```
make test
```

Clean generated outputs and Python caches,

```
make clean
```

Check the prompt-to-video CLI/import/device-selection path without downloading the full video model,

```
make video-check
```

Run prompt-to-video generation when you are ready to download the video model,

```
make video-example PROMPT="cinematic pan across a small robot on a desk"
```

AMD GPU note: generation now requires a local GPU provider by default. On this laptop, the normal Windows path is `DmlExecutionProvider` from `onnxruntime-directml`; the normal Linux path is `ROCMExecutionProvider` from a supported ROCm ONNX Runtime setup. `AzureExecutionProvider` and `CPUExecutionProvider` are not the laptop GPU, and the scripts refuse to use them unless `--allow_cpu` (ONNX scripts) or `--allow-cpu` (PyTorch scripts) is passed explicitly.

For native Windows AMD setup, install the DirectML dependencies with Windows Python:

```
python -m pip install -r requirements-directml.txt
```

When launching these make targets from WSL, they intentionally invoke `.venv-win\Scripts\python.exe` so DirectML can see the Windows AMD GPU; do not use WSL Python for the DirectML examples.