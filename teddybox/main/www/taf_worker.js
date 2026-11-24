/* TeddyBox: audio -> TAF (tonie file) in the browser, as a Web Worker.
 *
 * The page decodes the audio files itself (Web Audio, 48 kHz) and sends each
 * file as one chapter. Here: Opus encoding with libopus (opusenc.js, from
 * opus-recorder, MIT/BSD) in CELT-only mode, 60 ms frames, like the
 * original tonies, then the TAF layout:
 *   - 4096 bytes protobuf header (SHA-1 and length of the audio data,
 *     audio ID, chapter pages), then the Ogg stream
 *   - first 4096 bytes of the stream: OpusHead and OpusTags pages
 *   - every further Ogg page exactly 4096 bytes, only whole packets; the
 *     gap is filled with Opus padding in the packets
 *   - each chapter starts on a new page
 *
 * Messages in:  {cmd:'start', bitrate, audioId}
 *               {cmd:'chapter', l:Float32Array, r:Float32Array} (48 kHz)
 *               {cmd:'finish'}
 * Messages out: {type:'ready'} {type:'progress', done, total}
 *               {type:'chapter', index} {type:'done', blob, chapters, bytes, seconds}
 *               {type:'error', message}
 */
'use strict';
var Module = {
    onRuntimeInitialized: function () {
        ready = true;
        postMessage({type: 'ready'});
    }
};
var ready = false;
importScripts('opusenc.js' + self.location.search);

const PAGE = 4096;
const FRAME = 2880;          /* 60 ms at 48 kHz */
const MAX_PACKET = 4000;
const APP_RESTRICTED_LOWDELAY = 2051; /* CELT only: the box decodes nothing else */

/* ---- Ogg CRC ---- */
const crcTable = new Uint32Array(256);
for (let i = 0; i < 256; i++) {
    let r = i << 24;
    for (let j = 0; j < 8; j++) r = (r & 0x80000000) ? ((r << 1) ^ 0x04c11db7) : (r << 1);
    crcTable[i] = r >>> 0;
}
function oggCrc(b) {
    let c = 0;
    for (let i = 0; i < b.length; i++) c = ((c << 8) ^ crcTable[((c >>> 24) ^ b[i]) & 0xff]) >>> 0;
    return c >>> 0;
}

/* ---- SHA-1 (crypto.subtle is not available on http pages) ---- */
function Sha1() {
    this.h = new Uint32Array([0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0]);
    this.buf = new Uint8Array(64);
    this.n = 0;
    this.len = 0;
    this.w = new Uint32Array(80);
}
Sha1.prototype.block = function (b, o) {
    const w = this.w, h = this.h;
    for (let i = 0; i < 16; i++) w[i] = (b[o + 4 * i] << 24 | b[o + 4 * i + 1] << 16 | b[o + 4 * i + 2] << 8 | b[o + 4 * i + 3]) >>> 0;
    for (let i = 16; i < 80; i++) { const x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = (x << 1 | x >>> 31) >>> 0; }
    let a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
    for (let i = 0; i < 80; i++) {
        let f, k;
        if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5a827999; }
        else if (i < 40) { f = bb ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = bb ^ c ^ d; k = 0xca62c1d6; }
        const t = ((a << 5 | a >>> 27) + f + e + k + w[i]) >>> 0;
        e = d; d = c; c = (bb << 30 | bb >>> 2) >>> 0; bb = a; a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
};
Sha1.prototype.update = function (b) {
    let i = 0;
    this.len += b.length;
    if (this.n) {
        while (this.n < 64 && i < b.length) this.buf[this.n++] = b[i++];
        if (this.n < 64) return;
        this.block(this.buf, 0);
        this.n = 0;
    }
    for (; i + 64 <= b.length; i += 64) this.block(b, i);
    while (i < b.length) this.buf[this.n++] = b[i++];
};
Sha1.prototype.digest = function () {
    const bits = this.len * 8, pad = new Uint8Array(((this.n < 56) ? 56 : 120) - this.n + 8);
    pad[0] = 0x80;
    const hi = Math.floor(bits / 4294967296), lo = bits >>> 0;
    const p = pad.length;
    pad[p - 8] = hi >>> 24; pad[p - 7] = hi >>> 16; pad[p - 6] = hi >>> 8; pad[p - 5] = hi;
    pad[p - 4] = lo >>> 24; pad[p - 3] = lo >>> 16; pad[p - 2] = lo >>> 8; pad[p - 1] = lo;
    this.update(pad);
    const out = new Uint8Array(20);
    for (let i = 0; i < 5; i++) { out[4 * i] = this.h[i] >>> 24; out[4 * i + 1] = this.h[i] >>> 16; out[4 * i + 2] = this.h[i] >>> 8; out[4 * i + 3] = this.h[i]; }
    return out;
};

