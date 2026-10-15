#!/usr/bin/env python3
"""Makes the app's icon from the game's own art: the game's memory card save icon (the PS2's 3D icon format: a model with its
texture), drawn large on a rounded square of blue, the way the Crash Twinsanity Mac port's icon is made. Nothing of the game's
goes into the repository: the icon is made in the build folder from your own disc.

    tools/make_icon.py DISC [--icon PATH] [--out build/icon] [--yaw DEGREES] [--pitch DEGREES] [--light] [--smooth]

--icon is the icon's file on the disc (Wrath of Cortex's STUFF/CRASH.ICO, Crash's head, by default), or a file in one of the
disc's Radical archives as ARCHIVE:NAME (Crash Tag Team Racing's COMMON/COMMON.RCF:art/memcard/crash.ico, a crate with
Crash's face, the same bytes the game writes to the card as list.ico, copy.ico and delete.ico).

Writes icon_1024.png and, on macOS, AppIcon.icns (iconutil).
"""
import argparse
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    import numpy as np
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    # No icon then (the app is the same without one): the build goes on
    print("make_icon.py: Pillow and NumPy are needed for the icon (python3 -m pip install pillow numpy); no icon made",
          file=sys.stderr)
    out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else Path("build/icon")
    out.mkdir(parents=True, exist_ok=True)
    (out / "AppIcon.icns").write_bytes(b"")
    sys.exit(0)

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_TOOLS = [ROOT / "ps2-native-runtime/tools", ROOT.parent / "ps2-native-runtime/tools"]


def read_disc_file(disc, path):
    """A file on the disc, or ARCHIVE:NAME, a file in a Radical archive (an .RCF) on the disc"""
    for tools in RUNTIME_TOOLS:
        if (tools / "disc.py").exists():
            sys.path.insert(0, str(tools))
            break
    from disc import Disc  # noqa: E402
    archive, _, name = path.partition(":")
    data = Disc(disc).read_file(archive)
    return read_rcf_file(data, name, archive) if name else data


def read_rcf_file(data, name, archive):
    """A file in a Radical archive ("ATG CORE CEMENT LIBRARY"): at 0x24 its directory's offset, then the names' offset, and
    at 0x38 the files' count; the directory's entries are (a hash of the name, offset, size), in the hashes' order; the names
    (each after 16 bytes, the last its length with the terminator, then 3 bytes) follow the files' order on the disc, so a
    name's file is the entry with the same place in the offsets' order. Its folders are written with \\ or /"""
    if not data.startswith(b"ATG CORE CEMENT LIBRARY"):
        raise SystemExit(f"{archive} isn't a Radical archive")
    directory, _, names, _, _, count = struct.unpack_from("<6I", data, 0x24)
    entries = sorted((struct.unpack_from("<3I", data, directory + 12 * n) for n in range(count)), key=lambda e: e[1])
    at = names + 8
    for _, offset, size in entries:
        length = struct.unpack_from("<I", data, at + 12)[0]
        if data[at + 16:at + 15 + length].decode("latin-1").lower() == name.replace("/", "\\").lower():
            return data[offset:offset + size]
        at += 16 + length + 3
    raise SystemExit(f"{archive} has no {name}")


def decompress_texture(data, at):
    """A compressed icon texture (the icon's type's bit 3): its size in bytes, then runs of 16-bit pixels: a count below
    0x8000 repeats the next pixel that many times, one above it is followed by 0x10000 - count pixels as they are"""
    size = struct.unpack_from("<I", data, at)[0]
    at += 4
    end = at + size
    pixels = []
    while at < end:
        count = struct.unpack_from("<H", data, at)[0]
        at += 2
        if count & 0x8000:
            literal = 0x10000 - count
            pixels.extend(struct.unpack_from(f"<{literal}H", data, at))
            at += 2 * literal
        else:
            pixels.extend([struct.unpack_from("<H", data, at)[0]] * count)
            at += 2
    if len(pixels) != 128 * 128:
        raise SystemExit(f"the icon's compressed texture has {len(pixels)} pixels, not 128x128")
    return np.array(pixels, dtype=np.uint16).reshape(128, 128)


