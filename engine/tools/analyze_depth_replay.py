#!/usr/bin/env python3
"""Compare private nearest-depth replay with effective attached depth, without changing rendering."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


def compare(nearest, reference, fmt):
    if len(nearest) != len(reference) or len(nearest) % 4:
        raise ValueError('Depth planes must have identical whole-word lengths')
    if fmt not in (126, 129, 130):
        raise ValueError('Unsupported reference depth format')
    histogram = {'0': 0, '1': 0, '2': 0, '3-4': 0, '5-16': 0, '17-256': 0, '>256': 0}
    covered = invalid = negative = positive = 0
    minimum = maximum = None
    pixels = bytearray()
    for (near_bits,), (ref_bits,) in zip(struct.iter_unpack('<I', nearest), struct.iter_unpack('<I', reference)):
        near = struct.unpack('<f', struct.pack('<I', near_bits))[0]
        if math.isinf(near) and near > 0:
            pixels.append(0)
            continue
        if not math.isfinite(near) or not 0 <= near <= 1:
            invalid += 1
            pixels.append(255)
            continue
        if fmt == 129:
            delta = round(near * 0xffffff) - (ref_bits & 0xffffff)
        else:
            ref = struct.unpack('<f', struct.pack('<I', ref_bits))[0]
            if not math.isfinite(ref) or not 0 <= ref <= 1:
                invalid += 1
                pixels.append(255)
                continue
            # Both are non-negative IEEE32 values: bit order is numeric order.
            delta = near_bits - ref_bits
        covered += 1
        negative += delta < 0
        positive += delta > 0
        minimum = delta if minimum is None else min(minimum, delta)
        maximum = delta if maximum is None else max(maximum, delta)
        absolute = abs(delta)
        bucket = str(absolute) if absolute <= 2 else '3-4' if absolute <= 4 else '5-16' if absolute <= 16 else '17-256' if absolute <= 256 else '>256'
        histogram[bucket] += 1
        pixels.append(32 if not absolute else min(254, 64 + absolute * 12))
    return {'covered': covered, 'invalid': invalid, 'uncovered': len(nearest) // 4 - covered - invalid,
            'histogram_abs_delta': histogram, 'negative': negative, 'positive': positive,
            'signed_delta_min': minimum, 'signed_delta_max': maximum,
            'units': 'rounded D24 candidate code, not guaranteed raster quantization' if fmt == 129 else 'positive IEEE32 ULP',
            'interpretation': 'Large differences may be legitimate occlusion or ProjectionMask depth writes; unequal pixels alone do not prove a bug.'}, bytes(pixels)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('metadata', type=Path)
    args = parser.parse_args()
    meta = json.loads(args.metadata.read_text())
    if meta.get('status') != 'captured' or meta.get('mode') != 'nearest-depth-MIN':
        raise ValueError('Expected a completed nearest-depth-MIN capture')
    width, height = meta['width'], meta['height']
    if not isinstance(width, int) or not isinstance(height, int) or min(width, height) < 1 or width * height > 2 * 1024 * 1024:
        raise ValueError('Invalid capture dimensions')
    prefix = args.metadata.with_suffix('')
    nearest_path = Path(str(prefix) + '-nearest.bin')
    reference_path = Path(str(prefix) + '-reference.bin')
    nearest, reference = nearest_path.read_bytes(), reference_path.read_bytes()
    if len(nearest) != width * height * 4 or len(reference) != len(nearest):
        raise ValueError('Capture dimensions do not match depth planes')
    result, pixels = compare(nearest, reference, meta['depth_format'])
    result.update({'schema': 1, 'frame': meta['frame'], 'ordinal': meta['ordinal'],
                   'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (args.metadata, nearest_path, reference_path)}})
    Path(str(prefix) + '-delta.pgm').write_bytes(f'P5\n{width} {height}\n255\n'.encode() + pixels)
    Path(str(prefix) + '-analysis.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
