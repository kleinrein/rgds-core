"""Check panel routing from one process's WAYLAND_DEBUG protocol trace.

Renderer buffers and fullscreen requests alone do not establish visibility.
Both application surfaces must enter their distinct assigned outputs and remain
there until teardown begins. This verifies compositor routing, not LCD pixels.
"""
import re


def panel_placement(log):
    outputs = {}
    surfaces = {}
    toplevels = {}
    panels = {}
    assigned = {}
    visible = {}
    observed = {}
    for line in log.splitlines():
        if m := re.search(r'wl_output[#@](\d+)\.geometry\((-?\d+),\s*(-?\d+)', line):
            outputs[m[1]] = (int(m[2]), int(m[3]))
        elif m := re.search(r'get_xdg_surface\(new id xdg_surface[#@](\d+), wl_surface[#@](\d+)\)', line):
            surfaces[m[1]] = m[2]
        elif m := re.search(r'xdg_surface[#@](\d+)\.get_toplevel\(new id xdg_toplevel[#@](\d+)\)', line):
            toplevels[m[2]] = surfaces.get(m[1])
        elif m := re.search(r'xdg_toplevel[#@](\d+)\.set_app_id\("org\.rgds\.(top|bottom)"\)', line):
            panels[m[2]] = toplevels.get(m[1])
        elif m := re.search(r'xdg_toplevel[#@](\d+)\.set_fullscreen\(wl_output[#@](\d+)\)', line):
            assigned[toplevels.get(m[1])] = m[2]
        elif m := re.search(r'wl_surface[#@](\d+)\.(enter|leave)\(wl_output[#@](\d+)\)', line):
            surface, action, output = m.groups()
            if surface not in panels.values(): continue
            if action == 'enter':
                if assigned.get(surface) != output: raise ValueError('Panel entered a different assigned output')
                visible.setdefault(surface, set()).add(output)
                observed[surface] = output
            else:
                visible.setdefault(surface, set()).discard(output)
                if surface in observed: raise ValueError('Panel became hidden before teardown')
        elif m := re.search(r'wl_surface[#@](\d+)\.destroy\(', line):
            if m[1] in panels.values(): break
    if set(panels) != {'top', 'bottom'} or None in panels.values(): raise ValueError('Both panel surfaces are required')
    result = {}
    for panel, surface in panels.items():
        output = assigned.get(surface)
        if not output or output not in outputs or visible.get(surface) != {output}:
            raise ValueError('Panel is not visible on its assigned output')
        result[panel] = [outputs[output]]
    if result['top'] == result['bottom']: raise ValueError('Both panels share an output')
    return result
