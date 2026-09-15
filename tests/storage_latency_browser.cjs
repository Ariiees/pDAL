const {chromium}=require('playwright');
const fs=require('fs'),assert=require('assert');
(async()=>{
 const root=require('path').resolve(__dirname,'../host/viewer');const browser=await chromium.launch({headless:true});
 const page=await browser.newPage({viewport:{width:1500,height:1000}});
 await page.route('**/*',route=>route.abort());
 await page.setContent(fs.readFileSync(root+'/index.html','utf8').replace(/<script\b[^>]*>[\s\S]*?<\/script>/g,'').replace(/<link\b[^>]*>/g,''));
 await page.evaluate(()=>document.getElementById('loginScreen')?.classList.remove('visible'));
 await page.addStyleTag({content:fs.readFileSync(root+'/styles.css','utf8')});
 let app=fs.readFileSync(root+'/app.js','utf8').replace(/^import .*;\n/gm,'').replace(/\nboot\(\);\s*$/,'');
 app+='\nwindow.testApi={state,renderTrips,renderContext,fetchBodyTimed,renderRetrievalLatency,bindEvents};';
 await page.addScriptTag({content:app});
 await page.evaluate(()=>{
  const {state,renderTrips,renderContext}=testApi;
  const make=(id,locations)=>({id,label:id,start_ns:'1789387200000000000',end_ns:'1789387209900000000',duration_ns:'9900000000',candidate_bytes:1000,storage_locations:locations,modalities:[{resource:'position',start_ns:'1789387200000000000',end_ns:'1789387209900000000',stored_bytes:1000,record_count:100,storage_locations:locations}]});
  state.trips=[make('SSD recording',['SSD']),make('HDD archive',['HDD']),make('Both copies',['SSD','HDD'])];
  state.trip=state.trips[1];state.selectedNs=state.trip.start_ns;state.brakeStatus='not-recorded';renderTrips();renderContext();
 });
 assert((await page.locator('#tripTitle').innerText()).includes('HDD'));
 await page.selectOption('#storageFilter','HDD');await page.evaluate(()=>testApi.renderTrips());
 assert.equal(await page.locator('.trip-card').count(),2);
 assert.equal(await page.locator('#tripCount').innerText(),'2/3');
 assert((await page.locator('#tripList').innerText()).includes('SSD + HDD'));
 const timing=await page.evaluate(async()=>{
  window.fetch=async()=>{await new Promise(r=>setTimeout(r,50));return {status:200,arrayBuffer:async()=>{await new Promise(r=>setTimeout(r,150));return new ArrayBuffer(80)}}};
  await testApi.fetchBodyTimed('http://test/api/closest?resource=camera.front&t_ns=1789387202000000000',{});
  return testApi.state.retrievals[0];
 });
 assert(timing.elapsed>=190 && timing.elapsed<1500,timing.elapsed);
 assert((await page.locator('#retrievalLatency').innerText()).includes('Camera sample'));
 const failed=await page.evaluate(async()=>{
  window.fetch=async()=>({status:200,arrayBuffer:async()=>{throw new Error('partial response')}});
  try{await testApi.fetchBodyTimed('http://test/api/closest?resource=lidar.top&t_ns=1789387202000000000',{})}catch{}
  return testApi.state.retrievals[0].status;
 });
 assert.equal(failed,'transfer failed');
 const stale=await page.evaluate(async()=>{
  let finish;window.fetch=()=>new Promise(resolve=>finish=resolve);
  const before=testApi.state.retrievals.length;
  const pending=testApi.fetchBodyTimed('http://test/api/closest?resource=position&t_ns=1789387202000000000',{});
  testApi.state.trip={...testApi.state.trip};finish({status:200,arrayBuffer:async()=>new ArrayBuffer(48)});
  await pending;return testApi.state.retrievals.length===before;
 });
 assert(stale);
 await page.locator('.retrieval-latency summary').click();
 await page.screenshot({path:require('path').join(require('os').tmpdir(),'pdal-storage-latency-browser.png')});
 const box=await page.locator('.retrieval-latency').boundingBox();assert(box.x+box.width<=1500 && box.y+box.height<=1000,JSON.stringify(box));
 fs.writeFileSync(require('path').join(require('os').tmpdir(),'pdal-browser-validation.json'),JSON.stringify({ssd_hdd_both_labels:true,hdd_filter:true,full_body_latency_ms:timing.elapsed,body_failure_reported:true,stale_trip_timing_ignored:true,footer_inside_viewport:true},null,2)+'\n');
 console.log('Storage and retrieval latency browser checks passed');await browser.close();
})().catch(e=>{console.error(e);process.exit(1)});
