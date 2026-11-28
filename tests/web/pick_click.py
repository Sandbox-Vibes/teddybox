#!/usr/bin/env python3
"""Setup page: a real mouse click on every file field (label.drop) must open
the file chooser. 27 Nov: the TAF and audio fields did nothing, because the
<progress> inside the label is labelable too and came before the <input>, so
the label activated the progress bar instead of the file input."""
import asyncio, http.server, os, sys, threading
from playwright.async_api import async_playwright

PAGE = os.environ.get('SETUP_HTML') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../teddybox/main/setup.html')


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.split('?')[0] == '/':
            body = open(PAGE, 'rb').read()
            ct = 'text/html; charset=utf-8'
        else:
            body, ct = b'{}', 'application/json'  # the page copes with empty answers
        self.send_response(200)
        self.send_header('Content-Type', ct)
        self.end_headers()
        self.wfile.write(body)


srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()


async def main():
    bad = 0
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/usr/bin/google-chrome', args=['--no-sandbox'])
        pg = await b.new_page(viewport={'width': 1200, 'height': 900})
        await pg.route('https://**', lambda r: r.abort())
        await pg.goto('http://127.0.0.1:%d/' % srv.server_port)
        await pg.wait_for_timeout(500)
        ids = await pg.evaluate("[...document.querySelectorAll('label.drop input[type=file]')].map(i=>i.id)")
        for fid in ids:
            tab = await pg.evaluate("id=>document.getElementById(id).closest('.tab').id.replace('tab-','')", fid)
            await pg.click('.tabs button[data-tab="%s"]' % tab)
            await pg.wait_for_timeout(400)  # let the tab settle (System: online update box)
            # fields in a closed <details> (Advanced) are opened as a user would
            await pg.evaluate("id=>{const d=document.getElementById(id).closest('details');if(d)d.open=true}", fid)
            lab = pg.locator('label.drop:has(#%s) .t b' % fid)
            ok = False
            # up to 3 tries: the System tab can still move (online update box)
            # between measuring and clicking; a real fault fails every time
            for _ in range(3):
                await lab.scroll_into_view_if_needed()
                bb = await lab.bounding_box()
                try:
                    async with pg.expect_file_chooser(timeout=1500):
                        await pg.mouse.click(bb['x'] + 5, bb['y'] + 5)
                    ok = True
                    break
                except Exception:
                    await pg.wait_for_timeout(300)
            if ok:
                print('OK   %s: click opens the file chooser' % fid)
            else:
                print('FAIL %s: click does not open the file chooser' % fid)
                bad += 1
        await b.close()
    if not ids:
        print('FAIL no file fields found')
        bad += 1
    sys.exit(1 if bad else 0)


asyncio.run(main())
