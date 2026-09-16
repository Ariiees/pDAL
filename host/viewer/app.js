import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {parse} from 'https://esm.sh/@loaders.gl/core@4.3.4';
import {LASLoader} from 'https://esm.sh/@loaders.gl/las@4.3.4';

const $ = (id) => document.getElementById(id);
// ?role= is accepted only as a login-form prefill, never as auth
const requestedRole = new URLSearchParams(window.location.search).get('role');
const state = {
  role: requestedRole || 'fleet_analyst',
  roles: [],
  trips: [],
  trip: null,
  access: {},
  timeline: {},
  brakeEvents: [],
  brakeStatus: "loading",
  route: [],
  selectedNs: null,
  networkBytes: 0,
  retrievals: [],
  recordsReturned: 0,
  sensorGeneration: 0,
  tripGeneration: 0,
  cameraUrl: null,
  token: null,
  expiresAt: null,
};

let map;
let mapReady = false;
let sensorTimer;
let sensorController;
let tripController;
let toastTimer;
let countdownTimer;
let retrievalSequence = 0;
let lidarView;
const cameraView = {zoom: 1, x: 0, y: 0, dragging: false, lastX: 0, lastY: 0};

function reportClientStatus(values) {
  fetch(api('/client-status', values), {cache: 'no-store'}).catch(() => {});
}

async function trackedFetch(url, options = {}) {
  const tripGeneration = state.tripGeneration;
  const token = state.token;
  const headers = {...(options.headers || {})};
  if (state.token && url.startsWith('/api/')) {
    headers['Authorization'] = `Bearer ${state.token}`;
  }
  const {response, body} = await fetchBodyTimed(url, {...options, headers, cache: options.cache || 'no-store'});
  const measured = Number(response.headers.get('x-demo-network-bytes') || body.byteLength);
  if (tripGeneration === state.tripGeneration && token === state.token) {
    state.networkBytes += measured;
    updateBandwidth();
  }
  if (!response.ok) {
    let details;
    try { details = JSON.parse(new TextDecoder().decode(body)); }
    catch { details = {message: `HTTP ${response.status}`}; }
    const error = new Error(details.message || details.error || `HTTP ${response.status}`);
    error.status = response.status;
    error.details = details;
    if (response.status === 401 && state.token && token === state.token) {
      clearSession();
      showLogin('Session expired — sign in again');
    }
    throw error;
  }
  return {response, body};
}

async function fetchJson(url, options = {}) {
  const {body} = await trackedFetch(url, options);
  return JSON.parse(new TextDecoder().decode(body));
}

function api(path, parameters = {}) {
  const query = new URLSearchParams(parameters);
  return `${path}?${query}`;
}

function formatBytes(value) {
  if (value == null) return 'Unavailable';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let size = Number(value);
  let index = 0;
  while (size >= 1000 && index < units.length - 1) { size /= 1000; index += 1; }
  const digits = size >= 100 || index === 0 ? 0 : size >= 10 ? 1 : 2;
  return `${size.toFixed(digits)} ${units[index]}`;
}

