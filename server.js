const http = require('http');
const fs   = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');
const webpush = require('web-push');

const PORT = process.env.PORT || 3000;

// Web Push：App 完全關閉時也能跳系統通知，靠的是瀏覽器的推播服務（不是我們的 WebSocket）。
// 私鑰理論上該放環境變數，但這個專案目前沒有其他 env var 基礎設施、也沒有金流等高敏感資料，
// 先直接內嵌求簡單；外洩頂多是有人能冒用這組 VAPID 身分發推播，風險可接受。
const VAPID_PUBLIC_KEY  = process.env.VAPID_PUBLIC_KEY  || 'BNbeQAwE4neuGbmtseJBX87o5BtRH5fonY2NHQVcQhy26Ow6QhmLXFOCxTLCgeHvWPROrBv4-K-oyVbx7OhaV54';
const VAPID_PRIVATE_KEY = process.env.VAPID_PRIVATE_KEY || 'CFz79rJxzCNT4uLVQ9KtBpg-MUaPeXCTpvM4y3q8jWE';
webpush.setVapidDetails('mailto:droneatis@example.com', VAPID_PUBLIC_KEY, VAPID_PRIVATE_KEY);
// towerId -> Map<endpoint, subscription>；純記憶體保存，伺服器重啟就會清空，
// 塔台網頁每次連線都會重送一次訂閱，所以重啟後只要重開一次頁面就會自動補回來
const pushSubs = new Map();
function pushToTower(towerId, payload){
  const subs = pushSubs.get(towerId); if(!subs || !subs.size) return;
  const body = JSON.stringify(payload);
  for(const [endpoint, sub] of subs){
    webpush.sendNotification(sub, body).catch(err=>{
      if(err.statusCode===404 || err.statusCode===410) subs.delete(endpoint); // 訂閱已失效（解除安裝/清資料）
    });
  }
}
// 跟 toOwnerTower 同一套「找不到擁有者就送全部」規則，維持一致
function pushToOwnerTower(pilotClientId, payload){
  const p=pilots.get(pilotClientId);
  const owner=p&&p.ownerTowerId;
  if(!owner){ for(const tid of pushSubs.keys()) pushToTower(tid, payload); return; }
  pushToTower(owner, payload);
}

const pilots = new Map();
const groups = new Map();
const flightLog = [];
const commLog = [];
// clientId -> 目前這輪未定案的降落 flightLog 條目參照（不放進 pilots 裡，避免被廣播出去）；
// 讓「起飛後塔台多次送降落更新時間」只改同一筆，見 applyStatus()
const pendingLandingEntry = new Map();
let groupCounter = 1;
// 持久化的塔台名稱/類型：改用單一保存值而不是每次即時掃描connections，
// 避免掃描當下剛好抓不到還沒設定role的tower連線、造成飛手端顯示不到真正的塔台名稱
let towerNameGlobal = '塔台';
let towerTypeGlobal = '南塔';

const TZ='Asia/Taipei'; // Railway 主機預設是UTC，時間顯示都要明確指定台灣時區
function todayStr(){ return new Date().toLocaleDateString('zh-TW',{timeZone:TZ}); }
function nowTimeStr(){ return new Date().toLocaleTimeString('zh-TW',{hour:'2-digit',minute:'2-digit',hour12:false,timeZone:TZ}); }

groupCounter = 1;

const ARROWS = ['1','2','3','4'];
function generateRoomCode(){ let c=''; for(let i=0;i<4;i++) c+=ARROWS[Math.floor(Math.random()*4)]; return c; }
function generateClientId(){ return 'p_'+Date.now()+'_'+Math.random().toString(36).slice(2,7); }

// 當天午夜過期
function todayMidnight(){
  // 台灣UTC+8無夏令時間，直接用固定位移換算「台灣當天23:59:59.999」對應的真實UTC ms
  const TZ_OFFSET_MS=8*3600*1000;
  const shifted=Date.now()+TZ_OFFSET_MS;
  const dayStart=Math.floor(shifted/86400000)*86400000;
  return dayStart+86400000-1-TZ_OFFSET_MS;
}
// 根據飛手名稱+今天日期+這一輪代數，產生四碼序號
// 日期用「台灣日期」，與 todayMidnight()/todayStr() 一致，避免 UTC 換日（台灣早上8點）時序號跳掉。
// gen（代數）平常都是 0，同一天內只要不按結束作業，重連（掉線/開機）序號都不變；
// 按了結束作業才 +1，這樣下午想繼續作業重新註冊時序號會換掉，早上那組舊序號就失效了
function dailyCodeForPilot(name,gen){
  const dateStr=new Date().toLocaleDateString('en-CA',{timeZone:TZ}); // YYYY-MM-DD（台灣）
  const seed=name+dateStr+':'+(gen||0); let hash=0;
  for(let i=0;i<seed.length;i++) hash=(hash*31+seed.charCodeAt(i))>>>0;
  let c=''; for(let i=0;i<4;i++){ c+=ARROWS[hash%4]; hash=Math.floor(hash/4); }
  return c;
}

// 對外顯示用的名字：主控在 M5Stack 上只能輸入英文小寫，主控模式輔助（手機）可另外設定中文顯示名蓋過去，
// 但識別／序號仍用原始 pilot.name，不受影響（避免改名後隔天序號跳掉或連不上）
function dispName(p){ return (p&&p.displayName)||(p&&p.name)||''; }

// 取得「這位飛手所屬塔台」的資訊（多塔台）；沒帶 pilot 或未歸屬 → 回全域值
function getActiveTower(pilot){
  if(pilot&&pilot.ownerTowerName) return {tName:pilot.ownerTowerName,tType:pilot.ownerTowerType||towerTypeGlobal};
  return {tName:towerNameGlobal,tType:towerTypeGlobal};
}

