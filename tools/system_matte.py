"""The matte of a studio product photo: the machine, on transparency.

Every picture in artwork/systems/ is a studio product photo. The
generator was asked for a transparent background and painted one instead - a
smooth sweep, or a checkerboard of two flat tones - so the backdrop has to be
taken away again before the launcher can draw the machine on its own dark
theme.

Two things make this hard, and both were learned the hard way from the
pictures themselves.

The first is the model. The sharp segmentation models punch holes through
pale panels: a Game Boy Advance loses the middle of its screen, a Coleco
Vision loses its keypad. The blunt one keeps whole machines but believes a
desk is part of the subject. So the silhouette is taken from the model that
keeps whole machines, and only the parts of the machine that were lost are
put back afterwards.

The second is telling a lost part of the machine from a real opening in it. A
blank screen is much the same colour as the sweep behind it, so the model
cannot tell them apart, and it sometimes reads the screen as studio and
punches a hole through the hardware. The repair is therefore geometric where
geometry decides and chromatic where colour decides:

* The parts of the machine the model lost are found by closing the
  confident silhouette: whatever the closing fills in belongs to the machine.
* What shows through such a missing part decides what it is. A real opening
  - the gap between a joystick and a panel, the space inside a coil - shows
  the studio. A part of the machine - the lens of a screen, the casing of a
  mouse - does not. The missing part is closed in the second case and left
  open in the first, and it is closed with the pixels of the photograph,
  which are the true surface of the machine.
* A fringe of milk along the outline is studio that the model kept, so it is
  dropped. The pixels the model was sure about are never dropped.
* A scrap of studio floating beside the machine is dropped as well.

The result is a cut-out with the machine whole and the studio gone.
"""
from pathlib import Path
import math

import numpy as np
from PIL import Image, ImageFilter
from scipy import ndimage

# The working size of the matte. The launcher draws these small, and the
# segmentation model is quickest on a picture of about this size.
WORK_SIZE = 1024
# The segmentation models to use. Neither one is right on its own: the sharp
# model punches holes through pale panels, the blunt one leaves a sliver of the
# backdrop standing along the edge of the machine. So both are run, and a pixel
# is the machine only when the two of them agree.
SEGMENT_MODEL = 'birefnet-general'
SEGMENT_MODEL_SECOND = 'u2net'
# Where the model must be sure before a pixel is trusted against the studio.
TRUSTED_ALPHA = 235
# The lowest alpha that still counts as the machine at all.
MIN_ALPHA = 96
# How far a pixel may sit from the modelled studio and still be called studio.
STUDIO_TOLERANCE = 18
# A tighter tolerance for the inside of a missing part, where the difference
# between the studio and a pale part of the machine is a few units of colour.
HOLE_TOLERANCE = 10
# How much of a missing part has to be studio before it is a real opening.
HOLE_STUDIO_SHARE = 0.5
# The border band of the picture, where the studio always is.
RING = 0.06
# A scrap smaller than this share of the machine is debris.
DEBRIS_SHARE = 0.02
# A missing part smaller than this is noise, not a part of the machine.
MIN_PART = 16


def _luma(pixels):
    return 0.299 * pixels[..., 0] + 0.587 * pixels[..., 1] + 0.114 * pixels[..., 2]


def _ring_mask(size):
    """The border band of a picture, where the studio always is."""
    width, height = size
    band = max(2, int(min(width, height) * RING))
    mask = np.zeros((height, width), bool)
    mask[:band, :] = True
    mask[-band:, :] = True
    mask[:, :band] = True
    mask[:, -band:] = True
    return mask


def _flat_model(pixels, ring):
    """The studio as one flat colour."""
    tone = np.asarray([np.median(pixels[ring][:, channel]) for channel in range(3)], np.int16)
    return np.zeros(pixels.shape, np.int16) + tone


def _sweep_model(pixels, ring):
    """The studio as a smooth sweep: a quadratic surface per channel."""
    height, width = pixels.shape[:2]
    ys, xs = np.nonzero(ring)
    if len(ys) < 64:
        return None
    u = 2.0 * xs / max(1, width - 1) - 1.0
    v = 2.0 * ys / max(1, height - 1) - 1.0
    design = np.stack([np.ones(len(ys)), u, v, u * v, u * u, v * v], axis=1)
    grid_u = 2.0 * np.arange(width) / max(1, width - 1) - 1.0
    grid_v = 2.0 * np.arange(height) / max(1, height - 1) - 1.0
    model = np.zeros((height, width, 3), np.float64)
    for channel in range(3):
        solution, *_ = np.linalg.lstsq(design, pixels[ys, xs, channel].astype(np.float64), rcond=None)
        model[..., channel] = (
            solution[0] + solution[1] * grid_u[None, :] + solution[2] * grid_v[:, None] +
            solution[3] * grid_u[None, :] * grid_v[:, None] + solution[4] * grid_u[None, :]**2 +
            solution[5] * grid_v[:, None]**2
        )
    return np.clip(model, 0, 255).astype(np.int16)


