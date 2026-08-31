const http = require('http');
const fs   = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

const PORT = process.env.PORT || 3000;

const pilots = new Map();
const groups = new Map();
const flightLog = [];
const commLog = [];
let groupCounter = 1;
// 持久化的塔台名稱/類型：改用單一保存值而不是每次即時掃描connections，
// 避免掃描當下剛好抓不到還沒設定role的tower連線、造成飛手端顯示不到真正的塔台名稱
let towerNameGlobal = '塔台';
let towerTypeGlobal = '南塔';

const TZ='Asia/Taipei'; // Railway 主機預設是UTC，時間顯示都要明確指定台灣時區
function todayStr(){ return new Date().toLocaleDateString('zh-TW',{timeZone:TZ}); }
function nowTimeStr(){ return new Date().toLocaleTimeString('zh-TW',{hour:'2-digit',minute:'2-digit',hour12:false,timeZone:TZ}); }

// 預設分類
groups.set('g_0', {name:'預設分類', memberIds:[]});
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
// 根據飛手名稱+今天日期，產生固定四碼序號
function dailyCodeForPilot(name){
  const dateStr=new Date().toISOString().slice(0,10);
  const seed=name+dateStr; let hash=0;
  for(let i=0;i<seed.length;i++) hash=(hash*31+seed.charCodeAt(i))>>>0;
  let c=''; for(let i=0;i<4;i++){ c+=ARROWS[hash%4]; hash=Math.floor(hash/4); }
  return c;
}