def parse_icon(data):
    """The PS2 icon: its triangles' vertices (x, y, z), normals, texture coordinates and colours, and its 128x128 texture
    (RGBA)"""
    magic, shapes, texture_type, _, count = struct.unpack_from("<IIIfI", data, 0)
    if magic != 0x10000:
        raise SystemExit("not a PS2 icon")
    at = 20
    positions, normals, uvs, colours = [], [], [], []
    for _ in range(count):
        x, y, z, _w = struct.unpack_from("<4h", data, at)
        at += 8 * shapes  # every shape's position (the first used)
        nx, ny, nz, _w = struct.unpack_from("<4h", data, at)
        at += 8
        u, v = struct.unpack_from("<2h", data, at)
        at += 4
        r, g, b, a = data[at:at + 4]
        at += 4
        positions.append((x / 4096, y / 4096, z / 4096))
        normals.append((nx / 4096, ny / 4096, nz / 4096))
        uvs.append((u / 4096, v / 4096))
        colours.append((r, g, b))
    # The animation: its header and each frame's keys
    _, _, _, _, frames = struct.unpack_from("<IIfII", data, at)
    at += 20
    for _ in range(frames):
        # A frame: its shape, its keys' count, then the keys (a time and a value each)
        _, keys = struct.unpack_from("<2I", data, at)
        at += 8 + keys * 8
    if texture_type & 8:
        pixels = decompress_texture(data, at)
    else:
        pixels = np.frombuffer(data, dtype="<u2", count=128 * 128, offset=at).reshape(128, 128)
    rgba = np.zeros((128, 128, 4), np.uint8)
    rgba[..., 0] = (pixels & 31) * 255 // 31
    rgba[..., 1] = ((pixels >> 5) & 31) * 255 // 31
    rgba[..., 2] = ((pixels >> 10) & 31) * 255 // 31
    rgba[..., 3] = 255
    return np.array(positions), np.array(normals), np.array(uvs), np.array(colours, np.float32) / 128.0, rgba


def sample(texture, uv, smooth):
    """The texture's colour at each texture coordinate: its texel, or with smooth blended from its four nearest texels (drawn
    large, a 128x128 texture taken texel by texel shows its squares; but a texture of many small pieces, as a head's is,
    would bleed the pieces' edges into each other)"""
    th, tw = texture.shape[:2]
    if not smooth:
        tx = np.clip((uv[..., 0] * tw).astype(int), 0, tw - 1)
        ty = np.clip((uv[..., 1] * th).astype(int), 0, th - 1)
        return texture[ty, tx, :3].astype(np.float32)
    x = np.clip(uv[..., 0] * tw - 0.5, 0, tw - 1)
    y = np.clip(uv[..., 1] * th - 0.5, 0, th - 1)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    x1, y1 = np.minimum(x0 + 1, tw - 1), np.minimum(y0 + 1, th - 1)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    rgb = texture[..., :3].astype(np.float32)
    top = rgb[y0, x0] * (1 - fx) + rgb[y0, x1] * fx
    bottom = rgb[y1, x0] * (1 - fx) + rgb[y1, x1] * fx
    return top * (1 - fy) + bottom * fy


def render(positions, normals, uvs, colours, texture, size, yaw, pitch, light, smooth):
    """The model drawn: turned, fitted to the picture, its triangles textured and lit by their vertex colours, nearest first.
    With light, the vertex colours are also shaded by a light from above, left and in front (as seen), for a model whose
    colours are flat (a box's faces would otherwise be told apart only by their edges)"""
    a, b = math.radians(yaw), math.radians(pitch)
    turn = np.array([[math.cos(a), 0, math.sin(a)], [0, 1, 0], [-math.sin(a), 0, math.cos(a)]])
    tilt = np.array([[1, 0, 0], [0, math.cos(b), -math.sin(b)], [0, math.sin(b), math.cos(b)]])
    with np.errstate(all="ignore"):
        p = positions @ (tilt @ turn).T
        n = normals @ (tilt @ turn).T
    if light:
        # The picture's y points down and z away: the light comes from up (-y), left (-x) and the viewer (-z)
        towards = np.array([-0.35, -0.6, -0.7])
        towards /= np.linalg.norm(towards)
        colours = colours * (0.55 + 0.45 * np.clip(n @ towards, 0, 1))[:, None]
    # The icon's y points down; the picture's does too. Fitted with a margin
    lo, hi = p[:, :2].min(0), p[:, :2].max(0)
    scale = size * 0.92 / (hi - lo).max()
    centre = (lo + hi) / 2
    xy = (p[:, :2] - centre) * scale + size / 2
    depth = p[:, 2]
    image = np.zeros((size, size, 4), np.float32)
    zbuffer = np.full((size, size), np.inf, np.float32)
    for t in range(0, len(p) - 2, 3):
        v = xy[t:t + 3]
        x0, y0 = np.floor(v.min(0)).astype(int)
        x1, y1 = np.ceil(v.max(0)).astype(int)
        x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, size - 1), min(y1, size - 1)
        if x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        (ax, ay), (bx, by), (cx, cy) = v
        area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
        if abs(area) < 1e-9:
            continue
        w0 = ((bx - gx) * (cy - gy) - (by - gy) * (cx - gx)) / area
        w1 = ((cx - gx) * (ay - gy) - (cy - gy) * (ax - gx)) / area
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        z = w0 * depth[t] + w1 * depth[t + 1] + w2 * depth[t + 2]
        region = zbuffer[y0:y1 + 1, x0:x1 + 1]
        closer = inside & (z < region)
        if not closer.any():
            continue
        uv = w0[..., None] * uvs[t] + w1[..., None] * uvs[t + 1] + w2[..., None] * uvs[t + 2]
        texel = sample(texture, uv, smooth)
        lit = w0[..., None] * colours[t] + w1[..., None] * colours[t + 1] + w2[..., None] * colours[t + 2]
        rgb = np.clip(texel * lit, 0, 255)
        out = image[y0:y1 + 1, x0:x1 + 1]
        out[closer, :3] = rgb[closer]
        out[closer, 3] = 255
        region[closer] = z[closer]
    return Image.fromarray(image.astype(np.uint8), "RGBA")


