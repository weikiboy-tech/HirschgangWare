import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const source = file => "data:text/javascript;base64," +
  readFileSync(new URL(file, import.meta.url)).toString("base64");
const { Camera, imageToWorld, screenPoint, shortestTurn, worldPoint, worldToImage } = await import(source("../web/map.js"));
const { decode, LiveConnection } = await import(source("../web/live.js"));

const near = (a,b) => assert.ok(Math.abs(a-b) < 1e-7, a + " differs from " + b);
for (const [size,w,h] of [[15360,8192,8192],[12800,1024,2048],[8192,400,300]]) {
  assert.deepEqual(worldToImage(0,size,size,w,h),{x:0,y:0});
  assert.deepEqual(worldToImage(size,0,size,w,h),{x:w,y:h});
  const px=worldToImage(1234,5678,size,w,h);
  const p=imageToWorld(px.x,px.y,size,w,h);
  near(p.x,1234); near(p.z,5678);
}
const changes=[];
const c=new Camera(value=>changes.push(value));
c.smooth=false;
c.setFollow(true);
c.updateMarker({x:100,z:200,heading:90});
c.step(16);
assert.deepEqual([c.x,c.z],[100,200]);
c.zoom(2,{x:20,y:80},600,400,.1,4);
assert.equal(c.follow,true);
assert.deepEqual([c.x,c.z],[100,200]);
c.rotate=true;
c.pan(10,0);
assert.equal(c.follow,false);
assert.equal(c.rotate,true);
assert.deepEqual(changes,[true,false]);
for (const angle of [0,90,180,270]) {
  c.updateMarker({x:100,z:200,heading:angle});
  c.step(16);
  const a=angle*Math.PI/180, origin=screenPoint({x:c.x,z:c.z},c,600,400);
  const nose=screenPoint({x:c.x+Math.sin(a)*10,z:c.z+Math.cos(a)*10},c,600,400);
  near(nose.x,origin.x); near(nose.y,origin.y-20);
  const original=worldPoint(screenPoint({x:25,z:350},c,600,400),c,600,400);
  near(original.x,25); near(original.z,350);
}
assert.equal(shortestTurn(359,1),2);
assert.equal(shortestTurn(1,359),-2);
c.setFollow(true);
c.rotate=true;
c.smooth=false;
c.updateMarker({x:100,z:200,heading:359});
c.step(16);
const start=c.angle;
c.smooth=true;
c.updateMarker({x:160,z:200,heading:1});
assert.equal(c.step(16),true);
assert.ok(shortestTurn(start,c.angle)>0 && shortestTurn(start,c.angle)<2);
for(let i=0;i<120;i++) c.step(16);
assert.equal(c.step(16),false);
near(c.x,160); near(shortestTurn(c.angle,1),0);
c.updateMarker(null);
assert.equal(c.marker,null);
c.smooth=false;
assert.equal(c.step(16),false);
near(shortestTurn(c.angle,0),0);
assert.equal(c.follow,true);

const valid=JSON.stringify({v:1,live:true,map:"chernarusplus",me:{x:1,z:2,heading:-179},layers:{loot:[{x:3,z:4,category:2}]}});
assert.equal(decode(valid).me.heading,-179);
for(const bad of ["", "{}", "[", valid.replace("chernarusplus","../secret"),
    valid.replace('"x":1','"x":"1"'), valid.replace('"loot"','"private"'),
    valid.replace('"live":true','"live":false'), valid.replace('"layers":{','"layers":{"__proto__":[],')])
  assert.equal(decode(bad),null);
assert.equal(decode(valid.repeat(5000)),null);

const timers = new Map(), sockets = [], states = [], packets = [];
let id = 0;
const env = {
  setTimeout(fn,ms) { timers.set(++id,{fn,ms}); return id; },
  clearTimeout(n) { timers.delete(n); },
  WebSocket: class { constructor(){ sockets.push(this); } close(){ this.onclose?.(); } }
};
const runTimer = ms => {
  const entry=[...timers].find(([,t])=>t.ms===ms);
  assert.ok(entry);
  timers.delete(entry[0]);
  entry[1].fn();
};
const live=new LiveConnection("ws://127.0.0.1:8765/ws",s=>states.push(s),p=>packets.push(p),env);
live.start();
sockets[0].onmessage({data:valid});
assert.equal(states.at(-1),"online");
assert.equal(packets.length,1);
sockets[0].close();
assert.equal(states.at(-1),"offline");
runTimer(500);
sockets[1].onmessage({data:"garbage"});
assert.equal(states.at(-1),"offline");
runTimer(1000);
sockets[2].onmessage({data:valid});
runTimer(8000);
assert.equal(states.at(-1),"offline");
live.stop();
assert.equal(timers.size,0);
console.log("PASS: coordinate borders, resolution, camera inverse, follow, heading, interpolation, validation and reconnect");
