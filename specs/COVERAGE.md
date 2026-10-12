# Карта процедур DD2.EXE и покрытие портом

Сгенерировано `re/tools/coverage.py`. Точки входа -- все цели
near-вызовов (`E8`) в кодовой части образа плюс точки, которые
вызываются только через указатели (ISR, таблицы тегов, react).
Размер -- оценка «до следующей точки входа».

Всего процедур: **231**; портировано **152**, частично **0**, не начато **0** (66% затронуто).

Отдельно: **реализовано иначе** (поведение своё, совпадения с оригиналом нет) -- **2**: `0x25e3`, `0x649c`.

И ещё **96** процедур заменены эквивалентом (EGA->SDL, libc, свои декодеры) и **7** -- не процедуры, а внутренние блоки уже перенесённых функций.

## Покрытие по областям кода

| область | что там | всего | портировано | частично | не начато |
|---|---|---|---|---|---|
| `0x0000`..`0x0400` | старт C0.ASM, свой INT 9, детект видео/джойстика | 0 | 0 | 0 | 0 |
| `0x0400`..`0x0e00` | F-меню, конфигураторы, экраны информации | 11 | 11 | 0 | 0 |
| `0x0e00`..`0x1500` | PROGPIC, чтение CTLPANEL, публикация таблиц тайлов | 3 | 3 | 0 | 0 |
| `0x1500`..`0x1900` | main(), заставки TITLE1/TITLE2/STARPIC/INTRO | 4 | 4 | 0 | 0 |
| `0x1900`..`0x2300` | объектная система: bbox, SetState, Update, кадр 0x1EAB | 10 | 10 | 0 | 0 |
| `0x2300`..`0x2e00` | сканы тайлов, попапы очков, экраны паузы/статуса | 4 | 4 | 0 | 0 |
| `0x2e00`..`0x3600` | перемещение и коллизии, подтягивание, призы 0x3539 | 13 | 13 | 0 | 0 |
| `0x3600`..`0x4400` | старт игрока, двери 0x394F, состояния Дэйва, смерть | 18 | 18 | 0 | 0 |
| `0x4400`..`0x5400` | спавн врагов и их поведение, спавн по тегам 0x53FF | 44 | 44 | 0 | 0 |
| `0x5400`..`0x6800` | уровни: загрузка, кэш тайлов, переходы | 2 | 2 | 0 | 0 |
| `0x6800`..`0x7800` | главный цикл 0x6720, аттракт, звёздный движок | 12 | 12 | 0 | 0 |
| `0x7800`..`0x9400` | видео: PIC, EGA-плоскости, шрифт, RLE/HUFF, вывод текста | 28 | 28 | 0 | 0 |
| `0x9400`..`0xc000` | спрайтовый блиттер 0x94C4, звук, ISR, PIT, обёртки libc | 1 | 1 | 0 | 0 |
| `0xc000`..`0x27040` | библиотека Borland C++ 3.0 (не переносится) | 69 | 1 | 0 | 68 |

## Портировано

