#!/usr/bin/env python3
"""
tus_convert.py — Convert JT-8000 HTML editor preset exports to C++ hex arrays.

Usage:
    python3 tus_convert.py <input_file.txt>

Input format (from HTML editor "Export" or serial dump):
    // JT-8000 Preset: MyPresetName
    PATCH:21,123;  // CC 21 WAVE = 123
    PATCH:23,76;   // CC 23 CUTOFF = 60%
    ...

Output: C++ initialiser blocks ready to paste into TUS_Presets.h.

Each preset becomes a TUSPatchCC entry with a uint8_t data[128] array
indexed by CC number. Paste the output into the kTUS_Patches[] array
and update kTUS_COUNT.
"""

import re
import sys

def parse_presets(filepath):
    """Parse a text file containing one or more JT-8000 preset exports."""
    with open(filepath, 'r') as f:
        content = f.read()

    # Split on preset header lines
    blocks = re.split(r'// JT-8000 Preset: ', content)
    blocks = [b for b in blocks if b.strip()]

    presets = []
    for block in blocks:
        lines = block.strip().split('\n')
        name = lines[0].strip()

        # Build CC array, default all to 0
        data = [0] * 128
        for line in lines:
            m = re.match(r'PATCH:(\d+),(\d+);', line.strip())
            if m:
                cc  = int(m.group(1))
                val = int(m.group(2))
                if 0 <= cc <= 127:
                    data[cc] = val

        presets.append((name, data))

    return presets


def format_cpp(presets, start_index=0):
    """Format presets as C++ TUSPatchCC initialisers."""
    output = []
    for i, (name, data) in enumerate(presets):
        idx = start_index + i
        output.append(f'    /* {idx}: {name} */')
        output.append(f'    {{ "{name}", {{')

        # 16 rows of 8 bytes each = 128 CCs
        for row in range(16):
            start = row * 8
            end   = start + 8
            vals  = data[start:end]
            hexvals = ', '.join(f'0x{v:02X}' for v in vals)
            comma = ',' if row < 15 else ' '
            output.append(f'        {hexvals}{comma}  // CC {start:3d}-{end-1:3d}')

        output.append('    }},')
        output.append('')

    return '\n'.join(output)


def main():
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} <input_file.txt>')
        print('  Converts JT-8000 PATCH:cc,val; exports to C++ hex arrays.')
        sys.exit(1)

    filepath = sys.argv[1]
    presets = parse_presets(filepath)

    if not presets:
        print(f'No presets found in {filepath}')
        sys.exit(1)

    print(f'// Converted {len(presets)} preset(s) from {filepath}')
    print(f'// Paste into kTUS_Patches[] in TUS_Presets.h')
    print(f'// Update kTUS_COUNT to match total preset count')
    print()
    print(format_cpp(presets))
    print(f'// Total: {len(presets)} preset(s), {len(presets) * 128} bytes data')


if __name__ == '__main__':
    main()
