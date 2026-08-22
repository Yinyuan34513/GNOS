"""
gitsub.py - 自动将当前仓库中的 gitlink（mode 160000）转换为正式 git submodule
无参数执行：自动扫描所有 gitlink，获取远程地址并添加为 submodule
支持: github.com, gitlab.com, gitee.com, atomgit.com, gitcode.com, git.*.org
"""

import subprocess
import sys
import os
import shutil
import re
import tempfile


# ============================================================
# 配置
# ============================================================
BACKUP_BASE_DIR = "/home/gittmp"

# 备份时跳过的目录（构建产物、缓存等）
SKIP_DIRS = {
    'target', 'build', 'dist', 'node_modules', 'out', 'obj', 'bin',
    '.next', '__pycache__', '.venv', 'venv', '.gradle', '.idea',
    '.vscode', '.cache', '.pytest_cache', '.mypy_cache',
    '.tox', '.eggs', '*.egg-info', 'vendor', 'Pods',
}

# 已知代理前缀
PROXY_PREFIXES = [
    'https://hk.gh-proxy.org/',
    'https://ghproxy.com/',
    'https://ghproxy.net/',
    'https://mirror.ghproxy.com/',
    'https://hub.gitmirror.com/',
    'https://cors.isteed.cc/',
    'https://kkgithub.com/',
    'https://bgithub.xyz/',
    'https://github.boki.moe/',
    'https://gitclone.com/',
    'https://hub.fastgit.xyz/',
    'https://github.com.cnpmjs.org/',
    'https://hub.njuu.lu/',
    'https://github.moeyy.xyz/',
    'https://gh-proxy.com/',
    'https://ghps.cc/',
    'https://gh.ddlc.top/',
    'https://slink.ltd/',
    'https://hub.yzuu.cf/',
    'https://gitdun.com/',
]

# 支持的代码托管平台域名模式
SUPPORTED_HOSTS_RE = re.compile(
    r'(?:github\.com|gitlab\.com|gitee\.com|atomgit\.com|gitcode\.com|git\.[^/]+\.org)'
)


# ============================================================
# 工具函数
# ============================================================
def run(cmd, cwd=None, check=True):
    """执行命令并返回输出"""
    result = subprocess.run(
        cmd, shell=True, cwd=cwd,
        capture_output=True, text=True
    )
    if check and result.returncode != 0:
        print(f"  ❌ 命令失败: {cmd}")
        print(f"     {result.stderr.strip()}")
        return None
    return result.stdout.strip()


def ensure_backup_dir():
    """确保备份根目录存在"""
    os.makedirs(BACKUP_BASE_DIR, exist_ok=True)


