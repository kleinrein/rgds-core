"""Resolve console input nodes from sysfs capabilities, never event-number guesses."""
from pathlib import Path
import struct


def bits(path, word_bits=struct.calcsize('L') * 8):
    """Kernel sysfs prints unpadded native-long words, highest word first."""
    try:
        value = 0
        for word in path.read_text().split():
            part = int(word, 16)
            if part < 0 or part >= 1 << word_bits:
                return 0
            value = (value << word_bits) | part
        return value
    except (OSError, ValueError):
        return 0


def discover(sysfs=Path('/sys/class/input'), devices=Path('/dev/input'), bottom_hint=''):
    touch=[];pads=[];lids=[]
    for entry in sorted(Path(sysfs).glob('event*')):
        node=Path(devices)/entry.name
        if not node.exists():continue
        device=entry/'device'
        try:name=(device/'name').read_text().strip()
        except OSError:continue
        absolute=bits(device/'capabilities/abs');keys=bits(device/'capabilities/key')
        switches=bits(device/'capabilities/sw')
        properties=bits(device/'properties')
        direct=bool(properties & (1<<1))
        xy=bool(absolute & 1 and absolute & 2)
        mt=bool(absolute & (1<<53) and absolute & (1<<54))
        # Older vendor touchscreen drivers can omit INPUT_PROP_DIRECT. A
        # BTN_TOUCH digitizer without BTN_TOOL_FINGER is still a touchscreen;
        # touchpads advertising that tool must not become the lower panel.
        touchscreen = direct or bool(keys & (1<<330) and not keys & (1<<325))
        if touchscreen and (xy or mt):
            touch.append((str(node),str(device.resolve()),name))
        console_pad = 'retrogame_joypad' in name or 'ANBERNIC-rk3568' in name or name=='gpio-keys-control'
        if (keys & (1<<304)) and (xy or console_pad or 'joypad' in name.lower() or 'gamepad' in name.lower()):
            pads.append((str(node),name))
        if switches&1:
            wake=None
            for parent in (device.resolve(),*device.resolve().parents):
                candidate=parent/'power/wakeup'
                if candidate.is_file():wake=str(candidate);break
            if wake:lids.append((str(node),wake))
    matched=[node for node,path,name in touch if bottom_hint and bottom_hint in path]
    selected_touch=matched[0] if len(matched)==1 else touch[0][0] if len(touch)==1 else None
    console=[node for node,name in pads if 'retrogame_joypad' in name or 'ANBERNIC-rk3568' in name or name=='gpio-keys-control']
    selected_pad=console[0] if len(console)==1 else pads[0][0] if len(pads)==1 else None
    return {'touch':selected_touch,'keys':selected_pad,
            'profile':'mainline-rg-ds' if any(node==selected_pad and name=='gpio-keys-control' for node,name in pads) else 'stock',
            'lid':lids[0][0] if len(lids)==1 else None,'lid_wakeup':lids[0][1] if len(lids)==1 else None,
            'touch_ambiguous':len(touch)>1 and selected_touch is None,'keys_ambiguous':len(pads)>1 and selected_pad is None}
