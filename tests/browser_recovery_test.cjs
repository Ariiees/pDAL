const {chromium} = require('playwright');
const http = require('http');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

(async () => {
  const root = path.resolve(__dirname, '../host/viewer');
  let online = false, token = 'session-1', sessionChecks = 0;
  const role = {id: 'incident_investigator', label: 'Incident Investigator'};
  const trip = {id: 'test-trip', label: 'Test trip', start_ns: '1789387200000000000',
    end_ns: '1789387210000000000', duration_ns: '10000000000', candidate_bytes: 100,
    modalities: [], storage_locations: ['SSD']};
  const server = http.createServer((req, res) => {
    const route = req.url.split('?')[0];
    const json = (body, status = 200) => {
      res.writeHead(status, {'Content-Type': 'application/json'});
      res.end(JSON.stringify(body));
    };
    if (route.startsWith('/api/')) {
      if (!online) return json({error: 'Pi is offline'}, 502);
      if (route === '/api/health') return json({status: 'ready', decode_location: 'host'});
      if (route === '/api/roles') return json({roles: [role]});
      if (route === '/api/login') return json({token, role: role.id, label: role.label, expires_in: 900});
      if (route === '/api/logout') return json({ok: true});
      if (req.headers.authorization !== `Bearer ${token}`) return json({error: 'expired'}, 401);
      if (route === '/api/session') {
        sessionChecks++;
        return json({authenticated: true, role: role.id, label: role.label, expires_in: 900});
      }
      if (route === '/api/trips') return json({recordings: [trip], access: {}});
      if (route === '/api/access') return json({resources: {}});
      if (route === '/api/timeline') return json({tracks: {}, access: {}});
      return json({});
    }
    if (route === '/client-status') return json({});
    const file = route === '/' ? 'index.html' : route.slice(1);
    if (!['index.html', 'app.js', 'styles.css'].includes(file)) return json({}, 404);
    res.writeHead(200, {'Content-Type': file.endsWith('.js') ? 'text/javascript' :
      file.endsWith('.css') ? 'text/css' : 'text/html', 'Cache-Control': 'no-store'});
    res.end(fs.readFileSync(path.join(root, file)));
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await chromium.launch({headless: true});
  try {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    // Simulate a CDN outage: the real app module and login must still work.
    await page.route('https://**/*', route => route.abort());
    await page.goto(base);
    await page.waitForFunction(() => document.getElementById('loginError').textContent.includes('Retrying'));
    assert(await page.locator('#loginSubmit').isDisabled());
    online = true;
    // Automatic retry, with no page reload or button press.
    await page.waitForFunction(() => !document.getElementById('loginSubmit').disabled, null, {timeout: 10000});
    assert.equal(await page.locator('#loginRoleSelect option').count(), 1);
    await page.selectOption('#loginRoleSelect', role.id);
    await page.fill('#loginPassword', 'incident-demo');
    await page.click('#loginSubmit');
    await page.waitForFunction(() => !document.getElementById('loginScreen').classList.contains('visible'));
    for (let i = 0; i < 3; i++) {
      await page.reload();
      await page.waitForFunction(() => !document.getElementById('loginScreen').classList.contains('visible'));
    }
    assert.equal(sessionChecks, 3);
    online = false;
    await page.reload();
    await page.waitForFunction(() => document.getElementById('loginError').textContent.includes('Retrying'));
    assert.equal(await page.evaluate(() => sessionStorage.getItem('pdal_token')), token);
    online = true;
    await page.click('#retryConnection');
    await page.waitForFunction(() => !document.getElementById('loginScreen').classList.contains('visible'));
    token = 'session-2'; // Pi restart / expired token
    await page.reload();
    await page.waitForFunction(() => !document.getElementById('loginSubmit').disabled);
    assert(await page.locator('#loginScreen').evaluate(el => el.classList.contains('visible')));
    assert.equal(await page.evaluate(() => sessionStorage.getItem('pdal_token')), null);
    await page.fill('#loginPassword', 'incident-demo');
    await page.click('#loginSubmit');
    await page.waitForFunction(() => !document.getElementById('loginScreen').classList.contains('visible'));
    // A dead logout endpoint must not prevent immediate local logout.
    await page.route('**/api/logout', () => {});
    await page.click('#logoutButton');
    await page.waitForFunction(() => document.getElementById('loginScreen').classList.contains('visible'));
    assert.equal(await page.evaluate(() => sessionStorage.getItem('pdal_token')), null);
    await page.route('**/api/login', () => {});
    await page.fill('#loginPassword', 'incident-demo');
    await page.click('#loginSubmit');
    await page.waitForFunction(() => document.getElementById('loginError').textContent.includes('timed out'), null,
      {timeout: 15000});
    assert(!(await page.locator('#loginSubmit').isDisabled()));
    await page.unroute('**/api/login');
    assert.deepEqual(errors, []);
    console.log(JSON.stringify({cdn_failure_login: true, offline_auto_retry: true,
      refresh_session_restore: 3, offline_session_preserved: true,
      expired_session_relogin: true, offline_logout_immediate: true, hung_login_timeout: true}, null, 2));
  } finally {
    await browser.close();
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => {console.error(error); process.exit(1);});
