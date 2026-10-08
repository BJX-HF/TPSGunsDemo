
(()=>{
'use strict';
const $=id=>document.getElementById(id);
const defaults={peak:2,fall:.65,up:.06,hold:.08,down:.24,rollA:.7};
let playing=false,lastFrame=0,elapsed=0,raf=0;
const ease=u=>1-Math.pow(1-Math.max(0,Math.min(1,u)),3);
function params(){const p={};for(const id of Object.keys(defaults))p[id]=Number($(id).value);p.total=p.up+p.hold+p.down;return p;}
function offset(t,p){if(t<p.up)return p.peak*ease(t/p.up);if(t<p.up+p.hold)return p.peak*p.fall;if(t<p.total)return p.peak*p.fall*(1-ease((t-p.up-p.hold)/p.down));return 0;}
function roll(t,p){return t>=p.total?0:p.rollA*Math.max(0,1-t/p.total)*Math.cos(2*Math.PI*t/.07);}
function phase(t,p){if(t<p.up)return '上抬';if(t<p.up+p.hold)return '稳定（已回弹）';if(t<p.total)return '下降';return '结束 / 归零';}
function draw(canvas,p,t,isRoll){
 const rect=canvas.getBoundingClientRect(),dpr=window.devicePixelRatio||1;
 const w=Math.max(280,rect.width),h=rect.height;
 canvas.width=Math.round(w*dpr);canvas.height=Math.round(h*dpr);
 const c=canvas.getContext('2d');c.setTransform(dpr,0,0,dpr,0,0);c.clearRect(0,0,w,h);
 const m={l:50,r:20,t:32,b:34},pw=w-m.l-m.r,ph=h-m.t-m.b;
 const hi=isRoll?Math.max(.2,p.rollA*1.15):p.peak*1.16,lo=isRoll?-hi:0;
 const X=x=>m.l+x/p.total*pw,Y=y=>m.t+(hi-y)/(hi-lo)*ph;
 const shades=['#edf4fc','#eaf7f2','#f4eef9'];const bounds=[0,p.up,p.up+p.hold,p.total];
 if(!isRoll){for(let i=0;i<3;i++){c.fillStyle=shades[i];c.fillRect(X(bounds[i]),m.t,X(bounds[i+1])-X(bounds[i]),ph);}}
 c.font='11px system-ui, Microsoft YaHei';c.fillStyle='#53677c';
 for(let j=0;j<=4;j++){const yy=lo+(hi-lo)*j/4;c.strokeStyle='#e0e7ee';c.beginPath();c.moveTo(m.l,Y(yy));c.lineTo(w-m.r,Y(yy));c.stroke();c.fillText(yy.toFixed(1)+'°',5,Y(yy)+4);}
 for(let j=0;j<=5;j++){const tt=p.total*j/5;c.fillStyle='#64748b';c.textAlign='center';c.fillText(tt.toFixed(2),X(tt),h-12);}
 c.textAlign='left';c.fillStyle='#30445b';c.font='12px system-ui, Microsoft YaHei';c.fillText(isRoll?'Roll 当前偏移：衰减包络 × cos':'Pitch 后坐力偏移 q(t)',m.l,18);
 if(!isRoll){c.fillStyle='#6b7c90';c.font='11px system-ui, Microsoft YaHei';const labs=['上抬','稳定','下降'];for(let i=0;i<3;i++){if(X(bounds[i+1])-X(bounds[i])>35){c.textAlign='center';c.fillText(labs[i],X((bounds[i]+bounds[i+1])/2),m.t+16);}}c.textAlign='left';}
 c.strokeStyle=isRoll?'#7060a0':'#2563a6';c.lineWidth=2.3;c.beginPath();
 if(isRoll){for(let j=0;j<=700;j++){const tt=p.total*j/700,jy=Y(roll(tt,p));j?c.lineTo(X(tt),jy):c.moveTo(X(tt),jy);}}
 else{c.moveTo(X(0),Y(0));for(let j=1;j<=120;j++){const tt=p.up*j/120;c.lineTo(X(tt),Y(p.peak*ease(j/120)));}c.lineTo(X(p.up),Y(p.peak*p.fall));c.lineTo(X(p.up+p.hold),Y(p.peak*p.fall));for(let j=1;j<=160;j++){const u=j/160;c.lineTo(X(p.up+p.hold+p.down*u),Y(p.peak*p.fall*(1-ease(u))));}}
 c.stroke();
 if(!isRoll){c.fillStyle='#148776';for(let tt=0;tt<=p.total+1e-8;tt+=1/60){c.beginPath();c.arc(X(Math.min(tt,p.total)),Y(offset(tt,p)),2.5,0,Math.PI*2);c.fill();}}
 c.strokeStyle='#b86b12';c.lineWidth=1;c.setLineDash([5,4]);c.beginPath();c.moveTo(X(t),m.t);c.lineTo(X(t),h-m.b);c.stroke();c.setLineDash([]);c.fillStyle='#b86b12';c.beginPath();c.arc(X(t),Y(isRoll?roll(t,p):offset(t,p)),4,0,Math.PI*2);c.fill();
}
function render(){const p=params(),t=p.total*Number($('scrubTime').value)/1000;for(const id of ['peak','up','hold','down','rollA'])$(id+'Out').textContent=Number(p[id]).toFixed(id==='peak'||id==='rollA'?2:3)+(id==='peak'||id==='rollA'?'°':' s');$('fallOut').textContent=Math.round(p.fall*100)+'%';$('timeOut').textContent=t.toFixed(3)+' s';$('phaseText').textContent=phase(t,p);$('offsetText').textContent=offset(t,p).toFixed(3)+'°';const d=offset(t,p)-offset(Math.max(0,t-1/60),p);$('deltaText').textContent=(d>0?'+':'')+d.toFixed(3)+'°';draw($('recoilCanvas'),p,t,false);draw($('rollCanvas'),p,t,true);}
function stop(){playing=false;cancelAnimationFrame(raf);$('playBtn').textContent='播放一枪 · 4 倍慢速';}
function frame(now){if(!playing)return;if(lastFrame)elapsed+=(now-lastFrame)/1000/4;lastFrame=now;const p=params();$('scrubTime').value=String(Math.round(Math.min(1,elapsed/p.total)*1000));render();if(elapsed>=p.total){stop();return;}raf=requestAnimationFrame(frame);}
$('playBtn').addEventListener('click',()=>{if(playing){stop();return;}elapsed=0;lastFrame=0;$('scrubTime').value='0';playing=true;$('playBtn').textContent='暂停';render();raf=requestAnimationFrame(frame);});
$('resetBtn').addEventListener('click',()=>{stop();for(const [id,value]of Object.entries(defaults))$(id).value=String(value);$('scrubTime').value='0';render();});
for(const id of [...Object.keys(defaults),'scrubTime'])$(id).addEventListener('input',()=>{stop();render();});
$('printBtn').addEventListener('click',()=>window.print());
window.addEventListener('resize',render);render();
})();