function formatInstant(ns, withDate = false) {
  if (ns == null) return '—';
  const date = new Date(Number(BigInt(ns) / 1000000n));
  const time = date.toLocaleTimeString([], {hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit', fractionalSecondDigits: 3});
  return withDate ? `${date.toLocaleDateString([], {month: 'short', day: '2-digit'})} ${time}` : time;
}

function formatDelta(deltaNs) {
  if (deltaNs == null) return '—';
  const delta = Number(deltaNs) / 1e6;
  const sign = delta > 0 ? '+' : delta < 0 ? '−' : '';
  const absolute = Math.abs(delta);
  return absolute >= 1000 ? `${sign}${(absolute / 1000).toFixed(3)} s` : `${sign}${absolute.toFixed(1)} ms`;
}

function formatDuration(ns) {
  const seconds = Number(BigInt(ns) / 1000000000n);
  const minutes = Math.floor(seconds / 60);
  return `${minutes}m ${String(seconds % 60).padStart(2, '0')}s`;
}

function updateBandwidth() {
  const candidate = Number(state.trip?.candidate_bytes || 0);
  $('candidateBytes').textContent = candidate ? formatBytes(candidate) : '—';
  $('networkBytes').textContent = formatBytes(state.networkBytes);
  $('recordsReturned').textContent = state.recordsReturned.toLocaleString();
  if (!candidate) {
    $('reductionValue').textContent = '—';
  } else {
    const avoided = Math.max(0, candidate - state.networkBytes);
    $('reductionValue').textContent = `${(avoided / candidate * 100).toFixed(2)}%`;
  }
}

function parseRecordStream(buffer) {
  const bytes = new Uint8Array(buffer);
  if (bytes.byteLength < 8 || new TextDecoder().decode(bytes.slice(0, 8)) !== 'PDALSTR1') {
    throw new Error('Invalid pDAL record stream');
  }
  const view = new DataView(buffer);
  const decoder = new TextDecoder();
  const records = [];
  let offset = 8;
  while (offset < bytes.byteLength) {
    if (offset + 12 > bytes.byteLength) throw new Error('Truncated pDAL frame header');
    const metadataLength = view.getUint32(offset, false);
    const payloadLength = Number(view.getBigUint64(offset + 4, false));
    offset += 12;
    if (offset + metadataLength + payloadLength > bytes.byteLength) throw new Error('Truncated pDAL frame');
    const metadataText = decoder.decode(bytes.subarray(offset, offset + metadataLength));
    const timestampMatch = metadataText.match(/"timestamp_ns"\s*:\s*([0-9]+)/);
    const metadata = JSON.parse(metadataText);
    metadata.timestamp_ns = timestampMatch ? timestampMatch[1] : String(metadata.timestamp_ns);
    offset += metadataLength;
    const payload = bytes.slice(offset, offset + payloadLength);
    offset += payloadLength;
    records.push({metadata, payload});
  }
  return records;
}

function decodeGps(payload) {
  if (payload.byteLength < 48) throw new Error(`Unexpected GPS payload size ${payload.byteLength}`);
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const point = {
    latitude: view.getFloat64(0, true),
    longitude: view.getFloat64(8, true),
    altitude: view.getFloat64(16, true),
    covX: view.getFloat64(24, true),
    covY: view.getFloat64(32, true),
    covZ: view.getFloat64(40, true),
  };
  if (!Number.isFinite(point.latitude) || !Number.isFinite(point.longitude) ||
      Math.abs(point.latitude) > 90 || Math.abs(point.longitude) > 180) {
    throw new Error('AVS GPS record contains invalid coordinates');
  }
  return point;
}

function initMap() {
  map = new window.maplibregl.Map({
    container: 'map',
    center: [-75.75, 39.68],
    zoom: 11,
    attributionControl: true,
    style: 'https://tiles.openfreemap.org/styles/bright',
  });
  map.addControl(new window.maplibregl.NavigationControl({showCompass: false}), 'top-right');
  map.on('load', () => {
    mapReady = true;
    map.addSource('route', {type: 'geojson', data: emptyFeatureCollection()});
    map.addLayer({id: 'route-glow', type: 'line', source: 'route', filter: ['==', ['geometry-type'], 'LineString'], paint: {'line-color': '#51d9d1', 'line-width': 8, 'line-opacity': .17}});
    map.addLayer({id: 'route-line', type: 'line', source: 'route', filter: ['==', ['geometry-type'], 'LineString'], paint: {'line-color': '#51d9d1', 'line-width': 2.5, 'line-opacity': .95}});
    map.addLayer({id: 'route-hit', type: 'circle', source: 'route', filter: ['==', ['geometry-type'], 'Point'], paint: {'circle-radius': 7, 'circle-color': '#51d9d1', 'circle-opacity': .01}});
    map.addSource('current', {type: 'geojson', data: emptyFeatureCollection()});
    map.addLayer({id: 'current-halo', type: 'circle', source: 'current', paint: {'circle-radius': 12, 'circle-color': '#51d9d1', 'circle-opacity': .2}});
    map.addLayer({id: 'current-point', type: 'circle', source: 'current', paint: {'circle-radius': 5, 'circle-color': '#eafffd', 'circle-stroke-color': '#51d9d1', 'circle-stroke-width': 2}});
    map.on('mouseenter', 'route-hit', () => { map.getCanvas().style.cursor = 'pointer'; });
    map.on('mouseleave', 'route-hit', () => { map.getCanvas().style.cursor = ''; });
    map.on('click', 'route-hit', (event) => {
      const feature = event.features?.[0];
      if (feature?.properties?.timestamp_ns) setSelectedTime(String(feature.properties.timestamp_ns));
    });
    updateMapRoute();
  });
}

function emptyFeatureCollection() { return {type: 'FeatureCollection', features: []}; }

function updateMapRoute() {
  if (!mapReady || !state.route.length) return;
  const coordinates = state.route.map((point) => [point.longitude, point.latitude]);
  const features = [
    {type: 'Feature', properties: {}, geometry: {type: 'LineString', coordinates}},
    ...state.route.map((point) => ({
      type: 'Feature',
      properties: {timestamp_ns: point.timestampNs},
      geometry: {type: 'Point', coordinates: [point.longitude, point.latitude]},
    })),
  ];
  map.getSource('route').setData({type: 'FeatureCollection', features});
  reportClientStatus({state: 'map-rendered', role: state.role, route_points: state.route.length});
  const bounds = coordinates.reduce(
    (result, coordinate) => result.extend(coordinate),
    new window.maplibregl.LngLatBounds(coordinates[0], coordinates[0]),
  );
  map.fitBounds(bounds, {padding: 48, maxZoom: 16, duration: 700});
}

function updateCurrentPosition(point, timestampNs) {
  if (!mapReady || !point) return;
  map.getSource('current').setData({
    type: 'FeatureCollection',
    features: [{type: 'Feature', properties: {}, geometry: {type: 'Point', coordinates: [point.longitude, point.latitude]}}],
  });
  $('gpsPosition').textContent = `${point.latitude.toFixed(6)}, ${point.longitude.toFixed(6)} · ${point.altitude.toFixed(1)} m`;
  $('gpsTimestamp').textContent = `GPS timestamp ${formatInstant(timestampNs, true)}`;
}

function applyCameraView() {
  const image = $('cameraImage');
  image.style.transform = `translate(${cameraView.x}px, ${cameraView.y}px) scale(${cameraView.zoom})`;
  $('cameraZoomLabel').textContent = cameraView.zoom === 1 ? 'FIT' : `${cameraView.zoom.toFixed(1)}×`;
}

function setCameraZoom(value) {
  cameraView.zoom = Math.max(1, Math.min(8, value));
  if (cameraView.zoom === 1) {
    cameraView.x = 0;
    cameraView.y = 0;
  } else {
    const stage = document.querySelector('.camera-stage');
    const maxX = stage.clientWidth * (cameraView.zoom - 1) / 2;
    const maxY = stage.clientHeight * (cameraView.zoom - 1) / 2;
    cameraView.x = Math.max(-maxX, Math.min(maxX, cameraView.x));
    cameraView.y = Math.max(-maxY, Math.min(maxY, cameraView.y));
  }
  applyCameraView();
}

function setupCameraView() {
  const stage = document.querySelector('.camera-stage');
  const image = $('cameraImage');
  $('cameraZoomIn').addEventListener('click', () => setCameraZoom(cameraView.zoom * 1.35));
  $('cameraZoomOut').addEventListener('click', () => setCameraZoom(cameraView.zoom / 1.35));
  $('cameraZoomReset').addEventListener('click', () => setCameraZoom(1));
  $('cameraExpand').addEventListener('click', async () => {
    setCameraZoom(1);
    if (document.fullscreenElement === stage) {
      await document.exitFullscreen();
    } else {
      await stage.requestFullscreen();
    }
  });
  document.addEventListener('fullscreenchange', () => {
    $('cameraExpand').textContent = document.fullscreenElement === stage ? '×' : '⛶';
    setCameraZoom(1);
  });
  stage.addEventListener('wheel', (event) => {
    if (!image.src) return;
    event.preventDefault();
    setCameraZoom(cameraView.zoom * (event.deltaY < 0 ? 1.2 : 1 / 1.2));
  }, {passive: false});
  stage.addEventListener('dblclick', () => setCameraZoom(1));
  stage.addEventListener('pointerdown', (event) => {
    if (cameraView.zoom === 1 || event.target.closest('.camera-controls')) return;
    cameraView.dragging = true;
    cameraView.lastX = event.clientX;
    cameraView.lastY = event.clientY;
    image.classList.add('dragging');
    stage.setPointerCapture(event.pointerId);
  });
  stage.addEventListener('pointermove', (event) => {
    if (!cameraView.dragging) return;
    cameraView.x += event.clientX - cameraView.lastX;
    cameraView.y += event.clientY - cameraView.lastY;
    cameraView.lastX = event.clientX;
    cameraView.lastY = event.clientY;
    setCameraZoom(cameraView.zoom);
  });
  const stopDrag = () => {
    cameraView.dragging = false;
    image.classList.remove('dragging');
  };
  stage.addEventListener('pointerup', stopDrag);
  stage.addEventListener('pointercancel', stopDrag);
  applyCameraView();
}

function initLidar() {
  const container = $('lidarCanvas');
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0x061417);
  const camera = new THREE.PerspectiveCamera(52, 2, .02, 10000);
  camera.position.set(0, 0, 20);
  camera.up.set(0, 1, 0);
  const renderer = new THREE.WebGLRenderer({antialias: true});
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  container.prepend(renderer.domElement);
  const controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.dampingFactor = .08;
  controls.target.set(0, 0, 0);
  let cloud = null;
  let grid = null;
  let axes = null;
  let fittedSpans = null;
  let fittedRadius = 1;
  let viewportWidth = 0;
  let viewportHeight = 0;
  function fitTopDown(spans, radius) {
    const aspect = Math.max(camera.aspect, .01);
    const halfVertical = Math.max(spans[1] / 2, 1);
    const halfHorizontal = Math.max(spans[0] / 2, 1);
    const verticalTangent = Math.tan(THREE.MathUtils.degToRad(camera.fov / 2));
    const distanceForHeight = halfVertical / verticalTangent;
    const distanceForWidth = halfHorizontal / (verticalTangent * aspect);
    const distance = Math.max(distanceForHeight, distanceForWidth) * 1.12 + spans[2] / 2;
    camera.up.set(0, 1, 0);
    camera.position.set(0, 0, distance);
    camera.near = Math.max(.1, distance - radius * 2);
    camera.far = Math.max(1000, distance + radius * 4);
    camera.updateProjectionMatrix();
    controls.target.set(0, 0, 0);
    controls.minDistance = Math.max(radius * .12, 1);
    controls.maxDistance = Math.max(distance * 4, radius * 8);
    controls.update();
  }
  function resize() {
    const width = container.clientWidth;
    const height = container.clientHeight;
    renderer.setSize(width, height, false);
    camera.aspect = width / Math.max(height, 1);
    camera.updateProjectionMatrix();
    if (fittedSpans && (width !== viewportWidth || height !== viewportHeight)) {
      fitTopDown(fittedSpans, fittedRadius);
    }
    viewportWidth = width;
    viewportHeight = height;
  }
  function animate() {
    resize();
    controls.update();
    renderer.render(scene, camera);
    requestAnimationFrame(animate);
  }
  animate();
  lidarView = {
    clear() {
      if (!cloud) return;
      scene.remove(cloud);
      cloud.geometry.dispose();
      cloud.material.dispose();
      cloud = null;
    },
    setCloud(positions) {
      if (cloud) {
        scene.remove(cloud);
        cloud.geometry.dispose();
        cloud.material.dispose();
      }
      if (grid) {
        scene.remove(grid);
        grid.geometry.dispose();
        grid.material.dispose();
      }
      if (axes) {
        scene.remove(axes);
        axes.geometry.dispose();
        axes.material.dispose();
      }
      const centered = new Float32Array(positions.length);
      const count = positions.length / 3;
      const minimum = [Infinity, Infinity, Infinity];
      const maximum = [-Infinity, -Infinity, -Infinity];
      for (let i = 0; i < positions.length; i += 3) {
        for (let axis = 0; axis < 3; axis += 1) {
          minimum[axis] = Math.min(minimum[axis], positions[i + axis]);
          maximum[axis] = Math.max(maximum[axis], positions[i + axis]);
        }
      }
      const center = minimum.map((value, axis) => (value + maximum[axis]) / 2);
      const spans = minimum.map((value, axis) => maximum[axis] - value);
      const colors = new Float32Array(positions.length);
      let radius = 1;
      for (let i = 0; i < positions.length; i += 3) {
        const x = positions[i] - center[0];
        const y = positions[i + 1] - center[1];
        const z = positions[i + 2] - center[2];
        centered[i] = x; centered[i + 1] = y; centered[i + 2] = z;
        radius = Math.max(radius, Math.hypot(x, y, z));
        const tone = spans[2] > 0 ? Math.max(0, Math.min(1, (positions[i + 2] - minimum[2]) / spans[2])) : .5;
        const color = new THREE.Color().setHSL(.55 - tone * .13, .88, .58 + tone * .13);
        colors[i] = color.r; colors[i + 1] = color.g; colors[i + 2] = color.b;
      }
      const geometry = new THREE.BufferGeometry();
      geometry.setAttribute('position', new THREE.BufferAttribute(centered, 3));
      geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));
      geometry.computeBoundingSphere();
      const material = new THREE.PointsMaterial({
        size: Math.max(1, radius / 85),
        vertexColors: true,
        sizeAttenuation: true,
        transparent: true,
        opacity: .96,
        depthWrite: false,
      });
      cloud = new THREE.Points(geometry, material);
      scene.add(cloud);
      const gridSize = Math.max(100, Math.ceil(radius * 2 / 100) * 100);
      grid = new THREE.GridHelper(gridSize, 20, 0x3b666c, 0x173238);
      grid.rotation.x = Math.PI / 2;
      scene.add(grid);
      axes = new THREE.AxesHelper(radius * .12);
      scene.add(axes);
      fittedSpans = spans;
      fittedRadius = radius;
      fitTopDown(spans, radius);
      return {count, radius, spans};
    },
  };
}

