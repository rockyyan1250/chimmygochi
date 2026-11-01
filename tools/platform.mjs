import { existsSync } from 'node:fs';
import { homedir } from 'node:os';
import { join, resolve } from 'node:path';
import { SerialPort } from 'serialport';

// Arduino IDE 2 bundles arduino-cli; fall back to one on PATH.
const bundled = {
  darwin: ['/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli'],
  win32: [process.env.LOCALAPPDATA, process.env.ProgramFiles].filter(Boolean)
    .map(dir => join(dir, dir === process.env.LOCALAPPDATA ? 'Programs' : '', 'Arduino IDE',
      'resources', 'app', 'lib', 'backend', 'resources', 'arduino-cli.exe')),
}[process.platform] || [];
export const cli = process.env.ARDUINO_CLI || bundled.find(p => existsSync(p)) || 'arduino-cli';

// The compiler probes thousands of header paths; inside OneDrive (cloud placeholders) each
// lookup is slow enough to stall builds. On Windows keep libraries, sketch and cache local.
// Not under AppData: packaged (MSIX) terminals silently redirect new folders there.
const local = process.env.CHIMMY_LOCAL || (process.platform === 'win32'
  ? join(homedir(), '.chimmygochi') : null);
export const arduinoUser = local ? join(local, 'arduino') : resolve('.deps/arduino');
export const buildCache = local ? join(local, 'build-cache') : resolve('build/cache');
export const sketchDir = local ? join(local, 'sketch', 'ChimmyGoChi') : resolve('build/sketch/ChimmyGoChi');
// Holds lv_conf.h for library sources (-I).
export const includeDir = local ? join(local, 'include') : resolve('firmware');

// PORT overrides; otherwise pick the single Espressif native USB device (VID 303A).
export async function findPort() {
  if (process.env.PORT) return process.env.PORT;
  const ports = await SerialPort.list();
  const esp = ports.filter(p => (p.vendorId || '').toLowerCase() === '303a');
  if (esp.length === 1) return esp[0].path;
  const seen = ports.map(p => `${p.path}${p.vendorId ? ` (VID ${p.vendorId})` : ''}`).join(', ') || 'none';
  throw Error(`${esp.length ? 'Several' : 'No'} ESP32-S3 USB ports found; set PORT. Ports: ${seen}`);
}