def _split_tones(samples):
    """The two tones of a painted checkerboard, found by splitting their brightness."""
    lumas = _luma(samples.astype(np.float64))
    low, high = float(lumas.min()), float(lumas.max())
    for _ in range(16):
        middle = (low + high) / 2.0
        under, over = lumas[lumas < middle], lumas[lumas >= middle]
        if not len(under) or not len(over):
            break
        low, high = float(under.mean()), float(over.mean())
    if high - low < 20:
        return None
    first = samples[lumas < (low + high) / 2.0]
    second = samples[lumas >= (low + high) / 2.0]
    if not len(first) or not len(second):
        return None
    return (
        np.asarray([np.median(first[:, c]) for c in range(3)],
                   np.int16), np.asarray([np.median(second[:, c]) for c in range(3)], np.int16)
    )


def _checker_model(pixels, ring):
    """The studio as a painted checkerboard: two flat tiles laid over each other.

    A checkerboard is not one colour but two, and a machine with white trim
    stands on the lighter of them as easily as on the darker. The tiles are
    therefore not told apart by their colour alone but by which tile the
    picture says stands at that place: the nearer of the two tones is the
    tile that is there.
    """
    samples = pixels[ring]
    tones = _split_tones(samples)
    if tones is None:
        return None
    first, second = tones
    to_first = np.abs(pixels - first).max(axis=2)
    to_second = np.abs(pixels - second).max(axis=2)
    return np.where((to_first <= to_second)[..., None], first, second)


def _score(model, pixels, ring):
    """How well a model explains the border band of the picture."""
    residual = np.abs(pixels - model).max(axis=2)[ring]
    if not len(residual):
        return 1e9
    middle = float(np.median(residual))
    wrong = float((residual > STUDIO_TOLERANCE).mean())
    return middle + wrong * 60.0


def backdrop_model(flat):
    """The studio surface of a render, as an image of its own.

    Three shapes are tried against the border band, which is always studio:
    one flat colour, a smooth sweep, and the two tiles of a painted
    checkerboard. The one that explains the band best is the studio of that
    render.
    """
    pixels = np.asarray(flat).astype(np.int16)
    ring = _ring_mask(flat.size)
    candidates = [('flat', _flat_model(pixels, ring))]
    sweep = _sweep_model(pixels, ring)
    if sweep is not None:
        candidates.append(('sweep', sweep))
    checker = _checker_model(pixels, ring)
    if checker is not None:
        candidates.append(('checker', checker))
    best = None
    for name, model in candidates:
        score = _score(model, pixels, ring)
        if best is None or score < best[0]:
            best = (score, name, model)
    return best[2], best[1]


def _studio_like(pixels, model, tolerance):
    """Which pixels are the studio: the model, near enough."""
    return np.abs(pixels - model).max(axis=2) <= tolerance


def _shadow_like(pixels, model):
    """Which pixels are the studio seen through a veil of grey.

    A cast shadow is the studio pulled down. It is darker than the model, it
    stays close to neutral, and it changes slowly from one pixel to the next.
    """
    level = _luma(model.astype(np.float32))
    observed = _luma(pixels.astype(np.float32))
    with np.errstate(divide='ignore', invalid='ignore'):
        ratio = np.where(level > 20, observed / np.maximum(level, 1.0), 1.0)
    low = ndimage.minimum_filter(ratio, size=3)
    high = ndimage.maximum_filter(ratio, size=3)
    chroma = pixels.max(axis=2) - pixels.min(axis=2)
    return ((ratio >= 0.3) & (ratio <= 0.97) & (high - low <= 0.09) & (chroma <= 24))


def _components(mask):
    """The connected parts of a mask, each with its size."""
    labels, count = ndimage.label(mask)
    if not count:
        return labels, 0, np.zeros(0)
    sizes = ndimage.sum(mask, labels, range(1, count + 1))
    return labels, count, sizes


