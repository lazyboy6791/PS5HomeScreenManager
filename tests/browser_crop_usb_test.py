# Browser checks of the crop window (needs Playwright with Chromium). Run from the source folder:
#   python3 tests/browser_crop_test.py   (after make host)
import sys,os,tempfile
sys.path.insert(0,'tests'); sys.argv=['x','build/host/homescreenmanager']
import test_host as t
from PIL import Image, ImageDraw
from playwright.sync_api import sync_playwright
tmp=tempfile.mkdtemp(); usb=os.path.join(tmp,'usb0'); os.makedirs(usb)
im=Image.new('RGB',(1200,1200),(30,160,90)); ImageDraw.Draw(im).rectangle((0,0,600,1200),fill=(220,160,30)); im.save(os.path.join(usb,'cover.png'))
db=os.path.join(tmp,'a.db'); t.make_db(db)
s=t.Server(db,os.path.join(tmp,'d'),{"HH_USB_FMT":os.path.join(tmp,'usb%d'),"HH_USER_APP_DIR":os.path.join(tmp,'apps')})
res=[]
def ok(c,m): print(('ok   ' if c else 'FAIL ')+m); res.append(c)
try:
  with sync_playwright() as p:
    b=p.chromium.launch(); pg=b.new_page(viewport={'width':1100,'height':900}); logs=[]
    pg.on('pageerror',lambda e: logs.append('ERR '+str(e)))
    pg.goto(t.BASE+'/'); pg.wait_for_timeout(1500)
    pg.evaluate("() => { editFolder(null); }")
    pg.click('#fmUsbBtn'); pg.wait_for_timeout(1500)
    n=pg.evaluate("() => usbPics.length"); ok(n>=1,'USB picture list has %d picture(s)'%n)
    pg.evaluate("() => pickUsb(0)"); pg.wait_for_timeout(800)
    ok(pg.is_visible('#cropView'),'crop window opens for a USB picture')
    pg.click('text=Use this crop'); pg.wait_for_timeout(600)
    r=pg.evaluate("() => [String(fmPendingUsb), fmPendingFile && fmPendingFile.type]")
    ok(r[0]=='null' and r[1]=='image/png','USB pick becomes a cropped file: %s'%r)
    pg.evaluate("() => { fmPendingFile=null; }"); pg.evaluate("() => pickUsb(0)"); pg.wait_for_timeout(500)
    pg.click('text=Use without cropping'); pg.wait_for_timeout(300)
    ok(pg.evaluate("() => !!fmPendingUsb && fmPendingFile === null"),'without cropping keeps the USB path')
    pg.evaluate("() => { fmPendingFile=null; fmPendingUsb=null; }"); pg.evaluate("() => pickUsb(0)"); pg.wait_for_timeout(500)
    pg.click('#cropView >> text=Cancel'); pg.wait_for_timeout(300)
    ok(pg.evaluate("() => !fmPendingUsb && !fmPendingFile"),'cancel selects nothing')
    ok(not logs,'no script errors '+str(logs)); b.close()
finally: s.stop()
print('ALL OK' if all(res) else 'SOME FAILED')
