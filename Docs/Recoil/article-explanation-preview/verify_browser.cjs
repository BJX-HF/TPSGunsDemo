const {spawn}=require('node:child_process');
const fs=require('node:fs');
const path=require('node:path');
const {pathToFileURL}=require('node:url');
const root=__dirname;
const page=path.resolve(root,'../FPS相机后坐力与伪代码详解.html');
const port=19361;
const browser=spawn('C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',[
 '--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check',
 '--disable-background-networking','--disable-component-update',
 `--user-data-dir=${path.join(root,'chrome-qa-profile')}`,
 `--remote-debugging-port=${port}`,'about:blank'
],{windowsHide:true,stdio:'ignore'});
let ws;
const pending=new Map();let next=0;const errors=[];
const delay=ms=>new Promise(r=>setTimeout(r,ms));
const assert=(ok,message)=>{if(!ok)throw Error(message)};
async function send(method,params={}){const id=++next;return new Promise((resolve,reject)=>{pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}));});}
async function evaluate(expression){const r=await send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error(r.exceptionDetails.text);return r.result.value;}
async function screenshot(name){const r=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});fs.writeFileSync(path.join(root,name),Buffer.from(r.data,'base64'));}
(async()=>{
 let tabs;
 for(let i=0;i<60;i++){try{tabs=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();if(tabs.length)break;}catch{}await delay(100);}
 assert(tabs?.length,'Chrome debug endpoint unavailable');
 ws=new WebSocket(tabs.find(t=>t.type==='page').webSocketDebuggerUrl);
 await new Promise((resolve,reject)=>{ws.addEventListener('open',resolve,{once:true});ws.addEventListener('error',reject,{once:true});});
 ws.addEventListener('message',event=>{const m=JSON.parse(event.data);if(m.id){const p=pending.get(m.id);pending.delete(m.id);if(m.error)p.reject(Error(m.error.message));else p.resolve(m.result);}else if(m.method==='Runtime.exceptionThrown')errors.push(m.params.exceptionDetails.text);});
 await send('Runtime.enable');await send('Page.enable');
 await send('Emulation.setDeviceMetricsOverride',{width:1440,height:1080,deviceScaleFactor:1,mobile:false});
 await send('Page.navigate',{url:pathToFileURL(page).href});
 for(let i=0;i<40;i++){if(await evaluate('document.readyState === "complete"'))break;await delay(100);}
 const desktop=await evaluate(`({title:document.title,sections:document.querySelectorAll('section').length,images:[...document.images].every(i=>i.complete&&i.naturalWidth>0),overflow:document.documentElement.scrollWidth>innerWidth,phase:document.getElementById('phaseText').textContent})`);
 assert(desktop.sections===14&&desktop.images&&!desktop.overflow,'Desktop layout/image check failed');
 await screenshot('desktop-top.png');
 await evaluate(`document.getElementById('demo').scrollIntoView({behavior:'instant'});`);await delay(100);await screenshot('desktop-demo.png');
 const stable=await evaluate(`(()=>{const el=document.getElementById('scrubTime');el.value='260';el.dispatchEvent(new Event('input'));return {phase:document.getElementById('phaseText').textContent,offset:document.getElementById('offsetText').textContent,delta:document.getElementById('deltaText').textContent}})()`);
 assert(stable.phase.includes('稳定')&&stable.offset==='1.300°'&&stable.delta==='0.000°','Stable-stage readout failed');
 const end=await evaluate(`(()=>{const el=document.getElementById('scrubTime');el.value='1000';el.dispatchEvent(new Event('input'));return {phase:document.getElementById('phaseText').textContent,offset:document.getElementById('offsetText').textContent}})()`);
 assert(end.offset==='0.000°'&&end.phase.includes('结束'),'Endpoint reset failed');
 await evaluate(`document.getElementById('resetBtn').click();document.getElementById('playBtn').click();`);await delay(1800);
 const playback=await evaluate(`({position:document.getElementById('scrubTime').value,label:document.getElementById('playBtn').textContent})`);
 assert(playback.position==='1000'&&playback.label.includes('播放'),'Animation completion failed');
 await send('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});
 await evaluate(`scrollTo({top:0,behavior:'instant'});dispatchEvent(new Event('resize'));`);await delay(100);await screenshot('mobile-top.png');
 assert(!await evaluate('document.documentElement.scrollWidth>innerWidth'),'Mobile viewport overflow');
 await evaluate(`document.getElementById('demo').scrollIntoView({behavior:'instant'});dispatchEvent(new Event('resize'));`);await delay(100);await screenshot('mobile-demo.png'); await evaluate(`document.getElementById('recoilCanvas').scrollIntoView({behavior:'instant'});`); await screenshot('mobile-chart.png');
 assert(errors.length===0,`Browser exceptions: ${errors.join(', ')}`);
 console.log(JSON.stringify({desktop,stable,end,playback,mobileOverflow:false,exceptions:errors,screenshots:4},null,2));
 await send('Browser.close');ws.close();
})().catch(error=>{console.error(error);process.exitCode=1;if(ws)ws.close();browser.kill();});
