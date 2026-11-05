/* EMO web i18n — vi | en | it | zh (wired-style). Preference in localStorage. */
(function (global) {
  'use strict';
  const SUPPORTED = ['vi', 'en', 'it', 'zh'];
  const STORAGE_KEY = 'emo.ui.lang';
  let currentLang = 'vi';

  const STRINGS = {
    vi: {
      'doc.title': 'EMO · Cài đặt',
      'header.title': 'EMO · Cài đặt',
      'lang.label': 'Ngôn ngữ',
      'nav.wifi': '📶 Wi‑Fi',
      'nav.status': '📊 Trạng thái',
      'nav.daily': '🕒 Daily',
      'nav.acts': '🎬 Hoạt cảnh',
      'nav.cfg': '⚙️ Xiaozhi & Cấu hình',
      'nav.motion': '🦵 Chân / Motion',
      'wifi.h2': 'Bắt Wi‑Fi',
      'wifi.intro': 'Chọn mạng nhà, nhập mật khẩu, bấm Lưu. Robot reboot và nối STA.',
      'wifi.scan': 'Quét mạng',
      'wifi.ssid': 'SSID',
      'wifi.pass': 'Mật khẩu',
      'wifi.save': 'Lưu & kết nối',
      'wifi.hint_ap': 'Điện thoại đang nối hotspot robot. Chọn Wi‑Fi nhà, nhập mật khẩu, bấm Lưu & kết nối.',
      'status.h2': 'Trạng thái',
      'status.intro': 'Pin, liên kết K210, chế độ hiện tại.',
      'status.dock': 'Đang trên sạc: chỉ phát âm thanh + hoạt cảnh mắt — chân bị khoá để không té.',
      'status.tone': 'Test loa',
      'status.mic': 'Record mic 3s → WAV',
      'daily.h2': 'Lịch sinh hoạt Daily',
      'daily.intro': 'Chỉnh giờ / hoạt cảnh theo khung giờ. Robot tự chạy khi đến giờ (SNTP). Self‑control daily vẫn random ngay.',
      'daily.enable': 'Bật lịch theo giờ',
      'daily.tz': 'Múi giờ UTC',
      'daily.fake': 'Giờ giả (test, HHMM, −1 = tắt)',
      'daily.now': 'Hiện tại / tiếp theo',
      'daily.add': '+ Thêm khung',
      'daily.save': 'Lưu lịch',
      'daily.reset': 'Khôi phục gốc',
      'acts.h2': 'Hoạt cảnh Daily',
      'acts.intro': 'Chạm để chạy ngay (mặt + tiếng; chân chỉ khi không sạc).',
      'acts.random': 'Random 1 hoạt cảnh',
      'cfg.h2': 'Xiaozhi & Cấu hình',
      'cfg.intro': 'Ghép cặp Xiaozhi và các tuỳ chọn robot trong một chỗ.',
      'cfg.xz_help': '1) Voice backend = Xiaozhi + Save. 2) Có internet. 3) Lấy mã pair. 4) Vào xiaozhi.me thêm thiết bị.',
      'cfg.pair': 'Lấy mã pair',
      'cfg.copy': 'Copy mã',
      'cfg.dismiss': 'Đã pair — ẩn mã',
      'cfg.voice': 'Voice backend',
      'cfg.speed': 'Motion speed %',
      'cfg.ota': 'Xiaozhi OTA URL',
      'cfg.ws': 'Xiaozhi WS URL',
      'cfg.token': 'Xiaozhi token',
      'cfg.emoa_ip': 'EMOA IP',
      'cfg.emoa_port': 'EMOA port',
      'cfg.docked': 'Docked / đứng yên',
      'cfg.explore': 'Auto explore',
      'cfg.idle': 'Idle faces',
      'cfg.cliff': 'Cliff interrupt',
      'cfg.pet': 'Petting',
      'cfg.yolo': 'YOLO react',
      'cfg.save': 'Save',
      'cfg.power': 'Tắt nguồn robot',
      'motion.h2': 'Chân / Motion',
      'motion.intro': 'Test gait và file .mot. Tắt Docked / rút sạc trước khi đi.',
      'motion.dock': 'Đang sạc — nút walk/motion bị khoá.',
      'motion.explore': 'Explore 20s',
      'motion.stop': 'Stop',
      'motion.wiggle': 'Wiggle',
      'motion.play': 'Play',
      'motion.loop': 'Loop',
      'motion.release': 'Servos off',
      'motion.file': 'Motion file',
      'motion.speed': 'Speed × / options',
    },
    en: {
      'doc.title': 'EMO · Settings',
      'header.title': 'EMO · Settings',
      'lang.label': 'Language',
      'nav.wifi': '📶 Wi‑Fi',
      'nav.status': '📊 Status',
      'nav.daily': '🕒 Daily',
      'nav.acts': '🎬 Scenes',
      'nav.cfg': '⚙️ Xiaozhi & Config',
      'nav.motion': '🦵 Legs / Motion',
      'wifi.h2': 'Connect Wi‑Fi',
      'wifi.intro': 'Pick your network, enter the password, tap Save. The robot reboots and joins STA.',
      'wifi.scan': 'Scan networks',
      'wifi.ssid': 'SSID',
      'wifi.pass': 'Password',
      'wifi.save': 'Save & connect',
      'wifi.hint_ap': 'Phone is on the robot hotspot. Choose home Wi‑Fi, enter password, Save & connect.',
      'status.h2': 'Status',
      'status.intro': 'Battery, K210 link, current mode.',
      'status.dock': 'On charger: sound + face only — legs locked so it will not tip over.',
      'status.tone': 'Test speaker',
      'status.mic': 'Record mic 3s → WAV',
      'daily.h2': 'Daily schedule',
      'daily.intro': 'Edit time slots and scenes. Runs automatically (SNTP). Self‑control daily still picks a random scene now.',
      'daily.enable': 'Enable timed schedule',
      'daily.tz': 'Timezone UTC',
      'daily.fake': 'Fake time (test, HHMM, −1 = off)',
      'daily.now': 'Now / next',
      'daily.add': '+ Add slot',
      'daily.save': 'Save schedule',
      'daily.reset': 'Restore defaults',
      'acts.h2': 'Daily scenes',
      'acts.intro': 'Tap to run now (face + sound; legs only off charger).',
      'acts.random': 'Random one scene',
      'cfg.h2': 'Xiaozhi & Config',
      'cfg.intro': 'Pair Xiaozhi and robot options in one place.',
      'cfg.xz_help': '1) Voice backend = Xiaozhi + Save. 2) Internet. 3) Get pair code. 4) Add device on xiaozhi.me.',
      'cfg.pair': 'Get pair code',
      'cfg.copy': 'Copy code',
      'cfg.dismiss': 'Paired — hide code',
      'cfg.voice': 'Voice backend',
      'cfg.speed': 'Motion speed %',
      'cfg.ota': 'Xiaozhi OTA URL',
      'cfg.ws': 'Xiaozhi WS URL',
      'cfg.token': 'Xiaozhi token',
      'cfg.emoa_ip': 'EMOA IP',
      'cfg.emoa_port': 'EMOA port',
      'cfg.docked': 'Docked / stand still',
      'cfg.explore': 'Auto explore',
      'cfg.idle': 'Idle faces',
      'cfg.cliff': 'Cliff interrupt',
      'cfg.pet': 'Petting',
      'cfg.yolo': 'YOLO react',
      'cfg.save': 'Save',
      'cfg.power': 'Power off robot',
      'motion.h2': 'Legs / Motion',
      'motion.intro': 'Test gait and .mot files. Undock / uncheck Docked before walking.',
      'motion.dock': 'On charger — walk/motion locked.',
      'motion.explore': 'Explore 20s',
      'motion.stop': 'Stop',
      'motion.wiggle': 'Wiggle',
      'motion.play': 'Play',
      'motion.loop': 'Loop',
      'motion.release': 'Servos off',
      'motion.file': 'Motion file',
      'motion.speed': 'Speed × / options',
    },
    it: {
      'doc.title': 'EMO · Impostazioni',
      'header.title': 'EMO · Impostazioni',
      'lang.label': 'Lingua',
      'nav.wifi': '📶 Wi‑Fi',
      'nav.status': '📊 Stato',
      'nav.daily': '🕒 Daily',
      'nav.acts': '🎬 Scene',
      'nav.cfg': '⚙️ Xiaozhi e Config',
      'nav.motion': '🦵 Gambe / Motion',
      'wifi.h2': 'Connetti Wi‑Fi',
      'wifi.intro': 'Scegli la rete, inserisci la password, Salva. Il robot riavvia e si collega in STA.',
      'wifi.scan': 'Scansiona reti',
      'wifi.ssid': 'SSID',
      'wifi.pass': 'Password',
      'wifi.save': 'Salva e connetti',
      'wifi.hint_ap': 'Il telefono è sull’hotspot del robot. Scegli il Wi‑Fi di casa, password, Salva e connetti.',
      'status.h2': 'Stato',
      'status.intro': 'Batteria, collegamento K210, modalità attuale.',
      'status.dock': 'In carica: solo audio + volto — gambe bloccate per non cadere.',
      'status.tone': 'Test altoparlante',
      'status.mic': 'Registra mic 3s → WAV',
      'daily.h2': 'Programma Daily',
      'daily.intro': 'Modifica orari e scene. Parte in automatico (SNTP). Self‑control daily sceglie subito una scena a caso.',
      'daily.enable': 'Attiva programma orario',
      'daily.tz': 'Fuso UTC',
      'daily.fake': 'Ora finta (test, HHMM, −1 = off)',
      'daily.now': 'Ora / prossimo',
      'daily.add': '+ Aggiungi slot',
      'daily.save': 'Salva programma',
      'daily.reset': 'Ripristina default',
      'acts.h2': 'Scene Daily',
      'acts.intro': 'Tocca per eseguire ora (volto + audio; gambe solo fuori dalla base).',
      'acts.random': 'Una scena a caso',
      'cfg.h2': 'Xiaozhi e Config',
      'cfg.intro': 'Associa Xiaozhi e le opzioni del robot insieme.',
      'cfg.xz_help': '1) Voice backend = Xiaozhi + Save. 2) Internet. 3) Codice pair. 4) Aggiungi dispositivo su xiaozhi.me.',
      'cfg.pair': 'Ottieni codice',
      'cfg.copy': 'Copia codice',
      'cfg.dismiss': 'Associato — nascondi',
      'cfg.voice': 'Voice backend',
      'cfg.speed': 'Velocità motion %',
      'cfg.ota': 'Xiaozhi OTA URL',
      'cfg.ws': 'Xiaozhi WS URL',
      'cfg.token': 'Xiaozhi token',
      'cfg.emoa_ip': 'EMOA IP',
      'cfg.emoa_port': 'EMOA port',
      'cfg.docked': 'In base / fermo',
      'cfg.explore': 'Auto explore',
      'cfg.idle': 'Volti idle',
      'cfg.cliff': 'Interruzione cliff',
      'cfg.pet': 'Petting',
      'cfg.yolo': 'YOLO react',
      'cfg.save': 'Salva',
      'cfg.power': 'Spegni robot',
      'motion.h2': 'Gambe / Motion',
      'motion.intro': 'Test gait e file .mot. Togli dalla base prima di camminare.',
      'motion.dock': 'In carica — walk/motion bloccati.',
      'motion.explore': 'Explore 20s',
      'motion.stop': 'Stop',
      'motion.wiggle': 'Wiggle',
      'motion.play': 'Play',
      'motion.loop': 'Loop',
      'motion.release': 'Servos off',
      'motion.file': 'File motion',
      'motion.speed': 'Speed × / opzioni',
    },
    zh: {
      'doc.title': 'EMO · 设置',
      'header.title': 'EMO · 设置',
      'lang.label': '语言',
      'nav.wifi': '📶 Wi‑Fi',
      'nav.status': '📊 状态',
      'nav.daily': '🕒 Daily',
      'nav.acts': '🎬 场景',
      'nav.cfg': '⚙️ Xiaozhi 与配置',
      'nav.motion': '🦵 腿部 / 动作',
      'wifi.h2': '连接 Wi‑Fi',
      'wifi.intro': '选择家里网络，输入密码后保存。机器人会重启并连接 STA。',
      'wifi.scan': '扫描网络',
      'wifi.ssid': 'SSID',
      'wifi.pass': '密码',
      'wifi.save': '保存并连接',
      'wifi.hint_ap': '手机已连机器人热点。选择家里 Wi‑Fi，输入密码，保存并连接。',
      'status.h2': '状态',
      'status.intro': '电量、K210 连接、当前模式。',
      'status.dock': '充电中：仅声音+表情，腿部锁定以防摔倒。',
      'status.tone': '测试扬声器',
      'status.mic': '录音 3 秒 → WAV',
      'daily.h2': 'Daily 日程',
      'daily.intro': '编辑时间段与场景。到点自动播放（SNTP）。语音 self‑control daily 仍可立刻随机一个。',
      'daily.enable': '启用定时日程',
      'daily.tz': '时区 UTC',
      'daily.fake': '假时间（测试 HHMM，−1 关闭）',
      'daily.now': '现在 / 下一档',
      'daily.add': '+ 添加时段',
      'daily.save': '保存日程',
      'daily.reset': '恢复默认',
      'acts.h2': 'Daily 场景',
      'acts.intro': '点按立即播放（表情+声音；离座才动腿）。',
      'acts.random': '随机一个场景',
      'cfg.h2': 'Xiaozhi 与配置',
      'cfg.intro': '配对 Xiaozhi 与机器人选项放在一起。',
      'cfg.xz_help': '1) Voice backend = Xiaozhi 并保存。2) 联网。3) 获取配对码。4) 在 xiaozhi.me 添加设备。',
      'cfg.pair': '获取配对码',
      'cfg.copy': '复制配对码',
      'cfg.dismiss': '已配对 — 隐藏',
      'cfg.voice': 'Voice backend',
      'cfg.speed': '动作速度 %',
      'cfg.ota': 'Xiaozhi OTA URL',
      'cfg.ws': 'Xiaozhi WS URL',
      'cfg.token': 'Xiaozhi token',
      'cfg.emoa_ip': 'EMOA IP',
      'cfg.emoa_port': 'EMOA 端口',
      'cfg.docked': '底座 / 站立不动',
      'cfg.explore': '自动探索',
      'cfg.idle': '待机表情',
      'cfg.cliff': '悬崖中断',
      'cfg.pet': '抚摸',
      'cfg.yolo': 'YOLO 反应',
      'cfg.save': '保存',
      'cfg.power': '关闭机器人',
      'motion.h2': '腿部 / 动作',
      'motion.intro': '测试步态和 .mot。走路前请离开底座。',
      'motion.dock': '充电中 — 行走/动作已锁定。',
      'motion.explore': '探索 20 秒',
      'motion.stop': '停止',
      'motion.wiggle': '摆动',
      'motion.play': '播放',
      'motion.loop': '循环',
      'motion.release': '松开舵机',
      'motion.file': '动作文件',
      'motion.speed': '速度 × / 选项',
    }
  };

  function normalizeLang(lang) {
    lang = String(lang || '').toLowerCase().slice(0, 2);
    return SUPPORTED.indexOf(lang) >= 0 ? lang : 'vi';
  }
  function t(key) {
    const pack = STRINGS[currentLang] || STRINGS.vi;
    return pack[key] || (STRINGS.en[key] || key);
  }
  function applyI18n(root) {
    root = root || document;
    root.querySelectorAll('[data-i18n]').forEach(el => {
      const key = el.getAttribute('data-i18n');
      if (!key) return;
      const val = t(key);
      if (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA') {
        if (el.getAttribute('data-i18n-placeholder') != null) el.placeholder = val;
        else el.value = val;
      } else el.textContent = val;
    });
    root.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
      const key = el.getAttribute('data-i18n-placeholder');
      if (key) el.placeholder = t(key);
    });
    root.querySelectorAll('[data-i18n-title]').forEach(el => {
      const key = el.getAttribute('data-i18n-title');
      if (key) el.title = t(key);
    });
    document.title = t('doc.title');
    try { document.dispatchEvent(new CustomEvent('emo-i18n', { detail: { lang: currentLang } })); } catch (_) {}
  }
  function getLang() { return currentLang; }
  function setLang(lang) {
    currentLang = normalizeLang(lang);
    try { localStorage.setItem(STORAGE_KEY, currentLang); } catch (_) {}
    const sel = document.getElementById('uiLangSelect');
    if (sel && sel.value !== currentLang) sel.value = currentLang;
    applyI18n(document);
    return currentLang;
  }
  function initI18n() {
    let lang = 'vi';
    try { lang = localStorage.getItem(STORAGE_KEY) || lang; } catch (_) {}
    currentLang = normalizeLang(lang);
    const sel = document.getElementById('uiLangSelect');
    if (sel) {
      sel.value = currentLang;
      if (!sel._i18nBound) {
        sel._i18nBound = true;
        sel.addEventListener('change', () => setLang(sel.value));
      }
    }
    applyI18n(document);
    setTimeout(() => applyI18n(document), 50);
  }

  global.I18N = { t, getLang, setLang, apply: applyI18n, init: initI18n, STRINGS, SUPPORTED };
  global.t = t;
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', initI18n);
  else initI18n();
})(window);
