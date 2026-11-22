// SummerCart64 USB driver for the browser: Web Serial on desktop, WebUSB (FTDI) on Android.
// Protocol: https://github.com/Polprzewodnikowy/SummerCart64/blob/main/docs/03_usb_interface.md

const enc = new TextEncoder(), dec = new TextDecoder();
export const hex = (u8, max = 24) => Array.from(u8.slice(0, max), b => b.toString(16).padStart(2, '0')).join(' ') + (u8.length > max ? ' …' : '');
const u32 = (v) => [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255];
const readU32 = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

export const FTDI_VENDOR_ID = 0x0403;

// ---------- FTDI via WebUSB (Android's Web Serial does not list FTDI chips) ----------
// Emulates the parts of the Web Serial port interface that the driver uses.
export class FtdiUsbPort {
  constructor(device) { this.device = device; this.readable = null; this.writable = null; this.index = 1; }
  async ctrl(request, value) {
    const r = await this.device.controlTransferOut({ requestType: 'vendor', recipient: 'device', request, value, index: this.index });
    if (r.status !== 'ok') throw new Error(`FTDI control request ${request} failed`);
  }
  async open() {
    const d = this.device;
    if (!d.opened) await d.open();
    if (d.configuration === null) await d.selectConfiguration(1);
    const iface = d.configuration.interfaces[0];
    await d.claimInterface(iface.interfaceNumber);
    const eps = iface.alternate.endpoints;
    const epIn = eps.find(e => e.direction === 'in' && e.type === 'bulk');
    const epOut = eps.find(e => e.direction === 'out' && e.type === 'bulk');
    if (!epIn || !epOut) throw new Error('No suitable USB endpoints found');
    this.epIn = epIn.endpointNumber; this.epOut = epOut.endpointNumber; this.pkt = epIn.packetSize || 512;
    await this.ctrl(0, 0);          // reset
    await this.ctrl(0, 1);          // purge RX buffer
    await this.ctrl(0, 2);          // purge TX buffer
    await this.ctrl(9, 16);         // latency timer 16 ms: fewer empty status packets, less CPU load
    await this.ctrl(2, 0);          // no flow control
    const self = this;
    this.readable = new ReadableStream({
      async pull(controller) {
        for (;;) {
          let r;
          try { r = await d.transferIn(self.epIn, self.pkt * 32); }
          catch (e) { self.readable = null; controller.error(e); return; }
          const b = new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
          const parts = [];                                   // every USB packet starts with 2 status bytes
          for (let off = 0; off < b.length; off += self.pkt) {
            const end = Math.min(off + self.pkt, b.length);
            if (end - off > 2) parts.push(b.slice(off + 2, end));
          }
          if (parts.length) {
            const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0)); let o = 0;
            for (const p of parts) { out.set(p, o); o += p.length; }
            controller.enqueue(out); return;
          }
        }
      },
      cancel() { self.readable = null; }
    });
    this.writable = new WritableStream({
      async write(chunk) {
        for (let off = 0; off < chunk.length; off += 65536) {
          const r = await d.transferOut(self.epOut, chunk.subarray(off, Math.min(off + 65536, chunk.length)));
          if (r.status !== 'ok') throw new Error('USB write error');
        }
      }
    });
  }
  async setSignals({ dataTerminalReady }) { await this.ctrl(1, dataTerminalReady ? 0x0101 : 0x0100); }
  async getSignals() {
    const r = await this.device.controlTransferIn({ requestType: 'vendor', recipient: 'device', request: 5, value: 0, index: this.index }, 2);
    return { dataSetReady: !!(r.data.getUint8(0) & 0x20) };
  }
  async close() { this.readable = null; try { await this.device.close(); } catch {} }
}

// Ports the user already granted earlier – lets the page reconnect without a prompt.
export async function grantedPorts() {
  const out = [];
  if ('serial' in navigator) {
    try { out.push(...(await navigator.serial.getPorts()).map(p => ({ kind: 'serial', port: p }))); } catch {}
  }
  if ('usb' in navigator) {
    try {
      const devs = (await navigator.usb.getDevices()).filter(d => d.vendorId === FTDI_VENDOR_ID);
      out.push(...devs.map(d => ({ kind: 'usb', port: new FtdiUsbPort(d) })));
    } catch {}
  }
  return out;
}
export const requestSerialPort = async () => navigator.serial.requestPort();
export const requestUsbPort = async () => new FtdiUsbPort(await navigator.usb.requestDevice({ filters: [{ vendorId: FTDI_VENDOR_ID }] }));

