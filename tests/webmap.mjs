import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const source = file => "data:text/javascript;base64," +
  readFileSync(new URL(file, import.meta.url)).toString("base64");
const { Camera, imageToWorld, screenPoint, shortestTurn, headingOnMap, worldPoint, worldToImage } = await import(source("../web/map.js"));
const { CorpseVisibility, decode, healthLine, LiveConnection } = await import(source("../web/live.js"));

assert.equal(healthLine({health: 0}, true), "HP 0");
assert.equal(healthLine({health: 100}, true), "HP 100");
assert.equal(healthLine({health: 75}, false), "");
for (const health of [undefined, 101, -1, "75", null, 0.5]) assert.equal(healthLine({health}, true), "");

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
for (const heading of [0, 90, 180, 270]) {
  const expected = [0, 90, -180, -90][heading / 90];
  assert.equal(headingOnMap(heading, 0), expected);
  assert.equal(headingOnMap(heading, heading), 0);
}
assert.equal(headingOnMap(1,359),2);
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
const stashPacket = JSON.parse(valid);
const highValuePacket = {...stashPacket, layers:{highValues:[{x:520,z:940}]}};
assert.deepEqual(decode(JSON.stringify(highValuePacket)).layers.highValues,[{x:520,z:940}]);
assert.deepEqual(decode(JSON.stringify({...highValuePacket,layers:{highValues:[{x:520,z:940,bitcoin:true}]}})).layers.highValues,[{x:520,z:940,bitcoin:true}]);
assert.equal(decode(JSON.stringify({...highValuePacket,layers:{highValues:[{x:520,z:940,bitcoin:"yes"}]}})),null);
assert.equal(decode(JSON.stringify({...highValuePacket,layers:{highValues:[{x:-1,z:940}]}})),null);
stashPacket.layers = {stashLoot: [
  {x:450,z:960,kind:"backpack",label:"HuntingBag_Autumn"},
  {x:560,z:960,kind:"vehicle-key",label:"ExpansionCarKey_Red"},
  {x:670,z:960,kind:"stash",label:"UndergroundStash"},
]};
assert.deepEqual(decode(JSON.stringify(stashPacket)).layers.stashLoot,
  stashPacket.layers.stashLoot.map(({x, z, kind}) => ({x, z, kind})));
