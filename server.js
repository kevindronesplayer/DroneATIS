const http = require('http');
const fs   = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');

const PORT = process.env.PORT || 3000;

const pilots = new Map();
const groups = new Map();
const flightLog = [];
let groupCounter = 1;

// 預設分類
groups.set('g_0', {name:'預設分類', memberIds:[]});
groupCounter = 1;

const ARROWS = ['1','2','3','4'];
function generateRoomCode(){ let c=''; for(let i=0;i<4;i++) c+=ARROWS[Math.floor(Math.random()*4)]; return c; }
function generateClientId(){ return 'p_'+Date.now()+'_'+Math.random().toString(36).slice(2,7); }

// 當天午夜過期
function todayMidnight(){ const d=new Date(); d.setHours(23,59,59,999); return d.getTime(); }
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
  let tName='塔台'; let tType='南塔';
  connections.forEach((c)=>{
    if(c.role==='tower'){ if(c.towerName) tName=c.towerName; if(c.towerType) tType=c.towerType; }
  });
  return {tName,tType};
}

function generateCSV(){
  const lines=['\uFEFF日期,分類名稱,飛手名稱,動作,時間'];
  flightLog.forEach(r=>lines.push(`${r.date},${r.groupName||'未分類'},${r.pilotName},${r.type==='takeoff'?'起飛':'降落'},${r.time}`));
  return lines.join('\n');
}

