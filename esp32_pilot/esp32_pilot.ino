/*
 * DroneATIS — M5Stack Core2
 * 函式庫：M5Unified, M5GFX, WebSockets(Markus Sattler 2.7.2), ArduinoJson, Preferences, WiFi
 */

#include <M5Unified.h>
#include <M5GFX.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <TinyGPSPlus.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ── 設定 ────────────────────────────────────────────────────────────────────
#define SERVER_HOST   "tramway.proxy.rlwy.net"
#define SERVER_PORT   26772
#define TZ_OFFSET     8
#define NTP_SERVER    "pool.ntp.org"
#define GPS_RX_PIN    32   // Core2 PORT.A（外接I2C腳位，這裡改當UART用；訊號1=RXD）
#define GPS_TX_PIN    33   // Core2 PORT.A（訊號2=TXD）
#define GPS_BAUD      115200
#define FW_VERSION    37
#define UPDATE_CHECK_URL "https://droneatis-production.up.railway.app/firmware/version.json"

// ── NVS 儲存 ─────────────────────────────────────────────────────────────────
Preferences prefs;
String savedSSID     = "";
String savedPassword = "";
String pilotName     = "";
// 手機「主控模式輔助」可另外設定顯示名字（可中文）；只影響畫面顯示，不影響序號/重連用的 pilotName
String pilotDisplayName = "";

// ── WebSocket ─────────────────────────────────────────────────────────────────
WebSocketsClient wsClient;
bool wsConnected  = false;
bool wsConnecting = false;

// ── 飛行模式 ──────────────────────────────────────────────────────────────────
enum PilotMode { MODE_NONE, MODE_MASTER, MODE_FOLLOWER, MODE_GATHER };
PilotMode pilotMode = MODE_NONE;
// MODE_FOLLOWER：跟隨主控，純顯示、不回應
// MODE_GATHER （飛聚跟隨）：跟主控模式一樣強制回報，但回報對象是主控者、不是塔台
#define IS_FOLLOWER_CONN (pilotMode==MODE_FOLLOWER||pilotMode==MODE_GATHER)
#define NEEDS_ACK        (pilotMode==MODE_MASTER||pilotMode==MODE_GATHER)
String masterCode   = "";

// ── 狀態變數 ──────────────────────────────────────────────────────────────────
String clientId       = "";
String roomCode       = "";
String currentStatus  = "開機預備";
String lastMessage    = "";
String lastMessageTime = "";  // 塔台訊息發送時間（HH:MM），跟line一樣讓飛手知道是何時發出
bool showingMessage   = false;  // true=待命畫面顯示lastMessage，false=顯示currentStatus（兩者不同時顯示，看誰是最新的）
bool everReceivedCommand = false;  // 塔台是否曾經真的發過指令/訊息；沒有的話開機畫面要顯示「等待塔台來訊」而不是預設的「開機預備」
String landingTimeStr = "";
String landingReason  = "";
String groupName      = "";
String towerName      = "塔台";
String towerType      = "南塔";
String notamCode      = "";
String rwyDir         = "";
bool   towerConnected = false;
bool   gpsEnabled     = false;
bool   gpsFixed       = false;
TinyGPSPlus gps;
double gpsLat = 0, gpsLng = 0;
bool   ackPending     = false;
unsigned long ackReceivedAt = 0;
unsigned long ackDeadline   = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastConnCheck = 0;
unsigned long lastWifiBeginAt = 0;
unsigned long lastTimeUpd   = 0;
unsigned long lastBuzzAt    = 0;
int buzzPhase = 0;
bool landing60Fired = false;   // 降落倒數剩1分鐘的提醒只叫一次
int landingLastAlertSec = -1;  // 剩10秒以內，記錄上次已經叫過的那一秒，確保每一秒只叫一次
unsigned long rwyNoticeUntil = 0;
unsigned long followerConfirmUntil = 0;  // 主控收到飛聚跟隨回報訊息，顯示到這個時間就清掉
unsigned long lastActivity = 0;
bool screenDimmed = false;
const unsigned long IDLE_DIM_MS = 120000;

// 亮度
int brightnessLevel = 1;
const uint8_t BRIGHT_VAL[3] = {60, 140, 220};
const char*   BRIGHT_LBL[3] = {"暗","中","亮"};

// 降落
enum LandState { LAND_NONE, LAND_WAIT_ACK, LAND_COUNTDOWN };
// 這兩個 struct 定義要放在檔案很前面：Arduino 會在檔案最上面自動產生函式原型宣告，
// 如果型別定義放在後面（例如原本放在多 NOTAM那段），自動產生的原型會因為型別還不存在而編譯失敗
struct NotamSlot {
  String code, status, lastMessage, lastMessageTime, landingTimeStr, landingReason;
  String rwy, groupName; // 跑道/分類現在每個 NOTAM 各自獨立，不是共用同一份
  bool showingMessage=false, everReceivedCommand=false, ackPending=false;
  LandState landState=LAND_NONE;
  int immEndSec=-1;
};
struct NotamRowView{ String code,statusText,lastMessage,rwy,groupName; bool ackPending,showingMessage; LandState landState; };
LandState landState = LAND_NONE;
unsigned long landBtnPressAt = 0;
bool landBtnPressed = false;
unsigned long landDonePressAt = 0;
bool landDonePressed = false;

// 數字鍵盤
enum KeypadMode { KP_NONE, KP_NOTAM, KP_TURNPOINT };
KeypadMode keypadMode = KP_NONE;
String keypadBuffer   = "";
bool notamHadValue    = false;   // 開啟公告鍵盤時是否已有舊值（判斷是否為「第二次輸入」）
int  turnpointSource  = 0;       // 0=無, 1=專屬轉點按鈕, 2=公告二次輸入後詢問
String turnBtnLabel   = "就位";  // 底部按鈕目前顯示文字：「轉點」或「就位」，開機預設就位

// 韌體更新
int    pendingFwVersion = 0;
String pendingFwUrl     = "";
String pendingFwNotes   = "";

// ── 畫面狀態 ──────────────────────────────────────────────────────────────────
enum Screen {
  SCR_BOOT, SCR_NAME_INPUT, SCR_WIFI_SCAN, SCR_WIFI_PASS,
  SCR_MODE_SELECT, SCR_FOLLOWER_CODE, SCR_CODE, SCR_IDLE,
  SCR_COMMAND, SCR_END, SCR_CHARGING, SCR_TURNPOINT_CONFIRM, SCR_POWEROFF_CONFIRM, SCR_WIFI_CHANGE_CONFIRM, SCR_UPDATE_CONFIRM, SCR_MORE_MENU,
  SCR_NOTAM_LIST, SCR_NOTAM_DELETE_CONFIRM
};
Screen currentScreen = SCR_BOOT;
Screen moreMenuReturnScreen = SCR_IDLE;  // 開啟「更多」選單前所在的畫面，關閉後要回去
Screen renameReturnScreen = SCR_IDLE;    // 點名字改名前所在的畫面（IDLE 或 COMMAND），改完要回去
Screen wifiChangeReturnScreen = SCR_IDLE; // 點「更換WiFi」前所在的畫面，取消要回去
Screen powerOffReturnScreen = SCR_IDLE;  // 開啟關機確認前所在的畫面，取消後要回去
bool sessionEnded = false;  // 主控是否已按過「結束任務」，關機前必須先結束任務

// WiFi 掃描
struct WifiEntry { String ssid; int rssi; };
WifiEntry wifiList[20];
int wifiCount = 0;
int wifiScroll = 0;
String pendingSSID = "";

// 通用鍵盤
String kbBuffer  = "";
String kbHint    = "";
String kbTarget  = "";
bool   kbShift   = false;
int    kbPage    = 0;
int    kbMaxLen  = 20;

// ── 顏色 ──────────────────────────────────────────────────────────────────────
#define CLR_BG      0x0000
#define CLR_ACCENT  0x07FF
#define CLR_GREEN   0x07E0
#define CLR_RED     0xF800
#define CLR_AMBER   0xFD20
#define CLR_WHITE   0xFFFF
#define CLR_GRAY    0x7BEF
#define CLR_SURFACE 0x2124
#define CLR_DARK    0x10A2

// ── 字型 ──────────────────────────────────────────────────────────────────────
void fXs(){ M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(1); }
void fSm(){ M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(1); }
void fLg(){ M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(2); }

// UTF-8 顯示寬度估算：中文全形算1個單位，英數半形算0.55個單位（String::length()是算bytes，中文一字3bytes會算錯）
float estimateTextWidth(const String &s){
  float units=0; size_t i=0, n=s.length();
  while(i<n){
    uint8_t c=(uint8_t)s[i];
    if(c<0x80){ units+=0.55f; i+=1; }
    else if((c&0xE0)==0xC0){ units+=1.0f; i+=2; }
    else if((c&0xF0)==0xE0){ units+=1.0f; i+=3; }
    else if((c&0xF8)==0xF0){ units+=1.0f; i+=4; }
    else { i+=1; }
  }
  return units;
}

// 自由訊息依字數多寡自動縮放字體，避免長訊息超出螢幕、短訊息又顯得太小
void drawFitText(String txt, int cx, int cy, uint16_t color){
  float units=estimateTextWidth(txt); if(units<1) units=1;
  float scale=300.0f/(24.0f*units);
  if(scale>1.4f) scale=1.4f;
  if(scale<0.55f) scale=0.55f;
  M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(scale);
  M5.Display.setTextColor(color); M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(txt,cx,cy);
}

// ── 時間工具 ──────────────────────────────────────────────────────────────────
String getNowTime(){
  struct tm t; if(!getLocalTime(&t)) return "--:--";
  char buf[6]; sprintf(buf,"%02d:%02d",t.tm_hour,t.tm_min);
  return String(buf);
}
int getNowTotalSecs(){
  struct tm t; if(!getLocalTime(&t)) return 0;
  return t.tm_hour*3600+t.tm_min*60+t.tm_sec;
}
int immEndSec = -1; // 馬上降落：固定倒數截止秒（當天秒數）；-1 表示非馬上降落

// ── 多 NOTAM（最多3個，清單進入式）─────────────────────────────────────────
// 只有1個 NOTAM 時完全不會用到這一段，跟原本行為完全一樣：drawIdle/drawCommand/
// handleBuzzer/checkLandDoneHold 等既有邏輯繼續讀寫上面那批全域變數，一行都不用改。
// 2個以上才會用到：切換查看哪個 NOTAM 時，把目前這批全域變數存回背景陣列、
// 換另一筆的內容進來，這樣「畫面在看的」永遠就是這批全域變數，只是內容換了。
// （struct 定義本身放到檔案很前面去了，這裡只留全域變數跟函式）
NotamSlot notamSlots[3];
int notamCount = 1;      // 1~3；等於1時完全比照舊行為
int activeNotamIdx = 0;  // 目前畫面在看的是第幾個 NOTAM
bool notamAddMode = false; // 公告鍵盤目前是「新增一個 NOTAM」而不是「編輯目前這個」

void saveActiveNotamSlot(){
  NotamSlot &s=notamSlots[activeNotamIdx];
  s.code=notamCode; s.status=currentStatus; s.lastMessage=lastMessage; s.lastMessageTime=lastMessageTime;
  s.landingTimeStr=landingTimeStr; s.landingReason=landingReason;
  s.showingMessage=showingMessage; s.everReceivedCommand=everReceivedCommand; s.ackPending=ackPending;
  s.landState=landState; s.immEndSec=immEndSec;
  s.rwy=rwyDir; s.groupName=groupName;
}
// 切換去看某個 NOTAM：先把目前這份存回原本的 slot，再把目標 slot 的內容載入成目前這批全域變數
void switchToNotamSlot(int idx){
  if(idx<0||idx>=notamCount) return;
  if(idx!=activeNotamIdx) saveActiveNotamSlot();
  activeNotamIdx=idx;
  NotamSlot &s=notamSlots[idx];
  notamCode=s.code; currentStatus=s.status; lastMessage=s.lastMessage; lastMessageTime=s.lastMessageTime;
  landingTimeStr=s.landingTimeStr; landingReason=s.landingReason;
  showingMessage=s.showingMessage; everReceivedCommand=s.everReceivedCommand; ackPending=s.ackPending;
  landState=s.landState; immEndSec=s.immEndSec;
  rwyDir=s.rwy; groupName=s.groupName;
}
// 清單第 i 列要顯示的內容：如果是目前正在看的那個，資料還在全域變數裡（還沒存回陣列），
// 其餘的才是 notamSlots[i] 裡存好的
NotamRowView notamRowView(int i){
  NotamRowView v;
  if(i==activeNotamIdx){
    v.code=notamCode; v.statusText=currentStatus; v.lastMessage=lastMessage;
    v.ackPending=ackPending; v.showingMessage=showingMessage; v.landState=landState;
    v.rwy=rwyDir; v.groupName=groupName;
  } else {
    NotamSlot &s=notamSlots[i];
    v.code=s.code; v.statusText=s.status; v.lastMessage=s.lastMessage;
    v.ackPending=s.ackPending; v.showingMessage=s.showingMessage; v.landState=s.landState;
    v.rwy=s.rwy; v.groupName=s.groupName;
  }
  return v;
}

int landTimeSecs();
int landDiffSec(){ int d=landTimeSecs()-getNowTotalSecs(); if(d<-43200) d+=86400; return d; }
int landTimeSecs(){
  if(immEndSec>=0) return immEndSec;
  // 輸入 HHMM 代表「要在 HH:MM 前完成降落」，截止點為 HH:(MM-1):59
  if(landingTimeStr.length()<4) return 0;
  int lh=landingTimeStr.substring(0,2).toInt();
  int lm=landingTimeStr.substring(2,4).toInt();
  int s=lh*3600+lm*60-1; if(s<0) s+=86400; return s;
}
String getLandTimeDisplay(){
  if(landingTimeStr.length()<4) return "";
  int lh=landingTimeStr.substring(0,2).toInt();
  int lm=landingTimeStr.substring(2,4).toInt();
  char buf[6]; sprintf(buf,"%02d:%02d",lh,lm);
  return String(buf);
}

// ── NVS 讀寫 ──────────────────────────────────────────────────────────────────
void loadPrefs(){
  prefs.begin("datis",true);
  savedSSID     = prefs.getString("ssid","");
  savedPassword = prefs.getString("pass","");
  pilotName     = prefs.getString("name","");
  pilotDisplayName = prefs.getString("dispname","");
  prefs.end();
}
void saveWifi(String ssid, String pass){
  prefs.begin("datis",false);
  prefs.putString("ssid",ssid);
  prefs.putString("pass",pass);
  prefs.end();
}
void saveDisplayName(String name){
  prefs.begin("datis",false);
  prefs.putString("dispname",name);
  prefs.end();
}
void saveName(String name){
  prefs.begin("datis",false);
  prefs.putString("name",name);
  prefs.end();
}

