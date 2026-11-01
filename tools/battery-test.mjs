import { execFileSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

// Host build of Battery.h tests: CXX overrides, else the first compiler on PATH.
const exe = resolve('build', process.platform === 'win32' ? 'battery-test.exe' : 'battery-test');
mkdirSync('build', { recursive: true });
const gnu = ['-std=c++11', '-Wall', '-Wextra', '-Werror', 'tools/battery-test.cpp', '-o', exe];
const msvc = ['/nologo', '/std:c++14', '/utf-8', '/W4', '/WX', '/EHsc', 'tools/battery-test.cpp',
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
console.log('Battery decode/format tests passed.');
