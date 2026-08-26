// Optional host-side decoder verification: deno run --allow-net laz_decode_test.js PI_URL
import {parse} from 'https://esm.sh/@loaders.gl/core@4.3.4';
import {LASLoader} from 'https://esm.sh/@loaders.gl/las@4.3.4';

const base = (globalThis.Deno?.args?.[0] || 'http://127.0.0.1:8090').replace(/\/$/, '');
const catalog = await (await fetch(`${base}/api/trips?role=incident_investigator`)).json();
if (!catalog.recordings?.length) throw new Error('No AVS recording available');
const trip = catalog.recordings[0];
const selected = (BigInt(trip.start_ns) + BigInt(trip.end_ns)) / 2n;
const query = new URLSearchParams({
  role: 'incident_investigator',
  trip: trip.id,
  resource: 'lidar.top',
  t_ns: String(selected),
});
const stream = await (await fetch(`${base}/api/closest?${query}`)).arrayBuffer();
const view = new DataView(stream);
if (new TextDecoder().decode(stream.slice(0, 8)) !== 'PDALSTR1') throw new Error('Invalid pDAL stream');
const metadataLength = view.getUint32(8, false);
const payloadLength = Number(view.getBigUint64(12, false));
const payloadOffset = 20 + metadataLength;
const payload = stream.slice(payloadOffset, payloadOffset + payloadLength);
const cloud = await parse(payload, LASLoader, {worker: false, las: {shape: 'mesh', skip: 1, fp64: false}});
const positions = cloud?.attributes?.POSITION?.value;
if (!positions?.length || positions.length % 3 !== 0) throw new Error('LAZ decode produced no 3D positions');
const bounds = [[Infinity, Infinity, Infinity], [-Infinity, -Infinity, -Infinity]];
let finitePoints = 0;
for (let index = 0; index < positions.length; index += 3) {
  const point = [positions[index], positions[index + 1], positions[index + 2]];
  if (!point.every(Number.isFinite)) continue;
  finitePoints += 1;
  for (let axis = 0; axis < 3; axis += 1) {
    bounds[0][axis] = Math.min(bounds[0][axis], point[axis]);
    bounds[1][axis] = Math.max(bounds[1][axis], point[axis]);
  }
}
if (!finitePoints) throw new Error('LAZ decode produced no finite 3D positions');
console.log(`PASS host LAZ decode: ${finitePoints.toLocaleString()} finite points from one pDAL record`);
console.log(`BOUNDS min=${bounds[0].map((value) => value.toFixed(3)).join(',')} max=${bounds[1].map((value) => value.toFixed(3)).join(',')}`);