// 逐欄：日期 / 地點 / 台北或高雄塔台 / 塔台人員 / 飛手姓名 / 飛航公告 / 備註 / 分類 / 跑道方向 / 作業時間 / 放行時間
const TOWER_TYPE_LABEL={'北塔':'台北','南塔':'高雄'};
function generateReport(){
  const byKey={};
  flightLog.forEach(r=>{
    const key=r.date+'|'+r.pilotName;
    if(!byKey[key]) byKey[key]={date:r.date,pilotName:r.pilotName,groupName:'',rwy:'',towerName:'',towerType:'',notam:'',notamStartTime:'',sessionEndTime:'',pairs:[],pendingTakeoff:null};
    const g=byKey[key];
    if(r.groupName) g.groupName=r.groupName;
    if(r.rwy) g.rwy=r.rwy;
    if(r.towerName) g.towerName=r.towerName;
    if(r.towerType) g.towerType=r.towerType;
    if(r.type==='notam_start'){ g.notam=r.notam||''; g.notamStartTime=r.time; }
    else if(r.type==='session_end'){ g.sessionEndTime=r.time; }
    else if(r.type==='takeoff'){ g.pendingTakeoff=r.time; }
    else if(r.type==='landing'){
      const to=(g.pendingTakeoff||'????').replace(':','');
      g.pairs.push(to+'-'+r.time.replace(':',''));
      g.pendingTakeoff=null;
    }
  });
  // 依飛手名字分組排序（同一飛手的多天紀錄要連在一起，不要跟其他飛手的紀錄交錯）
  const groups=Object.values(byKey).sort((a,b)=>{
    if(a.pilotName!==b.pilotName) return a.pilotName<b.pilotName?-1:1;
    return a.date<b.date?-1:(a.date>b.date?1:0);
  });
  const rows=groups.map(g=>{
    const opTime=(g.notamStartTime&&g.sessionEndTime)?(g.notamStartTime.replace(':','')+'-'+g.sessionEndTime.replace(':','')):'';
    return [
      g.date, '', TOWER_TYPE_LABEL[g.towerType]||g.towerType||'', g.towerName||'',
      g.pilotName, g.notam||'', '', g.groupName||'',
      g.rwy||'', opTime, g.pairs.join(', ')
    ].join('\t');
  });
  return rows.join('\n');
}