const server = http.createServer((req,res)=>{
  if(req.url==='/download-log'){
    const csv=generateCSV();
    const date=new Date().toLocaleDateString('zh-TW').replace(/\//g,'-');
    res.writeHead(200,{'Content-Type':'text/csv;charset=utf-8','Content-Disposition':`attachment;filename="flight-log-${date}.csv"`});
    res.end(csv); return;
  }
  let fp=path.join(__dirname,'public',req.url==='/'?'index.html':req.url);
  const mime={'.html':'text/html','.js':'text/javascript','.css':'text/css'};
  fs.readFile(fp,(err,data)=>{
    if(err){res.writeHead(404);res.end('Not found');return;}
    res.writeHead(200,{'Content-Type':mime[path.extname(fp)]||'text/plain'});
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
function getTowerName(){
  let n='塔台';
  connections.forEach((c)=>{ if(c.role==='tower'&&c.towerName) n=c.towerName; });
  return n;
}
function getTowerType(){
  let t='南塔';
  connections.forEach((c)=>{ if(c.role==='tower'&&c.towerType) t=c.towerType; });
  return t;
}
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

function applyStatus(pilot,status,landingTime){
  pilot.status=status;
  if(landingTime) pilot.landingTime=landingTime;
  const gn=groupName(pilot.groupId);
  if(status==='可以起飛'){
    pilot.takeoffTime=new Date().toISOString();
    flightLog.push({date:new Date().toLocaleDateString('zh-TW'),groupName:gn,pilotName:pilot.name,type:'takeoff',time:new Date().toLocaleTimeString('zh-TW',{hour:'2-digit',minute:'2-digit',hour12:false})});
  }
  if(status==='降落'){
    flightLog.push({date:new Date().toLocaleDateString('zh-TW'),groupName:gn,pilotName:pilot.name,type:'landing',time:new Date().toLocaleTimeString('zh-TW',{hour:'2-digit',minute:'2-digit',hour12:false})});
  }
  pilot.ackPending=true;
  pilot.ackStatus='pending'; // pending / ack / takeoff / landing_ack / landing_done
  pilot.ackDeadline=Date.now()+30000;
}

function updateGroupStatus(groupId,status,landingTime,immediate){
  const g=groups.get(groupId); if(!g) return;
  g.memberIds.forEach(cid=>{
    const p=pilots.get(cid); if(!p) return;
    applyStatus(p,status,landingTime);
    toPilot(cid,{type:'command',status,landingTime:landingTime||null,immediate:!!immediate,groupName:groupName(groupId)});
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
        if(msg.towerName) conn.towerName=msg.towerName;
        if(msg.towerType) conn.towerType=msg.towerType;
        // 先送 groups_update，再送 tower_state，確保塔台先有分類資料
        ws.send(JSON.stringify({type:'groups_update',groups:groupSnap()}));
        ws.send(JSON.stringify({type:'tower_state',pilots:pilotSnap(),groups:groupSnap(),flightLog:flightLog.slice(-200)}));
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
          applyStatus(pilot,status,landingTime);
          toPilot(clientId,{type:'command',status,landingTime:landingTime||null,immediate:!!immediate,groupName:''});
          toFollowers(clientId,{type:'follower_sync',status,landingTime:landingTime||null,immediate:!!immediate,groupName:''});
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
        pilot.lastMessage=msg.message;
        pilot.ackPending=true; pilot.ackStatus='pending'; pilot.ackDeadline=Date.now()+30000;
        toPilot(msg.clientId,{type:'message',message:msg.message});
        toFollowers(msg.clientId,{type:'message',message:msg.message});
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
        // 加入主控的 followers 清單
        if(!masterPilot.followers) masterPilot.followers=[];
        masterPilot.followers.push({clientId:fid, name});
        // 送出已連線
        ws.send(JSON.stringify({
          type:'follower_registered',
          clientId:fid,
          groupName:groupName(masterPilot.groupId),
          towerName: getTowerName(),
          towerType: getTowerType(),
          status: masterPilot.status,
          landingTime: masterPilot.landingTime,
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
          const wasTowerConnected=ep.towerConnected;
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
            groupId:null,status:'開機預備',wifi:true,gps:false,
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
        if(ackType==='landing_done') pilot.ackPending=false;
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

      case 'pilot_end_session':
        toTower({type:'session_ended',pilotName:msg.pilotName});
        break;

      case 'pilot_ask_status':{
        const pilot=pilots.get(conn.clientId);
        const name=pilot?pilot.name:msg.pilotName;
        toTower({type:'pilot_asking',pilotName:name,clientId:conn.clientId});
        break;
      }

      case 'pilot_notam':{
        const pilot=pilots.get(conn.clientId);
        if(!pilot) return;
        pilot.notam=msg.notam;
        toTower({type:'pilots_update',pilots:pilotSnap()});
        toTower({type:'pilot_notam_update',pilotName:pilot.name,clientId:conn.clientId,notam:msg.notam});
        toFollowers(conn.clientId,{type:'notam_update',notam:msg.notam});
        break;
      }

      case 'pilot_turnpoint':{
        const pilot=pilots.get(conn.clientId);
        if(!pilot) return;
        pilot.turnpoint={minutes:msg.minutes,viaNotam:!!msg.viaNotam,ts:Date.now()};
        toTower({type:'pilots_update',pilots:pilotSnap()});
        toTower({type:'pilot_turnpoint',pilotName:pilot.name,clientId:conn.clientId,minutes:msg.minutes,viaNotam:!!msg.viaNotam});
        break;
      }

      case 'pilot_land_report':{
        const pilot=pilots.get(conn.clientId);
        if(!pilot) return;
        pilot.status='降落';
        pilot.ackStatus='landing_done';  // 更新回應狀態為已降落
        pilot.ackPending=false;
        const gn=groupName(pilot.groupId);
        const ldTime=new Date().toLocaleTimeString('zh-TW',{hour:'2-digit',minute:'2-digit',hour12:false}).replace(':','');
        flightLog.push({date:new Date().toLocaleDateString('zh-TW'),groupName:gn,pilotName:pilot.name,type:'landing',time:ldTime});
        toTower({type:'pilots_update',pilots:pilotSnap()});
        toTower({type:'pilot_land_report',pilotName:pilot.name,clientId:conn.clientId});
        break;
      }

      case 'tower_rwy':{
        const pilot=pilots.get(msg.clientId);
        if(!pilot) return;
        pilot.rwy=msg.rwy;
        toPilot(msg.clientId,{type:'rwy_update',rwy:msg.rwy});
        toFollowers(msg.clientId,{type:'rwy_update',rwy:msg.rwy});
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