// 取得目前已連線的塔台資訊
function getActiveTower(){
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
  const rows=Object.values(byKey).map(g=>{
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
  let fp=path.join(__dirname,'public',req.url==='/'?'index.html':req.url);
  const mime={'.html':'text/html','.js':'text/javascript','.css':'text/css','.json':'application/json','.bin':'application/octet-stream'};
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

function pilotSnap(){ return Array.from(pilots.values()).map(p=>({...p})); }
function groupSnap(){ return Array.from(groups.entries()).map(([id,g])=>({groupId:id,name:g.name,memberIds:g.memberIds})); }
function groupName(gid){ const g=groups.get(gid); return g?g.name:''; }

// 對話紀錄：塔台發送的訊息/指令 + 飛手回報，供塔台「飛行記錄」頁面顯示
function pushComm(pilotName,dir,text){
  const entry={date:todayStr(),time:nowTimeStr(),pilotName,dir,text};
  commLog.push(entry);
  if(commLog.length>500) commLog.shift();
  toTower({type:'comm_log_add',entry});
}

function applyStatus(pilot,status,landingTime){
  pilot.status=status;
  pilot.lastCommType='status';
  pilot.hasCommand=true;
  pilot.lastMessageTime=nowTimeStr(); // 塔台每次來訊（指令或訊息）的時間，飛手端顯示用，跟 line 一樣
  if(landingTime) pilot.landingTime=landingTime;
  const gn=groupName(pilot.groupId);
  const {tName,tType}=getActiveTower();
  if(status==='可以起飛'){
    pilot.takeoffTime=new Date().toISOString();
    flightLog.push({date:todayStr(),groupName:gn,pilotName:pilot.name,type:'takeoff',time:nowTimeStr(),rwy:pilot.rwy||'',towerName:tName,towerType:tType});
  }
  if(status==='降落'){
    flightLog.push({date:todayStr(),groupName:gn,pilotName:pilot.name,type:'landing',time:nowTimeStr(),rwy:pilot.rwy||'',towerName:tName,towerType:tType});
    pilot.landingLocked=true; // 送出降落/馬上降落後鎖定，飛手回報降落前塔台不能再發其他指令/訊息給這個飛手
  }
  pushComm(pilot.name,'tower',status+(landingTime?(' '+landingTime):''));
  pilot.ackPending=true;
  pilot.ackStatus='pending'; // pending / ack / takeoff / landing_ack / landing_done
  pilot.ackDeadline=Date.now()+30000;
}

// 送出降落/馬上降落後鎖定；鎖定中只允許再次送「降落」（例如更新時間），其他指令/訊息要擋掉
function canSendToPilot(pilot,newStatus){ return !pilot.landingLocked || newStatus==='降落'; }

function updateGroupStatus(groupId,status,landingTime,immediate){
  const g=groups.get(groupId); if(!g) return;
  g.memberIds.forEach(cid=>{
    const p=pilots.get(cid); if(!p) return;
    if(!canSendToPilot(p,status)) return; // 跳過正在降落鎖定中的飛手，不影響同分類其他人
    applyStatus(p,status,landingTime);
    toPilot(cid,{type:'command',status,landingTime:landingTime||null,immediate:!!immediate,groupName:groupName(groupId),time:p.lastMessageTime});
    toFollowers(cid,{type:'follower_sync',status,landingTime:landingTime||null,immediate:!!immediate,groupName:groupName(groupId),time:p.lastMessageTime});
  });
  toTower({type:'pilots_update',pilots:pilotSnap()});
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
      case 'tower_hello':
        conn.role='tower';
        if(msg.towerName){ conn.towerName=msg.towerName; towerNameGlobal=msg.towerName; }
        if(msg.towerType){ conn.towerType=msg.towerType; towerTypeGlobal=msg.towerType; }
        // 先送 groups_update，再送 tower_state，確保塔台先有分類資料
        ws.send(JSON.stringify({type:'groups_update',groups:groupSnap()}));
        ws.send(JSON.stringify({type:'tower_state',pilots:pilotSnap(),groups:groupSnap(),flightLog:flightLog.slice(-200),commLog:commLog.slice(-200)}));
        break;

      case 'tower_add_pilot':{
        let found=null;
        pilots.forEach(p=>{if(p.roomCode===msg.roomCode&&Date.now()<p.roomCodeExpiry)found=p;});
        if(!found){ws.send(JSON.stringify({type:'error',message:'序號無效或已過期'}));return;}
        found.towerConnected=true;
        // 取得塔台名字和類型
        let tName='塔台'; let tType='南塔';
        connections.forEach((c,w)=>{
          if(c.role==='tower'){
            if(c.towerName) tName=c.towerName;
            if(c.towerType) tType=c.towerType;
          }
        });
        toTower({type:'pilots_update',pilots:pilotSnap()});
        toPilot(found.clientId,{type:'tower_connected',groupName:groupName(found.groupId),towerName:tName,towerType:tType});
        break;
      }

      case 'tower_command':{
        const {clientId,landingTime,isOther,immediate}=msg;
        const status=isOther?msg.status:msg.status;  // 直接使用，不加前綴
        const pilot=pilots.get(clientId); if(!pilot) return;
        if(pilot.groupId) updateGroupStatus(pilot.groupId,status,landingTime,immediate);
        else{
          if(!canSendToPilot(pilot,status)){
            ws.send(JSON.stringify({type:'error',message:pilot.name+' 正在降落中，尚未回報，無法發送其他指令'}));
            return;
          }
          applyStatus(pilot,status,landingTime);
          toPilot(clientId,{type:'command',status,landingTime:landingTime||null,immediate:!!immediate,groupName:'',time:pilot.lastMessageTime});
          toFollowers(clientId,{type:'follower_sync',status,landingTime:landingTime||null,immediate:!!immediate,groupName:'',time:pilot.lastMessageTime});
          toTower({type:'pilots_update',pilots:pilotSnap()});
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
        console.log('[MSG] sending to pilot:', pilot.name, 'clientId:', msg.clientId);
        // 確認 connections 裡有這個 clientId
        let found=false;
        wss.clients.forEach(ws=>{
          const c=connections.get(ws);
          if(c&&c.clientId===msg.clientId) found=true;
        });
        console.log('[MSG] connection found:', found);
        if(!canSendToPilot(pilot,null)){
          ws.send(JSON.stringify({type:'error',message:pilot.name+' 正在降落中，尚未回報，無法發送訊息'}));
          break;
        }
        const msgTime=nowTimeStr();
        pilot.lastMessage=msg.message;
        pilot.lastMessageTime=msgTime;
        pilot.lastCommType='message';
        pilot.hasCommand=true;
        pilot.ackPending=true; pilot.ackStatus='pending'; pilot.ackDeadline=Date.now()+30000;
        pushComm(pilot.name,'tower',msg.message);
        toPilot(msg.clientId,{type:'message',message:msg.message,time:msgTime});
        toFollowers(msg.clientId,{type:'message',message:msg.message,time:msgTime});
        toTower({type:'pilots_update',pilots:pilotSnap()});
        break;
      }

      case 'tower_create_group':{
        const gid='g_'+(groupCounter++);
        groups.set(gid,{name:msg.name||'新分類',memberIds:[]});
        toTower({type:'groups_update',groups:groupSnap()});
        break;
      }

      case 'tower_rename_group':{
        const g=groups.get(msg.groupId); if(!g) break;
        g.name=msg.name;
        g.memberIds.forEach(cid=>{
          toPilot(cid,{type:'group_update',groupName:msg.name});
          toFollowers(cid,{type:'group_update',groupName:msg.name});
        });
        toTower({type:'groups_update',groups:groupSnap()});
        break;
      }

      case 'tower_delete_group':{
        const g=groups.get(msg.groupId); if(!g) break;
        g.memberIds.forEach(cid=>{const p=pilots.get(cid);if(p){p.groupId=null;toPilot(cid,{type:'group_update',groupName:''});}});
        groups.delete(msg.groupId);
        toTower({type:'groups_update',groups:groupSnap()});
        toTower({type:'pilots_update',pilots:pilotSnap()});
        break;
      }

      case 'tower_assign_group':{
        const {clientId,groupId}=msg;
        const pilot=pilots.get(clientId); if(!pilot) return;
        if(pilot.groupId){const old=groups.get(pilot.groupId);if(old)old.memberIds=old.memberIds.filter(x=>x!==clientId);}
        pilot.groupId=groupId||null;
        if(groupId){const grp=groups.get(groupId);if(grp&&!grp.memberIds.includes(clientId))grp.memberIds.push(clientId);}
        toPilot(clientId,{type:'group_update',groupName:groupName(groupId)});
        toFollowers(clientId,{type:'group_update',groupName:groupName(groupId)});
        toTower({type:'pilots_update',pilots:pilotSnap()});
        toTower({type:'groups_update',groups:groupSnap()});
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
        const fid = 'f_'+generateClientId();
        conn.role='follower'; conn.clientId=fid; conn.masterClientId=masterPilot.clientId; conn.followerName=name;
        conn.gather=!!msg.gather; // 飛聚跟隨模式：需強制回報給主控者
        // 加入主控的 followers 清單；同名跟隨者重連（掉線重連/換分頁）要換掉舊的，不要一直疊加重複項目
        if(!masterPilot.followers) masterPilot.followers=[];
        masterPilot.followers=masterPilot.followers.filter(f=>f.name!==name);
        masterPilot.followers.push({clientId:fid, name, gather:!!msg.gather});
        // 送出已連線
        ws.send(JSON.stringify({
          type:'follower_registered',
          clientId:fid,
          groupName:groupName(masterPilot.groupId),
          towerName: getTowerName(),
          towerType: getTowerType(),
          status: masterPilot.status,
          landingTime: masterPilot.landingTime,
          lastMessage: masterPilot.lastMessage||'',
          lastMessageTime: masterPilot.lastMessageTime||'',
          lastCommType: masterPilot.lastCommType||'status',
          hasCommand: !!masterPilot.hasCommand,
          notam: masterPilot.notam||'',
          rwy: masterPilot.rwy||''
        }));
        // 告知塔台有跟隨者
        toTower({type:'pilots_update', pilots:pilotSnap()});
        break;
      }

      case 'pilot_rename':{
        const pilot=pilots.get(conn.clientId); if(!pilot) return;
        pilot.name=msg.name;
        toTower({type:'pilots_update',pilots:pilotSnap()});
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
        toTower({type:'pilots_update',pilots:pilotSnap()});
        break;
      }

      case 'pilot_register':{
        const pilotName=msg.name||'未知飛手';
        const roomCode=dailyCodeForPilot(pilotName);  // 每天固定序號
        const expiry=todayMidnight();

        // 若同名飛手已存在（斷線重連），保留其資料
        let existingId=null;
        pilots.forEach((p,cid)=>{ if(p.name===pilotName) existingId=cid; });

        let clientId;
        if(existingId){
          // 重連：沿用舊 clientId 和資料
          clientId=existingId;
          conn.role='pilot'; conn.clientId=clientId;
          const ep=pilots.get(clientId);
          // 配對狀態只在「同一天」內自動延續；跨天視為過期，要求塔台重新輸入序號
          const lastSeenSameDay=ep.lastSeen&&(new Date(ep.lastSeen).toLocaleDateString('zh-TW',{timeZone:TZ})===todayStr());
          const wasTowerConnected=ep.towerConnected&&lastSeenSameDay;
          if(!wasTowerConnected) ep.towerConnected=false;
          ep.wifi=true; ep.lastSeen=Date.now();
          ep.roomCode=roomCode; ep.roomCodeExpiry=expiry;
          ep.battery=msg.battery||ep.battery||100;

          ws.send(JSON.stringify({type:'registered',clientId,roomCode,reconnect:true}));
          toTower({type:'pilots_update',pilots:pilotSnap()});

          // 如果之前已有塔台配對，自動重新發送 tower_connected，不需要塔台重新輸入序號
          if(wasTowerConnected){
            const {tName,tType}=getActiveTower();
            toPilot(clientId,{type:'tower_connected',groupName:groupName(ep.groupId),towerName:tName,towerType:tType,reconnect:true});
          }
        } else {
          // 全新飛手
          clientId=generateClientId();
          conn.role='pilot'; conn.clientId=clientId;
          pilots.set(clientId,{
            clientId,name:pilotName,roomCode,notam:'',rwy:'',
            roomCodeExpiry:expiry,
            groupId:null,status:'開機預備',lastCommType:'status',hasCommand:false,wifi:true,gps:false,
            lat:null,lng:null,battery:msg.battery||100,lastMessage:'',
            lastSeen:Date.now(),ackPending:false,ackStatus:'',ackDeadline:null,
            landingTime:null,takeoffTime:null,towerConnected:false,
            connectedAt:new Date().toISOString(),
          });
          ws.send(JSON.stringify({type:'registered',clientId,roomCode}));
          toTower({type:'pilots_update',pilots:pilotSnap()});
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
        toTower({type:'pilots_update',pilots:pilotSnap()});
        break;
      }

      case 'pilot_ack':{
        const pilot=pilots.get(conn.clientId); if(!pilot) return;
        const ackType=msg.ackType||'ack'; // ack / takeoff / landing_ack / landing_done
        pilot.ackPending = (ackType==='landing_ack'); // landing_ack 後還要等 landing_done
        pilot.ackStatus=ackType;
        if(ackType==='landing_done'){ pilot.ackPending=false; pilot.landingLocked=false; }
        toTower({type:'pilots_update',pilots:pilotSnap()});
        console.log('[ACK] master clientId:', conn.clientId, 'ackType:', ackType);
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
        toTower({type:'pilots_update',pilots:pilotSnap()});
        break;
      }

      case 'pilot_end_session':{
        const pilot=pilots.get(conn.clientId);
        if(pilot){
          const {tName,tType}=getActiveTower();
          flightLog.push({date:todayStr(),groupName:groupName(pilot.groupId),pilotName:pilot.name,type:'session_end',time:nowTimeStr(),rwy:pilot.rwy||'',towerName:tName,towerType:tType});
          pushComm(pilot.name,'tower','任務結束');
        }
        toTower({type:'session_ended',pilotName:pilot?pilot.name:msg.pilotName});
        break;
      }

      case 'pilot_ask_status':{
        const pilot=pilots.get(conn.clientId);
        const name=pilot?pilot.name:msg.pilotName;
        const askType=msg.askType==='duration'?'duration':'airport';
        pushComm(name,'pilot',askType==='duration'?'詢問放行時長':'詢問機場狀況');
        toTower({type:'pilot_asking',pilotName:name,clientId:conn.clientId,askType});
        break;
      }

      case 'pilot_notam':{
        const pilot=pilots.get(conn.clientId);
        if(!pilot) return;
        pilot.notam=msg.notam;
        const today=todayStr();
        const hasStart=flightLog.some(r=>r.type==='notam_start'&&r.pilotName===pilot.name&&r.date===today);
        if(!hasStart){
          const {tName,tType}=getActiveTower();
          flightLog.push({date:today,groupName:groupName(pilot.groupId),pilotName:pilot.name,type:'notam_start',time:nowTimeStr(),notam:msg.notam,rwy:pilot.rwy||'',towerName:tName,towerType:tType});
        }
        pushComm(pilot.name,'pilot','更新飛航公告: '+msg.notam);
        toTower({type:'pilots_update',pilots:pilotSnap()});
        toTower({type:'pilot_notam_update',pilotName:pilot.name,clientId:conn.clientId,notam:msg.notam});
        toFollowers(conn.clientId,{type:'notam_update',notam:msg.notam});
        break;
      }

      case 'pilot_turnpoint':{
        const pilot=pilots.get(conn.clientId);
        if(!pilot) return;
        pilot.turnpoint={minutes:msg.minutes,viaNotam:!!msg.viaNotam,ts:Date.now()};
        pilot.arrived=false;
        pushComm(pilot.name,'pilot','回報轉點'+(msg.viaNotam?'（公告轉點）':'')+'，約'+msg.minutes+'分鐘');
        toTower({type:'pilots_update',pilots:pilotSnap()});
        toTower({type:'pilot_turnpoint',pilotName:pilot.name,clientId:conn.clientId,minutes:msg.minutes,viaNotam:!!msg.viaNotam});
        break;
      }

      case 'follower_confirm':{
        // 飛聚跟隨模式：強制回報，回報給主控飛手本人（不是塔台）
        if(!conn.masterClientId) return;
        toPilot(conn.masterClientId, {type:'follower_confirm', followerName: conn.followerName||'', stage: msg.stage||''});
        break;
      }

      case 'pilot_arrived':{
        const pilot=pilots.get(conn.clientId);
        if(!pilot) return;
        pilot.turnpoint=null;
        pilot.arrived=true;
        pushComm(pilot.name,'pilot','已就位');
        toTower({type:'pilots_update',pilots:pilotSnap()});
        toTower({type:'pilot_arrived',pilotName:pilot.name,clientId:conn.clientId});
        break;
      }

      case 'pilot_land_report':{
        const pilot=pilots.get(conn.clientId);
        if(!pilot) return;
        pilot.status='降落';
        pilot.ackStatus='landing_done';  // 更新回應狀態為已降落
        pilot.ackPending=false;
        pilot.landingLocked=false;
        const gn=groupName(pilot.groupId);
        const {tName,tType}=getActiveTower();
        const ldTime=nowTimeStr().replace(':','');
        flightLog.push({date:todayStr(),groupName:gn,pilotName:pilot.name,type:'landing',time:ldTime,rwy:pilot.rwy||'',towerName:tName,towerType:tType});
        pushComm(pilot.name,'pilot','回報降落');
        toTower({type:'pilots_update',pilots:pilotSnap()});
        toTower({type:'pilot_land_report',pilotName:pilot.name,clientId:conn.clientId});
        break;
      }

      case 'tower_rwy':{
        const pilot=pilots.get(msg.clientId);
        if(!pilot) return;
        // 同分類的飛手要一起同步跑道方向，不是只有被點的那個
        const targets = pilot.groupId ? (groups.get(pilot.groupId)?.memberIds||[msg.clientId]) : [msg.clientId];
        targets.forEach(cid=>{
          const p=pilots.get(cid); if(!p) return;
          p.rwy=msg.rwy;
          toPilot(cid,{type:'rwy_update',rwy:msg.rwy});
          toFollowers(cid,{type:'rwy_update',rwy:msg.rwy});
        });
        toTower({type:'pilots_update',pilots:pilotSnap()});
        break;
      }
    }
  });

  ws.on('close',()=>{
    const conn=connections.get(ws);
    if(conn&&conn.clientId){
      const p=pilots.get(conn.clientId);
      if(p){p.wifi=false;p.lastSeen=Date.now();toTower({type:'pilots_update',pilots:pilotSnap()});}
      if(conn.role==='follower'){
        // 從主控的 followers 清單移除
        pilots.forEach(mp=>{
          if(mp.followers) mp.followers=mp.followers.filter(f=>f.clientId!==conn.clientId);
        });
        toTower({type:'pilots_update',pilots:pilotSnap()});
      }
    }
    connections.delete(ws);
  });
});

server.listen(PORT,()=>console.log(`Drone Tower Server running on http://localhost:${PORT}`));