// ── 電量 ──────────────────────────────────────────────────────────────────────
int getBattery(){ int b=M5.Power.getBatteryLevel(); return b>=0?b:100; }

void drawBattery(){
  int bat=getBattery();
  uint16_t bc=bat>50?CLR_GREEN:bat>20?CLR_AMBER:CLR_RED;
  M5.Display.fillRect(262,3,56,26,CLR_DARK);
  if(M5.Power.isCharging()){
    M5.Display.setFont(nullptr); M5.Display.setTextSize(1);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(CLR_AMBER);
    M5.Display.drawString("CHG",285,16); return;
  }
  M5.Display.drawRoundRect(268,5,36,22,3,CLR_GRAY);
  M5.Display.fillRect(304,11,4,10,CLR_GRAY);
  int fw=max(0,(int)(32.0f*bat/100.0f));
  if(fw>0) M5.Display.fillRect(270,7,fw,18,bc);
}

// 時鐘＋名字（共用：drawTopBar 整列重畫、updateClock 每秒局部重畫都要用同一套定位，
// 否則 updateClock 只清時鐘那塊小範圍會把名字開頭吃掉，看起來像被時間擋住）
void drawClockAndName(){
  M5.Display.fillRect(0,0,190,32,CLR_DARK);
  fXs();
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(CLR_WHITE);
  String tnow=getNowTime();
  M5.Display.drawString(tnow,4,16);
  // 名字接在時間後面（用實際字寬算起點，才不會被時間蓋住），不置中
  int nameX=4+M5.Display.textWidth(tnow)+14;
  M5.Display.setTextColor(CLR_ACCENT);
  M5.Display.setClipRect(nameX,0,184-nameX,32);          // 限制在時間與「更多」鍵之間，過長自動裁掉
  M5.Display.drawString(pilotDisplayName.length()>0?pilotDisplayName:pilotName,nameX,16);
  M5.Display.clearClipRect();
}

void drawTopBar(){
  M5.Display.fillRect(0,0,320,32,CLR_DARK);
  drawClockAndName();
  // 更多：加大按鍵、幾乎佔滿頂列高度，好按
  M5.Display.fillRoundRect(190,1,62,30,6,CLR_SURFACE); M5.Display.drawRoundRect(190,1,62,30,6,CLR_ACCENT);
  fSm(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_WHITE);
  M5.Display.drawString("更多",221,16);
  fXs();
  bool ok=(WiFi.status()==WL_CONNECTED&&wsConnected);
  M5.Display.fillCircle(258,16,4,ok?CLR_GREEN:CLR_RED);
  drawBattery();
}

// ── 鍵盤 ──────────────────────────────────────────────────────────────────────
void drawKeyBtn(int x,int y,int w,int h,String lbl){
  M5.Display.fillRoundRect(x,y,w,h,3,CLR_SURFACE);
  M5.Display.drawRoundRect(x,y,w,h,3,CLR_GRAY);
  M5.Display.setFont(nullptr);
  M5.Display.setTextSize(lbl.length()==1?2:1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(CLR_WHITE);
  M5.Display.drawString(lbl,x+w/2,y+h/2);
}

void drawKeyboard(){
  M5.Display.fillScreen(CLR_BG);
  M5.Display.setTextDatum(middle_center);
  fXs(); M5.Display.setTextColor(CLR_GRAY);
  M5.Display.drawString(kbHint,160,10);
  bool showBack=(kbTarget=="password");
  int boxX=showBack?50:6, boxW=showBack?264:308;
  if(showBack){
    M5.Display.fillRoundRect(4,20,42,28,4,CLR_SURFACE); M5.Display.drawRoundRect(4,20,42,28,4,CLR_GRAY);
    fSm(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_WHITE);
    M5.Display.drawString("<",25,34);
  }
  M5.Display.fillRoundRect(boxX,20,boxW,28,4,CLR_SURFACE);
  M5.Display.drawRoundRect(boxX,20,boxW,28,4,CLR_ACCENT);
  fSm(); M5.Display.setTextColor(CLR_WHITE);
  M5.Display.setTextDatum(middle_left);
  String disp=kbBuffer;
  if(kbTarget=="password"){
    String masked="";
    for(int i=0;i<(int)kbBuffer.length()-1;i++) masked+="*";
    if(kbBuffer.length()>0) masked+=kbBuffer[kbBuffer.length()-1];
    disp=masked;
  }
  M5.Display.drawString(disp.length()>0?disp:"_",boxX+8,34);

  if(kbTarget=="follower"){
    int keys[]={1,2,3,4};
    int kx=10,ky=60,kw=72,kh=72,gap=6;
    for(int i=0;i<4;i++){
      int x=kx+i*(kw+gap);
      M5.Display.fillRoundRect(x,ky,kw,kh,8,CLR_SURFACE);
      M5.Display.drawRoundRect(x,ky,kw,kh,8,CLR_GRAY);
      fLg(); M5.Display.setTextColor(CLR_WHITE);
      M5.Display.setTextDatum(middle_center);
      char c[2]; sprintf(c,"%d",keys[i]);
      M5.Display.drawString(c,x+kw/2,ky+kh/2);
    }
    M5.Display.fillRoundRect(10,144,146,52,8,CLR_RED);
    M5.Display.fillRoundRect(164,144,146,52,8,CLR_GREEN);
    fSm(); M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(CLR_WHITE);
    M5.Display.drawString("CLR",83,170);
    M5.Display.drawString("OK",237,170);
    M5.Display.fillRoundRect(10,204,300,28,8,CLR_SURFACE);
    M5.Display.drawRoundRect(10,204,300,28,8,CLR_GRAY);
    fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_GRAY);
    M5.Display.drawString("< 返回選擇模式",160,218);
    return;
  }

  String rows[4];
  if(kbShift){ rows[0]="QWERTYUIOP"; rows[1]="ASDFGHJKL"; rows[2]="ZXCVBNM"; rows[3]="1234567890"; }
  else { rows[0]="qwertyuiop"; rows[1]="asdfghjkl"; rows[2]="zxcvbnm"; rows[3]="1234567890"; }

  int ky=54,kh=34,gap=3;
  for(int i=0;i<10;i++){ int x=4+i*31; drawKeyBtn(x,ky,28,kh,String(rows[0][i])); }
  for(int i=0;i<9;i++){ int x=20+i*31; drawKeyBtn(x,ky+kh+gap,28,kh,String(rows[1][i])); }
  for(int i=0;i<7;i++){ int x=52+i*31; drawKeyBtn(x,ky+(kh+gap)*2,28,kh,String(rows[2][i])); }
  M5.Display.fillRoundRect(4,ky+(kh+gap)*3,42,kh,4,kbShift?CLR_ACCENT:CLR_SURFACE);
  M5.Display.drawRoundRect(4,ky+(kh+gap)*3,42,kh,4,CLR_GRAY);
  M5.Display.setFont(nullptr); M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kbShift?CLR_BG:CLR_WHITE);
  M5.Display.drawString(kbShift?"ABC":"abc",25,ky+(kh+gap)*3+kh/2);
  for(int i=0;i<10;i++){ int x=50+i*26; drawKeyBtn(x,ky+(kh+gap)*3,24,kh,String(rows[3][i])); }
  int by=ky+(kh+gap)*4+2;
  M5.Display.fillRoundRect(4,by,60,32,4,CLR_RED);
  M5.Display.fillRoundRect(68,by,80,32,4,CLR_SURFACE);
  M5.Display.fillRoundRect(152,by,50,32,4,CLR_SURFACE);
  M5.Display.fillRoundRect(206,by,110,32,4,CLR_GREEN);
  M5.Display.setFont(nullptr); M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(CLR_WHITE);
  M5.Display.drawString("CLR",34,by+16);
  M5.Display.drawString("SPC",108,by+16);
  M5.Display.drawString("DEL",177,by+16);
  M5.Display.drawString("OK",261,by+16);
}

void appendKb(String ch){
  if((int)kbBuffer.length()>=kbMaxLen) return;
  kbBuffer+=ch; drawKeyboard();
}

void showKbError(String msg){
  M5.Display.fillRect(10,208,300,20,CLR_BG);
  M5.Display.setFont(nullptr); M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(CLR_RED);
  M5.Display.drawString(msg,160,218);
}

void onKeyboardConfirm();
void drawModeSelect();

void handleKeyboardTouch(int tx,int ty){
  if(kbTarget=="password"&&tx<46&&ty>=20&&ty<=48){ currentScreen=SCR_WIFI_SCAN; drawWifiList(); return; }
  if(kbTarget=="follower"){
    int keys[]={1,2,3,4};
    int kx=10,ky=60,kw=72,kh=72,gap=6;
    for(int i=0;i<4;i++){
      int x=kx+i*(kw+gap);
      if(tx>=x&&tx<=x+kw&&ty>=ky&&ty<=ky+kh){ if(kbBuffer.length()<4){ kbBuffer+=String(keys[i]); drawKeyboard(); } return; }
    }
    if(ty>=144&&ty<=196){
      if(tx<164){ kbBuffer=""; drawKeyboard(); }
      else { if(kbBuffer.length()==4) onKeyboardConfirm(); else showKbError("請輸入4碼序號"); }
      return;
    }
    if(ty>=204&&ty<=232){ pilotMode=MODE_NONE; currentScreen=SCR_MODE_SELECT; drawModeSelect(); return; }
    return;
  }
  String rows[4];
  if(kbShift){ rows[0]="QWERTYUIOP"; rows[1]="ASDFGHJKL"; rows[2]="ZXCVBNM"; rows[3]="1234567890"; }
  else { rows[0]="qwertyuiop"; rows[1]="asdfghjkl"; rows[2]="zxcvbnm"; rows[3]="1234567890"; }
  int ky=54,kh=34,gap=3;
  for(int i=0;i<10;i++){ int x=4+i*31; if(tx>=x&&tx<=x+28&&ty>=ky&&ty<=ky+kh){ appendKb(String(rows[0][i])); return; } }
  for(int i=0;i<9;i++){ int x=20+i*31; if(tx>=x&&tx<=x+28&&ty>=ky+kh+gap&&ty<=ky+(kh+gap)*2){ appendKb(String(rows[1][i])); return; } }
  for(int i=0;i<7;i++){ int x=52+i*31; if(tx>=x&&tx<=x+28&&ty>=ky+(kh+gap)*2&&ty<=ky+(kh+gap)*3){ appendKb(String(rows[2][i])); return; } }
  if(tx>=4&&tx<=46&&ty>=ky+(kh+gap)*3&&ty<=ky+(kh+gap)*3+kh){ kbShift=!kbShift; drawKeyboard(); return; }
  for(int i=0;i<10;i++){ int x=50+i*26; if(tx>=x&&tx<=x+24&&ty>=ky+(kh+gap)*3&&ty<=ky+(kh+gap)*3+kh){ appendKb(String(rows[3][i])); return; } }
  int by=ky+(kh+gap)*4+2;
  if(ty>=by&&ty<=by+32){
    if(tx<64){ kbBuffer=""; drawKeyboard(); return; }
    if(tx>=68&&tx<148){ appendKb(" "); return; }
    if(tx>=152&&tx<202){ if(kbBuffer.length()>0){ kbBuffer=kbBuffer.substring(0,kbBuffer.length()-1); drawKeyboard(); } return; }
    if(tx>=206){ if(kbBuffer.length()==0){ showKbError("請先輸入內容"); return; } onKeyboardConfirm(); }
  }
}

// ── WiFi 掃描 ──────────────────────────────────────────────────────────────────
void drawConnecting(String msg);
void startWifiScan();

void drawWifiList(){
  M5.Display.fillScreen(CLR_BG);
  fXs(); M5.Display.setTextDatum(middle_center);
  M5.Display.fillRoundRect(4,2,56,24,4,CLR_SURFACE); M5.Display.drawRoundRect(4,2,56,24,4,CLR_RED);
  M5.Display.setTextColor(CLR_RED); M5.Display.drawString("關機",32,14);
  M5.Display.setTextColor(CLR_ACCENT);
  M5.Display.drawString("選擇 WiFi",160,14);
  M5.Display.fillRoundRect(250,2,66,24,4,CLR_SURFACE); M5.Display.drawRoundRect(250,2,66,24,4,CLR_ACCENT);
  M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("重掃",283,14);
  if(wifiCount==0){ M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("找不到WiFi，點擊重新掃描",160,120); return; }
  for(int i=0;i<5;i++){
    int idx=wifiScroll+i; if(idx>=wifiCount) break;
    int y=32+i*38;
    M5.Display.fillRoundRect(4,y,312,34,4,CLR_DARK);
    M5.Display.drawRoundRect(4,y,312,34,4,CLR_GRAY);
    M5.Display.setTextDatum(middle_left); M5.Display.setTextColor(CLR_WHITE);
    M5.Display.drawString(wifiList[idx].ssid,14,y+17);
    int sig=wifiList[idx].rssi;
    uint16_t sc=sig>-50?CLR_GREEN:sig>-70?CLR_AMBER:CLR_RED;
    M5.Display.setTextDatum(middle_right); M5.Display.setTextColor(sc);
    char rb[8]; sprintf(rb,"%ddB",sig);
    M5.Display.drawString(rb,308,y+17);
  }
  M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_GRAY);
  M5.Display.setFont(nullptr); M5.Display.setTextSize(1);
  M5.Display.drawString("上滑/下滑換頁  點擊選擇",160,231);
}

void handleWifiListTouch(int tx,int ty){
  if(ty<28&&tx<62){ drawPoweroffConfirm(); return; }
  if(ty<28&&tx>246){ startWifiScan(); return; }
  if(wifiCount==0){ startWifiScan(); return; }
  if(ty<32||ty>222) return;
  int i=(ty-32)/38, idx=wifiScroll+i;
  if(idx>=wifiCount) return;
  pendingSSID=wifiList[idx].ssid;
  kbBuffer=""; kbHint="輸入密碼: "+pendingSSID; kbTarget="password"; kbShift=false; kbMaxLen=64;
  currentScreen=SCR_WIFI_PASS; drawKeyboard();
}

void startWifiScan(){
  drawConnecting("掃描 WiFi...");
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.disconnect();
  delay(100);
  int n=WiFi.scanNetworks();
  wifiCount=min(n,20);
  for(int i=0;i<wifiCount;i++){ wifiList[i].ssid=WiFi.SSID(i); wifiList[i].rssi=WiFi.RSSI(i); }
  wifiScroll=0; currentScreen=SCR_WIFI_SCAN; drawWifiList();
}

// ── WiFi 連線 ──────────────────────────────────────────────────────────────────
void drawModeSelect();