async function decodeLaz(payload) {
  const arrayBuffer = payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.byteLength);
  const pointCloud = await parse(arrayBuffer, LASLoader, {worker: false, las: {shape: 'mesh', skip: 1, fp64: false}});
  const attribute = pointCloud?.attributes?.POSITION || pointCloud?.attributes?.position;
  if (!attribute?.value) throw new Error('LAZ decoder returned no POSITION attribute');
  return attribute.value;
}

// ── Session management ──────────────────────────────────────────────────────

function clearSession() {
  state.tripGeneration += 1;
  tripController?.abort();
  invalidateSensors();
  state.token = null;
  state.expiresAt = null;
  sessionStorage.removeItem('pdal_token');
  clearInterval(countdownTimer);
  countdownTimer = null;
  $('sessionCountdown').textContent = '';
}

function startCountdown(expiresIn) {
  state.expiresAt = Date.now() + expiresIn * 1000;
  clearInterval(countdownTimer);
  function tick() {
    const remaining = Math.max(0, Math.ceil((state.expiresAt - Date.now()) / 1000));
    $('sessionCountdown').textContent = remaining > 90
      ? `${Math.ceil(remaining / 60)}m`
      : `${remaining}s`;
    if (remaining <= 0) {
      clearInterval(countdownTimer);
      clearSession();
      showLogin('Session expired — sign in again');
    }
  }
  tick();
  countdownTimer = setInterval(tick, 1000);
}