/* ---- Opus padding: make a packet exactly D bytes longer ---- */
/* smallest growth possible for this packet, 0 = cannot be padded */
function padMin(p) {
    const c = p[0] & 3;
    if (c === 3) return (p[1] & 0x40) ? 0 : 1;
    return 2;
}
function padPacket(p, D) {
    const c = p[0] & 3;
    let head, rest, extra;
    if (c === 3) { head = [p[0], p[1] | 0x40]; rest = p.subarray(2); extra = D - 1; }
    else if (c === 0) { head = [p[0] | 3, 0x40 | 1]; rest = p.subarray(1); extra = D - 2; }
    else if (c === 1) { head = [p[0] | 3, 0x40 | 2]; rest = p.subarray(1); extra = D - 2; }
    else { head = [p[0] | 3, 0x80 | 0x40 | 2]; rest = p.subarray(1); extra = D - 2; }
    /* extra = padding length bytes + padding: m bytes 255, one byte r, then 254*m + r zeros */
    const m = Math.floor(extra / 255), r = extra - 255 * m;
    const out = new Uint8Array(p.length + D);
    let o = 0;
    for (const x of head) out[o++] = x;
    for (let i = 0; i < m; i++) out[o++] = 255;
    out[o++] = r;
    out.set(rest, o);
    /* the rest stays zero: padding */
    return out;
}
const segs = l => Math.floor(l / 255) + 1;

/* ---- pages ---- */
let enc = 0, pcmPtr = 0, outPtr = 0, audioId = 0, seq = 0, granule = 0, preskip = 0;
let packets = [], pageSamples = 0;
let pages = [], sha = null, chapters = [], samplesIn = 0, padLast = 0, lastGranule = 0;

function oggPage(packetList, gran, flags) {
    let nseg = 0, body = 0;
    for (const p of packetList) { nseg += segs(p.length); body += p.length; }
    const b = new Uint8Array(27 + nseg + body);
    const v = new DataView(b.buffer);
    b.set([0x4f, 0x67, 0x67, 0x53], 0);
    b[5] = flags;
    v.setUint32(6, gran >>> 0, true);
    v.setUint32(10, Math.floor(gran / 4294967296), true);
    v.setUint32(14, audioId, true);
    v.setUint32(18, seq++, true);
    b[26] = nseg;
    let s = 27, o = 27 + nseg;
    for (const p of packetList) {
        let l = p.length;
        while (l >= 255) { b[s++] = 255; l -= 255; }
        b[s++] = l;
        b.set(p, o);
        o += p.length;
    }
    v.setUint32(22, oggCrc(b), true);
    return b;
}

function emit(page) {
    sha.update(page);
    pages.push(page);
}

/* fills the gap of a page with padding in its packets; false if not possible */
function fillPage(list) {
    let total = 27;
    for (const p of list) total += segs(p.length) + p.length;
    let R = PAGE - total;
    for (let i = list.length - 1; i >= 0 && R > 0; i--) {
        const p = list[i], min = padMin(p);
        if (!min) continue;
        const f0 = p.length + segs(p.length);
        let D = R;
        while (D >= min && (p.length + D + segs(p.length + D)) - f0 > R) D--;
        if (D < min) continue;
        list[i] = padPacket(p, D);
        R -= (list[i].length + segs(list[i].length)) - f0;
    }
    return R === 0;
}

