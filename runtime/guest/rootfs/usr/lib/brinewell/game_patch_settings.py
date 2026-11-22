"""Default game launch patches, independent of the selected Proton tool."""

import json
import re
import shlex
import sys

from game_patches import PATCHES
import brinewell_vdf


LAUNCHER = "/usr/lib/brinewell/brinewell-game-launch"
# AppleDeck releases recorded the managed wrapper under its old path; a
# recorded/localconfig value with it is still the managed wrapper (resolved via
# the /usr/lib/appledeck compat symlink until the next sync rewrites it).
LEGACY_LAUNCHER = "/usr/lib/appledeck/appledeck-game-launch"
APP_PATH = ("UserLocalConfigStore", "Software", "Valve", "Steam", "apps")


def settings_path(state):
    return state.parent / "game-patches.json"


def load_settings(state):
    path = settings_path(state)
    if not path.exists():
        return {"schema": 2, "accounts": {}}
    data = json.loads(path.read_text())
    if (not isinstance(data, dict) or data.get("schema") not in (1, 2)
            or not isinstance(data.get("accounts"), dict)):
        raise ValueError("游戏专项补丁配置无效。")
    for account, records in data["accounts"].items():
        if not account.isdecimal() or not isinstance(records, dict):
            raise ValueError("游戏专项补丁账号记录无效。")
        for appid, record in records.items():
            if appid not in PATCHES or not isinstance(record, dict):
                raise ValueError("游戏专项补丁启动记录无效。")
            if any(record.get(key) is not None and not isinstance(record[key], str)
                   for key in ("previous", "applied", "priorApplied")):
                raise ValueError("游戏专项补丁启动选项无效。")
    # The previous schema stored optional enable switches. All supported
    # profiles are now defaults; retain only launch-option ownership records.
    return {"schema": 2, "accounts": data["accounts"]}


def save_settings(state, data):
    brinewell_vdf.atomic_write(settings_path(state), json.dumps(data, ensure_ascii=False, indent=2) + "\n")


def launch_options(appid, previous):
    prefix = shlex.join([LAUNCHER, "--profile", PATCHES[appid], "--"])
    if not previous:
        return prefix + " %command%"
    # Keep the user's command bytes and shell ordering. Complex shell programs
    # require the user to place the explicit launcher at the intended command.
    if any(char in previous for char in "\n\r`$;&|<>()"):
        raise ValueError("现有启动选项包含 shell 表达式，请手动添加专项补丁启动器。")
    try:
        arguments = shlex.split(previous)
    except ValueError as error:
        raise ValueError("现有启动选项无法解析，请手动添加专项补丁启动器。") from error
    if LAUNCHER in previous or LEGACY_LAUNCHER in previous:
        raise ValueError("现有启动选项已包含专项补丁启动器，请手动管理。")
    if "%command%" not in previous:
        return prefix + " %command% " + previous
    matches = list(re.finditer(r"(?<!\S)%command%(?!\S)", previous))
    if (len(matches) != 1 or previous.count("%command%") != 1
            or arguments.count("%command%") != 1):
        raise ValueError("现有启动选项的 %command% 用法需要手动处理。")
    position = matches[0].start()
    return previous[:position] + prefix + " " + previous[position:]


def synchronize(steam, state):
    data = load_settings(state)
    for config in sorted((steam / "userdata").glob("[0-9]*/config/localconfig.vdf")):
        account = config.parent.parent.name
        if not account.isdecimal():
            continue
        try:
            original = config.read_text()
            values, blocks, _ = brinewell_vdf.vdf_entries(original)
            if (APP_PATH[0].lower(),) not in blocks:
                raise ValueError("missing UserLocalConfigStore block")
        except (OSError, ValueError) as error:
            print(f"brinewell-game-patches: account={account}: {error}", file=sys.stderr)
            continue
        text = original
        records = data["accounts"].setdefault(account, {})
        for appid in sorted(PATCHES):
            path = (*APP_PATH, appid, "LaunchOptions")
            entry = values.get(tuple(part.lower() for part in path))
            current = entry[0] if entry else None
            record = records.get(appid)
            # Retry window: the ownership record was persisted but the vdf
            # write stopped, so localconfig still holds the value we observed
            # and were replacing when the write stopped (priorApplied) or the
            # pre-ownership options (previous). A MISSING priorApplied key
            # resumes only from previous; an explicit empty value is a real
            # observation (empty launch options), so a user clearing the field
            # later is only detached, never mistaken for that observation.
            pending = False
            if record and record.get("pending"):
                resumable = [record.get("previous")]
                if "priorApplied" in record:
                    resumable.append(record["priorApplied"])
                pending = current in resumable
            if record and current != record.get("applied") and not pending:
                record["detached"] = True
            if record and record.get("detached"):
                continue
            previous = record.get("previous") if record else current
            try:
                applied = launch_options(appid, previous)
            except ValueError as error:
                print(f"brinewell-game-patches: appid={appid} account={account}: {error}", file=sys.stderr)
                continue
            # priorApplied is the value observed in localconfig that this write
            # replaces; it is kept until the vdf write below succeeds so a
            # stopped write can resume from exactly that state.
            records[appid] = {"previous": previous, "applied": applied, "pending": True}
            if current is not None:
                records[appid]["priorApplied"] = current
            text = brinewell_vdf.vdf_set(text, path, applied)
        # Persist ownership before publishing options, so a stopped write can
        # resume from either the previous or the applied value.
        save_settings(state, data)
        if text != original:
            brinewell_vdf.atomic_write(config, text)
        for record in records.values():
            record.pop("pending", None)
            record.pop("priorApplied", None)
        if not records:
            data["accounts"].pop(account, None)
        save_settings(state, data)