function showLogin(message = '') {
  $('loginScreen').classList.add('visible');
  $('loginError').textContent = message;
  $('loginPassword').value = '';
}

function showApp() {
  $('loginScreen').classList.remove('visible');
}

async function doLogin() {
  const role = $('loginRoleSelect').value;
  const password = $('loginPassword').value;
  $('loginError').textContent = '';
  $('loginSubmit').disabled = true;
  try {
    const response = await fetch('/api/login', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({role, password}),
      cache: 'no-store',
    });
    const data = await response.json();
    if (response.status === 200) {
      state.token = data.token;
      state.role = data.role;
      sessionStorage.setItem('pdal_token', data.token);
      $('sessionRoleLabel').textContent = data.label || data.role;
      showApp();
      startCountdown(data.expires_in);
      $('connectionLabel').textContent = `Vehicle online · ${data.label || data.role}`;
      await loadTrips();
    } else if (response.status === 401) {
      $('loginError').textContent = 'Invalid role or password.';
    } else if (response.status === 429) {
      const retry = data.retry_after_seconds || 60;
      $('loginError').textContent = `Too many failed attempts. Try again in ${retry} s.`;
    } else if (response.status === 503) {
      $('loginError').textContent = 'Login not configured on the Pi.';
    } else {
      $('loginError').textContent = `Login failed (${response.status}).`;
    }
  } catch (err) {
    $('loginError').textContent = `Network error: ${err.message}`;
  } finally {
    $('loginSubmit').disabled = false;
  }
}

async function doLogout() {
  if (state.token) {
    try {
      await fetch('/api/logout', {
        method: 'POST',
        headers: {'Authorization': `Bearer ${state.token}`},
        cache: 'no-store',
      });
    } catch {}
  }
  clearSession();
  state.trips = [];
  state.trip = null;
  state.access = {};
  state.route = [];
  state.timeline = {};
  state.brakeEvents = []; state.brakeStatus = "loading"; renderBrakeEvents();
  state.networkBytes = 0;
  state.retrievals = []; renderRetrievalLatency();
  state.recordsReturned = 0;
  $('tripList').innerHTML = '';
  $('tripCount').textContent = '—';
  $('accessRows').innerHTML = '';
  updateBandwidth();
  showLogin();
  $('connectionLabel').textContent = 'Awaiting login…';
  $('sessionRoleLabel').textContent = '—';
}

// ── API helpers ─────────────────────────────────────────────────────────────

async function loadRoles() {
  const data = await fetchJson('/api/roles');
  state.roles = data.roles;
  if (!state.roles.some((role) => role.id === state.role)) state.role = 'fleet_analyst';
  $('loginRoleSelect').innerHTML = data.roles.map((role) => `<option value="${role.id}">${role.label}</option>`).join('');
  $('loginRoleSelect').value = state.role;
}

async function loadTrips() {
  const data = await fetchJson('/api/trips');
  state.trips = data.recordings;
  state.access = data.access || {};
  $('tripCount').textContent = state.trips.length;
  renderTrips();
  if (!state.trips.length) throw new Error('No AVS recordings were discovered');
  await selectTrip(state.trips[0].id);
}

function renderTrips() {
  const filter = $('storageFilter').value;
  const visible = state.trips.filter((trip) => !filter || trip.storage_locations?.includes(filter));
  $('tripCount').textContent = filter ? `${visible.length}/${state.trips.length}` : state.trips.length;
  $('tripList').innerHTML = visible.map((trip) => {
    const recorded = new Set(trip.modalities.map((item) => item.resource));
    const time = formatInstant(trip.start_ns);
    const active = state.trip?.id === trip.id;
    const detail = active ? `<div class="trip-detail">${trip.modalities.map((item) => `
      <div class="trip-detail-row">
        <b>${item.resource === 'position' ? 'GPS' : item.resource === 'camera.front' ? 'CAM' : 'LIDAR'}</b>
        <span>${formatInstant(item.start_ns)}–${formatInstant(item.end_ns)}</span>
        <span>${item.record_count == null ? 'count n/a' : Number(item.record_count).toLocaleString()} · ${formatBytes(item.stored_bytes)} · ${storageLabel(item)}</span>
      </div>`).join('')}<div class="event-empty">${brakeSummary()}</div></div>` : '';
    return `<button class="trip-card ${active ? 'active' : ''}" data-trip="${trip.id}">
      <div class="trip-day"><strong>${trip.label}</strong><time>${time}</time></div>
      <div class="trip-meta"><span>${formatDuration(trip.duration_ns)}</span><span>${formatBytes(trip.candidate_bytes)} · ${storageLabel(trip)}</span></div>
      <div class="modality-pips"><span class="${recorded.has('position') ? 'available' : ''}">GPS</span><span class="${recorded.has('camera.front') ? 'available' : ''}">CAM</span><span class="${recorded.has('lidar.top') ? 'available' : ''}">LIDAR</span></div>
      ${detail}
    </button>`;
  }).join('');
  document.querySelectorAll('.trip-card').forEach((button) => button.addEventListener('click', () => selectTrip(button.dataset.trip)));
}

