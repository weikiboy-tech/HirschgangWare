import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
let source=readFileSync(new URL('../src/trigger.cpp',import.meta.url),'utf8').replaceAll('\r\n','\n');
source=source.replace('#include "clock.h"\n','');
for(const hook of [
  'aim_assist::MotionProtocol gMotion;\nbool gMotionSetup=false;\n',
  '    const bool motionReply=gMotion.reply(line,qpcUs());\n',
  '    gMotion.reset();gMotionSetup=false;\n',
]) {
  assert.equal(source.split(hook).length,2,'exactly one additive receive/reset hook');
  source=source.replace(hook,'');
}
assert.equal(source.split('    if(!motionReply)parseLine(line);').length,2);
source=source.replace('    if(!motionReply)parseLine(line);','    parseLine(line);');
const start=source.indexOf('bool motionDeviceReady()');
const end=source.indexOf('int monitorPresetCount()',start);
assert.ok(start>=0 && end>start,'additive motion adapter boundaries');
const adapter=source.slice(start,end);
assert.ok(!adapter.includes('beginClick(') && !adapter.includes('finishClick('));
source=source.slice(0,start)+source.slice(end);
// Baseline: 9ced35a, prior to the aim-assist feature. Only the new read-only
// readiness/motion adapter and explicit receive/reset hooks may differ.
assert.equal(createHash('sha256').update(source).digest('hex'),
  'ff4ff22fdbc36cdfb5c807fcd9a8235c7ba22c228a281ce38d69894e9c042a1b',
  'existing click, capture, config, device and timing implementation must remain unchanged');
const app=readFileSync(new URL('../src/esp.cpp',import.meta.url),'utf8');
const clickUpdate=app.indexOf('trigger::update(gDzTriggerRegions, ccx, ccy, true);');
const assistUpdate=app.indexOf('updateAimAssist(snap,controlCamera);');
assert.ok(clickUpdate>=0 && assistUpdate>clickUpdate,
  'existing click decision/output runs before optional movement');
console.log('PASS original trigger implementation preserved; optional movement adapter cannot invoke clicks');
