// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The companion's flavour lines in the 14 game languages + the known ACNH builds (acnh_flavor.h).
// Our own text, translated by us (owner round 7, option C); names are each language's official ones
// from the game's String / LayoutMsg texts (STR_SNpcName rco/sza, STR_Common 900/928/958/978/1500,
// LayoutMsg/Version 0001 "Ver." ...). The builds: main NSO build ids per update version from the
// switch-cheats-db versions list (LEAD: only 3.0.3 FF1D1C05670DB602 is checked here; 3.0.2 has no
// entry there), display versions by update order (v<n> = 65536 * n: 1.0.0 .. 3.0.3 = v0 .. v34).
//
// TABLE = "<lang>|<key>|<text>", one line each; "*" = not a language (build rows).
// Escapes in <text>: "\n" = line break, "\_" = no-break space (French punctuation). "{v}" in
// ver.have = the running display version (expanded per known build as have.<index>). The package
// generator parses this very block (between the TSV markers), so keep its format.

#include "acnh_flavor.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace acnh::flavor {
namespace {

constexpr std::string_view Table = R"TSV(
*|build|7FC1BAFF976AECA4 1.0.0
*|build|B0D6D16556B61BF5 1.1.0
*|build|F829A27213D46F9F 1.1.1
*|build|3068F51723DC1A67 1.1.2
*|build|E2D890A4D19FAD02 1.1.3
*|build|A31F81D41E1039C5 1.1.4
*|build|20CA968C082118C2 1.2.0
*|build|9429FC5202937D7F 1.2.1
*|build|FDC1632048FACFBD 1.3.0
*|build|E43B24AF5D84F44C 1.3.1
*|build|AC5309B683630CED 1.4.0
*|build|7515E5F76D09F8A3 1.4.1
*|build|E0A8E2D6018ED365 1.4.2
*|build|0209750A17F48CB8 1.5.0
*|build|A210C7E78216E42B 1.5.1
*|build|36EEBA0C26F14216 1.6.0
*|build|0400F824B4DA556E 1.7.0
*|build|0E36C71C08334076 1.8.0
*|build|3F5E3459BE77E565 1.9.0
*|build|8379D9E90FF19B0F 1.10.0
*|build|2768322A8F2F45F1 1.11.0
*|build|1CD4B05D52E20EAA 1.11.1
*|build|E4BBD879D326A0AD 2.0.0
*|build|8C81A85AA4C1990B 2.0.1
*|build|E5759E5B7E31411B 2.0.2
*|build|205F55C725C16C6F 2.0.3
*|build|372C5EA461D03A7D 2.0.4
*|build|747A5B4CBC530AED 2.0.5
*|build|15765149DF53BA41 2.0.6
*|build|0948E48778171EE6 2.0.7
*|build|CBF780093C874152 2.0.8
*|build|5D913CF71EB24CB2 3.0.0
*|build|8F2CB7A9774959C8 3.0.1
*|build|FF1D1C05670DB602 3.0.3
USen|wait.0.t|Paving the Island Path
USen|wait.0.s|Gathering wood and crafting materials…
USen|wait.1.t|Clearing the Weeds
USen|wait.1.s|Preparing your slice of paradise…
USen|wait.2.t|Chatting with Tom Nook
USen|wait.2.s|Calculating your loan balance…
USen|wait.3.t|Waiting for the Mail
USen|wait.3.s|Checking the Resident Services inbox…
USen|wait.4.t|Checking the Dodo Code
USen|wait.4.s|Warming up the seaplane engines…
USen|ver.t|Dodo Airlines Flight Delayed
USen|ver.s|Your island needs Ver. 3.0.3 to take off!
USen|ver.have|(you have Ver. {v})
USen|card.1.t|Morning Broadcast On Air
USen|card.1.s|Isabelle is speaking! Watch the main screen.
USen|card.2.t|Pockets Open on the Big Screen
USen|card.2.s|Finish up there first!
USen|card.3.t|NookPhone in Use
USen|card.3.s|Put your phone away to continue!
USen|card.5.t|Island Business in Progress
USen|card.5.s|Wait for the chat to wrap up!
USen|card.9.t|Special Event Underway
USen|card.9.s|Items can't move right now!
USen|card.6.t|Hmm, That Won't Work
USen|card.6.s|Try a different item!
EUen|wait.0.t|Paving the Island Path
EUen|wait.0.s|Gathering wood and crafting materials…
EUen|wait.1.t|Clearing the Weeds
EUen|wait.1.s|Preparing your slice of paradise…
EUen|wait.2.t|Chatting with Tom Nook
EUen|wait.2.s|Calculating your loan balance…
EUen|wait.3.t|Waiting for the Mail
EUen|wait.3.s|Checking the Resident Services inbox…
EUen|wait.4.t|Checking the Dodo Code
EUen|wait.4.s|Warming up the seaplane engines…
EUen|ver.t|Dodo Airlines Flight Delayed
EUen|ver.s|Your island needs Ver. 3.0.3 to take off!
EUen|ver.have|(you have Ver. {v})
EUen|card.1.t|Morning Broadcast On Air
EUen|card.1.s|Isabelle is speaking! Watch the main screen.
EUen|card.2.t|Pockets Open on the Big Screen
EUen|card.2.s|Finish up there first!
EUen|card.3.t|NookPhone in Use
EUen|card.3.s|Put your phone away to continue!
EUen|card.5.t|Island Business in Progress
EUen|card.5.s|Wait for the chat to wrap up!
EUen|card.9.t|Special Event Underway
EUen|card.9.s|Items can't move right now!
EUen|card.6.t|Hmm, That Won't Work
EUen|card.6.s|Try a different item!
USes|wait.0.t|Pavimentando caminos isleños
USes|wait.0.s|Juntando madera y materiales…
USes|wait.1.t|Quitando la maleza
USes|wait.1.s|Preparando tu rincón del paraíso…
USes|wait.2.t|Charlando con Tom Nook
USes|wait.2.s|Calculando lo que queda del préstamo…
USes|wait.3.t|Esperando el correo
USes|wait.3.s|Revisando el buzón de la\noficina de gestión vecinal…
USes|wait.4.t|Comprobando el PIN Dodo
USes|wait.4.s|Calentando los motores del hidroavión…
USes|ver.t|Vuelo de Dodo Airlines retrasado
USes|ver.s|¡Tu isla necesita la Ver. 3.0.3 para despegar!
USes|ver.have|(tienes la Ver. {v})
USes|card.1.t|Anuncio matutino al aire
USes|card.1.s|¡Canela está hablando! Mira la pantalla principal.
USes|card.2.t|Bolsillos abiertos en la pantalla grande
USes|card.2.s|¡Termina allí primero!
USes|card.3.t|Nookófono en uso
USes|card.3.s|¡Guarda el Nookófono para continuar!
USes|card.5.t|Asuntos de la isla en curso
USes|card.5.s|¡Espera a que termine la charla!
USes|card.9.t|Evento especial en marcha
USes|card.9.s|¡Ahora no se pueden mover objetos!
USes|card.6.t|Mmm, eso no va a funcionar
USes|card.6.s|¡Prueba con otro objeto!
EUes|wait.0.t|Pavimentando caminos isleños
EUes|wait.0.s|Recogiendo madera y materiales…
EUes|wait.1.t|Arrancando hierbajos
EUes|wait.1.s|Preparando tu rincón del paraíso…
EUes|wait.2.t|Charlando con Tom Nook
EUes|wait.2.s|Calculando lo que queda del préstamo…
EUes|wait.3.t|Esperando el correo
EUes|wait.3.s|Revisando el buzón de la\noficina de gestión vecinal…
EUes|wait.4.t|Comprobando el PIN Dodo
EUes|wait.4.s|Calentando los motores del hidroavión…
EUes|ver.t|Vuelo de Dodo Airlines retrasado
EUes|ver.s|¡Tu isla necesita la Ver. 3.0.3 para despegar!
EUes|ver.have|(tienes la Ver. {v})
EUes|card.1.t|Anuncio matinal en directo
EUes|card.1.s|¡Canela está hablando! Mira la pantalla principal.
EUes|card.2.t|Bolsillos abiertos en la pantalla grande
EUes|card.2.s|¡Termina allí primero!
EUes|card.3.t|Nookófono en uso
EUes|card.3.s|¡Guarda el Nookófono para continuar!
EUes|card.5.t|Asuntos de la isla en curso
EUes|card.5.s|¡Espera a que termine la charla!
EUes|card.9.t|Evento especial en marcha
EUes|card.9.s|¡Ahora no se pueden mover objetos!
EUes|card.6.t|Mmm, eso no va a funcionar
EUes|card.6.s|¡Prueba con otro objeto!
USfr|wait.0.t|Pavage des chemins de l'île
USfr|wait.0.s|Récolte de bois et de matériaux…
USfr|wait.1.t|Désherbage en cours
USfr|wait.1.s|On prépare votre coin de paradis…
USfr|wait.2.t|Jasette avec Tom Nook
USfr|wait.2.s|Calcul du montant de votre prêt…
USfr|wait.3.t|En attente du courrier
USfr|wait.3.s|On vérifie la boîte du bureau des résidents…
USfr|wait.4.t|Vérification du Dodo Code
USfr|wait.4.s|On réchauffe les moteurs de l'hydravion…
USfr|ver.t|Vol de Dodo Airlines retardé
USfr|ver.s|Votre île a besoin de la Ver. 3.0.3\npour décoller!
USfr|ver.have|(vous avez la Ver. {v})
USfr|card.1.t|Annonce du matin en ondes
USfr|card.1.s|Marie a la parole! Regardez l'écran principal.
USfr|card.2.t|Poches ouvertes sur le grand écran
USfr|card.2.s|Terminez d'abord là-bas!
USfr|card.3.t|NookPhone en cours d'utilisation
USfr|card.3.s|Rangez votre téléphone pour continuer!
USfr|card.5.t|Affaires de l'île en cours
USfr|card.5.s|Attendez la fin de la conversation!
USfr|card.9.t|Évènement spécial en cours
USfr|card.9.s|Les objets ne peuvent pas bouger pour l'instant!
USfr|card.6.t|Hum, ça ne marchera pas
USfr|card.6.s|Essayez un autre objet!
EUfr|wait.0.t|Pavage des chemins de l'île
EUfr|wait.0.s|Récolte de bois et de matériaux…
EUfr|wait.1.t|Désherbage en cours
EUfr|wait.1.s|On prépare votre coin de paradis…
EUfr|wait.2.t|Causette avec Tom Nook
EUfr|wait.2.s|Calcul du montant de votre prêt…
EUfr|wait.3.t|En attente du courrier
EUfr|wait.3.s|On vérifie la boîte du bureau des résidents…
EUfr|wait.4.t|Vérification du Dodo Code
EUfr|wait.4.s|On réchauffe les moteurs de l'hydravion…
EUfr|ver.t|Vol de Dodo Airlines retardé
EUfr|ver.s|Votre île a besoin de la Ver. 3.0.3\npour décoller\_!
EUfr|ver.have|(vous avez la Ver. {v})
EUfr|card.1.t|Annonce du matin en direct
EUfr|card.1.s|Marie a la parole\_! Regardez l'écran principal.
EUfr|card.2.t|Poches ouvertes sur le grand écran
EUfr|card.2.s|Terminez d'abord là-bas\_!
EUfr|card.3.t|NookPhone en cours d'utilisation
EUfr|card.3.s|Rangez votre téléphone pour continuer\_!
EUfr|card.5.t|Affaires de l'île en cours
EUfr|card.5.s|Attendez la fin de la conversation\_!
EUfr|card.9.t|Évènement spécial en cours
EUfr|card.9.s|Les objets ne peuvent pas bouger\npour l'instant\_!
EUfr|card.6.t|Hum, ça ne marchera pas
EUfr|card.6.s|Essayez un autre objet\_!
EUde|wait.0.t|Inselwege werden gepflastert
EUde|wait.0.s|Holz und Materialien werden gesammelt…
EUde|wait.1.t|Unkraut wird gejätet
EUde|wait.1.s|Dein Stückchen Paradies wird vorbereitet…
EUde|wait.2.t|Plausch mit Tom Nook
EUde|wait.2.s|Deine Kredithöhe wird berechnet…
EUde|wait.3.t|Warten auf die Post
EUde|wait.3.s|Der Briefkasten im Servicecenter\nwird geprüft…
EUde|wait.4.t|Dodo-Code wird geprüft
EUde|wait.4.s|Die Motoren des Wasserflugzeugs\nwerden angewärmt…
EUde|ver.t|Dodo-Airlines-Flug verspätet
EUde|ver.s|Deine Insel braucht Ver. 3.0.3 zum Abheben!
EUde|ver.have|(du hast Ver. {v})
EUde|card.1.t|Morgendurchsage läuft
EUde|card.1.s|Melinda spricht! Schau auf den Hauptbildschirm.
EUde|card.2.t|Tasche auf dem großen Bildschirm offen
EUde|card.2.s|Bring das dort erst zu Ende!
EUde|card.3.t|NookPhone in Benutzung
EUde|card.3.s|Steck dein Telefon weg, um fortzufahren!
EUde|card.5.t|Inselgeschäfte im Gange
EUde|card.5.s|Warte, bis das Gespräch vorbei ist!
EUde|card.9.t|Sonderevent im Gange
EUde|card.9.s|Gegenstände können gerade\nnicht bewegt werden!
EUde|card.6.t|Hm, das klappt nicht
EUde|card.6.s|Versuch es mit einem anderen Gegenstand!
EUit|wait.0.t|Lavori sui sentieri dell'isola
EUit|wait.0.s|Raccolta di legna e materiali…
EUit|wait.1.t|Via le erbacce
EUit|wait.1.s|Preparazione del tuo angolo di paradiso…
EUit|wait.2.t|Due chiacchiere con Tom Nook
EUit|wait.2.s|Calcolo del prestito da rimborsare…
EUit|wait.3.t|In attesa della posta
EUit|wait.3.s|Controllo della cassetta del centro servizi…
EUit|wait.4.t|Verifica del PIN Dodo
EUit|wait.4.s|Riscaldamento dei motori dell'idrovolante…
EUit|ver.t|Volo Dodo Airlines in ritardo
EUit|ver.s|La tua isola ha bisogno della\nVer. 3.0.3 per decollare!
EUit|ver.have|(hai la Ver. {v})
EUit|card.1.t|Annuncio del mattino in onda
EUit|card.1.s|Sta parlando Fuffi!\nGuarda lo schermo principale.
EUit|card.2.t|Tasche aperte sullo schermo grande
EUit|card.2.s|Prima finisci lì!
EUit|card.3.t|Nook Phone in uso
EUit|card.3.s|Metti via il telefono per continuare!
EUit|card.5.t|Faccende isolane in corso
EUit|card.5.s|Aspetta che la chiacchierata finisca!
EUit|card.9.t|Evento speciale in corso
EUit|card.9.s|Ora non si possono spostare gli oggetti!
EUit|card.6.t|Mmm, così non va
EUit|card.6.s|Prova con un altro oggetto!
EUnl|wait.0.t|Eilandpaden worden aangelegd
EUnl|wait.0.s|Hout en materialen verzamelen…
EUnl|wait.1.t|Onkruid wieden
EUnl|wait.1.s|Jouw stukje paradijs wordt klaargemaakt…
EUnl|wait.2.t|Kletsen met Tom Nook
EUnl|wait.2.s|Je resterende lening wordt berekend…
EUnl|wait.3.t|Wachten op de post
EUnl|wait.3.s|De brievenbus van de servicebalie\nwordt gecheckt…
EUnl|wait.4.t|Dodo-code controleren
EUnl|wait.4.s|De motoren van het watervliegtuig\nwarmen op…
EUnl|ver.t|Vlucht van Dodo Airlines vertraagd
EUnl|ver.s|Je eiland heeft Ver. 3.0.3 nodig\nom op te stijgen!
EUnl|ver.have|(jij hebt Ver. {v})
EUnl|card.1.t|Ochtendomroep in de lucht
EUnl|card.1.s|Isabelle is aan het woord!\nKijk naar het hoofdscherm.
EUnl|card.2.t|Zakken open op het grote scherm
EUnl|card.2.s|Rond dat daar eerst af!
EUnl|card.3.t|NookPhone in gebruik
EUnl|card.3.s|Berg je telefoon op om verder te gaan!
EUnl|card.5.t|Eilandzaken in volle gang
EUnl|card.5.s|Wacht tot het gesprek is afgelopen!
EUnl|card.9.t|Speciaal evenement bezig
EUnl|card.9.s|Voorwerpen kunnen nu niet verplaatst worden!
EUnl|card.6.t|Hmm, dat gaat niet
EUnl|card.6.s|Probeer een ander voorwerp!
EUru|wait.0.t|Мостим островные тропинки
EUru|wait.0.s|Собираем древесину и материалы…
EUru|wait.1.t|Пропалываем сорняки
EUru|wait.1.s|Готовим твой уголок рая…
EUru|wait.2.t|Болтаем с Томом Нуком
EUru|wait.2.s|Подсчитываем остаток ссуды…
EUru|wait.3.t|Ждем почту
EUru|wait.3.s|Проверяем почтовый ящик бюро услуг…
EUru|wait.4.t|Проверяем Додо-код
EUru|wait.4.s|Прогреваем двигатели гидросамолета…
EUru|ver.t|Рейс Dodo Airlines задерживается
EUru|ver.s|Твоему острову нужна Вер. 3.0.3 для взлета!
EUru|ver.have|(у тебя Вер. {v})
EUru|card.1.t|Утренний эфир
EUru|card.1.s|Говорит Изабель! Смотри на главный экран.
EUru|card.2.t|Карманы открыты на большом экране
EUru|card.2.s|Сначала закончи там!
EUru|card.3.t|Нукофон занят
EUru|card.3.s|Убери телефон, чтобы продолжить!
EUru|card.5.t|Островные дела в разгаре
EUru|card.5.s|Подожди, пока закончится разговор!
EUru|card.9.t|Идет особое событие
EUru|card.9.s|Сейчас предметы перемещать нельзя!
EUru|card.6.t|Хм, так не выйдет
EUru|card.6.s|Попробуй другой предмет!
JPja|wait.0.t|島の道を整備中
JPja|wait.0.s|木材や素材を集めています…
JPja|wait.1.t|草むしり中
JPja|wait.1.s|あなただけの楽園を準備しています…
JPja|wait.2.t|たぬきちとおしゃべり中
JPja|wait.2.s|ローン残高を計算しています…
JPja|wait.3.t|郵便を待っています
JPja|wait.3.s|案内所のポストを確認しています…
JPja|wait.4.t|パスワードを確認中
JPja|wait.4.s|飛行機のエンジンを温めています…
JPja|ver.t|ドードー・エアラインズ 遅延のお知らせ
JPja|ver.s|島から飛び立つには Ver. 3.0.3 が必要です！
JPja|ver.have|（現在のバージョン：Ver. {v}）
JPja|card.1.t|朝の放送中
JPja|card.1.s|しずえさんがお話し中！メイン画面を見てね。
JPja|card.2.t|大きな画面でポケットを開いています
JPja|card.2.s|先にそちらを終わらせてね！
JPja|card.3.t|スマホを使用中
JPja|card.3.s|スマホをしまってから続けてね！
JPja|card.5.t|島のお仕事中
JPja|card.5.s|おしゃべりが終わるまで待ってね！
JPja|card.9.t|特別なイベント開催中
JPja|card.9.s|今はアイテムを動かせません！
JPja|card.6.t|うーん、それはできないみたい
JPja|card.6.s|別のアイテムを試してね！
KRko|wait.0.t|섬의 길을 정비하는 중
KRko|wait.0.s|목재와 재료를 모으고 있어요…
KRko|wait.1.t|잡초를 뽑는 중
KRko|wait.1.s|나만의 낙원을 준비하고 있어요…
KRko|wait.2.t|너굴과 수다 떠는 중
KRko|wait.2.s|대출 잔액을 계산하고 있어요…
KRko|wait.3.t|우편을 기다리는 중
KRko|wait.3.s|안내소 우편함을 확인하고 있어요…
KRko|wait.4.t|비밀번호 확인 중
KRko|wait.4.s|비행기 엔진을 예열하고 있어요…
KRko|ver.t|도도항공 항공편 지연 안내
KRko|ver.s|섬이 이륙하려면 Ver. 3.0.3이 필요해요!
KRko|ver.have|(현재 버전: Ver. {v})
KRko|card.1.t|아침 방송 중
KRko|card.1.s|여울이 말하고 있어요! 메인 화면을 봐 주세요.
KRko|card.2.t|큰 화면에서 주머니가 열려 있어요
KRko|card.2.s|먼저 그쪽을 마무리해 주세요!
KRko|card.3.t|스마트폰 사용 중
KRko|card.3.s|스마트폰을 넣어야 계속할 수 있어요!
KRko|card.5.t|섬 업무 진행 중
KRko|card.5.s|대화가 끝날 때까지 기다려 주세요!
KRko|card.9.t|특별 이벤트 진행 중
KRko|card.9.s|지금은 물건을 옮길 수 없어요!
KRko|card.6.t|음, 그건 안 되겠어요
KRko|card.6.s|다른 물건을 골라 보세요!
CNzh|wait.0.t|正在铺设岛上的道路
CNzh|wait.0.s|正在收集木材和材料…
CNzh|wait.1.t|正在拔杂草
CNzh|wait.1.s|正在打造你的专属乐园…
CNzh|wait.2.t|正在和狸克聊天
CNzh|wait.2.s|正在计算你的贷款余额…
CNzh|wait.3.t|正在等待信件
CNzh|wait.3.s|正在查看服务处的信箱…
CNzh|wait.4.t|正在确认密码
CNzh|wait.4.s|正在为飞机引擎预热…
CNzh|ver.t|DodoAirLines 航班延误
CNzh|ver.s|你的岛需要 Ver. 3.0.3 才能起飞！
CNzh|ver.have|（当前版本：Ver. {v}）
CNzh|card.1.t|早间广播进行中
CNzh|card.1.s|西施惠正在讲话！请看主画面。
CNzh|card.2.t|大画面上正打开着口袋
CNzh|card.2.s|请先在那边完成操作！
CNzh|card.3.t|正在使用手机
CNzh|card.3.s|请先收起手机再继续！
CNzh|card.5.t|岛上事务进行中
CNzh|card.5.s|请等待对话结束！
CNzh|card.9.t|特别活动进行中
CNzh|card.9.s|现在无法移动物品！
CNzh|card.6.t|嗯，这样不行呢
CNzh|card.6.s|试试别的物品吧！
TWzh|wait.0.t|正在鋪設島上的道路
TWzh|wait.0.s|正在收集木材和材料…
TWzh|wait.1.t|正在拔雜草
TWzh|wait.1.s|正在打造你的專屬樂園…
TWzh|wait.2.t|正在和狸克聊天
TWzh|wait.2.s|正在計算你的貸款餘額…
TWzh|wait.3.t|正在等待信件
TWzh|wait.3.s|正在查看服務處的信箱…
TWzh|wait.4.t|正在確認密碼
TWzh|wait.4.s|正在為飛機引擎預熱…
TWzh|ver.t|DodoAirLines 航班延誤
TWzh|ver.s|你的島需要 Ver. 3.0.3 才能起飛！
TWzh|ver.have|（目前版本：Ver. {v}）
TWzh|card.1.t|早晨廣播進行中
TWzh|card.1.s|西施惠正在講話！請看主畫面。
TWzh|card.2.t|大畫面上正開著口袋
TWzh|card.2.s|請先在那邊完成操作！
TWzh|card.3.t|正在使用手機
TWzh|card.3.s|請先收起手機再繼續！
TWzh|card.5.t|島上事務進行中
TWzh|card.5.s|請等待對話結束！
TWzh|card.9.t|特別活動進行中
TWzh|card.9.s|現在無法移動物品！
TWzh|card.6.t|嗯，這樣不行呢
TWzh|card.6.s|試試別的物品吧！
USen|phone.lock.t|Your Phone Is on Its Way
USen|phone.lock.s|Keep playing on the main screen!
EUen|phone.lock.t|Your Phone Is on Its Way
EUen|phone.lock.s|Keep playing on the main screen!
EUde|phone.lock.t|Dein Telefon kommt bald
EUde|phone.lock.s|Spiele auf dem Hauptbildschirm weiter!
EUes|phone.lock.t|Tu teléfono está en camino
EUes|phone.lock.s|¡Sigue jugando en la pantalla principal!
USes|phone.lock.t|Tu teléfono está en camino
USes|phone.lock.s|¡Sigue jugando en la pantalla principal!
EUfr|phone.lock.t|Ton téléphone arrive bientôt
EUfr|phone.lock.s|Continue à jouer sur l’écran principal !
USfr|phone.lock.t|Ton téléphone arrive bientôt
USfr|phone.lock.s|Continue à jouer sur l’écran principal !
EUit|phone.lock.t|Il tuo telefono sta arrivando
EUit|phone.lock.s|Continua a giocare sullo schermo principale!
EUnl|phone.lock.t|Je telefoon komt eraan
EUnl|phone.lock.s|Speel verder op het hoofdscherm!
EUru|phone.lock.t|Телефон уже в пути
EUru|phone.lock.s|Продолжай играть на основном экране!
JPja|phone.lock.t|スマホはもうすぐ届くよ
JPja|phone.lock.s|メイン画面で お話を進めよう！
KRko|phone.lock.t|스마트폰이 곧 도착해요
KRko|phone.lock.s|메인 화면에서 계속 진행해 주세요!
CNzh|phone.lock.t|手机就快送到啦
CNzh|phone.lock.s|请在主画面继续游玩！
TWzh|phone.lock.t|手機就快送到囉
TWzh|phone.lock.s|請在主畫面繼續遊玩！
)TSV";

