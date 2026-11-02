import { execFileSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

// Builds and runs a firmware-logic test on the PC: node tools/host-test.mjs battery-test
// (tools/<name>.cpp). CXX overrides the compiler, else the first one on PATH.
const name = process.argv[2];
if (!/^[a-z-]+$/.test(name || '')) throw Error('Usage: node tools/host-test.mjs <test name>');
const exe = resolve('build', process.platform === 'win32' ? `${name}.exe` : name);
mkdirSync('build', { recursive: true });
const gnu = ['-std=c++17', '-Wall', '-Wextra', '-Werror', `tools/${name}.cpp`, '-o', exe];
const msvc = ['/nologo', '/std:c++17', '/utf-8', '/W4', '/WX', '/EHsc', `tools/${name}.cpp`,
  `/Fe:${exe}`, '/Fo:build/'];
const candidates = process.env.CXX ? [process.env.CXX] : ['c++', 'g++', 'clang++', 'cl'];
let built = false;
for (const cxx of candidates) {
  try {
    execFileSync(cxx, /(^|[\\/])cl(\.exe)?$/i.test(cxx) ? msvc : gnu, { stdio: 'inherit' });
    built = true;
    break;
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
}
if (!built) throw Error(`No C++ compiler found (tried ${candidates.join(', ')}). Install one or set CXX.`);
execFileSync(exe, { stdio: 'inherit' });
