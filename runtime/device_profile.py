"""Read-only device identification; display modes are hints, not firmware approval."""
from pathlib import Path
import re


def detect(sysfs=Path('/sys')):
    sysfs = Path(sysfs)
    model = ''
    for name in ('model', 'compatible'):
        try:
            model += ' ' + (sysfs / 'firmware/devicetree/base' / name).read_text().replace('\0', ' ')
        except OSError:
            pass
    compact = re.sub(r'[^a-z0-9+]', '', model.lower())
    panels = []
    for path in sorted((sysfs / 'class/drm').glob('card*-DSI-*/modes')):
        try:
            if (path.parent / 'status').read_text().strip() != 'connected':
                continue
            modes = path.read_text().splitlines()
            panels.append(modes[0] if modes else '')
        except OSError:
            continue
    if 'rgdsplus' in compact or 'rgds+' in compact:
        device, source = 'rg-ds-plus', 'device-tree'
    elif 'rgds' in compact:
        # Some vendor kernels reuse the original board identifier on the Plus.
        device = 'rg-ds-plus' if panels == ['1024x768', '1024x768'] else 'rg-ds'
        source = 'panel-modes' if device == 'rg-ds-plus' else 'device-tree'
    elif panels == ['1024x768', '1024x768']:
        device, source = 'rg-ds-plus', 'panel-modes'
    else:
        device, source = 'unknown', 'unknown'
    return dict(model=device, source=source, panels=panels)


def stock_environment(env):
    from input_devices import discover
    profile = detect()
    env['RGDS_DEVICE_MODEL'] = profile['model']
    inputs = discover()
    if inputs['touch']:
        env['RGDS_TOUCH_DEV'] = env['NNDDSS_TOUCH_DEV'] = inputs['touch']
    if inputs['keys']:
        env['NNDDSS_KEYS_DEV'] = inputs['keys']
    if profile['model'] == 'rg-ds-plus':
        env['RGDS_TOP_DISPLAY'] = 'DSI-1'
        env['RGDS_BOTTOM_DISPLAY'] = 'DSI-2'
        # The Plus has one lower touchscreen. If firmware exposes multiple
        # candidates, use SDL's routing until a tester verifies the raw node.
        if not inputs['touch']:
            env['RGDS_TOUCH_DEV'] = 'none'
        env['RGDS_INPUT_AMBIGUOUS'] = '1' if (
            not inputs['touch'] or not inputs['keys'] or inputs['touch_ambiguous'] or inputs['keys_ambiguous']) else '0'
    return env