String wifiFailReason(){
  switch(WiFi.status()){
    case WL_NO_SSID_AVAIL:  return "找不到訊號\n請確認熱點是2.4GHz";
    case WL_CONNECT_FAILED: return "密碼錯誤或連線被拒絕";
    case WL_CONNECTION_LOST:return "連線中斷";
    case WL_DISCONNECTED:   return "逾時無回應\n請確認熱點是2.4GHz";
    default:                return "連線失敗\n狀態碼"+String((int)WiFi.status());
  }
}

void connectWiFiSaved(){
  drawConnecting("WiFi 連線中...");
  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
  int t=0; while(WiFi.status()!=WL_CONNECTED&&t<50){ delay(500); t++; }
  if(WiFi.status()==WL_CONNECTED){
    configTime(TZ_OFFSET*3600,0,NTP_SERVER);
    drawConnecting("NTP 對時...");
    struct tm ti; int nt=0; while(!getLocalTime(&ti)&&nt<20){delay(500);nt++;}
    checkForUpdate();
  } else { drawConnecting(wifiFailReason()); delay(2500); startWifiScan(); }
}

// ── 韌體更新 ──────────────────────────────────────────────────────────────────
// 更新說明字太長會爆框：用縮小字自動換行，最多3行，超過截斷加「…」
void drawNotesWrapped(String txt,int cx,int topY,int lineH,int maxLines,int cpl){
  M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(0.58);
  M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_GRAY);
  int i=0,y=topY,line=0;
  while(i<(int)txt.length() && line<maxLines){
    int j=i,cnt=0;
    while(j<(int)txt.length() && cnt<cpl){
      unsigned char c=txt[j];
      j += (c<0x80)?1:(c<0xE0)?2:(c<0xF0)?3:4;
      cnt++;
    }
    String seg=txt.substring(i,j);
    if(line==maxLines-1 && j<(int)txt.length()) seg+="…";
    M5.Display.drawString(seg,cx,y);
    i=j; y+=lineH; line++;
  }
}
void drawUpdateConfirm(){
  currentScreen=SCR_UPDATE_CONFIRM;
  M5.Display.fillScreen(CLR_BG); M5.Display.setTextDatum(middle_center);
  fLg(); M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("發現新版本",160,58);
  fSm(); M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("v"+String(pendingFwVersion),160,86);
  if(pendingFwNotes.length()>0) drawNotesWrapped(pendingFwNotes,160,110,17,3,20);
  fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_GRAY);
  M5.Display.drawString("是否現在更新？",160,168);
  M5.Display.fillRoundRect(20,186,130,46,10,CLR_SURFACE); M5.Display.drawRoundRect(20,186,130,46,10,CLR_GRAY);
  fSm(); M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("取消",85,209);
  M5.Display.fillRoundRect(170,186,130,46,10,CLR_ACCENT);
  M5.Display.setTextColor(CLR_BG); M5.Display.drawString("更新",235,209);
}

