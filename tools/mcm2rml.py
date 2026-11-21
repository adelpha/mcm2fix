#!/usr/bin/env python3
"""mcm2rml: writes MenuMaX copies (.rml + .rcss) of the game's menu screens
from the game's own layouts (the .dtm files in UI\\*.DAT).

  python3 mcm2rml.py <game UI folder> <out dir>

Writes <out dir>/<screen>.rml and .rcss for each screen in SCREENS, plus the
shared menus.rcss. A screen copies one or more of the game's dialogs (its
<body game-dialogs="...">); MenuMaX shows it while the game shows them
(MaXMenus=1). Every control becomes an element tied to the game's control of
the same name on its own dialog (game="Name" inside game-dialog="..."), at
the same place (1dp = one 640x480 menu pixel); pictures are the game's own
(/dat/<ARCHIVE>/<file>, read from the DATs at run time). The output is meant
to be edited by hand afterwards.
"""
import os
import re
import struct
import sys

# ---------------------------------------------------------------- archives

def rs2(path):
    d = open(path, 'rb').read()
    if d[:3] != b'RS2':
        raise SystemExit(f'{path}: not an RS2 archive')
    n = struct.unpack_from('<I', d, 4)[0]
    p = 8
    out = {}
    for _ in range(n):
        ln = struct.unpack_from('<I', d, p)[0]
        p += 4
        name = d[p:p + ln].split(b'\0')[0].decode('latin1')
        p += ln
        off, size = struct.unpack_from('<II', d, p)
        p += 8
        out[name.replace('/', '\\').lower()] = (name, d[off:off + size])
    return out


def tga_size(data):
    return struct.unpack_from('<HH', data, 12)


def parse_dtm(text):
    secs = []
    cur = None
    for line in text.splitlines():
        m = re.match(r'^\[(.+)\]\s*$', line)
        if m:
            cur = {'_name': m.group(1)}
            secs.append(cur)
            continue
        if cur is None or '=' not in line or line.startswith(';'):
            continue
        k, v = line.split('=', 1)
        k = k.strip()
        v = re.sub(r"\s*'(True|False)$", '', v.strip())
        cur[k] = v[1:-1] if v.startswith('"') and v.endswith('"') else v
    return secs


class Game:
    def __init__(self, ui_dir):
        self.ui = ui_dir
        self.dats = {}
        self.sprites = {}  # sprite name -> (ARCHIVE, path in archive)

    def dat(self, name):
        name = name.upper()
        if name not in self.dats:
            path = None
            for f in os.listdir(self.ui):
                if f.upper() == name + '.DAT':
                    path = os.path.join(self.ui, f)
            if not path:
                raise SystemExit(f'no {name}.DAT in {self.ui}')
            self.dats[name] = rs2(path)
        return self.dats[name]

    def layout(self, archive):
        """The dialog's controls; self.info[archive] its [Set_Control] and
        [Set_Information] keys."""
        d = self.dat(archive)
        dtm = [v for k, v in d.items() if k.endswith('.dtm')][0][1].decode('latin1')
        secs = parse_dtm(dtm)
        anim = False
        controls = []
        info = {}
        own = {}
        for s in secs:
            if s['_name'] == 'Set_Anim':
                anim = True
                continue
            if anim:
                if 'Files1' in s:
                    self.sprites.setdefault(s['_name'], (archive.upper(), s['Files1']))
                    own[s['_name']] = (archive.upper(), s['Files1'])
                continue
            if s['_name'] in ('Set_Control', 'Set_Information'):
                info.update(s)
                continue
            controls.append(s)
        self.own = getattr(self, 'own', {})
        self.own[archive.upper()] = own
        self.info = getattr(self, 'info', {})
        self.info[archive.upper()] = info
        return controls

    def use(self, archive):
        """Sprites are looked for in this archive first (names repeat)."""
        self.current = archive.upper()

    def where(self, name):
        cur = getattr(self, 'current', None)
        if cur and name in self.own.get(cur, {}):
            return self.own[cur][name]
        return self.sprites.get(name)

    def sprite(self, name, opts=''):
        if not name:
            return None
        w = self.where(name)
        if not w:
            return None
        arch, path = w
        path = path.split(',')[0].strip()  # an animation: its first picture
        return '/dat/' + arch + '/' + path.replace('\\', '/') + ('?' + opts if opts else '')

    def frames(self, name):
        """An animation's pictures (Files1 = a.tga,b.tga,...), in order."""
        w = self.where(name) if name else None
        if not w:
            return []
        arch, paths = w
        return ['/dat/' + arch + '/' + p.strip().replace('\\', '/') for p in paths.split(',') if p.strip()]

    def size(self, name):
        arch, path = self.where(name)
        return tga_size(self.dat(arch)[path.lower()][1])


