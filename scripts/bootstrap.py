"""Project-local Microsoft C++/Windows SDK bootstrap. No installer or registry writes.

All packages come from Microsoft's VS manifest or the official NuGet feed.
VS payload hashes and NuGet SHA-512 hashes are checked before extraction.
"""
import base64
import concurrent.futures
import hashlib
import gzip
import json
import pathlib
import shutil
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
import time
import argparse

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS = ROOT / '.tools'
TOOLS.mkdir(exist_ok=True)


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as response:
        data = response.read()
        return gzip.decompress(data) if response.headers.get('Content-Encoding') == 'gzip' else data


def extract(archive, destination, vs=False):
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as package:
        for item in package.infolist():
            name = item.filename.replace('\\', '/')
            if vs:
                if not name.startswith('Contents/'):
                    continue
                name = name[len('Contents/'):]
            target = (destination / name).resolve()
            if not target.is_relative_to(destination.resolve()):
                raise RuntimeError('Unsafe archive member: ' + name)
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with package.open(item) as source, target.open('wb') as output:
                    shutil.copyfileobj(source, output)


def download(url, path, digest=None, algorithm='sha256'):
    if path.exists() and (not digest or hashlib.new(algorithm, path.read_bytes()).digest() == digest):
        return
    with urllib.request.urlopen(urllib.request.Request(url, method='HEAD'), timeout=45) as response:
        size = int(response.headers['Content-Length'])
    parts = TOOLS / (path.name + '.parts')
    parts.mkdir(exist_ok=True)
    chunk = 4 * 1024 * 1024
    def part(index):
        start, end = index * chunk, min(size - 1, (index + 1) * chunk - 1)
        output = parts / str(index)
        if output.exists() and output.stat().st_size == end - start + 1:
            return output
        for attempt in range(4):
            try:
                request = urllib.request.Request(url, headers={'Range': f'bytes={start}-{end}', 'Accept-Encoding': 'identity'})
                with urllib.request.urlopen(request, timeout=75) as response:
                    data = response.read()
                    if len(data) != end - start + 1:
                        raise RuntimeError('Incomplete range download')
                output.write_bytes(data)
                return output
            except Exception:
                if attempt == 3:
                    raise
                time.sleep(1)
    indexes = list(range((size + chunk - 1) // chunk))
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(part, indexes))
    temporary = path.with_suffix('.download')
    hash_value = hashlib.new(algorithm)
    with temporary.open('wb') as target:
        for index in indexes:
            data = (parts / str(index)).read_bytes()
            target.write(data)
            hash_value.update(data)
    if digest and hash_value.digest() != digest:
        raise RuntimeError('Package hash mismatch: ' + path.name)
    temporary.replace(path)
    if not parts.resolve().is_relative_to(TOOLS.resolve()):
        raise RuntimeError('Refusing cleanup outside project tool cache')
    shutil.rmtree(parts)


def compiler():
    cached = TOOLS / 'vs-catalog.json'
    if not cached.exists():
        channel = json.loads(fetch('https://aka.ms/vs/17/release/channel'))
        manifest = next(x for x in channel['channelItems'] if x['id'] == 'Microsoft.VisualStudio.Manifests.VisualStudio')
        cached.write_bytes(fetch(manifest['payloads'][0]['url']))
    catalog = json.loads(cached.read_text(encoding='utf-8'))
    base = 'Microsoft.VC.14.44.17.14.'
    wanted = [base + suffix for suffix in (
        'Tools.HostX64.TargetX64.base', 'Tools.HostX64.TargetX64.Res.base',
        'CRT.Headers.base', 'CRT.x64.Desktop.base', 'CRT.x64.Store.base', 'CRT.Redist.X64.base')]
    for ident in wanted:
        candidates = [p for p in catalog['packages'] if p['id'] == ident and p.get('language', 'en-US') == 'en-US']
        package = candidates[-1]
        for payload in package['payloads']:
            archive = TOOLS / payload['fileName']
            stamp = TOOLS / (archive.name + '.extracted')
            if stamp.exists():
                continue
            print('Downloading compiler:', archive.name, flush=True)
            download(payload['url'], archive, bytes.fromhex(payload['sha256']))
            extract(archive, TOOLS / 'msvc', vs=True)
            stamp.touch()


seen = set()
sdk_pins = {}
restored_packages = {}


def nuget(name, version, dependencies=True):
    version = sdk_pins.get(name.lower(), version)
    restored_packages[name] = version
    key = (name.lower(), version.lower())
    if key in seen:
        return
    seen.add(key)
    destination = ROOT / 'packages' / (name + '.' + version)
    stamp = destination / '.restored'
    base = 'https://api.nuget.org/v3-flatcontainer/' + name.lower() + '/' + version.lower() + '/' + name.lower() + '.' + version.lower() + '.nupkg'
    if not stamp.exists():
        archive = TOOLS / (name + '.' + version + '.nupkg')
        print('Downloading NuGet:', name, version, flush=True)
        leaf = json.loads(fetch('https://api.nuget.org/v3/registration5-gz-semver2/' + name.lower() + '/' + version.lower() + '.json'))
        entry = leaf['catalogEntry']
        if isinstance(entry, str):
            entry = json.loads(fetch(entry))
        digest = base64.b64decode(entry['packageHash'])
        download(base, archive, digest, 'sha512')
        extract(archive, destination)
        stamp.touch()
    if dependencies:
        namespace = {'n': 'http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd'}
        specification = ET.parse(next(destination.glob('*.nuspec'))).getroot()
        # SDK direct dependencies win over transitive lower bounds (some aren't published).
        if name.lower() == 'microsoft.windowsappsdk':
            for node in specification.iter():
                if node.tag.rsplit('}', 1)[-1] == 'dependency':
                    sdk_pins[node.attrib['id'].lower()] = node.attrib['version'].strip('[]()').split(',')[0].strip()
        # Nuspec namespaces differ between packagers; local-name matching handles both.
        for node in specification.iter():
            if node.tag.rsplit('}', 1)[-1] == 'dependency':
                dependency = node.attrib['id']
                if name.lower() == 'microsoft.windowsappsdk' and dependency.rsplit('.', 1)[-1] not in {'Base', 'Foundation', 'InteractiveExperiences', 'WinUI', 'DWrite'}:
                    continue
                required = node.attrib['version'].strip('[]()').split(',')[0].strip()
                if not required:
                    raise RuntimeError('Dependency has no pinned lower bound: ' + dependency)
                nuget(dependency, required)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--framework-dependent', action='store_true')
    parser.add_argument('--windows-app-sdk', default='2.5.1', help='Pinned runtime; 2.6.4 has a reported window-dragging regression')
    options = parser.parse_args()
    compiler()
    sdk = '10.0.26100.9457'
    nuget('Microsoft.Windows.SDK.CPP', sdk, dependencies=False)
    nuget('Microsoft.Windows.SDK.CPP.x64', sdk, dependencies=False)
    nuget('Microsoft.Windows.CppWinRT', '3.0.260818.1', dependencies=False)
    nuget('Microsoft.WindowsAppSDK', options.windows_app_sdk)
    if options.framework_dependent:
        nuget('Microsoft.WindowsAppSDK.Runtime', options.windows_app_sdk, dependencies=False)
    (TOOLS / 'dependencies.json').write_text(json.dumps({'sdk': sdk, 'cppwinrt': '3.0.260818.1', 'windowsAppSdk': options.windows_app_sdk, 'packages': restored_packages}, indent=2))
    print('Project-local toolchain is ready.', flush=True)
