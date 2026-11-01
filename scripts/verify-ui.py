"""Verify isolated WinUI data without injected input; final checks exercise window activation."""
import csv
import datetime
import json
import pathlib
import sqlite3
import subprocess
import argparse

ROOT = pathlib.Path(__file__).resolve().parents[1]
VERSION = (ROOT / 'VERSION').read_text().strip()
parser = argparse.ArgumentParser()
parser.add_argument('--exe', default=f'dist/MouseClickStats-{VERSION}-portable/runtime/MouseClickStats.UI.exe')
parser.add_argument('--stress', action='store_true')
parser.add_argument('--slow-read', action='store_true')
parser.add_argument('--preview', action='store_true')
parser.add_argument('--output-dir', help='Verification directory inside build/')
args = parser.parse_args()
directory = (ROOT / args.output_dir).resolve() if args.output_dir else ROOT / 'build' / (f'ui-verification-v{VERSION}' + ('-stress' if args.stress else ''))
if not directory.resolve().is_relative_to((ROOT / 'build').resolve()) or directory.resolve() == (ROOT / 'build').resolve():
    raise RuntimeError('Verification directory must be a child of build/')
directory.mkdir(parents=True, exist_ok=True)
for name in ('report.json', 'error.txt', 'exit-notification.ok'):
    file = directory / name
    if file.exists():
        file.unlink()
for file in directory.glob('[0-9][0-9]-*.png'):
    if not file.resolve().is_relative_to((ROOT / 'build').resolve()):
        raise RuntimeError('Unsafe verification cleanup path')
    file.unlink()
marker = directory / 'slow-read'
if args.slow_read: marker.write_text('350ms background delay', encoding='utf-8')
elif marker.exists(): marker.unlink()
db = sqlite3.connect(directory / 'stats.db')
db.executescript('''
PRAGMA user_version=1;
CREATE TABLE IF NOT EXISTS applications(app_key TEXT PRIMARY KEY,name TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS daily_stats(date TEXT,app_key TEXT,
left_clicks INTEGER,right_clicks INTEGER,middle_clicks INTEGER,x1_clicks INTEGER,x2_clicks INTEGER,
wheel_up INTEGER,wheel_down INTEGER,wheel_left INTEGER,wheel_right INTEGER,
PRIMARY KEY(date,app_key));
DELETE FROM daily_stats;
DELETE FROM applications;
''')
candidates = [pathlib.Path(p) for p in (
    'C:/Program Files/Google/Chrome/Application/chrome.exe',
    'C:/Program Files/Microsoft VS Code/Code.exe', 'E:/steam/steam.exe',
    'C:/Windows/explorer.exe', 'C:/Windows/System32/notepad.exe', 'C:/Windows/System32/cmd.exe')]
executables = [p for p in candidates if p.exists()][:4]
if len(executables) != 4: raise RuntimeError('Four local icon test applications are required')
counts = [[8230,2410,45,120,64], [1250,84,31,15,10], [420,40,6,2,1], [110,11,3,0,0]]
apps = [(str(p),p.name,clicks) for p,clicks in zip(executables,counts)]
apps.append(('shell:taskbar', 'Windows 任务栏', [1320,80,8,0,0]))
today = datetime.date.today()
for key, name, clicks in apps:
    db.execute('INSERT INTO applications VALUES(?,?)', (key, name))
    for ago in range(90):
        factor = 1 if ago == 0 else 0.2 + (ago * 17 % 9) / 10
        counts = [int(value * factor) for value in clicks]
        db.execute('INSERT INTO daily_stats VALUES(?,?,?,?,?,?,?,?,?,?,?)',
                   ((today - datetime.timedelta(days=ago)).isoformat(), key, *counts, 2440, 3600, 0, 120))
if args.stress:
    for i in range(1200):
        key = f'C:\\UI-stress-test\\Application{i:04d}.exe'
        name = f'测试应用 {i:04d}.exe'
        clicks = [i % 19 + 1, i % 3, 0, 0, 0]
        apps.append((key,name,clicks))
        db.execute('INSERT INTO applications VALUES(?,?)', (key,name))
        db.execute('INSERT INTO daily_stats VALUES(?,?,?,?,?,?,?,?,?,?,?)', (today.isoformat(),key,*clicks,0,0,0,0))
db.execute('INSERT INTO daily_stats VALUES(?,?,?,?,?,?,?,?,?,?,?)', ('2001-01-01', apps[0][0], 10, 20, 30, 40, 50, 0, 0, 0, 0))
lifetime_total = db.execute('SELECT SUM(left_clicks+right_clicks+middle_clicks+x1_clicks+x2_clicks) FROM daily_stats').fetchone()[0]
db.commit()
db.close()
exe = (ROOT / args.exe).resolve()
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 4  # SW_SHOWNOACTIVATE, no game focus change.
if args.preview:
    startup.wShowWindow = 1  # Interactive preview must not inherit a hidden shell's startup state.
    subprocess.Popen([str(exe),'--preview',str(directory)],cwd=exe.parent,startupinfo=startup)
    print(directory)
    raise SystemExit(0)
result = subprocess.run([str(exe), '--verify', str(directory)], cwd=exe.parent, startupinfo=startup, timeout=90)
if (directory / 'error.txt').exists():
    raise RuntimeError((directory / 'error.txt').read_text(encoding='utf-8'))
if result.returncode:
    raise RuntimeError('WinUI verification process exited with code ' + str(result.returncode))
report = json.loads((directory / 'report.json').read_text())
assert report['ok'], report
assert report['totalClicks'] == sum(sum(app[2]) for app in apps), report
assert report['applications'] == len(apps) and report['buttonLegendItems'] == 5, report
assert report['emptyDayClicks'] == 0, report
assert report['iconsLoaded'] >= 4, report
assert report['unchangedListResets'] == 0, report
assert report['dragUpdatesDeferred'], report
assert report['liveUpdateDelta'] == 4 and report['liveRowUpdated'], report
assert report['hiddenOverviewRenders'] == 0, report
assert report['permissionStatesVerified'] and report['taskbarCategoryVerified'], report
assert report['recentActivityVerified'] and (directory / 'exit-notification.ok').read_text() == 'received', report
assert report['lifetimeVerified'] and report['lifetimeClicks'] == lifetime_total, report
assert report['speedTestVerified'] and report['immersiveTitleBarVerified'], report
assert report['overviewGroupingVerified'], report
assert report['startupModesVerified'] and report['activationRestoreVerified'], report
assert report['activatedForeground'] and report['crossProcessActivationVerified'] and report['maximizedRestoreVerified'], report
if args.stress:
    assert report['realizedContainers'] < 200, report
if args.slow_read:
    assert report['worstHeartbeatMs'] < 200, report
assert len(list(directory.glob('[0-9][0-9]-*.png'))) == report['screenshots'] == 19, report
with (directory / 'export.csv').open(encoding='utf-8-sig', newline='') as stream:
    rows = list(csv.DictReader(stream))
assert len(rows) == len(apps)
assert sum(sum(int(row[button]) for button in ('left', 'right', 'middle', 'side1', 'side2')) for row in rows) == report['totalClicks'] + report['liveUpdateDelta']
print('PASS: WinUI verification, startup modes, activation restore, overview groups, lifetime counts, click speed test, permissions, incremental updates, minimized exit notification, 19 screenshots and CSV checked')
print(json.dumps(report))
print(directory)
