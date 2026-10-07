#!/usr/bin/env python3
"""Convert .blend or .blendrig to native USD using the registered reader."""
import argparse
from pathlib import Path
import sys

from pxr import Sdf, Usd


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--strict", action="store_true", help="Reject every reported unsupported feature")
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if source == output or output.suffix.lower() not in {".usd", ".usda", ".usdc"}:
        parser.error("output must be a distinct .usd, .usda or .usdc file")
    layer = Sdf.Layer.FindOrOpen(str(source), {"strict": "1"} if args.strict else {})
    if not layer:
        raise RuntimeError("translation failed")
    stage = Usd.Stage.Open(layer)
    for message in stage.GetDefaultPrim().GetAttribute("blender:diagnostics").Get() or []:
        print("diagnostic: " + message, file=sys.stderr)
    if not layer.Export(str(output)):
        raise RuntimeError("USD export failed")
    print(output)


if __name__ == "__main__":
    main()
