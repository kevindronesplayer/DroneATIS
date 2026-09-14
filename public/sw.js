// 這是即時 WebSocket 儀表板，不做任何快取——快取到舊的 index.html/server 邏輯
// 會讓塔台看到過期畫面卻不自知，比沒有 PWA 更危險。這支 service worker
// 存在只是為了讓瀏覽器判定可安裝（Add to Home Screen / 安裝提示），並保留未來
// 加 Push 通知的掛載點。
self.addEventListener('install', () => { self.skipWaiting(); });
self.addEventListener('activate', (e) => { e.waitUntil(self.clients.claim()); });
self.addEventListener('fetch', (e) => { e.respondWith(fetch(e.request)); });
