Use img2img as,

```
python img2img_ort_directml.py ^
  --ort_dir "C:\models\sd15-ort-img2img" ^
  --input input.jpg ^
  --output output.png ^
  --prompt "clean studio product photo, softbox lighting, high detail" ^
  --strength 0.5 --steps 25 --cfg 7.5 --size 512
```