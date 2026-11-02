import { execFileSync } from 'node:child_process';
import { readFileSync, mkdirSync, copyFileSync, existsSync, readdirSync, statSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { cli, findPort, buildCache, sketchDir as sketch, includeDir } from './platform.mjs';
const deps = JSON.parse(readFileSync('firmware/dependencies.json'));
// Copy only files whose bytes changed: fresh timestamps make arduino-cli redo library
// detection for the whole sketch, which turned small edits into 10-20 minute builds.
function place(src, dst) {
  const data = readFileSync(src);
  if (existsSync(dst) && readFileSync(dst).equals(data)) return;
  copyFileSync(src, dst);
}
mkdirSync(sketch, { recursive: true });
if (includeDir !== resolve('firmware')) {
  mkdirSync(includeDir, { recursive: true });
  place('firmware/lv_conf.h', `${includeDir}/lv_conf.h`);
}
for (const f of readdirSync('firmware/ChimmyGoChi'))
  if (statSync(join('firmware/ChimmyGoChi', f)).isFile()) place(join('firmware/ChimmyGoChi', f), join(sketch, f));
place('firmware/ChimmyGoChi/generated/font_tc.c', `${sketch}/font_tc.c`);
execFileSync(process.execPath, ['tools/icons.mjs'], { stdio: 'inherit' });
place('firmware/ChimmyGoChi/generated/icons.c', `${sketch}/icons.c`);
place('firmware/ChimmyGoChi/generated/icons.h', `${sketch}/icons.h`);
for (const f of ['es8311.c', 'es8311.h', 'es8311_reg.h']) {
  if (!existsSync(`.deps/waveshare/${f}`)) throw Error('Run npm run setup first.');
  place(`.deps/waveshare/${f}`, `${sketch}/${f}`);
}
const args = ['--config-file', resolve('.deps/arduino-cli.yaml')];
if (process.argv[2] === 'upload') {
  // Native USB needs watchdog reset; baud changes can fail when it re-enumerates.
  const uploadPattern = '--chip {build.mcu} --port "{serial.port}" --baud 115200 {upload.flags} --before default-reset --after watchdog-reset write-flash {upload.erase_cmd} -z --flash-mode keep --flash-freq keep --flash-size keep {build.bootloader_addr} "{build.path}/{build.project_name}.bootloader.bin" 0x8000 "{build.path}/{build.project_name}.partitions.bin" 0x10000 "{build.path}/{build.project_name}.bin" {upload.extra_flags}';
  execFileSync(cli, [...args, 'upload', '--fqbn', deps.fqbn, '--port', await findPort(),
    '--upload-property', `upload.pattern_args=${uploadPattern}`,
    '--input-dir', resolve('build/firmware'), sketch], { stdio: 'inherit' });
  await new Promise(resolve=>setTimeout(resolve,3000));
} else {
  execFileSync(cli, [...args, 'compile', '--fqbn', deps.fqbn, '--build-path', buildCache,
    '--output-dir', resolve('build/firmware'),
    '--build-property', `compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -I${includeDir}`,
    '--build-property', `compiler.cpp.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -I${includeDir}`,
    sketch], { stdio: 'inherit' });
}
