"""Package corresponding source separately from the minimal runtime ZIP."""
import argparse
import hashlib
from pathlib import Path
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--project', type=Path, required=True)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--version', required=True)
args = parser.parse_args()
root, build = args.project.resolve(), args.build.resolve()
name = 'ttp_ac3dts'
output = build / 'Release' / f'{name}-{args.version}-source.zip'
a52 = Path((build / 'a52-source-path.txt').read_text(encoding='utf-8').strip())
ffmpeg = Path((build / 'ffmpeg-source-path.txt').read_text(encoding='utf-8').strip())
cache = Path((build / 'ffmpeg-build-path.txt').read_text(encoding='utf-8').strip())
make = cache / 'make-4.4.1.tar.gz'
if not make.exists():
    make = root / 'third_party/make-4.4.1.tar.gz'
if hashlib.sha256(make.read_bytes()).hexdigest() != 'dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3':
    raise RuntimeError('GNU make source digest mismatch')

def files(base):
    for path in sorted(base.rglob('*')):
        if path.is_file() and not any(x in ('.git', '__pycache__', 'WinRel', 'WinDebug') for x in path.relative_to(base).parts) and path.suffix.lower() not in ('.pyc', '.pdb', '.obj', '.exe', '.dll', '.lib'):
            yield path

with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for entry in ('CMakeLists.txt', 'build.ps1', 'README.md', '.gitignore', 'cmake', 'src', 'include', 'docs', 'licenses', '.github'):
        path = root / entry
        for file in files(path) if path.is_dir() else [path]:
            archive.write(file, name + '/' + file.relative_to(root).as_posix())
    for source, destination in ((a52, 'a52/source'), (ffmpeg, 'ffmpeg')):
        for file in files(source):
            archive.write(file, name + '/third_party/' + destination + '/' + file.relative_to(source).as_posix())
    archive.write(make, name + '/third_party/make-4.4.1.tar.gz')
    archive.writestr(name + '/BUILD_VERSION', args.version + '\n')
print(output)
