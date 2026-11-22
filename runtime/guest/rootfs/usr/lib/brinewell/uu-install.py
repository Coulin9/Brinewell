#!/usr/bin/env python3
"""Install the official UU payload using Brinewell's private ARM64 runtime."""
import base64
import fcntl
import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
from urllib.parse import urlsplit

RUNTIME = Path('/usr/lib/brinewell/uu-runtime')
HELPERS = Path('/usr/lib/brinewell')
STATE = Path('/home/uu/uu')
PAYLOAD = STATE / 'brinewell-payload'
RUNNING = Path('/tmp/uu')
UNIT = Path('/etc/systemd/system/uuplugin.service')
SERVICE = 'uuplugin.service'
METADATA = 'https://router.uu.163.com/api/plugin?type=steam-deck-plugin-x86_64'
TRUSTED_HOSTS = {'uurouter.gdl.netease.com', 'uurouter-19.gdl.nieapps.com'}
MEMBERS = {'uuplugin', 'xuplugin-guardian', 'uu.conf'}


def emit(kind, value):
    print(kind + ' ' + str(value).replace('\n', ' ').replace('\r', ' '), flush=True)


def progress(percent, message):
    emit('progress', f'{percent} {message}')


def environment():
    env = os.environ.copy()
    env.update(PATH=str(RUNTIME / 'bin') + ':/usr/bin:/usr/sbin:/bin:/sbin',
               LD_LIBRARY_PATH=str(RUNTIME / 'lib'),
               XTABLES_LIBDIR='/run/brinewell-uu-xtables',
               CURL_CA_BUNDLE=str(RUNTIME / 'ca-certificates.crt'),
               HOME=str(STATE), FEX_ROOTFS='/', FEX_DISABLETELEMETRY='1',
               FEX_SERVERSOCKETPATH='brinewell-uu',
               XDG_DATA_HOME=str(STATE / 'data'),
               XDG_CONFIG_HOME=str(STATE / 'config'),
               FEX_APP_CONFIG_LOCATION=str(STATE / 'config'))
    return env


def run(args, timeout=30, check=True, input=None):
    result = subprocess.run(args, text=True, input=input, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=timeout, env=environment())
    if check and result.returncode:
        # Diagnostics are consumed by the root-only guest control log.
        print(result.stderr[-4000:], file=sys.stderr)
        raise RuntimeError('操作失败：' + Path(args[0]).name)
    return result


def binding():
    path = RUNNING / '.steam_deck_sn_qrcode'
    try:
        directory = RUNNING.lstat()
        if not stat.S_ISDIR(directory.st_mode) or directory.st_uid != 0 or directory.st_mode & 0o022:
            return None
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_size > 16384:
            return None
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, encoding='utf-8') as source:
            text = source.read(16385).strip('\n')
    except (OSError, UnicodeError):
        return None
    identifier = re.fullmatch(r'[a-fA-F0-9]{32}', text.strip())
    if identifier:
        return 'binding', identifier.group(0).lower()
    lines = text.splitlines()
    if (10 <= len(lines) <= 90 and all(len(line) <= 180 for line in lines)
            and set(text) <= set(' ▀▄█\n') and any(c in text for c in '▀▄█')):
        return 'qrcode', base64.b64encode(text.encode()).decode()
    return None


def service_state():
    result = run(['systemctl', 'show', SERVICE, '--property=ActiveState,MainPID',
                  '--value'], check=False)
    fields = result.stdout.splitlines()
    pid = next((int(v) for v in fields if v.isdecimal() and int(v) > 1), None)
    if result.returncode:
        return 'stopped'
    if 'activating' in fields:
        return 'starting'
    if 'active' not in fields or pid is None:
        return 'stopped'
    try:
        command = Path(f'/proc/{pid}/cmdline').read_bytes()
    except OSError:
        return 'stopped'
    # A unit installed by an AppleDeck release still execs the launcher through
    # the /usr/lib/appledeck compat symlink; count it as starting too.
    if any(path in command.split(b'\0')[:3] for path in
           (b'/usr/lib/brinewell/uu-launch', b'/usr/lib/appledeck/uu-launch')):
        return 'starting'
    # Existing manual installations use a monitor MainPID. Check their UU child too.
    if b'uuplugin' in command and (b'FEX' in command or b'/tmp/uu/uuplugin' in command):
        return 'running'
    if b'uuplugin_monitor.sh' in command:
        for proc in Path('/proc').iterdir():
            if not proc.name.isdecimal():
                continue
            try:
                child = (proc / 'cmdline').read_bytes()
                if b'/tmp/uu/uuplugin' not in child.split(b'\0')[:2]:
                    continue
                # UU moves itself out of the service cgroup. Its monitor remains
                # the parent, so use that relationship to identify the live child.
                status = (proc / 'status').read_text()
                if re.search(rf'^PPid:\s+{pid}$', status, re.MULTILINE):
                    return 'running'
            except OSError:
                pass
        # The live monitor may still be downloading or restarting its child.
        return 'starting'
    return 'stopped'