async function selectTrip(tripId) {
  const trip = state.trips.find((item) => item.id === tripId);
  if (!trip) return;
  const generation = ++state.tripGeneration;
  tripController?.abort();
  tripController = new AbortController();
  state.trip = trip;
  state.access = {};
  state.route = [];
  state.timeline = {};
  state.brakeEvents = []; state.brakeStatus = "loading"; renderBrakeEvents();
  state.networkBytes = 0;
  state.retrievals = []; renderRetrievalLatency();
  state.recordsReturned = 0;
  state.selectedNs = String((BigInt(trip.start_ns) + BigInt(trip.end_ns)) / 2n);
  invalidateSensors();
  if (mapReady) {
    map.getSource('route').setData(emptyFeatureCollection());
    map.getSource('current').setData(emptyFeatureCollection());
  }
  renderTrips();
  renderContext();
  renderAccess();
  drawTimeline();
  updateBandwidth();
  // Resolve this trip's access before scheduling any payload reads.
  try {
    await loadAccess();
    if (generation !== state.tripGeneration) return;
    scheduleSensorLoad(0);
    const background = await Promise.allSettled([loadTimeline(), loadRoute()]);
    if (generation !== state.tripGeneration) return;
    const failed = background.find((result) => result.status === 'rejected');
    if (failed) showToast(failed.reason.message, true);
  } catch (error) {
    if (generation !== state.tripGeneration) return;
    for (const resource of ['camera.front', 'lidar.top']) {
      state.access[resource] = {authorized: false, status: error.status || 503, reason: error.message};
    }
    renderAccess();
    showToast(error.message, true);
  }
}

function renderContext() {
  const trip = state.trip;
  $('tripTitle').textContent = `${trip.label} · ${storageLabel(trip)}`;
  $('tripWindow').textContent = `${formatInstant(trip.start_ns, true)} → ${formatInstant(trip.end_ns)}`;
  $('selectedTime').textContent = formatInstant(state.selectedNs, true);
  $('candidateBytes').textContent = formatBytes(trip.candidate_bytes);
}

function renderAccess() {
  for (const [resource, panel] of [['camera.front', 'camera'], ['lidar.top', 'lidar']]) {
    const info = state.access[resource];
    const allowed = Boolean(info?.authorized);
    const denied = info && !allowed && (!info.status || [401, 403].includes(info.status));
    $(`${panel}Lock`).classList.toggle('visible', Boolean(denied));
    $(`${panel}State`).textContent = allowed ? 'AUTHORIZED' : !info ? 'CHECKING ACCESS' : denied ? 'DENIED' : 'UNAVAILABLE';
    $(`${panel}State`).classList.toggle('authorized', allowed);
    if (info && !allowed && !denied) setSensorEmpty(panel, 'Temporarily unavailable', 'Select the trip again to retry.');
  }
  const policy = Object.values(state.access).find((entry) => entry.policy)?.policy;
  $('policyVersion').textContent = policy ? `${policy} · enforced onboard` : 'pDAL policy active';
}

async function loadTimeline() {
  const generation = state.tripGeneration;
  const trip = state.trip;
  const role = state.role;
  const data = await fetchJson(api('/api/timeline', {trip: trip.id}), {signal: tripController?.signal});
  if (generation !== state.tripGeneration || state.trip !== trip || state.role !== role) return;
  state.brakeEvents = data.hard_braking?.events || [];
  state.brakeStatus = data.hard_braking?.status || 'unavailable';
  renderBrakeEvents();
  renderTrips();
  state.timeline = data.tracks || {};
  drawTimeline();
}

async function loadRoute() {
  const generation = state.tripGeneration;
  const gps = state.trip.modalities.find((item) => item.resource === 'position');
  if (!gps || !state.access.position?.authorized) return;
  const everyN = Math.max(1, Math.ceil((gps.record_count || 1) / 1800));
  const {body} = await trackedFetch(api('/api/history', {
    trip: state.trip.id,
    resource: 'position',
    start_ns: gps.start_ns,
    end_ns: gps.end_ns,
    every_n: everyN,
    max_records: 50000,
  }), {signal: tripController?.signal});
  if (generation !== state.tripGeneration) return;
  const records = parseRecordStream(body);
  state.recordsReturned += records.length;
  state.route = records.map((record) => ({...decodeGps(record.payload), timestampNs: record.metadata.timestamp_ns}));
  updateBandwidth();
  updateMapRoute();
}

async function loadAccess() {
  const generation = state.tripGeneration;
  const params = state.trip ? {trip: state.trip.id} : {};
  const data = await fetchJson(api('/api/access', params), {signal: tripController?.signal});
  if (generation !== state.tripGeneration) return;
  state.access = data.resources || state.access;
  renderAccess();
  renderAccessPanel(data);
}

