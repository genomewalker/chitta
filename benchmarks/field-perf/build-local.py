"""Configure all generated CMake output inside this worktree."""
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = Path('/maps/projects/fernandezguerra/apps/repos/cc-soul/chitta/build')
args = ['cmake', '-S', str(root / 'chitta'), '-B', str(root / 'chitta/build'),
        '-DCMAKE_BUILD_TYPE=Release']
for line in (main / 'CMakeCache.txt').read_text().splitlines():
    if line.startswith('CHITTA_') or line.startswith(('CMAKE_C_COMPILER:', 'CMAKE_CXX_COMPILER:', 'Python3_ROOT_DIR:')):
        key, value = line.split('=', 1)
        if key.startswith('CHITTA_FIELD_ROOT:'):
            value = str(root / 'chitta-field')
        args.append('-D' + key + '=' + value)
for path in (main / '_deps').glob('*-src'):
    args.append('-DFETCHCONTENT_SOURCE_DIR_' + path.name[:-4].upper() + '=' + str(path))
lib = '/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/lib/libopenblas.so'
args.extend(['-DBLAS_openblas_LIBRARY=' + lib, '-DBLAS_LIBRARIES=' + lib])
subprocess.run(args, check=True)