def load_submodules_gitignore():
    """读取 submodules.gitignore，返回需要跳过的 gitlink 路径集合"""
    ignore_file = "submodules.gitignore"
    skipped = set()
    if not os.path.exists(ignore_file):
        return skipped

    with open(ignore_file, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            # 支持 glob 简单匹配
            skipped.add(line)

    if skipped:
        print(f"📄 已加载 {ignore_file}，跳过 {len(skipped)} 个条目:")
        for s in sorted(skipped):
            print(f"   ⏭  {s}")
    return skipped


def should_skip(path, ignore_set):
    """检查路径是否应被跳过（精确匹配或前缀匹配）"""
    if path in ignore_set:
        return True
    # 支持目录前缀: "foo/" 匹配 "foo/bar"
    for pattern in ignore_set:
        if pattern.endswith('/') and path.startswith(pattern):
            return True
        # 简单通配符: "foo*" 匹配 "foobar"
        if '*' in pattern:
            regex = '^' + re.escape(pattern).replace(r'\*', '.*') + '$'
            if re.match(regex, path):
                return True
    return False


# ============================================================
# 代理去除
# ============================================================
def strip_proxy(url):
    """去除代理地址，保留真实仓库地址"""
    if not url:
        return None

    original = url

    # 方法1：精确匹配已知代理前缀
    for prefix in PROXY_PREFIXES:
        if url.startswith(prefix):
            url = url[len(prefix):]
            break

    # 方法2：通用正则 - https://任意代理域名/https://真实地址
    match = re.search(r'https?://[^/]+/(https?://.+)$', url)
    if match:
        candidate = match.group(1)
        # 仅当提取出的地址属于支持的平台时才替换
        if SUPPORTED_HOSTS_RE.search(candidate):
            url = candidate

    # 方法3：兜底 - 从原始 URL 中提取支持平台的地址
    if not SUPPORTED_HOSTS_RE.search(url) and SUPPORTED_HOSTS_RE.search(original):
        match = re.search(
            r'(https?://(?:' + SUPPORTED_HOSTS_RE.pattern + r')/[^/\s]+/[^/\s]+?)(?:\.git)?/?$',
            original
        )
        if match:
            url = match.group(1)
            if not url.endswith('.git'):
                url += '.git'

    # 清理
    while url.startswith('/'):
        url = url[1:]
    if not url.startswith('http'):
        url = 'https://' + url

    if url != original:
        print(f"   🔄 去除代理: {original}")
        print(f"   ✅ 真实地址: {url}")

    return url


# ============================================================
# 核心逻辑
# ============================================================
def find_gitlinks():
    """扫描当前仓库中所有 gitlink（mode 160000）条目"""
    output = run("git ls-files --stage")
    if output is None:
        return []

    gitlinks = []
    for line in output.splitlines():
        parts = line.split('\t', 1)
        if len(parts) == 2:
            meta = parts[0].split()
            path = parts[1]
            if meta[0] == '160000':
                gitlinks.append(path)
    return gitlinks


def get_remote_url(path):
    """从目录内的 .git 获取远程仓库地址，并自动去除代理"""
    url = run("git remote get-url origin", cwd=path, check=False)
    if url:
        return strip_proxy(url)
    return None


def backup_directory(path, backup_path):
    """备份目录，自动跳过构建产物和大文件目录"""
    def ignore_func(directory, contents):
        ignored = set()
        for item in contents:
            if item in SKIP_DIRS and os.path.isdir(os.path.join(directory, item)):
                ignored.add(item)
        return ignored

    if os.path.exists(backup_path):
        shutil.rmtree(backup_path)

    shutil.copytree(path, backup_path, ignore=ignore_func, dirs_exist_ok=True)
    print(f"   💾 已备份（跳过构建目录）-> {backup_path}")


def convert_to_submodule(path, url):
    """将 gitlink 转换为正式 submodule"""
    print(f"\n📦 处理: {path}")
    print(f"   远程地址: {url}")

    # 1. 从索引移除 gitlink
    print("   [1/4] 移除 gitlink...")
    result = run(f'git rm --cached "{path}"', check=False)
    if result is None:
        print("   ⚠️  移除失败，尝试继续...")

    # 2. 备份目录（跳过 target/ 等）
    ensure_backup_dir()
    backup_path = os.path.join(BACKUP_BASE_DIR, f"gitsub_backup_{os.path.basename(path)}")
    print("   [2/4] 备份本地文件...")
    backup_ok = False
    if os.path.exists(path):
        try:
            backup_directory(path, backup_path)
            backup_ok = True
        except Exception as e:
            print(f"   ❌ 备份失败: {e}")
            print("   ⚠️  跳过备份，继续转换（本地修改可能丢失）")
        shutil.rmtree(path)
    else:
        print("   ⚠️  目录不存在，跳过备份")

    # 3. 添加为 submodule
    print("   [3/4] 添加 submodule...")
    result = run(f'git submodule add "{url}" "{path}"')
    if result is None:
        print(f"   ❌ 添加 submodule 失败: {path}")
        if backup_ok and os.path.exists(backup_path):
            print("   ♻️  恢复备份...")
            shutil.copytree(backup_path, path, dirs_exist_ok=True)
        return False

    # 4. 恢复本地修改
    if backup_ok and os.path.exists(backup_path):
        print("   [4/4] 恢复本地修改...")
        for item in os.listdir(backup_path):
            src = os.path.join(backup_path, item)
            dst = os.path.join(path, item)
            if item == '.git':
                continue
            if os.path.isdir(src):
                if not os.path.exists(dst):
                    shutil.copytree(src, dst)
            else:
                if not os.path.exists(dst):
                    shutil.copy2(src, dst)
        shutil.rmtree(backup_path)
    else:
        print("   [4/4] 无需恢复本地文件")

    print(f"   ✅ 完成: {path}")
    return True


# ============================================================
# 命令
# ============================================================
def cmd_add():
    """默认操作：自动添加子模块"""
    print("🔍 扫描 gitlink 条目...")
    gitlinks = find_gitlinks()

    if not gitlinks:
        print("✅ 没有找到需要转换的 gitlink，当前没有子模块需要添加。")
        return

    # 加载忽略列表
    ignore_set = load_submodules_gitignore()

    # 过滤
    filtered = []
    for g in gitlinks:
        if should_skip(g, ignore_set):
            print(f"   ⏭  跳过（gitignore）: {g}")
        else:
            filtered.append(g)

    if not filtered:
        print("✅ 所有 gitlink 均已被 submodules.gitignore 忽略。")
        return

    print(f"\n找到 {len(filtered)} 个 gitlink（共 {len(gitlinks)} 个，忽略 {len(gitlinks) - len(filtered)} 个）:\n")
    for g in filtered:
        print(f"  - {g}")

    # 获取远程地址
    entries = []
    for path in filtered:
        url = get_remote_url(path)
        if url:
            entries.append((path, url))
        else:
            print(f"\n⚠️  无法获取 {path} 的远程地址（目录可能已损坏）")
            entries.append((path, None))

    # 汇总
    print("\n" + "=" * 50)
    print("即将处理:")
    for path, url in entries:
        status = url if url else "❌ 无远程地址"
        print(f"  {path} -> {status}")

    valid = [(p, u) for p, u in entries if u]
    if not valid:
        print("\n❌ 没有可处理的条目，请手动检查。")
        return

    confirm = input(f"\n确认转换 {len(valid)} 个子模块？(y/N): ")
    if confirm.lower() != 'y':
        print("已取消。")
        return

    success = 0
    for path, url in valid:
        if convert_to_submodule(path, url):
            success += 1

    print(f"\n{'=' * 50}")
    print(f"✅ 完成！成功转换 {success}/{len(valid)} 个子模块。")
    print("💡 记得执行: git commit -m '添加子模块'")


def cmd_list():
    """列出所有子模块及其地址"""
    print("📋 当前 gitlink / submodule 列表:\n")

    gitlinks = find_gitlinks()
    if not gitlinks:
        print("  (无)")
        return

    submodules = {}
    if os.path.exists('.gitmodules'):
        output = run("git config --file .gitmodules --get-regexp path")
        if output:
            for line in output.splitlines():
                key, val = line.split(' ', 1)
                name = key.replace('submodule.', '').replace('.path', '')
                submodules[val] = name

    for path in gitlinks:
        name = submodules.get(path, '(未注册)')
        url = get_remote_url(path) or "未知"
        print(f"  📁 {path}")
        print(f"     名称: {name}")
        print(f"     地址: {url}")
        print()


def main():
    if len(sys.argv) > 1:
        arg = sys.argv[1]
        if arg in ('-l', '--list', 'list'):
            cmd_list()
        elif arg in ('-h', '--help', 'help'):
            print("用法:")
            print("  python3 gitsub.py          自动扫描并添加子模块")
            print("  python3 gitsub.py list     查看所有子模块及地址")
            print("  python3 gitsub.py help     显示帮助")
            print()
            print("配置文件:")
            print("  submodules.gitignore       每行一个路径，跳过对应 gitlink")
            print(f"  备份目录: {BACKUP_BASE_DIR}")
        else:
            print(f"未知参数: {arg}")
            print("使用 help 查看帮助")
    else:
        cmd_add()


if __name__ == '__main__':
    main()