function encodeFrame(l, r, off) {
    const f = Module.HEAPF32, base = pcmPtr >> 2;
    for (let i = 0; i < FRAME; i++) {
        const j = off + i;
        f[base + 2 * i] = j < l.length ? l[j] : 0;
        f[base + 2 * i + 1] = j < r.length ? r[j] : 0;
    }
    const n = Module._opus_encode_float(enc, pcmPtr, FRAME, outPtr, MAX_PACKET);
    if (n < 0) throw new Error('opus_encode ' + n);
    return Module.HEAPU8.slice(outPtr, outPtr + n);
}
const SILENCE = new Float32Array(0);

function flushPage(final, endGranule) {
    if (!packets.length) return;
    if (!fillPage(packets)) {
        /* rare (one packet at a 255 boundary): one more silent frame */
        packets.push(encodeFrame(SILENCE, SILENCE, 0));
        pageSamples += FRAME;
        granule += FRAME;
        if (!fillPage(packets)) throw new Error('page padding');
    }
    const g = final ? Math.max(lastGranule, Math.min(granule, endGranule)) : granule;
    lastGranule = g;
    emit(oggPage(packets, g, final ? 4 : 0));
    packets = [];
}

function addPacket(p) {
    let total = 27, nseg = 0;
    for (const q of packets) { total += segs(q.length) + q.length; nseg += segs(q.length); }
    if (packets.length && (total + segs(p.length) + p.length > PAGE || nseg + segs(p.length) > 255)) flushPage(false);
    packets.push(p);
    granule += FRAME;
}

function ctl(req, val) {
    const a = Module._malloc(4);
    Module.HEAP32[a >> 2] = val;
    Module._opus_encoder_ctl(enc, req, a);
    Module._free(a);
}
function ctlGet(req) {
    const out = Module._malloc(4), a = Module._malloc(4);
    Module.HEAP32[a >> 2] = out;
    Module._opus_encoder_ctl(enc, req, a);
    const v = Module.HEAP32[out >> 2];
    Module._free(a);
    Module._free(out);
    return v;
}

function start(bitrate, id) {
    const err = Module._malloc(4);
    enc = Module._opus_encoder_create(48000, 2, APP_RESTRICTED_LOWDELAY, err);
    const e = Module.HEAP32[err >> 2];
    Module._free(err);
    if (!enc || e) throw new Error('opus_encoder_create ' + e);
    ctl(4002, bitrate);          /* OPUS_SET_BITRATE */
    ctl(4006, 1);                /* OPUS_SET_VBR */
    ctl(4010, 10);               /* OPUS_SET_COMPLEXITY */
    preskip = ctlGet(4027);      /* OPUS_GET_LOOKAHEAD */
    pcmPtr = Module._malloc(FRAME * 2 * 4);
    outPtr = Module._malloc(MAX_PACKET);
    audioId = id >>> 0;
    seq = 0; granule = 0; lastGranule = 0; padLast = 0; packets = []; pages = []; chapters = []; samplesIn = 0;
    sha = new Sha1();
    /* OpusHead and OpusTags: together the first 4096 bytes */
    const head = new Uint8Array(19);
    head.set([0x4f, 0x70, 0x75, 0x73, 0x48, 0x65, 0x61, 0x64, 1, 2]);
    new DataView(head.buffer).setUint16(10, preskip, true);
    new DataView(head.buffer).setUint32(12, 48000, true);
    const p0 = oggPage([head], 0, 2);
    let L = PAGE - p0.length - 27;
    while (L + segs(L) > PAGE - p0.length - 27) L--;
    if (L + segs(L) !== PAGE - p0.length - 27) throw new Error('tags size');
    const tags = new Uint8Array(L);
    const te = new TextEncoder();
    const vendor = te.encode('TeddyBox');
    const dv = new DataView(tags.buffer);
    let o = 0;
    tags.set(te.encode('OpusTags'), o); o += 8;
    dv.setUint32(o, vendor.length, true); o += 4;
    tags.set(vendor, o); o += vendor.length;
    dv.setUint32(o, 1, true); o += 4;
    const padLen = L - o - 4;
    dv.setUint32(o, padLen, true); o += 4;
    const c = te.encode('PAD=');
    tags.set(c, o);
    tags.fill(0x30, o + c.length);
    emit(p0);
    emit(oggPage([tags], 0, 0));
}

