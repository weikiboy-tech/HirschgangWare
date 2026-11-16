export const clamp = (n, min, max) => Math.max(min, Math.min(max, n));
export const radians = degrees => degrees * Math.PI / 180;
export const shortestTurn = (from, to) => ((to - from + 180) % 360 + 360) % 360 - 180;

export function worldToImage(x, z, size, width, height) {
  return { x: x * width / size, y: (size - z) * height / size };
}
export function imageToWorld(x, y, size, width, height) {
  return { x: x * size / width, z: size - y * size / height };
}

function rotate(x, y, degrees) {
  const a = radians(degrees), c = Math.cos(a), s = Math.sin(a);
  return { x: c * x - s * y, y: s * x + c * y };
}

export function screenPoint(world, camera, width, height) {
  const p = rotate((world.x - camera.x) * camera.scale, (camera.z - world.z) * camera.scale, -camera.angle);
  return { x: p.x + width / 2, y: p.y + height / 2 };
}

export function worldPoint(screen, camera, width, height) {
  const p = rotate(screen.x - width / 2, screen.y - height / 2, camera.angle);
  return { x: camera.x + p.x / camera.scale, z: camera.z - p.y / camera.scale };
}

function approach(value, target, dt, milliseconds, epsilon) {
  if (!milliseconds || Math.abs(target - value) <= epsilon) return target;
  return target + (value - target) * Math.exp(-dt / milliseconds);
}

export class Camera {
  constructor(onFollowChange = () => {}) {
    this.x = this.z = this.angle = 0;
    this.scale = 1;
    this.follow = false;
    this.rotate = false;
    this.smooth = true;
    this.milliseconds = 120;
    this.target = null;
    this.marker = null;
    this.onFollowChange = onFollowChange;
  }

  setFollow(value) {
    if (this.follow === value) return;
    this.follow = value;
    this.onFollowChange(value);
  }

  updateMarker(target) {
    this.target = target;
    if (!target) this.marker = null;
    else if (!this.marker) this.marker = { ...target };
  }

  step(elapsed) {
    const dt = clamp(elapsed, 0, 100);
    const ms = this.smooth ? this.milliseconds : 0;
    let moving = false;
    if (this.marker && this.target) {
      const target = this.target, marker = this.marker;
      marker.x = approach(marker.x, target.x, dt, ms, 0.05);
      marker.z = approach(marker.z, target.z, dt, ms, 0.05);
      if (Number.isFinite(target.heading)) {
        const old = Number.isFinite(marker.heading) ? marker.heading : target.heading;
        marker.heading = approach(old, old + shortestTurn(old, target.heading), dt, ms, 0.05);
      } else {
        delete marker.heading;
      }
      if (this.follow) { this.x = marker.x; this.z = marker.z; }
      moving = marker.x !== target.x || marker.z !== target.z ||
        (Number.isFinite(target.heading) && Math.abs(shortestTurn(marker.heading, target.heading)) > 0.001);
    }

    const wanted = this.rotate && this.marker ? this.marker.heading ?? 0 : 0;
    const turn = shortestTurn(this.angle, wanted);
    this.angle = approach(this.angle, this.angle + turn, dt, ms, 0.05);
    return moving || Math.abs(shortestTurn(this.angle, wanted)) > 0.001;
  }

  pan(dx, dy) {
    this.setFollow(false);
    const p = rotate(dx / this.scale, dy / this.scale, this.angle);
    this.x -= p.x;
    this.z += p.y;
  }

  zoom(factor, anchor, width, height, min, max) {
    const previous = worldPoint(anchor, this, width, height);
    this.scale = clamp(this.scale * factor, min, max);
    if (this.follow && this.marker) return;
    const after = worldPoint(anchor, this, width, height);
    this.x += previous.x - after.x;
    this.z += previous.z - after.z;
  }
}