stashPacket.layers.stashLoot.push({x:720,z:960,kind:"tool",label:"Hammer"});
assert.equal(decode(JSON.stringify(stashPacket)), null);
const { playerLines, trackingTargets, savedPlayer, resolveSavedPlayer } = await import(source("../web/live.js"));
const playerPacket = JSON.parse(valid);
const firstId = "fedcba9876543210-42", secondId = "fedcba9876543210-43";
playerPacket.layers.players = [{x:3,z:4,heading:90,id:firstId,name:"Freund",held:"Rifle",friend:true},{x:5,z:6,id:secondId}];
playerPacket.layers.events = [{x:7,z:8,kind:"airdrop",label:"Airdrop"}];
const decoded = decode(JSON.stringify(playerPacket));
const visible = trackingTargets(decoded, true);
const remembered = savedPlayer({id:firstId,name:"Freund"});
assert.equal(resolveSavedPlayer(remembered,visible),firstId);
assert.equal(resolveSavedPlayer(remembered,new Map()),null);
assert.equal(resolveSavedPlayer(remembered,new Map([[secondId,{name:"Freund"}]])),secondId);
assert.equal(resolveSavedPlayer(remembered,new Map([[secondId,{name:"Freund"}],["0123456789abcdef-9",{name:"Freund"}]])),null);
assert.equal(resolveSavedPlayer(savedPlayer({id:"own",name:"Freund"}),new Map([[secondId,{name:"Freund"}]])),null);
assert.equal(savedPlayer({id:"invalid",name:"Freund"}),null);
assert.deepEqual(decoded.layers.events, playerPacket.layers.events);
const corpse = { x: 7, z: 8, id: "0123456789abcdef-7", ttlMs: 420000 };
const deathPacket = {...playerPacket, layers: {corpses: [corpse]}};
assert.deepEqual(decode(JSON.stringify(deathPacket)).layers.corpses, [corpse]);
for (const ttlMs of [0, -1, 420001, 1.5]) {
  assert.equal(decode(JSON.stringify({...deathPacket, layers: {corpses: [{...corpse, ttlMs}]}})), null);
}
assert.equal(decode(JSON.stringify({...deathPacket, layers: {corpses: [corpse, corpse]}})), null);
const corpseView = new CorpseVisibility();
corpseView.update([corpse], 1000);
corpseView.update([{...corpse, ttlMs: 419500}], 1610); // Delayed delivery may not extend the clock.
assert.equal(corpseView.visible(corpse, 420999), true);
assert.equal(corpseView.visible(corpse, 421000), false);
assert.equal(corpseView.click(corpse.id, 2000), false);
assert.equal(corpseView.click("0123456789abcdef-8", 2100), false);
assert.equal(corpseView.click(corpse.id, 2200), false);
assert.equal(corpseView.click(corpse.id, 2300), true);
corpseView.update([{...corpse, ttlMs: 418600}], 2400);
assert.equal(corpseView.visible(corpse, 2400), false);
assert.equal(new CorpseVisibility([...corpseView.hidden]).hidden.has(corpse.id), true);
assert.deepEqual(playerLines(visible.get(firstId), decoded.me), ["Freund  3m", "Rifle"]);
assert.deepEqual(playerLines(visible.get(firstId), null), ["Freund", "Rifle"]);
assert.deepEqual(playerLines(visible.get(firstId), decoded.me, {playerNames:false}), ["Rifle"]);
assert.deepEqual(playerLines(visible.get(firstId), decoded.me, {heldItems:false}), ["Freund  3m"]);
assert.deepEqual(playerLines(visible.get(firstId), decoded.me, {playerNames:false, heldItems:false}), []);
assert.equal(decode(JSON.stringify({...playerPacket, heldShared:false})).heldShared, false);
assert.equal(decode(JSON.stringify({...playerPacket, namesShared:false})).namesShared, false);
assert.equal(decode(JSON.stringify({...playerPacket, namesShared:true})).namesShared, true);
assert.deepEqual(playerLines({held:"Rifle"}, decoded.me), ["Rifle"]);
assert.deepEqual(playerLines(visible.get(secondId), decoded.me), []);
assert.equal(decode(JSON.stringify({...playerPacket, layers: {events: [{x:7,z:8,kind:"unverified",label:"Airdrop"}]}})),null);
assert.equal(visible.get(firstId).heading,90);
assert.equal(visible.get(firstId).name,"Freund");
assert.equal(visible.get(firstId).friend,true);
assert.equal(visible.get(secondId).heading,undefined);
assert.deepEqual([...trackingTargets(decode(JSON.stringify(playerPacket)), false).keys()], ["own"]);
const guestPacket = { ...playerPacket, me: null };
assert.deepEqual([...trackingTargets(decode(JSON.stringify(guestPacket)), true).keys()], [firstId, secondId]);
const guestCamera = new Camera();
guestCamera.rotate = true;
guestCamera.smooth = false;
guestCamera.updateMarker(visible.get(firstId));
guestCamera.step(16);
assert.equal(guestCamera.angle,90);
assert.equal(guestCamera.x,0);
guestCamera.setFollow(true);
guestCamera.step(16);
assert.equal(guestCamera.x,3);
guestCamera.pan(20,0);
assert.equal(guestCamera.follow,false);
assert.equal(guestCamera.angle,90);
playerPacket.layers.players.reverse();
assert.equal(trackingTargets(decode(JSON.stringify(playerPacket)), true).get(firstId).x,3);
playerPacket.layers.players.reverse();
playerPacket.layers.players[1].id = firstId;
assert.equal(decode(JSON.stringify(playerPacket)),null);
playerPacket.layers.players[1].id = secondId;
playerPacket.layers.players[0].heading = "90";
assert.equal(decode(JSON.stringify(playerPacket)),null);
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
