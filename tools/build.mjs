import { execFileSync } from 'node:child_process';
import { readFileSync, mkdirSync, cpSync, existsSync } from 'node:fs';
import { resolve } from 'node:path';
const deps = JSON.parse(readFileSync('firmware/dependencies.json'));
const cli = process.env.ARDUINO_CLI || '/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli';
const sketch = resolve('build/sketch/ChimmyGoChi');
mkdirSync(sketch, { recursive: true });
cpSync('firmware/ChimmyGoChi', sketch, { recursive: true });
cpSync('firmware/ChimmyGoChi/generated/font_tc.c', `${sketch}/font_tc.c`);
cpSync('firmware/ChimmyGoChi/generated/font_clock.c', `${sketch}/font_clock.c`);
for (const f of ['es8311.c', 'es8311.h', 'es8311_reg.h']) {
  if (!existsSync(`.deps/waveshare/${f}`)) throw Error('Run npm run setup first.');
  cpSync(`.deps/waveshare/${f}`, `${sketch}/${f}`);
}
const args = ['--config-file', resolve('.deps/arduino-cli.yaml')];
if (process.argv[2] === 'upload') {
  // Native USB needs watchdog reset; baud changes can fail when it re-enumerates.
  const uploadPattern = '--chip {build.mcu} --port "{serial.port}" --baud 115200 {upload.flags} --before default-reset --after watchdog-reset write-flash {upload.erase_cmd} -z --flash-mode keep --flash-freq keep --flash-size keep {build.bootloader_addr} "{build.path}/{build.project_name}.bootloader.bin" 0x8000 "{build.path}/{build.project_name}.partitions.bin" 0x10000 "{build.path}/{build.project_name}.bin" {upload.extra_flags}';
  execFileSync(cli, [...args, 'upload', '--fqbn', deps.fqbn, '--port', process.env.PORT || '/dev/cu.usbmodem2301',
    '--upload-property', `upload.pattern_args=${uploadPattern}`,
    '--input-dir', resolve('build/firmware'), sketch], { stdio: 'inherit' });
  await new Promise(resolve=>setTimeout(resolve,3000));
} else {
  execFileSync(cli, [...args, 'compile', '--fqbn', deps.fqbn, '--build-path', resolve('build/cache'),
    '--output-dir', resolve('build/firmware'),
    '--build-property', `compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -I${resolve('firmware')}`,
    '--build-property', `compiler.cpp.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -I${resolve('firmware')}`,
    sketch], { stdio: 'inherit' });
}
