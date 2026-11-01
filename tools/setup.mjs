import { execFileSync } from 'node:child_process';
import { mkdirSync, existsSync, writeFileSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { cli, arduinoUser } from './platform.mjs';

const deps = JSON.parse(readFileSync('firmware/dependencies.json'));
mkdirSync(arduinoUser, { recursive: true });
// Quoted forward-slash path keeps the YAML valid for Windows drive letters.
writeFileSync('.deps/arduino-cli.yaml', `directories:\n  user: ${JSON.stringify(arduinoUser.replaceAll('\\', '/'))}\n`);
const args = ['--config-file', resolve('.deps/arduino-cli.yaml')];
const installed = execFileSync(cli, [...args, 'core', 'list'], { encoding: 'utf8' });
if (!installed.includes('3.3.12')) throw Error('Install the pinned esp32:esp32@3.3.12 core first.');
execFileSync(cli, [...args, 'lib', 'install', ...deps.libraries], { stdio: 'inherit' });
mkdirSync('.deps/waveshare', { recursive: true });
for (const name of ['es8311.c', 'es8311.h', 'es8311_reg.h']) {
  const dst = `.deps/waveshare/${name}`;
  if (!existsSync(dst)) execFileSync('curl', ['--fail', '--location', '--retry', '2', '--output', dst,
    `https://raw.githubusercontent.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16/${deps.waveshareCommit}/examples/arduino/07_ES8311/${name}`], { stdio: 'inherit' });
}
execFileSync('curl', ['--fail', '--location', '--output', '.deps/waveshare/LICENSE',
  `https://raw.githubusercontent.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16/${deps.waveshareCommit}/LICENSE`], { stdio: 'inherit' });