void doFirmwareUpdate(){
  M5.Display.fillScreen(CLR_BG); M5.Display.setTextDatum(middle_center);
  fSm(); M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("更新中，請勿關機...",160,120);
  WiFiClientSecure client; client.setInsecure();
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret=httpUpdate.update(client, pendingFwUrl);
  if(ret==HTTP_UPDATE_FAILED){
    Serial.printf("[OTA] failed (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
    fXs(); M5.Display.setTextColor(CLR_RED); M5.Display.drawString("更新失敗，繼續使用目前版本",160,160);
    delay(2000);
    currentScreen=SCR_MODE_SELECT; drawModeSelect();
  } else if(ret==HTTP_UPDATE_NO_UPDATES){
    currentScreen=SCR_MODE_SELECT; drawModeSelect();
  }
  // HTTP_UPDATE_OK：裝置會自動重開機，不會執行到這裡
}

void checkForUpdate(){
  drawConnecting("檢查更新中...");
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setTimeout(6000);
  bool ok=false;
  if(http.begin(client, UPDATE_CHECK_URL)){
    int code=http.GET();
    if(code==200){
      String body=http.getString();
      StaticJsonDocument<256> doc;
      if(!deserializeJson(doc,body)){
        int remoteVer=doc["version"]|0;
        String url=doc["url"]|"";
        String notes=doc["notes"]|"";
        if(remoteVer>FW_VERSION && url.length()>0){
          pendingFwVersion=remoteVer; pendingFwUrl=url; pendingFwNotes=notes;
          ok=true;
        }
      }
    }
    http.end();
  }
  if(ok) drawUpdateConfirm();
  else { currentScreen=SCR_MODE_SELECT; drawModeSelect(); }
}

// ── 模式選擇 ──────────────────────────────────────────────────────────────────
void connectWebSocket();
void drawFollowerInput();

void drawModeSelect(){
  M5.Display.fillScreen(CLR_BG);
  M5.Display.setFont(nullptr); M5.Display.setTextSize(2);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(CLR_ACCENT);
  M5.Display.drawString("DroneATIS",150,24);
  M5.Display.setFont(nullptr); M5.Display.setTextSize(1); M5.Display.setTextColor(CLR_GRAY);
  M5.Display.drawString(pilotName,150,46);
  // 關機鍵（右上角）：原本太小很難點準，加大範圍
  M5.Display.fillRoundRect(220,2,98,38,8,CLR_SURFACE); M5.Display.drawRoundRect(220,2,98,38,8,CLR_RED);
  fSm(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_RED);
  M5.Display.drawString("關機",269,21);
  // WiFi鍵（左上角）：連不上網路、卡在這頁選不了模式時，不用先硬選一個模式才能改 WiFi；同樣加大
  M5.Display.fillRoundRect(2,2,86,38,8,CLR_SURFACE); M5.Display.drawRoundRect(2,2,86,38,8,CLR_ACCENT);
  M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("WiFi",45,21);
  // 說明文字用縮小字避免爆框
  #define MSSUB(txt,cy,col) do{ M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(0.62); M5.Display.setTextColor(col); M5.Display.drawString(txt,160,cy); }while(0)
  // 主控模式
  M5.Display.fillRoundRect(16,62,288,56,10,CLR_SURFACE);
  M5.Display.drawRoundRect(16,62,288,56,10,CLR_GREEN);
  fSm(); M5.Display.setTextColor(CLR_GREEN); M5.Display.drawString("主控模式",160,82);
  MSSUB("獨立連線，回應塔台",104,CLR_GRAY);
  // 跟隨模式
  M5.Display.fillRoundRect(16,124,288,56,10,CLR_SURFACE);
  M5.Display.drawRoundRect(16,124,288,56,10,CLR_ACCENT);
  fSm(); M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("跟隨模式",160,144);
  MSSUB("只看畫面，不回應",166,CLR_GRAY);
  // 飛聚跟隨模式
  M5.Display.fillRoundRect(16,186,288,52,10,CLR_SURFACE);
  M5.Display.drawRoundRect(16,186,288,52,10,CLR_AMBER);
  fSm(); M5.Display.setTextColor(CLR_AMBER); M5.Display.drawString("飛聚跟隨模式",160,205);
  MSSUB("需回報，回報給主控",225,CLR_GRAY);
  #undef MSSUB
}

// ── 序號畫面 ──────────────────────────────────────────────────────────────────
void drawShowCode(){
  M5.Display.fillScreen(CLR_BG);
  M5.Display.setFont(nullptr); M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(CLR_ACCENT);
  M5.Display.drawString("DroneATIS",160,14);
  fXs(); M5.Display.setTextColor(CLR_GRAY);
  M5.Display.drawString("請將序號告知塔台人員",160,36);
  fSm(); M5.Display.setTextColor(CLR_WHITE);
  M5.Display.drawString(pilotName,160,58);
  fLg(); M5.Display.setTextColor(CLR_ACCENT);
  M5.Display.drawString(roomCode,160,110);
  fXs(); M5.Display.setTextColor(CLR_GRAY);
  M5.Display.drawString("序號由數字 1-4 組成",160,152);
  M5.Display.drawString("有效 24 小時",160,174);
  M5.Display.drawString("等待塔台連線...",160,200);
  // 這個畫面通常要等塔台加入，時間可能不短，之前完全沒有關機入口，只能硬拔電池
  M5.Display.fillRoundRect(110,214,100,22,6,CLR_SURFACE); M5.Display.drawRoundRect(110,214,100,22,6,CLR_RED);
  M5.Display.setTextColor(CLR_RED); M5.Display.drawString("關機",160,225);
}

void drawFollowerInput(){
  kbBuffer=""; kbHint="輸入主控序號（4碼，數字1-4）";
  kbTarget="follower"; kbMaxLen=4;
  currentScreen=SCR_FOLLOWER_CODE; drawKeyboard();
}

// ── WebSocket 連線 ─────────────────────────────────────────────────────────────
void sendRegister();
void sendFollowerRegister();
void drawIdle();
void drawNotamList();

void webSocketEvent(WStype_t wsType, uint8_t* payload, size_t length);

void wakeScreen(){
  lastActivity=millis();
  if(screenDimmed){ M5.Display.setBrightness(BRIGHT_VAL[brightnessLevel]); screenDimmed=false; }
}

void connectWebSocket(){
  drawConnecting("連接塔台...");
  Serial.println("[WS] connectWebSocket() called");
  Serial.print("[WS] Host: "); Serial.println(SERVER_HOST);
  Serial.print("[WS] WiFi IP: "); Serial.println(WiFi.localIP());

  IPAddress resolvedIP;
  if(WiFi.hostByName(SERVER_HOST, resolvedIP)){
    Serial.print("[WS] DNS OK -> "); Serial.println(resolvedIP);
  } else {
    Serial.println("[WS] DNS FAILED");
  }

  wsClient.onEvent(webSocketEvent);
  wsClient.begin(SERVER_HOST, SERVER_PORT, "/");                    // TCP proxy 明文連線
  wsClient.setExtraHeaders("Host: droneatis-production.up.railway.app");
  wsClient.enableHeartbeat(15000, 3000, 2);
  wsClient.setReconnectInterval(5000);
}

void connectWebSocketFollower(){
  drawConnecting("連接主控...");
  Serial.println("[WS] connectWebSocketFollower() called");
  Serial.print("[WS] WiFi IP: "); Serial.println(WiFi.localIP());

  IPAddress resolvedIP;
  if(WiFi.hostByName(SERVER_HOST, resolvedIP)){
    Serial.print("[WS] DNS OK -> "); Serial.println(resolvedIP);
  } else {
    Serial.println("[WS] DNS FAILED");
  }

  wsClient.onEvent(webSocketEvent);
  wsClient.begin(SERVER_HOST, SERVER_PORT, "/");
  wsClient.setExtraHeaders("Host: droneatis-production.up.railway.app");
  wsClient.enableHeartbeat(15000, 3000, 2);
  wsClient.setReconnectInterval(5000);
}

void sendRegister(){
  notamCode=""; rwyDir=""; groupName="";
  StaticJsonDocument<128> doc;
  doc["type"]="pilot_register"; doc["name"]=pilotName;
  doc["battery"]=getBattery(); doc["gps"]=false;
  String o; serializeJson(doc,o); wsClient.sendTXT(o);
}

void sendFollowerRegister(){
  StaticJsonDocument<128> doc;
  doc["type"]="follower_register"; doc["name"]=pilotName; doc["masterCode"]=masterCode;
  doc["gather"]=(pilotMode==MODE_GATHER);
  String o; serializeJson(doc,o); wsClient.sendTXT(o);
}

void updateGpsReading(){
  while(Serial2.available()) gps.encode(Serial2.read());
  gpsFixed = gps.location.isValid() && gps.location.age()<3000;
  if(gpsFixed){ gpsLat=gps.location.lat(); gpsLng=gps.location.lng(); }
}

void sendHeartbeat(){
  if(!wsConnected) return;
  StaticJsonDocument<192> doc;
  doc["type"]="pilot_update"; doc["battery"]=getBattery();
  doc["wifi"]=(WiFi.status()==WL_CONNECTED);
  if(!gpsEnabled) doc["gps"]=false;
  else if(gpsFixed){ doc["gps"]=true; doc["lat"]=gpsLat; doc["lng"]=gpsLng; }
  else doc["gps"]="searching";
  String o; serializeJson(doc,o); wsClient.sendTXT(o);
}

void sendAck(String ackType){
  if(pilotMode!=MODE_MASTER) return;
  StaticJsonDocument<128> doc;
  doc["type"]="pilot_ack"; doc["ackType"]=ackType; doc["notamIndex"]=activeNotamIdx;
  String o; serializeJson(doc,o); wsClient.sendTXT(o);
}

// 飛聚跟隨模式：回報給主控者（伺服器再轉給主控），不進塔台
void sendGatherConfirm(String stage){
  if(pilotMode!=MODE_GATHER) return;
  StaticJsonDocument<96> doc;
  doc["type"]="follower_confirm"; doc["stage"]=stage;
  String o; serializeJson(doc,o); wsClient.sendTXT(o);
}

// 統一回報入口：主控 → 塔台；飛聚跟隨 → 主控者
void submitAck(String stage){
  if(pilotMode==MODE_MASTER) sendAck(stage);
  else if(pilotMode==MODE_GATHER) sendGatherConfirm(stage);
}

void sendEndSession(){
  if(IS_FOLLOWER_CONN) return;
  StaticJsonDocument<64> doc; doc["type"]="pilot_end_session"; doc["pilotName"]=pilotName;
  String o; serializeJson(doc,o); wsClient.sendTXT(o);
}

// ── WS 事件 ───────────────────────────────────────────────────────────────────
void drawCommand();
void drawMessage();
void buzz(int,int);
void beep2();
void beep3();

void webSocketEvent(WStype_t wsType, uint8_t* payload, size_t length){
  switch(wsType){
    case WStype_DISCONNECTED:
      Serial.println("[WS] DISCONNECTED");
      wsConnected=false; towerConnected=false;
      wsConnecting=false;
      break;
    case WStype_CONNECTED:
      Serial.println("[WS] CONNECTED!");
      wsConnected=true;
      wsConnecting=false;
      if(pilotMode==MODE_MASTER) sendRegister();
      else if(IS_FOLLOWER_CONN) sendFollowerRegister();
      break;
    case WStype_TEXT:
      Serial.print("[WS] TEXT: "); Serial.println((char*)payload);
      {
      StaticJsonDocument<1024> doc; // 加大過：多 NOTAM 時 tower_connected 會帶 notams 陣列，512 常常不夠
      if(deserializeJson(doc,(char*)payload)) return;
      String type=doc["type"].as<String>();

      if(type=="registered"){
        clientId=doc["clientId"].as<String>(); roomCode=doc["roomCode"].as<String>();
        bool isReconnect=doc["reconnect"]|false;
        if(isReconnect && towerConnected){
          // 重連且之前已有塔台配對，保持當前畫面，等待 tower_connected 訊息
          // 不跳回 SCR_CODE
        } else {
          currentScreen=SCR_CODE; drawShowCode(); beep2();
        }
      }
      else if(type=="follower_registered"){
        towerConnected=true;
        groupName=doc["groupName"]|""; towerName=doc["towerName"]|"塔台"; towerType=doc["towerType"]|"南塔";
        notamCode=doc["notam"]|""; rwyDir=doc["rwy"]|"";
        // 加入時同步主控目前的狀態/降落時間/訊息，避免跟隨端一直卡在開機的預設值
        currentStatus=doc["status"]|"開機預備";
        landingReason="";
        JsonVariant lt=doc["landingTime"];
        if(lt.isNull()||lt.as<String>()=="null"||lt.as<String>()=="") landingTimeStr="";
        else {
          String raw=lt.as<String>();
          if(raw.length()>=4&&isDigit(raw[0])&&isDigit(raw[1])&&isDigit(raw[2])&&isDigit(raw[3])){
            landingTimeStr=raw.substring(0,4);
            if(raw.length()>4) landingReason=raw.substring(4);
          } else landingTimeStr=raw;
        }
        immEndSec=-1;
        bool landDone=(doc["landDone"]|false); // 主控已回報降落完成 → 直接看「降落＋時間」，不進回報流程
        landState=(currentStatus=="降落"&&!landDone)?LAND_WAIT_ACK:LAND_NONE;
        lastMessageTime=doc["lastMessageTime"]|"";  // 上次塔台來訊時間（指令或訊息）
        String lct=doc["lastCommType"]|"status";
        if(lct=="message"){ lastMessage=doc["lastMessage"]|""; showingMessage=(lastMessage.length()>0); }
        else { showingMessage=false; }
        everReceivedCommand=(doc["hasCommand"]|false);
        // 飛聚跟隨模式：加入時若已有塔台指令/訊息，一律要求回報一次，直接停在指令畫面
        if(!landDone && pilotMode==MODE_GATHER&&(everReceivedCommand||showingMessage)){
          ackPending=true; ackReceivedAt=millis(); ackDeadline=millis()+30000; buzzPhase=0;
          currentScreen=SCR_COMMAND;
          if(showingMessage) drawMessage(); else drawCommand();
          beep3();
        } else {
          currentScreen=SCR_IDLE; drawIdle(); beep3();
        }
      }
      else if(type=="tower_connected"){
        towerConnected=true;
        groupName=doc["groupName"]|""; towerName=doc["towerName"]|"塔台"; towerType=doc["towerType"]|"南塔";
        // 斷線重連（WiFi掉線/重開機）沿用伺服器記住的飛航公告/跑道，不要被重設成空白
        if(doc.containsKey("notam")) notamCode=doc["notam"]|"";
        if(doc.containsKey("rwy")) rwyDir=doc["rwy"]|"";
        // 重連時如果伺服器記得不只1個 NOTAM，把「背景」那幾個 slot 的代碼/狀態復原給清單顯示用。
        // 目前正在看的那個 slot 完全不動——它的全域變數（含 landState）斷線期間本來就沒被清掉，
        // 保留著才能讓下面「斷線當下正在降落流程」的保護邏輯正常運作，這裡沒必要也不該去蓋掉它
        if(doc.containsKey("notams")){
          JsonArray arr=doc["notams"].as<JsonArray>();
          int n=arr.size();
          if(n>1){
            if(n>3)n=3;
            notamCount=n;
            int i=0;
            for(JsonObject o:arr){
              if(i>=3) break;
              if(i!=activeNotamIdx){
                notamSlots[i].code=o["code"]|""; notamSlots[i].status=o["status"]|"開機預備";
                notamSlots[i].rwy=o["rwy"]|""; notamSlots[i].groupName=o["groupName"]|"";
              }
              i++;
            }
            if(activeNotamIdx>=notamCount) activeNotamIdx=0;
          }
        }
        // 如果斷線當下正在降落流程（還沒按收到降落指令，或已經在倒數還沒回報完成），
        // 重連不能硬切回 SCR_IDLE——那個畫面沒有降落回報按鈕，飛手會卡住回報不了
        if(landState==LAND_WAIT_ACK||landState==LAND_COUNTDOWN){ currentScreen=SCR_COMMAND; drawCommand(); }
        else { currentScreen=SCR_IDLE; drawIdle(); }
        beep3();
      }
      else if(type=="tower_info"){ // 塔台改南北塔或名字
        towerName=doc["towerName"]|towerName; towerType=doc["towerType"]|towerType;
        if(currentScreen==SCR_IDLE) drawIdle();
      }
      else if(type=="name_update"){ // 手機「主控模式輔助」設定的顯示名字（只換畫面顯示，不動 pilotName 本身）
        pilotDisplayName=doc["name"]|"";
        saveDisplayName(pilotDisplayName); // 記住，下次開機就直接用這個名字
        if(currentScreen==SCR_IDLE||currentScreen==SCR_COMMAND) updateClock();
      }
      else if(type=="command"||type=="follower_sync"){
        // 多 NOTAM（只有主控才有這個概念）：指令是給「目前沒在看」的那個 NOTAM 時，
        // 直接切過去顯示那一則，不要只是靜靜震動——否則塔台不知道飛手畫面正停在哪個 NOTAM，
        // 送指令會像沒反應一樣；切換前會先把目前這份存回原本的 slot，不會遺失
        if(pilotMode==MODE_MASTER){
          if(doc.containsKey("notamCount")) notamCount=constrain((int)doc["notamCount"],1,3);
          int ni=doc["notamIndex"]|0;
          if(ni>=0&&ni<3&&doc.containsKey("notamCode")) notamSlots[ni].code=doc["notamCode"].as<String>();
          if(ni!=activeNotamIdx && ni>=0 && ni<notamCount) switchToNotamSlot(ni);
        }
        showingMessage=false;
        everReceivedCommand=true;
        currentStatus=doc["status"].as<String>();
        bool immediateLand=doc["immediate"]|false;
        JsonVariant lt=doc["landingTime"];
        landingReason="";
        if(immediateLand){
          // 馬上降落：從收到指令當下起算，剛好倒數 1 分鐘
          immEndSec=(getNowTotalSecs()+60)%86400;
          char buf[5]; sprintf(buf,"%02d%02d",immEndSec/3600,(immEndSec/60)%60);
          landingTimeStr=String(buf);
        }
        else if(lt.isNull()||lt.as<String>()=="null"||lt.as<String>()=="") { landingTimeStr=""; immEndSec=-1; }
        else {
          immEndSec=-1;
          String raw=lt.as<String>();
          if(raw.length()>=4&&isDigit(raw[0])&&isDigit(raw[1])&&isDigit(raw[2])&&isDigit(raw[3])){
            landingTimeStr=raw.substring(0,4);
            if(raw.length()>4) landingReason=raw.substring(4);
          } else landingTimeStr=raw;
        }
        groupName=doc["groupName"]|groupName;
        // 塔台這次來訊的時間（指令也要顯示，跟 line 一樣）；沒帶就用本機時鐘
        { String st=doc["time"]|""; if(st.length()==0) st=getNowTime(); if(st=="--:--") st=""; lastMessageTime=st; }
        landState=(currentStatus=="降落")?LAND_WAIT_ACK:LAND_NONE;
        if(NEEDS_ACK){ ackPending=true; ackReceivedAt=millis(); ackDeadline=millis()+30000; buzzPhase=0; }
        wakeScreen();
        currentScreen=SCR_COMMAND; drawCommand(); buzz(1000,300);
      }
      else if(type=="message"){
        if(pilotMode==MODE_MASTER){
          if(doc.containsKey("notamCount")) notamCount=constrain((int)doc["notamCount"],1,3);
          int ni=doc["notamIndex"]|0;
          if(ni>=0&&ni<3&&doc.containsKey("notamCode")) notamSlots[ni].code=doc["notamCode"].as<String>();
          if(ni!=activeNotamIdx && ni>=0 && ni<notamCount) switchToNotamSlot(ni);
        }
        showingMessage=true;
        everReceivedCommand=true;
        lastMessage=doc["message"].as<String>(); landState=LAND_NONE;
        // 發送時間：優先用塔台送來的，沒有就用本機時鐘（訊息剛到，兩者幾乎一樣）
        { String st=doc["time"]|""; if(st.length()==0) st=getNowTime(); if(st=="--:--") st=""; lastMessageTime=st; }
        if(NEEDS_ACK){ ackPending=true; ackReceivedAt=millis(); ackDeadline=millis()+30000; buzzPhase=0; }
        wakeScreen();
        currentScreen=SCR_COMMAND; drawMessage(); buzz(880,200);
      }
      else if(type=="group_update"){
        if(doc.containsKey("towerName")) towerName=doc["towerName"].as<String>();
        int gni=doc["notamIndex"]|0;
        // 多 NOTAM 時，分類現在是每筆各自的，不是給「目前正在看」那筆才更新背景那幾筆的畫面資料；
        // 背景那筆只更新資料不用重畫，除非剛好停在清單畫面上（那樣才會被看到）
        if(pilotMode==MODE_MASTER && notamCount>1 && gni>=0 && gni<notamCount && gni!=activeNotamIdx){
          notamSlots[gni].groupName=doc["groupName"]|"";
          if(currentScreen==SCR_NOTAM_LIST) drawNotamList();
        } else {
          groupName=doc["groupName"]|"";
          if(currentScreen==SCR_IDLE) drawIdle();
          else if(currentScreen==SCR_NOTAM_LIST) drawNotamList();
        }
      }
      else if(type=="rwy_update"){
        int rni=doc["notamIndex"]|0;
        if(pilotMode==MODE_MASTER && notamCount>1 && rni>=0 && rni<notamCount && rni!=activeNotamIdx){
          notamSlots[rni].rwy=doc["rwy"]|"";
          if(currentScreen==SCR_NOTAM_LIST) drawNotamList();
        } else {
          rwyDir=doc["rwy"]|"";
          if(currentScreen==SCR_IDLE){
            drawIdle();
            M5.Display.fillRect(20,130,280,40,CLR_SURFACE);
            M5.Display.drawRect(20,130,280,40,CLR_AMBER);
            fSm(); M5.Display.setTextDatum(middle_center);
            M5.Display.setTextColor(CLR_AMBER);
            M5.Display.drawString("跑道改"+rwyDir,160,150);
            rwyNoticeUntil=millis()+5000;
          } else if(currentScreen==SCR_NOTAM_LIST){
            drawNotamList();
          }
        }
      }
      else if(type=="notam_update"){
        notamCode=doc["notam"]|"";
        if(currentScreen==SCR_IDLE) drawIdle();
      }
      else if(type=="ack_overdue"){ if(NEEDS_ACK) buzzPhase=2; }
      else if(type=="follower_ack_sync"){
        // 飛聚跟隨模式自己管理回報狀態，忽略主控的回報同步
        if(pilotMode!=MODE_GATHER){
          String ackType=doc["ackType"]|"ack";
          Serial.println("[FOLLOWER] ack_sync received, ackType="+ackType);
          if(ackType=="ack"||ackType=="takeoff"){ landState=LAND_NONE; currentScreen=SCR_IDLE; drawIdle(); }
          else if(ackType=="landing_ack"){ landState=LAND_COUNTDOWN; drawCommand(); }
          else if(ackType=="landing_done"){ landState=LAND_NONE; currentScreen=SCR_IDLE; drawIdle(); }
        }
      }
      else if(type=="follower_error"){ drawConnecting("序號無效或已過期"); delay(2000); drawFollowerInput(); }
      else if(type=="follower_confirm"){
        String fname=doc["followerName"]|"跟隨者";
        String st=doc["stage"]|"";
        String stTxt=(st=="landing_ack")?" 收到降落指令":(st=="landing_done")?" 已降落完成":(st=="takeoff")?" 已收到(起飛)":" 已確認收到";
        if(currentScreen==SCR_IDLE){
          M5.Display.fillRect(20,200,280,26,CLR_SURFACE); fXs(); M5.Display.setTextDatum(middle_center);
          M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString(fname+stTxt,160,213);
          followerConfirmUntil=millis()+10000; // 顯示10秒後自動清掉
        }
        buzz(1000,80);
      }
      break;
    }
    case WStype_ERROR:
      Serial.print("[WS] ERROR: "); Serial.println((char*)payload);
      break;
    case WStype_PING:
      Serial.println("[WS] PING received");
      break;
    case WStype_PONG:
      Serial.println("[WS] PONG received");
      break;
    default:
      Serial.print("[WS] unknown event type: "); Serial.println((int)wsType);
      break;
  }
}

// ── 蜂鳴器 ───────────────────────────────────────────────────────────────────
void buzz(int freq,int ms){ M5.Speaker.tone(freq,ms); }
void beep2(){ buzz(880,100);delay(150);buzz(1100,100); }
void beep3(){ buzz(880,100);delay(100);buzz(1100,100);delay(100);buzz(1320,150); }

void handleBuzzer(unsigned long now){
  if(pilotMode==MODE_FOLLOWER) return;
  if(landState==LAND_COUNTDOWN){
    int diff=landDiffSec();
    if(diff<=0&&now-lastBuzzAt>1500){buzz(800,1200);lastBuzzAt=now;}
    else if(diff<=60&&diff>10&&!landing60Fired){ landing60Fired=true; buzz(900,200); } // 剩1分鐘提醒一次
    else if(diff<=10&&diff>0&&diff!=landingLastAlertSec){ landingLastAlertSec=diff; buzz(1000,150); } // 剩10秒內每秒叫一次
    return;
  }
  landing60Fired=false; landingLastAlertSec=-1; // 不在倒數狀態，重置，下一輪降落才會重新提醒
  if(!ackPending){buzzPhase=0;return;}
  unsigned long e=now-ackReceivedAt;
  if(e>10000&&e<30000&&buzzPhase<1) buzzPhase=1;
  if(e>=30000&&buzzPhase<2) buzzPhase=2;
  if(buzzPhase==1&&now-lastBuzzAt>1000){buzz(1200,200);lastBuzzAt=now;}
  if(buzzPhase==2&&now-lastBuzzAt>1500){buzz(800,1200);lastBuzzAt=now;}
}

// ── 畫面函數 ──────────────────────────────────────────────────────────────────
void drawConnecting(String msg){
  M5.Display.fillScreen(CLR_BG); M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(nullptr); M5.Display.setTextSize(2);
  M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("DroneATIS",160,68);
  fSm(); M5.Display.setTextColor(CLR_AMBER);
  int nl=msg.indexOf('\n');
  if(nl>=0){
    M5.Display.drawString(msg.substring(0,nl),160,110);
    M5.Display.setTextColor(CLR_GRAY);
    M5.Display.drawString(msg.substring(nl+1),160,140);
  } else {
    M5.Display.drawString(msg,160,118);
  }
}

void drawCharging(){
  M5.Display.fillScreen(CLR_BG); M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(nullptr); M5.Display.setTextSize(2);
  M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("DroneATIS",160,55);
  int bat=getBattery(); uint16_t bc=bat>50?CLR_GREEN:bat>20?CLR_AMBER:CLR_RED;
  fLg(); M5.Display.setTextColor(bc);
  char buf[8]; sprintf(buf,"%d%%",bat); M5.Display.drawString(buf,160,118);
  M5.Display.drawRoundRect(116,150,88,32,4,CLR_GRAY); M5.Display.fillRect(204,160,6,12,CLR_GRAY);
  int fw=max(0,(int)(82.0f*bat/100.0f)); M5.Display.fillRoundRect(118,152,fw,28,3,bc);
  fXs(); M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("充電中 · 按任意鍵開機",160,198);
}

void drawGpsBtn(){
  uint16_t fc=!gpsEnabled?CLR_GRAY:(!gpsFixed?CLR_WHITE:CLR_GREEN);
  M5.Display.fillRoundRect(232,34,82,22,4,CLR_SURFACE); M5.Display.drawRoundRect(232,34,82,22,4,fc);
  fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(fc);
  M5.Display.drawString("GPS",273,45);
}

// NOTAM 清單（2個以上才會顯示）：每一列是一個 NOTAM，點列進去看/操作那個 NOTAM 的待命或指令畫面，
// 點 ✕ 刪除（至少留1個），最下面沒滿3個時有「新增」列
void drawNotamList(){
  currentScreen=SCR_NOTAM_LIST;
  M5.Display.fillScreen(CLR_BG); drawTopBar();
  bool ok=towerConnected&&wsConnected;
  fXs(); M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(ok?CLR_GREEN:CLR_RED);
  M5.Display.drawString(ok?("● "+towerType+" "+towerName):"● 無連線",8,42);
  int y=54, rh=54, gap=6;
  for(int i=0;i<notamCount;i++){
    NotamRowView v=notamRowView(i);
    uint16_t bd=CLR_GRAY;
    String tag="";
    if(v.ackPending){ bd=CLR_AMBER; tag="待回應"; }
    else if(v.landState==LAND_COUNTDOWN){ bd=CLR_AMBER; tag="降落倒數"; }
    else if(v.landState==LAND_WAIT_ACK){ bd=CLR_AMBER; tag="收到降落"; }
    M5.Display.fillRoundRect(8,y,270,rh,8,CLR_SURFACE); M5.Display.drawRoundRect(8,y,270,rh,8,bd);
    fSm(); M5.Display.setTextDatum(middle_left); M5.Display.setTextColor(CLR_AMBER);
    M5.Display.drawString(v.code.length()?v.code:("NOTAM"+String(i+1)),16,y+14);
    fXs(); M5.Display.setTextColor(CLR_WHITE);
    String statusText=v.showingMessage?v.lastMessage:v.statusText;
    M5.Display.drawString(statusText,16,y+30);
    // 塔台如果有幫這筆 NOTAM 設跑道/分類，放右邊、上下排列（不要跟代碼/狀態同一行擠成一長串）：
    // 回應標籤在右上，跑道再下面一行，分類最下面一行；框加高一點才擺得下這3行，字不會疊在一起
    M5.Display.setTextDatum(middle_right);
    if(tag.length()){ M5.Display.setTextColor(bd); M5.Display.drawString(tag,272,y+12); }
    if(v.rwy.length()){ M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString(v.rwy,272,y+28); }
    if(v.groupName.length()){ M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("["+v.groupName+"]",272,y+44); }
    M5.Display.fillRoundRect(284,y,28,rh,6,CLR_SURFACE); M5.Display.drawRoundRect(284,y,28,rh,6,CLR_RED);
    fSm(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_RED); M5.Display.drawString("X",298,y+rh/2);
    y+=rh+gap;
  }
  if(notamCount<3){
    M5.Display.fillRoundRect(8,y,304,36,8,CLR_SURFACE); M5.Display.drawRoundRect(8,y,304,36,8,CLR_ACCENT);
    fSm(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_ACCENT);
    M5.Display.drawString("+ 新增 NOTAM",160,y+18);
  }
}

void goToNotamDetail(int idx){
  switchToNotamSlot(idx);
  if(everReceivedCommand){ currentScreen=SCR_COMMAND; drawCommand(); }
  else { currentScreen=SCR_IDLE; drawIdle(); }
}

int pendingDeleteNotamIdx=-1;
void drawNotamDeleteConfirm(int idx){
  currentScreen=SCR_NOTAM_DELETE_CONFIRM; pendingDeleteNotamIdx=idx;
  NotamRowView v=notamRowView(idx);
  M5.Display.fillRect(0,172,320,68,CLR_SURFACE); M5.Display.drawRect(0,172,320,68,CLR_RED);
  fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_WHITE);
  M5.Display.drawString("刪除 "+(v.code.length()?v.code:("NOTAM"+String(idx+1)))+"？",160,182);
  M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("該 NOTAM 的資料會清掉",160,198);
  M5.Display.fillRoundRect(10,204,140,28,8,CLR_SURFACE); M5.Display.drawRoundRect(10,204,140,28,8,CLR_GRAY);
  M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("取消",80,218);
  M5.Display.fillRoundRect(170,204,140,28,8,CLR_RED);
  M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("刪除",240,218);
}