function chapter(l, r) {
    /* the chapter starts on a new page: its index counted in 4096 blocks
       of the stream, the first chapter is 0 (the box adds the header block) */
    flushPage(false);
    chapters.push(chapters.length ? pages.length - 1 : 0);
    const n = Math.max(l.length, r.length);
    for (let off = 0; off < n; off += FRAME) {
        addPacket(encodeFrame(l, r, off));
        if ((off / FRAME) % 50 === 0) postMessage({type: 'progress', done: off, total: n});
    }
    samplesIn += n;
    padLast = Math.ceil(n / FRAME) * FRAME - n;
    postMessage({type: 'progress', done: n, total: n});
}

function finish() {
    /* the encoder delays by the lookahead: one more silent frame for it */
    /* granule of the end: decoder output, which starts with "preskip"
       samples to drop; the zeros after the last chapter are cut off */
    const end = granule - padLast + preskip;
    addPacket(encodeFrame(SILENCE, SILENCE, 0));
    flushPage(true, end);
    Module._free(pcmPtr);
    Module._free(outPtr);
    Module._opus_encoder_destroy(enc);
    enc = 0;
    const digest = sha.digest();
    let audioBytes = 0;
    for (const p of pages) audioBytes += p.length;
    /* protobuf: 1 sha1_hash, 2 num_bytes, 3 audio_id, 4 track_page_nums (packed), 5 _fill */
    const pb = [];
    const varint = v => { while (v >= 128) { pb.push((v % 128) | 128); v = Math.floor(v / 128); } pb.push(v); };
    pb.push(0x0a, 20); for (const x of digest) pb.push(x);
    pb.push(0x10); varint(audioBytes);
    pb.push(0x18); varint(audioId);
    const tp = [];
    { const save = pb.length; for (const c of chapters) varint(c); tp.push(...pb.splice(save)); }
    pb.push(0x22); varint(tp.length); pb.push(...tp);
    const left = PAGE - 4 - pb.length;
    /* fill: tag, length varint, zeros; find the fill length that fits */
    let fill = left - 2;
    for (; fill >= 0; fill--) {
        let vl = 1, x = fill;
        while (x >= 128) { vl++; x = Math.floor(x / 128); }
        if (1 + vl + fill === left) break;
    }
    if (fill < 0) throw new Error('header fill');
    pb.push(0x2a); varint(fill);
    const header = new Uint8Array(PAGE);
    new DataView(header.buffer).setUint32(0, PAGE - 4, false);
    header.set(pb, 4);
    const blob = new Blob([header].concat(pages), {type: 'application/octet-stream'});
    pages = [];
    postMessage({type: 'done', blob: blob, chapters: chapters.length, bytes: PAGE + audioBytes, seconds: samplesIn / 48000});
}

self.onmessage = function (ev) {
    const d = ev.data;
    try {
        if (!ready) throw new Error('encoder not loaded');
        if (d.cmd === 'start') start(d.bitrate || 96000, d.audioId || Math.floor(Date.now() / 1000));
        else if (d.cmd === 'chapter') { chapter(d.l, d.r || d.l); postMessage({type: 'chapter', index: chapters.length - 1}); }
        else if (d.cmd === 'finish') finish();
    } catch (e) {
        postMessage({type: 'error', message: String(e && e.message || e)});
    }
};