# ---------------------------------------------------------------- layout

def xywh(s):
    return [int(float(t)) for t in s['Xywh'].split(',')]


def colour(v):
    v = int(v)
    return '#%02x%02x%02x' % (v & 255, (v >> 8) & 255, (v >> 16) & 255)


def esc(t):
    return t.replace('&&', '&').replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


LINE = 15  # text line height (Arial 13dp in a 15-pixel cell, as the game's 15-pixel font)


class Writer:
    def __init__(self, game):
        self.g = game
        self.rcss = []
        self.prefix = ''

    def rule(self, sel, body):
        self.rcss.append(f'{sel} {{ {body} }}')

    def text_style(self, s, h, sprite_w=0):
        st = []
        if s.get('FontHeight') and float(s['FontHeight']) > 15:
            st.append('font-size: 14dp')
        if s.get('TextColor') and s['TextColor'] != '16777215':
            st.append('color: ' + colour(s['TextColor']))
        if s.get('TextDrop', '-1').startswith('0'):
            st.append('font-effect: none')
        av = s.get('TextAlignV', 'TOP')
        top = 0
        if h >= 2 * LINE + 8 and s.get('Type', '').strip() == 'STATICTEXT':
            st.append('white-space: normal')  # room for more lines: the text wraps
            av = 'TOP'
        if av == 'MIDDLE':
            top = max(0, (h - LINE) // 2)
        elif av == 'BOTTOM':
            top = max(0, h - LINE - int(s.get('TextTMargin', '0')))
        if top:
            st.append(f'padding-top: {top}dp')
        lm = int(s.get('TextLMargin', '0')) + sprite_w
        if lm:
            st.append(f'padding-left: {lm}dp')
        al = s.get('TextAlign', 'LEFT')
        if sprite_w == 0 and al in ('CENTER', 'RIGHT'):
            st.append('text-align: ' + al.lower())
        return st

    def element(self, s, by, out, indent):
        name = s['_name']
        t = s.get('Type', '').strip()
        x, y, w, h = xywh(s)
        pos = f'left: {x}dp; top: {y}dp; width: {w}dp; height: {h}dp;'
        pad = '  ' * indent
        text = esc(s.get('Text', ''))
        kids = [c for c in by.values() if c.get('Anchor') == name]
        g = self.g
        eid = self.prefix + name
        sel = '#' + eid
        tip = s.get('ToolTipText')
        attrs = f'id="{eid}" game="{name}"' + (f' title="{esc(tip)}"' if tip else '')
        if s.get('FX', '').strip().lower() == 'anim':
            # A pop-out panel: MenuMaX draws its show / hide animation's
            # pictures as the game plays them.
            for key, attr in (('FXAnimIn', 'game-anim-in'), ('FXAnimOut', 'game-anim-out')):
                fr = g.frames(s.get(key))
                if len(fr) > 1:
                    attrs += f' {attr}="{" ".join(fr)}"'

        def styled(st):
            return pos + (' ' + '; '.join(st) + ';' if st else '')

        if t == 'STATIC':
            spr = s.get('AnimNorm')
            if s.get('_frame'):
                self.rule(sel, 'decorator: ninepatch(plate-outer, plate-inner, 1);')
            elif g.sprite(spr):
                self.rule(sel, f'decorator: image({g.sprite(spr)});')
            if not kids:
                out.append(f'{pad}<div {attrs} class="plate" game-text="off" style="{pos}"/>')
                return
            out.append(f'{pad}<div {attrs} class="plate" game-text="off" style="{pos}">')
            for c in reading_order(kids):
                self.element(c, by, out, indent + 1)
            out.append(f'{pad}</div>')
            return
        if t == 'STATICTEXT':
            spr = s.get('AnimNorm')
            if g.sprite(spr):
                self.rule(sel, f'decorator: image({g.sprite(spr)});')
            out.append(f'{pad}<div {attrs} class="text" style="{styled(self.text_style(s, h))}">{text}</div>')
            return
        if t == 'MULTISTATE' and s.get('State0'):
            on, off = s.get('State1'), s.get('State0')
            if off != 'CheckBoxOff' and g.sprite(off):
                self.rule(f'{sel} .box', f'decorator: image({g.sprite(off)});')
                if g.sprite(on):
                    self.rule(f'{sel}.on .box', f'decorator: image({g.sprite(on)});')
                ow, oh = g.size(off)
                self.rule(f'{sel} .box', f'width: {ow}dp; height: {oh}dp;')
            out.append(f'{pad}<div {attrs} class="check" style="{styled(self.text_style(s, h, 14))}"><div class="box"/><span class="t">{text}</span></div>')
            return
        if t == 'MULTISTATE':
            out.append(f'{pad}<div {attrs} class="cycle" style="{styled(self.text_style(s, h))}">{text}</div>')
            return
        if t == 'RADIOBUTTON':
            on, off = s.get('AnimOn'), s.get('AnimOff')
            if on and on.startswith('OptTab'):
                self.rule(sel, f'decorator: image({g.sprite(off, "black")});')
                self.rule(sel + '.on', f'decorator: image({g.sprite(on, "black")});')
                out.append(f'{pad}<div {attrs} class="tab" style="{styled(self.text_style(s, h))}">{text}</div>')
                return
            if on and on != 'RadioButtonOn' and g.sprite(on):
                # A radio button drawn as a picture of its own (no text box).
                self.rule(sel, f'decorator: image({g.sprite(off)});')
                self.rule(sel + '.on', f'decorator: image({g.sprite(on)});')
                if s.get('AnimOffDisable') and g.sprite(s['AnimOffDisable']):
                    self.rule(sel + '.disabled', f'decorator: image({g.sprite(s["AnimOffDisable"])});')
                out.append(f'{pad}<div {attrs} class="pick" style="{styled(self.text_style(s, h))}">{text}</div>')
                return
            out.append(f'{pad}<div {attrs} class="radio" style="{styled(self.text_style(s, h, 14))}"><div class="box"/><span class="t">{text}</span></div>')
            return
        if t in ('BUTTON', 'SCROLLUP', 'SCROLLDOWN'):
            norm = s.get('AnimNorm')
            lit = s.get('AnimFocus') or norm
            push = s.get('AnimPush') or norm
            grey = s.get('AnimDisable')
            if g.sprite(norm):
                self.rule(sel, f'decorator: image({g.sprite(norm)});')
            if lit != norm and g.sprite(lit):
                self.rule(f'{sel}:hover, {sel}:focus-visible', f'decorator: image({g.sprite(lit)});')
            if push != norm and g.sprite(push):
                self.rule(f'{sel}:active', f'decorator: image({g.sprite(push)});')
            if grey and g.sprite(grey):
                self.rule(f'{sel}.disabled', f'decorator: image({g.sprite(grey)});')
            body = text if s.get('LocalTextId') or s.get('_label') or s.get('Text') else ''
            st = self.text_style(s, h) if body else []
            out.append(f'{pad}<div {attrs} class="button" style="{styled(st)}">{body}</div>')
            return
        if t in ('HSCROLLBAR', 'VSCROLLBAR'):
            vert = t == 'VSCROLLBAR'
            track = s.get('AnimNorm')
            thumb = s.get('AnimThumb') or ('ElevatorThumbUp' if vert else 'HorizSliderThumbUp')
            tw, th = g.size(thumb) if g.where(thumb) else (12, 12)
            if track and g.sprite(track):
                self.rule(f'{sel} slidertrack', f'decorator: image({g.sprite(track)});')
            elif s.get('_track'):
                self.rule(f'{sel} slidertrack', f'decorator: image({g.sprite(s["_track"], "bar")});')
            if vert:
                self.rule(f'{sel} sliderbar', f'width: {tw}dp; height: {th}dp; margin-left: {(w - tw) // 2}dp; '
                          f'decorator: image({g.sprite(thumb)});')
            else:
                self.rule(f'{sel} sliderbar', f'width: {tw}dp; height: {th}dp; margin-top: {(h - th) // 2}dp; '
                          f'decorator: image({g.sprite(thumb)});')
            extra = ' orientation="vertical"' if vert else ''
            out.append(f'{pad}<input type="range"{extra} {attrs} class="slider{" vslider" if vert else ""}" style="{pos}"/>')
            return
        if t == 'DROPDOWNLIST':
            style = pos.replace(f'height: {h}dp;', 'height: 21dp;')
            if s.get('TextColor'):
                style += ' color: ' + colour(s['TextColor']) + ';'
            if s.get('TextAlign', 'LEFT').strip() in ('CENTER', 'RIGHT'):
                style += ' text-align: ' + s['TextAlign'].strip().lower() + ';'
            tb = s.get('AnimTextBack')
            if tb and tb != 'DropDown05' and g.sprite(tb):
                self.rule(f'{sel} selectvalue', f'decorator: image({g.sprite(tb)});')
            lb = s.get('AnimListBack')
            if lb and g.sprite(lb):
                self.rule(f'{sel} selectbox', f'decorator: image({g.sprite(lb)});')
            out.append(f'{pad}<select {attrs} class="dropdown" style="{style}"></select>')
            return
        if t == 'LISTBOX':
            spr = s.get('AnimNorm')
            if g.sprite(spr):
                self.rule(sel, f'decorator: image({g.sprite(spr)});')
            st = []
            if s.get('TextColor'):
                st.append('color: ' + colour(s['TextColor']))
            if s.get('SelectColor'):
                self.rule(f'{sel} .row.on', 'color: ' + colour(s['SelectColor']) + ';')
            al = s.get('TextAlign', 'LEFT')
            if al in ('CENTER', 'RIGHT'):
                st.append('text-align: ' + al.lower())
            if s.get('TextDrop', '-1').startswith('0'):
                st.append('font-effect: none')
            # The track preview: a picture the game draws itself, shown through.
            win = ' game-window' if name == 'PictureBox' else ''
            out.append(f'{pad}<div {attrs}{win} class="list" style="{styled(st)}"></div>')
            return
        if t == 'EDITBOX':
            spr = s.get('AnimNorm')
            if g.sprite(spr):
                self.rule(sel, f'decorator: image({g.sprite(spr)});')
            st = []
            if s.get('TextColor'):
                st.append('color: ' + colour(s['TextColor']))
            lm = int(s.get('TextLMargin', '0'))
            if lm:
                st.append(f'padding-left: {lm}dp')
            ml = s.get('MaxLen')
            out.append(f'{pad}<input type="text" {attrs} class="edit"{f" maxlength={chr(34)}{ml}{chr(34)}" if ml else ""} style="{styled(st)}"/>')
            return
        out.append(f'{pad}<!-- {name}: {t} not copied yet -->')

def covered(controls):
    """Names of controls the game draws under a later box plate (hidden by it)."""
    out = set()
    order = [c for c in controls if 'Xywh' in c]
    absx = {}
    by = {c['_name']: c for c in order}
    def pos(c):
        x, y, w, h = xywh(c)
        a = c.get('Anchor')
        if a and a in by:
            ax, ay = pos(by[a])
            x, y = x + ax, y + ay
        return x, y
    for i, c in enumerate(order):
        if c.get('Type', '').strip() == 'STATIC':
            continue
        x, y = pos(c)
        _, _, w, h = xywh(c)
        for p in order[i + 1:]:
            if p.get('Type', '').strip() != 'STATIC' or p['_name'] == c.get('Anchor') or p.get('Show', '-1').startswith('0'):
                continue
            # Only a plate beside it (in the same panel) covers it; a pop-out
            # panel (FX) shows at other times.
            if p.get('FX', '').strip() or p.get('Anchor') != c.get('Anchor'):
                continue
            px, py = pos(p)
            _, _, pw, ph = xywh(p)
            if px <= x and py <= y and x + w <= px + pw and y + h <= py + ph:
                out.add(c['_name'])
    return out


def reading_order(items):
    def key(s):
        x, y, w, h = xywh(s)
        return (y // 8, x)
    return sorted(items, key=key)


def resolve_page(controls):
    by = {}
    for s in controls:
        if 'Xywh' in s:
            by[s['_name']] = s
    roots = [s for s in by.values() if not s.get('Anchor') or s['Anchor'] not in by]
    return by, reading_order(roots)


# ---------------------------------------------------------------- MCM2MaX's Graphics page (menus.cpp)

GAP, BOXW, LEFTX = 9, 228, 8
RIGHTX = LEFTX + BOXW + GAP
TOPY, TOPH = 7, 76
LOWY = TOPY + TOPH + GAP
ROWTOP, ROWSTEP = 27, 22
FEATURESH = ROWTOP + 2 * ROWSTEP + 20 + 7
SLIDERS = [('Mcm2fixAniso', 'Anisotropy'), ('Mcm2fixMsaa', 'Antialiasing'), ('Mcm2fixView', 'View Distance')]
CHECKS = [('Mcm2fixVSync', 'VSync'), ('Mcm2fixWideView', 'Widescreen View *'), ('Mcm2fixMotionBlur', 'Motion Blur'),
          ('Mcm2fixSmoothEdges', 'Smooth Tree Edges'), ('Mcm2fixShadows', 'Soft Rider Shadows *'),
          ('Mcm2fixReflections', 'Bike Reflections *'), ('Mcm2fixTextures', 'Replacement Textures *')]
ROWS = len(SLIDERS) + len(CHECKS)
BOXH = ROWTOP + ROWS * ROWSTEP + 22


def place(by, name, x, y, w, h):
    by[name]['Xywh'] = f'{x},{y},{w},{h}'


def max_graphics(controls):
    by = {s['_name']: s for s in controls}
    place(by, 'DisplayResolutionBox', LEFTX, TOPY, BOXW, TOPH)
    place(by, 'DisplayResolutionDropDown', 25, 50, 178, 21 + 200)
    place(by, 'TerrainQualityBox', RIGHTX, TOPY, BOXW, TOPH)
    place(by, 'TerrainQualitySliderBar', 11, 28, 204, 16)
    place(by, 'rbwLabel6', 11, 45, 15, 17)
    place(by, 'rbwLabel7', 197, 45, 18, 17)
    place(by, 'GraphicFeaturesBox', LEFTX, LOWY, BOXW, FEATURESH)
    place(by, 'ShadowsCheckBox', 8, ROWTOP, 211, 20)
    place(by, 'ParticlesCheckBox', 8, ROWTOP + ROWSTEP, 211, 20)
    place(by, 'SkyCheckBox', 8, ROWTOP + 2 * ROWSTEP, 211, 20)
    for n in ('DisplayResolutionBox', 'TerrainQualityBox', 'GraphicFeaturesBox'):
        by[n]['_frame'] = True
    add = []
    add.append({'_name': 'Mcm2fixBox', 'Type': 'STATIC', 'Xywh': f'{RIGHTX},{LOWY},{BOXW},{BOXH}', '_frame': True})
    add.append({'_name': 'Mcm2fixBoxText', 'Type': 'STATICTEXT', 'Anchor': 'Mcm2fixBox', 'Xywh': '8,6,211,18',
                'Text': 'Enhancements', 'LocalTextId': '1'})
    for i, (n, label) in enumerate(SLIDERS):
        y = ROWTOP + i * ROWSTEP
        add.append({'_name': n + 'Text', 'Type': 'STATICTEXT', 'Anchor': 'Mcm2fixBox', 'Xywh': f'8,{y},140,18',
                    'Text': label})
        add.append({'_name': n, 'Type': 'HSCROLLBAR', 'Anchor': 'Mcm2fixBox', 'Xywh': f'148,{y + 2},70,16',
                    'AnimThumb': 'GSmallHSliderUp', '_track': 'GHorizontalSlider01'})
    for i, (n, label) in enumerate(CHECKS):
        y = ROWTOP + (len(SLIDERS) + i) * ROWSTEP
        add.append({'_name': n, 'Type': 'MULTISTATE', 'Anchor': 'Mcm2fixBox', 'Xywh': f'8,{y},211,20', 'Text': label,
                    'State0': 'CheckBoxOff', 'TextLMargin': '3'})
    add.append({'_name': 'Mcm2fixFootnote', 'Type': 'STATICTEXT', 'Anchor': 'Mcm2fixBox',
                'Xywh': f'8,{ROWTOP + ROWS * ROWSTEP},211,16', 'Text': '* after restarting the game'})
    for i, (n, label) in enumerate([('Mcm2fixBorderless', 'Borderless'), ('Mcm2fixExclusive', 'Exclusive')]):
        add.append({'_name': n, 'Type': 'RADIOBUTTON', 'Anchor': 'DisplayResolutionBox',
                    'Xywh': f'{118 if i else 8},28,{101 if i else 105},17', 'Text': label, 'TextAlignV': 'MIDDLE',
                    'TextLMargin': '3', 'AnimOn': 'RadioButtonOn', 'AnimOff': 'RadioButtonOff'})
    add.append({'_name': 'Mcm2fixMaX', 'Type': 'BUTTON', 'Xywh': f'{LEFTX + (BOXW - 117) // 2},{LOWY + FEATURESH + GAP + 12},117,33',
                'Text': 'More Settings', 'AnimNorm': 'AdvancedBttnUp', 'AnimPush': 'AdvancedBttnDown',
                'TextAlignV': 'MIDDLE', 'TextAlign': 'CENTER', '_label': True})
    return controls + add


# ---------------------------------------------------------------- output

# Each screen: its file, the game dialogs it copies (archives; the first is the
# base: its background covers the menu when it is full size), edits, and the
# control Esc / B / the right mouse button clicks (the first found).
SCREENS = [
    ('options', ['OPTBASE', 'OPTGAME', 'OPTGRAPH', 'OPTSOUND', 'OPTCONT', 'OPTMESS', 'OPTGAR']),
    ('optadvg', ['OPTADVG']),
    ('messbox1', ['MESSBOX1']),
    ('messbox2', ['MESSBOX2']),
    ('messbox3', ['MESSBOX3']),
    # The main menu: no background of its own (MCM2MaX's flag and picture,
    # MenuFlag, show through).
    ('mainmenu', ['MAINMENU']),
    ('single', ['SPBASE', 'SPEVENT', 'SPRINFO', 'SPBIKER']),
    ('multi', ['MPBASE', 'MPEVENT', 'MPRINFO', 'MPBIKER', 'MPOPT']),
    ('hostjoin', ['HOSTJOIN']),
    ('career', ['PCC', 'PCCBIKER', 'PCCNEXT', 'PCCSTAND']),
    ('careerstart', ['PCSTART']),
    ('careernew', ['PCNEW']),
    ('careerdeal', ['PCDEAL']),
    ('careerdone', ['PCDONE']),
    ('careerfinish', ['PCFINISH']),
    ('careerrace', ['PCLRACE']),
    ('careertrans', ['PCTRANS']),
    ('hiscores', ['HISCORES']),
    ('replays', ['GRBASE', 'GHOSTFIL', 'REPLYFIL']),
    ('globset', ['GLOBSET']),
    ('serial', ['SERPOPUP']),
    # The credits: the game's video shows through (only Back is copied).
    ('credits', ['CREDITS']),
]
NO_BACKGROUND = {'MAINMENU', 'CREDITS'}
EDITS = {'OPTGRAPH': max_graphics}
BACKS = ['Back', 'BackButton', 'CancelButton', 'Cancel', 'NoButton', 'No', 'CloseButton', 'Close', 'OKButton', 'OK']

HEAD = """<rml>
<head>
  <title>{title}</title>
  <link type="text/rcss" href="menus.rcss"/>
  <link type="text/rcss" href="{file}.rcss"/>
</head>
<!--
  A copy of the game's own {title} (MaXMenus=1), drawn over it while the game
  shows it. Made by tools/mcm2rml.py from the game's layouts, then edited by
  hand. 1dp = one pixel of the game's 640x480 menus. See ..\\README.txt.

  game-dialogs     the game dialogs (layouts) this screen copies
  game-opaque      it covers the whole menu (nothing under it is drawn)
  game-dialog="x"  the part that copies the game's dialog x: shown while the
                   game shows x, where the game put it
  game="Name"      tied to the game's control of that name on that dialog
  game-back        the control Esc / B / the right mouse button clicks
-->
<body game-dialogs="{dialogs}"{opaque}{back}>
<div id="screen">
"""


def dialog_size(g, arch):
    info = g.info[arch]
    try:
        _, _, w, h = [int(float(t)) for t in info.get('Xywh', '0,0,640,480').split(',')]
    except ValueError:
        w, h = 640, 480
    return w, h


def default_place(g, arch, base_full):
    """Where the game puts the dialog (MenuMaX moves it to the game's place)."""
    w, h = dialog_size(g, arch)
    if w >= 640 and h >= 480:
        return 0, 0
    if g.info[arch].get('PopUp', '0').startswith('-1'):
        return (640 - w) // 2, (480 - h) // 2
    return (20, 35) if base_full else (0, 0)


def write_screen(g, out_dir, file, archs):
    w = Writer(g)
    for a in archs:
        g.layout(a)
    base = archs[0]
    bw, bh = dialog_size(g, base)
    base_full = bw >= 640 and bh >= 480
    opaque = base_full and bool(g.info[base].get('BackgroundImage')) and base not in NO_BACKGROUND
    names = set()
    for a in archs:
        names |= {c['_name'] for c in g.layout(a)}
    back = next((b for b in BACKS if b in {c['_name'] for c in g.layout(base)}), None)
    title = file
    rml = [HEAD.format(title=title, file=file, dialogs=' '.join(a.lower() for a in archs),
                       opaque=' game-opaque' if opaque else '', back=f' game-back="{back}"' if back else '')]
    for a in archs:
        g.use(a)
        controls = g.layout(a)
        if a in EDITS:
            controls = EDITS[a](controls)
        hidden = covered(controls)
        controls = [c for c in controls if c['_name'] not in hidden]
        by, roots = resolve_page(controls)
        dw, dh = dialog_size(g, a)
        x, y = default_place(g, a, base_full)
        w.prefix = a.lower() + '-'
        bg = None if a in NO_BACKGROUND else g.info[a].get('BackgroundImage')
        did = 'd-' + a.lower()
        # (Magenta in a background is a hole the game draws into: a 3-D view,
        # e.g. the bike on Single Player > Bike; the game's own frame shows
        # there.)
        if bg and g.sprite(bg):
            w.rule('#' + did, f'decorator: image({g.sprite(bg)});')
        rml.append('')
        rml.append(f'  <!-- {a}.DAT -->')
        if hidden:
            rml.append(f'  <!-- not copied: {", ".join(sorted(hidden))} (the game draws a box over it) -->')
        rml.append(f'  <div id="{did}" class="dialog" game-dialog="{a.lower()}" style="left: {x}dp; top: {y}dp; width: {dw}dp; height: {dh}dp;">')
        # Pop-out panels (FX) last: the game draws them over the rest.
        for s in sorted(roots, key=lambda c: bool(c.get('FX', '').strip())):
            w.element(s, by, rml, 2)
        rml.append('  </div>')
    rml.append('</div>')
    rml.append('</body>')
    rml.append('</rml>')
    open(os.path.join(out_dir, file + '.rml'), 'w').write('\n'.join(rml) + '\n')
    open(os.path.join(out_dir, file + '.rcss'), 'w').write(
        f'/* {file}.rml: each control\'s own pictures (from the game\'s layouts). Made by tools/mcm2rml.py. */\n' +
        '\n'.join(w.rcss) + '\n')


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    g = Game(sys.argv[1])
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    g.layout('GLOBAL')
    g.layout('OPTBASE')
    for file, archs in SCREENS:
        write_screen(g, out_dir, file, archs)
    g.use('OPTBASE')
    plate = g.sprite('GPlate03', 'frame=2') if g.where('GPlate03') else None
    if not plate:
        g.layout('OPTGRAPH')
        plate = g.sprite('GPlate03', 'frame=2')
    pw, ph = g.size('GPlate03')
    shared = SHARED.format(
        cb_off=g.sprite('CheckBoxOff'), cb_on=g.sprite('CheckBoxOn'),
        cb_off_grey=g.sprite('CheckBoxOffGrey'), cb_on_grey=g.sprite('CheckBoxOnGrey'),
        rb_off=g.sprite('RadioButtonOff'), rb_on=g.sprite('RadioButtonOn'),
        rb_off_grey=g.sprite('RadioButtonOffGrey'), rb_on_grey=g.sprite('RadioButtonOnGrey'),
        dd_text=g.sprite('DropDown05'), dd_arrow=g.sprite('DownArrowUp'), dd_arrow_lit=g.sprite('DownArrowLit'),
        dd_arrow_down=g.sprite('DownArrowDown'), dd_list=g.sprite('DropDownBG05'), dd_scroll=g.sprite('VerticleScroll04'),
        dd_thumb=g.sprite('ElevatorThumbUp'), plate=plate, pw=pw, ph=ph, pw2=pw - 4, ph2=ph - 4)
    open(os.path.join(out_dir, 'menus.rcss'), 'w').write(shared)
    print('wrote', ', '.join(f for f, _ in SCREENS), 'and menus.rcss to', out_dir)


SHARED = '''/* Shared by the game menu copies (menus/*.rml). 1dp = one pixel of the
   game's 640x480 menus. Made by tools/mcm2rml.py, then edited by hand. Pictures:
   /dat/<ARCHIVE>/<file> is the game's own (UI\\<ARCHIVE>.DAT); options after
   '?': black (black is see-through too), opaque, frame=N (only the outer N
   pixels), bar (a slider track without its tick marks). */

body, div, span, p {{ display: block; }}
span {{ display: inline; }}
body {{
  width: 100%;
  height: 100%;
  font-family: Arial;
  font-weight: bold;
  font-size: 13dp;
  line-height: 15dp;
  color: #ffffff;
  font-effect: shadow(1dp 1dp #000000);
  white-space: nowrap;
}}
/* The 640x480 menu area in the middle of the window. */
#screen {{
  position: absolute;
  left: 50%;
  top: 50%;
  width: 640dp;
  height: 480dp;
  margin-left: -320dp;
  margin-top: -240dp;
}}
#screen div, #screen input, #screen select {{ position: absolute; box-sizing: border-box; }}
#screen span {{ position: static; }}
#screen .list div {{ position: static; }}
.game-hidden {{ display: none; }}

/* Everything that can be picked: arrows / d-pad move, Enter / A clicks. */
.tab, .button, .check, .radio, .pick, .cycle, .slider, .dropdown, .edit, .list .row {{ tab-index: auto; nav: auto; focus: auto; }}
.disabled {{ tab-index: none; nav: none; focus: none; }}

/* Box plates on MCM2MaX's own layouts: the edge of a game plate only. */
@spritesheet plate {{
  src: {plate};
  plate-outer: 0px 0px {pw}px {ph}px;
  plate-inner: 2px 2px {pw2}px {ph2}px;
}}

/* Tabs: the picked one is lit (.on). */
.tab {{ text-align: center; }}

/* Checkboxes and radio buttons: the box, then the text. */
.check .box, .radio .box {{ left: 0; top: 0; width: 14dp; height: 18dp; }}
.radio .box {{ height: 14dp; }}
.check .box {{ decorator: image({cb_off}); }}
.check.on .box {{ decorator: image({cb_on}); }}
.check.disabled .box {{ decorator: image({cb_off_grey}); }}
.check.on.disabled .box {{ decorator: image({cb_on_grey}); }}
.radio .box {{ decorator: image({rb_off}); }}
.radio.on .box {{ decorator: image({rb_on}); }}
.radio.disabled .box {{ decorator: image({rb_off_grey}); }}
.radio.on.disabled .box {{ decorator: image({rb_on_grey}); }}
.disabled {{ color: #8c8c8c; }}

/* A setting that steps through its choices (Chat Box Mode). */
.cycle {{ text-align: center; }}

/* A radio button drawn as a picture of its own. */
.pick {{ text-align: center; }}

/* Lists: the rows the game shows now; .on the chosen one. */
.list {{ overflow: hidden; padding: 2dp 4dp; }}
.list .row {{ display: block; height: 16dp; line-height: 16dp; }}
.list .row:hover {{ background-color: #3a4a8c80; }}
.list .row.on {{ background-color: #3a4a8c; }}
/* A control's tooltip (its title=, the game's tooltip text), shown by MenuMaX
   when the pointer rests on it. */
#max-tooltip {{ position: absolute; z-index: 100; padding: 2dp 5dp; background-color: #fff8d0;
  border: 1dp #000000; color: #000000; font-size: 12dp; font-effect: none; white-space: nowrap; pointer-events: none; }}

/* Edit boxes. */
.edit {{ padding-top: 3dp; }}

/* Vertical sliders (a list's scroll bar). */
.vslider slidertrack {{ width: 100%; }}

/* Sliders: the track and its thumb (each slider's own pictures below). */
.slider slidertrack {{ height: 100%; }}
.slider sliderarrowdec, .slider sliderarrowinc {{ width: 0; height: 0; }}

/* Dropdowns: the text box, the arrow, the list. */
.dropdown {{ text-align: left; }}
.dropdown selectvalue {{ display: block; width: auto; margin-right: 21dp; height: 18dp; padding: 3dp 4dp 0 4dp; decorator: image({dd_text}); }}
.dropdown selectarrow {{ display: block; width: 21dp; height: 21dp; decorator: image({dd_arrow}); }}
.dropdown:hover selectarrow, .dropdown:focus-visible selectarrow {{ decorator: image({dd_arrow_lit}); }}
.dropdown selectarrow:active {{ decorator: image({dd_arrow_down}); }}
.dropdown selectbox {{
  margin-top: 0;
  max-height: 200dp;
  overflow-y: auto;
  background-color: #10183a;
  border: 1dp #5a6290;
}}
.dropdown selectbox option {{ padding: 0 4dp; height: 16dp; line-height: 16dp; }}
.dropdown selectbox option:hover, .dropdown selectbox option:checked {{ background-color: #3a4a8c; color: #ffffff; }}
.dropdown selectbox scrollbarvertical {{ width: 21dp; decorator: image({dd_scroll}); }}
.dropdown selectbox scrollbarvertical sliderbar {{ width: 17dp; margin-left: 2dp; decorator: image({dd_thumb}); }}
.dropdown selectbox scrollbarvertical sliderarrowdec, .dropdown selectbox scrollbarvertical sliderarrowinc {{ width: 0; height: 0; }}
'''

if __name__ == '__main__':
    main()