void handleNotamListTouch(int tx,int ty){
  int y=54, rh=54, gap=6;
  for(int i=0;i<notamCount;i++){
    if(ty>=y&&ty<=y+rh){
      if(tx>=284&&tx<=312){ if(notamCount>1) drawNotamDeleteConfirm(i); return; }
      if(tx>=8&&tx<=278){ goToNotamDetail(i); return; }
      return;
    }
    y+=rh+gap;
  }
  if(notamCount<3 && ty>=y&&ty<=y+36&&tx>=8&&tx<=312){
    // 新增 NOTAM：跟編輯共用同一個公告鍵盤，OK 確認時用 notamAddMode 分辨
    notamHadValue=false; notamAddMode=true; keypadMode=KP_NOTAM; keypadBuffer="";
    drawKeypad();
  }
}
void handleNotamDeleteConfirmTouch(int tx,int ty){
  if(pendingDeleteNotamIdx<0) return;
  if(tx<160){ pendingDeleteNotamIdx=-1; drawNotamList(); return; }
  int idx=pendingDeleteNotamIdx; pendingDeleteNotamIdx=-1;
  StaticJsonDocument<64> doc; doc["type"]="pilot_notam_manage"; doc["action"]="remove"; doc["index"]=idx;
  String o; serializeJson(doc,o); wsClient.sendTXT(o);
  // 本機也同步砍掉，索引比被刪的大的都要往前移一位
  for(int i=idx;i<notamCount-1;i++) notamSlots[i]=notamSlots[i+1];
  notamCount--;
  if(activeNotamIdx==idx) activeNotamIdx=0;
  else if(activeNotamIdx>idx) activeNotamIdx--;
  switchToNotamSlot(activeNotamIdx); // 重新把全域變數對齊到（可能變動過的）activeNotamIdx
  beep2();
  // drawIdle() 本身不會設定 currentScreen（一直都是靠呼叫端先設好），這裡漏掉的話 currentScreen
  // 會停在 SCR_NOTAM_DELETE_CONFIRM，之後所有觸控都會被導去已經失效的刪除確認畫面處理常式，
  // 導致畫面看起來對了但整個點不動（包含新增 NOTAM 的「+」鍵）
  currentScreen=SCR_IDLE;
  drawIdle(); // notamCount 若已經回到1，drawIdle() 會自己判斷改顯示原本的單一 NOTAM 畫面；還有2個以上會轉去清單
}

void drawIdle(){
  // 2個以上 NOTAM 時，「回到待命畫面」改成回到 NOTAM 清單，不是回到單一 NOTAM 的畫面——
  // 這樣不用去改每一個原本呼叫 drawIdle() 的地方（更多選單返回、改名返回、WiFi取消…都一樣直接受惠）
  if(pilotMode==MODE_MASTER && notamCount>1){ drawNotamList(); return; }
  M5.Display.fillScreen(CLR_BG); drawTopBar();

  bool ok=towerConnected&&wsConnected;
  fSm(); M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(ok?CLR_GREEN:CLR_RED);
  M5.Display.drawString(ok?("● "+towerType+" "+towerName):"● 無連線",8,46);

  if(pilotMode==MODE_MASTER) drawGpsBtn();

  // 公告框（觸控 y:68~88）；原本跟上面塔台連線狀態那行貼太近，往下挪一點
  M5.Display.fillRoundRect(6,68,208,20,4,CLR_SURFACE); M5.Display.drawRoundRect(6,68,208,20,4,0x4228);
  fXs(); M5.Display.setTextDatum(middle_left);
  if(notamCode.length()>0){ M5.Display.setTextColor(CLR_AMBER); M5.Display.drawString("飛航公告: "+notamCode,10,78); }
  else if(pilotMode==MODE_MASTER){ M5.Display.setTextColor(0x5AEB); M5.Display.drawString("點擊輸入飛航公告",10,78); }
  else { M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("飛航公告",10,78); }
  // 「+」新增一個 NOTAM（最多3個）；只有主控看得到，滿3個就不顯示
  if(pilotMode==MODE_MASTER && notamCount<3){
    M5.Display.fillRoundRect(218,68,26,20,4,CLR_SURFACE); M5.Display.drawRoundRect(218,68,26,20,4,CLR_ACCENT);
    M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("+",231,78);
  }

  if(rwyDir.length()>0){
    fXs(); M5.Display.setTextDatum(middle_right); M5.Display.setTextColor(CLR_WHITE);
    M5.Display.drawString(rwyDir,316,78);
  }
  if(groupName.length()>0){
    fXs(); M5.Display.setTextDatum(middle_left); M5.Display.setTextColor(CLR_ACCENT);
    M5.Display.drawString("["+groupName+"]",8,106);
  }

  uint16_t sc=CLR_WHITE;
  if(currentStatus=="可以起飛") sc=CLR_GREEN;
  else if(currentStatus=="降落") sc=CLR_AMBER;

  bool hasReason=(currentStatus=="降落"&&landingTimeStr.length()>0&&landingReason.length()>0);
  M5.Display.setTextDatum(middle_center);

  // 塔台來訊時間放在內容上方（跟 line 一樣）
  if(everReceivedCommand && lastMessageTime.length()>0){
    fXs(); M5.Display.setTextColor(CLR_GRAY);
    M5.Display.drawString("塔台 "+lastMessageTime+" 來訊",160,128);
  }

  // 手動訊息跟選單狀態不同時顯示，看哪個是最新收到的
  if(showingMessage&&lastMessage.length()>0){
    drawFitText(lastMessage,160,152,CLR_WHITE);
  } else if(!everReceivedCommand){
    fSm(); M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("等待塔台來訊",160,150);
  } else if(hasReason){
    // 只留「降落＋時間」，字放大
    M5.Display.setTextColor(sc);
    M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(1.5);
    M5.Display.drawString("降落 "+getLandTimeDisplay(),160,148);
    fXs(); M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString(landingReason,160,180);
  } else if(currentStatus=="降落"&&landingTimeStr.length()>0){
    M5.Display.setTextColor(sc);
    M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(1.5);
    M5.Display.drawString("降落 "+getLandTimeDisplay(),160,152);
  } else {
    M5.Display.setTextColor(sc);
    fLg(); M5.Display.drawString(currentStatus,160,150);
  }
  // 飛行中的降落回報按鈕：不管上面顯示的是狀態還是訊息，都要能點得到
  if(currentStatus=="可以起飛"&&pilotMode==MODE_MASTER){
    M5.Display.fillRoundRect(20,180,280,34,10,0x3000); M5.Display.drawRoundRect(20,180,280,34,10,CLR_AMBER);
    fSm(); M5.Display.setTextColor(CLR_AMBER); M5.Display.drawString("長按3秒回報降落",160,197);
  }

  // 底部按鍵說明（對應3個實體按鍵 A/B/C）
  if(IS_FOLLOWER_CONN){
    M5.Display.fillRect(0,222,320,18,CLR_SURFACE);
    int fSegW=320/3;
    M5.Display.drawFastVLine(fSegW,222,18,0x4228);
    fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(0xBDF7);
    M5.Display.drawString("亮度",fSegW/2,231);
    // 說明文字縮小，避免超出螢幕或擋到左邊的「亮度」
    M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(0.6); M5.Display.setTextColor(CLR_ACCENT);
    M5.Display.drawString(pilotMode==MODE_GATHER?"飛聚跟隨·需回報":"僅顯示，不回應",fSegW+(320-fSegW)/2,231);
  } else {
    M5.Display.fillRect(0,218,320,20,CLR_BG);
    M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(1);
    M5.Display.setTextDatum(middle_center);
    int segW=320/3; uint16_t lblGray=0xBDF7;
    M5.Display.setTextColor(lblGray);
    M5.Display.drawString("亮度",segW/2,225);
    M5.Display.setTextColor(CLR_ACCENT);
    M5.Display.drawString(turnBtnLabel,segW+segW/2,225);
    M5.Display.setTextColor((currentStatus=="可以起飛")?lblGray:CLR_ACCENT);
    M5.Display.drawString("詢問",segW*2+segW/2,225);
  }
}

void drawAckButton(){
  fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_GRAY);
  M5.Display.drawString("10秒未按將叫",160,156);
  M5.Display.fillRoundRect(40,166,240,62,12,CLR_GREEN);
  if(landState==LAND_COUNTDOWN){
    fSm(); M5.Display.setTextColor(CLR_BG); M5.Display.drawString("降落完成",160,186);
    fXs(); M5.Display.setTextColor(0x0841); M5.Display.drawString("長按3秒送出",160,208);
  } else {
    fLg(); M5.Display.setTextColor(CLR_BG); M5.Display.drawString("收  到",160,197);
  }
}

void drawFollowerViewHint(){
  fSm(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_GRAY);
  M5.Display.drawString("跟隨模式 · 僅顯示",160,190);
}