| адрес | ~размер | вызовов | что это | C |
|---|---|---|---|---|
| `0x041c` | 99 | 2 | диспетчер F-меню | `input.c` :: `Input_MenuKey` |
| `0x047f` | 73 | 0 | F2 звук | `input.c` :: `Input_DirY` |
| `0x04c8` | 9 | 0 | F3 клавиатура | `input.c` :: `Input_DirY` |
| `0x04d1` | 15 | 0 | F4 джойстик | `input.c` :: `Input_DirY` |
| `0x04e0` | 63 | 0 | F5 сброс | `input.c` :: `Input_DirY` |
| `0x051f` | 60 | 0 | ESC/выход | `input.c` :: `Input_DirY` |
| `0x055b` | 1160 | 0 | TAB статус | `input.c` :: `Input_DirY` |
| `0x09e3` | 369 | 2 | загрузчик чанка спрайтов: 5 плоскостей с offset 2, размер = rec[+0]*rec[+2] | `dd2.c` :: `decode_sprite_chunk` |
| `0x0b54` | 155 | 0 | НЕ процедура: внутренний блок 0x09E3 (фантомная 140-я запись actor-info, перекрывает таблицу призов 0x1260) | -- |
| `0x0bef` | 420 | 1 | строка: '91 Borland Intl.'; вызывает: центрированный текст, текст | `main.c` :: `Screen_SpriteTest` |
| `0x0dad` | 1146 | 1 | загрузка уровня | `dd2.c` :: `dd2_load_level` |
| `0x1227` | 633 | 1 | экран карты уровня: PROGPIC.DD2 + панели LEVEL n / LOADING | `main.c` :: `map_screen` |
| `0x14a0` | 88 | 1 | титульный экран (TITLE1/TITLE2) | `video.c` :: `Video_TitlePics` |
| `0x14f8` | 254 | 1 | цикл титульного экрана | `video.c` :: `Video_AttractFrame` |
| `0x15f6` | 419 | 1 | main() | `main.c` :: `main` |
| `0x1799` | 91 | 1 | Obj_Alloc | `objects.c` :: `Obj_Alloc` |
| `0x17f4` | 232 | 43 | Obj_Spawn | `objects.c` :: `Obj_Spawn` |
| `0x18dc` | 82 | 1 | пересечение bbox | `objects.c` :: `Obj_Overlap` |
| `0x192e` | 146 | 9 | bbox по spriteInfo | `objects.c` :: `Obj_Box` |
| `0x19c0` | 46 | 4 | гравитация | `objects.c` :: `Obj_Gravity` |
| `0x19ee` | 219 | 3 | область: объектная система | `objects.c` :: `Obj_PushOutY` |
| `0x1ac9` | 212 | 11 | область: объектная система | `objects.c` :: `Obj_PushOutX` |
| `0x1b9d` | 63 | 2 | область: объектная система | `objects.c` :: `Obj_GroundBelow` |
| `0x1bdc` | 334 | 2 | область: объектная система | `level.c` :: `Level_Camera` |
| `0x1d2a` | 76 | 51 | SetState(rec) | `objects.c` :: `Obj_SetState` |
| `0x1d76` | 309 | 1 | обновление объекта | `objects.c` :: `Obj_Update` |
| `0x1eab` | 773 | 1 | кадр: 3 прохода по объектам | `objects.c` :: `Obj_Frame` |
| `0x21b0` | 981 | 2 | строка: 'G A M E O V E R'; вызывает: экран карты, ждать клавишу, пауза, кадр объектов | `main.c` :: `game_restart` |
| `0x2585` | 1415 | 1 | дельта кадра | `objects.c` :: `Obj_Delta` |
| `0x2b0c` | 289 | 24 | постановка команды рисования спрайта в очередь [0x905C] | `main.c` :: `dave_door` |
| `0x2c2d` | 283 | 7 | очередь спрайта с битом 0x8000 (вспышка) -- двойник 0x2B0C | `main.c` :: `draw_sprite_flash` |
| `0x2d48` | 191 | 1 | отрисовка спрайта: запись = sprite*4 + (screen_x mod 8)/4 | `main.c` :: `draw_sprite` |
| `0x2e07` | 78 | 1 | кламп+применение перемещения | `objects.c` :: `Obj_ClampMove` |
| `0x2e55` | 22 | 0 | react записей: DAVEOPEN, DAVEENTER1, DAVEENTER2, DAVEENTER3...; вызывает: очередь спрайта, проверка ухода с экрана | `objects.c` :: `Obj_DrawCullReact` |
| `0x2e6b` | 69 | 0 | react записей: MASTER1, MASTER2, MASTCLAW1, MASTCLAW2; вызывает: очередь спрайта (вспышка), очередь спрайта, проверка ухода с экрана | `main.c` :: `obj_react` |
| `0x2eb0` | 25 | 0 | react записей: SLIMERDYSW, SLIMERDYNW; вызывает: очередь спрайта, проверка ухода с экрана | `level.c` :: `?` |
| `0x2ec9` | 72 | 0 | react записей: RELOAD1, RELOAD2, WOLFRUNL4; вызывает: очередь спрайта (вспышка), очередь спрайта, проверка ухода с экрана | `objects.c` :: `Obj_ReloadReact` |
| `0x2f11` | 229 | 0 | react записей: ZOMBWALKW1, ZOMBWALKW2, ZOMBWALKW3, ZOMBWALKW4...; вызывает: выталкивание из тайла, таблица «случайных» байт, SetState, bbox | `objects.c` :: `Obj_WalkAI` |
| `0x2ff6` | 279 | 0 | react записей: SLIMEWTOP1, SLIMEWTOP2, SPIDERL1, SPIDERL2; вызывает: SetState, bbox, выталкивание из тайла, очередь спрайта (вспышка) | `main.c` :: `obj_react` |
| `0x310d` | 24 | 0 | fn записей: ZATTACKW1, ZATTACKW2, ZATTACKW3, ZOMBDOWN1... | `objects.c` :: `Obj_FaceDave` |
| `0x3125` | 991 | 1 | веер осколков (Obj_ChunkFan); вызывает: Obj_Spawn | `objects.c` :: `Obj_ChunkFan` |
| `0x3504` | 26 | 0 | fn записей: BONUS100, BONUS200, BONUS400, BONUS800... | `objects.c` :: `Obj_BonusTimer` |
| `0x351e` | 27 | 0 | fn записей: BONUS1UP | `objects.c` :: `Obj_BonusTimer` |
| `0x3539` | 63 | 2 | приз/бонус | `level.c` :: `Level_SpawnBonus` |
| `0x3578` | 243 | 3 | вызывает: Obj_Spawn, звук | `objects.c` :: `score_extra_reset` |
| `0x366b` | 43 | 0 | fn записей: CHUNKNW1, CHUNKNW2, CHUNKSW1, CHUNKSW2; вызывает: SetState | `objects.c` :: `Obj_ChunkFly` |
| `0x3696` | 14 | 0 | fn записей: SPARKCHUNK | `objects.c` :: `Obj_SparkChunkFly` |
| `0x36a4` | 99 | 0 | react записей: CHUNKNW1, CHUNKNW2, CHUNKSW1, CHUNKSW2; вызывает: SetState, очередь спрайта | `objects.c` :: `Obj_ChunkLand` |
| `0x3707` | 58 | 0 | react записей: SPARKCHUNK; вызывает: очередь спрайта | `objects.c` :: `Obj_SparkReact` |
| `0x3741` | 109 | 1 | старт игрока (слот 0) | `level.c` :: `Level_SpawnPlayer` |
| `0x37ae` | 227 | 3 | диспатч состояния по cmd | `dave.c` :: `Dave_Dispatch` |
| `0x3891` | 190 | 0 | Дэйв: стойка/ходьба | `dave.c` :: `Dave_Frame` |
| `0x394f` | 268 | 0 | двери/телепорты | `level.c` :: `Level_Door` |
| `0x3a5b` | 276 | 0 | дверь: звук 0x0C и адрес анимации в видеопамяти (fn записи 0x834) | `level.c` :: `Level_DoorOpen` |
| `0x3b6f` | 190 | 0 | Дэйв: прицел/подтягивание | `dave.c` :: `Dave_Aim` |
| `0x3c2d` | 31 | 0 | Дэйв: перезарядка | `dave.c` :: `Dave_Reload` |
| `0x3c4c` | 404 | 4 | Дэйв: hitscan | `dave.c` :: `Dave_Hitscan` |
| `0x3de0` | 659 | 0 | Дэйв: выстрел | `dave.c` :: `Dave_Shoot` |
| `0x4073` | 4 | 1 | fn записей: AIMW2, AIMNW2, AIMSW2 | `dave.c` :: `Dave_Step` |
| `0x4077` | 705 | 0 | Дэйв: прыжок | `dave.c` :: `Dave_Jump` |
| `0x4338` | 72 | 0 | react записей: DAVESTANDW, DAVEWALKW1, DAVEWALKW2, DAVEWALKW3...; вызывает: выталкивание из тайла, SetState, очередь спрайта | `objects.c` :: `Obj_WalkerReact` |
| `0x4380` | 41 | 0 | react записей: AIMW1, AIMW2, AIMNW1, AIMNW2...; вызывает: выталкивание из тайла, SetState, очередь спрайта | `objects.c` :: `Obj_WalkerReact` |
| `0x43a9` | 121 | 0 | react записей: DAVEJUMPW1, DAVEJUMPW2, DAVEJUMPW3; вызывает: звук, bbox, SetState, очередь спрайта | `dave.c` :: `Dave_PhysReact` |
| `0x4422` | 188 | 1 | скан тайлов вокруг Дэйва | `level.c` :: `Level_ScanTiles` |
| `0x44de` | 76 | 1 | спавн: тег 1 | `level.c` :: `Enemy_Tag1` |
| `0x452a` | 249 | 0 | fn записей: ZOMBWALKW1, ZOMBWALKW2, ZOMBWALKW3, ZOMBWALKW4; вызывает: SetState | `main.c` :: `obj_perframe` |
| `0x4623` | 119 | 0 | react записей: ZOMBDOWN1, ZOMBDOWN2; вызывает: SetState, очередь спрайта (вспышка), очередь спрайта | `objects.c` :: `Obj_ZombieDownReact` |
| `0x469a` | 58 | 1 | спавн: тег 2 | `level.c` :: `Enemy_Tag2` |
| `0x46d4` | 151 | 0 | fn записей: HNCHWALKW1, HNCHWALKW2, HNCHWALKW3, HNCHWALKW4; вызывает: таблица «случайных» байт, звук, SetState | `objects.c` :: `Obj_HunchbackAI` |
| `0x476b` | 75 | 0 | fn записей: HNATTACKW3; вызывает: Obj_Spawn | `objects.c` :: `Obj_HunchThrow` |
| `0x47b6` | 48 | 0 | react записей: KNIFEW; вызывает: выталкивание из тайла, SetState, очередь спрайта, проверка ухода с экрана | `objects.c` :: `Obj_KnifeReact` |
| `0x47e6` | 146 | 1 | спавн: тег 3 | `level.c` :: `Enemy_Tag3` |
| `0x4878` | 58 | 0 | fn записей: SLIMEWTOP1, SLIMEWTOP2, SLIMEWBOT1, SLIMEWBOT2; вызывает: таблица «случайных» байт, SetState | `objects.c` :: `Obj_SlimeWander` |
| `0x48b2` | 76 | 0 | react записей: SLIMEJMPSW, SLIMEJMPNW; вызывает: SetState, звук, очередь спрайта, проверка ухода с экрана | `objects.c` :: `Obj_SlimeCrawl` |
| `0x48fe` | 54 | 1 | спавн: тег 4 | `level.c` :: `Enemy_Tag4` |
| `0x4934` | 59 | 1 | спавн: тег 5 | `level.c` :: `Enemy_Tag5` |
| `0x496f` | 98 | 0 | fn записей: STARTDAVE; вызывает: звук, SetState | `objects.c` :: `Obj_GhostStartDave` |
| `0x49d1` | 62 | 0 | fn записей: GHOSTSOLID; вызывает: звук | `objects.c` :: `Obj_GhostSolid` |
| `0x4a0f` | 155 | 0 | fn записей: GHOSTW1, STARTDAVE | `main.c` :: `obj_perframe` |
| `0x4aaa` | 55 | 1 | спавн: тег 9 | `level.c` :: `Enemy_Tag9` |
| `0x4ae1` | 106 | 0 | fn записей: WOLFRUNL1, WOLFRUNL2, WOLFRUNL3, WOLFRUNL4; вызывает: SetState | `objects.c` :: `Obj_WolfAI` |
| `0x4b4b` | 225 | 0 | react записей: WOLFRUNL1, WOLFRUNL2, WOLFRUNL3, WOLFRUNL4; вызывает: выталкивание из тайла, SetState, bbox, очередь спрайта (вспышка) | `objects.c` :: `Obj_WolfReact` |
| `0x4c2c` | 4 | 0 | fn записей: WOLFJUMPL | `objects.c` :: `Obj_WolfJump` |
| `0x4c30` | 176 | 0 | react записей: WOLFJUMPL; вызывает: SetState, bbox, очередь спрайта (вспышка), очередь спрайта | `objects.c` :: `Obj_WolfBounce` |
| `0x4ce0` | 51 | 1 | спавн: тег 7 | `level.c` :: `Enemy_Tag7` |
| `0x4d13` | 158 | 0 | fn записей: SKULLFIREL | `objects.c` :: `Obj_SkullAI` |
| `0x4db1` | 25 | 0 | react записей: SKULLFIREL; вызывает: очередь спрайта, проверка ухода с экрана | `objects.c` :: `Obj_SkullReact` |
| `0x4dca` | 61 | 1 | спавн: тег 6 | `level.c` :: `Enemy_Tag6` |
| `0x4e07` | 108 | 0 | fn записей: SPIDERL1, SPIDERL2; вызывает: SetState | `objects.c` :: `Obj_SpiderAI` |
| `0x4e73` | 151 | 0 | react записей: STARTDAVE; вызывает: выталкивание из тайла, SetState, очередь спрайта | `main.c` :: `dave_door` |
| `0x4f0a` | 88 | 0 | react записей: STARTDAVE; вызывает: выталкивание из тайла, SetState, очередь спрайта | `main.c` :: `dave_door` |
| `0x4f62` | 56 | 1 | спавн: тег 0x0B | `level.c` :: `Enemy_TagB` |
| `0x4f9a` | 26 | 0 | fn записей: FRANKGOL1, FRANKGOL2, FRANKGOL3, FRANKGOL4; вызывает: SetState | `objects.c` :: `Obj_FrankAnim` |
| `0x4fb4` | 82 | 0 | fn записей: FRANKGOL1; вызывает: Obj_Spawn | `objects.c` :: `Obj_FrankThrow` |
| `0x5006` | 31 | 0 | fn записей: FRANKSPARK | `objects.c` :: `Obj_SparkFly` |
| `0x5025` | 22 | 0 | fn записей: FRANKSPARK; вызывает: SetState | `objects.c` :: `Obj_SparkTimer` |
| `0x503b` | 101 | 0 | react записей: FRANKSPARK; вызывает: очередь спрайта | `objects.c` :: `Obj_FrankSparkReact` |
| `0x50a0` | 56 | 1 | спавн: тег 0x0C | `level.c` :: `Enemy_TagC` |
| `0x50d8` | 295 | 0 | fn записей: MASTER1; вызывает: Obj_Spawn, SetState, звук | `main.c` :: `obj_perframe` |
| `0x51ff` | 211 | 0 | fn записей: MASTER1, MASTER2; вызывает: Obj_Spawn, SetState | `main.c` :: `obj_perframe` |
| `0x52d2` | 165 | 0 | fn записей: MASTCLAW1, MASTCLAW2 | `main.c` :: `obj_perframe` |
| `0x5377` | 21 | 0 | fn записей: MASTFIRE1, MASTFIRE2 | `objects.c` :: `Obj_MastFire` |
| `0x538c` | 51 | 1 | спавн: тег 0x0D | `level.c` :: `Enemy_TagD` |
| `0x53bf` | 19 | 0 | react записей: DELBERT1, DELBERT3, DELBERT4; вызывает: очередь спрайта | `objects.c` :: `Obj_DrawCullReact` |
| `0x53d2` | 38 | 0 | react записей: DELBERT1; вызывает: SetState, очередь спрайта | `objects.c` :: `Obj_DelbertReact` |
| `0x53f8` | 7 | 0 | fn записей: DELBERT4 | `objects.c` :: `Obj_DelbertWin` |
| `0x53ff` | 4055 | 2 | спавн из плоскости тегов | `level.c` :: `Level_SpawnTags` |
| `0x63d6` | 842 | 1 | инициализация звёздного движка | `video.c` :: `Video_AttractBegin` |
| `0x6720` | 1170 | 1 | главный цикл: заставка+игра | `main.c` :: `main` |
| `0x6bb2` | 579 | 1 | строка: 'press a button:'; вызывает: рамка окна, центрированный текст, текст, тайл 8x8 | `main.c` :: `Screen_JoyConfig` |
| `0x6df5` | 147 | 4 | строка: 'ESC'; вызывает: текст | `input.c` :: `Input_KeyName` |
| `0x6e88` | 212 | 1 | строка: 'F11'; вызывает: текст | `input.c` :: `Input_KeyName` |
| `0x6f5c` | 585 | 1 | конфиг клавиш | `input.c` :: `?` |
| `0x71a5` | 323 | 2 | построить {cmd,button1,button2} | `input.c` :: `Input_Frame` |
| `0x72e8` | 158 | 4 | детект джойстика | `platform.c` :: `plat_open_pad` |
| `0x7386` | 40 | 1 | строка: 'CTRL : Drop down' | `main.c` :: `main` |
| `0x73ae` | 413 | 6 | строка: 'CTRL : Drop down' | `main.c` :: `main` |
| `0x754b` | 166 | 3 | опрос ввода: выбор устройства по [0x97D6] (0=0x71A5 клавиатура, 2/3=0x73AE джойстик) | `main.c` :: `input_apply_pad` |
| `0x75f1` | 70 | 10 | вызывает: ждать клавишу | `input.c` :: `Input_WaitKey` |
| `0x7637` | 453 | 4 | загрузка файла | `dd2.c` :: `dd2_load_any` |
| `0x77fc` | 102 | 2 | загрузка PIC (ветка A, RLE 0xFE без 'PIC') | `dd2.c` :: `dd2_rle_expand` |
| `0x7862` | 214 | 3 | загрузка PIC (ветка B): выбор декодера и показ | `video.c` :: `Video_ShowPicAt` |
| `0x7938` | 16 | 5 | пауза перед показом | `video.c` :: `Video_Prep` |
| `0x7948` | 531 | 1 | HUFF | `dd2.c` :: `dd2_huff_expand` |
| `0x7b5b` | 153 | 1 | RLEW (реальный вход 0x7B4D; 0x7B5B -- середина инструкции) | `dd2.c` :: `dd2_rlew_expand` |
| `0x7bf4` | 131 | 1 | RLE FE/count/value | `dd2.c` :: `dd2_rle_expand` |
| `0x7c77` | 155 | 3 | показ PIC | `video.c` :: `Video_ShowPicAt` |
| `0x7d12` | 89 | 1 | область: видео: PIC, плоскости, шрифт, RLE/HUFF | `video.c` :: `Video_Clear` |
| `0x7d6b` | 270 | 2 | работа с портами EGA | `main.c` :: `present_now` |
| `0x7e79` | 313 | 1 | строка: 'th: Height: Source:' | `video.c` :: `Video_SetWidth` |
| `0x7fb2` | 23 | 4 | область: видео: PIC, плоскости, шрифт, RLE/HUFF | `video.c` :: `Video_SetWidth` |
| `0x7fc9` | 358 | 5 | строка: '1991 Borland Intl.'; вызывает: тайл 8x8 | `main.c` :: `present_now` |
| `0x812f` | 127 | 8 | вызывает: рамка панели | `main.c` :: `port_sprite_bbox` |
| `0x81ae` | 92 | 16 | рамка окна + анимация роста 0x820A/0x823C (в порте только конечная панель) | `video.c` :: `Video_CenterPanel` |
| `0x820a` | 50 | 2 | область: видео: PIC, плоскости, шрифт, RLE/HUFF | `video.c` :: `Video_FrameB` |
| `0x823c` | 1136 | 2 | область: видео: PIC, плоскости, шрифт, RLE/HUFF | `video.c` :: `Video_FrameC` |
| `0x86ac` | 192 | 1 | вызывает: символ | `input.c` :: `Input_LineEdit` |
| `0x876c` | 173 | 36 | вывод строки | `video.c` :: `Video_PrintStr` |
| `0x8819` | 56 | 1 | ширина строки | `video.c` :: `Video_StrWidth` |
| `0x8851` | 1055 | 38 | строка по центру | `video.c` :: `Video_PrintStrC` |
| `0x8c70` | 66 | 1 | тик таймера | `video.c` :: `Video_TickDelay` |
| `0x8cb2` | 15 | 24 | очередь звука (приоритет + запуск) | `sound.c` :: `Sound_SFX` |
| `0x8cc1` | 494 | 0 | Sound_Effect | `sound.c` :: `Sound_Effect` |
| `0x8eaf` | 20 | 6 | таблица «случайных» байт | `level.c` :: `RandByte` |
| `0x9076` | 50 | 3 | выбор плоскости EGA | `video.c` :: `Video_SetPlane` |
| `0x90a8` | 178 | 5 | start address + pel pan | `video.c` :: `Video_SetScroll` |
| `0x915a` | 354 | 26 | работа с портами EGA | `video.c` :: `Video_Tile8` |
| `0x92bc` | 87 | 4 | заливка | `video.c` :: `Video_Fill` |
| `0x9313` | 52 | 6 | построчная копия | `video.c` :: `Video_RowCopy` |
| `0x9347` | 381 | 9 | вывод символа | `video.c` :: `Video_PrintChar` |
| `0x94c4` | 2780 | 1 | самоизменяющийся xlatb-блиттер прогонов логотипа INTRO (не портирован); `draw_sprite` моделирует 0x98A1/A0BB | -- |
| `0xffff` | 769 | 0 | react записей: ? | `sound.c` :: `?` |

