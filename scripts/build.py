"""Build with Microsoft's project-local tools; bootstrap.py supplies verified dependencies."""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]
PACKAGES = ROOT / 'packages'
TOOLS = ROOT / '.tools'
BUILD = ROOT / 'build' / 'msvc'
def run(arguments, env, cwd=BUILD):
    subprocess.run([str(x) for x in arguments], cwd=cwd, env=env, check=True)


def first(pattern, root=PACKAGES):
    matches = sorted(root.glob(pattern))
    if not matches:
        raise RuntimeError('Build dependency is missing: ' + pattern)
    return matches[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--framework-dependent', action='store_true')
    parser.add_argument('--skip-tests', action='store_true')
    parser.add_argument('--output-dir')
    args = parser.parse_args()
    versions = json.loads((TOOLS / 'dependencies.json').read_text())
    locked = versions.get('packages', {})
    sdk_packages = sorted(PACKAGES / (name + '.' + version) for name, version in locked.items() if name.lower().startswith('microsoft.windowsappsdk')) if locked else sorted(p for p in PACKAGES.glob('Microsoft.WindowsAppSDK.*') if p.name.endswith('.' + versions['windowsAppSdk']))
    components = [p for p in sdk_packages if not p.name.startswith('Microsoft.WindowsAppSDK.Runtime.')]
    BUILD.mkdir(parents=True, exist_ok=True)
    version = (ROOT / 'VERSION').read_text().strip()
    out = (ROOT / args.output_dir).resolve() if args.output_dir else ROOT / 'dist' / ('MouseClickStats-' + version + ('-small' if args.framework_dependent else '-portable'))
    if not out.is_relative_to(ROOT):
        raise RuntimeError('Output directory must stay within the project')
    out.mkdir(parents=True, exist_ok=True)
    runtime_out = out / "runtime"
    runtime_out.mkdir(exist_ok=True)
    msvc = first('msvc/VC/Tools/MSVC/*', TOOLS)
    sdk = PACKAGES / ('Microsoft.Windows.SDK.CPP.' + versions['sdk'])
    sdkx64 = PACKAGES / ('Microsoft.Windows.SDK.CPP.x64.' + versions['sdk'])
    include_root = first('c/Include/*', sdk)
    library_root = sdkx64 / 'c'
    rc = first('Microsoft.Windows.SDK.BuildTools.*/bin/*/x64/rc.exe')
    cppwinrt = PACKAGES / ('Microsoft.Windows.CppWinRT.' + versions['cppwinrt']) / 'bin/cppwinrt.exe'
    compiler = msvc / 'bin' / 'Hostx64' / 'x64' / 'cl.exe'
    linker = compiler.with_name('link.exe')
    env = os.environ.copy()
    env['PATH'] = str(compiler.parent) + os.pathsep + str(rc.parent) + os.pathsep + env.get('PATH', '')
    include_dirs = [msvc / 'include'] + [include_root / item for item in ('ucrt', 'shared', 'um', 'winrt')]
    env['INCLUDE'] = ';'.join(str(p) for p in include_dirs)
    env['LIB'] = ';'.join(str(p) for p in (msvc / 'lib/x64', library_root / 'ucrt/x64', library_root / 'um/x64'))

    # Generate matching Windows and Windows App SDK C++/WinRT projections together.
    generated = BUILD / ('generated-' + versions['windowsAppSdk'])
    stamp = generated / '.version'
    stamp_value = json.dumps(versions, sort_keys=True)
    if not stamp.exists() or stamp.read_text() != stamp_value:
        print('Generating C++/WinRT projections…', flush=True)
        windows_metadata = sdk / 'c/UnionMetadata' / include_root.name / 'Windows.winmd'
        metadata = {}
        webview = [PACKAGES / (name + '.' + version) for name, version in locked.items() if name.lower() == 'microsoft.web.webview2'] if locked else sorted(PACKAGES.glob('Microsoft.Web.WebView2.*'))
        metadata_roots = [*components, *webview]
        for package in metadata_roots:
            for file in package.rglob('*.winmd'):
                if '10.0.17763.0' in file.parts:
                    continue
                if file.parts[len(PACKAGES.parts)].startswith('Microsoft.Web.WebView2.') and file.name != 'Microsoft.Web.WebView2.Core.winmd':
                    continue
                metadata.setdefault(file.name, file)
        command = [cppwinrt, '-in', windows_metadata, '-out', generated, '-base']
        for file in metadata.values():
            command += ['-in', file]
        run(command, env)
        stamp.write_text(stamp_value)
    winui_includes = []
    for package in sdk_packages:
        for folder in (package / 'include', package / 'build/native/include'):
            if folder.is_dir():
                winui_includes.append(folder)
    includes = ['/I' + str(ROOT / 'src'), '/I' + str(generated)] + ['/I' + str(p) for p in winui_includes]
    flags = ['/nologo', '/c', '/std:c++17', '/EHsc', '/utf-8', '/O1', '/Gy', '/Gw', '/MT', '/W4', '/permissive-',
             '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_WIN32_WINNT=0x0A00']

    def compile(source, extra=()):
        source = ROOT / source
        suffix = '-framework' if extra else ''
        obj = BUILD / (source.parent.name + '-' + source.stem + suffix + '.obj')
        inputs = [source, pathlib.Path(__file__), stamp, *list((ROOT / 'src').rglob('*.h'))]
        if obj.exists() and obj.stat().st_mtime >= max(p.stat().st_mtime for p in inputs):
            return obj
        print('Compiling', source.relative_to(ROOT), flush=True)
        compile_flags = [('/std:c++20' if flag == '/std:c++17' else flag) for flag in flags] if source.parent.name == 'ui' else flags
        if source.parent.name == 'ui':
            compile_flags += ['/Zc:char8_t-']
        run([compiler, *compile_flags, *includes, *extra, '/Fo' + str(obj), source], env)
        return obj

    core = [compile('src/' + source) for source in ('core/counters.cpp', 'core/input_merger.cpp', 'core/dashboard_model.cpp', 'platform/windows_helpers.cpp', 'platform/privileges.cpp', 'platform/startup.cpp', 'storage/sqlite_store.cpp')]
    worker = compile('src/collector/persistence.cpp')
    common_libs = ['user32.lib', 'shell32.lib', 'advapi32.lib', 'ole32.lib', 'oleaut32.lib', 'uuid.lib', 'taskschd.lib']

    def resource(source, target, defines=()):
        source = ROOT / source
        run([rc, '/nologo', '/c65001', *defines, '/fo' + str(target), source.name], env, source.parent)
        return target

    def link(name, objects, gui=True, extra=(), directory=None):
        target = (directory or out) / name
        run([linker, '/NOLOGO', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF', '/MANIFEST:NO',
             '/SUBSYSTEM:' + ('WINDOWS' if gui else 'CONSOLE'), '/OUT:' + str(target),
             *objects, *common_libs, *extra], env)
        return target

    collector_res = resource('src/collector/collector.rc', BUILD / 'collector.res')
    collector = link('MouseClickStats.exe', [*core, worker, compile('src/collector/main.cpp'), collector_res])

    # Match the app-local manifests/binaries selected by the SDK's native deployment targets.
    ui_manifest = None
    if not args.framework_dependent:
        runtime_roots = []
        for package in components:
            for pattern in ('runtimes-framework/win-x64/native',):
                directory = package / pattern
                if directory.is_dir():
                    runtime_roots.append(directory)
        for directory in runtime_roots:
            for file in directory.rglob('*'):
                if file.is_file() and file.suffix.lower() not in {'.pdb', '.lib', '.exp', '.xml', '.winmd'}:
                    relative = file.relative_to(directory)
                    destination = runtime_out / relative
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(file, destination)
        # Reproduce the component InProcessServer/ProxyStub activation registrations
        # from Microsoft's WindowsAppSDK.SelfContained.targets for Windows 10 1903+.
        ns = 'urn:schemas-microsoft-com:asm.v1'
        asm3 = 'urn:schemas-microsoft-com:asm.v3'
        winrt = 'urn:schemas-microsoft-com:winrt.v1'
        ET.register_namespace('', ns)
        ET.register_namespace('asmv3', asm3)
        ET.register_namespace('winrtv1', winrt)
        manifest = ET.parse(ROOT / 'assets/app.manifest')
        registrations = {}
        for fragment in (p / 'runtimes-framework/package.appxfragment' for p in components):
            if not fragment.exists():
                continue
            component = ET.parse(fragment).getroot()
            appx = {'m': 'http://schemas.microsoft.com/appx/manifest/foundation/windows10'}
            for server in component.findall('./m:Extensions/m:Extension/m:InProcessServer', appx):
                dll = server.find('m:Path', appx).text
                entry = registrations.setdefault(dll, ET.Element('{' + asm3 + '}file', {'name': dll}))
                for cls in server.findall('m:ActivatableClass', appx):
                    ET.SubElement(entry, '{' + winrt + '}activatableClass', {'name': cls.attrib['ActivatableClassId'], 'threadingModel': 'both'})
            for proxy in component.findall('./m:Extensions/m:Extension/m:ProxyStub', appx):
                dll = proxy.find('m:Path', appx).text
                if dll in {'PushNotificationsLongRunningTask.ProxyStub.dll', 'Microsoft.Windows.Widgets.dll'}:
                    continue
                entry = registrations.setdefault(dll, ET.Element('{' + asm3 + '}file', {'name': dll}))
                ET.SubElement(entry, '{' + asm3 + '}comClass', {'clsid': '{' + proxy.attrib['ClassId'] + '}'})
                for interface in proxy.findall('m:Interface', appx):
                    ET.SubElement(entry, '{' + asm3 + '}comInterfaceProxyStub', {'name': interface.attrib['Name'], 'iid': '{' + interface.attrib['InterfaceId'] + '}'})
        if not registrations:
            raise RuntimeError('SDK app-local activation fragments were not found')
        for entry in registrations.values():
            manifest.getroot().append(entry)
        for package in components:
            for pattern in ('metadata/*.winmd', 'metadata/10.0.18362.0/*.winmd'):
                for file in package.glob(pattern):
                    shutil.copy2(file, runtime_out / file.name)
            for name in ('license.txt', 'NOTICE.txt', 'ThirdPartyNotices.txt'):
                file = package / name
                if file.exists():
                    destination = runtime_out / 'licenses' / (package.name + '-' + name)
                    destination.parent.mkdir(exist_ok=True)
                    shutil.copy2(file, destination)
        # WinUI's native DLLs use the dynamic VC runtime even though our own EXEs use /MT.
        redist = first('msvc/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT', TOOLS)
        for pattern in ('msvcp140*.dll', 'vcruntime140*.dll', 'concrt140.dll'):
            for file in redist.glob(pattern):
                shutil.copy2(file, runtime_out / file.name)
        for file in PACKAGES.glob('Microsoft.Web.WebView2.*/build/native/x64/WebView2Loader.dll'):
            shutil.copy2(file, runtime_out / file.name)
        ui_manifest = BUILD / 'ui.manifest'
        manifest.write(ui_manifest, encoding='utf-8', xml_declaration=True)
        # Index app-local XBF/theme files and merge component PRI maps into resources.pri.
        # Do not index a previously generated resources.pri on incremental builds.
        pri = runtime_out / 'resources.pri'
        if pri.exists():
            pri.unlink()
        pri_input = BUILD / 'pri-input'
        pri_input.mkdir(exist_ok=True)
        for file in runtime_out.glob('Microsoft*.pri'):
            shutil.copy2(file, pri_input / file.name)
        makepri = rc.with_name('makepri.exe')
        result = subprocess.run([str(makepri), 'new', '/pr', str(pri_input), '/cf', str(ROOT / 'assets/priconfig.xml'),
                                 '/of', str(pri), '/o'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = result.stdout.decode('utf-16-le', errors='replace')
        print('\n'.join(line for line in output.splitlines() if line.strip() and not line.startswith('Language Qualifiers:')), flush=True)
        if result.returncode:
            raise subprocess.CalledProcessError(result.returncode, 'makepri')
        ui_res = resource('src/ui/ui.rc', BUILD / 'ui.res')
    else:
        ui_res = resource('src/ui/ui.rc', BUILD / 'ui.res')
    extra_flags = ['/DMOUSE_FRAMEWORK_DEPENDENT=1'] if args.framework_dependent else []
    ui_objects = [*core, compile('src/ui/main.cpp', extra_flags), compile('src/ui/dashboard.cpp'), ui_res]
    ui_libs = ['windowsapp.lib', 'runtimeobject.lib', 'comctl32.lib', 'gdi32.lib', 'version.lib', 'dwmapi.lib']
    if args.framework_dependent:
        bootstrap = next(file for package in sdk_packages for file in package.glob('lib/native/x64/Microsoft.WindowsAppRuntime.Bootstrap.lib'))
        ui_libs.append(str(bootstrap))
        dll = next(file for package in sdk_packages for file in package.glob('runtimes/win-x64/native/Microsoft.WindowsAppRuntime.Bootstrap.dll'))
        shutil.copy2(dll, runtime_out / dll.name)
    ui = link('MouseClickStats.UI.exe', ui_objects, extra=ui_libs, directory=runtime_out)
    if ui_manifest:
        run([rc.with_name('mt.exe'), '-nologo', '-manifest', ui_manifest, '-outputresource:' + str(ui) + ';#1'], env)
    if not args.skip_tests:
        tests = link('mouse_tests.exe', [*core, worker, compile('tests/core_tests.cpp')], gui=False)
        print('Running counter/storage tests…', flush=True)
        run([tests], env)
        print('Running isolated collector smoke test…', flush=True)
        subprocess.run([str(collector), '--smoke-test', str(BUILD / 'smoke.db')], cwd=BUILD, env=env, timeout=15, check=True)
        # Keep test executables out of the final application directory.
        tests.replace(BUILD / tests.name)
    shutil.copy2(ROOT / 'README.md', out / 'README.md')
    if (ROOT / 'docs').is_dir():
        shutil.copytree(ROOT / 'docs', out / 'docs', dirs_exist_ok=True)
    print('Build complete:', out, flush=True)
    print('Collector:', collector.stat().st_size, 'bytes; WinUI executable:', ui.stat().st_size, 'bytes', flush=True)


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode)