void drawCommand(){
  M5.Display.fillScreen(CLR_BG); drawTopBar();
  uint16_t bar=(currentStatus=="可以起飛")?CLR_GREEN:(currentStatus=="降落")?CLR_AMBER:CLR_RED;
  M5.Display.fillRect(0,32,320,26,bar);
  fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_BG);
  M5.Display.drawString(notamCount>1&&notamCode.length()>0?("塔台指令 · "+notamCode):"塔台指令",160,45);
  uint16_t sc=(currentStatus=="可以起飛")?CLR_GREEN:(currentStatus=="降落")?CLR_AMBER:CLR_WHITE;
  bool hasReason=(currentStatus=="降落"&&landingTimeStr.length()>0&&landingReason.length()>0);
  // 塔台來訊時間放在內容上方（跟 line 一樣）；降落畫面資訊較密，這行略過（待命畫面仍會顯示）
  if(lastMessageTime.length()>0 && !(currentStatus=="降落"&&landingTimeStr.length()>0)){
    fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_GRAY);
    M5.Display.drawString("塔台 "+lastMessageTime+" 來訊",160,71);
  }
  if(currentStatus=="降落"&&landingTimeStr.length()>0){
    M5.Display.setTextColor(sc); M5.Display.setTextDatum(middle_center);
    if(hasReason){ fSm(); M5.Display.drawString(currentStatus,80,90); }
    else { fLg(); M5.Display.drawString(currentStatus,80,110); }
    M5.Display.drawFastVLine(158,60,105,0x3186);
    fXs(); M5.Display.setTextColor(CLR_GRAY); M5.Display.setTextDatum(middle_left);
    M5.Display.drawString("降落時間",165,76);
    fSm(); M5.Display.setTextColor(CLR_AMBER); M5.Display.drawString(getLandTimeDisplay(),165,96);
    fXs(); M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("還有 --:--",165,120);
    if(hasReason){ fXs(); M5.Display.setTextColor(CLR_WHITE); M5.Display.setTextDatum(middle_center); M5.Display.drawString(landingReason,80,130); }
  } else {
    fLg(); M5.Display.setTextColor(sc); M5.Display.setTextDatum(middle_center);
    M5.Display.drawString(currentStatus,160,105);
  }
  if(NEEDS_ACK) drawAckButton();
  else drawFollowerViewHint();
}

void drawMessage(){
  M5.Display.fillScreen(CLR_BG); drawTopBar();
  M5.Display.fillRect(0,32,320,26,CLR_ACCENT);
  fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_BG);
  M5.Display.drawString(notamCount>1&&notamCode.length()>0?("塔台訊息 · "+notamCode):"塔台訊息",160,45);
  // 發送時間放在訊息內容上方（跟 line 一樣）
  if(lastMessageTime.length()>0){
    fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_GRAY);
    M5.Display.drawString("塔台 "+lastMessageTime+" 發送",160,74);
  }
  drawFitText(lastMessage,160,124,CLR_WHITE);
  if(NEEDS_ACK) drawAckButton();
  else drawFollowerViewHint();
}

void drawLandingComplete(){
  M5.Display.fillScreen(CLR_BG); drawTopBar();
  fLg(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_GREEN);
  M5.Display.drawString("降落完成",160,110);
  fSm(); M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("已回報",160,155);
  currentScreen=SCR_IDLE; delay(2000); drawIdle();
}

void drawEndConfirm(){
  currentScreen=SCR_END;
  M5.Display.fillRect(0,172,320,68,CLR_SURFACE); M5.Display.drawRect(0,172,320,68,CLR_ACCENT);
  fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_WHITE);
  M5.Display.drawString("確定結束任務?",160,190);
  M5.Display.fillRoundRect(10,204,140,28,8,CLR_SURFACE); M5.Display.drawRoundRect(10,204,140,28,8,CLR_GRAY);
  M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("取消",80,218);
  M5.Display.fillRoundRect(170,204,140,28,8,CLR_RED);
  M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("確定",240,218);
}

void drawPoweroffConfirm(){
  powerOffReturnScreen=currentScreen;
  currentScreen=SCR_POWEROFF_CONFIRM;
  M5.Display.fillRect(0,172,320,68,CLR_SURFACE); M5.Display.drawRect(0,172,320,68,CLR_RED);
  fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_WHITE);
  M5.Display.drawString("確定關機?",160,190);
  M5.Display.fillRoundRect(10,204,140,28,8,CLR_SURFACE); M5.Display.drawRoundRect(10,204,140,28,8,CLR_GRAY);
  M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("取消",80,218);
  M5.Display.fillRoundRect(170,204,140,28,8,CLR_RED);
  M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("確定",240,218);
}

void drawMoreMenu(){
  moreMenuReturnScreen=currentScreen;
  currentScreen=SCR_MORE_MENU;
  M5.Display.fillScreen(CLR_BG); M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(1.3);
  M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("更多功能",160,26);
  // 選單按鈕字級統一縮小一點（0.85），避免太擠

  if(pilotMode==MODE_MASTER){
    // 結束任務
    M5.Display.fillRoundRect(20,48,280,38,10,CLR_SURFACE); M5.Display.drawRoundRect(20,48,280,38,10,CLR_GRAY);
    M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(0.85);
    M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("結束任務",160,67);
    // GPS 開關
    M5.Display.fillRoundRect(20,90,280,38,10,CLR_SURFACE); M5.Display.drawRoundRect(20,90,280,38,10,CLR_GRAY);
    M5.Display.setTextColor(gpsEnabled?CLR_GREEN:CLR_WHITE);
    M5.Display.drawString(String("GPS：")+(gpsEnabled?"開啟":"關閉"),160,109);
    // 關機（結束任務前不可用；文字縮成一行，不要疊兩行擠在一起）
    uint16_t poColor=sessionEnded?CLR_RED:CLR_GRAY;
    M5.Display.fillRoundRect(20,132,280,38,10,CLR_SURFACE); M5.Display.drawRoundRect(20,132,280,38,10,poColor);
    M5.Display.setTextColor(poColor);
    M5.Display.drawString(sessionEnded?"關機":"關機（先結束任務）",160,151);
    // 返回選擇模式
    M5.Display.fillRoundRect(20,174,280,38,10,CLR_SURFACE); M5.Display.drawRoundRect(20,174,280,38,10,CLR_GRAY);
    M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("返回選擇模式",160,193);
  } else {
    M5.Display.fillRoundRect(20,80,280,50,10,CLR_SURFACE); M5.Display.drawRoundRect(20,80,280,50,10,CLR_RED);
    M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(0.85);
    M5.Display.setTextColor(CLR_RED); M5.Display.drawString("關機",160,105);
    M5.Display.fillRoundRect(20,142,280,50,10,CLR_SURFACE); M5.Display.drawRoundRect(20,142,280,50,10,CLR_GRAY);
    M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("返回選擇模式",160,167);
  }
  fXs(); M5.Display.setTextColor(CLR_GRAY);
  if(pilotMode==MODE_MASTER && roomCode.length()>0)
    M5.Display.drawString("序號 "+roomCode+" · 點空白返回",160,220);
  else
    M5.Display.drawString("點擊空白處返回",160,220);
}

void drawWifiChangeConfirm(){
  wifiChangeReturnScreen=currentScreen; // 記住從哪個畫面點進來的，取消要回得去
  currentScreen=SCR_WIFI_CHANGE_CONFIRM;
  M5.Display.fillRect(0,172,320,68,CLR_SURFACE); M5.Display.drawRect(0,172,320,68,CLR_ACCENT);
  fXs(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_WHITE);
  M5.Display.drawString("更換WiFi？",160,182);
  M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("目前連線將中斷",160,198);
  M5.Display.fillRoundRect(10,204,140,28,8,CLR_SURFACE); M5.Display.drawRoundRect(10,204,140,28,8,CLR_GRAY);
  M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("取消",80,218);
  M5.Display.fillRoundRect(170,204,140,28,8,CLR_ACCENT);
  M5.Display.setTextColor(CLR_BG); M5.Display.drawString("確定",240,218);
}

void doPoweroff(){
  M5.Display.fillScreen(CLR_BG); M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(nullptr); M5.Display.setTextSize(2);
  M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("關機中...",160,120);
  delay(300);
  M5.Power.powerOff();
}
void returnFromPoweroff(){
  if(powerOffReturnScreen==SCR_WIFI_SCAN){ currentScreen=SCR_WIFI_SCAN; drawWifiList(); }
  else if(powerOffReturnScreen==SCR_COMMAND){ currentScreen=SCR_COMMAND; drawCommand(); }
  else if(powerOffReturnScreen==SCR_MODE_SELECT){ currentScreen=SCR_MODE_SELECT; drawModeSelect(); }
  else if(powerOffReturnScreen==SCR_CODE){ currentScreen=SCR_CODE; drawShowCode(); }
  else { currentScreen=SCR_IDLE; drawIdle(); }
}

void showEndMsg(){
  M5.Display.fillRect(40,100,240,36,CLR_SURFACE);
  fSm(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_GREEN);
  M5.Display.drawString("已通知塔台",160,118); delay(2000);
  drawIdle(); drawPoweroffConfirm();
}

// ── 倒數更新 ──────────────────────────────────────────────────────────────────
void updateClock(){
  drawClockAndName();
  if(currentScreen==SCR_COMMAND&&(landState==LAND_WAIT_ACK||landState==LAND_COUNTDOWN)&&landingTimeStr.length()>=4){
    int diff=landDiffSec();
    M5.Display.fillRect(162,110,158,28,CLR_BG);
    M5.Display.setFont(&fonts::efontTW_24); M5.Display.setTextSize(1); M5.Display.setTextDatum(middle_left);
    if(diff>0){ char buf[16]; sprintf(buf,"還有 %02d:%02d",diff/60,diff%60); M5.Display.setTextColor(diff<60?CLR_RED:CLR_GREEN); M5.Display.drawString(buf,165,122); }
    else { M5.Display.setTextColor(CLR_RED); M5.Display.drawString("降落時間到!",165,122); }
  }
}

// ── checkConnection ──────────────────────────────────────────────────────────
void checkConnection(){
  if(WiFi.status()!=WL_CONNECTED){
    wsConnecting=false;
    // checkConnection() 每3秒跑一次；如果每次斷線都立刻重打 WiFi.begin()，等於每3秒打斷一次還在
    // 進行中的連線協商，反而永遠連不穩、一直斷線重連。至少間隔久一點再重試一次
    if(millis()-lastWifiBeginAt>10000){ lastWifiBeginAt=millis(); WiFi.begin(savedSSID.c_str(),savedPassword.c_str()); }
    return;
  }
  if(!wsConnected && !wsConnecting){
    wsConnecting=true;
    if(IS_FOLLOWER_CONN) connectWebSocketFollower();
    else connectWebSocket();
    return;
  }
  drawBattery();
}

// ── 長按降落 ──────────────────────────────────────────────────────────────────
void checkLandBtnHold(){
  if(!landBtnPressed) return;
  if(currentStatus!="可以起飛"){landBtnPressed=false;return;}
  bool touching=false;
  if(M5.Touch.getCount()>0){ auto t=M5.Touch.getDetail(0); if(t.isPressed()&&t.y>175&&t.y<213) touching=true; }
  if(!touching){
    landBtnPressed=false;
    M5.Display.fillRoundRect(20,180,280,34,10,0x3000); M5.Display.drawRoundRect(20,180,280,34,10,CLR_AMBER);
    fSm(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_AMBER);
    M5.Display.drawString("長按3秒回報降落",160,197); return;
  }
  unsigned long held=millis()-landBtnPressAt;
  M5.Display.fillRoundRect(20,180,280,34,10,0x3000);
  int prog=min(276,(int)(276*held/3000)); if(prog>0) M5.Display.fillRoundRect(22,182,prog,30,8,CLR_AMBER);
  M5.Display.drawRoundRect(20,180,280,34,10,CLR_AMBER);
  fSm(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_BG);
  M5.Display.drawString("長按3秒回報降落",160,197);
  if(held>=3000){
    landBtnPressed=false;
    StaticJsonDocument<64> doc; doc["type"]="pilot_land_report"; doc["pilotName"]=pilotName; doc["notamIndex"]=activeNotamIdx;
    String o; serializeJson(doc,o); wsClient.sendTXT(o);
    currentStatus="降落"; landingTimeStr=""; landingReason=""; landState=LAND_NONE;
    currentScreen=SCR_IDLE; drawIdle(); buzz(1000,300);
  }
}

void checkLandDoneHold(){
  if(!landDonePressed) return;
  if(landState!=LAND_COUNTDOWN){landDonePressed=false;return;}
  bool touching=false;
  if(M5.Touch.getCount()>0){auto t=M5.Touch.getDetail(0);if(t.isPressed()&&t.y>164)touching=true;}
  if(!touching){landDonePressed=false;drawAckButton();return;}
  unsigned long held=millis()-landDonePressAt;
  M5.Display.fillRoundRect(40,166,240,62,12,CLR_GREEN);
  int prog=min(236,(int)(236*held/3000)); if(prog>0) M5.Display.fillRoundRect(42,168,prog,58,10,0x03E0);
  fSm(); M5.Display.setTextColor(CLR_BG); M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("降落完成",160,186);
  fXs(); M5.Display.setTextColor(0x0841); M5.Display.drawString("長按3秒送出",160,208);
  if(held>=3000){
    landDonePressed=false; submitAck("landing_done");
    ackPending=false; buzzPhase=0; landState=LAND_NONE; drawLandingComplete();
  }
}

// ── 飛航公告鍵盤 ──────────────────────────────────────────────────────────────
void drawKeypad(){
  M5.Display.fillScreen(CLR_BG); M5.Display.fillRect(0,0,320,32,CLR_BG);
  M5.Display.setTextDatum(middle_center);
  M5.Display.fillRoundRect(4,2,40,26,4,CLR_SURFACE); M5.Display.drawRoundRect(4,2,40,26,4,CLR_GRAY);
  fSm(); M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("<",24,15);
  fXs(); M5.Display.setTextColor(CLR_AMBER); M5.Display.drawString(notamAddMode?"新增 NOTAM 號碼 (U+4碼)":"飛航公告號碼 (U+4碼)",160,10);
  String preview="U"+(keypadBuffer.length()>0?keypadBuffer:"____");
  fLg(); M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString(preview,160,36);
  int keys[]={1,2,3,4,5,6,7,8,9,-1,0,-2};
  int kx=10,ky=58,kw=94,kh=40,gap=4;
  for(int i=0;i<12;i++){
    int col=i%3,row=i/3,x=kx+col*(kw+gap),y=ky+row*(kh+gap);
    uint16_t bg=(keys[i]==-2)?CLR_GREEN:(keys[i]==-1)?CLR_RED:CLR_SURFACE;
    M5.Display.fillRoundRect(x,y,kw,kh,6,bg); M5.Display.drawRoundRect(x,y,kw,kh,6,CLR_GRAY);
    fSm(); M5.Display.setTextColor(CLR_WHITE); M5.Display.setTextDatum(middle_center);
    if(keys[i]==-1) M5.Display.drawString("CLR",x+kw/2,y+kh/2);
    else if(keys[i]==-2) M5.Display.drawString("OK",x+kw/2,y+kh/2);
    else{char c[2];sprintf(c,"%d",keys[i]);M5.Display.drawString(c,x+kw/2,y+kh/2);}
  }
}

