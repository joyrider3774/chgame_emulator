// microSD card helpers shared by the CHGame and AKA web emulators and their
// card manager page (sdcard.html). The same file is in both repositories
// (chgame_emulator/web and aka_emulator/web): change both.
//
//   SDTools.kind(bytes)      'zip', 'image' or null
//   SDTools.unpack(buffer)   -> Promise of [{ path, dir, data, time }]: the
//                            files and folders in a zip file or a FAT12/16/32
//                            card image (a bare volume or one with an MBR
//                            partition table). Paths are relative, '/' separated.
//   SDTools.zip(entries)     -> Uint8Array: a zip file (stored, not
//                            compressed: card contents are mostly compressed
//                            already, and it keeps this small and quick)
//
// No library: zip entries are inflated with the browser's own
// DecompressionStream('deflate-raw').
var SDTools = (function () {
  'use strict';

  function u16(b, o) { return b[o] | (b[o + 1] << 8); }
  function u32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0; }

  // a path from an archive or image, made safe to put on the card: no
  // leading '/', no '.' or '..' parts, '/' separators
  function cleanPath(p) {
    var out = [];
    p.replace(/\\/g, '/').split('/').forEach(function (part) {
      if (part === '' || part === '.') return;
      if (part === '..') out.pop(); else out.push(part);
    });
    return out.join('/');
  }

  function isFatBootSector(b, o) {
    if (b.length < o + 512 || b[o + 510] !== 0x55 || b[o + 511] !== 0xAA) return false;
    if (b[o] !== 0xEB && b[o] !== 0xE9) return false;
    var bps = u16(b, o + 11);
    return (bps === 512 || bps === 1024 || bps === 2048 || bps === 4096) && b[o + 13] !== 0 && b[o + 16] !== 0;
  }

  function kind(b) {
    if (b.length >= 4 && b[0] === 0x50 && b[1] === 0x4B && (b[2] === 3 || b[2] === 5)) return 'zip';
    if (b.length >= 512 && b[510] === 0x55 && b[511] === 0xAA) return 'image';
    return null;
  }

  // ---- zip ----

  function inflate(data) {
    if (typeof DecompressionStream === 'undefined')
      return Promise.reject(new Error('this browser cannot unpack compressed zip files'));
    var stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Response(stream).arrayBuffer().then(function (b) { return new Uint8Array(b); });
  }

  function dosDate(date, time) {
    if (!date) return new Date();
    return new Date(1980 + (date >> 9), ((date >> 5) & 15) - 1, date & 31,
                    time >> 11, (time >> 5) & 63, (time & 31) * 2);
  }

  function unzip(b) {
    // the end of central directory record, searched from the back (a comment
    // of up to 64 KB may follow it)
    var eocd = -1;
    for (var i = b.length - 22; i >= Math.max(0, b.length - 22 - 65535); i--) {
      if (u32(b, i) === 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) return Promise.reject(new Error('not a zip file (no central directory)'));
    var count = u16(b, eocd + 10), at = u32(b, eocd + 16);
    if (count === 0xFFFF || at === 0xFFFFFFFF) return Promise.reject(new Error('zip64 files are not supported'));
    var utf8 = new TextDecoder('utf-8'), latin = new TextDecoder('latin1');
    var jobs = [];
    for (var n = 0; n < count; n++) {
      if (u32(b, at) !== 0x02014b50) return Promise.reject(new Error('damaged zip file'));
      var flags = u16(b, at + 8), method = u16(b, at + 10);
      var time = u16(b, at + 12), date = u16(b, at + 14);
      var csize = u32(b, at + 20), nlen = u16(b, at + 28), xlen = u16(b, at + 30), clen = u16(b, at + 32);
      var local = u32(b, at + 42);
      var raw = b.subarray(at + 46, at + 46 + nlen);
      var name = (flags & 0x800 ? utf8 : latin).decode(raw);
      at += 46 + nlen + xlen + clen;
      // macOS's resource forks and Finder files are not card contents
      if (/(^|\/)__MACOSX\//.test(name) || /(^|\/)\.DS_Store$/.test(name)) continue;
      var path = cleanPath(name);
      if (!path) continue;
      var stamp = dosDate(date, time);
      if (/\/$/.test(name)) { jobs.push(Promise.resolve({ path: path, dir: true, time: stamp })); continue; }
      var start = local + 30 + u16(b, local + 26) + u16(b, local + 28);
      var data = b.subarray(start, start + csize);
      if (method === 0) jobs.push(Promise.resolve({ path: path, dir: false, data: data.slice(), time: stamp }));
      else if (method === 8) jobs.push(inflate(data).then((function (p, t) {
        return function (d) { return { path: p, dir: false, data: d, time: t }; };
      })(path, stamp)));
      else return Promise.reject(new Error(name + ': zip compression method ' + method + ' is not supported'));
    }
    return Promise.all(jobs);
  }

  var crcTable = null;
  function crc32(d) {
    if (!crcTable) {
      crcTable = new Uint32Array(256);
      for (var n = 0; n < 256; n++) {
        var c = n;
        for (var k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
        crcTable[n] = c >>> 0;
      }
    }
    var crc = 0xFFFFFFFF;
    for (var i = 0; i < d.length; i++) crc = crcTable[(crc ^ d[i]) & 0xFF] ^ (crc >>> 8);
    return (crc ^ 0xFFFFFFFF) >>> 0;
  }

  function zip(entries) {
    var enc = new TextEncoder(), parts = [], central = [], offset = 0;
    function w16(a, o, v) { a[o] = v & 255; a[o + 1] = (v >>> 8) & 255; }
    function w32(a, o, v) { w16(a, o, v & 0xFFFF); w16(a, o + 2, v >>> 16); }
    entries.forEach(function (e) {
      var name = enc.encode(e.path + (e.dir ? '/' : ''));
      var data = e.dir ? new Uint8Array(0) : e.data;
      var t = e.time || new Date();
      var dtime = (t.getHours() << 11) | (t.getMinutes() << 5) | (t.getSeconds() >> 1);
      var ddate = (Math.max(0, t.getFullYear() - 1980) << 9) | ((t.getMonth() + 1) << 5) | t.getDate();
      var crc = crc32(data);
      var lh = new Uint8Array(30 + name.length);
      w32(lh, 0, 0x04034b50); w16(lh, 4, 20); w16(lh, 6, 0x800); w16(lh, 8, 0);
      w16(lh, 10, dtime); w16(lh, 12, ddate); w32(lh, 14, crc);
      w32(lh, 18, data.length); w32(lh, 22, data.length); w16(lh, 26, name.length); w16(lh, 28, 0);
      lh.set(name, 30);
      var ch = new Uint8Array(46 + name.length);
      w32(ch, 0, 0x02014b50); w16(ch, 4, 20); w16(ch, 6, 20); w16(ch, 8, 0x800); w16(ch, 10, 0);
      w16(ch, 12, dtime); w16(ch, 14, ddate); w32(ch, 16, crc);
      w32(ch, 20, data.length); w32(ch, 24, data.length); w16(ch, 28, name.length);
      w32(ch, 38, e.dir ? 0x10 : 0); w32(ch, 42, offset);
      ch.set(name, 46);
      parts.push(lh, data);
      central.push(ch);
      offset += lh.length + data.length;
    });
    var csize = central.reduce(function (s, c) { return s + c.length; }, 0);
    var end = new Uint8Array(22);
    w32(end, 0, 0x06054b50); w16(end, 8, entries.length); w16(end, 10, entries.length);
    w32(end, 12, csize); w32(end, 16, offset);
    var all = parts.concat(central, [end]);
    var out = new Uint8Array(all.reduce(function (s, p) { return s + p.length; }, 0)), o = 0;
    all.forEach(function (p) { out.set(p, o); o += p.length; });
    return out;
  }

  // ---- FAT card images ----

  function unfat(b) {
    // the volume: at sector 0 (a "superfloppy", as the emulators make) or
    // the first FAT partition of an MBR
    var base = 0;
    if (!isFatBootSector(b, 0)) {
      if (b[510] !== 0x55 || b[511] !== 0xAA) throw new Error('not a card image');
      base = -1;
      for (var p = 0; p < 4; p++) {
        var pe = 446 + p * 16, type = b[pe + 4], lba = u32(b, pe + 8);
        if (type === 0xEE) throw new Error('GPT partitioned images are not supported');
        if (type === 0x07) throw new Error('exFAT and NTFS cards are not supported: format the card FAT32');
        if ([0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E].indexOf(type) >= 0 && lba && isFatBootSector(b, lba * 512)) {
          base = lba * 512;
          break;
        }
      }
      if (base < 0) throw new Error('no FAT partition found in the image');
    }
    var bps = u16(b, base + 11), spc = b[base + 13], rsvd = u16(b, base + 14), nfats = b[base + 16];
    var rootEnts = u16(b, base + 17), tot = u16(b, base + 19) || u32(b, base + 32);
    var fatsz = u16(b, base + 22) || u32(b, base + 36);
    var rootSecs = Math.ceil(rootEnts * 32 / bps);
    var dataSec = rsvd + nfats * fatsz + rootSecs;
    var clusters = Math.floor((tot - dataSec) / spc);
    var bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    var fat = base + rsvd * bps, csize = spc * bps;
    if (base + dataSec * bps > b.length) throw new Error('the image is cut short');

    function next(c) {
      if (bits === 12) { var v = u16(b, fat + c + (c >> 1)); return c & 1 ? v >> 4 : v & 0xFFF; }
      if (bits === 16) return u16(b, fat + c * 2);
      return u32(b, fat + c * 4) & 0x0FFFFFFF;
    }
    var eoc = bits === 12 ? 0xFF8 : bits === 16 ? 0xFFF8 : 0x0FFFFFF8;
    function chain(c) {
      var out = [];
      while (c >= 2 && c < eoc && out.length <= clusters) { out.push(c); c = next(c); }
      return out;
    }
    function clusterData(c, size) {
      var list = chain(c), out = new Uint8Array(size === undefined ? list.length * csize : size), o = 0;
      for (var i = 0; i < list.length && o < out.length; i++) {
        var at = base + (dataSec + (list[i] - 2) * spc) * bps;
        var n = Math.min(csize, out.length - o);
        out.set(b.subarray(at, at + n), o);
        o += n;
      }
      return out;
    }

    var entries = [];
    function readDir(d, prefix, depth) {
      if (depth > 32) return;
      var lfn = [];
      for (var o = 0; o + 32 <= d.length; o += 32) {
        var first = d[o];
        if (first === 0) break;
        if (first === 0xE5) { lfn = []; continue; }
        var attr = d[o + 11];
        if (attr === 0x0F) {
          var s = '';
          [[1, 10], [14, 26], [28, 32]].forEach(function (r) {
            for (var i = o + r[0]; i < o + r[1]; i += 2) s += String.fromCharCode(u16(d, i));
          });
          lfn[(first & 0x1F) - 1] = s;
          continue;
        }
        if (attr & 0x08) { lfn = []; continue; }        // the volume label
        var name;
        if (lfn.length) {
          name = lfn.join('');
          var cut = name.indexOf('\u0000');
          if (cut >= 0) name = name.substring(0, cut);
          name = name.replace(/￿+$/, '');
        } else {
          var base8 = String.fromCharCode.apply(null, d.subarray(o, o + 8)).replace(/ +$/, '');
          var ext = String.fromCharCode.apply(null, d.subarray(o + 8, o + 11)).replace(/ +$/, '');
          if (first === 0x05) base8 = 'å' + base8.substring(1);
          if (d[o + 12] & 0x08) base8 = base8.toLowerCase();
          if (d[o + 12] & 0x10) ext = ext.toLowerCase();
          name = ext ? base8 + '.' + ext : base8;
        }
        lfn = [];
        if (name === '.' || name === '..') continue;
        var clus = u16(d, o + 26) | (bits === 32 ? u16(d, o + 20) << 16 : 0);
        var path = prefix ? prefix + '/' + name : name;
        var time = dosDate(u16(d, o + 24), u16(d, o + 22));
        if (attr & 0x10) {
          entries.push({ path: cleanPath(path), dir: true, time: time });
          if (clus) readDir(clusterData(clus), path, depth + 1);
        } else {
          entries.push({ path: cleanPath(path), dir: false, data: clusterData(clus, u32(d, o + 28)), time: time });
        }
      }
    }
    if (bits === 32) readDir(clusterData(u32(b, base + 44)), '', 0);
    else {
      var root = base + (rsvd + nfats * fatsz) * bps;
      readDir(b.subarray(root, root + rootEnts * 32), '', 0);
    }
    return entries;
  }

  function unpack(buffer) {
    var b = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
    var k = kind(b);
    if (k === 'zip') return unzip(b);
    if (k === 'image') {
      try { return Promise.resolve(unfat(b)); } catch (e) { return Promise.reject(e); }
    }
    return Promise.reject(new Error('neither a zip file nor a card image'));
  }

  return { kind: kind, unpack: unpack, zip: zip, cleanPath: cleanPath };
})();
