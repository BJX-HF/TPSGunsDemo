from pathlib import Path
import html
import re
from html.parser import HTMLParser

ROOT = Path(__file__).resolve().parent
PAGE = ROOT.parent / 'FPS相机后坐力与伪代码详解.html'
source = PAGE.read_text(encoding='utf-8')
code = html.unescape(re.search(r'<pre id="singleShotCode"><code>(.*?)</code></pre>', source, re.S).group(1))
code = code.split('# 引擎适配示意')[0]
scope = {}
exec(compile(code, 'article_example', 'exec'), scope)
Recoil = scope['SingleShotRecoil']

deltas = []
shot = Recoil(lambda pitch, yaw: deltas.append((pitch, yaw)))
shot.Fire(2, -.4, .06, .08, .24, .65)
shot.Tick(.05)
assert abs(shot.previous[0] - 2 * (1 - (1 - .05 / .06) ** 3)) < 1e-12
shot.Tick(.02)
assert shot.phase == 'HOLD'
assert abs(shot.elapsed - .01) < 1e-12
assert abs(shot.previous[0] - 1.3) < 1e-12
shot.Tick(1)
assert shot.phase == 'IDLE' and shot.previous == (0, 0)
assert abs(sum(delta[0] for delta in deltas)) < 1e-12
assert abs(sum(delta[1] for delta in deltas)) < 1e-12

for durations, timestep in [((0, 0, 0), 0), ((.001, .001, .001), 1 / 60)]:
    shot = Recoil(lambda p, y: None)
    shot.Fire(2, -.4, *durations, .65)
    shot.Tick(timestep)
    assert shot.phase == 'IDLE'

shot = Recoil(lambda p, y: None)
shot.Fire(2, -.4, .06, .08, .24, .65)
for _ in range(23):
    shot.Tick(1 / 60)
assert shot.phase == 'IDLE'

class Structure(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = []
        self.links = []
    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if 'id' in attrs:
            self.ids.append(attrs['id'])
        if tag == 'a' and attrs.get('href', '').startswith('#'):
            self.links.append(attrs['href'][1:])

parser = Structure()
parser.feed(source)
assert len(parser.ids) == len(set(parser.ids))
assert set(parser.links) <= set(parser.ids)
assert '__ORIGINAL_' not in source
assert len(re.findall(r'src="data:image/', source)) == 2
script = re.search(r'<script>(.*?)</script>', source, re.S).group(1)
(ROOT / 'page-script.js').write_text(script, encoding='utf-8')
print('PASS: code executes, cross-phase remainder, zero/sub-tick durations, 60 Hz completion, net-zero recovery')
print('PASS: unique IDs, all directory links resolve, two embedded original images, no placeholders')
print('Sections:', len(re.findall(r'<section ', source)))
print('HTML bytes:', PAGE.stat().st_size)