def active():
    return service_state() == 'running'


def report_status():
    # Type=simple completes its start job before uu-launch has execed FEX.
    deadline = time.monotonic() + 3
    state = service_state()
    while state == 'starting' and time.monotonic() < deadline:
        time.sleep(0.2)
        state = service_state()
    if state == 'running':
        code = binding()
        emit('status', 'installed UU 已安装并运行' if code else 'installed UU 已运行，正在生成绑定二维码')
        if code:
            emit(*code)
        return True
    if state == 'starting':
        emit('status', 'error UU 正在启动，请稍后刷新')
    elif UNIT.exists() or PAYLOAD.exists():
        emit('status', 'error UU 未正常运行，请点击安装以修复')
    else:
        emit('status', 'absent 尚未安装 UU 加速器插件')
    return False


def prerequisites():
    if os.geteuid() != 0:
        raise RuntimeError('安装服务需要系统权限')
    if platform.machine() not in ('aarch64', 'arm64'):
        raise RuntimeError('此安装功能仅支持 Brinewell 的 ARM64 SteamOS')
    if not Path('/run/systemd/system').is_dir():
        raise RuntimeError('SteamOS 的 systemd 尚未就绪')
    for binary in ('FEX', 'FEXServer', 'curl', 'ip', 'nft', 'xtables-nft-multi'):
        if not os.access(RUNTIME / 'bin' / binary, os.X_OK):
            raise RuntimeError('Brinewell 运行库不完整，请重新安装最新 Brinewell')
    for binary in ('systemctl', 'install', 'cp', 'mkdir'):
        if shutil.which(binary) is None:
            raise RuntimeError('SteamOS 缺少系统命令：' + binary)
    if not Path('/dev/net/tun').exists():
        if shutil.which('modprobe'):
            run(['modprobe', 'tun'], check=False)
        if not Path('/dev/net/tun').exists():
            Path('/dev/net').mkdir(exist_ok=True)
            os.mknod('/dev/net/tun', stat.S_IFCHR | 0o600, os.makedev(10, 200))
    if not stat.S_ISCHR(Path('/dev/net/tun').stat().st_mode):
        raise RuntimeError('SteamOS 的 TUN 网络设备不可用')
    if shutil.disk_usage('/home').free < 64 * 1024 * 1024:
        raise RuntimeError('SteamOS 存储空间不足，需要至少 64 MB')
    # Verify kernel nftables support with a temporary empty, private table.
    table = 'brinewell_uu_check_' + str(os.getpid())
    run([str(RUNTIME / 'bin/nft'), '-f', '-'], input=f'add table inet {table}\ndelete table inet {table}\n')


def download(url, destination, max_bytes):
    run([str(RUNTIME / 'bin/curl'), '--fail', '--silent', '--show-error',
         '--proto', '=https', '--tlsv1.2', '--connect-timeout', '15',
         '--max-time', '120', '--retry', '2', '--max-filesize', str(max_bytes),
         '--header', 'Accept: text/plain', '--output', str(destination), url], timeout=380)
    if destination.stat().st_size > max_bytes:
        raise RuntimeError('UU 下载文件超过预期大小')


def trusted_payload_url(url):
    parsed = urlsplit(url)
    return (parsed.scheme == 'https' and parsed.hostname in TRUSTED_HOSTS
            and not parsed.username and not parsed.password and parsed.port in (None, 443)
            and re.fullmatch(r'/uuplugin/steam-deck-plugin-x86_64/v[0-9.]+/uu\.tar\.gz', parsed.path))