function renderAccessPanel(data) {
  const rows = data.resources || {};
  const fallbackLabels = {'position': 'GPS', 'camera.front': 'Front Camera', 'lidar.top': 'LiDAR'};
  $('accessRows').innerHTML = Object.entries(rows).map(([key, info]) => {
    const authorized = info.authorized;
    const badge = authorized
      ? `<span class="access-badge ok">AUTHORIZED</span>`
      : `<span class="access-badge denied">${info.status || 403} · ${info.code || (info.status >= 500 || info.status === 429 ? 'UNAVAILABLE' : 'DENIED')}</span>`;
    const reason = !authorized && info.reason
      ? `<small>${info.reason}</small>`
      : '';
    return `<div class="access-row"><span>${info.label || fallbackLabels[key] || key}</span>${badge}${reason}</div>`;
  }).join('');
  const policy = Object.values(rows).find((entry) => entry.policy)?.policy;
  if (policy) $('policyVersion').textContent = `${policy} · enforced onboard`;
}

function setSelectedTime(ns) {
  if (!state.trip) return;
  const value = BigInt(ns);
  const clamped = value < BigInt(state.trip.start_ns) ? BigInt(state.trip.start_ns) : value > BigInt(state.trip.end_ns) ? BigInt(state.trip.end_ns) : value;
  state.selectedNs = String(clamped);
  $('selectedTime').textContent = formatInstant(state.selectedNs, true);
  drawTimeline();
  scheduleSensorLoad(130);
}

function scheduleSensorLoad(delay) {
  invalidateSensors();
  sensorTimer = setTimeout(loadSelectedSensors, delay);
}

function setSensorEmpty(panel, title, detail = '') {
  const empty = $(`${panel}Empty`);
  empty.style.display = 'flex';
  empty.querySelector('strong').textContent = title;
  empty.querySelector('small').textContent = detail;
}

function invalidateSensors() {
  clearTimeout(sensorTimer);
  state.sensorGeneration += 1;
  sensorController?.abort();
  sensorController = new AbortController();
  const image = $('cameraImage');
  image.removeAttribute('src');
  image.style.display = 'none';
  if (state.cameraUrl) URL.revokeObjectURL(state.cameraUrl);
  state.cameraUrl = null;
  lidarView?.clear();
  for (const panel of ['camera', 'lidar']) {
    setSensorEmpty(panel, 'Waiting for selected timestamp');
    for (const field of ['Requested', 'Timestamp', 'Delta']) $(`${panel}${field}`).textContent = '—';
  }
}

async function loadSelectedSensors() {
  if (!state.trip || !state.selectedNs) return;
  const generation = state.sensorGeneration;
  const run = async (panel, load) => {
    try { await load(generation); }
    catch (error) {
      if (generation !== state.sensorGeneration || error.name === 'AbortError') return;
      if (panel !== 'gps') {
        setSensorEmpty(panel, 'No sample displayed', error.message);
        $(`${panel}State`).textContent = 'UNAVAILABLE';
      }
      showToast(error.message, true);
    }
  };
  const jobs = [];
  if (state.access.position?.authorized) jobs.push(run('gps', loadGpsClosest));
  if (state.access['camera.front']?.authorized) jobs.push(run('camera', loadCameraClosest));
  if (state.access['lidar.top']?.authorized) jobs.push(run('lidar', loadLidarClosest));
  await Promise.all(jobs);
}

async function closest(resource) {
  return trackedFetch(api('/api/closest', {trip: state.trip.id, resource, t_ns: state.selectedNs}), {signal: sensorController?.signal});
}

async function loadGpsClosest(generation) {
  const {response, body} = await closest('position');
  if (generation !== state.sensorGeneration) return;
  const records = parseRecordStream(body);
  if (!records.length) throw new Error('No GPS record near selected T');
  state.recordsReturned += records.length;
  const point = decodeGps(records[0].payload);
  updateCurrentPosition(point, response.headers.get('x-demo-record-timestamp'));
  $('gpsDelta').textContent = formatDelta(BigInt(response.headers.get('x-demo-delta-ns') || '0'));
  updateBandwidth();
}

async function loadCameraClosest(generation) {
  $('cameraState').textContent = 'LOADING';
  const {response, body} = await closest('camera.front');
  if (generation !== state.sensorGeneration) return;
  const records = parseRecordStream(body);
  if (!records.length) throw new Error('No retained camera frame near selected T');
  state.recordsReturned += records.length;
  const url = URL.createObjectURL(new Blob([records[0].payload], {type: 'image/jpeg'}));
  const decoded = new Image();
  decoded.src = url;
  try { await decoded.decode(); }
  catch (error) { URL.revokeObjectURL(url); throw error; }
  if (generation !== state.sensorGeneration) { URL.revokeObjectURL(url); return; }
  state.cameraUrl = url;
  $('cameraImage').src = url;
  setCameraZoom(1);
  reportClientStatus({state: 'camera-rendered', role: state.role,
    camera_width: decoded.naturalWidth, camera_height: decoded.naturalHeight});
  $('cameraImage').style.display = 'block';
  $('cameraEmpty').style.display = 'none';
  updateSensorReadout('camera', response);
  $('cameraState').textContent = 'AUTHORIZED';
  updateBandwidth();
}

async function loadLidarClosest(generation) {
  $('lidarState').textContent = 'DECODING ON HOST';
  const {response, body} = await closest('lidar.top');
  if (generation !== state.sensorGeneration) return;
  const records = parseRecordStream(body);
  if (!records.length) throw new Error('No LiDAR snapshot near selected T');
  const positions = await decodeLaz(records[0].payload);
  if (generation !== state.sensorGeneration) return;
  const cloud = lidarView.setCloud(positions);
  reportClientStatus({
    state: 'lidar-rendered',
    role: state.role,
    lidar_points: cloud.count,
    lidar_radius: cloud.radius.toFixed(2),
    lidar_span_x: cloud.spans[0].toFixed(2),
    lidar_span_y: cloud.spans[1].toFixed(2),
    lidar_span_z: cloud.spans[2].toFixed(2),
  });
  state.recordsReturned += records.length;
  $('lidarEmpty').style.display = 'none';
  updateSensorReadout('lidar', response);
  $('lidarState').textContent = `${cloud.count.toLocaleString()} POINTS`;
  updateBandwidth();
}