def background(size):
    """A radial gradient of blue on a rounded square, as the Twinsanity port's icon has"""
    centre, edge = (70, 170, 255), (10, 52, 150)
    y, x = np.mgrid[0:size, 0:size]
    d = np.sqrt((x - size / 2) ** 2 + (y - size * 0.42) ** 2) / (size * 0.75)
    d = np.clip(d, 0, 1)[..., None]
    rgb = (np.array(centre) * (1 - d) + np.array(edge) * d).astype(np.uint8)
    square = Image.fromarray(np.dstack([rgb, np.full((size, size), 255, np.uint8)]), "RGBA")
    mask = Image.new("L", (size, size), 0)
    inset = size * 100 // 1024
    ImageDraw.Draw(mask).rounded_rectangle((inset, inset, size - inset - 1, size - inset - 1), radius=size * 185 // 1024,
                                           fill=255)
    square.putalpha(mask)
    return square, inset


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("disc")
    parser.add_argument("--icon", default="STUFF/CRASH.ICO")
    parser.add_argument("--out", default=str(ROOT / "build/icon"))
    parser.add_argument("--yaw", type=float, default=20)
    parser.add_argument("--pitch", type=float, default=0)
    parser.add_argument("--light", action="store_true", help="shade the model by a light (for flat vertex colours)")
    parser.add_argument("--smooth", action="store_true", help="blend the texture's texels (for a texture of one picture)")
    args = parser.parse_args()
    positions, normals, uvs, colours, texture = parse_icon(read_disc_file(args.disc, args.icon))
    size = 1024
    # Drawn at twice the size and brought down, for smooth edges
    model = render(positions, normals, uvs, colours, texture, size * 2, args.yaw, args.pitch, args.light,
                   args.smooth)
    model = model.resize((size, size), Image.LANCZOS)
    icon, inset = background(size)
    inner = size - 2 * inset
    model = model.resize((inner * 88 // 100, inner * 88 // 100), Image.LANCZOS)
    # A soft shadow under the model
    shadow = Image.new("RGBA", model.size, (0, 0, 0, 0))
    shadow.putalpha(model.getchannel("A").point(lambda v: v * 90 // 255))
    shadow = shadow.filter(ImageFilter.GaussianBlur(size / 60))
    at = ((size - model.width) // 2, (size - model.height) // 2 + size // 60)
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    layer.paste(shadow, (at[0], at[1] + size // 50), shadow)
    layer.paste(model, at, model)
    mask = icon.getchannel("A")
    icon = Image.alpha_composite(icon, layer)
    icon.putalpha(Image.composite(icon.getchannel("A"), Image.new("L", (size, size), 0), mask))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    icon.save(out / "icon_1024.png")
    if shutil.which("iconutil"):
        with tempfile.TemporaryDirectory() as folder:
            iconset = Path(folder) / "AppIcon.iconset"
            iconset.mkdir()
            for n in (16, 32, 64, 128, 256, 512):
                icon.resize((n, n), Image.LANCZOS).save(iconset / f"icon_{n}x{n}.png")
                icon.resize((n * 2, n * 2), Image.LANCZOS).save(iconset / f"icon_{n}x{n}@2x.png")
            subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(out / "AppIcon.icns")], check=True)
    print(f"icon: {out / 'icon_1024.png'}")


if __name__ == "__main__":
    main()
