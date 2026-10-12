#!/usr/bin/env python3
"""SPIR-V шейдеров треугольника → examples/triangle/shaders.dpm (Р64).

    python3 examples/triangle/shaders/build.py

Нужен glslangValidator (пакет glslang-tools); байты SPIR-V лежат в
репозитории константами dpl, так что сборке программы он не нужен.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'shaders.dpm')
SHADERS = [('VERT', 'triangle.vert'), ('FRAG', 'triangle.frag')]


def main():
    version = subprocess.run(['glslangValidator', '--version'], capture_output=True, text=True).stdout.split('\n')[0]
    lines = ['// SPIR-V шейдеров треугольника (examples/triangle/shaders/*.vert, *.frag).',
             f'// Сгенерировано shaders/build.py ({version.strip()}) — не править.', '']
    with tempfile.TemporaryDirectory() as d:
        for name, src in SHADERS:
            spv = os.path.join(d, src + '.spv')
            subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.0', '-o', spv, os.path.join(HERE, src)],
                           check=True, capture_output=True)
            with open(spv, 'rb') as f:
                data = f.read()
            lines.append(f'// {src}: {len(data)} байт')
            # Одной строкой — канонический вид `dpl fmt` (переноса длинных строк в нём нет).
            lines.append(f'pub const {name}: List[u8] = [' + ', '.join(str(b) for b in data) + ']')
            lines.append('')
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines).rstrip('\n') + '\n')
    print(f'{os.path.relpath(OUT)}: записан')
    return 0


if __name__ == '__main__':
    sys.exit(main())