// Detect ROM byte order and convert to big-endian (.z64)
export function toZ64(rom) {
  const m = readU32(rom, 0);
  if (m === 0x80371240) return { data: rom, fmt: '.z64' };
  const out = new Uint8Array(rom.length);
  if (m === 0x37804012) { for (let i = 0; i + 1 < rom.length; i += 2) { out[i] = rom[i + 1]; out[i + 1] = rom[i]; } return { data: out, fmt: '.v64 → swapped' }; }
  if (m === 0x40123780) { for (let i = 0; i + 3 < rom.length; i += 4) { out[i] = rom[i + 3]; out[i + 1] = rom[i + 2]; out[i + 2] = rom[i + 1]; out[i + 3] = rom[i]; } return { data: out, fmt: '.n64 → swapped' }; }
  return { data: rom, fmt: 'unknown byte order (unchanged)' };
}

export class Sc64 {
  constructor({ onUsbData = () => {}, onLog = () => {}, onDisconnect = () => {} } = {}) {
    Object.assign(this, { onUsbData, onLog, onDisconnect });
    this.port = this.reader = this.writer = null;
    this.rxBuf = new Uint8Array(0);
    this.pending = [];
    this.firmware = null;
  }
  get connected() { return !!this.writer; }

  async connect(port) {
    try {
      this.port = port;
      await port.open({ baudRate: 115200, bufferSize: 1 << 20 });
      await this.resetComm();
      this.writer = port.writable.getWriter();
      this.readLoop();
      const ident = dec.decode(await this.sendCmd('v', 0, 0, null, true, 3000));
      if (ident !== 'SCv2') throw new Error(`Unexpected identifier “${ident}” – is this really an SC64?`);
      const v = await this.sendCmd('V');
      this.firmware = `${(v[0] << 8) | v[1]}.${(v[2] << 8) | v[3]}.${readU32(v, 4)}`;
      return this.firmware;
    } catch (e) {
      await this.disconnect(true);
      throw e;
    }
  }

  async disconnect(silent = false) {
    const was = this.connected;
    try { await this.reader?.cancel(); } catch {}
    try { this.writer?.releaseLock(); } catch {}
    try { await this.port?.close(); } catch {}
    this.port = this.reader = this.writer = null;
    this.pending.splice(0).forEach(e => { clearTimeout(e.timer); e.reject(new Error('Disconnected')); });
    if (was && !silent) this.onDisconnect();
  }

  async sendCmd(id, arg0 = 0, arg1 = 0, data = null, expectResponse = true, timeoutMs = 15000) {
    if (!this.writer) throw new Error('SC64 not connected');
    const len = data ? data.length : 0;
    const pkt = new Uint8Array(12 + len);
    pkt.set(enc.encode('CMD'), 0); pkt[3] = id.charCodeAt(0);
    pkt.set(u32(arg0), 4); pkt.set(u32(arg1), 8);
    if (data) pkt.set(data, 12);
    let p = null;
    if (expectResponse) {
      p = new Promise((resolve, reject) => {
        const entry = { id, resolve, reject };
        entry.timer = setTimeout(() => {
          const i = this.pending.indexOf(entry);
          if (i >= 0) this.pending.splice(i, 1);
          reject(new Error(`Timeout on SC64 command '${id}'`));
        }, timeoutMs);
        this.pending.push(entry);
      });
    }
    await this.writer.write(pkt);
    return p;
  }

  // USB_WRITE: hand data to the N64 (the ROM picks it up with usb_poll/usb_read). No response.
  usbWrite(type, bytes) { return this.sendCmd('U', type, bytes.length, bytes, false); }