def fetch_payload(directory):
    metadata = directory / 'metadata'
    download(METADATA, metadata, 16384)
    fields = metadata.read_text().strip().split(',')
    if len(fields) < 2 or not re.fullmatch(r'[a-fA-F0-9]{32}', fields[1]):
        raise RuntimeError('UU 官方下载信息格式异常，请稍后重试')
    urls = [u for u in (fields[0], fields[2] if len(fields) > 2 else '') if trusted_payload_url(u)]
    if not urls:
        raise RuntimeError('UU 官方下载地址未通过验证')
    archive = directory / 'uu.tar.gz'
    for index, url in enumerate(urls):
        try:
            download(url, archive, 32 * 1024 * 1024)
            digest = hashlib.md5(archive.read_bytes()).hexdigest()
            if digest.lower() != fields[1].lower():
                raise RuntimeError('UU 安装包校验失败，请重试')
            break
        except (RuntimeError, subprocess.TimeoutExpired):
            if index == len(urls) - 1:
                raise
    extracted = directory / 'payload'
    extracted.mkdir(mode=0o700)
    with tarfile.open(archive, 'r:gz') as package:
        files = package.getmembers()
        names = [member.name.removeprefix('./') for member in files]
        if set(names) != MEMBERS or len(names) != len(MEMBERS):
            raise RuntimeError('UU 安装包内容不符合预期')
        for member, name in zip(files, names):
            if not member.isfile() or member.size > 24 * 1024 * 1024:
                raise RuntimeError('UU 安装包包含无效文件')
            source = package.extractfile(member)
            if source is None:
                raise RuntimeError('UU 安装包无法读取')
            with source, (extracted / name).open('wb') as output:
                shutil.copyfileobj(source, output)
            (extracted / name).chmod(0o644 if name == 'uu.conf' else 0o755)
    for name in ('uuplugin', 'xuplugin-guardian'):
        header = (extracted / name).read_bytes()[:20]
        if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\x3e\x00':
            raise RuntimeError('UU 安装包架构异常')
    return extracted


def owned_directory(path):
    if path.is_symlink():
        raise RuntimeError('UU 数据目录包含符号链接：' + str(path))
    if path.exists():
        info = path.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise RuntimeError('UU 数据目录权限异常：' + str(path))
    else:
        path.mkdir(mode=0o700)


def install():
    # An already running manual installation is preserved, including its binding.
    if active():
        progress(100, 'UU 已安装，无需重复安装')
        report_status()
        return
    progress(5, '正在检查 SteamOS 和运行依赖')
    prerequisites()
    for path in (STATE.parent, STATE, PAYLOAD, STATE / 'data', STATE / 'config', STATE / 'compat', RUNNING):
        owned_directory(path)
    if UNIT.is_symlink():
        raise RuntimeError('现有 UU 服务为链接或已被屏蔽，请检查系统服务配置')
    progress(20, '正在获取 UU 官方安装包')
    with tempfile.TemporaryDirectory(prefix='.brinewell-install-', dir=STATE) as temporary:
        extracted = fetch_payload(Path(temporary))
        run([str(RUNTIME / 'bin/FEX'), str(extracted / 'uuplugin'), '-v'])
        progress(55, '正在配置独立运行库和网络组件')
        if UNIT.exists():
            text = UNIT.read_text()
            # A unit installed by an AppleDeck release still names the old helper path.
            if ('/usr/lib/brinewell/uu-launch' not in text
                    and '/usr/lib/appledeck/uu-launch' not in text
                    and '/home/uu/uu/uuplugin_monitor.sh' not in text):
                raise RuntimeError('检测到其他来源的 UU 服务，请先检查现有安装')
            run(['systemctl', 'stop', SERVICE], timeout=90)
        for name in MEMBERS:
            os.replace(extracted / name, PAYLOAD / name)
        with tempfile.NamedTemporaryFile(prefix='.brinewell-uu-', dir=UNIT.parent, delete=False) as unit_file:
            temporary_unit = Path(unit_file.name)
            unit_file.write((HELPERS / SERVICE).read_bytes())
        temporary_unit.chmod(0o644)
        os.replace(temporary_unit, UNIT)
    run(['systemctl', 'daemon-reload'])
    run(['systemctl', 'reset-failed', SERVICE], check=False)
    progress(70, '正在启动 UU 并验证服务')
    run(['systemctl', 'start', SERVICE], timeout=90)
    start = time.monotonic()
    stable_since = None
    while time.monotonic() - start < 60:
        if active():
            stable_since = stable_since or time.monotonic()
            if binding() and time.monotonic() - stable_since >= 5:
                break
        else:
            stable_since = None
        time.sleep(1)
    if stable_since is None or time.monotonic() - stable_since < 5:
        run(['systemctl', 'stop', SERVICE], timeout=90, check=False)
        raise RuntimeError('UU 启动验证失败，可点击安装重试；详细信息见来宾日志')
    run(['systemctl', 'enable', SERVICE])
    if binding():
        progress(100, 'UU 已安装并开启开机启动，请用手机 UU App 扫码绑定')
    else:
        progress(100, 'UU 已安装并启动，二维码暂未就绪，请稍后刷新')
    report_status()


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ('status', 'install'):
        raise RuntimeError('不支持的 UU 操作')
    if sys.argv[1] == 'status':
        report_status()
        return
    os.umask(0o077)
    descriptor = os.open('/run/brinewell-uu.install.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'w'):
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('UU 正在安装，请稍后刷新')
        install()


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(repr(error), file=sys.stderr)
        emit('status', 'error ' + (str(error) if isinstance(error, RuntimeError) else 'UU 操作失败，请检查网络后重试'))
        sys.exit(1)
