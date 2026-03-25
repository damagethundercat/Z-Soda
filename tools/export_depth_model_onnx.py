#!/usr/bin/env python3
"""Export a local depth-estimation HF snapshot to ONNX and optionally validate it."""

from __future__ import annotations

import argparse
import contextlib
import types
from pathlib import Path

import numpy as np
import torch
from transformers import AutoModelForDepthEstimation


class DepthExportWrapper(torch.nn.Module):
    """Return only the predicted depth tensor for ONNX export."""

    def __init__(self, model: torch.nn.Module) -> None:
        super().__init__()
        self.model = model

    def forward(self, pixel_values: torch.Tensor) -> torch.Tensor:
        return self.model(pixel_values=pixel_values).predicted_depth


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export a local Hugging Face depth-estimation model to ONNX."
    )
    parser.add_argument("--model-dir", required=True, help="Local HF snapshot directory.")
    parser.add_argument("--output-path", required=True, help="Destination .onnx path.")
    parser.add_argument(
        "--input-size",
        type=int,
        default=518,
        help="Square export input size. DistillAnyDepth base preprocesses to 518 by default.",
    )
    parser.add_argument(
        "--opset",
        type=int,
        default=17,
        help="Requested ONNX opset version.",
    )
    parser.add_argument(
        "--validate-ort",
        action="store_true",
        help="Load the exported model with onnxruntime and run one CPU smoke inference.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite an existing output file.",
    )
    return parser.parse_args()


@contextlib.contextmanager
def patched_dynamic_depth_head(model: torch.nn.Module):
    head = getattr(model, "head", None)
    if head is None or head.__class__.__name__ != "DepthAnythingDepthEstimationHead":
        yield False
        return

    original_forward = head.forward

    def export_friendly_forward(
        self,
        hidden_states: list[torch.Tensor],
        patch_height,
        patch_width,
    ) -> torch.Tensor:
        hidden_states = hidden_states[self.head_in_index]

        predicted_depth = self.conv1(hidden_states)
        predicted_depth = torch.nn.functional.interpolate(
            predicted_depth,
            size=(patch_height * self.patch_size, patch_width * self.patch_size),
            mode="bilinear",
            align_corners=True,
        )
        predicted_depth = self.conv2(predicted_depth)
        predicted_depth = self.activation1(predicted_depth)
        predicted_depth = self.conv3(predicted_depth)
        predicted_depth = self.activation2(predicted_depth) * self.max_depth
        predicted_depth = predicted_depth.squeeze(dim=1)
        return predicted_depth

    head.forward = types.MethodType(export_friendly_forward, head)
    try:
        yield True
    finally:
        head.forward = original_forward


def build_validation_shapes(input_size: int) -> list[tuple[int, int]]:
    offset = 14
    base = max(14, input_size)
    return [
        (base, base),
        (base + offset, base + offset),
        (base, base + offset),
    ]


def export_model(
    model_dir: Path,
    output_path: Path,
    input_size: int,
    opset: int,
    *,
    overwrite: bool,
) -> None:
    if output_path.exists() and not output_path.is_file():
        raise RuntimeError(f"output path exists but is not a file: {output_path}")
    if output_path.exists() and not overwrite:
        raise RuntimeError(f"output path already exists: {output_path}")

    model = AutoModelForDepthEstimation.from_pretrained(model_dir, local_files_only=True)
    model.eval()
    wrapper = DepthExportWrapper(model)
    wrapper.eval()

    dummy_input = torch.randn(1, 3, input_size, input_size, dtype=torch.float32)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with torch.no_grad():
        with patched_dynamic_depth_head(model):
            torch.onnx.export(
                wrapper,
                (dummy_input,),
                output_path,
                input_names=["pixel_values"],
                output_names=["predicted_depth"],
                dynamic_axes={
                    "pixel_values": {0: "batch", 2: "height", 3: "width"},
                    "predicted_depth": {0: "batch", 1: "height", 2: "width"},
                },
                opset_version=opset,
                do_constant_folding=True,
                dynamo=False,
            )


def validate_with_ort(output_path: Path, input_size: int) -> dict[str, object]:
    import onnx
    import onnxruntime as ort

    onnx_model = onnx.load(output_path)
    onnx.checker.check_model(onnx_model)

    session = ort.InferenceSession(str(output_path), providers=["CPUExecutionProvider"])
    input_info = session.get_inputs()[0]
    output_info = session.get_outputs()[0]
    validation_runs: list[dict[str, object]] = []
    output_shapes: set[tuple[int, ...]] = set()
    for height, width in build_validation_shapes(input_size):
        dummy_input = np.random.randn(1, 3, height, width).astype("float32")
        output = session.run([output_info.name], {input_info.name: dummy_input})[0]
        output_shape = tuple(int(dim) for dim in output.shape)
        output_shapes.add(output_shape)
        validation_runs.append(
            {
                "input_shape": [1, 3, height, width],
                "output_shape": list(output_shape),
                "output_min": float(output.min()),
                "output_max": float(output.max()),
            }
        )

    if len(output_shapes) == 1 and len(validation_runs) > 1:
        frozen_shape = validation_runs[0]["output_shape"]
        raise RuntimeError(
            "Exported ONNX output shape stayed constant across validation shapes: "
            f"{frozen_shape}. The export is not dynamically preserving process resolution."
        )

    first_run = validation_runs[0]
    return {
        "input_name": input_info.name,
        "input_shape": input_info.shape,
        "output_name": output_info.name,
        "output_shape": first_run["output_shape"],
        "output_dtype": "float32",
        "output_min": first_run["output_min"],
        "output_max": first_run["output_max"],
        "validation_runs": validation_runs,
    }


if __name__ == "__main__":
    args = parse_args()
    model_dir = Path(args.model_dir).resolve()
    output_path = Path(args.output_path).resolve()

    if not model_dir.is_dir():
        raise SystemExit(f"model dir was not found: {model_dir}")

    export_model(
        model_dir,
        output_path,
        args.input_size,
        args.opset,
        overwrite=args.overwrite,
    )
    print(f"exported: {output_path}")
    print(f"size_bytes: {output_path.stat().st_size}")

    if args.validate_ort:
        report = validate_with_ort(output_path, args.input_size)
        print(f"ort_input: {report['input_name']} {report['input_shape']}")
        print(f"ort_output: {report['output_name']} {report['output_shape']} {report['output_dtype']}")
        print(f"ort_output_range: {report['output_min']:.6f} .. {report['output_max']:.6f}")
        for run in report["validation_runs"]:
            print(f"ort_validation: input={run['input_shape']} output={run['output_shape']}")
