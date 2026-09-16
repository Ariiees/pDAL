const {chromium}=require('playwright');
const fs=require('fs'),assert=require('assert');
(async()=>{
 const root=require('path').resolve(__dirname,'../host/viewer');
 const browser=await chromium.launch({headless:true});const page=await browser.newPage();
 await page.route('http://test/**',r=>r.fulfill({body:'',contentType:'text/html'}));await page.goto('http://test');
 await page.setContent(fs.readFileSync(root+'/index.html','utf8').replace(/<script\b[^>]*>[\s\S]*?<\/script>/g,'').replace(/<link\b[^>]*>/g,''));
 let app=fs.readFileSync(root+'/app.js','utf8').replace(/^import .*;\n/gm,'').replace(/\nboot\(\);\s*$/,'');
 app+=`\nwindow.testApi={state,loadAccess,loadTimeline,loadRoute,loadCameraClosest,loadSelectedSensors,selectTrip,setSelectedTime,renderAccess,invalidateSensors};`;
 await page.addScriptTag({content:app});
 const result=await page.evaluate(async()=>{
  const {state}=testApi;let pending=[];
  const wait=ms=>new Promise(r=>setTimeout(r,ms));
  const make=id=>({id,label:id,start_ns:'1789387200000000000',end_ns:'1789387210000000000',duration_ns:'10000000000',candidate_bytes:1000,storage_locations:['HDD'],modalities:[]});
  state.trips=[make('a'),make('b')];state.trip=state.trips[0];state.selectedNs=state.trip.start_ns;
  const ok={'camera.front':{authorized:true}};
  const denied={'camera.front':{authorized:false,status:403}};
  const json=o=>new Response(JSON.stringify(o));
  window.fetch=url=>url.startsWith('/client-status') ? Promise.resolve(new Response('')) : new Promise(resolve=>pending.push({url,resolve}));
  const old=testApi.loadAccess();state.trip=state.trips[1];state.tripGeneration++;
  state.access=ok;pending.shift().resolve(json({resources:denied}));await old;
  if(!state.access['camera.front'].authorized)throw Error('stale access overwrote selection');
  const timeline=testApi.loadTimeline();pending.shift().resolve(json({tracks:{},access:denied}));await timeline;
  if(!state.access['camera.front'].authorized)throw Error('timeline overwrote access');
  state.access={'camera.front':{authorized:false,status:503}};testApi.renderAccess();
  if(document.getElementById('cameraLock').classList.contains('visible'))throw Error('503 shown as locked');
  state.access=denied;testApi.renderAccess();
  if(!document.getElementById('cameraLock').classList.contains('visible'))throw Error('403 lock missing');
  state.access=ok;testApi.renderAccess();testApi.invalidateSensors();
  const canvas=document.createElement('canvas');canvas.width=2;canvas.height=2;canvas.getContext('2d').fillRect(0,0,2,2);
  const blob=await new Promise(r=>canvas.toBlob(r,'image/jpeg'));const payload=new Uint8Array(await blob.arrayBuffer());
  const frame=(timestamp,bytes=payload)=>{
   const metadata=new TextEncoder().encode(JSON.stringify({timestamp_ns:timestamp}));
   const body=new Uint8Array(20+metadata.length+bytes.length);body.set(new TextEncoder().encode('PDALSTR1'));const view=new DataView(body.buffer);
   view.setUint32(8,metadata.length);view.setBigUint64(12,BigInt(bytes.length));body.set(metadata,20);body.set(bytes,20+metadata.length);
   return new Response(body,{headers:{'x-demo-requested-t':timestamp,'x-demo-record-timestamp':timestamp,'x-demo-delta-ns':'0'}});
  };
  const oldCamera=testApi.loadCameraClosest(state.sensorGeneration);const request=pending.shift();
  testApi.setSelectedTime('1789387201000000000');request.resolve(frame('1789387200000000000'));await oldCamera;
  if(document.getElementById('cameraImage').getAttribute('src'))throw Error('stale camera during debounce');
  // Remove scheduled load and deliver a valid, decoded JPEG.
  testApi.invalidateSensors();const current=testApi.loadSelectedSensors();pending.shift().resolve(frame(state.selectedNs));await current;
  if(document.getElementById('cameraImage').style.display!=='block')throw Error('valid camera not displayed');
  testApi.invalidateSensors();const broken=testApi.loadSelectedSensors();pending.shift().resolve(frame(state.selectedNs,new Uint8Array([1,2,3])));await broken;
  if(document.getElementById('cameraImage').getAttribute('src'))throw Error('failed frame left stale camera');
  if(document.getElementById('cameraState').textContent!=='UNAVAILABLE')throw Error('decode failure not reported');
  // Switching trips waits for current access, then loads the camera even if old access was denied.
  pending=[];window.fetch=async url=>{
   if(url.startsWith('/api/access')){await wait(25);return json({resources:ok});}
   if(url.startsWith('/api/timeline'))return json({tracks:{},access:denied});
   if(url.startsWith('/api/closest'))return frame(state.selectedNs);
   return new Response('');
  };
  state.access=denied;await testApi.selectTrip('a');await wait(80);
  if(document.getElementById('cameraImage').style.display!=='block')throw Error('camera not loaded after access resolved');
  if(document.getElementById('cameraLock').classList.contains('visible'))throw Error('false lock after trip switch');
  return {stale_access_ignored:true,timeline_cannot_override_access:true,unavailable_not_denied:true,real_denial_preserved:true,old_frame_during_debounce_ignored:true,jpeg_decoded_before_display:true,failed_decode_clears_image:true,trip_switch_loads_after_access:true};
 });
 console.log(JSON.stringify(result,null,2));await browser.close();
})().catch(e=>{console.error(e);process.exit(1)});
