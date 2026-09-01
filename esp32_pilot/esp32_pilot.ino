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
#define FW_VERSION    22
#define UPDATE_CHECK_URL "https://droneatis-production.up.railway.app/firmware/version.json"

// ── NVS 儲存 ─────────────────────────────────────────────────────────────────
Preferences prefs;
String savedSSID     = "";
String savedPassword = "";
String pilotName     = "";

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
unsigned long lastTimeUpd   = 0;
unsigned long lastBuzzAt    = 0;
int buzzPhase = 0;
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
  SCR_COMMAND, SCR_END, SCR_CHARGING, SCR_TURNPOINT_CONFIRM, SCR_POWEROFF_CONFIRM, SCR_WIFI_CHANGE_CONFIRM, SCR_UPDATE_CONFIRM, SCR_MORE_MENU
};
Screen currentScreen = SCR_BOOT;
Screen moreMenuReturnScreen = SCR_IDLE;  // 開啟「更多」選單前所在的畫面，關閉後要回去
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
  prefs.end();
}
void saveWifi(String ssid, String pass){
  prefs.begin("datis",false);
  prefs.putString("ssid",ssid);
  prefs.putString("pass",pass);
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

void drawTopBar(){
  M5.Display.fillRect(0,0,320,32,CLR_DARK);
  fXs();
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(CLR_WHITE);
  M5.Display.drawString(getNowTime(),4,16);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(CLR_ACCENT);
  M5.Display.drawString(pilotName,160,16);
  M5.Display.fillRoundRect(206,4,40,24,4,CLR_SURFACE); M5.Display.drawRoundRect(206,4,40,24,4,CLR_GRAY);
  M5.Display.setTextColor(CLR_GRAY);
  M5.Display.drawString("更多",226,16);
  bool ok=(WiFi.status()==WL_CONNECTED&&wsConnected);
  M5.Display.fillCircle(252,16,5,ok?CLR_GREEN:CLR_RED);
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
  M5.Display.drawString("DroneATIS",160,24);
  M5.Display.setFont(nullptr); M5.Display.setTextSize(1); M5.Display.setTextColor(CLR_GRAY);
  M5.Display.drawString(pilotName,160,46);
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
  doc["type"]="pilot_ack"; doc["ackType"]=ackType;
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
      StaticJsonDocument<512> doc;
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
        currentScreen=SCR_IDLE; drawIdle(); beep3();
      }
      else if(type=="tower_info"){ // 塔台改南北塔或名字
        towerName=doc["towerName"]|towerName; towerType=doc["towerType"]|towerType;
        if(currentScreen==SCR_IDLE) drawIdle();
      }
      else if(type=="command"||type=="follower_sync"){
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
        groupName=doc["groupName"]|"";
        if(doc.containsKey("towerName")) towerName=doc["towerName"].as<String>();
        if(currentScreen==SCR_IDLE) drawIdle();
      }
      else if(type=="rwy_update"){
        rwyDir=doc["rwy"]|"";
        if(currentScreen==SCR_IDLE){
          drawIdle();
          M5.Display.fillRect(20,130,280,40,CLR_SURFACE);
          M5.Display.drawRect(20,130,280,40,CLR_AMBER);
          fSm(); M5.Display.setTextDatum(middle_center);
          M5.Display.setTextColor(CLR_AMBER);
          M5.Display.drawString("跑道改"+rwyDir,160,150);
          rwyNoticeUntil=millis()+5000;
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
  if(landState==LAND_COUNTDOWN){ int diff=landDiffSec(); if(diff<=0&&now-lastBuzzAt>1500){buzz(800,1200);lastBuzzAt=now;} return; }
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

void drawIdle(){
  M5.Display.fillScreen(CLR_BG); drawTopBar();

  bool ok=towerConnected&&wsConnected;
  fSm(); M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(ok?CLR_GREEN:CLR_RED);
  M5.Display.drawString(ok?("● "+towerType+" "+towerName):"● 無連線",8,46);

  if(pilotMode==MODE_MASTER) drawGpsBtn();

  // 公告框（觸控 y:58~78）
  M5.Display.fillRoundRect(6,58,214,20,4,CLR_SURFACE); M5.Display.drawRoundRect(6,58,214,20,4,0x4228);
  fXs(); M5.Display.setTextDatum(middle_left);
  if(notamCode.length()>0){ M5.Display.setTextColor(CLR_AMBER); M5.Display.drawString("飛航公告: "+notamCode,10,68); }
  else if(pilotMode==MODE_MASTER){ M5.Display.setTextColor(0x5AEB); M5.Display.drawString("點擊輸入飛航公告",10,68); }
  else { M5.Display.setTextColor(CLR_GRAY); M5.Display.drawString("飛航公告",10,68); }

  if(rwyDir.length()>0){
    fXs(); M5.Display.setTextDatum(middle_right); M5.Display.setTextColor(CLR_WHITE);
    M5.Display.drawString(rwyDir,316,68);
  }
  if(groupName.length()>0){
    fXs(); M5.Display.setTextDatum(middle_left); M5.Display.setTextColor(CLR_ACCENT);
    M5.Display.drawString("["+groupName+"]",8,96);
  }

  uint16_t sc=CLR_WHITE;
  if(currentStatus=="可以起飛") sc=CLR_GREEN;
  else if(currentStatus=="降落") sc=CLR_AMBER;

  bool hasReason=(currentStatus=="降落"&&landingTimeStr.length()>0&&landingReason.length()>0);
  M5.Display.setTextDatum(middle_center);

  // 塔台來訊時間放在內容上方（跟 line 一樣）
  if(everReceivedCommand && lastMessageTime.length()>0){
    fXs(); M5.Display.setTextColor(CLR_GRAY);
    M5.Display.drawString("塔台 "+lastMessageTime+" 來訊",160,118);
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
  M5.Display.drawString("塔台指令",160,45);
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
  M5.Display.drawString("塔台訊息",160,45);
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
  fLg(); M5.Display.setTextColor(CLR_ACCENT); M5.Display.drawString("更多功能",160,28);

  if(pilotMode==MODE_MASTER){
    // 結束任務
    M5.Display.fillRoundRect(20,48,280,38,10,CLR_SURFACE); M5.Display.drawRoundRect(20,48,280,38,10,CLR_GRAY);
    fSm(); M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("結束任務",160,67);
    // GPS 開關
    M5.Display.fillRoundRect(20,90,280,38,10,CLR_SURFACE); M5.Display.drawRoundRect(20,90,280,38,10,CLR_GRAY);
    M5.Display.setTextColor(gpsEnabled?CLR_GREEN:CLR_WHITE);
    M5.Display.drawString(String("GPS：")+(gpsEnabled?"開啟":"關閉"),160,109);
    // 關機（結束任務前不可用）
    uint16_t poColor=sessionEnded?CLR_RED:CLR_GRAY;
    M5.Display.fillRoundRect(20,132,280,38,10,CLR_SURFACE); M5.Display.drawRoundRect(20,132,280,38,10,poColor);
    if(sessionEnded){ M5.Display.setTextColor(poColor); M5.Display.drawString("關機",160,151); }
    else {
      fSm(); M5.Display.setTextColor(poColor); M5.Display.drawString("關機",160,145);
      fXs(); M5.Display.drawString("請先結束任務",160,161);
    }
    // 返回選擇模式
    fSm(); M5.Display.fillRoundRect(20,174,280,38,10,CLR_SURFACE); M5.Display.drawRoundRect(20,174,280,38,10,CLR_GRAY);
    M5.Display.setTextColor(CLR_WHITE); M5.Display.drawString("返回選擇模式",160,193);
  } else {
    M5.Display.fillRoundRect(20,80,280,50,10,CLR_SURFACE); M5.Display.drawRoundRect(20,80,280,50,10,CLR_RED);
    fSm(); M5.Display.setTextColor(CLR_RED); M5.Display.drawString("關機",160,105);
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

void showEndMsg(){
  M5.Display.fillRect(40,100,240,36,CLR_SURFACE);
  fSm(); M5.Display.setTextDatum(middle_center); M5.Display.setTextColor(CLR_GREEN);
  M5.Display.drawString("已通知塔台",160,118); delay(2000);
  drawIdle(); drawPoweroffConfirm();
}

// ── 倒數更新 ──────────────────────────────────────────────────────────────────
void updateClock(){
  M5.Display.fillRect(0,0,100,32,CLR_DARK);
  fXs(); M5.Display.setTextDatum(middle_left); M5.Display.setTextColor(CLR_WHITE);
  M5.Display.drawString(getNowTime(),6,16);
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
  if(WiFi.status()!=WL_CONNECTED){ wsConnecting=false; WiFi.begin(savedSSID.c_str(),savedPassword.c_str()); return; }
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
    StaticJsonDocument<64> doc; doc["type"]="pilot_land_report"; doc["pilotName"]=pilotName;
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
  fXs(); M5.Display.setTextColor(CLR_AMBER); M5.Display.drawString("飛航公告號碼 (U+4碼)",160,10);
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
  int keys[]={1,2,3,4,5,6,7,8,9,-1,0,-2};
  int kx=10,ky=58,kw=94,kh=40,gap=4;
  for(int i=0;i<12;i++){
    int col=i%3,row=i/3,x=kx+col*(kw+gap),y=ky+row*(kh+gap);
    if(tx>=x&&tx<=x+kw&&ty>=y&&ty<=y+kh){
      if(keys[i]==-1){keypadBuffer="";drawKeypad();return;}
      if(keys[i]==-2){
        if(keypadBuffer.length()==4){
          notamCode="U"+keypadBuffer;
          StaticJsonDocument<128> doc; doc["type"]="pilot_notam"; doc["notam"]=notamCode; doc["pilotName"]=pilotName;
          String o;serializeJson(doc,o);wsClient.sendTXT(o);
          keypadMode=KP_NONE;
          if(notamHadValue){ currentScreen=SCR_TURNPOINT_CONFIRM; drawTurnpointConfirm(); beep2(); }
          else { drawIdle(); beep2(); }
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
    StaticJsonDocument<128> doc; doc["type"]=IS_FOLLOWER_CONN?"follower_rename":"pilot_rename"; doc["name"]=pilotName;
    String o; serializeJson(doc,o); wsClient.sendTXT(o);
    currentScreen=SCR_IDLE; drawIdle();
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
  if((currentScreen==SCR_IDLE||currentScreen==SCR_COMMAND)&&tx>=206&&tx<=246&&ty>=4&&ty<=28){ drawMoreMenu(); return; }

  if(currentScreen==SCR_WIFI_SCAN){ handleWifiListTouch(tx,ty); return; }
  if(currentScreen==SCR_NAME_INPUT||currentScreen==SCR_WIFI_PASS||currentScreen==SCR_FOLLOWER_CODE){ handleKeyboardTouch(tx,ty); return; }
  if(currentScreen==SCR_MODE_SELECT){
    if(ty>=36&&ty<60){ kbBuffer=pilotName; kbHint="更改飛手名字（英文小寫）"; kbTarget="rename_mode"; kbShift=false; kbPage=0; kbMaxLen=10; currentScreen=SCR_NAME_INPUT; drawKeyboard(); return; }
    if(ty>=62&&ty<=118){ pilotMode=MODE_MASTER; connectWebSocket(); }
    else if(ty>=124&&ty<=180){ pilotMode=MODE_FOLLOWER; gpsEnabled=false; gpsFixed=false; drawFollowerInput(); }
    else if(ty>=186&&ty<=238){ pilotMode=MODE_GATHER; gpsEnabled=false; gpsFixed=false; drawFollowerInput(); }
    return;
  }
  if(currentScreen==SCR_IDLE){
    // WiFi 狀態燈：點擊可重新選擇 WiFi
    if(tx>=246&&tx<=264&&ty>=4&&ty<=28){ drawWifiChangeConfirm(); return; }
    // 名字觸控改名（主控）
    if(tx>80&&tx<188&&ty<32&&pilotMode==MODE_MASTER){ kbBuffer=pilotName; kbHint="更改飛手名字（英文小寫）"; kbTarget="rename"; kbShift=false; kbPage=0; kbMaxLen=10; currentScreen=SCR_NAME_INPUT; drawKeyboard(); return; }
    // GPS
    if(tx>232&&tx<314&&ty>34&&ty<56){ if(pilotMode==MODE_MASTER){ gpsEnabled=!gpsEnabled; if(!gpsEnabled)gpsFixed=false; drawGpsBtn(); sendHeartbeat(); } }
    // 公告
    if(tx<230&&ty>58&&ty<78&&pilotMode==MODE_MASTER){ notamHadValue=notamCode.length()>0; keypadMode=KP_NOTAM; keypadBuffer=notamCode.length()>0?notamCode.substring(1):""; drawKeypad(); return; }
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
    if(tx<160){
      if(powerOffReturnScreen==SCR_WIFI_SCAN){ currentScreen=SCR_WIFI_SCAN; drawWifiList(); }
      else if(powerOffReturnScreen==SCR_COMMAND){ currentScreen=SCR_COMMAND; drawCommand(); }
      else { currentScreen=SCR_IDLE; drawIdle(); }
    }
    else { doPoweroff(); }
  }
  else if(currentScreen==SCR_WIFI_CHANGE_CONFIRM){
    if(tx<160){ currentScreen=SCR_IDLE; drawIdle(); }
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
    if(tx<230&&ty>58&&ty<78&&pilotMode==MODE_MASTER){ notamHadValue=notamCode.length()>0; keypadMode=KP_NOTAM; keypadBuffer=notamCode.length()>0?notamCode.substring(1):""; drawKeypad(); return; }
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
    if(keypadMode!=KP_NONE){keypadMode=KP_NONE;drawIdle();return;}
    if(currentScreen==SCR_NAME_INPUT&&kbTarget=="rename"){ currentScreen=SCR_IDLE; drawIdle(); return; }
    if(currentScreen==SCR_NAME_INPUT&&kbTarget=="rename_mode"){ currentScreen=SCR_MODE_SELECT; drawModeSelect(); return; }
    if(currentScreen==SCR_WIFI_PASS){ currentScreen=SCR_WIFI_SCAN; drawWifiList(); return; }
    if(currentScreen==SCR_FOLLOWER_CODE){ pilotMode=MODE_NONE; currentScreen=SCR_MODE_SELECT; drawModeSelect(); return; }
    if(currentScreen==SCR_TURNPOINT_CONFIRM){ currentScreen=SCR_IDLE; drawIdle(); return; }
    if(currentScreen==SCR_POWEROFF_CONFIRM){
      if(powerOffReturnScreen==SCR_WIFI_SCAN){ currentScreen=SCR_WIFI_SCAN; drawWifiList(); }
      else if(powerOffReturnScreen==SCR_COMMAND){ currentScreen=SCR_COMMAND; drawCommand(); }
      else { currentScreen=SCR_IDLE; drawIdle(); }
      return;
    }
    if(currentScreen==SCR_WIFI_CHANGE_CONFIRM){ currentScreen=SCR_IDLE; drawIdle(); return; }
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
    drawCharging(); bool doboot=false;
    while(!doboot){
      M5.update();
      if(M5.BtnA.wasClicked()||M5.BtnB.wasClicked()||M5.BtnC.wasClicked()||M5.BtnPWR.wasClicked()) doboot=true;
      if(!M5.Power.isCharging()){ M5.Display.setBrightness(0);
        unsigned long unplugAt=millis();
        while(true){ M5.update();
          if(M5.BtnA.wasClicked()||M5.BtnB.wasClicked()||M5.BtnC.wasClicked()||M5.BtnPWR.wasClicked()){doboot=true;break;}
          if(M5.Power.isCharging()){ M5.Display.setBrightness(BRIGHT_VAL[brightnessLevel]);drawCharging();break; }
          // 保險逾時：避免拔線瞬間讀值不穩、按鍵在螢幕全暗狀態下沒被偵測到，導致卡住開不了機
          if(millis()-unplugAt>6000){ M5.Display.setBrightness(BRIGHT_VAL[brightnessLevel]); doboot=true; break; }
          delay(100); }
        if(doboot) break; }
      delay(100); static unsigned long lu=0; if(millis()-lu>1000){drawCharging();lu=millis();}
    }
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
