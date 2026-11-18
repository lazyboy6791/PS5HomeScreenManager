# Browser checks of the crop window (needs Playwright with Chromium). Run from the source folder:
#   python3 tests/browser_crop_test.py   (after make host)
import sys,os,tempfile,time
sys.path.insert(0,'tests'); sys.argv=['x','build/host/homescreenmanager']
import test_host as t
from PIL import Image, ImageDraw
from playwright.sync_api import sync_playwright
tmp=tempfile.mkdtemp()
im=Image.new('RGB',(1600,900),(200,40,40)); d=ImageDraw.Draw(im)
d.rectangle((800,0,1600,900),fill=(40,60,200)); d.ellipse((700,350,900,550),fill=(255,255,255))
for i in range(0,1600,100): d.line((i,0,i,900),fill=(255,255,255),width=2)
src=os.path.join(tmp,'wide.png'); im.save(src)
db=os.path.join(tmp,'a.db'); t.make_db(db)
s=t.Server(db,os.path.join(tmp,'d'),{"HH_USER_APP_DIR":os.path.join(tmp,'apps')})
res=[]
def ok(c,m):
  print(('ok   ' if c else 'FAIL ')+m); res.append(c)
try:
  with sync_playwright() as p:
    b=p.chromium.launch(); pg=b.new_page(viewport={'width':1100,'height':900}); logs=[]
    pg.on('console',lambda m: logs.append(m.text)); pg.on('pageerror',lambda e: logs.append('ERR '+str(e)))
    pg.goto(t.BASE+'/'); pg.wait_for_timeout(1500)
    pg.evaluate("editFolder(null)"); pg.wait_for_timeout(300)
    # 1. folder picture: square crop
    pg.set_input_files('#fmFile',src); pg.wait_for_timeout(600)
    ok(pg.is_visible('#cropView'),'crop window opens after choosing a file')
    st=pg.locator('#cropStage').bounding_box(); ok(abs(st['width']-st['height'])<2,'square frame for the folder picture')
    pg.screenshot(path='/tmp/cropt/1_open.png')
    info0=pg.inner_text('#cropInfo'); print('info:',info0)
    # drag left by 200px -> moves view right
    cx=st['x']+st['width']/2; cy=st['y']+st['height']/2
    pg.mouse.move(cx,cy); pg.mouse.down(); pg.mouse.move(cx-150,cy,steps=8); pg.mouse.up()
    pg.mouse.move(cx,cy); pg.mouse.wheel(0,-400); pg.wait_for_timeout(200)
    info1=pg.inner_text('#cropInfo'); ok(info1!=info0,'zoom with the wheel changes the result size: '+info1)
    pg.screenshot(path='/tmp/cropt/2_dragzoom.png')
    pg.click('text=Use this crop'); pg.wait_for_timeout(600)
    ok(not pg.is_visible('#cropView'),'window closes')
    r=pg.evaluate("""async function(){var f=fmPendingFile;var bm=await createImageBitmap(f);return [f.name,f.type,f.size,bm.width,bm.height]}()""") if False else pg.evaluate("(async function(){var f=fmPendingFile;var bm=await createImageBitmap(f);return [f.name,f.type,f.size,bm.width,bm.height]})()")
    print('result',r); ok(r[3]==r[4] and r[1]=='image/png','cropped result is a square PNG: %s'%r)
    ok(r[3]<900,'zoomed in so the crop is smaller than the full height (%d)'%r[3])
    # 2. cancel
    pg.evaluate("fmPendingFile=null"); pg.set_input_files('#fmFile',src); pg.wait_for_timeout(500)
    pg.click('#cropView >> text=Cancel'); pg.wait_for_timeout(200)
    ok(pg.evaluate("fmPendingFile")==None and not pg.is_visible('#cropView'),'cancel keeps nothing')
    # 3. as is
    pg.set_input_files('#fmFile',src); pg.wait_for_timeout(500)
    pg.click('text=Use without cropping'); pg.wait_for_timeout(200)
    ok(pg.evaluate("fmPendingFile && fmPendingFile.name")=='wide.png','without cropping keeps the original file')
    # 4. keyboard: background 16:9
    pg.evaluate("fmPendingFile=null"); pg.set_input_files('#fmBgFile',src); pg.wait_for_timeout(500)
    st=pg.locator('#cropStage').bounding_box(); ok(abs(st['width']/st['height']-16/9)<0.03,'16:9 frame for the background')
    pg.focus('#cropStage'); pg.keyboard.press('Enter'); ok('move' in pg.get_attribute('#cropStage','class'),'Enter switches move mode on')
    pg.keyboard.press('+'); pg.keyboard.press('+'); 
    for _ in range(5): pg.keyboard.press('ArrowRight')
    pg.screenshot(path='/tmp/cropt/3_keys.png')
    pg.click('text=Use this crop'); pg.wait_for_timeout(600)
    r=pg.evaluate("(async function(){var f=fm.bgFile;var bm=await createImageBitmap(f);return [f.name,f.type,f.size,bm.width,bm.height]})()")
    print('bg',r); ok(r[1]=='image/jpeg' and abs(r[3]/r[4]-16/9)<0.02,'background result is a 16:9 JPEG: %s'%r)
    # 5. small picture / wheel zoom limit
    print([l for l in logs if 'ERR' in l or 'rror' in l])
    ok(not [l for l in logs if l.startswith('ERR')],'no script errors')
    b.close()
finally:
  s.stop()
print('ALL OK' if all(res) else 'SOME FAILED')
