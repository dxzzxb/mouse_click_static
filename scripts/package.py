"""Package only the release application folders, excluding build tools and test data."""
import pathlib
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]
version = (root / 'VERSION').read_text().strip()
for variant in ('portable', 'small'):
    directory = root / 'dist' / ('MouseClickStats-' + version + '-' + variant)
    if not (directory / 'runtime' / 'MouseClickStats.UI.exe').exists():
        continue
    archive = root / 'dist' / ('MouseClickStats-' + version + '-win-x64-' + variant + '.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for file in sorted(directory.rglob('*')):
            if file.is_file():
                output.write(file, file.relative_to(directory.parent))
    print(archive)
    print('ZIP bytes:', archive.stat().st_size)