function updateSensorReadout(prefix, response) {
  const requested = response.headers.get('x-demo-requested-t');
  const timestamp = response.headers.get('x-demo-record-timestamp');
  const delta = BigInt(response.headers.get('x-demo-delta-ns') || '0');
  $(`${prefix}Requested`).textContent = formatInstant(requested);
  $(`${prefix}Timestamp`).textContent = formatInstant(timestamp);
  $(`${prefix}Delta`).textContent = formatDelta(delta);
}

function setupTimeline() {
  const canvas = $('timeline');
  let dragging = false;
  $('brakeEventSelect').addEventListener('change', (event) => {
    if (event.target.value) setSelectedTime(event.target.value);
  });
  const choose = (event) => {
    if (!state.trip) return;
    const rect = canvas.getBoundingClientRect();
    const left = 112;
    const right = rect.width - 24;
    const marker = nearestBrakeEvent(event.clientX - rect.left, left, right);
    if (marker) { setSelectedTime(marker.timestamp_ns); return; }
    const ratio = Math.max(0, Math.min(1, (event.clientX - rect.left - left) / Math.max(1, right - left)));
    const span = BigInt(state.trip.end_ns) - BigInt(state.trip.start_ns);
    setSelectedTime(String(BigInt(state.trip.start_ns) + BigInt(Math.round(Number(span) * ratio))));
  };
  canvas.addEventListener('pointerdown', (event) => { dragging = true; canvas.setPointerCapture(event.pointerId); choose(event); });
  canvas.addEventListener('pointermove', (event) => {
    if (dragging) choose(event);
    const rect = canvas.getBoundingClientRect();
    const marker = nearestBrakeEvent(event.clientX - rect.left, 112, rect.width - 24);
    canvas.title = marker ? `Hard-braking candidate · ${formatInstant(marker.timestamp_ns, true)}` : '';
  });
  canvas.addEventListener('pointerup', () => { dragging = false; });
  window.addEventListener('resize', drawTimeline);
}

function drawTimeline() {
  const canvas = $('timeline');
  const rect = canvas.getBoundingClientRect();
  const ratio = Math.min(window.devicePixelRatio || 1, 2);
  canvas.width = Math.max(1, rect.width * ratio);
  canvas.height = Math.max(1, rect.height * ratio);
  const ctx = canvas.getContext('2d');
  ctx.scale(ratio, ratio);
  const width = rect.width;
  const left = 112;
  const right = width - 24;
  const rows = [
    {resource: 'camera.front', label: 'CAMERA', y: 34, color: '#f4b763'},
    {resource: 'lidar.top', label: 'LIDAR', y: 72, color: '#6a9cff'},
    {resource: 'position', label: 'GPS', y: 110, color: '#51d9d1'},
  ];
  ctx.clearRect(0, 0, width, rect.height);
  ctx.font = '650 9px ui-monospace, monospace';
  rows.forEach((row) => {
    ctx.fillStyle = state.access[row.resource]?.authorized ? '#aebdc1' : '#536267';
    ctx.fillText(row.label, 17, row.y + 3);
    ctx.strokeStyle = '#26363a';
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(left, row.y); ctx.lineTo(right, row.y); ctx.stroke();
    if (!state.trip || !state.access[row.resource]?.authorized) {
      ctx.fillStyle = '#536267'; ctx.fillText('LOCKED BY pDAL', left + 8, row.y - 6); return;
    }
    ctx.strokeStyle = row.color;
    ctx.fillStyle = row.color;
    if (row.resource === 'position') {
      ctx.lineWidth = 3; ctx.beginPath(); ctx.moveTo(left, row.y); ctx.lineTo(right, row.y); ctx.stroke();
      return;
    }
    const timestamps = state.timeline[row.resource] || [];
    const occupied = new Set();
    timestamps.forEach((timestamp) => {
      const x = timeToX(timestamp, left, right);
      const pixel = Math.round(x);
      if (occupied.has(pixel)) return;
      occupied.add(pixel);
      ctx.beginPath(); ctx.arc(x, row.y, 2.2, 0, Math.PI * 2); ctx.fill();
    });
    ctx.fillStyle = '#64757a';
    ctx.fillText(`${timestamps.length.toLocaleString()} retained`, right - 92, row.y - 7);
  });
  if (state.trip && state.selectedNs) {
    const x = timeToX(state.selectedNs, left, right);
    ctx.strokeStyle = '#e9f0f2'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(x, 15); ctx.lineTo(x, 130); ctx.stroke();
    ctx.fillStyle = '#e9f0f2'; ctx.beginPath(); ctx.moveTo(x - 5, 13); ctx.lineTo(x + 5, 13); ctx.lineTo(x, 19); ctx.fill();
    ctx.font = '700 9px ui-monospace, monospace'; ctx.fillText('T', x + 7, 21);
  }
  if (state.trip) {
    ctx.strokeStyle = '#ff4545'; ctx.lineWidth = 2;
    for (const event of state.brakeEvents) {
      const x = timeToX(event.timestamp_ns, left, right);
      ctx.beginPath(); ctx.moveTo(x, 15); ctx.lineTo(x, 130); ctx.stroke();
    }
  }
}

function timeToX(ns, left, right) {
  if (!state.trip) return left;
  const start = BigInt(state.trip.start_ns);
  const span = BigInt(state.trip.end_ns) - start;
  const ratio = Number(BigInt(ns) - start) / Number(span || 1n);
  return left + ratio * (right - left);
}

async function runDenialProof() {
  if (!state.trip) return;
  try {
    await fetchJson(api('/api/denial-proof', {trip: state.trip.id, resource: 'camera.front'}));
    showToast('Unexpectedly allowed; inspect demo policy', true);
  } catch (error) {
    if (error.isSessionExpiry) return;
    const details = error.details || {};
    $('denialSummary').textContent = `${state.role} requested Front Camera for their purpose. pDAL returned HTTP ${error.status}; zero camera payload bytes crossed the network.`;
    $('denialDetails').textContent = JSON.stringify(details, null, 2);
    $('denialModal').classList.add('visible');
    $('denialModal').setAttribute('aria-hidden', 'false');
  }
}