def matte(image, session, session2=None, alpha=None, alpha2=None):
    """Cut the studio away, leaving the machine on transparency.

    The machine is the solid part of the segmentation, less the studio that
    clings to its outline, plus the parts of the machine the model lost. The
    colour of the edge is taken from the machine next to it rather than from
    the studio behind it, so no milk of the backdrop survives around the
    silhouette.

    Two models may be given, and then both have to agree: a pixel is the
    machine only when both models call it one. Each model is wrong in its
    own way - one punches holes through pale panels, the other leaves a sliver
    of the backdrop standing along an edge - and where they disagree the
    picture is looked at again by the rules below.

    The alpha of the segmentation may be given instead of a session, so the
    rules can be tried against a cached alpha without running the model
    again. The alpha is expected to be the one of the picture at WORK_SIZE.
    """
    flat = image.convert('RGB')
    if max(flat.size) > WORK_SIZE:
        scale = WORK_SIZE / float(max(flat.size))
        flat = flat.resize(
            (max(1, int(flat.width * scale)), max(1, int(flat.height * scale))), Image.Resampling.LANCZOS
        )
    pixels = np.asarray(flat).astype(np.int16)

    model, kind = backdrop_model(flat)

    if alpha is None:
        from rembg import remove
        cut = remove(flat, session=session)
        alpha = np.asarray(cut.getchannel('A')).astype(np.int16)
    else:
        alpha = np.asarray(alpha).astype(np.int16)
        if alpha.shape != pixels.shape[:2]:
            raise ValueError('the cached alpha does not fit the picture')
    if alpha2 is None and session2 is not None:
        from rembg import remove
        other = np.asarray(remove(flat, session=session2).getchannel('A')).astype(np.int16)
    else:
        other = None if alpha2 is None else np.asarray(alpha2).astype(np.int16)

    foreground = alpha >= MIN_ALPHA
    trusted = alpha >= TRUSTED_ALPHA

    # The parts of the machine the model lost: close the confident silhouette
    # and see what the closing fills in.
    solid = ndimage.binary_fill_holes(trusted)
    missing = solid & ~foreground
    labels, count, sizes = _components(missing)
    studio = _studio_like(pixels, model, HOLE_TOLERANCE)
    filled = np.zeros_like(foreground)
    for index in range(count):
        part = labels == (index + 1)
        if part.sum() < MIN_PART:
            continue
        shows_studio = float(studio[part].mean())
        if shows_studio < HOLE_STUDIO_SHARE:
            # A part of the machine: close it with the photograph.
            foreground |= part
            filled |= part

    studio = _studio_like(pixels, model, STUDIO_TOLERANCE)

    # The studio the second model does not believe in: a pixel the first
    # model calls the machine, the second one calls the backdrop, and where
    # the picture shows the studio and not the hardware. Such a pixel is
    # backdrop that the first model kept - a sliver of the painted
    # checkerboard standing along the edge of the machine.
    if other is not None:
        foreground &= ~((other < MIN_ALPHA) & studio & ~filled)

    # The fringe of studio along the outline: the model was not sure of these
    # pixels and the studio is what they show.
    foreground &= ~(studio & ~trusted)

    # Debris: a scrap of studio floating beside the machine.
    labels, count, sizes = _components(foreground)
    if count:
        biggest = int(np.argmax(sizes))
        for index in range(count):
            if index == biggest:
                continue
            part = labels == (index + 1)
            if sizes[index] < DEBRIS_SHARE * sizes[biggest] or studio[part].mean() > 0.5:
                foreground &= ~part

    # The edge: pull the silhouette in so no milk of the studio survives, and
    # take the colour of that edge from the machine next to it.
    core = ndimage.binary_erosion(foreground, iterations=1)
    rim = foreground & ~core
    colours = np.asarray(flat).copy()
    if rim.any():
        _distance, indices = ndimage.distance_transform_edt(~core, return_indices=True)
        colours[rim] = colours[indices[0][rim], indices[1][rim]]

    alpha_out = np.asarray(
        Image.fromarray((foreground * 255).astype('uint8')).filter(ImageFilter.GaussianBlur(0.7)), dtype=np.float32
    ) / 255.0
    out = Image.fromarray(colours.astype('uint8')).convert('RGBA')
    out.putalpha(Image.fromarray((alpha_out * 255).astype('uint8')))
    return out, kind


def fallback_matte(image):
    """The colour-model matte, used when no segmentation model is available.

    The studio of the render is modelled from the edge of the picture, the
    pixels that differ from it are the hardware, and the studio that clings
    to the outline is dropped. It is rougher than the segmentation matte -
    a pale part of the machine is indistinguishable from the studio behind
    it - but it keeps the pipeline runnable without rembg.
    """
    flat = image.convert('RGB')
    if max(flat.size) > WORK_SIZE:
        scale = WORK_SIZE / float(max(flat.size))
        flat = flat.resize(
            (max(1, int(flat.width * scale)), max(1, int(flat.height * scale))), Image.Resampling.LANCZOS
        )
    pixels = np.asarray(flat).astype(np.int16)
    model, _kind = backdrop_model(flat)
    deviation = np.abs(pixels - model).max(axis=2)

    foreground = deviation > STUDIO_TOLERANCE
    # The openings in the machine are closed: a screen is not a hole.
    solid = ndimage.binary_fill_holes(foreground)
    foreground |= solid & ~foreground

    # The scrap of studio floating beside the machine is dropped.
    labels, count, sizes = _components(foreground)
    if count:
        biggest = int(np.argmax(sizes))
        for index in range(count):
            if index == biggest:
                continue
            part = labels == (index + 1)
            if sizes[index] < DEBRIS_SHARE * sizes[biggest]:
                foreground &= ~part

    core = ndimage.binary_erosion(foreground, iterations=1)
    rim = foreground & ~core
    colours = np.asarray(flat).copy()
    if rim.any():
        _distance, indices = ndimage.distance_transform_edt(~core, return_indices=True)
        colours[rim] = colours[indices[0][rim], indices[1][rim]]

    alpha = np.asarray(
        Image.fromarray((foreground * 255).astype('uint8')).filter(ImageFilter.GaussianBlur(0.7)), dtype=np.float32
    ) / 255.0
    out = Image.fromarray(colours.astype('uint8')).convert('RGBA')
    out.putalpha(Image.fromarray((alpha * 255).astype('uint8')))
    return out
