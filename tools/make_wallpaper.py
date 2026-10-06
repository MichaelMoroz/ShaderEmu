"""Makes the desktop's pictures for the Linux image, as PPM files named wallpaper-NAME.ppm under
build/images/linux/root/usr/share (tools/make_linux_image.py then puts them in the image).

    python tools/make_wallpaper.py [FOLDER ...]

- Every line of linux/apps/wallpapers.txt is a photograph on Unsplash: a name and the photo's
  id (the end of its address). They are fetched here and not kept in this repository; the
  Unsplash licence allows using, changing and passing them on.
- Every picture in a FOLDER given on the command line is added too, named after its file.

A picture 1920 or more across is brought down to fit 1920x1080, one 1280 or more to fit
1280x720, and smaller ones are left as they are. The desktop crops and scales what it shows
to the screen (linux/apps/ui.h). Needs Pillow and, for the photographs, curl and the network.
"""
import io, os, re, subprocess, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'images', 'linux', 'root', 'usr', 'share')
FETCH = os.path.join(ROOT, 'build', 'fetch')
LIST = os.path.join(ROOT, 'linux', 'apps', 'wallpapers.txt')
AGENT = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36'


def fetch(photo):
    """The photograph with that Unsplash id, 2400 across at most, from build/fetch if it is there."""
    cached = os.path.join(FETCH, 'unsplash-%s.jpg' % photo)
    if not os.path.exists(cached):
        os.makedirs(FETCH, exist_ok=True)
        # the site answers a browser's request and refuses a bare one (and Python's): curl
        address = 'https://unsplash.com/photos/%s/download?force=true&w=2400' % photo
        data = subprocess.run(['curl', '-sL', '-m', '90', '-A', AGENT, '-H', 'Accept: image/avif,image/webp,image/*,*/*', address],
                              capture_output=True, check=True).stdout
        Image.open(io.BytesIO(data)).verify()   # a picture, not an error page
        open(cached, 'wb').write(data)
    return Image.open(cached)


def keep(name, picture):
    if getattr(picture, 'is_animated', False):
        picture.seek(0)   # an animation's first frame
    picture = picture.convert('RGB')
    w, h = picture.size
    box = (1920, 1080) if w >= 1920 else (1280, 720) if w >= 1280 else None
    if box:
        picture.thumbnail(box, Image.LANCZOS)
    picture.save(os.path.join(OUT, 'wallpaper-%s.ppm' % name), 'PPM')
    print('wallpaper-%s.ppm: %dx%d from %dx%d' % (name, picture.size[0], picture.size[1], w, h))


def main():
    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):
        if old.startswith('wallpaper-'):
            os.remove(os.path.join(OUT, old))
    for line in open(LIST, encoding='utf-8'):
        words = line.split('#')[0].split()
        if len(words) < 2:
            continue
        try:
            keep(words[0], fetch(words[1]))
        except Exception as error:   # the network, mostly: the image has the other pictures
            print('wallpaper-%s.ppm not made: %s' % (words[0], error), file=sys.stderr)
    for folder in sys.argv[1:]:
        for n, name in enumerate(sorted(os.listdir(folder))):
            try:
                picture = Image.open(os.path.join(folder, name))
            except Exception:
                continue   # not a picture
            # the image's file system takes short plain names
            stem = re.sub(r'[^a-z0-9]+', '-', os.path.splitext(name)[0].lower()).strip('-')[:16].strip('-')
            keep(stem or 'picture%d' % n, picture)


if __name__ == '__main__':
    main()