## Портировано частично (в C стоит TODO)

| адрес | ~размер | вызовов | что это | C |
|---|---|---|---|---|

## Рантайм/библиотека Borland -- заменены libc/SDL (79)

Эвристика: игровой код оригинала целиком лежит ниже `0xC000`,
выше -- библиотека Borland C++ 3.0 (malloc/free, printf, файловый
ввод-вывод, far-ptr арифметика, эмулятор плавающей точки). Эти
процедуры переносить не нужно, в порте их роль играют libc и SDL.

| адрес | ~размер | вызовов | что это | C |
|---|---|---|---|---|
| `0x0d93` | 26 | 1 | рантайм | -- |
| `0x9fa0` | 264 | 5 | malloc (far) | -- |
| `0xa0a8` | 1990 | 7 | free (far) | -- |
| `0xa86e` | 2663 | 2 | рантайм | -- |
| `0xb2d5` | 3430 | 4 | чтение файла | -- |
| `0xc03b` | 27 | 2 | библиотека Borland | -- |
| `0xc056` | 46 | 2 | библиотека Borland | -- |
| `0xc084` | 56 | 1 | библиотека Borland | -- |
| `0xc0bc` | 161 | 13 | библиотека Borland | -- |
| `0xc15d` | 218 | 1 | библиотека Borland | -- |
| `0xc237` | 80 | 2 | размер файла | -- |
| `0xc287` | 184 | 1 | библиотека Borland | -- |
| `0xc33f` | 11 | 5 | библиотека Borland | -- |
| `0xc34a` | 39 | 2 | библиотека Borland | -- |
| `0xc371` | 61 | 13 | библиотека Borland | -- |
| `0xc3ae` | 17 | 2 | библиотека Borland | -- |
| `0xc3bf` | 41 | 4 | библиотека Borland | -- |
| `0xc3e8` | 125 | 4 | библиотека Borland | -- |
| `0xc465` | 70 | 3 | библиотека Borland | -- |
| `0xc4ab` | 39 | 3 | библиотека Borland | -- |
| `0xc4d2` | 33 | 3 | библиотека Borland | -- |
| `0xc4f3` | 40 | 1 | библиотека Borland | -- |
| `0xc51b` | 26 | 2 | memset | -- |
| `0xc535` | 34 | 3 | memcpy (PIC) | -- |
| `0xc557` | 25 | 2 | библиотека Borland | -- |
| `0xc570` | 18 | 1 | библиотека Borland | -- |
| `0xc582` | 172 | 4 | fopen-обёртка | -- |
| `0xc62e` | 125 | 1 | библиотека Borland | -- |
| `0xc6ab` | 88 | 1 | библиотека Borland | -- |
| `0xc703` | 12 | 13 | out-порт (int-обёртка) | -- |
| `0xc70f` | 181 | 4 | библиотека Borland | -- |
| `0xc7c4` | 26 | 2 | библиотека Borland | -- |
| `0xc7de` | 28 | 1 | библиотека Borland | -- |
| `0xc7fa` | 51 | 1 | библиотека Borland | -- |
| `0xc82d` | 104 | 1 | библиотека Borland | -- |
| `0xc895` | 36 | 1 | библиотека Borland | -- |
| `0xc8b9` | 26 | 5 | библиотека Borland | -- |
| `0xc8d3` | 111 | 1 | библиотека Borland | -- |
| `0xc942` | 26 | 1 | библиотека Borland | -- |
| `0xc95c` | 22 | 3 | библиотека Borland | -- |
| `0xc972` | 1167 | 3 | библиотека Borland | -- |
| `0xce01` | 38 | 2 | библиотека Borland | -- |
| `0xce27` | 121 | 1 | библиотека Borland | -- |
| `0xcea0` | 389 | 7 | библиотека Borland | -- |
| `0xd025` | 230 | 4 | библиотека Borland | -- |
| `0xd10b` | 125 | 3 | библиотека Borland | -- |
| `0xd188` | 868 | 1 | библиотека Borland | -- |
| `0xd4ec` | 207 | 4 | библиотека Borland | -- |
| `0xd5bb` | 1442 | 4 | библиотека Borland | -- |
| `0xdb5d` | 150 | 12 | рантайм | -- |
| `0xdbf3` | 311 | 2 | библиотека Borland | -- |
| `0xdd2a` | 970 | 5 | библиотека Borland | -- |
| `0xe0f4` | 119 | 1 | библиотека Borland | -- |
| `0xe16b` | 260 | 1 | библиотека Borland | -- |
| `0xe26f` | 927 | 1 | библиотека Borland | -- |
| `0xe60e` | 243 | 2 | библиотека Borland | -- |
| `0xe701` | 28 | 5 | библиотека Borland | -- |
| `0xe71d` | 29 | 6 | библиотека Borland | -- |
| `0xe73a` | 22 | 1 | библиотека Borland | -- |
| `0xe750` | 29 | 3 | библиотека Borland | -- |
| `0xe76d` | 263 | 2 | библиотека Borland | -- |
| `0xe874` | 494 | 1 | библиотека Borland | -- |
| `0xea62` | 115 | 1 | библиотека Borland | -- |
| `0xead5` | 246 | 1 | библиотека Borland | -- |
| `0xebcb` | 271 | 1 | библиотека Borland | -- |
| `0xecda` | 264 | 5 | библиотека Borland | -- |
| `0xede2` | 728 | 5 | библиотека Borland | -- |
| `0xf0ba` | 179 | 1 | библиотека Borland | -- |
| `0xf16d` | 213 | 2 | библиотека Borland | -- |
| `0xf242` | 248 | 1 | библиотека Borland | -- |
| `0xf33a` | 240 | 1 | библиотека Borland | -- |
| `0xf42a` | 802 | 2 | библиотека Borland | -- |
| `0xf74c` | 47 | 4 | библиотека Borland | -- |
| `0xf77b` | 233 | 1 | библиотека Borland | -- |
| `0xf864` | 1947 | 1 | библиотека Borland | -- |
| `0x10300` | 17795 | 2 | библиотека Borland | -- |
| `0x14883` | 69509 | 1 | библиотека Borland | -- |
| `0x25808` | 263 | 1 | библиотека Borland | -- |
| `0x2590f` | 16 | 1 | библиотека Borland | -- |

## Игровые процедуры, ещё не начатые (0)

| адрес | ~размер | вызовов | что это | C |
|---|---|---|---|---|
