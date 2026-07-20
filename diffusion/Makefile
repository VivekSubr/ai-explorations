WINDOWS_PYTHON ?= C:/Users/subramaniamv/AppData/Local/Programs/Python/Python312/python.exe
DIRECTML_PYTHON ?= .venv-win/Scripts/python.exe
PYTHON ?= $(if $(wildcard $(DIRECTML_PYTHON)),$(DIRECTML_PYTHON),.venv/bin/python)
GPU_INFO_PYTHON ?= $(if $(wildcard $(DIRECTML_PYTHON)),$(DIRECTML_PYTHON),$(if $(wildcard .venv/bin/python),.venv/bin/python,python3))
ORT_DIR ?= $(if $(wildcard $(DIRECTML_PYTHON)),C:/models/sd15-ort-text2img,/mnt/c/models/sd15-ort-text2img)
IMAGE_MODEL ?= imagepipeline/EpiCRealism-Natural-Sin
IMAGE_VARIANT ?= fp16
IMAGE_DTYPE ?= auto
IMAGE_ATTENTION_SLICING ?= 0
IMAGE_STEPS ?= 20
IMAGE_WIDTH ?= 512
IMAGE_HEIGHT ?= 512
EXAMPLE_PROMPT ?= sexy indian woman
EXAMPLE_OUTPUT ?= output.png
TEXT2IMG_OUTPUT ?= output.png
TEXT2IMG_MODEL ?= $(IMAGE_MODEL)
TEXT2IMG_VARIANT ?= $(IMAGE_VARIANT)
TEXT2IMG_DTYPE ?= $(IMAGE_DTYPE)
TEXT2IMG_ATTENTION_SLICING ?= $(IMAGE_ATTENTION_SLICING)
TEXT2IMG_STEPS ?= $(IMAGE_STEPS)
TEXT2IMG_WIDTH ?= $(IMAGE_WIDTH)
TEXT2IMG_HEIGHT ?= $(IMAGE_HEIGHT)
TEXT2IMG_ORT_DIR ?=
EXAMPLE_MODEL ?= $(IMAGE_MODEL)
EXAMPLE_VARIANT ?= $(IMAGE_VARIANT)
EXAMPLE_DTYPE ?= $(IMAGE_DTYPE)
EXAMPLE_ATTENTION_SLICING ?= $(IMAGE_ATTENTION_SLICING)
EXAMPLE_STEPS ?= $(IMAGE_STEPS)
EXAMPLE_WIDTH ?= $(IMAGE_WIDTH)
EXAMPLE_HEIGHT ?= $(IMAGE_HEIGHT)
VIDEO_OUTPUT ?= video/output.mp4
VIDEO_MODEL ?= damo-vilab/text-to-video-ms-1.7b
VIDEO_DTYPE ?= auto
VIDEO_FRAMES ?= 8
VIDEO_STEPS ?= 5
VIDEO_WIDTH ?= 256
VIDEO_HEIGHT ?= 256
VIDEO_FPS ?= 8

.DEFAULT_GOAL := text2img

.PHONY: setup setup-directml text2img example video-example video-check gpu-info test-gpu test clean
setup: .venv/.requirements.stamp
setup-directml: .venv-win/.requirements-directml.stamp

.venv/bin/python:
	python3 -m venv .venv

.venv/.requirements.stamp: requirements.txt .venv/bin/python
	"$(PYTHON)" -m pip install --upgrade pip
	"$(PYTHON)" -m pip install -r requirements.txt
	touch .venv/.requirements.stamp

.venv-win/Scripts/python.exe:
	"$(WINDOWS_PYTHON)" -m venv .venv-win

.venv-win/.requirements-directml.stamp: requirements-directml.txt .venv-win/Scripts/python.exe
	"$(DIRECTML_PYTHON)" -m pip install --upgrade pip
	"$(DIRECTML_PYTHON)" -m pip install -r requirements-directml.txt
	touch .venv-win/.requirements-directml.stamp

text2img: .venv-win/.requirements-directml.stamp
	"$(DIRECTML_PYTHON)" text2img.py --prompt "$(if $(PROMPT),$(PROMPT),$(EXAMPLE_PROMPT))" --output "$(if $(strip $(TEXT2IMG_OUTPUT)),$(TEXT2IMG_OUTPUT),output.png)" $(if $(strip $(TEXT2IMG_ORT_DIR)),--ort_dir "$(TEXT2IMG_ORT_DIR)",--model "$(TEXT2IMG_MODEL)" --variant "$(TEXT2IMG_VARIANT)" --dtype "$(TEXT2IMG_DTYPE)" $(if $(filter 1 true yes on,$(TEXT2IMG_ATTENTION_SLICING)),--attention-slicing,)) --steps "$(TEXT2IMG_STEPS)" --width "$(TEXT2IMG_WIDTH)" --height "$(TEXT2IMG_HEIGHT)"

example: .venv-win/.requirements-directml.stamp
	"$(DIRECTML_PYTHON)" example.py --prompt "$(if $(PROMPT),$(PROMPT),$(EXAMPLE_PROMPT))" --output "$(if $(strip $(EXAMPLE_OUTPUT)),$(EXAMPLE_OUTPUT),output.png)" --model "$(EXAMPLE_MODEL)" --variant "$(EXAMPLE_VARIANT)" --dtype "$(EXAMPLE_DTYPE)" $(if $(filter 1 true yes on,$(EXAMPLE_ATTENTION_SLICING)),--attention-slicing,) --steps "$(EXAMPLE_STEPS)" --width "$(EXAMPLE_WIDTH)" --height "$(EXAMPLE_HEIGHT)"

video-example: .venv-win/.requirements-directml.stamp
	"$(DIRECTML_PYTHON)" video/example.py --prompt "$(if $(PROMPT),$(PROMPT),$(EXAMPLE_PROMPT))" --output "$(VIDEO_OUTPUT)" --model "$(VIDEO_MODEL)" --dtype "$(VIDEO_DTYPE)" --frames "$(VIDEO_FRAMES)" --steps "$(VIDEO_STEPS)" --width "$(VIDEO_WIDTH)" --height "$(VIDEO_HEIGHT)" --fps "$(VIDEO_FPS)"

video-check: .venv-win/.requirements-directml.stamp
	"$(DIRECTML_PYTHON)" video/example.py --check --dtype "$(VIDEO_DTYPE)"

gpu-info:
	"$(GPU_INFO_PYTHON)" gpu_info.py --ort-dir "$(ORT_DIR)"

test-gpu:
	"$(GPU_INFO_PYTHON)" gpu_info.py --ort-dir "$(ORT_DIR)" --require-gpu

test: .venv-win/.requirements-directml.stamp
	bash test_scripts.sh

clean:
	powershell -NoProfile -ExecutionPolicy Bypass -Command "$$ErrorActionPreference = 'SilentlyContinue'; Remove-Item -Force 'output.png','out.png'; Remove-Item -Force 'video\output.mp4'; Remove-Item -Recurse -Force '__pycache__','video\__pycache__','.pytest_cache','test-output'; exit 0"