uint32_t Next(std::string_view s, size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    int len = b0 < 0x80             ? 1
              : (b0 & 0xE0) == 0xC0 ? 2
              : (b0 & 0xF0) == 0xE0 ? 3
              : (b0 & 0xF8) == 0xF0 ? 4
                                    : 1;
    if (i + len > s.size())
        len = 1;
    uint32_t cp = len == 1 ? b0 : len == 2 ? (b0 & 0x1F) : len == 3 ? (b0 & 0x0F) : (b0 & 0x07);
    for (int k = 1; k < len; ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += len;
    return cp;
}

std::string Unescape(std::string_view t) {
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\\' && i + 1 < t.size() && (t[i + 1] == 'n' || t[i + 1] == '_')) {
            out += t[i + 1] == 'n' ? "\n" : "\xC2\xA0";
            ++i;
        } else {
            out += t[i];
        }
    }
    return out;
}

struct Data {
    std::vector<Line> lines;
    std::vector<Build> builds;
};

const Data& Get() {
    static const Data data = [] {
        Data d;
        std::vector<Line> templates;
        size_t at = 0;
        while (at < Table.size()) {
            size_t end = Table.find('\n', at);
            if (end == std::string_view::npos)
                end = Table.size();
            const std::string_view row = Table.substr(at, end - at);
            at = end + 1;
            const size_t t1 = row.find('|');
            const size_t t2 = t1 == std::string_view::npos ? t1 : row.find('|', t1 + 1);
            if (t2 == std::string_view::npos)
                continue;
            const std::string_view lang = row.substr(0, t1), key = row.substr(t1 + 1, t2 - t1 - 1),
                                   text = row.substr(t2 + 1);
            if (lang == "*") {
                if (key == "build" && text.size() > 17)
                    d.builds.push_back(
                        {std::string{text.substr(0, 16)}, std::string{text.substr(17)}});
                continue;
            }
            const Lang l = LangFromFolder(lang, Lang::Count);
            if (l == Lang::Count)
                continue;
            Line line{l, std::string{key}, Unescape(text)};
            if (line.key == "ver.have")
                templates.push_back(std::move(line));
            else
                d.lines.push_back(std::move(line));
        }
        for (const auto& t : templates)
            for (size_t b = 0; b < d.builds.size(); ++b) {
                std::string text = t.text;
                if (const size_t p = text.find("{v}"); p != std::string::npos)
                    text.replace(p, 3, d.builds[b].version);
                d.lines.push_back({t.lang, "have." + std::to_string(b), std::move(text)});
            }
        return d;
    }();
    return data;
}