function closeModal() {
  $('denialModal').classList.remove('visible');
  $('denialModal').setAttribute('aria-hidden', 'true');
}

function showToast(message, error = false) {
  clearTimeout(toastTimer);
  $('toast').textContent = message;
  $('toast').classList.toggle('error', error);
  $('toast').classList.add('visible');
  toastTimer = setTimeout(() => $('toast').classList.remove('visible'), 3800);
}

function bindEvents() {
  $('storageFilter').addEventListener('change', () => {
    renderTrips();
    const tier = $('storageFilter').value;
    const next = state.trips.find((trip) => !tier || trip.storage_locations?.includes(tier));
    if (next && tier && !state.trip?.storage_locations?.includes(tier)) selectTrip(next.id);
  });

  $('loginForm').addEventListener('submit', (event) => { event.preventDefault(); doLogin(); });
  $('logoutButton').addEventListener('click', doLogout);
  $('denialButton').addEventListener('click', runDenialProof);
  $('closeModal').addEventListener('click', closeModal);
  $('modalDone').addEventListener('click', closeModal);
  $('denialModal').addEventListener('click', (event) => { if (event.target === $('denialModal')) closeModal(); });
}

async function boot() {
  initMap();
  initLidar();
  setupCameraView();
  setupTimeline();
  bindEvents();
  try {
    const health = await fetchJson('/api/health');
    if (health.decode_location !== 'host') throw new Error('Invalid deployment boundary: decoding must run on host');
    document.querySelector('.pulse').classList.add('ready');
    await loadRoles();

    // Try to restore an existing session from sessionStorage
    const storedToken = sessionStorage.getItem('pdal_token');
    if (storedToken) {
      state.token = storedToken;
      try {
        const sessionData = await fetchJson('/api/session');
        if (sessionData.authenticated) {
          state.role = sessionData.role;
          $('loginRoleSelect').value = sessionData.role;
          $('sessionRoleLabel').textContent = sessionData.label || sessionData.role;
          showApp();
          startCountdown(sessionData.expires_in);
          $('connectionLabel').textContent = `Vehicle online · ${sessionData.label || sessionData.role}`;
          await loadTrips();
          return;
        }
      } catch {
        // 401 already handled in trackedFetch (clearSession + showLogin called).
        // For any other error, fall through to show the login form.
        clearSession();
        showLogin();
        return;
      }
    }

    showLogin();
    $('connectionLabel').textContent = 'Awaiting login…';
  } catch (error) {
    $('connectionLabel').textContent = 'Vehicle connection failed';
    showToast(error.message, true);
  }
}



async function fetchBodyTimed(url, options) {
  const request = new URL(url, window.location.href);
  const isData = ['/api/history', '/api/closest'].includes(request.pathname);
  const generation = state.tripGeneration;
  const trip = state.trip;
  const role = state.role;
  const id = ++retrievalSequence;
  const started = performance.now();
  let response;
  let complete = false;
  try {
    response = await fetch(url, options);
    const body = await response.arrayBuffer();
    complete = true;
    return {response, body};
  } finally {
    const elapsed = performance.now() - started;
    if (isData && generation === state.tripGeneration && state.trip === trip && state.role === role) {
      const resource = request.searchParams.get('resource');
      state.retrievals.push({id, resource, kind: request.pathname.endsWith('closest') ? 'sample' : 'history',
        timestamp: request.searchParams.get('t_ns') || request.searchParams.get('start_ns'),
        elapsed, status: complete ? response.status : options.signal?.aborted ? 'cancelled' : 'transfer failed'});
      state.retrievals.sort((a, b) => b.id - a.id);
      state.retrievals = state.retrievals.slice(0, 50);
      renderRetrievalLatency();
    }
  }
}


function renderRetrievalLatency() {
  const format = (entry) => {
    const label = {position: 'GPS', 'camera.front': 'Camera', 'lidar.top': 'LiDAR'}[entry.resource] || 'Data';
    const duration = entry.elapsed >= 1000 ? `${(entry.elapsed / 1000).toFixed(2)} s` : `${entry.elapsed.toFixed(1)} ms`;
    return `${label} ${entry.kind}: ${duration} · ${entry.status}`;
  };
  $('retrievalLatency').textContent = state.retrievals.length ? format(state.retrievals[0]) : 'No data requests yet';
  const list = $('retrievalHistory'); list.replaceChildren();
  for (const entry of state.retrievals) {
    const row = document.createElement('li');
    row.textContent = `${formatInstant(entry.timestamp, true)} · ${format(entry)}`;
    list.append(row);
  }
}


function storageLabel(item) {
  return (item?.storage_locations || []).filter((tier) => tier === 'SSD' || tier === 'HDD').join(' + ') || 'Location unavailable';
}


function brakeSummary() {
  if (state.brakeStatus === 'ready') return `HARD-BRAKING CANDIDATES · ${state.brakeEvents.length}`;
  return {
    loading: 'Checking brake events…', denied: 'Brake events restricted by pDAL',
    'not-recorded': 'No brake records in this time range',
    unavailable: 'Brake event scan unavailable',
  }[state.brakeStatus] || 'Brake events unavailable';
}


function renderBrakeEvents() {
  const select = $('brakeEventSelect');
  select.replaceChildren(new Option('Jump to hard-braking candidate…', ''));
  for (const event of state.brakeEvents) {
    select.add(new Option(formatInstant(event.timestamp_ns, true), event.timestamp_ns));
  }
  select.hidden = state.brakeEvents.length === 0;
  $('timelineHint').textContent = state.brakeEvents.length
    ? 'Red lines: hard-braking candidates · click to jump'
    : brakeSummary();
}


function nearestBrakeEvent(x, left, right) {
  let nearest = null;
  let distance = 7;
  for (const event of state.brakeEvents) {
    const delta = Math.abs(timeToX(event.timestamp_ns, left, right) - x);
    if (delta < distance) { nearest = event; distance = delta; }
  }
  return nearest;
}

boot();
