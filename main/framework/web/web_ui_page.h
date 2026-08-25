#pragma once

// 内嵌配置页（随固件编译进 WebConfigApi 的 GET / 响应）。
// 根据 /api/config 返回的 Schema 动态渲染表单，保存时 PUT /api/config。
static const char INDEX_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>无屏语音助手 · 配置</title>
<style>
  body{font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:#f4f5f7;margin:0;color:#222}
  header{background:#1f6feb;color:#fff;padding:14px 18px;font-size:18px;font-weight:600}
  .wrap{max-width:720px;margin:16px auto;padding:0 14px}
  .group{background:#fff;border:1px solid #e3e5e8;border-radius:8px;margin-bottom:14px;overflow:hidden}
  .group h2{margin:0;padding:10px 14px;background:#fafbfc;border-bottom:1px solid #eee;font-size:15px;color:#333}
  .row{display:flex;align-items:center;gap:12px;padding:12px 14px;border-bottom:1px solid #f2f3f5}
  .row:last-child{border-bottom:none}
  .row label{flex:0 0 84px;font-size:14px;color:#444}
  .row input[type=text],.row input[type=password],.row input[type=number],.row select,.row input[type=color]{flex:1;min-width:0;padding:6px 8px;border:1px solid #c9cdd3;border-radius:6px;font-size:14px}
  .row input[type=checkbox]{width:20px;height:20px}
  .actions{padding:16px 0;text-align:right}
  .lan{background:#fff7e6;border:1px solid #ffe0a3;border-radius:8px;padding:10px 14px;font-size:13px;color:#8a6d1a;margin-bottom:14px}
  button{background:#1f6feb;color:#fff;border:none;padding:10px 22px;border-radius:6px;font-size:15px;cursor:pointer}
  button.danger{background:#e5534b;margin-left:8px}
  button:disabled{opacity:.5}
  #toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#333;color:#fff;padding:10px 18px;border-radius:6px;display:none}
  .logbox{background:#0d1117;border:1px solid #e3e5e8;border-radius:8px;padding:8px;margin-bottom:14px;font-family:Consolas,Menlo,monospace;font-size:12px;max-height:340px;overflow-y:auto}
  .logbox .l{display:block;line-height:1.5;white-space:pre-wrap;word-break:break-all}
  .logbox .L-I{color:#7ee787}.logbox .L-W{color:#d29922}.logbox .L-E{color:#ff7b72}
  .logbox .tm{color:#8b949e;margin-right:8px}.logbox .tg{color:#79c0ff;margin-right:8px}
  .logbar{display:flex;align-items:center;justify-content:space-between;margin-bottom:6px}
</style>
</head>
<body>
<header>无屏语音助手 · 配置中心</header>
<div class="wrap">
  <div class="lan">仅限局域网访问：请确保手机/电脑与设备在同一网络。密码仅用于设备登录服务端，不会回显明文；接口地址需以 http:// 或 https:// 开头（https 需设备联网后自动校时）。</div>
  <div id="groups"></div>
  <div class="actions">
    <button id="save" onclick="saveConfig()">保存并生效</button>
  </div>
  <div class="group">
    <h2>设备日志（登录 / 请求 / 对话）</h2>
    <div class="logbar">
      <span style="font-size:12px;color:#666">自动刷新，最新在底部</span>
      <button class="danger" onclick="clearLogs()" style="padding:4px 10px;font-size:12px">清空</button>
    </div>
    <div class="logbox" id="logbox"></div>
  </div>
</div>
<div id="toast"></div>
<script>
const SCHEMA=[], VALUES={}, controls={};

async function loadConfig(){
  const r=await fetch('/api/config');
  const data=await r.json();
  SCHEMA.length=0; SCHEMA.push(...data.schema);
  Object.keys(VALUES).forEach(k=>delete VALUES[k]);
  Object.assign(VALUES,data.values);
  render();
}

function render(){
  const groups={};
  SCHEMA.forEach(item=>{ (groups[item.group]=groups[item.group]||[]).push(item); });
  const host=document.getElementById('groups');
  host.innerHTML='';
  Object.keys(groups).forEach(gname=>{
    const box=document.createElement('div'); box.className='group';
    const h=document.createElement('h2'); h.textContent=gname; box.appendChild(h);
    groups[gname].forEach(item=>{
      const row=document.createElement('div'); row.className='row';
      const lab=document.createElement('label'); lab.textContent=item.label;
      row.appendChild(lab);
      row.appendChild(controlFor(item));
      box.appendChild(row);
    });
    host.appendChild(box);
  });
}

function controlFor(item){
  const k=item.key, cur=VALUES[k]??'';
  let el;
  if(item.type==='enum'){
    el=document.createElement('select');
    (item.options||'').split('|').filter(o=>o).forEach(opt=>{
      const o=document.createElement('option'); o.value=opt; o.textContent=opt;
      if(opt===cur) o.selected=true;
      el.appendChild(o);
    });
  } else if(item.type==='bool'){
    el=document.createElement('input'); el.type='checkbox'; el.checked=(cur==='1'||cur==='true');
  } else if(item.type==='color'){
    el=document.createElement('input'); el.type='color'; el.value=cur||'#FFFFFF';
  } else if(item.type==='int'){
    el=document.createElement('input'); el.type='number'; el.step='1'; el.value=cur;
  } else if(item.type==='password'){
    el=document.createElement('input'); el.type='password'; el.autocomplete='new-password'; el.value=cur;
  } else {
    el=document.createElement('input'); el.type='text'; el.value=cur;
  }
  controls[k]=el;
  return el;
}

async function saveConfig(){
  const btn=document.getElementById('save'); btn.disabled=true;
  const payload={};
  for(const k in controls){
    const el=controls[k];
    if(el.type==='checkbox') payload[k]=el.checked?'1':'0';
    else if(el.type==='color') payload[k]=el.value;
    else payload[k]=el.value;
  }
  const r=await fetch('/api/config',{
    method:'PUT',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify(payload)
  });
  btn.disabled=false;
  const res=await r.json();
  toast(res.ok?('已保存 '+res.updated+' 项'):'保存失败');
  if(res.ok) loadConfig();
}

function toast(msg){
  const t=document.getElementById('toast');
  t.textContent=msg; t.style.display='block';
  setTimeout(()=>t.style.display='none',1800);
}

let lastLogCount=0;
function fmtTime(ms){
  const s=Math.floor(ms/1000);
  const h=String(Math.floor(s/3600)).padStart(2,'0');
  const m=String(Math.floor((s%3600)/60)).padStart(2,'0');
  const sec=String(s%60).padStart(2,'0');
  return h+':'+m+':'+sec;
}
async function refreshLogs(){
  try{
    const r=await fetch('/api/logs');
    const data=await r.json();
    const box=document.getElementById('logbox');
    const logs=data.logs||[];
    if(logs.length===0){ box.innerHTML='<span style="color:#8b949e">暂无日志</span>'; lastLogCount=0; return; }
    // 只在条数变化时重绘，避免滚动跳动
    if(logs.length!==lastLogCount){
      box.innerHTML=logs.map(e=>{
        const cls=e.l==='E'?'L-E':(e.l==='W'?'L-W':'L-I');
        const tag=(e.tag||'').replace(/[<>&]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]));
        const msg=(e.m||'').replace(/[<>&]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]));
        return '<span class="l '+cls+'"><span class="tm">'+fmtTime(e.t)+'</span><span class="tg">'+tag+'</span>'+msg+'</span>';
      }).join('');
      lastLogCount=logs.length;
      box.scrollTop=box.scrollHeight;
    }
  }catch(err){ /* 网络抖动忽略 */ }
}
async function clearLogs(){
  try{
    const r=await fetch('/api/logs',{method:'DELETE'});
    await r.json();
    lastLogCount=0;
    refreshLogs();
    toast('日志已清空');
  }catch(err){ toast('清空失败'); }
}
refreshLogs();
setInterval(refreshLogs,2000);
loadConfig();
</script>
</body>
</html>
)HTML";