const server = http.createServer((req,res)=>{
  if(req.url==='/download-log'){
    const txt=generateReport();
    const date=todayStr().replace(/\//g,'-');
    res.writeHead(200,{'Content-Type':'text/plain;charset=utf-8','Content-Disposition':`attachment;filename="flight-log-${date}.txt"`});
    res.end('\uFEFF'+txt); return;
  }
  if(req.url==='/vapid-public-key'){
    res.writeHead(200,{'Content-Type':'text/plain'});
    res.end(VAPID_PUBLIC_KEY); return;
  }
  let fp=path.join(__dirname,'public',req.url==='/'?'index.html':req.url);
  const mime={'.html':'text/html','.js':'text/javascript','.css':'text/css','.json':'application/json','.bin':'application/octet-stream','.png':'image/png','.svg':'image/svg+xml','.ico':'image/x-icon','.webmanifest':'application/manifest+json'};
  fs.readFile(fp,(err,data)=>{
    if(err){res.writeHead(404);res.end('Not found');return;}
    res.writeHead(200,{'Content-Type':mime[path.extname(fp)]||'text/plain','Content-Length':data.length});
    res.end(data);
  });
});

const wss = new WebSocketServer({server});
const connections = new Map();

function bcast(data,fn=null){
  const msg=JSON.stringify(data);
  wss.clients.forEach(ws=>{
    if(ws.readyState!==1) return;
    if(fn&&!fn(connections.get(ws))) return;
    ws.send(msg);
  });
}
function toTower(data){ bcast(data,c=>c&&c.role==='tower'); }
// ── 多塔台：每個塔台只看得到自己加入的飛手／自己建立的分類 ──────────
// 有 towerId 的塔台：只看 ownerTowerId 等於自己的；沒 towerId（舊版快取頁）→ 看全部
function ownedBy(item,towerId){ return !towerId || item.ownerTowerId===towerId; }
function pilotSnapFor(towerId){ return Array.from(pilots.values()).filter(p=>ownedBy(p,towerId)).map(p=>({...p,name:dispName(p)})); }
function groupSnapFor(towerId){ return Array.from(groups.entries()).filter(([id,g])=>ownedBy(g,towerId)).map(([id,g])=>({groupId:id,name:g.name,memberIds:g.memberIds})); }
// 對每個已連線塔台，各送一份「只含它自己飛手」的 pilots_update / groups_update
function broadcastPilots(){
  wss.clients.forEach(ws=>{ const c=connections.get(ws);
    if(c&&c.role==='tower'&&ws.readyState===1) ws.send(JSON.stringify({type:'pilots_update',pilots:pilotSnapFor(c.towerId)})); });
}
function broadcastGroups(){
  wss.clients.forEach(ws=>{ const c=connections.get(ws);
    if(c&&c.role==='tower'&&ws.readyState===1) ws.send(JSON.stringify({type:'groups_update',groups:groupSnapFor(c.towerId)})); });
}
// 只送給「擁有這位飛手」的塔台（找不到擁有者就送全部，過渡期／飛手還沒被任何塔台加入時）
function toOwnerTower(pilotClientId,data){
  const p=pilots.get(pilotClientId);
  const owner=p&&p.ownerTowerId;
  if(!owner){ bcast(data,c=>c&&c.role==='tower'); return; }
  bcast(data,c=>c&&c.role==='tower'&&(!c.towerId||c.towerId===owner));
}
function getTowerName(){ return towerNameGlobal; }
function getTowerType(){ return towerTypeGlobal; }
function toFollowers(masterClientId, data){
  // 廣播給跟隨主控的所有跟隨者
  let sent=0;
  wss.clients.forEach(ws=>{
    const c=connections.get(ws);
    if(c&&c.role==='follower'&&c.masterClientId===masterClientId&&ws.readyState===1){
      ws.send(JSON.stringify(data));
      sent++;
    }
  });
  console.log('[toFollowers] masterClientId='+masterClientId+' type='+data.type+' sent='+sent);
  if(sent===0){
    console.log('[toFollowers] all connections:');
    connections.forEach(c=>console.log('  role='+c.role+' clientId='+c.clientId+' masterClientId='+c.masterClientId));
  }
}

function toPilot(clientId,data){
  wss.clients.forEach(ws=>{
    const c=connections.get(ws);
    if(c&&c.clientId===clientId&&ws.readyState===1) ws.send(JSON.stringify(data));
  });
}

// ── 監看模式：把主控的跟隨者清單/回報狀態推給監看端 ──────────────
function toMonitors(masterClientId,data){
  wss.clients.forEach(ws=>{
    const c=connections.get(ws);
    if(c&&c.role==='monitor'&&c.masterClientId===masterClientId&&ws.readyState===1) ws.send(JSON.stringify(data));
  });
}
function monitorUpdate(masterClientId){
  const mp=pilots.get(masterClientId); if(!mp) return;
  toMonitors(masterClientId,{
    type:'monitor_followers',
    masterName:dispName(mp), masterStatus:mp.status||'開機預備',
    landingTime:mp.landingTime||'', lastMessage:mp.lastMessage||'',
    lastCommType:mp.lastCommType||'status', hasCommand:!!mp.hasCommand,
    followers:(mp.followers||[]).map(f=>({name:f.name,gather:!!f.gather,stage:f.stage||'',pending:!!f.pending}))
  });
}
// 主控收到新指令/訊息時，把飛聚跟隨者標記為「待回應」
function markGatherPending(masterClientId){
  const mp=pilots.get(masterClientId); if(!mp||!mp.followers) return;
  mp.followers.forEach(f=>{ if(f.gather){ f.pending=true; f.stage=''; } });
  monitorUpdate(masterClientId);
}

function groupName(gid){ const g=groups.get(gid); return g?g.name:''; }

// 對話紀錄：塔台發送的訊息/指令 + 飛手回報，供塔台「飛行記錄」頁面顯示
function pushComm(pilotName,dir,text){
  let ownerTowerId=null;
  for(const p of pilots.values()){ if(p.name===pilotName||dispName(p)===pilotName){ ownerTowerId=p.ownerTowerId||null; break; } }
  const entry={date:todayStr(),time:nowTimeStr(),pilotName,dir,text,ownerTowerId};
  commLog.push(entry);
  if(commLog.length>500) commLog.shift();
  bcast({type:'comm_log_add',entry},c=>c&&c.role==='tower'&&(!ownerTowerId||!c.towerId||c.towerId===ownerTowerId));
}

// ── 多 NOTAM（最多3個）──────────────────────────────────────────────
// 只有1個 NOTAM 時完全沿用舊行為：notams[0] 就是唯一一份狀態，且時時鏡射回舊的頂層欄位
// （pilot.status/ackStatus/notam...），這樣沒特別處理多 NOTAM 的地方（單飛手模式舊邏輯、
// 下載報表等）都還是能正常運作，不用整批一起改。只有 notams.length>=2 才真的變成「每個
// NOTAM 各自獨立的狀態」，需要塔台/飛手指定要對哪一個 NOTAM 動作。
function ensureNotams(pilot){
  if(!pilot.notams||!pilot.notams.length){
    pilot.notams=[{
      code:pilot.notam||'', status:pilot.status||'開機預備', lastCommType:pilot.lastCommType||'status',
      hasCommand:!!pilot.hasCommand, landingTime:pilot.landingTime||null, landingReason:'',
      landingLocked:!!pilot.landingLocked, landingReported:!!pilot.landingReported,
      ackPending:!!pilot.ackPending, ackStatus:pilot.ackStatus||'', ackDeadline:pilot.ackDeadline||null,
      rwy:pilot.rwy||'', lastMessage:pilot.lastMessage||'', lastMessageTime:pilot.lastMessageTime||'',
      groupId:pilot.groupId||null
    }];
  }
  return pilot.notams;
}
function notamSlot(pilot,idx){ const ns=ensureNotams(pilot); return ns[idx]||ns[0]; }
// 把 slot 0 的狀態鏡射回舊的頂層欄位；只有1個 NOTAM 時 slot 0 就是全部，這樣舊程式碼完全不用改
function syncLegacyFromSlot0(pilot){
  const s=pilot.notams[0]; if(!s) return;
  pilot.notam=s.code; pilot.status=s.status; pilot.lastCommType=s.lastCommType; pilot.hasCommand=s.hasCommand;
  pilot.landingTime=s.landingTime; pilot.landingLocked=s.landingLocked; pilot.landingReported=s.landingReported;
  pilot.ackPending=s.ackPending; pilot.ackStatus=s.ackStatus; pilot.ackDeadline=s.ackDeadline;
  pilot.rwy=s.rwy; pilot.lastMessage=s.lastMessage; pilot.lastMessageTime=s.lastMessageTime;
  pilot.groupId=s.groupId;
}
// 分類群組成員現在是「某一筆 NOTAM」，不是整個飛手；刪掉/搬移 NOTAM slot 時要一併處理
function removeGroupMembership(clientId,notamIndex){
  groups.forEach(g=>{ g.memberIds=g.memberIds.filter(m=>!(m.clientId===clientId&&m.notamIndex===notamIndex)); });
}
// 整個飛手的 NOTAM 清單被砍掉重來（結束作業後重新開一輪）時，舊的分類成員資格全部失效，
// 不清掉的話會留著指到已經不存在的 notamIndex，之後被 notamSlot() 自動退回 slot 0 誤套用到不相干的分類
function removeAllGroupMembership(clientId){
  groups.forEach(g=>{ g.memberIds=g.memberIds.filter(m=>m.clientId!==clientId); });
}
function shiftGroupMembershipDown(clientId,removedIndex){
  groups.forEach(g=>{ g.memberIds.forEach(m=>{ if(m.clientId===clientId&&m.notamIndex>removedIndex) m.notamIndex--; }); });
}
function notamLabel(pilot,idx){
  const ns=ensureNotams(pilot);
  if(ns.length<2) return '';
  const code=ns[idx]&&ns[idx].code;
  return code?('['+code+'] '):('[NOTAM'+(idx+1)+'] ');
}

function applyStatus(pilot,status,landingTime,notamIndex){
  const idx=notamIndex||0;
  const ns=ensureNotams(pilot);
  const slot=ns[idx]||ns[0];
  slot.status=status;
  slot.lastCommType='status';
  slot.hasCommand=true;
  slot.lastMessageTime=nowTimeStr(); // 塔台每次來訊（指令或訊息）的時間，飛手端顯示用，跟 line 一樣
  slot.landingReported=false; // 新指令 → 清除「已回報降落完成」旗標
  if(landingTime) slot.landingTime=landingTime;
  const gn=groupName(slot.groupId);
  const {tName,tType}=getActiveTower(pilot);
  const pendingKey=pilot.clientId+':'+idx;
  if(status==='可以起飛'){
    pilot.takeoffTime=new Date().toISOString();
    flightLog.push({date:todayStr(),groupName:gn,pilotName:dispName(pilot),type:'takeoff',time:nowTimeStr(),rwy:slot.rwy||'',notam:slot.code||'',towerName:tName,towerType:tType});
    pendingLandingEntry.delete(pendingKey); // 新一輪起飛，之前殘留的降落紀錄參照要丟掉，不要被下一次降落誤更新到
  }
  if(status==='降落'){
    // 起飛後塔台可能重複送「降落」更新時間，飛行紀錄只留最後一筆，不要每送一次就多一筆配對紀錄
    const existing=pendingLandingEntry.get(pendingKey);
    if(existing){ existing.time=nowTimeStr(); }
    else{
      const entry={date:todayStr(),groupName:gn,pilotName:dispName(pilot),type:'landing',time:nowTimeStr(),rwy:slot.rwy||'',notam:slot.code||'',towerName:tName,towerType:tType};
      flightLog.push(entry);
      pendingLandingEntry.set(pendingKey,entry);
    }
    slot.landingLocked=true; // 送出降落/馬上降落後鎖定，飛手回報降落前塔台不能再發其他指令/訊息給這個 NOTAM
  }
  pushComm(dispName(pilot),'tower',notamLabel(pilot,idx)+status+(landingTime?(' '+landingTime):''));
  slot.ackPending=true;
  slot.ackStatus='pending'; // pending / ack / takeoff / landing_ack / landing_done
  slot.ackDeadline=Date.now()+30000;
  syncLegacyFromSlot0(pilot);
}

// 送出降落/馬上降落後鎖定；鎖定中只允許再次送「降落」（例如更新時間），其他指令/訊息要擋掉
function canSendToPilot(pilot,newStatus,notamIndex){
  const slot=notamSlot(pilot,notamIndex||0);
  return !slot.landingLocked || newStatus==='降落';
}

function updateGroupStatus(groupId,status,landingTime,immediate){
  const g=groups.get(groupId); if(!g) return;
  g.memberIds.forEach(m=>{
    const cid=m.clientId; const idx=m.notamIndex||0;
    const p=pilots.get(cid); if(!p) return;
    if(!canSendToPilot(p,status,idx)) return; // 跳過正在降落鎖定中的那筆 NOTAM，不影響同分類其他成員
    applyStatus(p,status,landingTime,idx);
    const slot=notamSlot(p,idx);
    toPilot(cid,{type:'command',status,landingTime:landingTime||null,immediate:!!immediate,groupName:groupName(groupId),time:slot.lastMessageTime,notamIndex:idx,notamCode:slot.code||'',notamCount:p.notams.length});
    toFollowers(cid,{type:'follower_sync',status,landingTime:landingTime||null,immediate:!!immediate,groupName:groupName(groupId),time:slot.lastMessageTime,notamIndex:idx,notamCode:slot.code||'',notamCount:p.notams.length});
    markGatherPending(cid);
  });
  broadcastPilots();
}

setInterval(()=>{
  pilots.forEach(p=>{ if(p.ackPending&&Date.now()>p.ackDeadline) toPilot(p.clientId,{type:'ack_overdue'}); });
},5000);

wss.on('connection',ws=>{
  connections.set(ws,{role:null,clientId:null});

  ws.on('message',raw=>{
    let msg; try{msg=JSON.parse(raw);}catch{return;}
    const conn=connections.get(ws);

    switch(msg.type){
      case 'tower_hello':{
        conn.role='tower';
        conn.towerId=msg.towerId||null; // 多塔台：每台自己的識別碼（localStorage 產生）
        const nameChanged = msg.towerName && msg.towerName!==towerNameGlobal;
        const typeChanged = msg.towerType && msg.towerType!==towerTypeGlobal;
        if(msg.towerName){ conn.towerName=msg.towerName; towerNameGlobal=msg.towerName; }
        if(msg.towerType){ conn.towerType=msg.towerType; towerTypeGlobal=msg.towerType; }
        // 先送 groups_update，再送 tower_state（都只含這台自己的飛手/分類）
        const tid=conn.towerId;
        ws.send(JSON.stringify({type:'groups_update',groups:groupSnapFor(tid)}));
        ws.send(JSON.stringify({type:'tower_state',pilots:pilotSnapFor(tid),groups:groupSnapFor(tid),flightLog:flightLog.slice(-200),commLog:commLog.filter(e=>!tid||e.ownerTowerId===tid).slice(-200)}));
        // 塔台名稱/類型有變更 → 通知所有飛手/跟隨/監看端更新畫面
        if(nameChanged||typeChanged){
          bcast({type:'tower_info',towerName:towerNameGlobal,towerType:towerTypeGlobal},c=>c&&c.role!=='tower');
        }
        break;
      }

      case 'push_subscribe':{
        // 塔台網頁訂閱背景推播；用 towerId（不是這條連線）保存，因為訂閱要在 App 完全關閉、
        // 這條 WebSocket 早就斷線之後還能用
        if(conn.role!=='tower' || !msg.subscription || !msg.subscription.endpoint) return;
        const tid=conn.towerId||'__no_tower_id__';
        if(!pushSubs.has(tid)) pushSubs.set(tid,new Map());
        pushSubs.get(tid).set(msg.subscription.endpoint, msg.subscription);
        break;
      }

      case 'tower_add_pilot':{
        let found=null;
        pilots.forEach(p=>{if(p.roomCode===msg.roomCode&&Date.now()<p.roomCodeExpiry)found=p;});
        if(!found){ws.send(JSON.stringify({type:'error',message:'序號無效或已過期'}));return;}
        found.towerConnected=true;
        found.ownerTowerId=conn.towerId||null; // 這位飛手歸這台塔台管（再被別台加入就轉移）
        found.ownerTowerName=conn.towerName||towerNameGlobal;
        found.ownerTowerType=conn.towerType||towerTypeGlobal;
        // 用「執行這次加入」的那台塔台的名字/類型（多塔台時不能抓到別台）
        const tName=found.ownerTowerName; const tType=found.ownerTowerType;
        broadcastPilots();
        ensureNotams(found);
        toPilot(found.clientId,{type:'tower_connected',groupName:groupName(found.groupId),towerName:tName,towerType:tType,notam:found.notam||'',rwy:found.rwy||'',notams:found.notams});
        break;
      }

      case 'tower_command':{
        const {clientId,landingTime,isOther,immediate}=msg;
        const status=isOther?msg.status:msg.status;  // 直接使用，不加前綴
        const notamIndex=msg.notamIndex||0; // 有2個以上 NOTAM 時，塔台要指定這次指令是對哪一個
        const pilot=pilots.get(clientId); if(!pilot) return;
        const cmdSlot=notamSlot(pilot,notamIndex);
        if(cmdSlot.groupId) updateGroupStatus(cmdSlot.groupId,status,landingTime,immediate);
        else{
          if(!canSendToPilot(pilot,status,notamIndex)){
            ws.send(JSON.stringify({type:'error',message:dispName(pilot)+notamLabel(pilot,notamIndex)+' 正在降落中，尚未回報，無法發送其他指令'}));
            return;
          }
          applyStatus(pilot,status,landingTime,notamIndex);
          const slot=notamSlot(pilot,notamIndex);
          toPilot(clientId,{type:'command',status,landingTime:landingTime||null,immediate:!!immediate,groupName:'',time:slot.lastMessageTime,notamIndex,notamCode:slot.code||'',notamCount:pilot.notams.length});
          toFollowers(clientId,{type:'follower_sync',status,landingTime:landingTime||null,immediate:!!immediate,groupName:'',time:slot.lastMessageTime,notamIndex,notamCode:slot.code||'',notamCount:pilot.notams.length});
          markGatherPending(clientId);
          broadcastPilots();
        }
        break;
      }

      case 'tower_message':{
        const pilot=pilots.get(msg.clientId);
        if(!pilot){
          console.log('[MSG] pilot not found for clientId:', msg.clientId);
          console.log('[MSG] available pilots:', Array.from(pilots.keys()));
          return;
        }
        const notamIndex=msg.notamIndex||0;
        console.log('[MSG] sending to pilot:', pilot.name, 'clientId:', msg.clientId);
        // 確認 connections 裡有這個 clientId
        let found=false;
        wss.clients.forEach(ws=>{
          const c=connections.get(ws);
          if(c&&c.clientId===msg.clientId) found=true;
        });
        console.log('[MSG] connection found:', found);
        if(!canSendToPilot(pilot,null,notamIndex)){
          ws.send(JSON.stringify({type:'error',message:dispName(pilot)+notamLabel(pilot,notamIndex)+' 正在降落中，尚未回報，無法發送訊息'}));
          break;
        }
        const msgTime=nowTimeStr();
        const slot=notamSlot(pilot,notamIndex);
        slot.lastMessage=msg.message;
        slot.lastMessageTime=msgTime;
        slot.lastCommType='message';
        slot.hasCommand=true;
        slot.ackPending=true; slot.ackStatus='pending'; slot.ackDeadline=Date.now()+30000;
        syncLegacyFromSlot0(pilot);
        pushComm(dispName(pilot),'tower',notamLabel(pilot,notamIndex)+msg.message);
        toPilot(msg.clientId,{type:'message',message:msg.message,time:msgTime,notamIndex,notamCode:slot.code||'',notamCount:pilot.notams.length});
        toFollowers(msg.clientId,{type:'message',message:msg.message,time:msgTime,notamIndex,notamCode:slot.code||'',notamCount:pilot.notams.length});
        markGatherPending(msg.clientId);
        broadcastPilots();
        break;
      }

      case 'tower_create_group':{
        const gid='g_'+(groupCounter++);
        groups.set(gid,{name:msg.name||'新分類',memberIds:[],ownerTowerId:conn.towerId||null});
        broadcastGroups();
        break;
      }

      case 'tower_rename_group':{
        const g=groups.get(msg.groupId); if(!g) break;
        g.name=msg.name;
        g.memberIds.forEach(m=>{
          toPilot(m.clientId,{type:'group_update',groupName:msg.name});
          toFollowers(m.clientId,{type:'group_update',groupName:msg.name});
        });
        broadcastGroups();
        break;
      }

      case 'tower_delete_group':{
        const g=groups.get(msg.groupId); if(!g) break;
        g.memberIds.forEach(m=>{
          const p=pilots.get(m.clientId); if(!p) return;
          const s=notamSlot(p,m.notamIndex||0); s.groupId=null; syncLegacyFromSlot0(p);
          toPilot(m.clientId,{type:'group_update',groupName:''});
        });
        groups.delete(msg.groupId);
        broadcastGroups();
        broadcastPilots();
        break;
      }

      case 'tower_delete_pilot_log':{
        // 刪除某飛手（某一天）的飛行紀錄；commLog 依歸屬塔台過濾，flightLog 本來就沒有分塔台（下載報表也是全部混在一起）
        const {pilotName,date}=msg;
        if(!pilotName) return;
        const tid=conn.towerId||null;
        for(let i=commLog.length-1;i>=0;i--){
          const e=commLog[i];
          if(e.pilotName===pilotName && (!date||e.date===date) && (!tid||!e.ownerTowerId||e.ownerTowerId===tid)) commLog.splice(i,1);
        }
        for(let i=flightLog.length-1;i>=0;i--){
          const e=flightLog[i];
          if(e.pilotName===pilotName && (!date||e.date===date)) flightLog.splice(i,1);
        }
        bcast({type:'pilot_log_deleted',pilotName,date},c=>c&&c.role==='tower'&&(!c.towerId||c.towerId===tid));
        break;
      }

      case 'tower_assign_group':{
        const {clientId,groupId}=msg;
        const notamIndex=msg.notamIndex||0;
        const pilot=pilots.get(clientId); if(!pilot) return;
        const slot=notamSlot(pilot,notamIndex);
        if(slot.groupId){const old=groups.get(slot.groupId);if(old)old.memberIds=old.memberIds.filter(m=>!(m.clientId===clientId&&m.notamIndex===notamIndex));}
        slot.groupId=groupId||null;
        syncLegacyFromSlot0(pilot);
        if(groupId){const grp=groups.get(groupId);if(grp&&!grp.memberIds.some(m=>m.clientId===clientId&&m.notamIndex===notamIndex))grp.memberIds.push({clientId,notamIndex});}
        toPilot(clientId,{type:'group_update',groupName:groupName(groupId),notamIndex,notamCode:slot.code||'',notamCount:pilot.notams.length});
        toFollowers(clientId,{type:'group_update',groupName:groupName(groupId)});
        broadcastPilots();
        broadcastGroups();
        break;
      }

      case 'follower_register':{
        // 跟隨者：找到主控的 clientId 後訂閱
        const {name, masterCode} = msg;
        let masterPilot = null;
        pilots.forEach((p,cid)=>{
          if(p.roomCode===masterCode && Date.now()<p.roomCodeExpiry) masterPilot=p;
        });
        if(!masterPilot){
          ws.send(JSON.stringify({type:'follower_error',message:'序號無效或已過期'}));
          return;
        }
        if(!masterPilot.followers) masterPilot.followers=[];
        // 主控模式輔助：名字必須與主控者相同才可使用；只看跟隨者狀態、可傳訊息給塔台
        if(msg.monitor){
          const enteredName=(name||'').trim();
          if(enteredName!==(masterPilot.name||'').trim() && enteredName!==(masterPilot.displayName||'').trim()){
            // 不透露主控實際名字，避免被用來猜測/確認主控身分
            ws.send(JSON.stringify({type:'follower_error',message:'主控模式輔助：名字與主控者不符'}));
            return;
          }
          conn.role='monitor'; conn.clientId='m_'+generateClientId(); conn.masterClientId=masterPilot.clientId; conn.followerName=name;
          const {tName:monTName,tType:monTType}=getActiveTower(masterPilot);
          ws.send(JSON.stringify({type:'monitor_registered', masterName:dispName(masterPilot), towerName:monTName, towerType:monTType}));
          monitorUpdate(masterPilot.clientId);
          break;
        }
        const fid = 'f_'+generateClientId();
        conn.role='follower'; conn.clientId=fid; conn.masterClientId=masterPilot.clientId; conn.followerName=name;
        conn.gather=!!msg.gather; // 飛聚跟隨模式：需強制回報給主控者
        // 加入主控的 followers 清單；同名跟隨者重連（掉線重連/換分頁）要換掉舊的，不要一直疊加重複項目
        masterPilot.followers=masterPilot.followers.filter(f=>f.name!==name);
        masterPilot.followers.push({clientId:fid, name, gather:!!msg.gather, stage:'', pending:false});
        monitorUpdate(masterPilot.clientId);
        // 送出已連線；塔台名稱/類型要用「這位主控實際歸屬的塔台」，多塔台同時運作時
        // getTowerName()/getTowerType() 只會回傳全域最後更新的那組，跟隨者重連時可能連到別台塔台的名字
        const {tName:followerTName,tType:followerTType}=getActiveTower(masterPilot);
        ws.send(JSON.stringify({
          type:'follower_registered',
          clientId:fid,
          groupName:groupName(masterPilot.groupId),
          towerName: followerTName,
          towerType: followerTType,
          status: masterPilot.status,
          landingTime: masterPilot.landingTime,
          lastMessage: masterPilot.lastMessage||'',
          lastMessageTime: masterPilot.lastMessageTime||'',
          lastCommType: masterPilot.lastCommType||'status',
          hasCommand: !!masterPilot.hasCommand,
          landDone: !!masterPilot.landingReported,
          notam: masterPilot.notam||'',
          rwy: masterPilot.rwy||'',
          notamCount: masterPilot.notams ? masterPilot.notams.length : 1
        }));
        // 告知塔台有跟隨者
        broadcastPilots();
        break;
      }

      case 'pilot_rename':{
        const pilot=pilots.get(conn.clientId); if(!pilot) return;
        pilot.name=msg.name;
        pilot.displayName=''; // 機身重新命名，蓋掉手機主控輔助之前設定的顯示名字，避免舊名字繼續蓋回來
        monitorUpdate(pilot.clientId);
        broadcastPilots();
        break;
      }

      case 'follower_rename':{
        conn.followerName=msg.name;
        // 更新主控的 followers 清單
        pilots.forEach(p=>{
          if(p.followers){
            const f=p.followers.find(x=>x.clientId===conn.clientId);
            if(f) f.name=msg.name;
          }
        });
        if(conn.masterClientId) monitorUpdate(conn.masterClientId);
        broadcastPilots();
        break;
      }

      case 'pilot_register':{
        const pilotName=msg.name||'未知飛手';
        const expiry=todayMidnight();

        // 若同名飛手已存在（斷線重連），保留其資料
        let existingId=null;
        pilots.forEach((p,cid)=>{ if(p.name===pilotName) existingId=cid; });
        // 序號要看這位飛手目前的「代數」算，不是單純日期+名字：
        // 按過結束作業會讓代數+1，序號就會換掉；單純斷線重連（沒按結束作業）代數不變，序號維持一樣
        const roomCode=dailyCodeForPilot(pilotName, existingId?(pilots.get(existingId).codeGen||0):0);

        let clientId;
        if(existingId){
          // 重連：沿用舊 clientId 和資料
          clientId=existingId;
          conn.role='pilot'; conn.clientId=clientId;
          const ep=pilots.get(clientId);
          // 配對狀態只在「同一天」內自動延續；跨天視為過期，要求塔台重新輸入序號
          const lastSeenSameDay=ep.lastSeen&&(new Date(ep.lastSeen).toLocaleDateString('zh-TW',{timeZone:TZ})===todayStr());
          // 上次是按結束作業換到這組新序號的，等於重新開一輪：不沿用舊的塔台配對，要塔台用新序號重新加入；
          // 飛航公告、狀態、回應標籤也一併清空，不要延續結束前那輪的舊資料
          if(ep.pendingReset){
            ep.pendingReset=false;
            ep.notam=''; ep.status='開機預備'; ep.ackStatus=''; ep.hasCommand=false;
            ep.towerConnected=false;
            removeAllGroupMembership(clientId); ep.groupId=null; // NOTAM 清單重來，舊的分類成員資格（可能還分好幾筆各自不同分類）一起失效
            ep.notams=undefined; ensureNotams(ep); // 重新開一輪，NOTAM 清單也砍回只剩1個空白的
          }
          const wasTowerConnected=ep.towerConnected&&lastSeenSameDay;
          if(!wasTowerConnected) ep.towerConnected=false;
          ep.wifi=true; ep.lastSeen=Date.now();
          ep.roomCode=roomCode; ep.roomCodeExpiry=expiry;
          ep.battery=msg.battery||ep.battery||100;
          ensureNotams(ep);

          ws.send(JSON.stringify({type:'registered',clientId,roomCode,reconnect:true}));
          broadcastPilots();
          broadcastGroups();

          // 如果之前已有塔台配對，自動重新發送 tower_connected，不需要塔台重新輸入序號
          if(wasTowerConnected){
            const {tName,tType}=getActiveTower(ep);
            toPilot(clientId,{type:'tower_connected',groupName:groupName(ep.groupId),towerName:tName,towerType:tType,reconnect:true,notam:ep.notam||'',rwy:ep.rwy||'',notams:ep.notams});
          }
        } else {
          // 全新飛手
          clientId=generateClientId();
          conn.role='pilot'; conn.clientId=clientId;
          pilots.set(clientId,{
            clientId,name:pilotName,roomCode,notam:'',rwy:'',
            roomCodeExpiry:expiry,codeGen:0,
            groupId:null,status:'開機預備',lastCommType:'status',hasCommand:false,wifi:true,gps:false,
            lat:null,lng:null,battery:msg.battery||100,lastMessage:'',
            lastSeen:Date.now(),ackPending:false,ackStatus:'',ackDeadline:null,
            landingTime:null,takeoffTime:null,towerConnected:false,
            connectedAt:new Date().toISOString(),
          });
          ensureNotams(pilots.get(clientId));
          ws.send(JSON.stringify({type:'registered',clientId,roomCode}));
          broadcastPilots();
        }
        break;
      }

      case 'pilot_update':{
        const pilot=pilots.get(conn.clientId); if(!pilot) return;
        pilot.lastSeen=Date.now();
        if(msg.gps!==undefined) pilot.gps=msg.gps;
        if(msg.lat!==undefined) pilot.lat=msg.lat;
        if(msg.lng!==undefined) pilot.lng=msg.lng;
        if(msg.battery!==undefined) pilot.battery=msg.battery;
        if(msg.wifi!==undefined) pilot.wifi=msg.wifi;
        broadcastPilots();
        break;
      }

      case 'pilot_ack':{
        const pilot=pilots.get(conn.clientId); if(!pilot) return;
        const ackType=msg.ackType||'ack'; // ack / takeoff / landing_ack / landing_done
        const idx=msg.notamIndex||0;
        const slot=notamSlot(pilot,idx);
        slot.ackPending = (ackType==='landing_ack'); // landing_ack 後還要等 landing_done
        slot.ackStatus=ackType;
        // 飛手回應（收到/已起飛/收到降落指令/降落完成）也記進飛行紀錄，帶回應時間
        const ackLabelMap={ack:'已收到',takeoff:'已起飛',landing_ack:'收到降落指令',landing_done:'降落完成'};
        pushComm(dispName(pilot),'pilot',notamLabel(pilot,idx)+(ackLabelMap[ackType]||ackType));
        if(ackType==='landing_done'){
          slot.ackPending=false; slot.landingLocked=false; slot.landingReported=true;
          // 飛行紀錄的降落時間已經在 applyStatus() 送出「降落」指令當下記過了（塔台最後一次送的時間），
          // 這裡只是飛手確認，不要再多記一筆，只需要把參照清掉讓下一輪能重新建立
          pendingLandingEntry.delete(pilot.clientId+':'+idx);
        }
        syncLegacyFromSlot0(pilot);
        broadcastPilots();
        if(ackLabelMap[ackType]) pushToOwnerTower(conn.clientId,{title:'飛手回報',body:dispName(pilot)+notamLabel(pilot,idx)+' '+ackLabelMap[ackType]});
        console.log('[ACK] master clientId:', conn.clientId, 'ackType:', ackType, 'notamIndex:', idx);
        let followerCount=0;
        connections.forEach(c=>{ if(c.role==='follower'&&c.masterClientId===conn.clientId) followerCount++; });
        console.log('[ACK] followers found:', followerCount);
        toFollowers(conn.clientId,{type:'follower_ack_sync',ackType});
        break;
      }

      case 'tower_notam':{
        const pilot=pilots.get(msg.clientId); if(!pilot) return;
        pilot.notam=msg.notam;
        toPilot(msg.clientId,{type:'notam_update',notam:msg.notam});
        toFollowers(msg.clientId,{type:'notam_update',notam:msg.notam});
        broadcastPilots();
        break;
      }

      // 飛手（M5Stack）管理自己最多3個 NOTAM：新增／改代碼／刪除某一筆
      case 'pilot_notam_manage':{
        const pilot=pilots.get(conn.clientId); if(!pilot) return;
        const ns=ensureNotams(pilot);
        const action=msg.action;
        const code=(msg.code||'').toString().slice(0,20);
        if(action==='add'){
          if(ns.length>=3) return;
          ns.push({code, status:'開機預備', lastCommType:'status', hasCommand:false, landingTime:null, landingReason:'',
            landingLocked:false, landingReported:false, ackPending:false, ackStatus:'', ackDeadline:null,
            rwy:'', lastMessage:'', lastMessageTime:'', groupId:null});
        } else if(action==='edit'){
          const s=ns[msg.index]; if(!s) return;
          s.code=code;
        } else if(action==='remove'){
          if(ns.length<=1) return; // 至少保留1筆
          removeGroupMembership(pilot.clientId,msg.index);
          shiftGroupMembershipDown(pilot.clientId,msg.index);
          ns.splice(msg.index,1);
          pendingLandingEntry.delete(pilot.clientId+':'+msg.index);
        } else return;
        syncLegacyFromSlot0(pilot);
        broadcastPilots();
        broadcastGroups();
        toPilot(conn.clientId,{type:'notams_update',notams:ns});
        toOwnerTower(conn.clientId,{type:'pilot_notam_update',pilotName:dispName(pilot),clientId:conn.clientId,notam:ns[0].code});
        toFollowers(conn.clientId,{type:'notam_update',notam:ns[0].code});
        break;
      }

      case 'pilot_end_session':{
        const pilot=pilots.get(conn.clientId);
        if(pilot){
          pilot.codeGen=(pilot.codeGen||0)+1; // 結束作業後序號要換掉，下次 pilot_register 才不會算出同一組舊序號
          pilot.pendingReset=true; // 下次用新序號重新註冊時，飛航公告/狀態要清空，不要延續這輪舊資料
          pilot.status='結束作業'; // 讓塔台飛手列表的狀態欄也顯示，不要停在結束前最後一個狀態
          pilot.ackStatus='session_ended'; // 讓右側標籤（跟已起飛/已降落同一區）也顯示「結束作業」
          pilot.hasCommand=true;
          const {tName,tType}=getActiveTower(pilot);
          flightLog.push({date:todayStr(),groupName:groupName(pilot.groupId),pilotName:dispName(pilot),type:'session_end',time:nowTimeStr(),rwy:pilot.rwy||'',towerName:tName,towerType:tType});
          pushComm(dispName(pilot),'tower','任務結束');
          broadcastPilots();
        }
        toOwnerTower(conn.clientId,{type:'session_ended',pilotName:pilot?dispName(pilot):msg.pilotName});
        pushToOwnerTower(conn.clientId,{title:'飛手回報',body:(pilot?dispName(pilot):msg.pilotName)+' 結束作業'});
        break;
      }

      case 'pilot_ask_status':{
        const pilot=pilots.get(conn.clientId);
        const name=pilot?dispName(pilot):msg.pilotName;
        const askType=msg.askType==='duration'?'duration':'airport';
        const askLabel=askType==='duration'?'詢問放行時長':'詢問機場狀況';
        pushComm(name,'pilot',askLabel);
        toOwnerTower(conn.clientId,{type:'pilot_asking',pilotName:name,clientId:conn.clientId,askType});
        pushToOwnerTower(conn.clientId,{title:'飛手詢問',body:name+' '+askLabel});
        break;
      }

      case 'pilot_notam':{
        // 舊版單一 NOTAM 路徑（M5Stack 只有1個 NOTAM 時用這個）：固定寫 slot 0
        const pilot=pilots.get(conn.clientId);
        if(!pilot) return;
        const ns=ensureNotams(pilot);
        ns[0].code=msg.notam;
        syncLegacyFromSlot0(pilot);
        const today=todayStr();
        const hasStart=flightLog.some(r=>r.type==='notam_start'&&r.pilotName===dispName(pilot)&&r.date===today);
        if(!hasStart){
          const {tName,tType}=getActiveTower(pilot);
          flightLog.push({date:today,groupName:groupName(pilot.groupId),pilotName:dispName(pilot),type:'notam_start',time:nowTimeStr(),notam:msg.notam,rwy:pilot.rwy||'',towerName:tName,towerType:tType});
        }
        pushComm(dispName(pilot),'pilot','更新飛航公告: '+msg.notam);
        broadcastPilots();
        toOwnerTower(conn.clientId,{type:'pilot_notam_update',pilotName:dispName(pilot),clientId:conn.clientId,notam:msg.notam});
        toFollowers(conn.clientId,{type:'notam_update',notam:msg.notam});
        pushToOwnerTower(conn.clientId,{title:'飛手回報',body:dispName(pilot)+' 更新飛航公告 '+msg.notam});
        break;
      }

      case 'pilot_turnpoint':{
        const pilot=pilots.get(conn.clientId);
        if(!pilot) return;
        pilot.turnpoint={minutes:msg.minutes,viaNotam:!!msg.viaNotam,ts:Date.now()};
        pilot.arrived=false;
        pushComm(dispName(pilot),'pilot','回報轉點'+(msg.viaNotam?'（公告轉點）':'')+'，約'+msg.minutes+'分鐘');
        broadcastPilots();
        toOwnerTower(conn.clientId,{type:'pilot_turnpoint',pilotName:dispName(pilot),clientId:conn.clientId,minutes:msg.minutes,viaNotam:!!msg.viaNotam});
        pushToOwnerTower(conn.clientId,{title:'飛手回報',body:dispName(pilot)+' 回報轉點 約'+msg.minutes+'分'});
        break;
      }

      case 'follower_confirm':{
        // 飛聚跟隨模式：強制回報，回報給主控飛手本人（不是塔台）
        if(!conn.masterClientId) return;
        const stg=msg.stage||'ack';
        toPilot(conn.masterClientId, {type:'follower_confirm', followerName: conn.followerName||'', stage: stg});
        // 更新該跟隨者在 followers 清單裡的回報狀態，推給監看端
        const mp=pilots.get(conn.masterClientId);
        if(mp&&mp.followers){
          const f=mp.followers.find(x=>x.clientId===conn.clientId);
          if(f){ f.stage=stg; f.pending=false; }
        }
        monitorUpdate(conn.masterClientId);
        break;
      }

      case 'monitor_set_name':{
        // 主控模式輔助：用手機輸入法設定主控的顯示名（不影響原本序號/識別用的名字）
        if(conn.role!=='monitor') return;
        const mp=pilots.get(conn.masterClientId); if(!mp) return;
        const dn=(msg.name||'').toString().trim().slice(0,10);
        mp.displayName=dn;
        broadcastPilots();
        monitorUpdate(conn.masterClientId);
        toPilot(conn.masterClientId,{type:'name_update',name:dispName(mp)});
        break;
      }

      case 'monitor_to_tower_msg':{
        // 主控模式輔助：以主控者名義傳自由訊息給塔台
        if(conn.role!=='monitor') return;
        const mp=pilots.get(conn.masterClientId);
        const nm=(mp&&dispName(mp))||conn.followerName||'主控';
        const txt=(msg.message||'').toString().slice(0,120);
        if(!txt) return;
        pushComm(nm,'pilot',txt);
        toOwnerTower(conn.masterClientId,{type:'pilot_msg_to_tower', pilotName:nm, message:txt});
        pushToOwnerTower(conn.masterClientId,{title:'飛手訊息',body:nm+'：'+txt});
        break;
      }

      case 'pilot_arrived':{
        const pilot=pilots.get(conn.clientId);
        if(!pilot) return;
        pilot.turnpoint=null;
        pilot.arrived=true;
        pushComm(dispName(pilot),'pilot','已就位');
        broadcastPilots();
        toOwnerTower(conn.clientId,{type:'pilot_arrived',pilotName:dispName(pilot),clientId:conn.clientId});
        pushToOwnerTower(conn.clientId,{title:'飛手回報',body:dispName(pilot)+' 已就位'});
        break;
      }

      case 'pilot_land_report':{
        const pilot=pilots.get(conn.clientId);
        if(!pilot) return;
        const idx=msg.notamIndex||0;
        const slot=notamSlot(pilot,idx);
        slot.status='降落';
        slot.ackStatus='landing_done';  // 更新回應狀態為已降落
        slot.ackPending=false;
        slot.landingLocked=false;
        slot.landingReported=true;
        syncLegacyFromSlot0(pilot);
        const gn=groupName(slot.groupId);
        const {tName,tType}=getActiveTower(pilot);
        const ldTime=nowTimeStr().replace(':','');
        flightLog.push({date:todayStr(),groupName:gn,pilotName:dispName(pilot),type:'landing',time:ldTime,rwy:slot.rwy||'',notam:slot.code||'',towerName:tName,towerType:tType});
        pendingLandingEntry.delete(pilot.clientId+':'+idx); // 保險：這是飛手自己主動回報降落（沒有先前塔台指令），清掉任何殘留參照
        pushComm(dispName(pilot),'pilot',notamLabel(pilot,idx)+'回報降落');
        broadcastPilots();
        toOwnerTower(conn.clientId,{type:'pilot_land_report',pilotName:dispName(pilot),clientId:conn.clientId});
        pushToOwnerTower(conn.clientId,{title:'飛手回報',body:dispName(pilot)+notamLabel(pilot,idx)+' 回報降落'});
        break;
      }

      case 'tower_rwy':{
        const pilot=pilots.get(msg.clientId);
        if(!pilot) return;
        const idx=msg.notamIndex||0;
        const rwySlot=notamSlot(pilot,idx);
        // 同分類的 NOTAM 要一起同步跑道方向，不是只有被改的那一筆；每個成員各自用自己的 notamIndex
        const targets = rwySlot.groupId ? (groups.get(rwySlot.groupId)?.memberIds||[{clientId:msg.clientId,notamIndex:idx}]) : [{clientId:msg.clientId,notamIndex:idx}];
        targets.forEach(m=>{
          const p=pilots.get(m.clientId); if(!p) return;
          const tIdx=m.notamIndex||0;
          const s=notamSlot(p,tIdx);
          s.rwy=msg.rwy;
          syncLegacyFromSlot0(p);
          toPilot(m.clientId,{type:'rwy_update',rwy:msg.rwy,notamIndex:tIdx});
          toFollowers(m.clientId,{type:'rwy_update',rwy:msg.rwy});
        });
        broadcastPilots();
        break;
      }
    }
  });

  ws.on('close',()=>{
    const conn=connections.get(ws);
    if(conn&&conn.clientId){
      const p=pilots.get(conn.clientId);
      if(p){p.wifi=false;p.lastSeen=Date.now();broadcastPilots();}
      if(conn.role==='follower'){
        // 從主控的 followers 清單移除
        pilots.forEach(mp=>{
          if(mp.followers) mp.followers=mp.followers.filter(f=>f.clientId!==conn.clientId);
        });
        if(conn.masterClientId) monitorUpdate(conn.masterClientId);
        broadcastPilots();
      }
    }
    connections.delete(ws);
  });
});

server.listen(PORT,()=>console.log(`Drone Tower Server running on http://localhost:${PORT}`));
