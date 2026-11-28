"""Rebuild the geometrically smoothed ONLOI logo. Python 3, standard library only.
Original ink bounds: x=11..170, y=3..56. Output ink bounds: 7.5 x 2.5 mm.
The low-resolution bitmap is interpreted geometrically, not an original brand master.
"""
import math
from pathlib import Path
ROOT=Path(__file__).resolve().parent
NAME='ONLOI_7_5x2_5mm'
S=2.5/53

def arc(cx,cy,rx,ry,a,b):
    n=max(1,round(abs(b-a)/0.5))
    return [(cx+rx*math.cos(math.radians(a+(b-a)*i/n)),cy+ry*math.sin(math.radians(a+(b-a)*i/n))) for i in range(n+1)]
def rect(x1,y1,x2,y2): return [(x1,y1),(x2,y1),(x2,y2),(x1,y2)]
def ring(cx,cy,rx,ry,ix,iy):
    return [arc(cx,cy,rx,ry,a-0.5,a+180.5)+arc(cx,cy,ix,iy,a+180.5,a-0.5) for a in (0,180)]
polys=ring(37.5,29.5,26.5,26.5,21,21)
polys+=[rect(35,12,40,34)]
polys+=[[(70,55.5),(70,40)]+arc(87,40,17,17,180,360)[1:]+[(104,55.5),(99,55.5),(99,40)]+arc(87,40,12,12.5,360,180)[1:]+[(75,55.5)]]
polys+=[rect(114,3,119,55)]
polys+=ring(141.5,39.5,16.5,16,11.5,11)
polys+=[rect(166,24,170,55),arc(168,11.5,2,3.5,0,360)[:-1]]
polys=[[((x-90.5)*S,(y-29.5)*S) for x,y in p] for p in polys]

def points(p,digits=6,flip=False):
    return '\n'.join(f'        (xy {x:.{digits}f} {(-y if flip else y):.{digits}f})' for x,y in p+[p[0]])

fp=f'''(footprint "{NAME}" (version 20211014) (generator onloi_vector_generator)
  (layer "F.Cu")
  (descr "ONLOI smoothed vector logo; visible F.SilkS artwork 7.5 x 2.5 mm")
  (tags "onloi logo silkscreen")
  (attr exclude_from_pos_files exclude_from_bom)
  (fp_text reference "REF**" (at 0 0) (layer "F.Fab") hide
    (effects (font (size 0.5 0.5) (thickness 0.08))))
  (fp_text value "{NAME}" (at 0 0) (layer "F.Fab") hide
    (effects (font (size 0.5 0.5) (thickness 0.08))))
'''
for p in polys: fp+='  (fp_poly (pts\n'+points(p)+'\n    ) (layer "F.SilkS") (width 0) (fill solid))\n'
fp+=')\n'
(ROOT/'ONLOI_Logos.pretty').mkdir(exist_ok=True)
(ROOT/'ONLOI_Logos.pretty'/f'{NAME}.kicad_mod').write_text(fp)
sym=f'''(kicad_symbol_lib (version 20211014) (generator onloi_vector_generator)
  (symbol "{NAME}" (in_bom no) (on_board yes)
'''
for i,(k,v) in enumerate([('Reference','LOGO'),('Value',NAME),('Footprint','ONLOI_Logos:'+NAME),('Datasheet','')]):
    sym+=f'    (property "{k}" "{v}" (id {i}) (at 0 0 0) (effects (font (size 1.27 1.27)) hide))\n'
sym+=f'    (symbol "{NAME}_0_1"\n'
for p in polys:
    sym+='      (polyline (pts\n'+points(p,4,True)+'\n        ) (stroke (width 0.001) (type default)) (fill (type outline)))\n'
sym+='    )\n  )\n)\n'
(ROOT/'ONLOI_Logos.kicad_sym').write_text(sym)
# SVG master has analytic curves; the KiCad fill boundaries sample at 0.5 degree.
art='''<g fill="black">
<path fill-rule="evenodd" d="M64 29.5 A26.5 26.5 0 1 1 11 29.5 A26.5 26.5 0 1 1 64 29.5 Z M58.5 29.5 A21 21 0 1 0 16.5 29.5 A21 21 0 1 0 58.5 29.5 Z"/>
<rect x="35" y="12" width="5" height="22"/>
<path d="M70 55.5 V40 A17 17 0 0 1 104 40 V55.5 H99 V40 A12 12.5 0 0 0 75 40 V55.5 Z"/>
<rect x="114" y="3" width="5" height="52"/>
<path fill-rule="evenodd" d="M158 39.5 A16.5 16 0 1 1 125 39.5 A16.5 16 0 1 1 158 39.5 Z M153 39.5 A11.5 11 0 1 0 130 39.5 A11.5 11 0 1 0 153 39.5 Z"/>
<rect x="166" y="24" width="4" height="31"/>
<ellipse cx="168" cy="11.5" rx="2" ry="3.5"/>
</g>'''
(ROOT/'ONLOI_Smooth_7_5x2_5mm.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" width="7.5mm" height="2.5mm" viewBox="11 3 159 53">'+art+'</svg>')
print('Created footprint, symbol library and SVG master.')
