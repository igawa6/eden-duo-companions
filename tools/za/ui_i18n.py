# SPDX-FileCopyrightText: Copyright 2026 Sickops
# SPDX-License-Identifier: GPL-3.0-or-later
"""Companion-specific phrases. Ordinary game terms come from the player's message tables.

Columns: English, German, Spanish, Italian, Japanese, Korean, simplified/traditional Chinese.
LATAM uses its own game tables and these neutral Spanish companion phrases.
"""
LANG_COLUMN={2:0,5:1,7:2,4:3,1:4,8:5,9:6,10:7,11:2}
ROWS=r"""
VOYAGE RAPIDE|Fast travel|Schnellreise|Viaje rápido|Viaggio rapido|そらをとぶ|공중날기|飞翔|飛翔
Confirmez dans le jeu.|Confirm in the game.|Im Spiel bestätigen.|Confirma en el juego.|Conferma nel gioco.|ゲーム内で確認してください。|게임에서 확인하세요.|请在游戏内确认。|請在遊戲內確認。
Voyage indisponible pour le moment.|Fast travel is unavailable right now.|Schnellreise derzeit nicht verfügbar.|Viaje rápido no disponible ahora.|Viaggio rapido al momento non disponibile.|今はそらをとぶを使えません。|지금은 공중날기를 사용할 수 없습니다.|现在无法飞翔。|目前無法飛翔。
Destination indisponible.|Destination unavailable.|Ziel nicht verfügbar.|Destino no disponible.|Destinazione non disponibile.|目的地を選べません。|목적지를 선택할 수 없습니다.|目的地不可用。|目的地無法使用。
Petite taille|Small|Klein|Pequeña|Piccola|小さい|소형|小型|小型
Taille moyenne|Medium|Mittel|Mediana|Media|ふつう|중형|中型|中型
Grande taille|Large|Groß|Grande|Grande|大きい|대형|大型|大型
Pokémon possibles|Possible Pokémon|Mögliche Pokémon|Pokémon posibles|Pokémon possibili|出現するポケモン|출현 포켓몬|可能出现的宝可梦|可能出現的寶可夢
Combats de Dresseurs|Trainer battles|Trainerkämpfe|Combates de Entrenadores|Lotte di Allenatori|トレーナーとのバトル|트레이너 배틀|训练家对战|訓練家對戰
DIMENSIONS|Dimensions|Dimensionen|Dimensiones|Dimensioni|異次元|차원|异次元|異次元
RANG|Rank|Rang|Rango|Rango|ランク|랭크|级别|級別
OBJET TENU|Held item|Getragenes Item|Objeto equipado|Strumento tenuto|持ち物|지닌 도구|携带物品|攜帶物品
TOUS LES NIVEAUX|All levels|Alle Level|Todos los niveles|Tutti i livelli|全レベル|전체 레벨|全部等级|全部等級
NIVEAUX DE RECHERCHE|Research levels|Forschungslevel|Niveles de investigación|Livelli di ricerca|研究レベル|연구 레벨|研究等级|研究等級
POINTS CUMULÉS|Total points|Gesamtpunkte|Puntos totales|Punti totali|累計ポイント|누적 포인트|累计点数|累計點數
COMBATS NOCTURNES|Night battles|Nachtkämpfe|Combates nocturnos|Lotte notturne|夜のバトル|야간 배틀|夜间对战|夜間對戰
DIM. SAUVAGES|Wild dimensions|Wilde Dimensionen|Dim. salvajes|Dim. selvatiche|野生の異次元|야생 차원|野生异次元|野生異次元
DIM. COMBAT|Battle dimensions|Kampfdimensionen|Dim. de combate|Dim. di lotta|バトルの異次元|배틀 차원|对战异次元|對戰異次元
DIMENSION DE COMBAT|Battle dimension|Kampfdimension|Dimensión de combate|Dimensione di lotta|バトルの異次元|배틀 차원|对战异次元|對戰異次元
DIMENSION SAUVAGE|Wild dimension|Wilde Dimension|Dimensión salvaje|Dimensione selvatica|野生の異次元|야생 차원|野生异次元|野生異次元
Dimension active|Active dimension|Aktive Dimension|Dimensión activa|Dimensione attiva|出現中の異次元|활성 차원|活跃的异次元|出現中的異次元
RANG ∞|RANK ∞|RANG ∞|RANGO ∞|RANGO ∞|∞ランク|∞랭크|∞级|∞級
POINTS MANQUANTS|Points remaining|Fehlende Punkte|Puntos restantes|Punti mancanti|次までのポイント|남은 포인트|还需点数|還需點數
NIVEAU MAXIMUM|Maximum level|Höchstlevel|Nivel máximo|Livello massimo|最高レベル|최대 레벨|最高等级|最高等級
NIVEAU PRÉCÉDENT|Previous level|Vorheriges Level|Nivel anterior|Livello precedente|前のレベル|이전 레벨|上一级|上一級
NIVEAU ACTUEL|Current level|Aktuelles Level|Nivel actual|Livello attuale|現在のレベル|현재 레벨|当前等级|目前等級
NIVEAU SUIVANT|Next level|Nächstes Level|Nivel siguiente|Livello successivo|次のレベル|다음 레벨|下一级|下一級
ROYALE Z-A INFINI|Infinite Z-A Royale|Endloses Z-A Royale|Royale Z-A infinito|Royale Z-A infinito|∞ランクのＺＡロワイヤル|무한 Z-A 로열|无限Z-A登顶战|無限Z-A登頂戰
PROCHAIN TICKET|Next ticket|Nächstes Ticket|Siguiente ticket|Prossimo biglietto|次のチケット|다음 티켓|下一张挑战券|下一張挑戰券
POINTS TICKET|Ticket points|Ticketpunkte|Puntos de ticket|Punti biglietto|チケットポイント|티켓 포인트|挑战券点数|挑戰券點數
Données en direct|Live data|Live-Daten|Datos en directo|Dati in tempo reale|リアルタイム更新|실시간 데이터|实时数据|即時資料
CAPACITÉS|Moves|Attacken|Movimientos|Mosse|わざ|기술|招式|招式
CAPACITÉS DISPONIBLES|Available moves|Verfügbare Attacken|Movimientos disponibles|Mosse disponibili|覚えられる技|배울 수 있는 기술|可用招式|可用招式
REMPLACER LA CAPACITÉ|Replace move|Attacke ersetzen|Cambiar movimiento|Sostituisci mossa|技を入れ替える|기술 바꾸기|替换招式|替換招式
CATÉGORIE|Category|Kategorie|Categoría|Categoria|分類|분류|分类|分類
PUISSANCE|Power|Stärke|Potencia|Potenza|威力|위력|威力|威力
PRÉCISION|Accuracy|Genauigkeit|Precisión|Precisione|命中率|명중률|命中率|命中率
Pui.|Power|Stärke|Pot.|Pot.|威力|위력|威力|威力
Préc.|Acc.|Gen.|Prec.|Prec.|命中率|명중률|命中率|命中率
Statut|Status|Status|Estado|Stato|変化|변화|变化|變化
Physique|Physical|Physisch|Físico|Fisica|物理|물리|物理|物理
Spéciale|Special|Speziell|Especial|Speciale|特殊|특수|特殊|特殊
Capacité équipée.|Equipped move.|Ausgerüstete Attacke.|Movimiento equipado.|Mossa equipaggiata.|覚えている技|장착한 기술|已装备招式。|已裝備招式。
Cette capacité est déjà équipée.|This move is already equipped.|Diese Attacke ist schon ausgerüstet.|Este movimiento ya está equipado.|Questa mossa è già equipaggiata.|すでに覚えている技です|이미 장착한 기술입니다.|已装备此招式。|已裝備此招式。
Choisissez une capacité à remplacer, puis une capacité disponible.|Select a move slot, then an available move.|Wähle einen Platz, dann eine verfügbare Attacke.|Elige un espacio y un movimiento disponible.|Scegli uno spazio, poi una mossa disponibile.|入れ替える技と覚える技を選択|바꿀 기술과 배울 기술을 선택하세요.|选择要替换的招式及新招式。|選擇要替換的招式及新招式。
Prêt à remplacer la capacité sélectionnée.|Ready to replace the selected move.|Bereit, die gewählte Attacke zu ersetzen.|Listo para cambiar el movimiento.|Pronto a sostituire la mossa.|選択した技を入れ替えられます|선택한 기술을 바꿀 수 있습니다.|可以替换所选招式。|可以替換所選招式。
En ville, rappelez le Pokémon sorti.|In the city, recall your Pokémon.|Rufe dein Pokémon in der Stadt zurück.|En la ciudad, retira a tu Pokémon.|In città, richiama il tuo Pokémon.|街でポケモンをボールに戻してください|도시에서 포켓몬을 볼에 넣어 주세요.|请在城里收回宝可梦。|請在城裡收回寶可夢。
Consultation en boîte. Modification directe non disponible.|Box move details. Direct editing unavailable.|Box-Attacken ansehen. Direktes Ändern nicht verfügbar.|Detalles en cajas. Edición directa no disponible.|Dettagli nel Box. Modifica diretta non disponibile.|ボックスの技を確認。直接変更は未対応|박스 기술 확인. 직접 변경은 미지원.|查看盒中招式，暂不支持直接修改。|查看盒中招式，暫不支援直接修改。
Consultation des capacités sur cette plateforme.|Move details on this platform.|Attacken auf dieser Plattform ansehen.|Consulta de movimientos en esta plataforma.|Dettagli delle mosse su questa piattaforma.|この環境では技の確認に対応|이 환경에서는 기술 확인을 지원합니다.|此平台可查看招式详情。|此平台可查看招式詳情。
ÉCHANGER AVEC L'ÉQUIPE|Swap with party|Mit Team tauschen|Intercambiar con equipo|Scambia con la squadra|手持ちと入れ替え|지닌 포켓몬과 교환|与同行宝可梦交换|與同行寶可夢交換
EFFECTUER L'ÉCHANGE|Confirm swap|Tausch bestätigen|Confirmar intercambio|Conferma scambio|入れ替える|교환 확인|确认交换|確認交換
ANNULER|Cancel|Abbrechen|Cancelar|Annulla|キャンセル|취소|取消|取消
Choisissez le Pokémon de l'équipe à échanger.|Choose the party Pokémon to swap.|Wähle das Team-Pokémon für den Tausch.|Elige el Pokémon del equipo para intercambiar.|Scegli il Pokémon della squadra da scambiare.|入れ替える手持ちのポケモンを選択|교환할 지닌 포켓몬을 선택하세요.|选择要交换的同行宝可梦。|選擇要交換的同行寶可夢。
Échange confirmé dans le jeu|Swap confirmed in game|Tausch im Spiel bestätigt|Intercambio confirmado en el juego|Scambio confermato nel gioco|ゲーム内で入れ替えを確認|게임에서 교환 확인됨|已确认游戏内交换|已確認遊戲內交換
Revenez en ville et rappelez le Pokémon sorti. Vérifiez votre sélection.|Return to the city and recall your Pokémon. Check your selection.|Kehre in die Stadt zurück und rufe dein Pokémon zurück. Prüfe die Auswahl.|Vuelve a la ciudad y retira tu Pokémon. Revisa la selección.|Torna in città e richiama il Pokémon. Controlla la selezione.|街に戻りポケモンを戻して選択を確認|도시로 돌아가 포켓몬을 볼에 넣고 선택을 확인하세요.|请回到城里并收回宝可梦，确认选择。|請回到城裡並收回寶可夢，確認選擇。
Prêt pour l'échange direct.|Ready for direct swap.|Bereit zum direkten Tausch.|Listo para el intercambio directo.|Pronto per lo scambio diretto.|直接入れ替え可能|바로 교환할 수 있습니다.|可以直接交换。|可以直接交換。
C|C|C|C|C|C|C|C|C
FILTRES|Filters|Filter|Filtros|Filtri|フィルター|필터|筛选|篩選
POINTS D’INTÉRÊT|Points of interest|Orte|Lugares de interés|Punti d’interesse|注目スポット|관심 지점|地点|地點
CAFÉS|Cafés|Cafés|Cafeterías|Caffè|カフェ|카페|咖啡馆|咖啡館
Recharge estimée|Estimated cooldown|Geschätzte Ladezeit|Recarga estimada|Ricarica stimata|推定待ち時間|예상 대기 시간|预计冷却|預計冷卻
A|A|A|A|A|A|A|A|A
B|B|B|B|B|B|B|B|B
X|X|X|X|X|X|X|X|X
Y|Y|Y|Y|Y|Y|Y|Y|Y
Envoyez le Pokémon sélectionné, puis touchez une attaque.|Send out the selected Pokémon, then tap a move.|Setze das gewählte Pokémon ein und tippe auf eine Attacke.|Envía al Pokémon seleccionado y toca un movimiento.|Manda in campo il Pokémon scelto e tocca una mossa.|選んだポケモンを出してから技をタップ|선택한 포켓몬을 내보낸 뒤 기술을 누르세요|派出所选宝可梦后点击招式|派出所選寶可夢後點選招式
←|←|←|←|←|←|←|←|←
↑|↑|↑|↑|↑|↑|↑|↑|↑
→|→|→|→|→|→|→|→|→
↓|↓|↓|↓|↓|↓|↓|↓|↓
sauvegarde introuvable|Save not found|Spielstand nicht gefunden|Partida guardada no encontrada|Salvataggio non trovato|セーブデータが見つかりません|저장 데이터를 찾을 수 없습니다|未找到存档|找不到存檔
sauvegarde absente|No save data|Keine Speicherdaten|No hay datos guardados|Nessun salvataggio|セーブデータなし|저장 데이터 없음|无存档数据|無存檔資料
sauvegarde a jour|Save up to date|Spielstand aktuell|Partida actualizada|Salvataggio aggiornato|セーブデータ更新済み|저장 데이터 최신 상태|存档已更新|存檔已更新
sauvegarde lue|Save loaded|Spielstand geladen|Partida cargada|Salvataggio caricato|セーブデータ読込済み|저장 데이터 읽기 완료|存档已读取|存檔已讀取
somme de controle invalide|Invalid save checksum|Prüfsumme ungültig|Suma de verificación no válida|Checksum non valido|セーブデータの検証エラー|저장 데이터 검증 오류|存档校验失败|存檔驗證失敗
format de sauvegarde inconnu|Unknown save format|Unbekanntes Speicherformat|Formato de guardado desconocido|Formato del salvataggio sconosciuto|不明なセーブ形式|알 수 없는 저장 형식|未知存档格式|未知存檔格式
ÉQUIPE|Team|Team|Equipo|Squadra|てもち|팀|队伍|隊伍
BOÎTE|Boxes|Boxen|Cajas|Box|ボックス|박스|盒子|盒子
BOÎTES|Boxes|Boxen|Cajas|Box|ボックス|박스|盒子|盒子
CARTE|Map|Karte|Mapa|Mappa|マップ|지도|地图|地圖
OBJETS|Items|Items|Objetos|Strumenti|どうぐ|도구|道具|道具
POKÉMON|Pokémon|Pokémon|Pokémon|Pokémon|ポケモン|포켓몬|宝可梦|寶可夢
EN COURS|In progress|In Arbeit|En curso|In corso|進行中|진행 중|进行中|進行中
ARGENT DISPONIBLE|Money|Geld|Dinero|Denaro|所持金|소지금|持有金钱|持有金錢
AUTRES|Other|Sonstiges|Otros|Altro|その他|기타|其他|其他
BALLS|Poké Balls|Pokébälle|Poké Balls|Poké Ball|ボール|볼|精灵球|精靈球
CAPTURÉS|Caught|Gefangen|Capturados|Catturati|捕まえた|잡은 포켓몬|已捕获|已捕獲
CLÉS|Key items|Basis-Items|Objetos clave|Strumenti base|たいせつなもの|중요한 물건|重要物品|重要物品
Données de votre dernière sauvegarde|From your latest save|Aus deinem letzten Spielstand|Datos de la última partida guardada|Dati dell'ultimo salvataggio|最後にセーブしたデータ|마지막 저장 데이터|最近保存的数据|最近儲存的資料
DÉBLOQUÉES|Unlocked|Freigeschaltet|Desbloqueadas|Sbloccati|解放済み|해금됨|已解锁|已解鎖
EN STOCK|In storage|Aufbewahrt|En las cajas|Nei Box|ボックスにいる|박스에 있음|存放中|存放中
EXTRAS|Extra|Extras|Extras|Extra|エクストラ|엑스트라|额外|額外
Emplacements des zones · activité non suivie|Zone locations · activity not tracked|Zonenstandorte · Aktivität nicht erfasst|Ubicaciones · actividad no registrada|Posizioni · attività non monitorata|エリアの位置・活動状況は未対応|구역 위치 · 활성 상태 추적 안 함|区域位置 · 未追踪活动状态|區域位置 · 未追蹤活動狀態
Glisser pour parcourir les rubriques|Swipe to browse tabs|Wischen, um Tabs zu wechseln|Desliza para ver las pestañas|Scorri per vedere le schede|スワイプでタブを切り替え|밀어서 탭 이동|滑动浏览分页|滑動瀏覽分頁
Indisponible|Unavailable|Nicht verfügbar|No disponible|Non disponibile|データなし|정보 없음|暂无数据|暫無資料
Non identifié|Not identified|Noch nicht ermittelt|Sin identificar|Non identificato|未確認|확인되지 않음|尚未识别|尚未識別
MANQUANTS|Missing|Fehlend|Sin capturar|Mancanti|未捕獲|미포획|未捕获|未捕獲
MISSIONS|Missions|Missionen|Misiones|Missioni|ミッション|미션|任务|任務
MON PARCOURS À ILLUMIS|My Lumiose journey|Mein Weg in Illumina|Mi aventura en Luminalia|Il mio viaggio a Luminopoli|ミアレの冒険記録|미르시티의 모험 기록|我的密阿雷之旅|我的密阿雷之旅
MÉGA|Mega|Mega|Mega|Mega|メガ|메가|超级|超級
Méga-|Mega |Mega-|Mega-|Mega|メガ|메가|超级|超級
MÉGA : statuts de l'espèce de base|Mega status follows the base species|Mega-Status der Grundform|Mega: estado de la especie base|Lo stato Mega segue la specie base|メガの状態は元の種族に基づきます|메가 상태는 기본 종 기준|超级状态以基础种类为准|超級狀態以基礎種類為準
MÉGA-DIMENSION|Mega Dimension|Mega-Dimension|Megadimensión|Megadimensione|メガ次元|메가 차원|超级次元|超級次元
MÉGA-ÉVOLUTIONS|Mega Evolutions|Mega-Entwicklungen|Megaevoluciones|Megaevoluzioni|メガシンカ|메가진화|超级进化|超級進化
NIVEAU|Level|Level|Nivel|Livello|レベル|레벨|等级|等級
OBJECTIFS|Objectives|Ziele|Objetivos|Obiettivi|目標|목표|目标|目標
PLACER UN REPÈRE|Place a marker|Markierung setzen|Poner marcador|Metti un segnalino|マーカーを置く|마커 놓기|放置标记|放置標記
EFFACER LE REPÈRE|Clear marker|Markierung löschen|Borrar marcador|Rimuovi segnalino|マーカーを消す|마커 지우기|清除标记|清除標記
DIMENSION INCONNUE|Unknown hyperspace|Unbekannter Hyperraum|Dimensión desconocida|Dimensione sconosciuta|未知の異次元|알 수 없는 차원|未知异次元|未知異次元
DIM. INCONNUES|Unknown hyperspace|Unbekannte Hyperräume|Dim. desconocidas|Dim. sconosciute|未知の異次元|알 수 없는 차원|未知异次元|未知異次元
LIEUX D’INTÉRÊT|Landmarks|Sehenswürdigkeiten|Lugares de interés|Luoghi di interesse|名所|주요 장소|地标|地標
RÉCOMPENSES|Rewards|Belohnungen|Recompensas|Ricompense|報酬|보상|奖励|獎勵
Pokédollars|Poké Dollars|Pokédollar|Pokécuartos|Pokédollari|円|원|宝可元|寶可元
points de recherche|research points|Forschungspunkte|puntos de investigación|punti ricerca|調査ポイント|조사 포인트|调查点数|調查點數
VOIR SUR LA CARTE|Show on map|Auf Karte zeigen|Ver en el mapa|Mostra sulla mappa|マップで確認|지도에서 보기|在地图上查看|在地圖上查看
EMPLACEMENT INDISPONIBLE|Location unavailable|Ort nicht verfügbar|Ubicación no disponible|Posizione non disponibile|場所を表示できません|위치를 표시할 수 없습니다|无法显示位置|無法顯示位置
Charger les repères|Load map markers|Kartenmarker laden|Cargar marcadores|Carica indicatori|マーカー読み込み|마커 불러오기|加载地图标记|載入地圖標記
Ouvre la carte du jeu (+)|Open the game map (+)|Öffne die Spielkarte (+)|Abre el mapa del juego (+)|Apri la mappa di gioco (+)|ゲーム内のマップを開く（＋）|게임 지도를 여세요 (+)|打开游戏地图（＋）|開啟遊戲地圖（＋）
Carnet|Notebook|Notizbuch|Cuaderno|Taccuino|ノート|수첩|笔记|筆記
Où le trouver ?|Where to find it?|Wo zu finden?|¿Dónde encontrarlo?|Dove trovarlo?|出現場所|출현 장소|出现地点|出現地點
Aide au combat|Battle guide|Kampfhilfe|Guía de combate|Guida alla lotta|バトルガイド|배틀 가이드|对战指南|對戰指南
Épingler|Pin|Anheften|Fijar|Fissa|ピン留め|고정|置顶|置頂
Désépingler|Unpin|Lösen|Desfijar|Sblocca|ピンを外す|고정 해제|取消置顶|取消置頂
Ouvrir|Open|Öffnen|Abrir|Apri|開く|열기|打开|打開
Retirer|Remove|Entfernen|Quitar|Rimuovi|削除|삭제|移除|移除
Épingle des Pokémon et des missions pour les retrouver ici.|Pin Pokémon and missions to find them here.|Hefte Pokémon und Missionen an, um sie hier zu finden.|Fija Pokémon y misiones para encontrarlos aquí.|Fissa Pokémon e missioni per ritrovarli qui.|ポケモンやミッションをピン留めするとここに表示されます|포켓몬과 미션을 고정하면 여기에 표시됩니다|置顶宝可梦和任务后会显示在这里|置頂寶可夢和任務後會顯示在這裡
Rencontres de l’espèce, toutes formes confondues|Species encounters, including all forms|Fundorte der Art, einschließlich aller Formen|Encuentros de la especie, con todas sus formas|Incontri della specie, incluse tutte le forme|すべての姿の出現場所|모든 폼의 출현 장소|该种宝可梦所有形态的出现地点|該種寶可夢所有形態的出現地點
Aucun lieu connu dans les données disponibles.|No known location in the available data.|Kein Fundort in den verfügbaren Daten.|No hay lugares conocidos en los datos disponibles.|Nessun luogo noto nei dati disponibili.|確認できる出現場所はありません|확인된 출현 장소가 없습니다|暂无已知出现地点|暫無已知出現地點
Adversaire choisi dans le Pokédex|Opponent selected in the Pokédex|Gegner aus dem Pokédex|Rival elegido en la Pokédex|Avversario scelto nel Pokédex|図鑑で選んだ相手|도감에서 선택한 상대|从图鉴选择的对手|從圖鑑選擇的對手
Faiblesses|Weaknesses|Schwächen|Debilidades|Debolezze|弱点|약점|弱点|弱點
ZONE SAUVAGE {i}|Wild Zone {i}|Wildzone {i}|Zona salvaje {i}|Zona selvatica {i}|ワイルドゾーン {i}|와일드존 {i}|野生地带 {i}|野生地帶 {i}
Dimension sauvage active|Active wild dimension|Aktive wilde Dimension|Dimensión salvaje activa|Dimensione selvatica attiva|出現中の野生の異次元|활성 야생 차원|当前野生异次元|目前野生異次元
PLAN D'ILLUMIS|Lumiose map|Karte von Illumina|Mapa de Luminalia|Mappa di Luminopoli|ミアレの地図|미르시티 지도|密阿雷地图|密阿雷地圖
POINTS DE RECHERCHE CUMULÉS|Total research points|Gesamte Forschungspunkte|Puntos de investigación totales|Punti ricerca totali|累計調査ポイント|누적 조사 포인트|累计调查点数|累計調查點數
POINTS TICKET Z-A|Z-A ticket points|Z-A-Ticketpunkte|Puntos para el pase Z-A|Punti biglietto Z-A|ＺＡチケットポイント|Z-A 티켓 포인트|Z-A入场券点数|Z-A入場券點數
PREC|Previous|Zurück|Anterior|Precedente|前へ|이전|上一页|上一頁
SUIV|Next|Weiter|Siguiente|Successivo|次へ|다음|下一页|下一頁
PROGRESSION|Progress|Fortschritt|Progreso|Progressi|進行状況|진행 상황|进度|進度
Progression actualisée à la sauvegarde|Progress updates when you save|Fortschritt wird beim Speichern aktualisiert|El progreso se actualiza al guardar|I progressi si aggiornano al salvataggio|セーブ時に進行状況を更新|저장 시 진행 상황 갱신|保存时更新进度|儲存時更新進度
RECHERCHE INTERDIMENSIONNELLE|Hyperspace survey points|Dimensions-Forschungspunkte|Puntos de investigación dimensional|Punti ricerca extradimensionale|異次元調査ポイント|이차원 조사 포인트|异次元调查点数|異次元調查點數
ZONES SAUVAGES|Wild zones|Wildzonen|Zonas salvajes|Zone selvagge|ワイルドゾーン|와일드존|野生地带|野生地帶
COMBAT|Battle|Kampf|Combate|Lotta|バトル|배틀|对战|對戰
CENTRES|Centers|Center|Centros|Centri|センター|센터|中心|中心
ZOOM +|Zoom +|Zoom +|Zoom +|Zoom +|Zoom +|Zoom +|Zoom +|Zoom +
ZOOM −|Zoom −|Zoom −|Zoom −|Zoom −|Zoom −|Zoom −|Zoom −|Zoom −
RENCONTRES|Encounters|Begegnungen|Encuentros|Incontri|出現ポケモン|출현 포켓몬|出没的宝可梦|出沒的寶可夢
RETOUR AU POKÉDEX|Back to Pokédex|Zurück zum Pokédex|Volver a la Pokédex|Torna al Pokédex|図鑑に戻る|도감으로 돌아가기|返回图鉴|返回圖鑑
RETOUR AUX MISSIONS|Back to missions|Zurück zu Missionen|Volver a las misiones|Torna alle missioni|ミッションに戻る|미션으로 돌아가기|返回任务|返回任務
Recherche · aventures · Royale Z-A|Research · adventures · Z-A Royale|Forschung · Abenteuer · Z-A Royale|Investigación · aventuras · Torneo Z-A|Ricerca · avventure · Royale Z-A|調査・冒険・ＺＡロワイヤル|조사 · 모험 · Z-A 로열|调查 · 冒险 · Z-A登峰战|調查 · 冒險 · Z-A登峰戰
DONNÉES EN DIRECT INDISPONIBLES|Live data unavailable|Live-Daten nicht verfügbar|Datos en directo no disponibles|Dati in tempo reale non disponibili|リアルタイムデータを取得できません|실시간 데이터를 사용할 수 없음|实时数据不可用|即時資料無法使用
SAUVEGARDE NON LUE|Save not loaded|Spielstand nicht geladen|Partida guardada no cargada|Salvataggio non caricato|セーブデータ未読込|저장 데이터를 읽지 못함|尚未读取存档|尚未讀取存檔
TOUS|All|Alle|Todos|Tutti|すべて|모두|全部|全部
TOUT|All|Alle|Todos|Tutti|すべて|모두|全部|全部
VUS|Seen|Gesehen|Vistos|Visti|見つけた|발견|已发现|已發現
Touche une zone verte pour voir ses Pokémon.|Tap a green zone to see its Pokémon.|Tippe auf eine grüne Zone, um ihre Pokémon zu sehen.|Toca una zona verde para ver sus Pokémon.|Tocca una zona verde per vedere i suoi Pokémon.|緑のエリアをタップして出現ポケモンを表示|초록 구역을 눌러 출현 포켓몬 확인|点击绿色区域查看宝可梦|點選綠色區域查看寶可夢
VERT : ZONES SAUVAGES   ·   ROUGE : COMBAT|Green: wild zones · Red: battle zones|Grün: Wildzonen · Rot: Kampfzonen|Verde: zonas salvajes · Rojo: combate|Verde: zone selvagge · Rosso: lotta|緑：ワイルドゾーン・赤：バトルゾーン|초록: 와일드존 · 빨강: 배틀존|绿色：野生地带 · 红色：对战区域|綠色：野生地帶 · 紅色：對戰區域
VICTOIRES DU ROYALE INFINI|Infinite Z-A Royale wins|Siege im endlosen Z-A Royale|Victorias del Torneo Z-A infinito|Vittorie Royale Z-A infinito|無限ＺＡロワイヤル勝利数|무한 Z-A 로열 승리 횟수|无限Z-A登峰战胜场|無限Z-A登峰戰勝場
Z-A 2.0.2 requis pour le suivi en direct.|Z-A 2.0.2 is required for live data.|Z-A 2.0.2 wird für Live-Daten benötigt.|Se necesita Z-A 2.0.2 para los datos en directo.|Serve Z-A 2.0.2 per i dati in tempo reale.|リアルタイム表示にはＺＡ 2.0.2が必要|실시간 데이터에는 Z-A 2.0.2 필요|实时数据需要Z-A 2.0.2|即時資料需要Z-A 2.0.2
Aucun objet|No held item|Kein getragenes Item|Sin objeto equipado|Nessuno strumento|持ち物なし|지닌 물건 없음|无携带物品|無攜帶物品
Description indisponible dans les ressources chargées.|Description unavailable.|Beschreibung nicht verfügbar.|Descripción no disponible.|Descrizione non disponibile.|説明なし|설명 없음|暂无说明|暫無說明
Missions actives en premier - touche une mission pour ses détails|Active missions first · tap for details|Aktive Missionen zuerst · tippen für Details|Misiones activas primero · toca para ver detalles|Missioni attive prima · tocca per i dettagli|進行中を優先・タップで詳細|진행 중 우선 · 눌러서 상세 보기|进行中任务优先 · 点击查看详情|進行中任務優先 · 點選查看詳情
Progression indisponible|Progress unavailable|Fortschritt nicht verfügbar|Progreso no disponible|Progressi non disponibili|進行状況なし|진행 정보 없음|暂无进度|暫無進度
Baron possible|Possible alpha|Alpha möglich|Posible alfa|Possibile alfa|オヤブン出現あり|우두머리 출현 가능|可能出现头目|可能出現頭目
CAPTURÉ|Caught|Gefangen|Capturado|Catturato|捕獲済み|포획 완료|已捕获|已捕獲
VU|Seen|Gesehen|Visto|Visto|発見済み|발견|已发现|已發現
MANQUANT|Missing|Fehlend|Sin capturar|Mancante|未捕獲|미포획|未捕获|未捕獲
DISPONIBLE|Available|Verfügbar|Disponible|Disponibile|受注可能|수주 가능|可接取|可接取
TERMINÉE|Completed|Abgeschlossen|Completada|Completata|クリア済み|완료|已完成|已完成
À DÉCOUVRIR|Undiscovered|Unentdeckt|Sin descubrir|Da scoprire|未発見|미발견|未发现|未發現
ÉTAT INCONNU|Unknown status|Status unbekannt|Estado desconocido|Stato sconosciuto|状態不明|상태 불명|状态未知|狀態未知
Sélection impossible : fermez les menus du jeu.|Close the game menus to select a Pokémon.|Schließe die Spielmenüs zur Pokémon-Auswahl.|Cierra los menús para seleccionar un Pokémon.|Chiudi i menu per selezionare un Pokémon.|メニューを閉じて選んでください|메뉴를 닫고 포켓몬을 선택하세요|请关闭游戏菜单后选择|請關閉遊戲選單後選擇
Commande envoyée au jeu|Command sent to the game|Befehl an das Spiel gesendet|Orden enviada al juego|Comando inviato al gioco|ゲームに送信しました|게임에 명령을 보냈습니다|指令已发送|指令已傳送
Sélection en jeu indisponible. Vérifiez Z-A 2.0.2.|Selection unavailable. Check Z-A 2.0.2.|Auswahl nicht verfügbar. Prüfe Z-A 2.0.2.|Selección no disponible. Revisa Z-A 2.0.2.|Selezione non disponibile. Verifica Z-A 2.0.2.|選択できません。ＺＡ 2.0.2を確認|선택 불가. Z-A 2.0.2를 확인하세요|无法选择。请确认Z-A 2.0.2|無法選擇。請確認Z-A 2.0.2
Sélection du Pokémon en cours...|Selecting Pokémon…|Pokémon wird ausgewählt…|Seleccionando Pokémon…|Selezione del Pokémon…|ポケモンを選択中…|포켓몬 선택 중…|正在选择宝可梦…|正在選擇寶可夢…
Habitats|Habitats|Fundorte|Hábitats|Habitat|生息地|서식지|栖息地|棲息地
Combat|Battle|Kampf|Combate|Lotta|バトル|배틀|对战|對戰
Évolutions|Evolutions|Entwicklung|Evoluciones|Evoluzioni|進化|진화|进化|進化
Aucune évolution connue pour cette forme.|No known evolution for this form.|Keine Entwicklung für diese Form bekannt.|No se conoce ninguna evolución de esta forma.|Nessuna evoluzione nota per questa forma.|この姿の進化情報はありません。|이 모습의 진화 정보가 없습니다.|此形态没有已知的进化。|此形態沒有已知的進化。
Non capturés :|Uncaught:|Ungefangen:|Sin capturar:|Non catturati:|未捕獲：|미포획:|未捕获：|未捕獲：
Non capturés uniquement|Uncaught only|Nur ungefangene|Solo sin capturar|Solo non catturati|未捕獲のみ|미포획만|仅未捕获|僅未捕獲
Afficher tout|Show all|Alle anzeigen|Mostrar todos|Mostra tutti|すべて表示|모두 표시|显示全部|顯示全部
Tous les Pokémon de cette zone sont capturés.|All Pokémon in this zone are registered as caught.|Alle Pokémon dieser Zone wurden gefangen.|Has capturado todos los Pokémon de esta zona.|Hai catturato tutti i Pokémon di questa zona.|このゾーンのポケモンはすべて捕獲済みです。|이 구역의 포켓몬을 모두 잡았습니다.|已捕获此区域的所有宝可梦。|已捕獲此區域的所有寶可夢。
Niveau|Level|Level|Nivel|Livello|レベル|레벨|等级|等級
Montée de niveau|Level up|Levelanstieg|Subir de nivel|Aumenta di livello|レベルアップ|레벨 업|升级|升級
Condition spéciale|Special condition|Spezielle Bedingung|Condición especial|Condizione speciale|特別な条件|특수 조건|特殊条件|特殊條件
Montée de niveau avec forte amitié|Level up with high friendship|Levelanstieg bei hoher Freundschaft|Subir de nivel con amistad alta|Aumenta di livello con amicizia elevata|なかよし度が高いときにレベルアップ|친밀도가 높을 때 레벨 업|高亲密度时升级|高親密度時升級
Montée de niveau de jour avec forte amitié|Level up by day with high friendship|Tagsüber bei hoher Freundschaft|Subir de nivel de día con amistad alta|Aumenta di livello di giorno con amicizia elevata|昼になかよし度が高いときにレベルアップ|낮에 친밀도가 높을 때 레벨 업|白天高亲密度时升级|白天高親密度時升級
Montée de niveau de nuit avec forte amitié|Level up at night with high friendship|Nachts bei hoher Freundschaft|Subir de nivel de noche con amistad alta|Aumenta di livello di notte con amicizia elevata|夜になかよし度が高いときにレベルアップ|밤에 친밀도가 높을 때 레벨 업|夜晚高亲密度时升级|夜晚高親密度時升級
Échange|Trade|Tausch|Intercambio|Scambio|通信交換|통신교환|连接交换|連線交換
Échange avec objet tenu|Trade while holding|Tausch mit getragenem Item|Intercambio con objeto equipado|Scambio con strumento tenuto|道具を持たせて通信交換|도구를 지닌 채 통신교환|携带道具时连接交换|攜帶道具時連線交換
Utiliser|Use|Benutzen|Usar|Usa|使う|사용|使用|使用
Attaque > Défense|Attack > Defense|Angriff > Verteidigung|Ataque > Defensa|Attacco > Difesa|攻撃 ＞ 防御|공격 ＞ 방어|攻击 ＞ 防御|攻擊 ＞ 防禦
Attaque = Défense|Attack = Defense|Angriff = Verteidigung|Ataque = Defensa|Attacco = Difesa|攻撃 ＝ 防御|공격 ＝ 방어|攻击 ＝ 防御|攻擊 ＝ 防禦
Attaque < Défense|Attack < Defense|Angriff < Verteidigung|Ataque < Defensa|Attacco < Difesa|攻撃 ＜ 防御|공격 ＜ 방어|攻击 ＜ 防御|攻擊 ＜ 防禦
Utiliser sur un mâle|Use on a male|Bei einem Männchen benutzen|Usar en un macho|Usa su un maschio|オスに使う|수컷에게 사용|对雄性使用|對雄性使用
Utiliser sur une femelle|Use on a female|Bei einem Weibchen benutzen|Usar en una hembra|Usa su una femmina|メスに使う|암컷에게 사용|对雌性使用|對雌性使用
Montée de niveau de jour avec objet tenu|Level up by day while holding|Tagsüber mit getragenem Item|Subir de nivel de día con objeto equipado|Aumenta di livello di giorno con strumento tenuto|昼に道具を持たせてレベルアップ|낮에 도구를 지닌 채 레벨 업|白天携带道具时升级|白天攜帶道具時升級
Montée de niveau de nuit avec objet tenu|Level up at night while holding|Nachts mit getragenem Item|Subir de nivel de noche con objeto equipado|Aumenta di livello di notte con strumento tenuto|夜に道具を持たせてレベルアップ|밤에 도구를 지닌 채 레벨 업|夜晚携带道具时升级|夜晚攜帶道具時升級
Montée de niveau en connaissant|Level up knowing|Levelanstieg mit der Attacke|Subir de nivel con el movimiento|Aumenta di livello conoscendo|技を覚えた状態でレベルアップ|기술을 배운 채 레벨 업|学会这招后升级|學會這招後升級
Console à l’envers|Console upside down|Konsole auf dem Kopf|Consola al revés|Console capovolta|本体を逆さにする|본체를 거꾸로 들기|倒置主机|倒置主機
Forte amitié et capacité de type|High friendship and a move of type|Hohe Freundschaft und Attacke vom Typ|Amistad alta y movimiento de tipo|Amicizia elevata e mossa di tipo|高いなかよし度と技のタイプ|높은 친밀도와 기술 타입|高亲密度与招式属性|高親密度與招式屬性
De jour|By day|Tagsüber|De día|Di giorno|昼|낮|白天|白天
De nuit|At night|Nachts|De noche|Di notte|夜|밤|夜晚|夜晚
Rechercher|Search|Suchen|Buscar|Cerca|検索|검색|搜索|搜尋
Nom ou numéro|Name or number|Name oder Nummer|Nombre o número|Nome o numero|名前か番号|이름 또는 번호|名称或编号|名稱或編號
Case|Slot|Platz|Casilla|Posto|マス|칸|格位|格位
Résultats :|Results:|Ergebnisse:|Resultados:|Risultati:|結果：|결과:|结果：|結果：
Aucun résultat|No results|Keine Treffer|Sin resultados|Nessun risultato|該当なし|검색 결과 없음|无结果|無結果
Dernière lettre|Backspace|Letztes Zeichen|Borrar letra|Cancella lettera|一字削除|한 글자 삭제|退格|退格
Effacer|Clear|Leeren|Borrar|Cancella|消去|지우기|清除|清除
Effacer tout|Clear all|Alles löschen|Borrar todo|Cancella tutto|すべて消去|모두 지우기|清空|清空
N° national|National no.|Nationalnr.|N.º nacional|N. nazionale|全国番号|전국 번호|全国编号|全國編號
Cette case a changé. Relance la recherche.|This slot changed. Search again.|Dieser Platz hat sich geändert. Suche erneut.|La casilla cambió. Busca de nuevo.|Il posto è cambiato. Cerca di nuovo.|マスの内容が変わりました。再検索してください。|칸 내용이 바뀌었습니다. 다시 검색하세요.|格位已变更。请重新搜索。|格位已變更。請重新搜尋。
Plan de capture|Catch plan|Fangplan|Plan de captura|Piano di cattura|捕獲プラン|포획 계획|捕捉计划|捕捉計畫
Cibles à capturer :|Targets to catch:|Fangziele:|Objetivos por capturar:|Obiettivi da catturare:|未捕獲の目標：|포획 목표:|待捕捉目标：|待捕捉目標：
Sans zone connue :|No known zone:|Ohne bekannte Zone:|Sin zona conocida:|Senza zona nota:|出現ゾーン不明：|구역 미확인:|未知地带：|未知地帶：
Cibles :|Targets:|Ziele:|Objetivos:|Obiettivi:|目標：|목표:|目标：|目標：
Épingle des Pokémon non capturés dans le Pokédex.|Pin uncaught Pokémon in the Pokédex.|Hefte ungefangene Pokémon im Pokédex an.|Fija Pokémon sin capturar en la Pokédex.|Fissa Pokémon non catturati nel Pokédex.|未捕獲のポケモンを図鑑で登録してください。|미포획 포켓몬을 도감에서 고정하세요.|在图鉴中固定未捕捉的宝可梦。|在圖鑑中固定未捕捉的寶可夢。
Les zones avec le plus de cibles apparaissent en premier.|Zones with the most targets appear first.|Zonen mit den meisten Zielen stehen zuerst.|Las zonas con más objetivos aparecen primero.|Le zone con più obiettivi appaiono per prime.|目標が多いゾーンから表示します。|목표가 많은 구역부터 표시합니다.|优先显示目标最多的地带。|優先顯示目標最多的地帶。
Détails|Details|Details|Detalles|Dettagli|詳細|상세|详情|詳情
Agrandir|Expand map|Vergrößern|Ampliar|Espandi|マップ拡大|지도 확대|展开地图|展開地圖
Repère|Marker|Markierung|Marcador|Indicatore|マーカー|마커|标记|標記
Touche la carte|Tap the map|Tippe auf die Karte|Toca el mapa|Tocca la mappa|マップをタッチ|지도를 터치|点击地图|點擊地圖
Pose en cours…|Placing marker…|Markierung wird gesetzt…|Colocando marcador…|Posizionamento…|マーカーを設置中…|마커 놓는 중…|正在放置标记…|正在放置標記…
Repère placé|Marker placed|Markierung gesetzt|Marcador colocado|Indicatore posizionato|マーカーを設置した|마커를 놓았습니다|已放置标记|已放置標記
Repère effacé|Marker cleared|Markierung gelöscht|Marcador borrado|Indicatore rimosso|マーカーを消した|마커를 지웠습니다|已清除标记|已清除標記
Effacement en cours…|Clearing marker…|Markierung wird gelöscht…|Borrando marcador…|Rimozione…|マーカーを消去中…|마커 지우는 중…|正在清除标记…|正在清除標記…
Effacement à réessayer|Try clearing again|Erneut löschen|Vuelve a borrar el marcador|Riprova a rimuoverlo|もう一度お試しください|다시 시도하세요|请重试|請重試
Pose à réessayer|Try placing again|Erneut versuchen|Vuelve a intentarlo|Riprova|もう一度お試しください|다시 시도하세요|请重试|請重試
Repères proches|Nearby markers|Nahe Markierungen|Marcadores cercanos|Indicatori vicini|近くのマーカー|주변 마커|附近标记|附近標記
Sur le terrain|Deployed|Im Einsatz|En el campo|In campo|場に出ている|필드에 나옴|已派出|已派出
Prête|Ready|Bereit|Listo|Pronta|使用可能|사용 가능|可使用|可使用
Attaques efficaces prêtes :|Ready super-effective moves:|Bereite sehr effektive Attacken:|Movimientos supereficaces listos:|Mosse superefficaci pronte:|使用可能な効果抜群の技：|사용 가능한 효과가 굉장한 기술:|可用的效果绝佳招式：|可用的效果絕佳招式：
Prêtes à utiliser en vert|Ready moves in green|Bereite Attacken in Grün|En verde si están listos|Mosse pronte in verde|使用可能な技は緑色|사용 가능한 기술은 초록색|可用招式为绿色|可用招式為綠色
Voir dans le sac|View in bag|Im Beutel ansehen|Ver en la bolsa|Vedi nella borsa|バッグで見る|가방에서 보기|在包包中查看|在包包中查看
Retour aux évolutions|Back to evolutions|Zurück zu Entwicklungen|Volver a evoluciones|Torna alle evoluzioni|進化に戻る|진화로 돌아가기|返回进化|返回進化
Description|Description|Beschreibung|Descripción|Descrizione|説明|설명|说明|說明
Forme|Form|Form|Forma|Forma|姿|모습|形态|形態
Favoris|Favorites|Favoriten|Favoritos|Preferiti|お気に入り|즐겨찾기|收藏|收藏
Vide|Empty|Leer|Vacío|Vuoto|空き|비어 있음|空位|空位
Ajouter aux favoris|Add to favorites|Zu Favoriten|Añadir a favoritos|Aggiungi ai preferiti|お気に入りに追加|즐겨찾기 추가|加入收藏|加入收藏
Retirer des favoris|Remove favorite|Favorit entfernen|Quitar de favoritos|Rimuovi dai preferiti|お気に入りから外す|즐겨찾기 삭제|取消收藏|取消收藏
Favoris pleins|Favorites full|Favoriten voll|Favoritos llenos|Preferiti pieni|お気に入りがいっぱい|즐겨찾기가 가득 참|收藏已满|收藏已滿
Soins rapides|Quick healing|Schnellheilung|Curación rápida|Cura rapida|すばやく回復|빠른 회복|快速恢复|快速回復
Ma position|My location|Mein Standort|Mi ubicación|La mia posizione|現在地|현재 위치|当前位置|目前位置
Soins|Healing|Heilen|Curar|Cura|回復|회복|回复|回復
Soins compatibles|Compatible medicine|Geeignete Heilmittel|Objetos curativos compatibles|Rimedi adatti|使える回復道具|사용 가능한 회복 도구|可用回复道具|可用回復道具
Aucun soin compatible en stock|No compatible medicine in the Bag|Keine passenden Heilmittel im Beutel|No hay objetos curativos compatibles|Nessun rimedio adatto nella Borsa|使える回復道具がない|사용 가능한 회복 도구가 없습니다|背包里没有可用的回复道具|背包裡沒有可用的回復道具
Pokémon introuvable dans l'équipe.|Pokémon no longer in the party.|Pokémon nicht mehr im Team.|El Pokémon ya no está en el equipo.|Il Pokémon non è più in squadra.|ポケモンが手持ちにいない|포켓몬이 배틀 팀에 없습니다|队伍中没有这只宝可梦|隊伍中沒有這隻寶可夢
En cours|In progress|Aktiv|En curso|In corso|進行中|진행 중|进行中|進行中
Aucune mission en cours|No missions in progress|Keine aktiven Missionen|No hay misiones en curso|Nessuna missione in corso|進行中のミッションはありません|진행 중인 미션이 없습니다|没有进行中的任务|沒有進行中的任務
Principale|Main|Haupt|Principal|Principale|メイン|메인|主线|主線
Extra|Extra|Extra|Extra|Extra|エクストラ|엑스트라|额外|額外
Secondaire|Side|Neben|Secundaria|Secondaria|サイド|사이드|支线|支線
Retour|Back|Zurück|Volver|Indietro|戻る|뒤로|返回|返回
Objet tenu|Held item|Getragenes Item|Objeto equipado|Strumento tenuto|持ち物|지닌 물건|携带道具|攜帶道具
Compatibilité CT|TM compatibility|TM-Kompatibilität|Afinidad con MT|Compatibilità MT|わざマシンの対応|기술머신 호환|招式学习器兼容|招式學習器相容
Évolutions possibles|Possible evolutions|Entwicklungen|Evoluciones posibles|Evoluzioni possibili|進化できるポケモン|진화 가능 포켓몬|可进化宝可梦|可進化寶可夢
Quitter le choix d’objet|Exit item selection|Itemauswahl verlassen|Salir de la selección|Esci dalla scelta|道具選択を終了|도구 선택 종료|退出道具选择|退出道具選擇
Aucun Pokémon compatible|No compatible Pokémon|Keine passenden Pokémon|Ningún Pokémon compatible|Nessun Pokémon compatibile|対応するポケモンがいません|호환 포켓몬 없음|没有兼容的宝可梦|沒有相容的寶可夢
Aucun soin nécessaire|No healing needed|Keine Heilung nötig|No hace falta curar|Nessuna cura necessaria|回復は不要です|회복이 필요 없음|无需恢复|無需回復
Incompatible|Incompatible|Nicht geeignet|Incompatible|Incompatibile|使えない|호환되지 않음|不兼容|不相容
Déjà équipée|Already equipped|Bereits ausgerüstet|Ya equipado|Già equipaggiata|すでにセット済み|이미 장착됨|已装备|已裝備
Compatible|Compatible|Geeignet|Compatible|Compatibile|使える|호환됨|兼容|相容
Cible choisie|Selected target|Gewähltes Ziel|Objetivo seleccionado|Bersaglio selezionato|選んだポケモン|선택한 대상|已选目标|已選目標
À utiliser dans le Sac du jeu.|Use this in the game's Bag.|Im Beutel des Spiels einsetzen.|Úsalo en la bolsa del juego.|Usalo nella Borsa del gioco.|ゲーム内のバッグで使う|게임의 가방에서 사용하세요|请在游戏包包中使用|請在遊戲包包中使用
PV de la dernière sauvegarde|HP from the last save|KP aus dem letzten Spielstand|PS de la última partida guardada|PS dell’ultimo salvataggio|最後のセーブのＨＰ|마지막 저장의 HP|上次保存时的HP|上次儲存時的HP
Ouvrir les capacités|Open moves|Attacken öffnen|Abrir movimientos|Apri le mosse|技の画面を開く|기술 열기|打开招式界面|開啟招式畫面
Ouvrir la fiche|Open details|Details öffnen|Abrir ficha|Apri la scheda|詳細を開く|상세 열기|打开详情|開啟詳情
Objet actuel|Current item|Aktuelles Item|Objeto actual|Strumento attuale|現在の持ち物|현재 도구|当前道具|目前道具
Remplacement proposé|Proposed item|Vorgeschlagenes Item|Objeto propuesto|Strumento proposto|候補の道具|제안 도구|建议道具|建議道具
Équipe l'objet dans le jeu.|Give the item in the game.|Gib das Item im Spiel.|Equipa el objeto en el juego.|Assegna lo strumento nel gioco.|ゲーム内で道具を持たせる|게임에서 도구를 지니게 하세요|请在游戏中装备道具|請在遊戲中裝備道具
Retrait proposé|Removal preview|Vorschau zum Abnehmen|Vista previa de retirada|Anteprima rimozione|持ち物を外す場合|도구 회수 미리보기|取下预览|取下預覽
Retire l'objet dans le jeu.|Remove the item in the game.|Entferne das Item im Spiel.|Retira el objeto en el juego.|Rimuovi lo strumento nel gioco.|ゲーム内で道具を外す|게임에서 도구를 회수하세요|请在游戏中取下道具|請在遊戲中取下道具
Choisir un objet|Choose an item|Item wählen|Elegir objeto|Scegli uno strumento|道具を選ぶ|도구 선택|选择道具|選擇道具
Préparer le retrait|Preview removal|Abnahme planen|Preparar la retirada|Prepara la rimozione|外す場合を確認|회수 미리보기|预览取下|預覽取下
Confirmer l'objet tenu|Confirm held item|Item bestätigen|Confirmar objeto equipado|Conferma strumento tenuto|持ち物を確定|지닌 도구 확인|确认携带道具|確認攜帶道具
Appliquer le soin|Use medicine|Heilmittel einsetzen|Usar remedio|Usa il rimedio|回復アイテムを使う|회복 도구 사용|使用恢复道具|使用回復道具
Soin appliqué.|Medicine used.|Heilmittel eingesetzt.|Remedio usado.|Rimedio usato.|回復アイテムを使いました|회복 도구를 사용했습니다|已使用恢复道具|已使用回復道具
Objet tenu mis à jour.|Held item updated.|Item geändert.|Objeto equipado actualizado.|Strumento tenuto aggiornato.|持ち物を更新しました|지닌 도구가 변경되었습니다|已更新携带道具|已更新攜帶道具
Le jeu n'a pas confirmé le changement.|The game did not confirm the change.|Das Spiel hat die Änderung nicht bestätigt.|El juego no ha confirmado el cambio.|Il gioco non ha confermato la modifica.|ゲームで変更を確認できませんでした|게임에서 변경을 확인하지 못했습니다|游戏未确认更改|遊戲未確認變更
Changement en cours…|Updating…|Wird geändert…|Actualizando…|Aggiornamento…|変更中…|변경 중…|正在更新…|正在更新…
Reviens en ville et rappelle ton Pokémon.|Return to the city and recall your Pokémon.|Kehre in die Stadt zurück und rufe dein Pokémon zurück.|Vuelve a la ciudad y retira a tu Pokémon.|Torna in città e richiama il tuo Pokémon.|街に戻り、ポケモンをボールに戻してください|도시로 돌아가 포켓몬을 볼에 넣으세요|请回到城里并收回宝可梦|請回到城裡並收回寶可夢
Ferme les menus du jeu.|Close the in-game menus.|Schließe die Menüs im Spiel.|Cierra los menús del juego.|Chiudi i menu del gioco.|ゲームのメニューを閉じてください|게임 메뉴를 닫으세요|请关闭游戏菜单|請關閉遊戲選單
Rappelle ton Pokémon dans sa Poké Ball.|Recall your Pokémon to its Poké Ball.|Rufe dein Pokémon in seinen Pokéball zurück.|Retira a tu Pokémon a su Poké Ball.|Richiama il tuo Pokémon nella Poké Ball.|ポケモンをボールに戻してください|포켓몬을 몬스터볼에 넣으세요|请将宝可梦收回精灵球|請將寶可夢收回精靈球
Lecture de l'équipe en cours…|Reading the party…|Team wird gelesen…|Leyendo el equipo…|Lettura della squadra…|手持ちを読み込み中…|포켓몬을 읽는 중…|正在读取队伍…|正在讀取隊伍…
Choisis un Pokémon|Choose a Pokémon|Wähle ein Pokémon|Elige un Pokémon|Scegli un Pokémon|ポケモンを選ぶ|포켓몬을 선택하세요|选择一只宝可梦|選擇一隻寶可夢
Capacité actuelle|Current move|Aktuelle Attacke|Movimiento actual|Mossa attuale|現在の技|현재 기술|当前招式|目前招式
Paralysie|Paralysis|Paralyse|Parálisis|Paralisi|まひ|마비|麻痹|麻痺
Sommeil|Sleep|Schlaf|Sueño|Sonno|ねむり|잠듦|睡眠|睡眠
Gel|Freeze|Frost|Congelación|Congelamento|こおり|얼음|冰冻|冰凍
Brûlure|Burn|Verbrennung|Quemadura|Scottatura|やけど|화상|灼伤|灼傷
Empoisonnement|Poisoning|Vergiftung|Envenenamiento|Avvelenamento|どく|독|中毒|中毒
K.O.|Fainted|K.O.|Debilitado|KO|ひんし|기절|濒死|瀕死
Aperçu de l'objet tenu|Held item preview|Vorschau auf das getragene Item|Vista previa del objeto equipado|Anteprima dello strumento tenuto|持ち物の確認|지닌 도구 미리보기|携带道具预览|攜帶道具預覽
Utilise le soin sur ce Pokémon dans le Sac du jeu.|Use the medicine on this Pokémon in the game's Bag.|Setze das Heilmittel bei diesem Pokémon im Spiel ein.|Usa el remedio en este Pokémon desde la bolsa del juego.|Usa il rimedio su questo Pokémon nella Borsa del gioco.|ゲーム内のバッグでこのポケモンに使う|게임의 가방에서 이 포켓몬에게 사용하세요|请在游戏包包中对这只宝可梦使用|請在遊戲包包中對這隻寶可夢使用
EN STOCK|Owned now|Im Besitz|En posesión|In possesso|所持中|보유 중|持有中|持有中
Plus|More|Mehr|Más|Altro|その他|더 보기|更多|更多
Onglets|Tabs|Tabs|Pestañas|Schede|タブ|탭|标签页|分頁
Modifier|Edit|Bearbeiten|Editar|Modifica|編集|편집|编辑|編輯
Terminé|Done|Fertig|Listo|Fine|完了|완료|完成|完成
Fermer|Close|Schließen|Cerrar|Chiudi|閉じる|닫기|关闭|關閉
Équipe|Team|Team|Equipo|Squadra|てもち|팀|队伍|隊伍
Pokédex|Pokédex|Pokédex|Pokédex|Pokédex|図鑑|도감|图鉴|圖鑑
Boîte|Boxes|Boxen|Cajas|Box|ボックス|박스|盒子|盒子
Carte|Map|Karte|Mapa|Mappa|マップ|지도|地图|地圖
Sac|Bag|Beutel|Bolsa|Borsa|バッグ|가방|包包|包包
Missions|Missions|Missionen|Misiones|Missioni|ミッション|미션|任务|任務
Progression|Progress|Fortschritt|Progreso|Progressi|進行状況|진행 상황|进度|進度
SAC|Bag|Beutel|Bolsa|Borsa|バッグ|가방|包包|包包
POKÉDEX|Pokédex|Pokédex|Pokédex|Pokédex|図鑑|도감|图鉴|圖鑑
VIDE|Empty|Leer|Vacío|Vuoto|空き|비어 있음|空|空
Précédente|Previous|Zurück|Anterior|Precedente|前へ|이전|上一个|上一個
Suivante|Next|Weiter|Siguiente|Successivo|次へ|다음|下一个|下一個
Choisir une boîte|Choose a box|Box auswählen|Elegir una caja|Scegli un box|ボックスを選ぶ|박스 선택|选择盒子|選擇盒子
Glisse pour changer de boîte|Swipe to change boxes|Wischen zum Boxwechsel|Desliza para cambiar de caja|Scorri per cambiare box|スワイプでボックスを切替|밀어서 박스 변경|滑动切换盒子|滑動切換盒子
Choisis une place, puis un onglet.|Choose a slot, then a tab.|Wähle einen Platz und dann einen Tab.|Elige un espacio y luego una pestaña.|Scegli un posto, poi una scheda.|場所を選んでからタブを選択|자리를 고른 뒤 탭을 선택하세요|先选择位置，再选择标签页|先選擇位置，再選擇分頁
"""
FALLBACKS={}
for line in ROWS.strip().splitlines():
    parts=line.split('|')
    if len(parts)!=9:raise ValueError(parts)
    source=' '.join(parts[0].split())
    FALLBACKS[source]=dict(zip((2,5,7,4,1,8,9,10),parts[1:]))
    FALLBACKS[source][3]=source;FALLBACKS[source][11]=parts[3]

for source in ('?','-','F','M','♂','♀','—','‹','›','+','…','∨','EXP','PP','NO.','No.','N°','N.','/ 30 PLACES','x',*'0123456789'):
    translated={'x':'×','/ 30 PLACES':'/ 30','N°':'No.','NO.':'No.'}.get(source,source)
    FALLBACKS[source]={lang:translated for lang in (1,2,3,4,5,7,8,9,10,11)}
