// 這是即時 WebSocket 儀表板，不做任何快取——快取到舊的 index.html/server 邏輯
// 會讓塔台看到過期畫面卻不自知，比沒有 PWA 更危險。這支 service worker
// 主要負責兩件事：讓瀏覽器判定可安裝（Add to Home Screen / 安裝提示），
// 以及接收 App 完全關閉時也能送達的背景推播（push）。
self.addEventListener('install', () => { self.skipWaiting(); });
self.addEventListener('activate', (e) => { e.waitUntil(self.clients.claim()); });
self.addEventListener('fetch', (e) => { e.respondWith(fetch(e.request)); });

// App 完全關閉時，是瀏覽器在背景把 service worker 叫醒來處理這個事件，
// 不是我們自己的頁面在跑——所以通知內容要嵌在 push payload 裡，不能依賴頁面裡的任何狀態
self.addEventListener('push', (e) => {
  let data = {};
  try { data = e.data ? e.data.json() : {}; } catch (err) {}
  const title = data.title || 'DroneATIS';
  const opts = {
    body: data.body || '',
    icon: data.icon || '/icons/icon-192.png',
    badge: data.badge || '/icons/icon-192.png',
    tag: data.tag || 'droneatis-push',
    renotify: true,
  };
  e.waitUntil(self.registration.showNotification(title, opts));
});

self.addEventListener('notificationclick', (e) => {
  e.notification.close();
  e.waitUntil(
    self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then((list) => {
      for (const c of list) { if ('focus' in c) return c.focus(); }
      if (self.clients.openWindow) return self.clients.openWindow('/');
    })
  );
});