bool IsHangul(uint32_t cp) {
    return (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF) ||
           (cp >= 0x3130 && cp <= 0x318F);
}

} // namespace

const std::vector<Line>& Lines() {
    return Get().lines;
}

std::string_view Text(Lang lang, std::string_view key) {
    for (const auto& l : Get().lines)
        if (l.lang == lang && l.key == key)
            return l.text;
    return {};
}

const std::vector<Build>& Builds() {
    return Get().builds;
}

int BuildIndex(std::string_view hex) {
    // the table has the first 8 bytes; a build id is 16 significant bytes, the rest zero
    if (hex.size() < 16)
        return -1;
    for (size_t i = 32; i < hex.size(); ++i)
        if (hex[i] != '0')
            return -1;
    const auto& b = Get().builds;
    for (size_t k = 0; k < b.size(); ++k) {
        bool same = true;
        for (size_t i = 0; i < 16 && same; ++i)
            same = std::toupper(static_cast<unsigned char>(hex[i])) == b[k].id16[i];
        if (same)
            return static_cast<int>(k);
    }
    return -1;
}

std::vector<uint32_t> Codepoints(Lang lang) {
    std::vector<uint32_t> out;
    for (const auto& l : Get().lines) {
        if (l.lang != lang && lang != Lang::Count)
            continue;
        for (size_t i = 0; i < l.text.size();) {
            const uint32_t cp = Next(l.text, i);
            if (cp >= 0x20)
                out.push_back(cp);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<uint32_t> CodepointsAll() {
    return Codepoints(Lang::Count);
}

Lang CjkFaceFor(uint32_t cp) {
    if (IsHangul(cp))
        return Lang::KRko;
    if (cp < 0x2E80)
        return Lang::Count;
    static const auto sets = [] {
        std::array<std::vector<uint32_t>, 3> s{Codepoints(Lang::JPja), Codepoints(Lang::CNzh),
                                               Codepoints(Lang::TWzh)};
        return s;
    }();
    const auto in = [](const std::vector<uint32_t>& v, uint32_t c) {
        return std::binary_search(v.begin(), v.end(), c);
    };
    if (in(sets[0], cp)) // Japanese lines: Seurat-B has kana + kanji
        return Lang::Count;
    if (in(sets[1], cp))
        return Lang::CNzh;
    if (in(sets[2], cp))
        return Lang::TWzh;
    return Lang::Count;
}

int WaitCount() {
    static const int count =
        [] { // the table is constant: counted once (the glue asks every sample)
            int n = 0;
            while (!Text(Lang::USen, "wait." + std::to_string(n) + ".t").empty())
                ++n;
            return n;
        }();
    return count;
}

} // namespace acnh::flavor