void handleKeypadTouch2(int tx,int ty){
  if(tx>=4&&tx<=44&&ty>=2&&ty<=28){
    keypadMode=KP_NONE; notamAddMode=false;
    if(currentScreen==SCR_COMMAND) drawCommand(); else { currentScreen=SCR_IDLE; drawIdle(); }
    return;
  }
  int keys[]={1,2,3,4,5,6,7,8,9,-1,0,-2};
  int kx=10,ky=58,kw=94,kh=40,gap=4;
  for(int i=0;i<12;i++){
    int col=i%3,row=i/3,x=kx+col*(kw+gap),y=ky+row*(kh+gap);
    if(tx>=x&&tx<=x+kw&&ty>=y&&ty<=y+kh){
      if(keys[i]==-1){keypadBuffer="";drawKeypad();return;}
      if(keys[i]==-2){
        if(keypadBuffer.length()==4){
          String code="U"+keypadBuffer;
          StaticJsonDocument<128> doc;
          if(notamAddMode){
            // 新增一個 NOTAM（最多3個）：本機直接補一筆到陣列尾端，再切換過去看它
            notamAddMode=false;
            if(notamCount<3){
              int newIdx=notamCount;
              notamSlots[newIdx]=NotamSlot();
              notamSlots[newIdx].code=code;
              notamCount++;
              doc["type"]="pilot_notam_manage"; doc["action"]="add"; doc["code"]=code;
              String o;serializeJson(doc,o);wsClient.sendTXT(o);
              keypadMode=KP_NONE;
              switchToNotamSlot(newIdx);
              currentScreen=SCR_IDLE; drawIdle(); beep2();
            } else { keypadMode=KP_NONE; drawNotamList(); }
          } else {
            notamCode=code;
            notamSlots[activeNotamIdx].code=notamCode;
            if(notamCount>1){ // 2個以上 NOTAM 時要指定改哪個 slot，不能再用舊版單一 NOTAM 訊息（會改到伺服器第1筆）
              doc["type"]="pilot_notam_manage"; doc["action"]="edit"; doc["index"]=activeNotamIdx; doc["code"]=notamCode;
            } else {
              doc["type"]="pilot_notam"; doc["notam"]=notamCode; doc["pilotName"]=pilotName;
            }
            String o;serializeJson(doc,o);wsClient.sendTXT(o);
            keypadMode=KP_NONE;
            if(notamHadValue){ currentScreen=SCR_TURNPOINT_CONFIRM; drawTurnpointConfirm(); beep2(); }
            else { currentScreen=SCR_IDLE; drawIdle(); beep2(); } // 從 SCR_COMMAND 點進來編輯也要把 currentScreen 撥回IDLE，跟畫面同步
          }
        } else { M5.Display.fillRect(40,200,240,26,CLR_BG); fXs();M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_RED); M5.Display.drawString("請輸入4個數字",160,213); }
        return;
      }
      if(keypadBuffer.length()<4){keypadBuffer+=String(keys[i]);drawKeypad();}
      return;
    }
  }
}

// ── 轉點：是否轉點確認 + 分鐘數鍵盤 ──────────────────────────────────────────
void drawTurnpointConfirm(){
  M5.Display.fillScreen(CLR_BG); M5.Display.setTextDatum(middle_center);
  fLg(); M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("是否轉點？",160,90);
  fXs(); M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("公告已更新，是否同時回報轉點",160,124);
  M5.Display.fillRoundRect(20,150,130,60,10,CLR_SURFACE); M5.Display.drawRoundRect(20,150,130,60,10,CLR_GRAY);
  M5.Display.fillRoundRect(170,150,130,60,10,CLR_ACCENT);
  fSm(); M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("否",85,180);
  M5.Display.setTextColor(CLR_BG); M5.Display.drawString("是",235,180);
}

void drawMinuteKeypad(){
  M5.Display.fillScreen(CLR_BG); M5.Display.fillRect(0,0,320,32,CLR_BG);
  M5.Display.setTextDatum(middle_center);
  M5.Display.fillRoundRect(4,2,40,26,4,CLR_SURFACE); M5.Display.drawRoundRect(4,2,40,26,4,CLR_GRAY);
  fSm(); M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("<",24,15);
  fXs(); M5.Display.setTextColor(CLR_AMBER); M5.Display.drawString("轉點時間（大約幾分鐘）",160,10);
  String preview=(keypadBuffer.length()>0?keypadBuffer:"__")+" 分鐘";
  fXs(); M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString(preview,160,36);
  int keys[]={1,2,3,4,5,6,7,8,9,-1,0,-2};
  int kx=10,ky=58,kw=94,kh=40,gap=4;
  for(int i=0;i<12;i++){
    int col=i%3,row=i/3,x=kx+col*(kw+gap),y=ky+row*(kh+gap);
    uint16_t bg=(keys[i]==-2)?CLR_GREEN:(keys[i]==-1)?CLR_RED:CLR_SURFACE;
    M5.Display.fillRoundRect(x,y,kw,kh,6,bg); M5.Display.drawRoundRect(x,y,kw,kh,6,CLR_GRAY);
    fSm(); M5.Display.setTextColor(CLR_WHITE); M5.Display.setTextDatum(middle_center);
    if(keys[i]==-1) M5.Display.drawString("CLR",x+kw/2,y+kh/2);
    else if(keys[i]==-2) M5.Display.drawString("OK",x+kw/2,y+kh/2);
    else{char c[2];sprintf(c,"%d",keys[i]);M5.Display.drawString(c,x+kw/2,y+kh/2);}
  }
}

void sendTurnpoint(int minutes, bool viaNotam){
  StaticJsonDocument<128> doc;
  doc["type"]="pilot_turnpoint"; doc["pilotName"]=pilotName; doc["minutes"]=minutes; doc["viaNotam"]=viaNotam;
  String o; serializeJson(doc,o); wsClient.sendTXT(o);
}

void sendArrived(){
  StaticJsonDocument<64> doc;
  doc["type"]="pilot_arrived"; doc["pilotName"]=pilotName;
  String o; serializeJson(doc,o); wsClient.sendTXT(o);
}

void handleMinuteKeypadTouch(int tx,int ty){
  if(tx>=4&&tx<=44&&ty>=2&&ty<=28){
    keypadMode=KP_NONE; turnpointSource=0;
    currentScreen=SCR_IDLE; drawIdle();
    return;
  }
  int keys[]={1,2,3,4,5,6,7,8,9,-1,0,-2};
  int kx=10,ky=58,kw=94,kh=40,gap=4;
  for(int i=0;i<12;i++){
    int col=i%3,row=i/3,x=kx+col*(kw+gap),y=ky+row*(kh+gap);
    if(tx>=x&&tx<=x+kw&&ty>=y&&ty<=y+kh){
      if(keys[i]==-1){keypadBuffer="";drawMinuteKeypad();return;}
      if(keys[i]==-2){
        int n=keypadBuffer.toInt();
        if(keypadBuffer.length()==0||n<=0){
          M5.Display.fillRect(40,200,240,26,CLR_BG); fXs();M5.Display.setTextDatum(middle_center);
          M5.Display.setTextColor(CLR_RED); M5.Display.drawString("請輸入有效分鐘數",160,213);
          return;
        }
        sendTurnpoint(n, turnpointSource==2);
        if(turnpointSource==1) turnBtnLabel="就位";
        keypadMode=KP_NONE; turnpointSource=0;
        currentScreen=SCR_IDLE; drawIdle(); beep2();
        return;
      }
      if(keypadBuffer.length()<2){keypadBuffer+=String(keys[i]);drawMinuteKeypad();}
      return;
    }
  }
}

// ── 觸控處理 ──────────────────────────────────────────────────────────────────
void onKeyboardConfirm(){
  if(kbTarget=="name"){ pilotName=kbBuffer; saveName(pilotName); currentScreen=SCR_WIFI_SCAN; startWifiScan(); }
  else if(kbTarget=="rename"){
    pilotName=kbBuffer; saveName(pilotName);
    pilotDisplayName=""; saveDisplayName(""); // 機身重新命名，蓋掉手機主控輔助之前設定的顯示名字，避免又被蓋回去
    StaticJsonDocument<128> doc; doc["type"]=IS_FOLLOWER_CONN?"follower_rename":"pilot_rename"; doc["name"]=pilotName;
    String o; serializeJson(doc,o); wsClient.sendTXT(o);
    currentScreen=renameReturnScreen; if(currentScreen==SCR_COMMAND) drawCommand(); else { currentScreen=SCR_IDLE; drawIdle(); }
  }
  else if(kbTarget=="rename_mode"){ pilotName=kbBuffer; saveName(pilotName); currentScreen=SCR_MODE_SELECT; drawModeSelect(); }
  else if(kbTarget=="password"){ savedPassword=kbBuffer; saveWifi(pendingSSID,savedPassword); connectWiFiSaved(); }
  else if(kbTarget=="follower"){ masterCode=kbBuffer; connectWebSocketFollower(); }
}

void handleTouch(){
  if(!M5.Touch.getCount()) return;
  auto t=M5.Touch.getDetail(0); if(!t.wasPressed()) return;
  lastActivity=millis();
  if(screenDimmed){ M5.Display.setBrightness(BRIGHT_VAL[brightnessLevel]); screenDimmed=false; return; }
  int tx=t.x, ty=t.y;

  // 更多選單：頂部列，任何有畫面頂欄的畫面都能點
  if((currentScreen==SCR_IDLE||currentScreen==SCR_COMMAND||currentScreen==SCR_NOTAM_LIST)&&tx>=184&&tx<=248&&ty<=34){ drawMoreMenu(); return; }

  if(currentScreen==SCR_NOTAM_LIST){ handleNotamListTouch(tx,ty); return; }
  if(currentScreen==SCR_NOTAM_DELETE_CONFIRM){ handleNotamDeleteConfirmTouch(tx,ty); return; }
  if(currentScreen==SCR_WIFI_SCAN){ handleWifiListTouch(tx,ty); return; }
  if(currentScreen==SCR_NAME_INPUT||currentScreen==SCR_WIFI_PASS||currentScreen==SCR_FOLLOWER_CODE){ handleKeyboardTouch(tx,ty); return; }
  if(currentScreen==SCR_CODE){
    if(tx>=110&&tx<=210&&ty>=214&&ty<=236){ drawPoweroffConfirm(); return; }
    return;
  }
  if(currentScreen==SCR_MODE_SELECT){
    // 關機/WiFi 按鍵原本太小，這裡的觸控範圍要跟著上面加大過的畫面對齊
    if(ty<=42&&tx>=210){ drawPoweroffConfirm(); return; } // 右上角關機
    if(ty<=42&&tx<96){ drawWifiChangeConfirm(); return; } // 左上角更換WiFi
    if(ty>42&&ty<60&&tx>=96&&tx<210){ kbBuffer=pilotName; kbHint="更改飛手名字（英文小寫）"; kbTarget="rename_mode"; kbShift=false; kbPage=0; kbMaxLen=10; currentScreen=SCR_NAME_INPUT; drawKeyboard(); return; }
    // 3個模式按鈕原本中間留了空隙，常常點在縫上沒反應；觸控範圍改成緊接著分（以兩顆按鈕正中間為界），
    // 上下也各往外延伸一點，不用剛好點在畫的框線內才有效
    if(ty>=56&&ty<=121){ pilotMode=MODE_MASTER; connectWebSocket(); }
    else if(ty>121&&ty<=183){ pilotMode=MODE_FOLLOWER; gpsEnabled=false; gpsFixed=false; drawFollowerInput(); }
    else if(ty>183&&ty<=240){ pilotMode=MODE_GATHER; gpsEnabled=false; gpsFixed=false; drawFollowerInput(); }
    return;
  }
  if(currentScreen==SCR_IDLE){
    // WiFi 狀態燈：點擊可重新選擇 WiFi
    if(tx>=250&&tx<=272&&ty<=30){ drawWifiChangeConfirm(); return; }
    // 名字觸控改名（主控）：畫面現在顯示的是哪個名字（可能是手機主控輔助設定的），編輯就從那個開始，不要跳回舊的
    if(tx>=40&&tx<182&&ty<32&&pilotMode==MODE_MASTER){ renameReturnScreen=currentScreen; kbBuffer=pilotDisplayName.length()>0?pilotDisplayName:pilotName; kbHint="更改飛手名字（英文小寫）"; kbTarget="rename"; kbShift=false; kbPage=0; kbMaxLen=10; currentScreen=SCR_NAME_INPUT; drawKeyboard(); return; }
    // GPS
    if(tx>232&&tx<314&&ty>34&&ty<56){ if(pilotMode==MODE_MASTER){ gpsEnabled=!gpsEnabled; if(!gpsEnabled)gpsFixed=false; drawGpsBtn(); sendHeartbeat(); } }
    // 公告
    if(tx<214&&ty>68&&ty<88&&pilotMode==MODE_MASTER){ notamHadValue=notamCode.length()>0; keypadMode=KP_NOTAM; keypadBuffer=notamCode.length()>0?notamCode.substring(1):""; drawKeypad(); return; }
    if(tx>=214&&tx<=244&&ty>68&&ty<88&&pilotMode==MODE_MASTER&&notamCount<3){ notamHadValue=false; notamAddMode=true; keypadMode=KP_NOTAM; keypadBuffer=""; drawKeypad(); return; }
    // 降落長按
    if(ty>178&&ty<214&&currentStatus=="可以起飛"&&pilotMode==MODE_MASTER){ landBtnPressAt=millis(); landBtnPressed=true; }
    // 轉點／就位切換按鈕（底部第2格，共3格，對應實體 BtnB）
    if(ty>=216&&ty<=240&&tx>=107&&tx<213&&pilotMode==MODE_MASTER){
      if(turnBtnLabel=="轉點"){ turnpointSource=1; keypadMode=KP_TURNPOINT; keypadBuffer=""; drawMinuteKeypad(); }
      else { sendArrived(); turnBtnLabel="轉點"; drawIdle(); beep2(); }
      return;
    }
    // 跟隨／飛聚跟隨：底部左側可調亮度（待命畫面不回報，飛聚跟隨的回報在指令畫面）
    if(IS_FOLLOWER_CONN&&ty>=222&&ty<=240&&tx<107){
      brightnessLevel=(brightnessLevel+1)%3;
      M5.Display.setBrightness(BRIGHT_VAL[brightnessLevel]); buzz(1000,50);
      drawIdle(); return;
    }
  }
  else if(currentScreen==SCR_TURNPOINT_CONFIRM){
    if(tx<160){ currentScreen=SCR_IDLE; drawIdle(); }
    else { turnpointSource=2; keypadMode=KP_TURNPOINT; keypadBuffer=""; drawMinuteKeypad(); }
  }
  else if(currentScreen==SCR_POWEROFF_CONFIRM){
    if(tx<160) returnFromPoweroff();
    else { doPoweroff(); }
  }
  else if(currentScreen==SCR_WIFI_CHANGE_CONFIRM){
    if(tx<160){ currentScreen=wifiChangeReturnScreen; if(currentScreen==SCR_MODE_SELECT) drawModeSelect(); else { currentScreen=SCR_IDLE; drawIdle(); } }
    else { startWifiScan(); }
  }
  else if(currentScreen==SCR_UPDATE_CONFIRM){
    if(tx<160){ currentScreen=SCR_MODE_SELECT; drawModeSelect(); }
    else { doFirmwareUpdate(); }
  }
  else if(currentScreen==SCR_MORE_MENU){
    if(pilotMode==MODE_MASTER){
      if(ty>=48&&ty<=86){ drawEndConfirm(); }
      else if(ty>=90&&ty<=128){ gpsEnabled=!gpsEnabled; if(!gpsEnabled)gpsFixed=false; sendHeartbeat(); drawMoreMenu(); }
      else if(ty>=132&&ty<=170){ if(sessionEnded) drawPoweroffConfirm(); }
      else if(ty>=174&&ty<=212){ pilotMode=MODE_NONE; currentScreen=SCR_MODE_SELECT; drawModeSelect(); }
      else { currentScreen=moreMenuReturnScreen; if(currentScreen==SCR_COMMAND) drawCommand(); else { currentScreen=SCR_IDLE; drawIdle(); } }
    } else {
      if(ty>=80&&ty<=130){ drawPoweroffConfirm(); }
      else if(ty>=142&&ty<=192){ pilotMode=MODE_NONE; currentScreen=SCR_MODE_SELECT; drawModeSelect(); }
      else { currentScreen=moreMenuReturnScreen; if(currentScreen==SCR_COMMAND) drawCommand(); else { currentScreen=SCR_IDLE; drawIdle(); } }
    }
  }
  else if(currentScreen==SCR_COMMAND){
    // 名字觸控改名（主控）：SCR_IDLE 早就有這個功能，但收到指令切到 SCR_COMMAND 後這裡漏了同一個熱區，
    // 導致正在作業中（畫面幾乎都停在這頁）點名字完全沒反應，像是壞掉一樣
    if(tx>=40&&tx<182&&ty<32&&pilotMode==MODE_MASTER){ renameReturnScreen=currentScreen; kbBuffer=pilotDisplayName.length()>0?pilotDisplayName:pilotName; kbHint="更改飛手名字（英文小寫）"; kbTarget="rename"; kbShift=false; kbPage=0; kbMaxLen=10; currentScreen=SCR_NAME_INPUT; drawKeyboard(); return; }
    if(tx<214&&ty>68&&ty<88&&pilotMode==MODE_MASTER){ notamHadValue=notamCode.length()>0; keypadMode=KP_NOTAM; keypadBuffer=notamCode.length()>0?notamCode.substring(1):""; drawKeypad(); return; }
    if(tx>=214&&tx<=244&&ty>68&&ty<88&&pilotMode==MODE_MASTER&&notamCount<3){ notamHadValue=false; notamAddMode=true; keypadMode=KP_NOTAM; keypadBuffer=""; drawKeypad(); return; }
    if(ty>164&&tx>40&&tx<280&&NEEDS_ACK){
      if(landState==LAND_WAIT_ACK){ submitAck("landing_ack"); ackPending=false; buzzPhase=0; landState=LAND_COUNTDOWN; drawCommand(); buzz(880,150); }
      else if(landState==LAND_COUNTDOWN){ landDonePressAt=millis(); landDonePressed=true; }
      else { submitAck(currentStatus=="可以起飛"?"takeoff":"ack"); ackPending=false; buzzPhase=0; currentScreen=SCR_IDLE; drawIdle(); }
    }
  }
  else if(currentScreen==SCR_END){
    if(tx>160){sendEndSession();sessionEnded=true;currentScreen=SCR_IDLE;showEndMsg();}
    else {currentScreen=SCR_IDLE;drawIdle();}
  }
}

