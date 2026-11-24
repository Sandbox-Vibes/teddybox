#!/usr/bin/env python3
"""Runs the browser TAF encoder (teddybox/main/www/taf_worker.js) in Chrome
and writes the result: taf_run.py out.taf [chapter seconds ...]
Chapters are test signals (tones, sweep, noise), 44.1 kHz, resampled by
the browser to 48 kHz like real files. Needs playwright and Chrome."""
import asyncio, base64, http.server, os, sys, threading
from playwright.async_api import async_playwright

WWW = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../teddybox/main/www')
out = sys.argv[1]
secs = [float(x) for x in sys.argv[2:]] or [3.0, 0.5, 7.25]

PAGE = """<!doctype html><script>
async function run(secs){
  const w=new Worker('taf_worker.js?v=test');
  const q=[];let wake=null;w.onmessage=e=>{if(e.data.type==='progress')return;q.push(e.data);if(wake){wake();wake=null;}};
  w.onerror=e=>{q.push({type:'error',message:e.message});if(wake){wake();wake=null;}};
  const msg=async()=>{while(!q.length)await new Promise(r=>wake=r);return q.shift();};
  const wait=async t=>{for(;;){const m=await msg();if(m.type==='error')throw new Error(m.message);if(m.type===t)return m;}};
  await wait('ready');
  w.postMessage({cmd:'start',bitrate:96000,audioId:0x6123abcd});
  let k=0;
  for(const s of secs){
    // 44.1 kHz WAV-like buffer, decoded through Web Audio like a file
    const sr=44100,n=Math.round(s*sr),ch=k%2?1:2;
    const ctx=new OfflineAudioContext(ch,n,sr),b=ctx.createBuffer(ch,n,sr);
    for(let c=0;c<ch;c++){const d=b.getChannelData(c);for(let i=0;i<n;i++){const t=i/sr;
      d[i]=k===0?0.5*Math.sin(2*Math.PI*(c?660:440)*t):k===1?0.3*Math.sin(2*Math.PI*(200+2000*t/s)*t):0.2*(Math.random()*2-1);}}
    const wav=toWav(b);const dec=await new OfflineAudioContext(2,1,48000).decodeAudioData(wav);
    const l=new Float32Array(dec.length),r=new Float32Array(dec.length);
    dec.copyFromChannel(l,0);dec.copyFromChannel(r,dec.numberOfChannels>1?1:0);
    w.postMessage({cmd:'chapter',l,r},[l.buffer,r.buffer]);await wait('chapter');k++;
  }
  w.postMessage({cmd:'finish'});const d=await wait('done');
  const ab=new Uint8Array(await d.blob.arrayBuffer());let s='';for(let i=0;i<ab.length;i+=32768)s+=String.fromCharCode.apply(null,ab.subarray(i,i+32768));
  return {b64:btoa(s),chapters:d.chapters,seconds:d.seconds};
}
function toWav(b){const ch=b.numberOfChannels,n=b.length,v=new DataView(new ArrayBuffer(44+n*ch*2));
  const w=(o,s)=>{for(let i=0;i<s.length;i++)v.setUint8(o+i,s.charCodeAt(i));};
  w(0,'RIFF');v.setUint32(4,36+n*ch*2,true);w(8,'WAVE');w(12,'fmt ');v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,ch,true);
  v.setUint32(24,b.sampleRate,true);v.setUint32(28,b.sampleRate*ch*2,true);v.setUint16(32,ch*2,true);v.setUint16(34,16,true);w(36,'data');v.setUint32(40,n*ch*2,true);
  let o=44;for(let i=0;i<n;i++)for(let c=0;c<ch;c++){const x=Math.max(-1,Math.min(1,b.getChannelData(c)[i]));v.setInt16(o,x*32767,true);o+=2;}
  return v.buffer;}
</script>"""

class H(http.server.SimpleHTTPRequestHandler):
    def __init__(s, *a, **k): super().__init__(*a, directory=WWW, **k)
    def log_message(s, *a): pass
    def do_GET(s):
        if s.path.startswith('/test.html'):
            b = PAGE.encode(); s.send_response(200); s.send_header('Content-Type', 'text/html'); s.send_header('Content-Length', str(len(b))); s.end_headers(); s.wfile.write(b); return
        s.path = s.path.split('?')[0]; super().do_GET()

srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()

async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/usr/bin/google-chrome', args=['--no-sandbox'])
        pg = await b.new_page(); errs = []
        pg.on('console', lambda m: errs.append(m.text) if m.type == 'error' else None)
        await pg.goto(f'http://127.0.0.1:{srv.server_address[1]}/test.html')
        r = await pg.evaluate('s=>run(s)', secs)
        open(out, 'wb').write(base64.b64decode(r['b64']))
        print(f"{out}: {r['chapters']} chapters, {r['seconds']:.3f} s", *errs)
        await b.close()
asyncio.run(main())
