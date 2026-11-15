// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "mtas_text.h"
#include <array>
#include <string_view>

namespace mtas {
// IDs only: messages are decoded from the player's romfs, never bundled here.
inline constexpr int UiGameIds[]{1022,1023,1025,1039,1042,1078,1079,1080,1112,1121,1126,1166,1186,1218,1269,1270,1271,1272,1273,1274,1275,1284,1285,1286,1287,1288,1291,1336,1370,1373,1520,1644,1645,1646,1703,1778,1819,1966,2044,2048,2049,2050,2051,2171,2172,2236,2334,2707,2991,3046,3847,3848,4290,7031,12000,12001,20263,52005};
struct UiOwnLabel { const char* key; std::array<const char*, 13> words; };
// OptionMgr enum order; unshipped Turkish uses English, as LanguageBundle does.
inline constexpr UiOwnLabel UiOwnLabels[]{
    {"in_days", {"{0}天后", "in {0} days", "in {0} Tagen", "dans {0} jours", "{0}天後", "tra {0} giorni", "en {0} días", "あと{0}日", "через {0} дн.", "in {0} days", "{0}일 후", "อีก {0} วัน", "em {0} dias"}},
    {"birthday_date", {"{1}{0}日", "Day {0} of {1}", "Tag {0} im {1}", "Jour {0} de {1}", "{1}{0}日", "Giorno {0} di {1}", "Día {0} de {1}", "{1}の{0}日", "{0}, {1}", "Day {0} of {1}", "{1} {0}일", "วันที่ {0} {1}", "Dia {0} de {1}"}},
    {"not_gifted", {"今日未送礼", "Not gifted today", "Heute nicht beschenkt", "Pas de cadeau aujourd’hui", "今日未送禮", "Nessun regalo oggi", "Sin regalo hoy", "今日は未贈呈", "Без подарка сегодня", "Not gifted today", "오늘 선물 안 함", "ยังไม่ให้ของขวัญวันนี้", "Sem presente hoje"}},
    {"liked_by", {"喜欢的人", "Liked by", "Beliebt bei", "Aimé par", "喜歡的人", "Piace a", "Le gusta a", "好きな住民", "Кому нравится", "Liked by", "좋아하는 주민", "ผู้ที่ชอบ", "Quem gosta"}},
    {"needed_for", {"用于", "Needed for", "Benötigt für", "Nécessaire pour", "用於", "Necessario per", "Necesario para", "必要な用途", "Для чего нужно", "Needed for", "필요한 곳", "จำเป็นสำหรับ", "Necessário para"}},
    {"you_own", {"拥有", "You own", "Im Besitz", "En votre possession", "擁有", "Posseduti", "Tienes", "所持数", "В наличии", "You own", "보유 수", "ที่มี", "Em posse"}},
    {"not_yet", {"尚未", "Not yet", "Noch nicht", "Pas encore", "尚未", "Non ancora", "Aún no", "まだ", "Ещё нет", "Not yet", "아직", "ยังไม่", "Ainda não"}},
    {"chatted", {"已聊天", "Chatted", "Gesprochen", "Discussion faite", "已聊天", "Conversato", "Conversado", "会話済み", "Поговорили", "Chatted", "대화 완료", "คุยแล้ว", "Conversou"}},
    {"gifted", {"已送礼", "Gifted", "Beschenkt", "Cadeau offert", "已送禮", "Regalato", "Regalo dado", "贈呈済み", "Подарок вручён", "Gifted", "선물 완료", "ให้แล้ว", "Presente dado"}},
    {"ready", {"可制作", "Ready", "Bereit", "Prêt", "可製作", "Pronto", "Listo", "準備完了", "Готово", "Ready", "준비 완료", "พร้อม", "Pronto"}},
    {"likes", {"喜好", "Likes", "Vorlieben", "Préférences", "喜好", "Preferenze", "Gustos", "好み", "Предпочтения", "Likes", "선호", "ของที่ชอบ", "Preferências"}},
    {"companion", {"伴侣屏幕", "Companion", "Begleiter", "Compagnon", "伴侶螢幕", "Compagno", "Compañero", "コンパニオン", "Компаньон", "Companion", "동반 화면", "เพื่อนร่วมทาง", "Companheiro"}},
    {"birthday", {"生日", "Birthday", "Geburtstag", "Anniversaire", "生日", "Compleanno", "Cumpleaños", "誕生日", "День рождения", "Birthday", "생일", "วันเกิด", "Aniversário"}},

    {"more", {"更多", "more", "weitere", "de plus", "更多", "altri", "más", "件ほか", "ещё", "more", "더", "เพิ่มเติม", "mais"}},
    {"indoors", {"室内", "Indoors", "Innenbereich", "Intérieur", "室內", "Interno", "Interior", "屋内", "В помещении", "Indoors", "실내", "ในอาคาร", "Interior"}},
    {"ok", {"正常", "OK", "OK", "OK", "正常", "OK", "OK", "正常", "В норме", "OK", "정상", "ปกติ", "OK"}},
    {"overdue", {"已逾期", "Overdue", "Überfällig", "En retard", "已逾期", "Scaduta", "Vencida", "期限切れ", "Просрочено", "Overdue", "기한 초과", "เกินกำหนด", "Vencida"}},
    {"rep_to_next", {"距下一级所需声望", "Rep to next rank", "Ruf bis zum nächsten Rang", "Réputation avant le prochain rang", "距下一級所需聲望", "Reputazione al prossimo grado", "Reputación al siguiente rango", "次のランクまでの評判", "Репутация до следующего ранга", "Rep to next rank", "다음 등급까지 평판", "ชื่อเสียงถึงอันดับถัดไป", "Reputação para o próximo nível"}},
    {"birthday_week", {"本周生日", "Birthday this week", "Geburtstag diese Woche", "Anniversaire cette semaine", "本週生日", "Compleanno questa settimana", "Cumpleaños esta semana", "今週の誕生日", "Дни рождения на этой неделе", "Birthday this week", "이번 주 생일", "วันเกิดสัปดาห์นี้", "Aniversário nesta semana"}},
    {"no_met", {"尚未认识镇民", "No one met yet", "Noch niemanden getroffen", "Aucune rencontre pour le moment", "尚未認識鎮民", "Nessuno ancora incontrato", "Aún no conoces a nadie", "まだ誰とも出会っていません", "Пока ни с кем не знакомы", "No one met yet", "아직 만난 주민 없음", "ยังไม่พบใคร", "Ainda não conheceu ninguém"}},
    {"no_selected", {"未选择镇民", "No townsfolk selected", "Niemand ausgewählt", "Aucun habitant sélectionné", "未選擇鎮民", "Nessun abitante selezionato", "Ningún habitante seleccionado", "住民が選択されていません", "Житель не выбран", "No townsfolk selected", "선택한 주민 없음", "ยังไม่ได้เลือกชาวเมือง", "Nenhum morador selecionado"}},
    {"inventory_hint", {"随身物品将显示在这里", "Items you're carrying will show up here", "Mitgeführte Gegenstände erscheinen hier", "Les objets transportés apparaîtront ici", "隨身物品將顯示在這裡", "Gli oggetti trasportati appariranno qui", "Aquí aparecerán los objetos que llevas", "持ち歩いているアイテムをここに表示します", "Здесь появятся предметы в сумке", "Items you're carrying will show up here", "소지한 아이템이 여기에 표시됩니다", "สิ่งของที่พกจะแสดงที่นี่", "Os itens que carrega aparecerão aqui"}},
    {"social_hint", {"认识的镇民将显示在这里", "Townsfolk you've met will show up here", "Bekannte Einwohner erscheinen hier", "Les habitants rencontrés apparaîtront ici", "認識的鎮民將顯示在這裡", "Gli abitanti incontrati appariranno qui", "Aquí aparecerán los habitantes que conoces", "出会った住民をここに表示します", "Здесь появятся знакомые жители", "Townsfolk you've met will show up here", "만난 주민이 여기에 표시됩니다", "ชาวเมืองที่พบจะแสดงที่นี่", "Os moradores que conheceu aparecerão aqui"}},
    {"select_hint", {"在交友页面选择一位镇民", "Tap someone on the Social page", "Auf der Sozialseite jemanden antippen", "Touchez un habitant sur la page des relations", "在交友頁面選擇一位鎮民", "Tocca qualcuno nella pagina Sociale", "Toca a alguien en la página Social", "交友ページで住民を選んでください", "Выберите жителя на странице общения", "Tap someone on the Social page", "교우 페이지에서 주민을 선택하세요", "แตะชาวเมืองในหน้าสังคม", "Toque em alguém na página Social"}},
    {"rank_hint", {"排名与声望将显示在这里", "Rank and reputation will show up here", "Rang und Ruf erscheinen hier", "Le classement et la réputation apparaîtront ici", "排名與聲望將顯示在這裡", "Grado e reputazione appariranno qui", "Aquí aparecerán el rango y la reputación", "ランキングと評判をここに表示します", "Здесь появятся ранг и репутация", "Rank and reputation will show up here", "순위와 평판이 여기에 표시됩니다", "อันดับและชื่อเสียงจะแสดงที่นี่", "O nível e a reputação aparecerão aqui"}},
    {"loading", {"加载中", "Loading", "Laden", "Chargement", "載入中", "Caricamento", "Cargando", "読み込み中", "Загрузка", "Loading", "불러오는 중", "กำลังโหลด", "Carregando"}},
    {"unavailable", {"暂无数据", "Unavailable", "Nicht verfügbar", "Indisponible", "暫無資料", "Non disponibile", "No disponible", "データなし", "Нет данных", "Unavailable", "데이터 없음", "ไม่มีข้อมูล", "Indisponível"}},
    {"clock_top", {"上屏时钟与日期", "Clock & date on top", "Uhr und Datum oben", "Horloge et date en haut", "上螢幕時鐘與日期", "Ora e data in alto", "Reloj y fecha arriba", "上画面に時計と日付", "Часы и дата сверху", "Clock & date on top", "위 화면 시계와 날짜", "นาฬิกาและวันที่จอบน", "Relógio e data em cima"}},
    {"weather_season", {"含天气与季节", "Weather & season included", "Mit Wetter und Jahreszeit", "Avec météo et saison", "含天氣與季節", "Include meteo e stagione", "Incluye clima y estación", "天気と季節を含む", "С погодой и сезоном", "Weather & season included", "날씨와 계절 포함", "รวมอากาศและฤดูกาล", "Inclui clima e estação"}},
    {"tracker_top", {"上屏任务追踪", "Mission tracker on top", "Missionsanzeige oben", "Suivi de mission en haut", "上螢幕任務追蹤", "Missione in alto", "Seguimiento arriba", "上画面にミッション追跡", "Задания сверху", "Mission tracker on top", "위 화면 임무 추적", "ติดตามภารกิจจอบน", "Missão em cima"}},
    {"minimap_top", {"小地图保留在上屏", "Minimap stays on top", "Minikarte bleibt oben", "Mini-carte en haut", "小地圖保留在上螢幕", "Minimappa in alto", "Minimapa arriba", "ミニマップは上画面", "Мини-карта сверху", "Minimap stays on top", "미니맵은 위 화면에 유지", "แผนที่ย่ออยู่จอบน", "Minimapa em cima"}},
    {"dialogue_card", {"对话人物卡片", "Character card in dialogue", "Figurenkarte im Dialog", "Fiche pendant le dialogue", "對話人物卡片", "Scheda nel dialogo", "Ficha durante el diálogo", "会話中の人物カード", "Карточка при диалоге", "Character card in dialogue", "대화 중 인물 카드", "การ์ดตัวละครขณะสนทนา", "Ficha durante o diálogo"}},
    {"auto_card", {"自动显示对话人物", "Open speaker card", "Karte des Sprechers öffnen", "Ouvrir la fiche du personnage", "自動顯示對話人物", "Apri scheda interlocutore", "Abrir ficha del personaje", "話し相手のカードを開く", "Открывать карточку собеседника", "Open speaker card", "대화 상대 카드 열기", "เปิดการ์ดคู่สนทนา", "Abrir ficha do personagem"}},
    {"haptics", {"触感反馈", "Touch feedback", "Vibration", "Retour tactile", "觸感回饋", "Feedback tattile", "Respuesta háptica", "触覚フィードバック", "Тактильный отклик", "Touch feedback", "터치 진동", "การตอบสนองแบบสั่น", "Resposta tátil"}},
    {"reduced_motion", {"减少动态效果", "Reduced motion", "Weniger Bewegung", "Animations réduites", "減少動態效果", "Movimento ridotto", "Movimiento reducido", "動きを減らす", "Меньше движения", "Reduced motion", "동작 줄이기", "ลดการเคลื่อนไหว", "Movimento reduzido"}},
    {"saved", {"自动保存设置", "Settings saved automatically", "Automatisch gespeichert", "Réglages enregistrés automatiquement", "自動儲存設定", "Impostazioni salvate automaticamente", "Ajustes guardados automáticamente", "設定は自動保存", "Настройки сохраняются автоматически", "Settings saved automatically", "설정 자동 저장", "บันทึกการตั้งค่าอัตโนมัติ", "Configurações salvas automaticamente"}},
};
inline std::string ResolveUi(const Text& text, std::string_view key) {
    for (int id : UiGameIds)
        if (key == std::to_string(id))
            return text.Get(id);
    for (const auto& row : UiOwnLabels)
        if (key == row.key) {
            const auto lang = text.CurrentLanguage();
            return row.words[lang >= 0 && lang < 13 ? lang : 1];
        }
    return {};
}
inline std::string Ui(std::string_view key) { return ResolveUi(GameText(), key); }
inline std::string UiFormat(std::string value, const std::vector<std::string>& args) {
    // Substitute once so braces in names cannot become format placeholders.
    std::string out;
    for (std::size_t i = 0; i < value.size();) {
        if (i + 2 < value.size() && value[i] == '{' && value[i+2] == '}' &&
            value[i+1] >= '0' && value[i+1] <= '9' &&
            static_cast<std::size_t>(value[i+1] - '0') < args.size()) {
            out += args[value[i+1] - '0'];
            i += 3;
        } else {
            out += value[i++];
        }
    }
    return out;
}
} // namespace mtas