void handleButtons(){
  if(M5.BtnA.wasClicked()||M5.BtnB.wasClicked()||M5.BtnC.wasClicked()){
    lastActivity=millis();
    if(screenDimmed){ M5.Display.setBrightness(BRIGHT_VAL[brightnessLevel]); screenDimmed=false; return; }
  }
  if(M5.BtnA.wasClicked()){
    if(keypadMode!=KP_NONE){keypadMode=KP_NONE; notamAddMode=false; if(currentScreen==SCR_COMMAND) drawCommand(); else { currentScreen=SCR_IDLE; drawIdle(); } return;}
    if(currentScreen==SCR_NAME_INPUT&&kbTarget=="rename"){ currentScreen=renameReturnScreen; if(currentScreen==SCR_COMMAND) drawCommand(); else { currentScreen=SCR_IDLE; drawIdle(); } return; }
    if(currentScreen==SCR_NAME_INPUT&&kbTarget=="rename_mode"){ currentScreen=SCR_MODE_SELECT; drawModeSelect(); return; }
    if(currentScreen==SCR_WIFI_PASS){ currentScreen=SCR_WIFI_SCAN; drawWifiList(); return; }
    if(currentScreen==SCR_FOLLOWER_CODE){ pilotMode=MODE_NONE; currentScreen=SCR_MODE_SELECT; drawModeSelect(); return; }
    if(currentScreen==SCR_TURNPOINT_CONFIRM){ currentScreen=SCR_IDLE; drawIdle(); return; }
    if(currentScreen==SCR_POWEROFF_CONFIRM){ returnFromPoweroff(); return; }
    if(currentScreen==SCR_WIFI_CHANGE_CONFIRM){ currentScreen=wifiChangeReturnScreen; if(currentScreen==SCR_MODE_SELECT) drawModeSelect(); else { currentScreen=SCR_IDLE; drawIdle(); } return; }
    if(currentScreen==SCR_UPDATE_CONFIRM){ currentScreen=SCR_MODE_SELECT; drawModeSelect(); return; }
    if(currentScreen==SCR_MORE_MENU){ currentScreen=moreMenuReturnScreen; if(currentScreen==SCR_COMMAND) drawCommand(); else { currentScreen=SCR_IDLE; drawIdle(); } return; }
    if(currentScreen==SCR_NAME_INPUT||currentScreen==SCR_WIFI_SCAN) return;
    brightnessLevel=(brightnessLevel+1)%3;
    M5.Display.setBrightness(BRIGHT_VAL[brightnessLevel]); buzz(1000,50);
    if(currentScreen==SCR_IDLE) drawIdle();
    else if(currentScreen==SCR_COMMAND) drawCommand();
  }
  if(M5.BtnB.wasClicked()){
    if(pilotMode==MODE_MASTER&&currentScreen==SCR_IDLE){
      if(turnBtnLabel=="轉點"){ turnpointSource=1; keypadMode=KP_TURNPOINT; keypadBuffer=""; drawMinuteKeypad(); }
      else { sendArrived(); turnBtnLabel="轉點"; drawIdle(); beep2(); }
    }
  }
  if(M5.BtnC.wasClicked()){
    if(pilotMode!=MODE_MASTER) return;
    if(wsConnected){
      bool duringFlight=(currentStatus=="可以起飛");
      StaticJsonDocument<64> doc;
      doc["type"]="pilot_ask_status"; doc["pilotName"]=pilotName;
      doc["askType"]=duringFlight?"duration":"airport";
      String o; serializeJson(doc,o); wsClient.sendTXT(o);
      M5.Display.fillRect(20,200,280,26,CLR_SURFACE); fXs(); M5.Display.setTextDatum(middle_center);
      M5.Display.setTextColor(CLR_ACCENT);
      M5.Display.drawString(duringFlight?"已詢問塔台放行時長":"已詢問塔台機場狀況",160,213);
      buzz(880,80);
    }
  }
  if(M5.BtnPWR.wasClicked()){
    if(currentScreen==SCR_CHARGING){ M5.Display.setBrightness(BRIGHT_VAL[brightnessLevel]); currentScreen=SCR_BOOT; }
  }
  if(M5.BtnPWR.pressedFor(2000)){ doPoweroff(); }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info){
  if(event==ARDUINO_EVENT_WIFI_STA_DISCONNECTED){
    uint8_t reason=info.wifi_sta_disconnected.reason;
    Serial.printf("[WiFi] disconnected, reason=%d (%s)\n", reason, WiFi.disconnectReasonName((wifi_err_reason_t)reason));
  }
}

void setup(){
  auto cfg=M5.config(); M5.begin(cfg);
  M5.Display.setRotation(1); M5.Display.setBrightness(BRIGHT_VAL[brightnessLevel]);
  M5.Display.fillScreen(CLR_BG); M5.Display.setTextDatum(middle_center);
  M5.Speaker.setVolume(255); M5.Speaker.begin(); Serial.begin(115200);
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA); WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial2.setRxBufferSize(1024);
  Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  loadPrefs();

  if(M5.Power.isCharging()){
    drawCharging();
    bool doboot=false;
    unsigned long fullSince=0, notChgSince=0, lastRedraw=millis();
    while(!doboot){
      M5.update();
      if(M5.BtnA.wasClicked()||M5.BtnB.wasClicked()||M5.BtnC.wasClicked()||M5.BtnPWR.wasClicked()){ doboot=true; break; }
      bool chg=M5.Power.isCharging();
      int lvl=M5.Power.getBatteryLevel();
      // 充飽（電量≥99%）持續 15 秒 → 自動關機
      if(lvl>=99){ if(!fullSince) fullSince=millis(); if(millis()-fullSince>15000) doPoweroff(); }
      else fullSince=0;
      // 已拔線 → 持續 4 秒沒再充電就開機（螢幕保持亮，不再進黑畫面內迴圈避免卡死）
      if(!chg){ if(!notChgSince) notChgSince=millis(); if(millis()-notChgSince>4000){ doboot=true; break; } }
      else notChgSince=0;
      if(millis()-lastRedraw>1000){ drawCharging(); lastRedraw=millis(); }
      delay(80);
    }
    M5.Display.setBrightness(BRIGHT_VAL[brightnessLevel]);
  }

  M5.Display.fillScreen(CLR_BG); M5.Display.setFont(nullptr); M5.Display.setTextSize(2);
  M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_ACCENT);
  M5.Display.drawString("DroneATIS",160,120); delay(800);

  if(pilotName.length()==0){ kbBuffer=""; kbHint="設定飛手名字（英文小寫）"; kbTarget="name"; kbShift=false; kbMaxLen=10; currentScreen=SCR_NAME_INPUT; drawKeyboard(); }
  else if(savedSSID.length()==0){ startWifiScan(); }
  else {
    drawConnecting("自動連線 "+savedSSID+"...");
    WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(savedSSID.c_str(),savedPassword.c_str());
    int t=0; while(WiFi.status()!=WL_CONNECTED&&t<50){delay(500);t++;}
    if(WiFi.status()==WL_CONNECTED){
      configTime(TZ_OFFSET*3600,0,NTP_SERVER);
      struct tm ti; int nt=0; while(!getLocalTime(&ti)&&nt<20){delay(500);nt++;}
      checkForUpdate();
    } else { drawConnecting(wifiFailReason()); delay(2500); startWifiScan(); }
  }
  lastActivity=millis();
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop(){
  M5.update();
  wsClient.loop();
  if(!IS_FOLLOWER_CONN) updateGpsReading();
  unsigned long now=millis();
  if(now-lastHeartbeat>5000){ sendHeartbeat(); lastHeartbeat=now; }
  if(now-lastConnCheck>3000&&keypadMode==KP_NONE&&
     currentScreen!=SCR_WIFI_SCAN&&currentScreen!=SCR_NAME_INPUT&&
     currentScreen!=SCR_WIFI_PASS&&currentScreen!=SCR_MODE_SELECT&&
     currentScreen!=SCR_FOLLOWER_CODE&&currentScreen!=SCR_UPDATE_CONFIRM){ checkConnection(); lastConnCheck=now; }
  if(now-lastTimeUpd>1000){ lastTimeUpd=now; if(keypadMode==KP_NONE&&(currentScreen==SCR_IDLE||currentScreen==SCR_COMMAND)) updateClock(); }
  handleBuzzer(now);
  // 跑道通知5秒後清除
  if(rwyNoticeUntil>0&&now>rwyNoticeUntil&&currentScreen==SCR_IDLE){ rwyNoticeUntil=0; drawIdle(); }
  // 飛聚跟隨回報訊息顯示10秒後清除
  if(followerConfirmUntil>0&&now>followerConfirmUntil){ followerConfirmUntil=0; if(currentScreen==SCR_IDLE) drawIdle(); }
  // 閒置自動調暗（降落流程中不休眠，讓飛手隨時看得到倒數）
  bool inLanding = (landState==LAND_WAIT_ACK||landState==LAND_COUNTDOWN) || (currentStatus=="降落"&&landingTimeStr.length()>0);
  if((currentScreen==SCR_IDLE||currentScreen==SCR_COMMAND)&&!screenDimmed&&!inLanding&&now-lastActivity>IDLE_DIM_MS){ M5.Display.setBrightness(30); screenDimmed=true; }
  if(keypadMode==KP_NOTAM){ if(M5.Touch.getCount()){auto t=M5.Touch.getDetail(0);if(t.wasPressed())handleKeypadTouch2(t.x,t.y);} }
  else if(keypadMode==KP_TURNPOINT){ if(M5.Touch.getCount()){auto t=M5.Touch.getDetail(0);if(t.wasPressed())handleMinuteKeypadTouch(t.x,t.y);} }
  else if(currentScreen==SCR_NAME_INPUT||currentScreen==SCR_WIFI_PASS||currentScreen==SCR_FOLLOWER_CODE){ if(M5.Touch.getCount()){auto t=M5.Touch.getDetail(0);if(t.wasPressed())handleKeyboardTouch(t.x,t.y);} }
  else if(currentScreen==SCR_WIFI_SCAN){ if(M5.Touch.getCount()){auto t=M5.Touch.getDetail(0);if(t.wasPressed())handleWifiListTouch(t.x,t.y);} }
  else { handleTouch(); if(landBtnPressed) checkLandBtnHold(); if(landDonePressed) checkLandDoneHold(); }
  handleButtons();
  delay(20);
}