  async uploadRom(bytes, saveType = 0, onProgress = () => {}) {
    const { data, fmt } = toZ64(bytes);
    await this.sendCmd('R');                              // STATE_RESET
    await this.sendCmd('C', 5, 1);                        // BOOT_MODE = ROM (via bootloader, automatic CIC)
    await this.sendCmd('C', 6, saveType);                 // SAVE_TYPE
    const CHUNK = 1 << 20;
    for (let off = 0; off < data.length; off += CHUNK) {
      const part = data.subarray(off, Math.min(off + CHUNK, data.length));
      await this.sendCmd('M', off, part.length, part, true, 30000);   // MEMORY_WRITE into SDRAM at 0x0
      onProgress((off + part.length) / data.length);
    }
    return { size: data.length, fmt };
  }

  // ---------- internals ----------
  handlePacket(kind, id, data) {
    if (kind === 'CMP' || kind === 'ERR') {
      const idx = this.pending.findIndex(e => e.id === id);
      if (idx < 0) return;
      const [e] = this.pending.splice(idx, 1);
      clearTimeout(e.timer);
      kind === 'CMP' ? e.resolve(data) : e.reject(new Error(`SC64 reported an error for command '${id}'`));
      return;
    }
    if (id === 'U' && data.length >= 4) {                 // USB_WRITE from the N64
      const type = data[0];
      const len = (data[1] << 16) | (data[2] << 8) | data[3];
      this.onUsbData(type, data.slice(4, 4 + len));
    } else if (id === 'I') {
      this.onLog(`IS-Viewer: ${dec.decode(data)}`);
    } else if (id === 'G') {
      this.onLog('The N64 did not pick up data in time (discarded).', 'err');
    } else if (id === 'B') {
      this.onLog('SC64 button pressed.');
    }
  }

  parseRx() {
    while (this.rxBuf.length >= 8) {
      const kind = dec.decode(this.rxBuf.slice(0, 3));
      if (kind !== 'CMP' && kind !== 'ERR' && kind !== 'PKT') { this.rxBuf = this.rxBuf.slice(1); continue; }
      const len = readU32(this.rxBuf, 4);
      if (this.rxBuf.length < 8 + len) return;
      const id = String.fromCharCode(this.rxBuf[3]);
      const data = this.rxBuf.slice(8, 8 + len);
      this.rxBuf = this.rxBuf.slice(8 + len);
      try { this.handlePacket(kind, id, data); } catch (e) { this.onLog(`Packet handler failed: ${e.message}`, 'err'); }
    }
  }

  async readLoop() {
    const port = this.port;
    try {
      while (port.readable && this.port === port) {
        this.reader = port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await this.reader.read();
            if (done) break;
            const merged = new Uint8Array(this.rxBuf.length + value.length);
            merged.set(this.rxBuf); merged.set(value, this.rxBuf.length);
            this.rxBuf = merged; this.parseRx();
          }
        } finally { try { this.reader.releaseLock(); } catch {} }
      }
    } catch (e) {
      if (this.port === port) this.onLog(`Read error: ${e.message}`, 'err');
    }
    if (this.port === port) this.disconnect();            // cable pulled or device gone
  }

  async waitDsr(target, ms = 1000) {
    const t0 = performance.now();
    while (performance.now() - t0 < ms) {
      try { if ((await this.port.getSignals()).dataSetReady === target) return 'ok'; }
      catch (e) { return 'error: ' + e.message; }
      await sleep(10);
    }
    return 'timeout';
  }

  async resetComm() {
    // SC64 docs: DTR=1 → wait for DSR=1 → flush → DTR=0 → wait for DSR=0.
    // DTR must end up at 0 no matter what, otherwise the SC64 USB interface stays disabled.
    const steps = [];
    try { await this.port.setSignals({ dataTerminalReady: true }); } catch (e) { steps.push('DTR=1 error: ' + e.message); }
    steps.push('DSR→1 ' + await this.waitDsr(true));
    this.rxBuf = new Uint8Array(0);
    try { await this.port.setSignals({ dataTerminalReady: false }); } catch (e) { steps.push('DTR=0 error: ' + e.message); }
    steps.push('DSR→0 ' + await this.waitDsr(false));
    this.onLog(`USB reset: ${steps.join(', ')}`, 'muted');
  }
}
